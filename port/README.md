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
| `--mode fight\|desktop` | which of the game's two DOS/4GW binaries to run. Selects the automatic patch set and the Miles address table, both of which are per-image. Also picks the default `--exe` and the log name |
| `--assets DIR` | asset directory; becomes the working directory |
| `--args "solo"` | PSP command tail. `FIGHT.EXE` refuses to run unless `argv[1]` is `solo` — that is how `SF.EXE` launches it |
| `--patch A=HEX` | overwrite bytes at an original link-time address, e.g. `--patch A8F1B=EB37`; repeatable. Nothing beyond the automatic patches below is needed to run |
| `--voices N` | sound polyphony, 1–16 (default 16; the game itself asks for 8) |
| `--watch A` | poll a global at an original address and log every change |
| `--dumpkeys` | every 2 s, log which scancodes the game believes are held, and the exact game port read count |
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
byte from `0x00` to `0x03` on make and to `0x02` on release, which is precisely
what the ISR's `or`/`and` pair does. (Extended keys do not land at
`0x15DFCF + scancode`; the ISR looks the code up in a 16-entry table at
`0x15DC79` and stores at `0x15DFCF + 0x57 + index`, so LEFT is at `0x15E02F`.)

**Held keys must be released when the window loses focus.** The ISR only clears
a key on its break code, and SDL delivers no key-up for a key that was held
when focus went away — so alt-tabbing mid-turn leaves the game steering in
that direction forever. The SDL thread tracks which keys it has reported down
and sends break codes for all of them on `FOCUS_LOST` or `MINIMIZED`
(deliberately not on `LEAVE`, since the mouse leaving the window is normal
during play). Confirmed by holding LEFT, minimising, and watching the state
byte go `0x03` → `0x02`.

The scancode ring between the two threads is `volatile` with an explicit
barrier between writing the byte and publishing the tail. Without it the
compiler is free to reorder those stores, and a consumer that sees a stale
slot gets half of a two-byte extended sequence — a key that never comes back
up.

**The game port must read as empty, not as zero.** The symptom was the sub
steering left and down constantly, without a single key ever being pressed.
Keyboard, mouse and joystick all looked innocent at first: the key-state array
at `0x15DFCF` was verifiably all-zero, the pointer the game actually indexes
(`[0x15DFCB]`) pointed exactly where it should, `int 33h` reset reports no
mouse, and a counter on port `0x201` seemed to show only three reads.

That last one was a bad measurement, and it cost the most time. The counter
logged on `reads < 4 || reads % 500 == 0`, so "three lines" meant *anywhere
from 3 to 499 reads* — it could not distinguish startup detection from a
per-frame poll. The lesson is the one that has paid off repeatedly here:
instrument the exact quantity, not a sampled proxy.

The bug itself is in how a game port is read. Bits 0-3 are one-shots: software
strobes `0x201`, then polls until each bit falls, and *the number of iterations
is the axis value*. The stub returned `0xF0` — low bits already clear — so
every axis read as zero on the first poll. The game saw a perfectly functional
stick held hard into the bottom-left corner and flew there. With nothing
plugged in the one-shots never fire, the bits stay high, and the poll loop hits
its own timeout; returning `0xFF` makes detection fail, and the game now
strobes the port exactly once at startup and never again.

`--dumpkeys` logs, every two seconds, which scancodes the game believes are
held (both directly and through its indirection pointer) plus the exact game
port read count. That combination is what separated "the input layer is lying"
from "the input layer is fine and something else is steering".

**Mouse.** `mouse.c` implements int 33h over SDL, split the way the audio is:
the SDL thread feeds motion and buttons in, the game thread reads them back.
The position is integrated from *relative* motion and clamped to the range the
game sets, rather than sampled from the host cursor — that is what a real
driver does, and it is the only version where function 0x04 (set position)
means anything. Buttons are released on focus loss for the same reason held
keys are.

