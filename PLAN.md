# FIGHT.EXE — static port to Win32 without decompiling

> **Status: the port works and the game is playable.** See
> [`port/README.md`](port/README.md). `fightport.exe` loads the original image,
> services DOS/DPMI/VESA/IRQ from a vectored exception handler, and runs the
> game into its mission — 3D cockpit at 640x480x16 through a linear
> framebuffer, timer at the game's programmed 200 Hz, keyboard steering the
> submarine. No command-line patches required. The analysis below is what the
> implementation was built from; where reality differed it is marked with a
> correction.

## What the binary actually is

`FIGHT.EXE` is an MZ stub + **LE (linear executable)** for DOS/4GW, Watcom-linked.

| | |
|---|---|
| LE header | file offset `0x0000B610` |
| Objects | **2** |
| OBJ1 | code + rodata, linear base **`0x00010000`**, virtual size `0x12B822`, READ/EXEC, 32-bit |
| OBJ2 | data + bss + stack, linear base **`0x00140000`**, virtual size `0x36500`, READ/WRITE, 32-bit |
| Entry | **`0x00029754`** (obj1 + `0x19754`) |
| Initial ESP | `0x00176500` (top of obj2) |
| Pages | 331 × 4096, page map fully sequential, no iterated/compressed pages |
| Fixups | **20165, all internal**, 20164 × type 7 (absolute 32-bit), 1 × type 2 (selector) |
| Imports | **none** |

Two properties make the "keep the machine code, patch the edges" approach
viable:

1. **A complete relocation table.** All 20165 fixups are internal and typed, so
   every absolute pointer in the image is known precisely. Nothing has to be
   inferred.
2. **No imports and no external dependencies.** The image is entirely
   self-contained; the only things it needs from outside are DOS, DPMI and the
   BIOS, and that surface is small (below).

> **Correction (measured, not assumed).** An earlier draft of this plan claimed
> the objects could be mapped at their original bases `0x10000` / `0x140000`.
> **They cannot.** `vehtest` shows both `VirtualAlloc` calls failing with
> `ERROR_INVALID_ADDRESS` (487): by the time any user code runs, the CRT heap
> and two thread stacks already occupy `0x10000`–`0x1FFFFF`. That region is
> claimed during process creation, so no TLS callback or early initializer can
> get in front of it.
>
> This costs nothing, because the fixup table is exactly what makes rebasing
> possible. **Map both objects wherever the OS offers, and add the delta to all
> 20164 fixup targets at load time.** `le_extract.py` already takes the bases
> as parameters.
>
> Rebasing is sound by the format's contract — DOS/4GW places objects at
> whatever linear address is free, so every location that must change is in the
> table. Verified directly: the immediate of `mov edx, 0x140195` at `0x14676`
> *is* a fixup site targeting `0x140195`, while `rel32` call operands correctly
> are not (PC-relative, so they move with the code). In obj2, 89% of aligned
> dwords that fall inside an object range are fixup-covered; the remaining 155
> are consistent with coincidental data and are worth a spot check only if
> something misbehaves.
>
> Keep the two objects at their original **relative** distance (one allocation
> of `0x166500` bytes, obj1 at `+0`, obj2 at `+0x130000`). Nothing requires it,
> but it costs nothing and defuses any accidental relative assumption.

## The DOS surface to replace

Recursive-descent disassembly from the entry point plus all 5502 code-targeted
relocations reaches ~875 KB (71%) of obj1. Everything that cannot run outside
DOS/4GW:

### Software interrupts — 107 sites total

| int | sites | what |
|---|---|---|
| `0x21` | 83 | Watcom C runtime. open/close/read/write/lseek/creat/unlink/getcwd (35), set/get int vector `AH=25/35` (20), DOS memory alloc/free/setblock `AH=48/49/4A` (9), time/date `AH=2C/2A` (7), DOS version (3), exit (1) |
| `0x31` | 11 | DPMI — descriptor alloc, segment-to-descriptor, free memory block |
| `0x2F` | 8 | multiplex — DPMI/Windows detection (`AX=1687`) |
| `0x10` | 3 | **VESA VBE**, all `AX=4F05` (bank switch) |
| `0x33` | 1 | mouse init |

### Graphics — one compact module

The VBE driver is clustered in a single region around **`0x38900`–`0x39000`**:

- `0x38922` → `AX=4F00` get controller info
- `0x38A20` → `AX=4F01` get mode info
- `0x38BD6` → `AX=4F02` set mode
- `0x38E21` → `AX=4F07` set display start

