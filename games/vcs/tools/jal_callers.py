#!/usr/bin/env python3
"""List JAL call sites of target functions, grouped by containing function.

usage: jal_callers.py <elf> <function_map.csv> <target_hex> [...]
"""
import bisect
import struct
import sys


def load_segments(path):
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', d, 0x1c)[0]
    phnum = struct.unpack_from('<H', d, 0x2c)[0]
    segs = []
    for i in range(phnum):
        p = struct.unpack_from('<8I', d, phoff + i * 32)
        if p[0] == 1:
            segs.append((p[2], p[1], p[4]))  # vaddr, offset, filesz
    return d, segs


def main():
    d, segs = load_segments(sys.argv[1])
    rows = [l.strip().split(',') for l in open(sys.argv[2])][1:]
    starts = sorted((int(r[1], 16), r[0]) for r in rows)
    keys = [s[0] for s in starts]
    targets = {int(t, 16): t for t in sys.argv[3:]}
    for target in targets:
        jal = 0x0C000000 | ((target >> 2) & 0x03FFFFFF)
        sites = []
        for va, off, sz in segs:
            for i in range(0, sz - 3, 4):
                if struct.unpack_from('<I', d, off + i)[0] == jal:
                    sites.append(va + i)
        print(f"== {hex(target)}: {len(sites)} call sites")
        for s in sites:
            j = bisect.bisect_right(keys, s) - 1
            print(f"   {hex(s)} in {starts[j][1]}")


if __name__ == '__main__':
    main()
