"""Recursive-descent disassembly of the relocated image, then inventory every
instruction that cannot survive outside DOS/4GW (interrupts, port I/O,
privileged and segment-register ops).
"""
import json
import struct
from collections import Counter, defaultdict

from capstone import (Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_GRP_JUMP,
                      CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET)

info = json.load(open('relocs.json'))
OBJ = {o['index']: o for o in info['objects']}
img = {i: open('obj%d.bin' % i, 'rb').read() for i in OBJ}
CODE_BASE = OBJ[1]['base']
CODE_END = CODE_BASE + OBJ[1]['size']

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def fetch(addr, n=16):
    o = addr - CODE_BASE
    return img[1][o:o + n]


STOP = {'ret', 'retf', 'jmp', 'iret', 'iretd', 'hlt', 'ud2'}

seen = {}          # addr -> capstone insn
queue = []

# Seeds: the entry point, plus every relocation whose target lands in object 1.
queue.append(info['entry'])
code_targets = 0
for r in info['relocs']:
    if r['tobj'] == 1:
        queue.append(r['target'])
        code_targets += 1

# Indirect-call/jump tables: relocation slots inside object 2 that point into
# object 1 are almost always function pointers or switch tables.
print('seeds: entry + %d object-1 relocation targets' % code_targets)

while queue:
    addr = queue.pop()
    while True:
        if addr in seen or not (CODE_BASE <= addr < CODE_END):
            break
        buf = fetch(addr)
        if not buf:
            break
        try:
            ins = next(md.disasm(buf, addr))
        except StopIteration:
            break
        seen[addr] = ins
        g = set(ins.groups)
        if CS_GRP_JUMP in g or CS_GRP_CALL in g:
            op = ins.operands[0] if ins.operands else None
            if op is not None and op.type == CS_OP_IMM:
                queue.append(op.imm)
        if ins.mnemonic in STOP or CS_GRP_RET in g or CS_GRP_IRET in g:
            break
        addr = ins.address + ins.size

print('reached %d instructions, %d bytes (%.1f%% of object 1)'
      % (len(seen), sum(i.size for i in seen.values()),
         100.0 * sum(i.size for i in seen.values()) / OBJ[1]['size']))

# ---- inventory of everything that needs a Win32 replacement -----------------
ints = defaultdict(list)
portio = defaultdict(list)
segops = defaultdict(list)
priv = defaultdict(list)
PRIV = {'lgdt', 'lidt', 'lldt', 'ltr', 'cli', 'sti', 'in', 'out', 'insb',
        'insd', 'outsb', 'outsd', 'hlt', 'invd', 'wbinvd', 'mov cr', 'lmsw',
        'clts', 'arpl', 'lar', 'lsl', 'verr', 'verw', 'smsw', 'str', 'sldt'}
SEG = {'les', 'lds', 'lfs', 'lgs', 'lss', 'push', 'pop', 'mov'}

for a, ins in sorted(seen.items()):
    m = ins.mnemonic
    if m == 'int':
        ints[ins.op_str].append(a)
    elif m in ('in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd'):
        portio[m + ' ' + ins.op_str].append(a)
    elif m in ('les', 'lds', 'lfs', 'lgs', 'lss'):
        segops[m].append(a)
    elif m in PRIV:
        priv[m].append(a)
    elif m in ('push', 'pop', 'mov') and any(
            r in ins.op_str for r in ('cs', 'ds', 'es', 'fs', 'gs', 'ss')):
        # crude but catches explicit segment-register moves
        toks = ins.op_str.replace(',', ' ').split()
        if any(t in ('cs', 'ds', 'es', 'fs', 'gs', 'ss') for t in toks):
            segops[m + ' seg'].append(a)

print('\n=== INT instructions ===')
for k, v in sorted(ints.items(), key=lambda kv: -len(kv[1])):
    print('  int %-6s %5d sites   e.g. %s' %
          (k, len(v), ', '.join('0x%x' % x for x in v[:6])))

print('\n=== port I/O ===')
tot = 0
for k, v in sorted(portio.items(), key=lambda kv: -len(kv[1])):
    tot += len(v)
    print('  %-22s %5d   e.g. %s' %
          (k, len(v), ', '.join('0x%x' % x for x in v[:4])))
print('  total port-I/O sites: %d' % tot)

print('\n=== segment-register ops ===')
for k, v in sorted(segops.items(), key=lambda kv: -len(kv[1])):
    print('  %-14s %5d   e.g. %s' %
          (k, len(v), ', '.join('0x%x' % x for x in v[:6])))

print('\n=== other privileged ===')
for k, v in sorted(priv.items(), key=lambda kv: -len(kv[1])):
    print('  %-14s %5d   e.g. %s' %
          (k, len(v), ', '.join('0x%x' % x for x in v[:6])))

# Indirect control flow -- the risk area for a static port.
ind = [a for a, i in sorted(seen.items())
       if (CS_GRP_JUMP in set(i.groups) or CS_GRP_CALL in set(i.groups))
       and i.operands and i.operands[0].type != CS_OP_IMM]
print('\nindirect call/jmp sites: %d' % len(ind))

json.dump(dict(instructions={('0x%x' % a): (i.mnemonic + ' ' + i.op_str)
                             for a, i in seen.items()},
               ints={k: v for k, v in ints.items()},
               portio={k: v for k, v in portio.items()},
               indirect=ind),
          open('scan.json', 'w'))
print('wrote scan.json')
