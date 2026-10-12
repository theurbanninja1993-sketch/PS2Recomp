#!/usr/bin/env python3
"""Map host-RIP samples (PS2X_PC_SAMPLE, offsets from pcSampleHandler) to symbols.

usage: rip_hist.py <samples.bin> <sorted nm 'addr name' file> [name_regex] [top]
"""
import bisect
import collections
import re
import struct
import sys

data = open(sys.argv[1], 'rb').read()
offs = struct.unpack(f'<{len(data) // 4}i', data)
syms = [(int(a, 16), n) for a, n in (l.split()[:2] for l in open(sys.argv[2]) if len(l.split()) >= 2)]
syms.sort()
addrs = [a for a, _ in syms]
base = next(a for a, n in syms if 'pcSampleHandler' in n)
pattern = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
counts = collections.Counter()
for off in offs:
    j = bisect.bisect_right(addrs, base + off) - 1
    name = syms[j][1] if j >= 0 else '?'
    if pattern is None or pattern.search(name):
        counts[name] += 1
print(len(offs), 'samples')
for name, n in counts.most_common(top):
    print(f'{n:8d} {name}')
