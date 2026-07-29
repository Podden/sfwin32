"""Emit the verified Miles hook table for whichever image was extracted here.

ailverify.py proves a *given* address is the function it is assumed to be, by
finding the AIL_ format string that function pushes for its own debug log.
This inverts that: enumerate every function and let each one name itself, so
the table for a new binary is derived rather than guessed.

That matters because the game ships two DOS/4GW executables -- FIGHT.EXE and
DESKTOP.EXE -- built from the same engine at different link addresses. None of
FIGHT's 16 hook addresses are valid in DESKTOP, and a 5-byte JMP written at a
wrong address corrupts whatever really lives there.

Run in a directory holding obj1.bin/obj2.bin/relocs.json/scan.json:

    python le_extract.py <IMAGE>  &&  python scan.py  &&  python ailtable.py
"""
import json
import re
import sys

info = json.load(open('relocs.json'))
B2 = info['objects'][1]['base']
o2 = open('obj2.bin', 'rb').read()
scan = json.load(open('scan.json'))
ins = {int(k, 16): v for k, v in scan['instructions'].items()}
addrs = sorted(ins)

PUSH = re.compile(r'^push 0x([0-9a-f]+)$')
CALL = re.compile(r'^call 0x([0-9a-f]+)$')


def string_at(addr):
    p = addr - B2
    if p < 0 or p >= len(o2):
        return None
    e = o2.find(b'\x00', p)
    if e < 0 or e - p > 90:
        return None
    return o2[p:e].decode('latin1')


# Function starts: anything the image calls directly. A wrapper that is only
# ever reached indirectly would be missed, but every AIL entry point the game
# uses is called by name.
starts = set()
for a in addrs:
    m = CALL.match(ins[a])
    if m:
        starts.add(int(m.group(1), 16))
starts = sorted(starts)

# Each function is named by the first AIL_ format string it pushes. Stop at the
# next function start so a short function cannot borrow its neighbour's string
# -- that adjacency assumption is exactly what made ailmap.py wrong 6 times
# out of 16 on FIGHT.EXE.
found = {}
for i, fn in enumerate(starts):
    end = starts[i + 1] if i + 1 < len(starts) else fn + 0x400
    for a in addrs:
        if a < fn:
            continue
        if a >= end:
            break
        m = PUSH.match(ins[a])
        if not m:
            continue
        s = string_at(int(m.group(1), 16))
        if s and s.startswith('AIL_'):
            found.setdefault(s.split('(')[0], fn)
            break

# The entry points audio.c replaces.
WANTED = [
    'AIL_install_DIG_INI',
    'AIL_uninstall_DIG_driver',
    'AIL_allocate_sample_handle',
    'AIL_release_sample_handle',
    'AIL_init_sample',
    'AIL_set_sample_address',
    'AIL_set_sample_type',
    'AIL_start_sample',
    'AIL_end_sample',
    'AIL_set_sample_playback_rate',
    'AIL_set_sample_volume',
    'AIL_set_sample_pan',
    'AIL_set_sample_loop_count',
    'AIL_sample_status',
    'AIL_set_digital_master_volume',
    'AIL_sample_user_data',
]

missing = [n for n in WANTED if n not in found]

print('/* Derived by ailtable.py -- each address proved by the AIL_ format')
print(' * string that function pushes for its own debug logging. */')
for n in WANTED:
    if n in found:
        print('#define %-34s 0x%08Xu' % (n.upper(), found[n]))
    else:
        print('/* %-34s NOT FOUND */' % n.upper())

print()
print('%d/%d resolved' % (len(WANTED) - len(missing), len(WANTED)))
if missing:
    print('missing: ' + ', '.join(missing))
print('(%d AIL functions identified in total)' % len(found))
sys.exit(1 if missing else 0)
