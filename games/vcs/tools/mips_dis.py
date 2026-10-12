#!/usr/bin/env python3
"""Tiny MIPS (R3000) disassembler for reading IOP module code.

usage: mips_dis.py <file> <file_offset_of_vaddr0_hex> <start_vaddr_hex> <end_vaddr_hex>
"""
import struct
import sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def dis(pc, w):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    if w == 0:
        return 'nop'
    if op == 0:
        names = {0x21: 'addu', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu'}
        if fn in names:
            return f'{names[fn]} {R[rd]},{R[rs]},{R[rt]}'
        if fn in (0, 2, 3):
            return f"{['sll', '', 'srl', 'sra'][fn]} {R[rd]},{R[rt]},{sa}"
        if fn == 8:
            return f'jr {R[rs]}'
        if fn == 9:
            return f'jalr {R[rd]},{R[rs]}'
        return f'special fn={fn:#x}'
    if op in (2, 3):
        return f"{'j' if op == 2 else 'jal'} {((w & 0x3FFFFFF) << 2):#x}"
    tgt = pc + 4 + (simm(w) << 2)
    if op == 4:
        return f'beq {R[rs]},{R[rt]},{tgt:#x}'
    if op == 5:
        return f'bne {R[rs]},{R[rt]},{tgt:#x}'
    if op == 6:
        return f'blez {R[rs]},{tgt:#x}'
    if op == 7:
        return f'bgtz {R[rs]},{tgt:#x}'
    if op == 1:
        return f"{'bltz' if rt == 0 else 'bgez'} {R[rs]},{tgt:#x}"
    imm_ops = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori'}
    if op in imm_ops:
        v = w & 0xFFFF if op >= 12 else simm(w)
        return f'{imm_ops[op]} {R[rt]},{R[rs]},{v:#x}'
    if op == 15:
        return f'lui {R[rt]},{w & 0xFFFF:#x}'
    mem = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x28: 'sb', 0x29: 'sh', 0x2b: 'sw'}
    if op in mem:
        return f'{mem[op]} {R[rt]},{simm(w)}({R[rs]})'
    return f'op={op:#x} raw={w:#010x}'


def main():
    data = open(sys.argv[1], 'rb').read()
    base = int(sys.argv[2], 16)
    start, end = int(sys.argv[3], 16), int(sys.argv[4], 16)
    for pc in range(start, end, 4):
        w = struct.unpack_from('<I', data, base + pc)[0]
        print(f'{pc:06x}: {w:08x}  {dis(pc, w)}')


if __name__ == '__main__':
    main()