`FIGHT.EXE` never calls int 33h — verified, with the vector confirmed routed —
so this exists for `DESKTOP.EXE`, whose station UI is mouse-driven.

**DESKTOP reaches the driver two different ways**, and both have to work. Most
calls go through DPMI `int 31h AX=0300` "simulate real-mode interrupt" with
`0x33` in the call structure — the dispatcher at `0x70B3C` builds a packet
whose first word is the function number. That arm used to answer "no mouse"
from a stub of its own, so `mouse.c` was never consulted and the game exited
102. Function 0x0C arrives as a *direct* `int 33h` instead.

**It never polls function 0x03.** It installs an event handler with function
0x0C and expects to be called back, so without that the pointer never moves at
all. A real driver issues that call from inside its own interrupt, where the
interrupted registers are already saved; we are injecting it into arbitrary
game code, so they have to be preserved here. The handler ends in `retf`, which
supplies the hook: it returns to whatever address we push, so we push one that
cannot execute. The resulting fault is recognised by EIP, the registers are
restored, and the game resumes where it was. No generated code and no second
stack — and delivery reuses the same vetted point as a hardware interrupt.

**The game's coordinate range is not the screen's.** DESKTOP sets 5120 × 3840,
exactly 8× the 640×480 mode — sub-pixel precision. Adding raw screen pixels to
the position therefore moved the pointer about an eighth as far as it should,
which is a mouse that works but feels far too slow. Motion is scaled from the
window into whatever range functions 0x07/0x08 last set, carrying the
remainder rather than truncating it, so slow movement in a large range still
moves instead of rounding to zero.

**The second binary.** Schleichfahrt is two DOS/4GW executables that a 15 KB
`SF.EXE` launcher chains between: `FIGHT.EXE` (combat) and `DESKTOP.EXE`
(station, dialogue, trading — it announces itself as `MASSIVE Desktop V1.120`).
They are the same format, compiler and middleware, so the loader, emulator,
services, video and audio all apply unchanged. What does *not* transfer is
every address, and there are two sets of them.

The patch set is now per-mode and each patch states the bytes it expects,
so a wrong `--mode` is rejected instead of corrupting the image. Finding
DESKTOP's equivalents by eye is how you patch a function that merely starts
with the same opcode — `cmp dword ptr [m32], 0` is `833D`, and there are many
of those — so they are found by signature instead:

- `isrfind.py` locates the timer ISR by `mov ax,ds / mov ss,ax / mov esp,imm32`,
  which nothing else in either image does, and derives the device-API and
  BIOS-chaining sites from it.
- `ailtable.py` names every Miles entry point by the `AIL_` format string that
  function pushes for its own debug logging, inverting `ailverify.py`.

Both were validated by reproducing FIGHT's hand-derived answers exactly —
`isrfind.py` regenerates the identical `A8F1B=EB37`, and `ailtable.py` returns
all 16 addresses including the six that were originally wrong and corrected by
hand. None of FIGHT's 16 Miles addresses are valid in DESKTOP.

**The trap flag must not be re-armed for a resume into our own code.** This is
the bug that made `DESKTOP.EXE` unrunnable, and it had been latent all along.

Windows does not clear TF when it dispatches an exception to user mode: the
handler inherits the interrupted context's flags. Since the emulator runs the
game with TF set whenever a shadow selector is foreign, a fault taken during
that window lands in the handler still in single-step mode. The tail of the
handler then re-armed TF from `shadow_dirty()` regardless of *where* it was
resuming — so after the handler dropped the flag, that line put it straight
back, and execution crawled one instruction per exception through our own code
until a nested trap was declined and the process died with an unhandled
`STATUS_SINGLE_STEP`.

