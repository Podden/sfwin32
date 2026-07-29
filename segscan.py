"""Separate real segment-override prefixes from capstone's cosmetic `es:` on
string instructions. Only an explicit 0x26/0x64/0x65 prefix byte represents a
memory access Win32 cannot satisfy with a flat ES.
"""
import json
from collections import Counter, defaultdict

from capstone import (Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_GRP_JUMP,
                      CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET)

info = json.load(open('relocs.json'))
OBJ = {o['index']: o for o in info['objects']}
img1 = open('obj1.bin', 'rb').read()
BASE, END = OBJ[1]['base'], OBJ[1]['base'] + OBJ[1]['size']

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

STOP = {'ret', 'retf', 'jmp', 'iret', 'iretd', 'hlt', 'ud2'}
PREFIX_SEG = {0x26: 'es', 0x2e: 'cs', 0x36: 'ss', 0x3e: 'ds',
              0x64: 'fs', 0x65: 'gs'}
STRING_OPS = ('movs', 'stos', 'lods', 'scas', 'cmps', 'ins', 'outs')

seen = {}
queue = [info['entry']] + [r['target'] for r in info['relocs'] if r['tobj'] == 1]

while queue:
    addr = queue.pop()
    while True:
        if addr in seen or not (BASE <= addr < END):
            break
        try:
            ins = next(md.disasm(img1[addr - BASE:addr - BASE + 16], addr))
        except StopIteration:
            break
        seen[addr] = ins
        g = set(ins.groups)
        if (CS_GRP_JUMP in g or CS_GRP_CALL in g) and ins.operands \
                and ins.operands[0].type == CS_OP_IMM:
            queue.append(ins.operands[0].imm)
        if ins.mnemonic in STOP or CS_GRP_RET in g or CS_GRP_IRET in g:
            break
        addr = ins.address + ins.size

explicit = defaultdict(list)
implicit = Counter()

for a, ins in sorted(seen.items()):
    b = ins.bytes
    i = 0
    seg = None
    while i < len(b) and b[i] in (0x66, 0x67, 0xf0, 0xf2, 0xf3, *PREFIX_SEG):
        if b[i] in PREFIX_SEG:
            seg = PREFIX_SEG[b[i]]
        i += 1
    if seg:
        explicit[seg].append((a, ins.mnemonic + ' ' + ins.op_str))
    elif ins.mnemonic.startswith(STRING_OPS):
        implicit[ins.mnemonic] += 1

print('EXPLICIT segment-override prefixes (these are the real problem):')
for s in sorted(explicit, key=lambda k: -len(explicit[k])):
    print('  %s: %d sites' % (s, len(explicit[s])))

print('\nimplicit-ES string ops (harmless: ES == DS == flat on Win32):')
for m, n in implicit.most_common(12):
    print('  %-8s %d' % (m, n))
print('  total %d' % sum(implicit.values()))

es = explicit['es']
print('\n--- explicit ES sites: %d ---' % len(es))
# How clustered are they? Group into runs separated by >0x400 bytes.
runs = []
for a, t in es:
    if runs and a - runs[-1][-1][0] <= 0x400:
        runs[-1].append((a, t))
    else:
        runs.append([(a, t)])
runs.sort(key=lambda r: -len(r))
print('%d clusters; largest:' % len(runs))
for r in runs[:12]:
    print('  0x%06x..0x%06x  %3d sites' % (r[0][0], r[-1][0], len(r)))

json.dump({s: [[hex(a), t] for a, t in v] for s, v in explicit.items()},
          open('segsites.json', 'w'), indent=1)
print('\nwrote segsites.json')
