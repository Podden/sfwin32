"""Recover a symbol map for the statically linked Miles Sound System.

MSS is built with a debug-logging facility: every AIL entry point logs its own
name and arguments through a format string. Those strings survived into the
binary, and each is referenced from inside the function it names -- so the
relocation table hands us the API map directly, without any guessing.
"""
import json
import re

info = json.load(open('relocs.json'))
B1 = info['objects'][0]['base']
B2 = info['objects'][1]['base']
o2 = open('obj2.bin', 'rb').read()

# Function starts, from the recursive-descent scan.
ins = {int(k, 16): v for k, v in json.load(open('scan.json'))['instructions'].items()}
starts = sorted({int(v.split('0x')[1], 16) for v in ins.values()
                 if v.startswith('call 0x')})

def enclosing(addr):
    """Nearest call target at or below addr."""
    lo, hi = 0, len(starts)
    while lo < hi:
        mid = (lo + hi) // 2
        if starts[mid] <= addr: lo = mid + 1
        else: hi = mid
    return starts[lo - 1] if lo else None

# site -> target, so we can find who points at a string
by_target = {}
for r in info['relocs']:
    by_target.setdefault(r['target'], []).append(r['addr'])

rows = []
for m in re.finditer(rb'AIL_[A-Za-z0-9_]+\(', o2):
    start = m.start()
    end = o2.find(b'\x00', start)
    if end < 0 or end - start > 80:
        continue
    text = o2[start:end].decode('latin1')
    addr = B2 + start
    for site in by_target.get(addr, []):
        rows.append((enclosing(site), site, addr, text))

rows.sort(key=lambda r: (r[0] or 0))
print('%-10s %-10s %-10s %s' % ('FUNC', 'XREF', 'STRING@', 'NAME'))
seen = set()
for fn, site, saddr, text in rows:
    if fn in seen:
        continue
    seen.add(fn)
    print('0x%08X 0x%08X 0x%08X %s' % (fn or 0, site, saddr, text))
print('\n%d AIL entry points located' % len(seen))
