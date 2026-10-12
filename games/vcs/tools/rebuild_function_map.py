#!/usr/bin/env python3
"""Rebuild a clean PS2Recomp function map for SLUS_215.90.

The Ghidra export used for vcs_ghidra2*.csv kept only the first contiguous
chunk of each function, which left ~11k `entry_` fragments with unrecompiled
gaps between them (branches into those gaps hit "missing-target" at runtime).

This script derives boundaries the robust way:
  starts = Ghidra FUN_/thunk_/named starts  +  every JAL target in .text
           +  the ELF entry point
           +  function pointers built in code (lui/addiu) or stored in data,
              when they sit right after another function's epilogue
  end(f) = next start (capped at the end of .text)
`entry_` fragments that are not real call targets are absorbed into the
function that precedes them, so every instruction belongs to exactly one
recompiled function.

Any `name@0xADDR` stub bindings in the optional recompiler TOML are also
treated as starts (library code reached only via pointers / tail jumps).

usage: rebuild_function_map.py <elf> <ghidra.csv> <out.csv> [recomp.toml] [out_entry_hints.txt]
"""
import csv
import re
import struct
import sys


def load_elf(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "expected ELF32 LE"
    entry = struct.unpack_from("<I", data, 0x18)[0]
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = []
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        sections.append([name, typ, flags, addr, off, size])
    stroff = sections[shstrndx][4]
    out = {}
    for name, typ, flags, addr, off, size in sections:
        n = data[stroff + name:data.index(b"\0", stroff + name)].decode()
        out[n] = (addr, data[off:off + size] if typ != 8 else b"")
    return entry, out


def main():
    elf_path, ghidra_csv, out_csv = sys.argv[1:4]
    entry, sections = load_elf(elf_path)
    text_addr, text = sections[".text"]
    text_end = text_addr + len(text)

    names = {}
    with open(ghidra_csv) as f:
        for row in csv.DictReader(f):
            start = int(row["Start"], 16)
            if not row["Name"].startswith("entry_"):
                names[start] = row["Name"]

    if len(sys.argv) > 4:
        for m in re.finditer(r'"[A-Za-z_][\w.]*@0x([0-9A-Fa-f]+)"', open(sys.argv[4]).read()):
            start = int(m.group(1), 16)
            names.setdefault(start, f"FUN_{start:08x}")

    starts = set(names)
    starts.add(entry)
    jal_targets = set()
    for off in range(0, len(text), 4):
        word, = struct.unpack_from("<I", text, off)
        if (word >> 26) == 0x03:  # JAL
            pc = text_addr + off
            target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if text_addr <= target < text_end:
                jal_targets.add(target)
    starts |= jal_targets

    def word_at(addr):
        return struct.unpack_from("<I", text, addr - text_addr)[0]

    def follows_function_end(t):
        """True if t sits right after a function epilogue (jr $ra / j + delay
        slot), optionally separated by nop padding. Filters out jump-table
        targets in the middle of a function."""
        if t == text_addr:
            return True
        # epilogue at t-8-4k, its delay slot at t-4-4k, then k words of padding.
        # Padding is nop, or the dead "addiu $sp, $sp, +N" this compiler leaves
        # between functions.
        def is_padding(w):
            return w == 0 or ((w >> 16) == 0x27BD and (w & 0x8000) == 0)

        for k in range(9):
            epi = t - 8 - 4 * k
            if epi < text_addr:
                return False
            w = word_at(epi)
            if w == 0x03E00008 or (w >> 26) == 0x02:  # jr $ra | j target
                return True
            if not is_padding(word_at(t - 4 - 4 * k)):
                return False
        return False

    def plausible(t):
        return text_addr <= t < text_end and t % 4 == 0 and t not in starts and follows_function_end(t)

    def starts_with_prologue(t):
        first = word_at(t)
        return (first >> 16) == 0x27BD and (first & 0x8000) != 0  # addiu $sp, $sp, -N

    def looks_like_entry(t):
        """Looser check for pointers that are almost always code addresses
        (built in code or stored in writable data): accept a clean epilogue
        boundary, or a first instruction that is a stack-frame prologue
        (addiu $sp, $sp, -N) or a bare `jr $ra`. Rejects 16-bit-pair
        constants (0x00280028) and DMA packets embedded in .text."""
        if not (text_addr <= t < text_end and t % 4 == 0) or t in starts:
            return False
        if (t >> 16) == (t & 0xFFFF):
            return False
        if follows_function_end(t):
            return True
        first = word_at(t)
        return ((first >> 16) == 0x27BD and (first & 0x8000) != 0) or first == 0x03E00008

    # Pointers that fail the start heuristics become entry-point hints instead:
    # the recompiler gives them a callable entry inside their containing function
    # (no split), so indirect calls to small callbacks still dispatch.
    entry_hints = set()

    # Function pointers materialised in code: lui rX, hi ; addiu/ori rY, rX, lo
    code_ptrs = set()
    lui = {}
    for off in range(0, len(text), 4):
        word, = struct.unpack_from("<I", text, off)
        op, rs, rt, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
        if op == 0x0F:  # LUI
            lui[rt] = (imm << 16, off)
            continue
        if op in (0x09, 0x0D, 0x19) and rs in lui and off - lui[rs][1] <= 32:  # ADDIU / ORI / DADDIU
            hi = lui[rs][0]
            target = (hi | imm) if op == 0x0D else (hi + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if looks_like_entry(target):
                code_ptrs.add(target)
            elif text_addr <= target < text_end and target % 4 == 0 and (target >> 16) != (target & 0xFFFF):
                entry_hints.add(target)
    starts |= code_ptrs

    # Function pointers stored in data (vtables, callback tables, init arrays)
    data_ptrs = set()
    for sec_name, (sec_addr, sec_data) in sections.items():
        if sec_name in (".text", ".vutext") or not sec_data or sec_addr == 0:
            continue
        for off in range(0, len(sec_data) - 3, 4):
            target, = struct.unpack_from("<I", sec_data, off)
            # .rodata is full of switch jump tables (mid-function targets), so it
            # needs the epilogue check; writable data holds vtables/callbacks.
            if sec_name == ".rodata":
                # Also accept prologue-led targets: C++ vtables live in .rodata,
                # and switch-table case labels never open a stack frame.
                ok = plausible(target) or (looks_like_entry(target) and starts_with_prologue(target))
            else:
                ok = looks_like_entry(target)
                if not ok and text_addr <= target < text_end and target % 4 == 0 and \
                        (target >> 16) != (target & 0xFFFF):
                    entry_hints.add(target)
            if ok:
                data_ptrs.add(target)
    starts |= data_ptrs

    # C++ vtables: runs of >= 3 consecutive code pointers in data where at least
    # half the entries are already-known function starts. Accept every entry, which
    # picks up frameless leaf methods (getters) the per-pointer checks reject.
    # Switch tables don't qualify: their entries point into one function's body.
    vtable_ptrs = set()
    for sec_name, (sec_addr, sec_data) in sections.items():
        if sec_name in (".text", ".vutext") or not sec_data or sec_addr == 0:
            continue
        run = []
        for off in range(0, len(sec_data) - 3, 4):
            target, = struct.unpack_from("<I", sec_data, off)
            is_code = text_addr <= target < text_end and target % 4 == 0 and (target >> 16) != (target & 0xFFFF)
            if is_code:
                run.append(target)
                continue
            if len(run) >= 3 and sum(t in starts for t in run) * 2 >= len(run):
                vtable_ptrs.update(t for t in run if t not in starts)
            run = []
    starts |= vtable_ptrs
    for t in code_ptrs | data_ptrs | vtable_ptrs:
        names.setdefault(t, f"FUN_{t:08x}")

    starts = sorted(s for s in starts if text_addr <= s < text_end)

    entry_hints -= set(starts)
    if len(sys.argv) > 5:
        with open(sys.argv[5], "w") as f:
            for t in sorted(entry_hints):
                f.write(f"ptr_{t:08x}@0x{t:08X}\n")

    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Name", "Start", "End", "Size"])
        for i, start in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else text_end
            name = names.get(start, f"FUN_{start:08x}")
            w.writerow([name, f"0x{start:08X}", f"0x{end:08X}", end - start])

    print(f"ghidra non-fragment starts: {len(names)}, JAL targets: {len(jal_targets)}, "
          f"code pointer starts: {len(code_ptrs)}, data pointer starts: {len(data_ptrs)}, "
          f"vtable entry starts: {len(vtable_ptrs)}, entry hints: {len(entry_hints)}, total functions: {len(starts)}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
