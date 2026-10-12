#!/usr/bin/env python3
"""Find lui/addiu (or ori) pairs that build the given addresses in the VCS ELF.

usage: find_refs.py <elf> <function_map.csv> <addr_hex> [...]
"""
import bisect
import struct
import sys

d = open(sys.argv[1], 'rb').read()
rows = [l.strip().split(',') for l in open(sys.argv[2])][1:]
st = sorted((int(r[1], 16), r[0]) for r in rows)
keys = [x[0] for x in st]
phoff = struct.unpack_from('<I', d, 0x1c)[0]
segs = []
for i in range(struct.unpack_from('<H', d, 0x2c)[0]):
    p = struct.unpack_from('<8I', d, phoff + i * 32)
    if p[0] == 1:
        segs.append((p[2], p[1], p[4]))
for arg in sys.argv[3:]:
    target = int(arg, 16)
    hi, lo = (target + 0x8000) >> 16, target & 0xFFFF
    hi_or = target >> 16
    for va, off, sz in segs:
        for k in range(0, sz - 4, 4):
            w = struct.unpack_from('<I', d, off + k)[0]
            if (w >> 26) != 0xF or (w & 0xFFFF) not in (hi, hi_or):
                continue
            rt = (w >> 16) & 31
            for m in range(1, 12):
                if off + k + 4 * m + 4 > len(d):
                    break
                w2 = struct.unpack_from('<I', d, off + k + 4 * m)[0]
                op = w2 >> 26
                if ((w2 >> 21) & 31) == rt and (w2 & 0xFFFF) == lo and (
                        (op == 9 and (w & 0xFFFF) == hi) or (op == 0xD and (w & 0xFFFF) == hi_or) or
                        (op in (0x23, 0x2B, 0x20, 0x24, 0x21, 0x25, 0x28, 0x29, 0x37, 0x3F, 0x1E, 0x1F) and (w & 0xFFFF) == hi)):
                    j = bisect.bisect_right(keys, va + k) - 1
                    print(f'{arg}: {va + k:#x} in {st[j][1]}')
                    break
