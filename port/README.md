# fightport — FIGHT.EXE running on Win32 with SDL video

A static port of the DOS/4GW `FIGHT.EXE` (Schleichfahrt / Archimedean Dynasty).
The original machine code is loaded and executed unmodified; everything it asks
of DOS, DPMI, the VESA BIOS and the hardware ports is serviced from a vectored
exception handler. Video goes to an SDL2 window.

## Build

Requires MSVC (x86) and the SDL2 development package. **Must be Win32** — the
game is 32-bit machine code executed in-process.

```bash
cmake -S port -B port/build -A Win32 -DSDL2_ROOT=D:/dev/libs/SDL2-2.0.12
```

```bash
cmake --build port/build --config Release
```

## Run

```bash
port\build\Release\fightport.exe --assets D:\Schleichfahrt\Schleichfahrt\SF
```

| option | meaning |
|---|---|
| `--exe FILE` | the LE executable (default `FIGHT.EXE`, resolved after `--assets`) |
| `--assets DIR` | asset directory; becomes the working directory |
| `--args "solo"` | PSP command tail. `FIGHT.EXE` refuses to run unless `argv[1]` is `solo` — that is how `SF.EXE` launches it |
| `--patch A=HEX` | overwrite bytes at an original link-time address, e.g. `--patch A8F1B=EB37`; repeatable. Nothing beyond the automatic patches below is needed to run |
| `--watch A` | poll a global at an original address and log every change |
| `--trace` | log every DOS/DPMI call |

Everything is written to `fightport.log` next to the executable. `F12` or
closing the window quits.

## How it works

**Loading.** `leimage.c` parses the LE, copies both objects into one
`VirtualAlloc` block and applies all 20164 fixups with a rebase delta. The
original bases (`0x10000` / `0x140000`) are unusable under Win32 — the CRT heap
and thread stacks already own that range before any user code runs — so the OS
picks the address and the relocation table absorbs the difference.

**Execution.** `emu.c` installs a vectored exception handler and jumps to the
entry point. Software interrupts, port I/O, `cli`/`sti`, foreign segment
selectors and low-memory accesses all arrive as exceptions; the handler decodes
the instruction at `EIP`, services it against the `CONTEXT` registers, advances
`EIP` and resumes.

Three things turned out to matter more than expected:

- **Stale shadow selectors.** Only *failing* segment loads reach the handler:
  `mov es, <valid flat selector>` succeeds silently, so a shadow set earlier
  would go stale. While any shadow selector is foreign the emulator runs with
  the trap flag set and reconciles on each single step. `mov r16, Sreg` and
  `push Sreg` are patched up the same way, since the game would otherwise read
  Win32's real selector where it expected its own.
- **Low memory.** DOS/4GW's flat selector is zero-based, so the game reads the
  BIOS data area through plain flat pointers. Any faulting access below 1 MB is
  re-aimed at `g_dosmem`.
- **Demand paging.** The Watcom heap grows past the break we advertise, so the
  region is reserved generously and pages are committed on first touch. Buffers
  handed to `ReadFile`/`WriteFile` must be committed up front, because the
  kernel probes them instead of faulting. The heap grows through
  `int 21h AH=4A`, which is validated against the reserved region rather than
  blindly accepted — agreeing to a top we cannot map would hand the game
  memory that fails much later and somewhere unrelated.
- **Descriptor bases are linear, not resolved pointers.** When the game bases a
  descriptor at linear 0 it means the flat selector: its offsets are already
  linear addresses in the game's world and must be translated *per access*.
  Resolving the base once and adding the offset worked for the low-memory
  cases and then sent the heap allocator — which switches `DS` — clean off the
  end of the low-memory block.
- **The low megabyte is readable, on purpose.** DOS/4GW's flat selector made
  linear 0 a valid read, and the game relies on it: at `0x69BC8` an index of
  -1 sets a pointer to 0 and the code dereferences it anyway, discarding the
  value. Any access violation therefore enables the low-memory redirect and
  lets the decoder's effective address decide, rather than trusting the
  faulting address the OS reports — which for these faults comes back as
  `0xFFFFFFFF`.

