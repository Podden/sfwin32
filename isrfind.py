"""Locate the port's patch sites in an extracted image by byte signature.

The port needs four fixes that are all addresses in one specific binary, so
moving to a second binary means finding each one again. Reading disassembly by
eye is how you end up patching a function that merely starts with the same
opcode -- `cmp dword ptr [m32], 0` is 833D, and there are many of those.

Each site is found here by a signature that encodes what makes it that site:

  isr_stack    66 8C D8 66 8E D0 BC imm32
               mov ax,ds / mov ss,ax / mov esp,imm32 -- the timer ISR
               switching to its own stack. Nothing else in the image does
               this, and the imm32 is the stack address to relocate.

  device_api   the 833D immediately after that stack switch: the ISR testing
               the DPMI device-API vector before making a far call through it.

  bios_chain   the 833D whose branch leads to a `retf` (CB) -- the ISR
               chaining to the original BIOS int 8 handler.

Run in a directory holding obj1.bin and relocs.json.
"""
import json
import re
import struct
import sys

info = json.load(open('relocs.json'))
B1 = info['objects'][0]['base']
o1 = open('obj1.bin', 'rb').read()

STACK_SIG = bytes.fromhex('668CD8668ED0BC')


def find_all(sig, blob):
    out, i = [], 0
    while True:
        i = blob.find(sig, i)
        if i < 0:
            return out
        out.append(i)
        i += 1


hits = find_all(STACK_SIG, o1)
if not hits:
    print('no ISR stack switch found -- signature absent')
    sys.exit(1)

print('%-14s %-12s %s' % ('SITE', 'ADDRESS', 'DETAIL'))
for off in hits:
    stack_imm_off = off + len(STACK_SIG)
    stack_imm = struct.unpack_from('<I', o1, stack_imm_off)[0]
    mov_esp_at = B1 + off + 6          # the BC opcode byte
    print('%-14s 0x%08X   mov esp, 0x%08X  (imm at 0x%08X)'
          % ('isr_stack', mov_esp_at, stack_imm, B1 + stack_imm_off))

    # The device-API test is the next `cmp dword ptr [m32], 0` after it.
    tail = o1[stack_imm_off:stack_imm_off + 0x40]
    m = tail.find(bytes.fromhex('833D'))
    if m >= 0:
        vec = struct.unpack_from('<I', tail, m + 2)[0]
        addr = B1 + stack_imm_off + m
        # Where the "no callout" path resumes: the je that skips the call.
        je = tail.find(bytes.fromhex('74'), m)
        target = None
        if je >= 0:
            target = B1 + stack_imm_off + je + 2 + o1[stack_imm_off + je + 1]
        print('%-14s 0x%08X   cmp dword ptr [0x%08X], 0%s'
              % ('device_api', addr, vec,
                 '   skip-to 0x%08X' % target if target else ''))
        if target:
            rel = target - (addr + 2)
            if 0 <= rel < 0x80:
                print('%-14s %-12s --patch %X=EB%02X'
                      % ('', '', addr, rel))

    # The BIOS chain test: the next 833D whose neighbourhood contains a retf.
    win = o1[off:off + 0x400]
    for m in [x.start() for x in re.finditer(re.escape(bytes.fromhex('833D')), win)]:
        seg = win[m:m + 0x60]
        if bytes.fromhex('CB') not in seg:
            continue
        addr = B1 + off + m
        vec = struct.unpack_from('<I', win, m + 2)[0]
        print('%-14s 0x%08X   cmp dword ptr [0x%08X], 0   (retf within 0x60)'
              % ('bios_chain', addr, vec))
        break
