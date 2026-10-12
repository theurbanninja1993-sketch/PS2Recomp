#!/usr/bin/env python3
"""Map sampled guest PCs (PS2X_PC_SAMPLE output) to functions.

usage: pc_hist.py <samples.bin> <function_map.csv> [lo_hex hi_hex] [top]
"""
import bisect
import collections
import struct
import sys

data = open(sys.argv[1], 'rb').read()
pcs = struct.unpack(f'<{len(data) // 4}I', data)
rows = [l.strip().split(',') for l in open(sys.argv[2])][1:]
st = sorted((int(r[1], 16), r[0]) for r in rows)
keys = [x[0] for x in st]
lo = int(sys.argv[3], 16) if len(sys.argv) > 4 else 0
hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 1 << 32
top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
counts = collections.Counter()
for pc in pcs:
    if lo <= pc < hi:
        j = bisect.bisect_right(keys, pc) - 1
        counts[st[j][1] if j >= 0 else hex(pc)] += 1
print(f'{len(pcs)} samples, {sum(counts.values())} in range')
for name, n in counts.most_common(top):
    print(f'{n:8d} {name}')