**Video.** `video.c` implements VBE (`4F00`–`4F08`) over SDL2. The game is
offered the full standard mode set and picks **640x480x16 with the linear
framebuffer bit**, then maps the framebuffer through DPMI "map physical to
linear" — which hands it a pointer into our own buffer. From that point it
paints directly and makes no further VBE calls, so SDL runs on its own thread:
it owns the window, renderer and event queue while the game thread only writes
pixels.

Pointers inside a `VbeInfoBlock` are real-mode far pointers, not flat
addresses, so the mode list and OEM string live in the low megabyte that
`g_dosmem` models.

**Timer (IRQ0).** The game installs a handler on vector 8 and programs the PIT
to **200 Hz**. Nothing in the emulator can drive that on its own: once the game
is painting into the linear framebuffer it stops faulting entirely, so there is
no moment where we naturally regain control.

The tick is therefore delivered the way the hardware would have. A separate
thread suspends the game thread, pushes a real interrupt frame
(`EIP`, `CS`, `EFLAGS`) onto its stack, points `EIP` at the handler and resumes
it. The handler ends in `IRETD`, which pops the frame and returns to whatever
was interrupted — `IRETD` returning to the same privilege level is legal in
user mode, so nothing needs emulating on the way back. The rate is read from
the divisor the game itself stores at `0x15803E`, so changing the PIT rate at
runtime is honoured.

Four things make the difference between this working and corrupting the game:

- **Deadline accumulation.** Advancing the deadline by exactly one period —
  rather than resetting it to "now + period" — keeps the *average* rate
  correct. Resetting folds each wake-up's scheduling latency into the interval
  and runs the game's clock permanently slow (169 Hz instead of 200).
- **`EFLAGS` in the frame must be untouched.** The emulator uses the trap flag
  to track foreign segment selectors across short instruction runs. Clearing
  `TF` in the *saved* frame silently ends a single-step window, after which the
  shadow selector is stale and the next translated access lands on the wrong
  memory. The frame keeps the original flags; only the handler's own execution
  runs with `TF` cleared.
