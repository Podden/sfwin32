"""Derive the Miles hook table for any LE image, including the 3dfx build.

Same principle as ailtable.py: every AIL wrapper pushes a format string naming
itself for Miles' own debug log, so each function names itself rather than
being guessed from its neighbours. This version carries its own LE loader so
it works on images with more than two objects -- FIGHT3.EXE has five, with the
code in object 2 and the strings in object 5 -- and it applies the fixups,
without which every push/call operand is still an unrelocated placeholder.

    python ailtable3.py FIGHT3.EXE
"""
import struct, sys, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]

def load(path):
    raw = open(path, 'rb').read()
    le = u32(raw, 0x3c)
    h = lambda o: u32(raw, le + o)
    npages, psize, lastpage = h(0x14), h(0x28), h(0x2C)
    objtab, nobj, pagemap = le + h(0x40), h(0x44), le + h(0x48)
    fixpage, fixrec, datapages = le + h(0x68), le + h(0x6C), h(0x80)

    objs = []
    for i in range(nobj):
        o = objtab + i * 24
        objs.append(dict(vs=u32(raw,o), base=u32(raw,o+4), fl=u32(raw,o+8),
                         pi=u32(raw,o+12), pc=u32(raw,o+16)))
    lo = objs[0]['base']
    hi = objs[-1]['base'] + objs[-1]['vs']
    mem = bytearray(hi - lo)
    for ob in objs:
        off = 0
        for k in range(ob['pc']):
            idx = ob['pi'] + k
            e = raw[pagemap + (idx-1)*4: pagemap + idx*4]
            page = (e[0] << 16) | (e[1] << 8) | e[2]
            n = lastpage if page == npages else psize
            if off >= ob['vs']: break
            n = min(n, ob['vs'] - off)
            src = datapages + (page-1)*psize
            mem[ob['base']-lo+off : ob['base']-lo+off+n] = raw[src:src+n]
            off += n

    # fixups, at link-time addresses (delta 0)
    fpt = [u32(raw, fixpage + 4*i) for i in range(npages + 1)]
    applied = 0
    for p in range(npages):
        r, end = fixrec + fpt[p], fixrec + fpt[p+1]
        oi = None
        for i, ob in enumerate(objs):
            if ob['pc'] and ob['pi'] <= p+1 < ob['pi'] + ob['pc']:
                oi = i; break
        if oi is None: continue
        pbase = (p + 1 - objs[oi]['pi']) * 0x1000
        objoff = objs[oi]['base'] - lo
        while r < end:
            src_t, flg = raw[r], raw[r+1]
            r += 2
            lst = None; soff_one = 0
            if src_t & 0x20:
                count = raw[r]; r += 1
            else:
                soff_one = struct.unpack_from('<h', raw, r)[0]; r += 2; count = 1
            if (flg & 3) != 0: break
            if flg & 0x40: tobj = u16(raw, r); r += 2
            else:          tobj = raw[r]; r += 1
            toff = 0
            if (src_t & 0x0f) != 2:
                if flg & 0x10: toff = u32(raw, r); r += 4
                else:          toff = u16(raw, r); r += 2
            if src_t & 0x20:
                lst = r; r += 2*count
            if not (1 <= tobj <= len(objs)): continue
            target = objs[tobj-1]['base'] + toff
            for j in range(count):
                soff = struct.unpack_from('<h', raw, lst + 2*j)[0] if lst is not None else soff_one
                at = pbase + soff
                if (src_t & 0x0f) != 7: continue
                if at < 0 or at + 4 > objs[oi]['vs']: continue
                struct.pack_into('<I', mem, objoff + at, target)
                applied += 1
    return mem, lo, objs, applied

mem, LO, objs, nfix = load(sys.argv[1])
sys.stderr.write("loaded %s: %d objects, %d fixups applied\n"
                 % (sys.argv[1], len(objs), nfix))

def string_at(a):
    p = a - LO
    if p < 0 or p >= len(mem): return None
    e = mem.find(b'\x00', p)
    if e < 0 or e - p > 90: return None
    try: return mem[p:e].decode('latin1')
    except Exception: return None

md = Cs(CS_ARCH_X86, CS_MODE_32)
calls, pushes = set(), []
for ob in objs:
    if not (ob['fl'] & 4) or not ob['vs']: continue      # executable only
    base, size = ob['base'], min(ob['vs'], len(mem) - (ob['base']-LO))
    code = bytes(mem[ob['base']-LO : ob['base']-LO+size])
    # Linear sweep that resyncs: capstone stops at the first byte that is not
    # an instruction, and a megabyte of code has plenty of embedded data. One
    # missed function is one missing hook, so step over the bad byte and carry
    # on rather than stopping at the first table.
    off = 0
    while off < size:
        n = 0
        for ins in md.disasm(code[off:], base + off):
            n += 1
            if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
                calls.add(int(ins.op_str, 16))
            elif ins.mnemonic == 'push' and ins.op_str.startswith('0x'):
                pushes.append((ins.address, int(ins.op_str, 16)))
            off = ins.address - base + ins.size
        if n == 0:
            off += 1

starts = sorted(calls)
pushes.sort()
sys.stderr.write("%d call targets, %d immediate pushes\n" % (len(starts), len(pushes)))

import bisect
found = {}
for i, fn in enumerate(starts):
    end = starts[i+1] if i+1 < len(starts) else fn + 0x400
    j = bisect.bisect_left(pushes, (fn, 0))
    while j < len(pushes) and pushes[j][0] < end:
        s = string_at(pushes[j][1])
        if s and s.startswith('AIL_'):
            found.setdefault(s.split('(')[0], fn)
            break
        j += 1

WANTED = ['AIL_install_DIG_INI','AIL_uninstall_DIG_driver',
    'AIL_allocate_sample_handle','AIL_release_sample_handle','AIL_init_sample',
    'AIL_set_sample_address','AIL_set_sample_type','AIL_start_sample',
    'AIL_end_sample','AIL_set_sample_playback_rate','AIL_set_sample_volume',
    'AIL_set_sample_pan','AIL_set_sample_loop_count','AIL_sample_status',
    'AIL_set_digital_master_volume','AIL_sample_user_data']

missing = [n for n in WANTED if n not in found]
import os
name = os.path.basename(sys.argv[1]).split(".")[0].lower()
print("static const uint32_t g_ail_%s[A_COUNT] = {" % name)
for i in range(0, len(WANTED), 4):
    row = "    " + " ".join(
        ("0x%08Xu," % found[n]) if n in found else "0x00000000u,"
        for n in WANTED[i:i+4])
    print(row)
print("};")
print()
print("%d/%d resolved  (%d AIL functions identified in total)"
      % (len(WANTED)-len(missing), len(WANTED), len(found)))
if missing: print("missing: " + ", ".join(missing))
sys.exit(1 if missing else 0)
