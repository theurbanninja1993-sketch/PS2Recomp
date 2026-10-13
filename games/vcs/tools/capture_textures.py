#!/usr/bin/env python3
"""List the textures drawn in an F9 GS capture (TEX0 writes), resolved to
replacement keys via a PS2X_TEXREPL_DUMP _dump_index.log.

Usage: capture_textures.py <capture.bin> <dump_dir>
"""
import struct, sys, os, re, collections

def load_index(dump_dir):
    idx = {}
    path = os.path.join(dump_dir, "_dump_index.log")
    if not os.path.exists(path):
        return idx
    for line in open(path, errors="replace"):
        m = re.match(r"(\S+) tbp0=(\d+) tbw=(\d+) psm=(\d+) tw=(\d+) th=(\d+)", line)
        if m:
            idx[(int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))] = m.group(1)
    return idx

def parse(path):
    data = open(path, "rb").read()
    off = 0
    tex0 = None
    usage = collections.Counter()   # (tbp0,tbw,psm,tw,th) -> count
    while off + 8 <= len(data):
        typ, size = struct.unpack_from("<II", data, off)
        off += 8
        payload = data[off:off + size]
        off += size
        if typ == 2 and size >= 9:
            addr = payload[0]
            value = struct.unpack_from("<Q", payload, 1)[0]
            if addr == 0x06:
                tex0 = value
            continue
        if typ != 1 or len(payload) < 16:
            continue
        lo, hi = struct.unpack_from("<QQ", payload, 0)
        nloop = lo & 0x7FFF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF
        if nreg == 0:
            nreg = 16
        regs = [(hi >> (i * 4)) & 0xF for i in range(nreg)]
        pos = 16
        if flg == 0:  # PACKED
            for _ in range(nloop):
                for r in range(nreg):
                    if pos + 16 > len(payload):
                        break
                    vlo, vhi = struct.unpack_from("<QQ", payload, pos)
                    pos += 16
                    if regs[r] == 0x06:
                        tex0 = vlo
                    elif regs[r] == 0x0E:  # GIF A+D "AD" register: hi=addr, lo=value
                        addr = vhi & 0x7F
                        if addr == 0x06:
                            tex0 = vlo
                    if tex0 is not None and regs[r] in (0x06, 0x0E):
                        usage[(tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F,
                               (tex0 >> 26) & 0xF, (tex0 >> 30) & 0xF)] += 1
        elif flg == 1:  # REGLIST
            for _ in range(nloop):
                for r in range(nreg):
                    if pos + 8 > len(payload):
                        break
                    v = struct.unpack_from("<Q", payload, pos)[0]
                    pos += 8
                    if regs[r] == 0x06:
                        tex0 = v
                        if tex0 is not None:
                            usage[(tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F,
                                   (tex0 >> 26) & 0xF, (tex0 >> 30) & 0xF)] += 1
        # IMAGE packets are uploads: skipped here (the initial VRAM + TEX0
        # usage is enough to see which textures a screen draws).
    return usage

def main():
    capture, dump_dir = sys.argv[1], sys.argv[2]
    idx = load_index(dump_dir)
    usage = parse(capture)
    print(f"== {os.path.basename(capture)}: {len(usage)} distinct TEX0 textures, {sum(usage.values())} sets")
    rows = []
    for (tbp0, tbw, psm, tw, th), n in usage.most_common():
        key = idx.get((tbp0, tbw, psm, tw, th))
        edge = 1 << max(tw, th)
        tag = "SMALL" if edge <= 32 else ("64" if edge == 64 else ("128" if edge == 128 else f"{edge}"))
        rows.append((n, tbp0, tbw, psm, tw, th, edge, key, tag))
    # small textures first (glyph candidates), then notable others
    for n, tbp0, tbw, psm, tw, th, edge, key, tag in sorted(rows, key=lambda r: (r[6], -r[0])):
        if edge <= 64 or key is None:
            print(f"  x{n:<4} tbp0={tbp0:<6} tbw={tbw:<3} psm={psm:<3} {edge}px  {key or '(not in dump index)'}")

if __name__ == "__main__":
    main()
