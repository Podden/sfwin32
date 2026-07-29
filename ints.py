"""For every software-interrupt site, walk backwards through the linear
instruction stream to recover the AH / AX value that selects the DOS function.
"""
import json
import re
from collections import Counter

info = json.load(open('scan.json'))
ins = {int(k, 16): v for k, v in info['instructions'].items()}
addrs = sorted(ins)
pos = {a: i for i, a in enumerate(addrs)}

SET_AX = re.compile(r'^mov\s+(ah|ax|eax)\s*,\s*(0x[0-9a-f]+|\d+)$')

DOS = {
    0x09: 'print $-string', 0x1a: 'set DTA', 0x25: 'set int vector',
    0x2c: 'get time', 0x2a: 'get date', 0x30: 'get DOS version',
    0x33: 'ctrl-break', 0x35: 'get int vector', 0x3b: 'chdir',
    0x3c: 'creat', 0x3d: 'open', 0x3e: 'close', 0x3f: 'read',
    0x40: 'write', 0x41: 'unlink', 0x42: 'lseek', 0x43: 'chmod',
    0x44: 'ioctl', 0x47: 'getcwd', 0x48: 'alloc', 0x49: 'free',
    0x4a: 'setblock', 0x4b: 'exec', 0x4c: 'exit', 0x4e: 'findfirst',
    0x4f: 'findnext', 0x56: 'rename', 0x57: 'file date/time',
    0x59: 'get extended error', 0x5b: 'create new file',
    0x62: 'get PSP', 0x67: 'set handle count', 0x68: 'commit file',
}

for want in ('0x21', '0x31', '0x2f', '0x10', '0x33'):
    sites = info['ints'].get(want, [])
    if not sites:
        continue
    print('\n' + '=' * 70)
    print('int %s  -- %d sites' % (want, len(sites)))
    print('=' * 70)
    funcs = Counter()
    unknown = []
    for a in sorted(sites):
        i = pos[a]
        val = None
        for j in range(i - 1, max(0, i - 14), -1):
            m = SET_AX.match(ins[addrs[j]])
            if m:
                v = int(m.group(2), 16 if m.group(2).startswith('0x') else 10)
                val = v if m.group(1) == 'ah' else (v >> 8) & 0xff
                break
        if val is None:
            unknown.append(a)
        else:
            funcs[val] += 1
    for f, n in sorted(funcs.items()):
        print('  ah=0x%02x  x%-3d  %s' % (f, n, DOS.get(f, '') if want == '0x21' else ''))
    if unknown:
        print('  ah set indirectly at %d sites: %s'
              % (len(unknown), ', '.join('0x%x' % x for x in unknown[:12])))
