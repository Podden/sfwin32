"""Extract and relocate the objects of a DOS/4GW LE executable.

Produces a flat memory image identical to what DOS/4GW would have in RAM
after loading and applying fixups, plus a machine-readable description of
every relocation site (needed to tell code pointers from data).
"""
import json
import struct
import sys

PAGE = 0x1000


class LE:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        d = self.raw
        self.le = struct.unpack_from('<I', d, 0x3c)[0]
        if d[self.le:self.le + 2] != b'LE':
            raise SystemExit('not an LE image')
        u32 = lambda o: struct.unpack_from('<I', d, self.le + o)[0]
        self.num_pages = u32(0x14)
        self.eip_obj, self.eip = u32(0x18), u32(0x1c)
        self.esp_obj, self.esp = u32(0x20), u32(0x24)
        self.page_size = u32(0x28)
        self.last_page_size = u32(0x2c)
        self.obj_table = self.le + u32(0x40)
        self.num_objects = u32(0x44)
        self.page_map = self.le + u32(0x48)
        self.fixup_page_table = self.le + u32(0x68)
        self.fixup_record_table = self.le + u32(0x6c)
        self.data_pages = u32(0x80)

        self.objects = []
        for i in range(self.num_objects):
            size, base, flags, pmi, pcount, _ = struct.unpack_from(
                '<IIIIII', d, self.obj_table + i * 24)
            self.objects.append(dict(index=i + 1, virt_size=size, base=base,
                                     flags=flags, page_index=pmi,
                                     page_count=pcount))

    def object_bytes(self, obj):
        """Raw (pre-fixup) bytes of one object, zero-padded to virt_size."""
        d = self.raw
        out = bytearray()
        for k in range(obj['page_count']):
            idx = obj['page_index'] + k            # 1-based page-map index
            hi, lo, flags = struct.unpack_from('>HBB', d, self.page_map +
                                               (idx - 1) * 4)
            page = (hi << 8) | lo
            if flags != 0:
                raise SystemExit('page %d has unsupported flags %d' % (idx, flags))
            off = self.data_pages + (page - 1) * self.page_size
            n = self.last_page_size if page == self.num_pages else self.page_size
            out += d[off:off + n]
        out = out[:obj['virt_size']]
        out += b'\x00' * (obj['virt_size'] - len(out))
        return out

    def fixups(self):
        """Yield (page_map_index, page_offset, src_type, target_obj, target_off)."""
        d = self.raw
        fpt, frt = self.fixup_page_table, self.fixup_record_table
        offs = [struct.unpack_from('<I', d, fpt + i * 4)[0]
                for i in range(self.num_pages + 1)]
        for p in range(self.num_pages):
            i, end = frt + offs[p], frt + offs[p + 1]
            while i < end:
                src, flags = d[i], d[i + 1]
                i += 2
                srcoffs = []
                if src & 0x20:                      # source list
                    count = d[i]
                    i += 1
                else:
                    srcoffs.append(struct.unpack_from('<h', d, i)[0])
                    i += 2
                if (flags & 3) != 0:
                    raise SystemExit('external fixup not supported')
                if flags & 0x40:
                    tobj = struct.unpack_from('<H', d, i)[0]; i += 2
                else:
                    tobj = d[i]; i += 1
                toff = 0
                if (src & 0x0f) != 2:               # selector-only has no offset
                    if flags & 0x10:
                        toff = struct.unpack_from('<I', d, i)[0]; i += 4
                    else:
                        toff = struct.unpack_from('<H', d, i)[0]; i += 2
                if src & 0x20:
                    for _ in range(count):
                        srcoffs.append(struct.unpack_from('<h', d, i)[0])
                        i += 2
                for so in srcoffs:
                    yield (p + 1, so, src & 0x0f, tobj, toff)


def build(path):
    le = LE(path)
    images = {o['index']: bytearray(le.object_bytes(o)) for o in le.objects}
    bases = {o['index']: o['base'] for o in le.objects}
    # page-map index -> (object index, offset of that page within the object)
    owner = {}
    for o in le.objects:
        for k in range(o['page_count']):
            owner[o['page_index'] + k] = (o['index'], k * PAGE)

    sites = []
    skipped = []
    for pidx, soff, stype, tobj, toff in le.fixups():
        oidx, pbase = owner[pidx]
        off = pbase + soff
        target = bases[tobj] + toff
        if stype == 7:                              # 32-bit absolute offset
            # A fixup may straddle a page boundary; soff can be negative or
            # run past PAGE, which is exactly why we work on whole objects.
            if off < 0 or off + 4 > len(images[oidx]):
                skipped.append((oidx, off, stype, tobj, toff))
                continue
            struct.pack_into('<I', images[oidx], off, target)
            sites.append(dict(obj=oidx, off=off, addr=bases[oidx] + off,
                              type=stype, tobj=tobj, toff=toff, target=target))
        else:
            skipped.append((oidx, off, stype, tobj, toff))
    return le, images, bases, sites, skipped


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'FIGHT.EXE'
    le, images, bases, sites, skipped = build(path)

    for o in le.objects:
        i = o['index']
        print('OBJ%d base=0x%08x size=0x%x flags=0x%x'
              % (i, o['base'], o['virt_size'], o['flags']))
        open('obj%d.bin' % i, 'wb').write(images[i])
    print('entry  = 0x%08x' % (bases[le.eip_obj] + le.eip))
    print('stack  = 0x%08x' % (bases[le.esp_obj] + le.esp))
    print('fixups applied = %d, skipped = %r' % (len(sites), skipped))

    json.dump(dict(entry=bases[le.eip_obj] + le.eip,
                   stack=bases[le.esp_obj] + le.esp,
                   objects=[dict(index=o['index'], base=o['base'],
                                 size=o['virt_size'], flags=o['flags'])
                            for o in le.objects],
                   relocs=sites),
              open('relocs.json', 'w'))
    print('wrote obj1.bin, obj2.bin, relocs.json')