Plus bank-switch and palette helpers at **`0xA8240`–`0xA8300`**:

```
0x0A8270  mov eax, 0x4f05          ; VBE bank switch
0x0A827A  mov cl,  [0x15EF58]      ; window granularity shift
0x0A8282  cmp dword [0x15EF4C], 0  ; VBE 2.0 protected-mode bank func?
0x0A828B  call dword [0x15EF4C]    ;   yes -> call it directly
0x0A8293  int 0x10                 ;   no  -> real-mode BIOS
```

and a DAC palette loop writing R/G/B triples via `out dx, al` (`0x3C8`/`0x3C9`).

This is **banked VBE**, not a linear framebuffer — the game paints through a
64 KB window.

**The window is not reachable through a flat pointer.** obj1 spans
`0x10000`–`0x13B822`, which *contains* `0xA0000`, so the game cannot be using a
flat address for video memory. It gets a selector instead — via DPMI
`int 31h AX=0002` (segment `0xA000` to descriptor) — and writes through
`es:`/`ds:`. That is what the 258 `mov Sreg`, 11 `les` and 10 `lds` sites are
for.

Win32 cannot honour a foreign selector: `vehtest` confirms `mov es, <selector>`
raises `0xC0000005`. Emulating every `es:`-prefixed access through the handler
would be far too slow for a framebuffer.

**So do not emulate the selector — control the module.** Traced statically, the
graphics path is far narrower than the raw counts suggest.

**Explicit segment overrides are rare.** Of the 2155 operands capstone renders
with `es:`, 2080 are *implicit* string-instruction operands (`rep movsd` and
friends) where ES is part of the encoding, not an override — those are already
correct, because ES == DS == flat under Win32. Only **132 instructions carry a
real `0x26` prefix**, in 12 clusters, and none of them are in the graphics
module. Likewise, of 299 writes to ES, **216 are `pop es`** restores.

**The selector is one global, used in a two-instruction window.** At `0xa8318`:

```
0x0a8318  push es
0x0a831b  mov  bx, word ptr [0x15eed8]   ; <- the video selector, one global
0x0a8322  mov  es, ebx
0x0a8326  mov  cl, byte ptr [0x15ef58]   ; window granularity shift
0x0a832e  call dword ptr [0x156944]      ; VBE 2.0 PM bank-switch function
0x0a8336  pop  es
```

ES is loaded only to satisfy the VBE 2.0 protected-mode bank function, then
immediately restored. **It is not held across pixel writes.**

**Everything funnels through very few entry points:**

| what | where |
|---|---|
| VBE real-mode call dispatcher (`4F01`/`4F02`/`4F03`/`4F05` all route here) | `0x39c8c`, and `0x38800` for calls needing a transfer buffer |
| VBE wrappers (thin, one per function) | `0x38a18`, `0x38bd0`, `0x38c00`, `0x38c38`, `0x38c78` |
| bank switch / palette fast path | `0xa8270`–`0xa8340` |
| blit module | ~`0xa7700`–`0xa8500` |
| video selector | `[0x15eed8]` |
| PM bank function ptr | `[0x156944]`, `[0x15ef4c]` |
| window granularity shift | `[0x15ef58]` |
| blit dest / source | `[0x15ef88]` / `[0x15ef84]` |
| pitch, geometry | `[0x15ef38]`, `[0x15ef5c]`, `[0x15ef8c]` |

**Consequence: the selector never has to work.** Point `[0x15ef88]` at a plain
64 KB buffer you allocate, make the bank-switch function pointer yours, and the
existing `rep movsd` blits land in your buffer through a flat ES — untouched,
at full speed, with no faults. Then present that buffer with SDL.

The remaining unknown is what writes `[0x15eed8]`; no store to it appears in
the statically reached code, so it is set through a pointer or from a path the
sweep did not reach. The VEH log will show it on the first run — it is not a
blocker, because a cookie value is enough once the bank function is yours.

### Port I/O — ~191 raw sites

Real ones: VGA sequencer/CRTC/DAC via `dx` (`0x3C0`–`0x3DA`), keyboard `0x60`,
PIC `0x20`, PIT `0x40`/`0x43`, sound card via `dx`. A number of the 191 are
false positives from data decoded as code (`lldt`, `ltr`, `verw`, `str`,
`arpl` in the inventory are the tell) — the true count is lower.

