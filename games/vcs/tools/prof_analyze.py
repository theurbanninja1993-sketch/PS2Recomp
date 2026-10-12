#!/usr/bin/env python3
"""Attribute PS2X_PC_SAMPLE samples (<file>, <file>.maps, <file>.callers).

usage: prof_analyze.py <samples.bin> [top] [leaf-regex-for-caller-attribution]
Prints top symbols across the runner and shared libraries, a category split,
and for samples in leaf functions matching the regex (default memcpy|memmove|
memset) the caller ([rsp]) attribution.
"""
import bisect, collections, re, struct, subprocess, sys

path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
leaf_re = re.compile(sys.argv[3] if len(sys.argv) > 3 else r'memcpy|memmove|memset')

maps = open(path + '.maps').read().splitlines()
handler = int(maps[0].split()[1], 16)
regions = []
for line in maps[1:]:
    parts = line.split(None, 5)
    if len(parts) < 6 or 'x' not in parts[1]:
        continue
    lo, hi = (int(x, 16) for x in parts[0].split('-'))
    regions.append((lo, hi, int(parts[2], 16), parts[5].strip()))

symcache = {}
def symbols(lib):
    if lib not in symcache:
        out = subprocess.run(['nm', '-C', '--defined-only', lib], capture_output=True, text=True).stdout
        if not out.strip():
            out = subprocess.run(['nm', '-C', '-D', '--defined-only', lib], capture_output=True, text=True).stdout
        syms = []
        for l in out.splitlines():
            p = l.split(None, 2)
            if len(p) == 3 and p[1] in 'tTwWiI':
                syms.append((int(p[0], 16), p[2]))
        syms.sort()
        symcache[lib] = ([a for a, _ in syms], [n for _, n in syms])
    return symcache[lib]

def resolve(offset32):
    a32 = (handler + offset32) & 0xffffffff
    for lo, hi, off, lib in regions:
        l32 = lo & 0xffffffff
        if l32 <= a32 < l32 + (hi - lo):
            rel = a32 - l32 + off
            # PIE executable/shared objects: nm addresses are file-relative (vaddr ~ offset here)
            addrs, names = symbols(lib)
            j = bisect.bisect_right(addrs, rel) - 1
            return lib.split('/')[-1], (names[j] if j >= 0 else '?')
    return '?', '?'

def load(p):
    d = open(p, 'rb').read()
    return struct.unpack(f'<{len(d)//4}I', d)

samples = load(path)
try:
    callers = load(path + '.callers')
except OSError:
    callers = None

counts = collections.Counter()
leafcallers = collections.Counter()
for i, s in enumerate(samples):
    lib, name = resolve(s)
    counts[(lib, name)] += 1
    if callers is not None and leaf_re.search(name):
        leafcallers[resolve(callers[i])[1]] += 1

total = len(samples)
print(f'{total} samples')
for (lib, name), n in counts.most_common(top):
    print(f'{100*n/total:5.1f}% {lib:14.14s} {name[:110]}')

def category(lib, name):
    if 'VU1Interpreter' in name: return 'VU1'
    if name.startswith(('FUN_', 'ptr_', 'entry_')): return 'EE recompiled'
    if 'iop' in name.lower(): return 'IOP'
    if 'advanceEeTimers' in name: return 'EE timers'
    if leaf_re.search(name): return 'memcpy/memset'
    if lib.startswith('libc'): return 'libc other'
    if 'GS' in name or 'Gif' in name: return 'GS'
    if 'VIF' in name or 'PS2Memory' in name: return 'VIF/memory'
    return 'other'
cats = collections.Counter()
for (lib, name), n in counts.items():
    cats[category(lib, name)] += n
print('categories:', ', '.join(f'{k} {100*v/total:.1f}%' for k, v in cats.most_common()))
if leafcallers:
    print('callers of', leaf_re.pattern)
    for name, n in leafcallers.most_common(12):
        print(f'  {100*n/total:5.1f}% {name[:120]}')
