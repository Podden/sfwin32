"""Print the disassembly around a given address from the relocated image."""
import json
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

info = json.load(open('relocs.json'))
OBJ = {o['index']: o for o in info['objects']}
img1 = open('obj1.bin', 'rb').read()
BASE = OBJ[1]['base']
md = Cs(CS_ARCH_X86, CS_MODE_32)

for arg in sys.argv[1:]:
    if ':' in arg:
        a, back = arg.split(':')
        back = int(back)
    else:
        a, back = arg, 48
    addr = int(a, 16)
    start = addr - back
    print('--- around 0x%x ---' % addr)
    for i in md.disasm(img1[start - BASE:addr - BASE + 64], start):
        mark = '>>' if i.address == addr else '  '
        print('%s 0x%06x  %-24s %s' % (mark, i.address, i.bytes.hex(),
                                       i.mnemonic + ' ' + i.op_str))
    print()