`FIGHT.EXE` survived on luck: faults during a TF window are rare there.
`DESKTOP.EXE` scans its environment block with `ES` foreign and faults on every
byte, so it died within a second, every time. TF is now armed only when the
resumed EIP is inside the game image. The diagnosis came from logging EIP,
EFlags and thread id per exception — the marching consecutive addresses
(`4056A4, A7, A9, AC…`) through our own code are what made it unambiguous,
after two wrong theories drawn from a Release build's approximate
nearest-symbol attribution.

**Where DESKTOP stands.** It loads (16436 fixups, 0 skipped), takes all four
patches, hooks its own 16 Miles entry points, sets 640×480×16 with the linear
framebuffer, plays audio, runs its timer at the 200 Hz it actually asks for,
and responds to the mouse. It wants a start mode as `argv[1]` — `cold`, `warm`,
`ice` or `autosave`, not FIGHT's `solo`.

**The PIT-divisor shadow global is per-image, and reading the wrong one is a
silent bug, not a crash.** `GAME_PIT_DIVISOR_ADDR` was a single hardcoded
constant — FIGHT's address for its own copy of the last-programmed divisor —
used unconditionally regardless of mode. In DESKTOP's address space that same
numeric address is something else entirely, so the timer thread always read a
stray value there rather than DESKTOP's real state, landed on "not
reprogrammed" every time, and ran the timer at the BIOS default 18.2 Hz. There
was no crash and no log warning to point at it: the game looked like it was
just designed to run its station scenes slowly.

`--watch f9ce6` (found the same way as the ISR patch sites: the divisor is
stored to a global at the exact point the command byte and both divisor bytes
are written to ports 0x43/0x40) showed `0x0000 -> 0x174E` shortly after boot —
1193182/0x174E is 200.0 Hz, the identical rate FIGHT programs. DESKTOP was
never slow by design; it was asking for 200 Hz and silently being refused.
`g_pit_divisor_addr` is now set from the resolved mode before the timer thread
starts, alongside `GAME_PIT_DIVISOR_ADDR_FIGHT` / `_DESKTOP`.

Fixing it exposed a second bug the first one had been masking: the mouse
event-handler callback was only delivered on the "no hardware IRQ pending"
leftover slot in `irq_deliver_pending`. That was invisible while IRQ0 rarely
queued a tick (the 18.2 Hz bug above), but once the timer ran at its correct
200 Hz, that slot almost never came up and the pointer stopped moving in all
but the sparsest traffic. Mouse delivery is now tried first, ahead of the
timer and keyboard — one mouse event competing against a queued tick costs at
most a few microseconds of IRQ0 latency, which is never visible, whereas
starving the mouse behind a healthy timer was.

**Auditing for the same mistake elsewhere.** The PIT divisor was one specific
instance of a general failure mode — a constant derived from FIGHT.EXE, used
unconditionally for both images — so once it was found the rest of the
codebase was checked for the same pattern. Two more turned up:

- **The default command tail.** `g_cmdline` defaulted to the string `"solo"`
  at file scope, with no mode awareness at all. FIGHT wants exactly `"solo"`
  (that is how `SF.EXE` launches it); DESKTOP checks `argv[1]` against
  `"cold"`/`"warm"`/`"ice"`/`"autosave"` and recognises none of those —
  `"solo"` included — printing `FEHLER: Bitte starten Sie SF.EXE` and exiting.
  `--mode desktop` without also remembering `--args cold` hit that error
  every time; this is the exact failure this project diagnosed and worked
  around by hand earlier, without ever closing the root cause. `g_cmdline`
  is now `NULL` until resolved, defaulting to `"solo"` or `"cold"` alongside
  the executable name and log file, from the same per-mode block.