### Other

- `cli`/`sti` — 61 sites, all ignorable.
- Segment-register ops — 334 `push seg`, 295 `pop seg`, 258 `mov seg`. Harmless
  as long as every selector the game ever loads equals the flat Win32 DS.
- 1388 indirect call/jmp sites. **Irrelevant for the loader approach**; the
  main reason not to attempt a full source-level asm rebuild.

## Recommended approach: loader + fault-handler shim

Do **not** disassemble-and-reassemble. Instead:

1. A small 32-bit Win32 EXE maps one `0x166500`-byte block wherever the OS
   offers, writes obj1 at `+0` and obj2 at `+0x130000` with **all 20164 fixups
   rebased by the delta**, and `VirtualProtect`s the obj1 range to
   `PAGE_EXECUTE_READ`.
2. Install a **vectored exception handler** that decodes the faulting bytes at
   `EIP`, services the call by reading and writing the `CONTEXT` registers,
   advances `EIP`, and returns `EXCEPTION_CONTINUE_EXECUTION`.
3. Jump to the rebased entry point (`0x29754 + delta`).

The binary is never modified. Nothing needs to be understood except the ~107
interrupt sites and a handful of I/O ports — and those are *ABI contracts*
(documented DOS/DPMI/VBE calls), not game logic.

For hot paths where a fault per call is too slow — the bank switch, the palette
loop, the keyboard and timer ISRs — overwrite the **function prologue** with a
5-byte `jmp` to a native C implementation. That is the "change behaviour at key
locations" step, and it is a handful of functions, not hundreds.

### Two things to settle early

- **Stack.** `ESP = 0x176500` is set by the DOS/4GW *loader*, not by game code,
  so you are free to enter at `0x29754` on the normal Win32 thread stack (link
  with a large `/STACK`). That avoids calling Win32/SDL on a stack the OS does
  not own. Fall back to the original stack only if the Watcom runtime turns out
  to derive its heap bounds from `ESP`.
- **Selectors.** Make DPMI descriptor allocation (`int 31h AX=0000`) return the
  process's own DS. Then every `mov es, sel` the game performs is a no-op that
  happens to be correct.

### Verified on this machine

`vehtest/` (CMake + MSVC, `-A Win32`) executes a payload from a mapped,
`PAGE_EXECUTE_READ` object-1 region and services it from a VEH. All checks
pass on Windows 11 26100, MSVC 19.38, 32-bit process:

| behaviour | exception code | result |
|---|---|---|
| `int 0x21` | `0xC0000005` ACCESS_VIOLATION | trapped, `EIP += 2`, resumed |
| `out dx, al` | `0xC0000096` PRIVILEGED_INSTRUCTION | trapped, port `0x3C8` / value `0x2A` captured |
| `in al, dx` | `0xC0000096` | trapped, `AL` written back |
| `cli` / `sti` | `0xC0000096` | trapped, skipped |
| `mov es, <selector>` | `0xC0000005` | trapped and skippable — **but must not be emulated per-access** |

The decisive one: the payload returned `EAX = 0x11220009`, meaning a value
injected by the handler during `int 0x21` survived resumption and was then
partially overwritten by a value injected during `in al, dx`. **Servicing a DOS
call by writing results back into `CONTEXT` works.**

Note that `int` and `mov Sreg` raise `ACCESS_VIOLATION`, not
`PRIVILEGED_INSTRUCTION` — a handler must dispatch on the opcode at `EIP`, not
on the exception code.

## Milestones

1. Loader maps the image, jumps to entry, VEH logs every fault. It will die
   almost immediately — the log *is* the real, exhaustive to-do list, replacing
   all guesswork from static analysis.
2. Service `int 21h` file I/O against the `SF/` asset directory → game reaches
   asset loading.
3. Replace the VBE dispatcher (`0x39c8c` / `0x38800`) and the bank-switch
   helpers (`0xa8270`–`0xa8340`) with C; point `[0x15ef88]` at your own buffer
   and add the DAC palette → picture on screen. **Days, not weeks** — the
   surface is two dispatchers, four wrappers and about eight globals.
4. Keyboard/timer ISRs driven from an SDL event loop → input and game clock.
   The timer hook is already located: `int 2Fh AX=1684` then `int 31h AX=0200`
   and `int 21h AH=25` at `0xa91ac`–`0xa921d`, installing a handler at
   `0xa8ed2` on vector 8.
