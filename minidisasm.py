import struct, sys

path = sys.argv[1]
base = int(sys.argv[2], 16)
with open(path, 'rb') as f:
    data = f.read() + b'\x00' * 16

n = len(data) - 16
i = 0
out = []

def u32(off):
    return struct.unpack_from('<I', data, off)[0]

regnames = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
regs_push = {0x50:'eax',0x51:'ecx',0x52:'edx',0x53:'ebx',0x54:'esp',0x55:'ebp',0x56:'esi',0x57:'edi'}
regs_pop = {0x58:'eax',0x59:'ecx',0x5a:'edx',0x5b:'ebx',0x5c:'esp',0x5d:'ebp',0x5e:'esi',0x5f:'edi'}

def fmt_disp(disp):
    if disp == 0:
        return ""
    if disp > 0:
        return "+0x%x" % disp
    return "-0x%x" % (-disp)

while i < n:
    addr = base + i
    prefix_len = 0
    while i + prefix_len < n and data[i + prefix_len] == 0x2E:
        prefix_len += 1
    if prefix_len:
        i += prefix_len
        addr = base + i
        if i >= n:
            break
    b0 = data[i]
    if b0 == 0xE9:
        rel = struct.unpack_from('<i', data, i+1)[0]
        target = (addr + 5 + rel) & 0xffffffff
        out.append((addr, 5, "jmp 0x%x" % target))
        i += 5
        continue
    if b0 == 0x0F and i+1 < n and 0x80 <= data[i+1] <= 0x8F:
        cc = data[i+1]
        rel = struct.unpack_from('<i', data, i+2)[0]
        target = (addr + 6 + rel) & 0xffffffff
        out.append((addr, 6, "jcc_%02x 0x%x" % (cc, target)))
        i += 6
        continue
    if b0 == 0x0F and i+1 < n and 0x90 <= data[i+1] <= 0x9F:
        cc = data[i+1]
        modrm = data[i+2]
        mod = modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        length=3
        if mod==3:
            out.append((addr,length,"setcc_%02x %s" % (cc, regnames[rm])))
        else:
            base_reg = regnames[rm]
            disp=0
            if rm==4:
                sib=data[i+3]; length+=1; bs=sib&7; base_reg=regnames[bs]
            if mod==1:
                disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
            elif mod==2:
                disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
            out.append((addr,length,"setcc_%02x [%s]" % (cc, base_reg+fmt_disp(disp))))
        i += length
        continue
    if b0 in (0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47):
        out.append((addr,1,"inc %s" % regnames[b0-0x40]))
        i += 1
        continue
    if b0 in (0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F):
        out.append((addr,1,"dec %s" % regnames[b0-0x48]))
        i += 1
        continue
    if b0 == 0x88 and i+1 < n:
        modrm = data[i+1]
        mod=modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        regs8 = ['al','cl','dl','bl','ah','ch','dh','bh']
        length=2
        if mod==3:
            out.append((addr,length,"mov %s, %s" % (regs8[rm], regs8[reg])))
            i+=length
            continue
        base_reg = regnames[rm]
        disp=0
        if rm==4:
            sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
        if mod==1:
            disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
        elif mod==2:
            disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
        out.append((addr,length,"mov [%s], %s" % (base_reg+fmt_disp(disp), regs8[reg])))
        i+=length
        continue
    if b0 == 0x8A and i+1 < n:
        modrm = data[i+1]
        mod=modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        regs8 = ['al','cl','dl','bl','ah','ch','dh','bh']
        length=2
        if mod==3:
            out.append((addr,length,"mov %s, %s" % (regs8[reg], regs8[rm])))
            i+=length
            continue
        base_reg = regnames[rm]
        disp=0
        if rm==4:
            sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
        if mod==1:
            disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
        elif mod==2:
            disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
        out.append((addr,length,"mov %s, [%s]" % (regs8[reg], base_reg+fmt_disp(disp))))
        i+=length
        continue
    if b0 == 0xFF and i+1 < n:
        modrm = data[i+1]
        reg = (modrm>>3)&7
        if reg in (2, 3, 4, 5):
            mod = modrm>>6; rm=modrm&7
            length=2
            base_reg = regnames[rm]
            disp=0
            if mod!=3 and rm==4:
                sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
                ss=sib>>6; idx=(sib>>3)&7
                if idx!=4:
                    base_reg += "+%s*%d" % (regnames[idx], 1<<ss)
            if mod==1:
                disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
            elif mod==2:
                disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
            elif mod==0 and rm==5:
                disp=u32(i+length); length+=4; base_reg=None
            loc = (base_reg or "") + fmt_disp(disp)
            opn = {2:'call',3:'callf',4:'jmp',5:'jmpf'}[reg]
            out.append((addr,length,"%s [%s]" % (opn, loc)))
            i+=length
            continue
    if b0 in regs_push:
        out.append((addr, 1, "push " + regs_push[b0]))
        i += 1
        continue
    if b0 in regs_pop:
        out.append((addr, 1, "pop " + regs_pop[b0]))
        i += 1
        continue
    if b0 == 0xE8:
        rel = struct.unpack_from('<i', data, i+1)[0]
        target = (addr + 5 + rel) & 0xffffffff
        out.append((addr, 5, "call 0x%x" % target))
        i += 5
        continue
    if b0 == 0xC3:
        out.append((addr, 1, "ret"))
        i += 1
        continue
    if b0 == 0x90:
        out.append((addr, 1, "nop"))
        i += 1
        continue
    if b0 == 0x31 and i+1 < n:
        modrm = data[i+1]
        if modrm & 0xC0 == 0xC0:
            reg = regnames[(modrm>>3)&7]
            rm = regnames[modrm&7]
            out.append((addr, 2, "xor %s, %s" % (rm, reg)))
            i += 2
            continue
    if b0 == 0x89 and i+1 < n:
        modrm = data[i+1]
        mod = modrm >> 6
        reg = (modrm>>3)&7
        rm = modrm & 7
        if mod == 3:
            out.append((addr, 2, "mov %s, %s" % (regnames[rm], regnames[reg])))
            i += 2
            continue
        length = 2
        base_reg = regnames[rm]
        disp = 0
        if rm == 4:
            sib = data[i+2]
            length += 1
            bs = sib & 7
            base_reg = regnames[bs]
        if mod == 1:
            disp = struct.unpack_from('<b', data, i+length)[0]
            length += 1
        elif mod == 2:
            disp = struct.unpack_from('<i', data, i+length)[0]
            length += 4
        out.append((addr, length, "mov [%s], %s" % (base_reg+fmt_disp(disp), regnames[reg])))
        i += length
        continue
    if b0 == 0x85 and i+1 < n:
        modrm = data[i+1]
        if modrm & 0xC0 == 0xC0:
            reg = regnames[(modrm>>3)&7]
            rm = regnames[modrm&7]
            out.append((addr, 2, "test %s, %s" % (rm, reg)))
            i += 2
            continue
    if b0 == 0x74:
        rel = struct.unpack_from('<b', data, i+1)[0]
        target = (addr + 2 + rel) & 0xffffffff
        out.append((addr, 2, "jz 0x%x" % target))
        i += 2
        continue
    if b0 == 0x75:
        rel = struct.unpack_from('<b', data, i+1)[0]
        target = (addr + 2 + rel) & 0xffffffff
        out.append((addr, 2, "jnz 0x%x" % target))
        i += 2
        continue
    if b0 == 0xBA:
        val = u32(i+1)
        out.append((addr, 5, "mov edx, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xB8:
        val = u32(i+1)
        out.append((addr, 5, "mov eax, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xB9:
        val = u32(i+1)
        out.append((addr, 5, "mov ecx, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xBB:
        val = u32(i+1)
        out.append((addr, 5, "mov ebx, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xBC:
        val = u32(i+1)
        out.append((addr, 5, "mov esp, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xBD:
        val = u32(i+1)
        out.append((addr, 5, "mov ebp, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xBE:
        val = u32(i+1)
        out.append((addr, 5, "mov esi, 0x%x" % val))
        i += 5
        continue
    if b0 == 0xBF:
        val = u32(i+1)
        out.append((addr, 5, "mov edi, 0x%x" % val))
        i += 5
        continue
    if b0 == 0x6A:
        val = data[i+1]
        out.append((addr, 2, "push 0x%x" % val))
        i += 2
        continue
    if b0 == 0x68:
        val = u32(i+1)
        out.append((addr, 5, "push 0x%x" % val))
        i += 5
        continue
    if b0 == 0x8D:
        modrm = data[i+1]
        mod = modrm >> 6
        reg = (modrm>>3)&7
        rm = modrm & 7
        length = 2
        base_reg = regnames[rm]
        disp = 0
        if rm == 4:
            sib = data[i+2]
            length += 1
            ss = sib >> 6
            idx = (sib>>3)&7
            bs = sib & 7
            base_reg = regnames[bs]
            if idx != 4:
                base_reg += "+%s*%d" % (regnames[idx], 1<<ss)
        if mod == 1:
            disp = struct.unpack_from('<b', data, i+length)[0]
            length += 1
        elif mod == 2:
            disp = struct.unpack_from('<i', data, i+length)[0]
            length += 4
        elif mod == 0 and rm == 5:
            disp = u32(i+length)
            length += 4
            base_reg = None
        loc = (base_reg or "") + fmt_disp(disp)
        out.append((addr, length, "lea %s, [%s]" % (regnames[reg], loc)))
        i += length
        continue
    if b0 in (0xD9, 0xDC, 0xDD, 0xD8, 0xDB, 0xDE, 0xDF):
        modrm = data[i+1]
        mod = modrm >> 6
        reg = (modrm>>3)&7
        rm = modrm & 7
        length = 2
        base_reg = regnames[rm]
        disp = 0
        if mod != 3 and rm == 4:
            sib = data[i+2]
            length += 1
            bs = sib & 7
            base_reg = regnames[bs]
        if mod == 1:
            disp = struct.unpack_from('<b', data, i+length)[0]
            length += 1
        elif mod == 2:
            disp = struct.unpack_from('<i', data, i+length)[0]
            length += 4
        elif mod == 0 and rm == 5:
            disp = u32(i+length)
            length += 4
        mnemonic_map = {
          (0xD9,0):'fld', (0xD9,2):'fst', (0xD9,3):'fstp',
          (0xDC,0):'fadd', (0xDC,1):'fmul',
          (0xD8,0):'fadd', (0xD8,1):'fmul', (0xD8,4):'fsub', (0xD8,5):'fsubr', (0xD8,6):'fdiv', (0xD8,7):'fdivr',
          (0xDD,0):'fld', (0xDD,3):'fstp',
          (0xDB,0):'fild', (0xDB,3):'fistp',
          (0xDF,0):'fild', (0xDF,3):'fistp', (0xDF,5):'fild', (0xDF,7):'fistp',
          (0xDE,0):'fiadd', (0xDE,1):'fimul', (0xDE,4):'fisub', (0xDE,5):'fisubr', (0xDE,6):'fidiv', (0xDE,7):'fidivr',
        }
        mnem = mnemonic_map.get((b0,reg), "x87_%02x_%d" % (b0,reg))
        sz = 'qword' if b0 in (0xDC,0xDD,0xDF) else ('word' if b0 == 0xDE else 'dword')
        if mod == 3:
            out.append((addr, length, "%s st(%d)" % (mnem, rm)))
        else:
            out.append((addr, length, "%s %s [%s]" % (mnem, sz, base_reg + fmt_disp(disp))))
        i += length
        continue
    if b0 == 0x81 and i+1 < n:
        modrm = data[i+1]
        mod = modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        if mod==3:
            val = u32(i+2)
            opn = {0:'add',5:'sub'}.get(reg, 'grp1_%d' % reg)
            out.append((addr,6,"%s %s, 0x%x" % (opn, regnames[rm], val)))
            i+=6
            continue
    if b0 == 0x83 and i+1 < n:
        modrm = data[i+1]
        mod = modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        opn = {0:'add',1:'or',2:'adc',3:'sbb',4:'and',5:'sub',6:'xor',7:'cmp'}.get(reg, 'grp1_%d' % reg)
        if mod==3:
            val = struct.unpack_from('<b',data,i+2)[0]
            out.append((addr,3,"%s %s, %d" % (opn, regnames[rm], val)))
            i+=3
            continue
        length = 2
        base_reg = regnames[rm]
        disp = 0
        if rm == 4:
            sib = data[i+2]; length += 1
            bs = sib & 7; base_reg = regnames[bs]
        if mod == 1:
            disp = struct.unpack_from('<b', data, i+length)[0]; length += 1
        elif mod == 2:
            disp = struct.unpack_from('<i', data, i+length)[0]; length += 4
        val = struct.unpack_from('<b', data, i+length)[0]; length += 1
        out.append((addr, length, "%s dword [%s], %d" % (opn, base_reg+fmt_disp(disp), val)))
        i += length
        continue
    if b0 == 0xFF:
        modrm = data[i+1]
        mod=modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        length=2
        base_reg = regnames[rm]
        disp=0
        if mod!=3 and rm==4:
            sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
        if mod==1:
            disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
        elif mod==2:
            disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
        if reg==6:
            out.append((addr,length,"push [%s]" % (base_reg+fmt_disp(disp))))
        else:
            out.append((addr,length,"ff_group5_%d [%s]" % (reg, base_reg+fmt_disp(disp))))
        i+=length
        continue
    if b0 == 0xA1:
        val = u32(i+1)
        out.append((addr,5,"mov eax, [0x%x]" % val))
        i+=5
        continue
    if b0 == 0x8B:
        modrm = data[i+1]
        mod=modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        length=2
        base_reg = regnames[rm]
        disp=0
        if mod!=3 and rm==4:
            sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
        if mod==1:
            disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
        elif mod==2:
            disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
        if mod==3:
            out.append((addr,length,"mov %s, %s" % (regnames[reg], regnames[rm])))
        else:
            out.append((addr,length,"mov %s, [%s]" % (regnames[reg], base_reg+fmt_disp(disp))))
        i+=length
        continue
    if b0 == 0xC7:
        modrm = data[i+1]
        mod=modrm>>6; reg=(modrm>>3)&7; rm=modrm&7
        length=2
        base_reg = regnames[rm]
        disp=0
        if mod!=3 and rm==4:
            sib=data[i+2]; length+=1; bs=sib&7; base_reg=regnames[bs]
        if mod==1:
            disp=struct.unpack_from('<b',data,i+length)[0]; length+=1
        elif mod==2:
            disp=struct.unpack_from('<i',data,i+length)[0]; length+=4
        val = u32(i+length); length+=4
        out.append((addr,length,"mov dword [%s], 0x%x" % (base_reg+fmt_disp(disp), val)))
        i+=length
        continue
    out.append((addr, 1, "?? 0x%02x" % b0))
    i += 1

for addr, length, text in out:
    print("0x%06x  %s" % (addr, text))