- **The ISR re-entrancy spans.** `eip_in_isr()` uses `ISR8_SPAN`/`ISR9_SPAN` —
  byte widths, not addresses — to keep an injected interrupt from landing
  mid-handler, inside the stack-switch window where re-entry would corrupt
  saved registers. These were FIGHT-only measurements (0x1B0, 0x170) applied
  to whichever handler `dos_get_ivec()` reports, DESKTOP's included. Unlike
  the PIT divisor this one had not yet misbehaved: disassembling DESKTOP's
  own int8 and int9 handlers end-to-end measured them at 0x1A3 and 0x167
  bytes, both under FIGHT's spans, so nothing had actually landed
  mid-handler — but by a margin of only 13 and 9 bytes, on constants nothing
  connected to DESKTOP's actual code. A handful more bytes in a future
  rebuild of either binary would have turned this into the same class of
  silent failure, just harder to diagnose (occasional stack corruption
  instead of a wrong-but-consistent timer rate). Split into
  `ISR8_SPAN_FIGHT`/`_DESKTOP` and `ISR9_SPAN_FIGHT`/`_DESKTOP`, selected by
  a new general-purpose `g_desktop_mode` flag set alongside the PIT divisor.

`g_desktop_mode` also gates `--dumpkeys`, which read FIGHT's key-state array
address unconditionally and would have logged whatever unrelated data sits at
that numeric address in DESKTOP as if it were key state. Lower stakes than
the other two — it is an opt-in diagnostic, not something the game's own
correctness depends on — but the same mistake, so fixed the same way.

Everything else that is per-image already was: the Miles address table
(`g_ail_fight`/`g_ail_desktop` in `audio.c`), the voice-pool patch (skipped
outright for DESKTOP, which never needed it), and the four ISR/stack-probe
patch addresses in `main.c` (each checked against the bytes it expects before
applying). `video.c`, `mouse.c` and `leimage.c` hold no FIGHT-specific
addresses at all — they work through APIs (VBE, int 33h, the LE fixup table)
that are inherently image-agnostic, which is why DESKTOP's video, mouse and
loading worked correctly from the first attempt.

Getting there meant finding why it exited 102. `main` is a chain of init steps,
each `call <init>; test eax,eax; jne <next>` with its own exit code; the one
returning 0 was `0x17910`, which succeeds only if the object at `0x1060E8` is
initialised. That object turned out to be the *mouse*: the methods invoked on
it build int 33h packets and hand them to the DPMI dispatcher. Exit 102 meant
"no mouse", and the DPMI stub described above was the reason.

Finding it needed watching a struct rather than a dword, which is why `--watch`
now takes `ADDR:NDWORDS`. Watching `0x1060E8` alone said "never written";
watching `0x1060E8:16` showed fields `+0C/+10/+14` being set — turning "never
initialised" into "initialised but one field left zero", which is what pointed
at a missing *installed* flag rather than a missing init step.

**Sound.** The game drives a Sound Blaster through Miles Sound System 3.50F
and an `AIL3DIG` driver — 16-bit real-mode code that would need an 8086
interpreter plus DMA, DSP and IRQ emulation just to reach the speakers. None
of that is necessary, because the driver is only the bottom of the stack:
`audio.c` hooks the AIL API itself and mixes with SDL, replacing the entire
driver layer with a few hundred lines.

Two properties made that practical:

- **MSS carries its own symbol table, by accident.** It is built with a debug
  facility in which every entry point logs its own name
  (`"AIL_start_sample(0x%X)"`), and each string is referenced from inside the
  function it names. The relocation table therefore yields an exact API map —
  `ailmap.py` recovers 54 entry points the game calls, with no guesswork.
- **Miles is cdecl**, not the Watcom register convention the game itself uses,
  so the hooks are ordinary C functions needing no thunks. `AIL_set_sample_file`
  reading its arguments from `[esp+0x9c…]` and returning with a bare `ret` is
  what shows this.

`AIL_install_DIG_INI` returns 0 for success and fills in a driver handle, so
handing back a token is enough for the game to consider sound available. It
then allocates sample handles in a loop until one comes back NULL, which makes
the voice count ours to choose (16).