5. Sound last; stub the ports to silence until everything else runs.

## Incrementally replacing functions with C/C++

You need disassembly **per function, on demand** — never a global one. The
distinction from a full decompile: you are not building a source tree that must
compile, you are reading one function to write a fresh replacement behind a
hook. Everything else stays untouched machine code.

### The boundary needs no disassembly at all

Because the image is at fixed addresses, C code binds to the game by literal
address. No linker work, no symbol export, no build integration:

```c
#define g_vbe_pm_bank  (*(void**)0x0015EF4C)   /* global in obj2 */
#define g_win_gran_sh  (*(uint8_t*)0x0015EF58)

/* call an original function you haven't replaced yet */
static int (*orig_set_mode)(int) = (int(*)(int))0x00038BD6;
```

`relocs.json` is a free pointer map here: the 14663 fixup slots inside obj2 are
exactly the globals and struct fields that hold pointers. That is a large head
start on reconstructing types without decompiling anything.

### The one hard constraint: Watcom register calling convention

Measured over 19938 direct calls:

- only **902 (4.5%)** are preceded by a `push`; 9945 are preceded by `mov` and
  3399 by `lea` — arguments go in registers
- **no `add esp, N` after calls**, and 441 `ret N` forms — the **callee** cleans
  stack arguments
- the most common prologue is **`push ebx` (767 functions)** — EBX is both the
  third argument register *and* callee-saved

Verified against `0x14670`: reads EAX and EDX before writing them, returns EAX,
preserves EBX/ECX.

```
args      EAX, EDX, EBX, ECX, then stack (right to left)
floats    passed on the stack (prologues do `fld qword [esp+4]`)
return    EAX, or ST(0) for floating point
callee-saved   EBX, ESI, EDI, EBP
caller-saved   EAX, EDX, ECX
stack args     removed by the callee (`ret N`)
```

**Neither MSVC nor GCC can emit this.** GCC's `regparm(3)` uses EAX/EDX/**ECX**
— wrong register for argument 3. Do not try to approximate it. Two real
options:

1. **Thunks (recommended).** A ~6-instruction assembly shim per replaced
   function moves EAX/EDX/EBX/ECX onto the stack and calls a normal `cdecl` C
   function. Mechanical, generatable from a signature, and keeps you on
   MSVC/clang with modern C++.
2. **Open Watcom V2** targeting Win32 still exists and supports `__watcall`
   natively, so replacements are call-compatible with zero thunks — at the cost
   of tying the toolchain to Watcom.

### What you actually disassemble, per function

1. **Extent** — start address to its `ret`. Mechanical.
2. **Arity** — which of EAX/EDX/EBX/ECX are read before being written. Also
   mechanical, and scriptable across all 1621 known functions at once.
3. **Globals touched** — absolute displacements into obj2.

Reading the body is where a decompiler earns its keep. Load `obj1.bin` at
`0x10000` and `obj2.bin` at `0x140000` in Ghidra and set the Watcom register
convention, then decompile **one function at a time** as a reading aid. That is
a viewer, not a build dependency — it does not commit you to a decompiled
source tree.

### Verification

The hook is also the test harness. Keep the original function in place and have
the shim call both the original and the replacement, then compare return values
and touched memory. Toggle per function at runtime. This is what makes
incremental porting actually converge instead of drifting.

### Where to start

Prefer **whole modules with a narrow surface** over scattered leaf functions.
The VESA driver at `0x38900`–`0x39000` is the ideal first target: it is already
a self-contained module, its surface is documented VBE calls rather than game
logic, and it has to be replaced anyway.

## Tooling in this folder

| file | purpose |
|---|---|
| `le_extract.py` | parses the LE, applies all 20164 fixups, writes `obj1.bin`, `obj2.bin`, `relocs.json` |
| `scan.py` | recursive-descent disassembly, inventories ints / port I/O / privileged / segment ops → `scan.json` |
| `ints.py` | recovers the `AH` function number at every interrupt site |
| `dump.py` | `python dump.py 0xa8270:60` — disassembly around an address |
| `segscan.py` | separates real `0x26` segment overrides from cosmetic `es:` on string ops → `segsites.json` |
| `vehtest/` | the feasibility test above; `cmake -S vehtest -B vehtest/build -A Win32 && cmake --build vehtest/build --config Release` |

`obj1.bin` + `obj2.bin` are already exactly what must be mapped at `0x10000`
and `0x140000`.