- **Do not inject into unsafe windows.** Ticks are skipped while a single-step
  window is open, while `EIP` is outside the game's code (the emulator, SDL or
  the kernel), and while `EIP` is inside the ISR itself — real hardware masks
  IRQ0 until the handler acknowledges it, and the handler's `mov ss` / `mov
  esp` pair is a window where the stack pointer is inconsistent.
- **The ISR needs a real stack** — see below.

Measured: 200.0 Hz delivered, 0 skipped, sustained.

**Keyboard (IRQ1).** Same shape as the timer, and same reason: the game
installs an int 9 handler at `0x2BC22` and polls nothing. The SDL thread
translates each key event into PC set-1 scancodes and queues them; the timer
thread drains that queue by delivering IRQ1 through the same injection path.
The handler reads the scancode from port 0x60 itself, so the emulator only has
to make sure one interrupt is fired per queued byte. Keyboard is serviced on
every pass of the timer thread rather than only on a PIT deadline, so latency
is the ~1 ms sleep rather than one tick.

Three details matter:

- **The one 16-bit selector fixup.** Of the LE's 20165 fixups, 20164 are
  32-bit and one is a *selector* fixup — at obj1 offset `0x1BC2B`, which is
  exactly the `mov ax, imm16` at `0x2BC29` that this ISR loads into `DS` and
  `ES`. Skipping it left the immediate at zero, so the handler loaded a null
  selector and took a #GP on its first memory access. The loader now applies
  it using the host's own `DS`: there is one flat data selector here, and
  every displacement in the ISR is a rebased absolute address.
- **Positional mapping.** The table maps SDL's physical scancodes to set-1
  codes, not characters, so a German keyboard works without any locale
  handling — the game does its own translation through its tables at
  `0x15DD57` / `0x15DE25`.
- **Extended keys.** Arrows, right ctrl/alt, keypad enter and the navigation
  cluster are sent as a `0xE0` prefix followed by the code, because the ISR
  latches that prefix explicitly at `0x2BC48`. Without it the arrow keys would
  arrive as the numeric-keypad codes.

Verified end to end with `--watch`: a keypress moves the game's own key-state
byte at `0x15DFCF + scancode` from `0x00` to `0x03` on make and to `0x02` on
release, which is precisely what the ISR's `or`/`and` pair does.

## Status

**The game is playable.** It loads, shows its mission briefing, starts the
mission, and renders the in-game submarine cockpit — 3D world through the
viewport, HUD, compass, depth gauges, sonar and mission objective — and steers
in response to the arrow keys.

Working: loading and relocation, DOS file I/O, DPMI (descriptors, memory,
real-mode calls, physical mapping), VESA mode set and linear framebuffer, VGA
DAC palette, BIOS tick, IRQ0 at the game's programmed 200 Hz, and IRQ1
keyboard input reaching the game's own key-state table. No command-line
patches are needed.

Not done yet:

- **Sound.** The driver files (`SB16.DIG`) are opened but nothing is
  implemented; ports are stubbed to silence.
- **Mouse** reports "not installed"; the game configures it anyway and those
  calls are no-ops.
- **Playing a mission to completion** has not been done; sessions so far have
  ended in an in-game death, which the game exits cleanly on
  (`int 21h AH=4Ch`, code 2).

## Stability

Measured: two independent eight-minute unattended soaks and a ~7.5-minute
human play session, all at a steady 200.0 Hz with flat memory (~46 MB private,
no growth) and no emulator faults. The play session ended in an in-game death,
which is a clean shutdown rather than a failure — the game restores its
interrupt vectors and exits through `int 21h AH=4Ch`.

Getting there needed five fixes, four of them in the emulator rather than the
game.

**Interrupt delivery raced with the kernel.** This was the big one. Delivering
an interrupt by suspending the game thread and rewriting its context does not
work in a process that faults thousands of times a second: the kernel often
already has an exception pending for that thread, and when it delivers it
afterwards, the exception *record* describes the original fault while the
*context* carries our redirected EIP. The signature was an access violation
"reading address 0" reported at the first instruction of the keyboard ISR —
address 0 being the game's own deliberate null read at `0x69BF8`.

The shipped arrangement is a hybrid, because neither half is sufficient alone:

- **In-handler delivery** is the normal path. The interrupt frame is pushed
  onto the context the handler is about to hand back, so the frame and the new
  EIP are applied atomically by the same `NtContinue` that resumes the game.
  No race is possible. This is safe because the exception frame sits ~2.7 KB
  *below* the interrupted ESP (measured, not assumed — and checked at runtime),
  leaving the 12 bytes at `ESP-12` free.
- **External redirect** covers what the first path cannot: the game has loops
  that take no exceptions at all, and waiting for a tick that in-handler
  delivery can never produce deadlocks it. It is used only after the game has
  gone 3 ms without faulting — which is precisely the state in which no
  exception can be pending, so the race it is prone to cannot occur.
- **Stale exceptions are discarded** as a backstop: an exception reported
  exactly at an installed ISR entry is one the kernel had queued when we
  redirected. Dropping it is correct, not merely convenient — the interrupted
  address is on the ISR's stack frame, so the instruction re-runs after
  `IRETD` and faults again where it can be serviced.

Arming the trap flag externally instead of redirecting EIP was tried, on the
theory that only code owning the context should touch EIP/ESP. It is prettier
and it measured worse — delivery fell to ~75 Hz and it crashed inside half a
minute — so the redirect stayed. The rejected approach is recorded in `emu.c`
next to the code it lost to.

**The handler could fault, and then cascade.** A vectored handler is re-entered
for exceptions raised inside it, so one bad access recursed until the frame was
destroyed and the crash report was garbage (`ep->ContextRecord` reading back as
`0x23`). There is now a per-thread re-entrancy guard, and the `__except` block
reports from globals recorded as the handler runs rather than trusting `ep`.

**The handler serviced other people's exceptions.** A vectored handler is
process-wide: it was seeing faults from SDL's renderer and the CRT and trying
to emulate them. It is now confined to faults whose EIP lies inside the game
image, with demand-commit the deliberate exception (that one is keyed on the
faulting address).

**Wild pointers were dereferenced inside the handler.** Enabling the
low-memory redirect for every access violation meant a genuinely bad pointer
got `memcpy`'d to from within the handler. Addresses outside the low megabyte
are now checked for being committed before the emulator touches them, so a
wild access is reported instead of taking the emulator with it.

**The heap could be over-promised.** `int 21h AH=4Ah` is how the Watcom heap
grows; accepting a top beyond our mapping would have handed the game memory
that fails much later and somewhere unrelated. It is validated against the
reserved region.

### Resolved: the `exit(101)` init check

For a long stretch this port needed `--patch 1021F=EB23` to get past a test in
`main` that called `exit(101)`. **It no longer does** — the underlying bug is
fixed and the patch has been removed.

The chain, for the record, because none of it was visible from the check
itself:

1. `main` reads a global at `0x15E6C8` and exits if it is zero. That address
   has exactly **one** reference in the entire image — the read — so nothing
   appeared to write it.
2. It is not a standalone global: it is a member at `+0x304` of the video
   singleton at `0x15E3C4`, written by `mov [esi+0x304], eax` at `0x20A4C`.
3. That store is in the video system's static initialiser at `0x20670`, which
   is entry number six in Watcom's init table at `0x15DBB0` (6-byte entries of
   `flag, priority, pointer`; this one has priority `0x27`). The CRT walks the
   table from `0x9AAFC` in priority order with a threshold of `0xFF`, so it
   does run.
4. The value stored is whatever `0x38918` returns, and that function's tail is
   `mov eax, [ecx+2]; sar eax, 0x10` — **it returns the VBE version from the
   4F00 info block**. So the check is really "did VBE 4F00 succeed".
5. 4F00 succeeded on our side, but its result never reached the game. The info
   block travels through the DOS transfer buffer obtained from DPMI `0x0100`,
   which returns *both* a real-mode segment and a selector for the same bytes.
   We returned segment `0x1000` (linear `0x10000`) alongside the zero-based
   low-memory selector, so the copy-back read from linear `0` instead. The
   game got a zeroed block, `0x38918` returned 0, and `main` exited 101.

The fix was giving that buffer its own based selector (`SEL_DOSBUF`), which is
also what made VBE mode enumeration work — the two symptoms had one cause.
Verified with `--watch 15e6c8`: it now goes `0x00000000 -> 0x00000200`, the
VBE version, and the game reaches its briefing with no patches beyond the four
automatic ones below.

### Patches that *are* understood, applied automatically

These are consequences of the host environment differing from DOS/4GW in ways
the original code cannot know about. They live in `add_default_patches()` and
`relocate_isr_stack()` in `main.c`, each with its reasoning next to it.

| where | what | why |
|---|---|---|
| `0x124551` | probe → `ret` | Watcom's stack-overflow probe tests `ESP` against bounds the DOS/4GW loader established. The game now runs on the host thread's 16 MB stack, so those bounds describe nothing and the probe fires spuriously. |
| `0xA8F1B` | → `jmp 0xA8F54` | The ISR calls out through a DPMI "device API" vector when either half is non-zero. We never provide one, and the call is a *far* call, impossible in a flat Win32 process. The segment half cannot be zeroed from the emulator: the game reads it back with `mov [mem], es`, which never faults, and Windows discards segment-register changes made to a `CONTEXT` from a vectored handler. |
| `0xA9008` | → `jmp 0xA9051` | Periodically the ISR chains to the original BIOS int 8 handler by rewriting its own return frame and `retf`-ing into the old vector. There is no BIOS handler here, and the saved "selector" is Win32's data selector, which cannot be loaded into `CS`. The BIOS tick at 40:6C is maintained from wall time instead. |
| `0xA8F17` | ISR stack → obj2 tail | The ISR switches to a private stack with ~2.5 KB of headroom. That was ample on real hardware, but here every `out`/`cli`/`sti` inside the ISR faults into the emulator, and Windows delivers those exceptions on the current stack — so the handler and its logging ran inside that 2.5 KB and overwrote the globals below it, including the ISR's own data selector at `0x15804E`. It then loaded a null `DS` and took a #GP. obj2's tail is the original DOS/4GW stack region, unused by this port, so the ISR gets ~120 KB there. Applied in code rather than as a byte patch because the immediate is a relocated address. |