**Pairing a string with the nearest function start is a hint, not proof.**
`ailmap.py` does that, and it got **six of sixteen wrong** — including one that
pointed at `AIL_release_sequence_handle`, a MIDI call that was being answered
with a sample count, and another at `AIL_sample_status`, which the game polls
to find finished voices and which was being served by a three-argument no-op
returning garbage. The result was sounds that never appeared to end.

`ailverify.py` settles it properly: every wrapper *pushes its own format
string* before calling the logger, so scanning forward from a function start
for that push identifies it beyond doubt. It checks the table `audio.c`
actually hooks and exits non-zero on any mismatch — worth re-running after any
change there. The corrected map also renamed several entries:
`set_sample_file` is really `AIL_set_sample_address`, `stop_sample` is
`AIL_end_sample`, and what looked like `allocate_file_sample` is
`AIL_release_sample_handle`.

Semantics come from the published MSS header rather than inference:
`SMP_FREE 1`, `SMP_DONE 2` ("finished playing, or has never been started"),
`SMP_PLAYING 4`, and a loop count of **0 means loop forever**. Guessing at
these produced two wrong turns — first a status polarity that made the game
restart playing voices every frame, then silence — before the documentation
settled them.

Two further details were settled from the call sites:

- `AIL_set_sample_file` is used as `AIL_set_sample_address` — the pointer is
  raw PCM with no header of any kind and the third argument is a byte count
  (`sub eax,2; push eax; lea eax,[esi+6]; push eax` at `0xACB5E`).
- The data is **signed** 8-bit mono. A dump of a live buffer reads
  `00 FF FE FD … F8 … FF 00 01 03` — a smooth waveform about zero, not about
  `0x80`. Treating it as unsigned is audible as noise.

Mixing needs one more thing. Each voice can reach full scale by itself, so
summing several overflows, and the game routinely runs eight at once — heard
as crackle and distortion the moment more than one sound plays. Permanently
attenuating everything would make a lone sound quiet, and clipping sounds
broken, so the mixer rides the gain instead: it drops immediately when a buffer
would overflow and eases back afterwards. A single sound plays at full level, a
busy moment is turned down smoothly. Measured with eight voices active, peaks
sit around 17000–25000 of 32767 rather than pinned at the rail.

The setters take the audio lock as well as the obvious ones: rate, volume and
pan are read together per sample, so a half-applied change is audible even
though the individual stores could not tear on x86.

Two behaviours only show up once sound is actually working:

**Setting sample data must reset the loop count.** The game sets an infinite
loop (count 0) on some sounds and then simply does not call
`AIL_set_sample_loop_count` for one-shots, relying on Miles clearing it when
new data is set — visible at `0x23FC0`, where the call is skipped entirely
unless a comparison holds. Sample handles are a reused pool, so without that
reset a weapon firing on a handle that previously held a looping sound loops
forever.

**Polyphony is requested, not offered.** Supplying 16 sample handles achieves
nothing on its own: the pool routine at `0x23DE0(this, count)` asks for
`NumOfVoices` from CONFIG.DES, which ships as 8, and stops there. With eight
voices busy a new sound has nowhere to go and the game steals a playing one,
so firing cuts whatever else was sounding. A trampoline raises the request to
16 — the ceiling the game itself is built for, not a guess: two parallel
per-voice arrays (handles at `+0x11C` stride 4, a second at `+0x160` stride 8)
both end exactly at `+0x1E0`, where the "sound available" flag lives, so 16
fills each precisely and 17 would overwrite it. `--voices N` overrides.

For the record, the sample length is right as the game passes it. The size
helper at `0xACDD0` returns `record[0] >> 8`, counted from offset 4, and the
record carries two header bytes (the Sound Blaster time constant at `+4`)
before PCM at `+6` — so the `size - 2` at the call site is exact.

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

- **Mouse** reports "not installed"; the game configures it anyway and those
  calls are no-ops.
- **Music / Redbook CD audio.** Only digital samples are implemented; the
  sequencer and CD calls are left alone.
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
