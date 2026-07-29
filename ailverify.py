"""Verify the AIL symbol map the hard way.

ailmap.py pairs each debug string with the nearest preceding function start,
which is a good hint but not proof -- one entry (0x40F30) turned out to be
AIL_sample_status rather than the set_sample_loop_block the adjacent string
named. Every wrapper pushes its own format string before calling the logger,
so scanning forward from a function start for that push identifies it beyond
doubt.
"""
import json
import re
import sys

info = json.load(open('relocs.json'))
B2 = info['objects'][1]['base']
o2 = open('obj2.bin', 'rb').read()
ins = {int(k, 16): v for k, v in
       json.load(open('scan.json'))['instructions'].items()}

def string_at(addr):
    p = addr - B2
    if p < 0 or p >= len(o2):
        return None
    e = o2.find(b'\x00', p)
    if e < 0 or e - p > 90:
        return None
    return o2[p:e].decode('latin1')

PUSH = re.compile(r'^push 0x([0-9a-f]+)$')

def identify(fn, span=0x260):
    """The first push of an AIL_ format string inside the function names it."""
    for a in sorted(ins):
        if not (fn <= a < fn + span):
            continue
        m = PUSH.match(ins[a])
        if not m:
            continue
        s = string_at(int(m.group(1), 16))
        if s and s.startswith('AIL_'):
            return s.split('(')[0], a
    return None, None

# What audio.c hooks, as (address, the name the code assumes).
HOOKS = [
    (0x00041EC0, 'AIL_install_DIG_INI'),
    (0x00041D00, 'AIL_uninstall_DIG_driver'),
    (0x00041C00, 'AIL_allocate_sample_handle'),
    (0x00041A30, 'AIL_release_sample_handle'),
    (0x00041980, 'AIL_init_sample'),
    (0x00041790, 'AIL_set_sample_address'),
    (0x000416C0, 'AIL_set_sample_type'),
    (0x00041610, 'AIL_start_sample'),
    (0x00041400, 'AIL_end_sample'),
    (0x00041340, 'AIL_set_sample_playback_rate'),
    (0x00041280, 'AIL_set_sample_volume'),
    (0x000411C0, 'AIL_set_sample_pan'),
    (0x00041100, 'AIL_set_sample_loop_count'),
    (0x00040F30, 'AIL_sample_status'),
    (0x00040A70, 'AIL_set_digital_master_volume'),
    (0x0003FAC0, 'AIL_sample_user_data'),
    (0x00023DE0, '(game) voice pool setup'),
]

bad = 0
print('%-10s %-32s %-32s' % ('ADDR', 'ASSUMED', 'ACTUAL (from its own string)'))
for addr, assumed in HOOKS:
    actual, site = identify(addr)
    if assumed.startswith('(game)'):
        print('0x%08X %-32s %-32s' % (addr, assumed, actual or '(no AIL string)'))
        continue
    ok = (actual == assumed)
    if not ok:
        bad += 1
    print('0x%08X %-32s %-32s %s' % (addr, assumed, actual or '???',
                                     'ok' if ok else '<<< MISMATCH'))
print()
print('%d mismatch(es)' % bad)
sys.exit(1 if bad else 0)
