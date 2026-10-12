#!/usr/bin/env python3
"""Minimal PS2 VU microcode disassembler (enough to follow loads/stores/flow).

usage: vu_disasm.py <vucode.bin> <start_pc_hex> [count]
Each instruction pair is 8 bytes: lower word at +0, upper word at +4.
"""
import struct
import sys

DEST = lambda w: "".join(c for c, b in zip("xyzw", (24, 23, 22, 21)) if (w >> b) & 1)
BC = "xyzw"


def upper(w):
    op = w & 0x3F
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = DEST(w)
    flags = ("I" if w >> 31 & 1 else "") + ("E" if w >> 30 & 1 else "") + ("M" if w >> 29 & 1 else "") + \
            ("D" if w >> 28 & 1 else "") + ("T" if w >> 27 & 1 else "")
    names3 = {0x28: "add", 0x2c: "sub", 0x2a: "mul", 0x29: "madd", 0x2d: "msub", 0x2b: "max", 0x2f: "min",
              0x20: "addq", 0x1c: "mulq", 0x22: "addi", 0x1e: "muli", 0x26: "subi", 0x2e: "opmsub", 0x24: "subq",
              0x21: "maddq", 0x23: "maddi", 0x25: "msubq", 0x27: "msubi", 0x1d: "maxi", 0x1f: "mini"}
    if op < 0x20 and (op >> 2) in (0, 1, 2, 3, 4, 5, 6, 7):
        bcops = {0: "add", 1: "sub", 2: "madd", 3: "msub", 4: "max", 5: "mini", 6: "mul"}
        grp = op >> 2
        if grp in bcops:
            return f"{bcops[grp]}{BC[op & 3]}.{d} vf{fd}, vf{fs}, vf{ft}{BC[op & 3]} {flags}"
    if op in names3:
        return f"{names3[op]}.{d} vf{fd}, vf{fs}, vf{ft} {flags}"
    if op >= 0x3c:
        op2 = ((w >> 4) & 0x7C) | (w & 3)
        n2 = {0x10: "itof0", 0x11: "itof4", 0x12: "itof12", 0x13: "itof15", 0x14: "ftoi0", 0x15: "ftoi4",
              0x16: "ftoi12", 0x17: "ftoi15", 0x1d: "abs", 0x1f: "clip", 0x2c: "opmula", 0x2e: "mula",
              0x2f: "nop", 0x0a: "mulaq?", 0x28: "adda", 0x29: "madda", 0x2a: "mula?", 0x1c: "mulaq", 0x1e: "mulai"}
        bcacc = {0x00: "adda", 0x01: "suba", 0x02: "madda", 0x03: "msuba", 0x06: "mula"}
        g = op2 >> 2
        if g in bcacc:
            return f"{bcacc[g]}{BC[op2 & 3]}.{d} ACC, vf{fs}, vf{ft}{BC[op2 & 3]} {flags}"
        return f"{n2.get(op2, 'up2_%02x' % op2)}.{d} vf{ft}, vf{fs} {flags}"
    return f"upper_{op:02x} {w:08x} {flags}"


def lower(w):
    op = w >> 25
    ft, fs = (w >> 16) & 31, (w >> 11) & 31
    imm11 = w & 0x7FF
    simm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    d = DEST(w)
    if op == 0x00: return f"lq.{d} vf{ft}, {simm11}(vi{fs})"
    if op == 0x01: return f"sq.{d} vf{fs}, {simm11}(vi{ft})"
    if op == 0x04: return f"ilw.{d} vi{ft}, {simm11}(vi{fs})"
    if op == 0x05: return f"isw.{d} vi{ft}, {simm11}(vi{fs})"
    if op == 0x08: return f"iaddiu vi{ft}, vi{fs}, {imm15}"
    if op == 0x09: return f"isubiu vi{ft}, vi{fs}, {imm15}"
    if op == 0x10: return f"fceq vi1, {w & 0xFFFFFF:#x}"
    if op == 0x11: return f"fcset {w & 0xFFFFFF:#x}"
    if op == 0x12: return f"fcand vi1, {w & 0xFFFFFF:#x}"
    if op == 0x13: return f"fcor vi1, {w & 0xFFFFFF:#x}"
    if op == 0x16: return f"fsand vi{ft}, {imm11:#x}"
    if op == 0x1a: return f"fmand vi{ft}, vi{fs}"
    if op == 0x1c: return f"fcget vi{ft}"
    if op in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
        n = {0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq", 0x29: "ibne", 0x2c: "ibltz",
             0x2d: "ibgtz", 0x2e: "iblez", 0x2f: "ibgez"}[op]
        return f"{n} vi{ft}, vi{fs}, {simm11:+d}"
    if op == 0x40:
        sub = w & 0x3F
        idd = (w >> 6) & 31
        if sub == 0x30: return f"iadd vi{idd}, vi{fs}, vi{ft}"
        if sub == 0x31: return f"isub vi{idd}, vi{fs}, vi{ft}"
        if sub == 0x32: return f"iaddi vi{ft}, vi{fs}, {((w >> 6) & 31) - 32 if (w >> 10) & 1 else (w >> 6) & 31}"
        if sub == 0x34: return f"iand vi{idd}, vi{fs}, vi{ft}"
        if sub == 0x35: return f"ior vi{idd}, vi{fs}, vi{ft}"
        if sub >= 0x3c:
            sub2 = ((w >> 4) & 0x7C) | (w & 3)
            n = {0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd", 0x38: "div",
                 0x39: "sqrt", 0x3a: "rsqrt", 0x3b: "waitq", 0x3c: "mtir", 0x3d: "mfir", 0x3e: "ilwr", 0x3f: "iswr",
                 0x40: "rnext", 0x41: "rget", 0x42: "rinit", 0x43: "rxor", 0x64: "mfp", 0x68: "xtop", 0x69: "xitop",
                 0x6c: "xgkick", 0x70: "esadd", 0x71: "ersadd", 0x72: "eleng", 0x73: "erleng", 0x74: "eatanxy",
                 0x7e: "esqrt", 0x7f: "esin", 0x7b: "waitp", 0x7c: "esum", 0x7d: "ercpr", 0x6b: "einvsqrt?",
                 0x33: "nop"}.get(sub2, "lo2_%02x" % sub2)
            return f"{n}.{d} ft=vf{ft}/vi{ft} fs=vf{fs}/vi{fs}"
        return f"lo1_{sub:02x} {w:08x}"
    return f"lower_{op:02x} {w:08x}"


def main():
    code = open(sys.argv[1], "rb").read()
    pc = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    for _ in range(count):
        lo, up = struct.unpack_from("<II", code, pc)
        lstr = f"loi {lo:#010x}" if up >> 31 & 1 else lower(lo)
        print(f"{pc:04x}: {upper(up):42s} | {lstr}")
        pc += 8


if __name__ == "__main__":
    main()
