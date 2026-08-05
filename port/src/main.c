/* FIGHT.EXE static port -- entry point.
 *
 * Loads the original DOS/4GW image into this process, rebases it, installs the
 * emulation layer, and transfers control. The game's own machine code then
 * runs unmodified; everything it asks of DOS, DPMI, the VESA BIOS and the
 * hardware ports is serviced from the vectored exception handler.
 */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NULL until resolved: FIGHT.EXE and DESKTOP.EXE require different argv[1]
 * values (see the mode-aware default below), so there is no single constant
 * that is right unconditionally -- only "solo" happened to look like one
 * because FIGHT was the first and, for a while, only mode this port ran. */
const char *g_cmdline;

/* Schleichfahrt ships as two DOS/4GW binaries that SF.EXE chains between:
 * FIGHT.EXE is the combat module, DESKTOP.EXE the station -- story, dialogue
 * and trading. They are the same format, compiler and middleware, so the port
 * loads both; what differs is the patch set, since every automatic patch is an
 * address in one specific image. The mode selects that set explicitly rather
 * than having the port guess. */
typedef enum { MODE_FIGHT, MODE_DESKTOP, MODE_FIGHT3 } game_mode;
static game_mode g_mode = MODE_FIGHT;
static const char *mode_name(void)
{
    return g_mode == MODE_DESKTOP ? "desktop"
         : g_mode == MODE_FIGHT3  ? "fight3" : "fight";
}

static void usage(const char *argv0)
{
    printf("usage: %s [--chain] [--mode fight|desktop] [--exe IMAGE]\n"
           "          [--assets DIR] [--args ARGV1] [--patch ADDR=HEXBYTES]\n"
           "          [--watch ADDR] [--dumpkeys] [--calltrace] [--trace]\n"
           "\n"
           "  --chain  play through both halves the way SF.EXE originally\n"
           "           did: run desktop, and when it exits with the code\n"
           "           that means a mission was chosen, run fight; when\n"
           "           fight exits, go back to desktop. All other options\n"
           "           are passed through to each child (--mode is ours).\n"
           "\n"
           "  --mode   which of the game's two DOS/4GW binaries to run.\n"
           "           fight = the combat module (default, FIGHT.EXE),\n"
           "           desktop = the station (DESKTOP.EXE). Selects the\n"
           "           automatic patch set, which is per-image.\n"
           "\n"
           "  --calltrace  log every call into every identified Miles AIL_*\n"
           "           entry point (not just the 16 audio.c replaces) to\n"
           "           calltrace.csv, without changing behaviour. For\n"
           "           finding which Miles function a given code path uses.\n"
           "\n"
           "  --args   the command tail passed through the PSP. Defaults to\n"
           "           \"solo\" for fight, \"cold\" for desktop -- each\n"
           "           image checks argv[1] against its own fixed set and\n"
           "           refuses to run otherwise (desktop also accepts warm,\n"
           "           ice, autosave).\n"
           "\n"
           "  --patch  overwrite bytes at an original (link-time) address,\n"
           "           e.g. --patch 1021F=EB23 . This is the hook point for\n"
           "           changing behaviour without touching the rest of the\n"
           "           image; may be given more than once.\n", argv0);
}

/* --patch <hexaddr>=<hexbytes> : applied after relocation, before the code is
 * made executable. */
#define MAX_PATCH 32
static struct {
    uint32_t addr;
    uint8_t  b[32];
    int      n;
    uint8_t  expect[32];        /* original bytes this patch assumes */
    int      nexpect;           /* 0 = apply unconditionally         */
} g_patch[MAX_PATCH];
static int g_patches;

static int hexbytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    for (; s[0] && s[1] && n < max; s += 2) {
        char h[3];
        h[0] = s[0]; h[1] = s[1]; h[2] = '\0';
        out[n++] = (uint8_t)strtoul(h, NULL, 16);
    }
    return n;
}

static int parse_patch(const char *s)
{
    const char *eq = strchr(s, '=');
    if (!eq || g_patches >= MAX_PATCH) return 0;
    g_patch[g_patches].addr = (uint32_t)strtoul(s, NULL, 16);
    g_patch[g_patches].n = hexbytes(eq + 1, g_patch[g_patches].b, 32);
    g_patch[g_patches].nexpect = 0;
    if (!g_patch[g_patches].n) return 0;
    g_patches++;
    return 1;
}

/* Same, but the patch only applies if the bytes already there are the ones it
 * was derived from. Every automatic patch is a FIGHT.EXE address, and the port
 * now loads more than one binary -- writing those offsets blind into
 * DESKTOP.EXE would corrupt whatever happens to live there. A patch that
 * states its assumption can be checked instead of trusted. */
static int parse_patch_if(const char *s, const char *expect)
{
    if (!parse_patch(s)) return 0;
    g_patch[g_patches - 1].nexpect =
        hexbytes(expect, g_patch[g_patches - 1].expect, 32);
    return 1;
}

/* Patches the port always needs, because the host environment differs from
 * DOS/4GW in a way the original code cannot know about. Kept separate from
 * user --patch options so the reasoning stays visible. */
static void add_default_patches(void)
{
    if (g_mode == MODE_FIGHT3) {
        /* The 3dfx build is the same code through the same compiler, so
         * it needs the same three fixes at its own addresses. The stack
         * probe was located by the byte signature of FIGHT's -- `cmp
         * eax,esp / jae / sub / neg / cmp` -- which reproduces both
         * software builds' known sites exactly, so it is trusted here.
         * The two ISR sites were read out of the int 8 handler the game
         * actually installed (orig 0x9E382), which is where an unpatched
         * run faults: the far call at 0x9E3E3. Both skip displacements
         * come out at 0x37 and 0x47, the same as the software builds. */
        parse_patch_if("FCAF3=C3", "39");
        parse_patch_if("9E3CB=EB37", "833D");
        parse_patch_if("9E4B8=EB47", "833D");
        return;
    }
    if (g_mode == MODE_DESKTOP) {
        /* The same four fixes, at DESKTOP.EXE's own addresses. Each was
         * located by isrfind.py (byte signature) or by the "Stack Overflow!"
         * string, not by eye -- and the same tooling reproduces FIGHT's three
         * hand-derived sites exactly, which is why these are trusted.
         *
         * Watcom stack probe. The call at 0xD4816 names it outright, and it
         * opens with the same `cmp eax, esp` as FIGHT's. */
        parse_patch_if("D4825=C3", "39");

        /* ISR device-API callout: `cmp dword ptr [0xF9CD4], 0` immediately
         * after the ISR's stack switch, skipping to 0x91514. */
        parse_patch_if("914DB=EB37", "833D");

        /* ISR chaining to the BIOS int 8 handler: `cmp dword ptr [0xF9C80], 0`
         * with `je 0x91611` two bytes on -- the same 0x47 displacement as
         * FIGHT, the code being identical. */
        parse_patch_if("915C8=EB47", "833D");
        return;
    }

    /* Watcom's stack-overflow probe at 0x124551 tests ESP against bounds the
     * DOS/4GW loader established. We run the game on the host thread's 16 MB
     * stack instead, so those bounds describe nothing and the probe reports a
     * spurious "Stack Overflow!". Turn the probe into an immediate return. */
    parse_patch_if("124551=C3", "39");

    /* The timer ISR calls out through a DPMI "device API" vector whenever
     * either half of it is non-zero (0xA8F1B tests the offset, 0xA8F24 the
     * segment). We never provide such an API, and the call at 0xA8F33 is a
     * *far* call, which cannot work in a flat Win32 process at all.
     *
     * The segment half ends up non-zero regardless: the game reads it back
     * with `mov [mem], es`, which never faults, so the emulator cannot
     * substitute its shadow value -- and Windows discards segment-register
     * changes made to a CONTEXT from a vectored handler, so the real ES
     * cannot be zeroed either. Jump straight to the no-callout path instead.
     * 0xA8F1B -> jmp 0xA8F54. */
    parse_patch_if("A8F1B=EB37", "833D");

    /* Every so often the ISR chains to the *original* BIOS int 8 handler so
     * the BIOS clock keeps ticking: 0xA9008 tests a countdown, and the path
     * at 0xA9011 rewrites its own return frame and `retf`s into the old
     * vector (0xA9050).
     *
     * There is no BIOS handler here -- we are the timer source -- and the
     * far return could not work regardless, since the "selector" the game
     * saved is Win32's data selector, which cannot be loaded into CS. The
     * BIOS tick count at 40:6C is maintained from wall time instead, so
     * nothing is lost by always taking the plain IRETD path.
     * 0xA9008 -> jmp 0xA9051. */
    parse_patch_if("A9008=EB47", "833D");
}

/* The timer ISR switches to a private stack at 0x158A60, which leaves it about
 * 2.5 KB before it runs into the game's own globals. That was ample under
 * DOS/4GW, where the ISR ran to completion on real hardware.
 *
 * Here it is not: every `out`, `cli` and `sti` inside the ISR faults into the
 * emulator, and Windows delivers those exceptions on whatever stack is
 * current. So our handler -- and its logging -- executes inside that 2.5 KB
 * and runs straight through the globals below it. The observed symptom was the
 * ISR's own data-selector global at 0x15804E being overwritten, after which it
 * loaded a null DS and took a #GP on its first memory access.
 *
 * obj2's tail is the original DOS/4GW stack region, which this port never uses
 * (the game runs on the host thread's stack), so the ISR gets pointed at the
 * top of that instead: ~120 KB of room, and nothing else lives there. The
 * instruction's immediate is a relocated address, so this has to happen in
 * code rather than as a byte patch. */
static void relocate_isr_stack(void)
{
    /* Per image: the BC opcode byte, the stack the ISR sets up, and the top of
     * obj2 we move it to. Both were found by isrfind.py's signature for
     * `mov ax,ds / mov ss,ax / mov esp,imm32`, which nothing else in either
     * image does. DESKTOP's obj2 ends at 0x11F110, which is also its LE stack
     * pointer -- confirming the tail really is the unused DOS/4GW stack. */
    uint32_t site, was, moveto;

    if (g_mode == MODE_FIGHT3) {
        /* Same instruction, found the same way, in the ISR at 0x9E382:
         * `mov esp, imm32` at 0x9E3C6. The immediate is a relocated
         * address whose link-time value this port never sees, so unlike
         * the software builds it is checked for being inside the image
         * rather than against a known constant. Object 5 ends at
         * 0x177820, which is also the LE stack pointer -- the DOS/4GW
         * stack this port never uses. */
        uint8_t  *op3  = (uint8_t *)gp(0x0009E3C6u);
        uint32_t *imm3 = (uint32_t *)gp(0x0009E3C7u);
        uint32_t  lo   = (uint32_t)(uintptr_t)g_img.mem;
        if (*op3 != 0xBC || *imm3 < lo || *imm3 >= lo + g_img.size) {
            LOGW("fight3: ISR stack not relocated: 0x9E3C6 is %02X %08X",
                 *op3, *imm3);
            return;
        }
        LOGI("ISR stack relocated: 0x%08X -> 0x%08X (orig 0x%X -> 0x177720)",
             *imm3, (uint32_t)(uintptr_t)gp(0x00177720u), *imm3 - g_img.delta);
        *imm3 = (uint32_t)(uintptr_t)gp(0x00177720u);
        return;
    }
    site   = (g_mode == MODE_DESKTOP) ? 0x000914D6u : 0x000A8F16u;
    was    = (g_mode == MODE_DESKTOP) ? 0x000FA708u : 0x00158A60u;
    moveto = (g_mode == MODE_DESKTOP) ? 0x0011F000u : 0x00176400u;

    uint8_t  *op  = (uint8_t *)gp(site);
    uint32_t *imm = (uint32_t *)gp(site + 1);        /* mov esp, imm32     */
    uint32_t old;

    /* Same reasoning as the byte-checked patches: confirm this really is
     * `mov esp, <the original stack>` before rewriting four bytes of it. */
    if (*op != 0xBC || *imm != (uint32_t)(uintptr_t)gp(was)) {
        LOGW("ISR stack not relocated: 0x%X is %02X %08X, expected BC %08X",
             site, *op, *imm, (uint32_t)(uintptr_t)gp(was));
        return;
    }
    old = *imm;
    *imm = (uint32_t)(uintptr_t)gp(moveto);
    LOGI("ISR stack relocated: 0x%08X -> 0x%08X (orig 0x%X -> 0x%X)",
         old, *imm, was, moveto);
}

static void apply_patches(void)
{
    int i, j;
    for (i = 0; i < g_patches; i++) {
        uint8_t *p = (uint8_t *)gp(g_patch[i].addr);
        char before[80], after[80];
        int o = 0;

        if (g_patch[i].nexpect &&
            memcmp(p, g_patch[i].expect, g_patch[i].nexpect) != 0) {
            for (j = 0; j < g_patch[i].nexpect; j++)
                o += sprintf(before + o, "%02X", p[j]);
            o = 0;
            for (j = 0; j < g_patch[i].nexpect; j++)
                o += sprintf(after + o, "%02X", g_patch[i].expect[j]);
            LOGI("skipping patch at orig 0x%08X: found %s, expected %s "
                 "(not this binary)", g_patch[i].addr, before, after);
            continue;
        }

        for (j = 0; j < g_patch[i].n; j++) o += sprintf(before + o, "%02X", p[j]);
        memcpy(p, g_patch[i].b, g_patch[i].n);
        o = 0;
        for (j = 0; j < g_patch[i].n; j++) o += sprintf(after + o, "%02X", p[j]);
        LOGI("patched orig 0x%08X: %s -> %s", g_patch[i].addr, before, after);
    }
}

/* ---------------------------------------------------------- chain mode */

/* Runs in place of the game: launches this same executable as a child
 * process for each half in turn, watching exit codes the way SF.EXE
 * originally did.
 *
 * SF.EXE itself is not decompiled or reverse engineered for this --
 * DESKTOP.EXE never calls int21 AH=4B (EXEC). Instead it writes the
 * player's mission/loadout selection to dat\save\FIGHTIN.DES and exits with
 * a specific code, and FIGHT.EXE reads that same file at startup regardless
 * of its command line (confirmed by tracing both: DESKTOP writes 772+24
 * bytes to FIGHTIN.DES then calls `int21 AH=4C AL=02`, and a fresh FIGHT.EXE
 * run with nothing but `--args solo` opens that same file immediately).
 * FIGHTOUT.DES is the reverse handoff, written by FIGHT.EXE for DESKTOP to
 * read back.
 *
 * So all real state crosses the process boundary through those files, and
 * the supervisor's own job is small and entirely observable from outside:
 * run desktop, and if it exits with the code that means "a mission was
 * chosen" (2, confirmed), run fight next; when fight exits, go back to
 * desktop. Any other desktop exit code is treated as the player quitting
 * from the menu, ending the chain. FIGHT.EXE's own exit codes have not been
 * catalogued yet, so nothing is gated on them -- the return trip always
 * happens, per the one thing that was asked of this mode explicitly.
 *
 * fightport.exe's own state -- SDL's window/renderer, the audio device, the
 * vectored exception handler, the timer thread -- is not designed to be torn
 * down and rebuilt for a different image within one process, so each half
 * runs as a separate OS process, exactly as the original two binaries did. */

#define CHAIN_MISSION_EXIT_CODE 2

static void append_arg(char *cmd, size_t cap, const char *arg)
{
    size_t len = strlen(cmd);
    int quote = strchr(arg, ' ') != NULL;
    if (len) { strncat(cmd, " ", cap - len - 1); len++; }
    if (quote) strncat(cmd, "\"", cap - strlen(cmd) - 1);
    strncat(cmd, arg, cap - strlen(cmd) - 1);
    if (quote) strncat(cmd, "\"", cap - strlen(cmd) - 1);
}

/* Builds "<self.exe> <passthrough args, minus --chain/--mode> --mode X",
 * runs it to completion, and returns its exit code (-1 on launch failure).
 * Passing through whatever the user gave (assets dir, --trace, and so on)
 * means the chain does not need its own copy of every option main() already
 * understands -- only --chain and --mode are ours to decide here. */
static int run_child(const char *self_path, int argc, char **argv,
                      const char *mode)
{
    char cmd[4096];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    int i;

    cmd[0] = '\0';
    append_arg(cmd, sizeof cmd, self_path);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chain")) continue;
        if (!strcmp(argv[i], "--mode")) { i++; continue; }  /* ours to set */
        append_arg(cmd, sizeof cmd, argv[i]);
    }
    append_arg(cmd, sizeof cmd, "--mode");
    append_arg(cmd, sizeof cmd, mode);

    printf("[chain] %s\n", cmd);

    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("[chain] failed to launch '%s' (%lu)\n", cmd, GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    printf("[chain] mode %s exited with code %lu\n", mode, code);
    return (int)code;
}

static int chain_supervisor(int argc, char **argv)
{
    char self_path[MAX_PATH];
    int code;

    /* Each child runs for minutes at a time, and stdio fully buffers stdout
     * when it is not a console (redirected to a file, piped) -- unbuffered
     * here means the [chain] lines land as they happen instead of appearing
     * all at once whenever a buffer happens to fill or the process exits. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!GetModuleFileNameA(NULL, self_path, sizeof self_path)) {
        printf("[chain] GetModuleFileNameA failed (%lu)\n", GetLastError());
        return 1;
    }

    for (;;) {
        code = run_child(self_path, argc, argv, "desktop");
        if (code != CHAIN_MISSION_EXIT_CODE) {
            printf("[chain] desktop exit code %d is not the mission-selected "
                   "code (%d); treating as quit\n",
                   code, CHAIN_MISSION_EXIT_CODE);
            return 0;
        }
        run_child(self_path, argc, argv, "fight");
        printf("[chain] back to desktop\n");
    }
}

int main(int argc, char **argv)
{
    const char *exe = NULL;
    const char *assets = NULL;
    int want_calltrace = 0;
    DWORD old;
    int i;

    /* Checked before anything else: chain mode doesn't load an image or open
     * a window itself, it only spawns children that do. */
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--chain")) return chain_supervisor(argc, argv);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "fight"))        g_mode = MODE_FIGHT;
            else if (!strcmp(m, "desktop")) g_mode = MODE_DESKTOP;
            else if (!strcmp(m, "fight3"))  g_mode = MODE_FIGHT3;
            else { printf("unknown mode '%s' (fight|desktop)\n", m); return 2; }
        }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc)    exe = argv[++i];
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) g_cmdline = argv[++i];
        else if (!strcmp(argv[i], "--patch") && i + 1 < argc) {
            if (!parse_patch(argv[++i])) { usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "--voices") && i + 1 < argc)
            audio_set_voice_count(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            /* ADDR or ADDR:NDWORDS */
            const char *a = argv[++i];
            const char *colon = strchr(a, ':');
            g_watch_addr = (uint32_t)strtoul(a, NULL, 16);
            g_watch_dwords = colon ? atoi(colon + 1) : 1;
        }
        else if (!strcmp(argv[i], "--pixfmt") && i + 1 < argc)
            g_pixfmt_555 = !strcmp(argv[++i], "555");
        else if (!strcmp(argv[i], "--dumpvram") && i + 1 < argc)
            g_dumpvram_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dumpkeys")) g_dump_keys = 1;
        else if (!strcmp(argv[i], "--autokeys") && i + 1 < argc)
            g_autokeys = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cblit") && i + 1 < argc)
            g_cblit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cflush") && i + 1 < argc)
            g_cflush = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csurf") && i + 1 < argc)
            g_csurf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cdraw") && i + 1 < argc)
            g_cdraw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cdrawscan")) g_cdrawscan = 1;
        else if (!strcmp(argv[i], "--glidedebug") && i + 1 < argc)
            g_glidedebug = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--glidetrace") && i + 1 < argc)
            g_glidetrace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--glide") && i + 1 < argc)
            g_glide = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctex") && i + 1 < argc)
            g_ctex = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--srcwatch")) g_srcwatch = 1;
        else if (!strcmp(argv[i], "--rasterscan")) g_rasterscan = 1;
        else if (!strcmp(argv[i], "--spanprobe")) g_spanprobe = 1;
        else if (!strcmp(argv[i], "--craster") && i + 1 < argc)
            g_craster = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fbwatch") && i + 1 < argc)
            g_fbwatch = (int)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--listwatch") && i + 1 < argc) {
            extern void *g_listwatch_page;
            g_listwatch_page = (void *)(uintptr_t)
                (strtoul(argv[++i], NULL, 16) & ~0xFFFu);
        }
        else if (!strcmp(argv[i], "--calltrace")) want_calltrace = 1;
        else if (!strcmp(argv[i], "--no-extern-irq")) g_extern_irq = 0;
        else if (!strcmp(argv[i], "--trace"))  g_log_level = LOG_TRACE;
        else { usage(argv[0]); return 2; }
    }

    /* The mode picks the binary unless one was named explicitly. */
    if (!exe) exe = g_mode == MODE_DESKTOP ? "DESKTOP.EXE"
                  : g_mode == MODE_FIGHT3  ? "FIGHT3.EXE" : "FIGHT.EXE";

    /* Same for the command tail, unless --args overrode it. FIGHT.EXE wants
     * "solo" (that is how SF.EXE launches it); DESKTOP.EXE checks argv[1]
     * against "cold"/"warm"/"ice"/"autosave" and recognises none of those --
     * "solo" included -- printing "FEHLER: Bitte starten Sie SF.EXE" and
     * exiting. Before this had a mode-aware default, --mode desktop without
     * also remembering --args cold hit that error every time. */
    if (!g_cmdline)
        g_cmdline = (g_mode == MODE_DESKTOP) ? "cold" : "solo";

    /* One log per mode: running both binaries in turn into a single file
     * makes the two runs indistinguishable after the fact. */
    log_init(g_mode == MODE_DESKTOP ? "desktop.log"
           : g_mode == MODE_FIGHT3  ? "fight3.log" : "fight.log");
    LOGI("Schleichfahrt static port starting: mode %s, image %s",
         mode_name(), exe);

    /* The game opens its data with DOS-relative paths, so the asset directory
     * has to be the working directory. */
    if (assets) {
        if (!SetCurrentDirectoryA(assets)) {
            LOGE("cannot chdir to '%s' (%lu)", assets, GetLastError());
            printf("cannot chdir to '%s'\n", assets);
            return 1;
        }
        LOGI("assets: %s", assets);
    }

    /* Before video_init: the SDL thread decides there whether the window
     * gets an OpenGL pixel format, and the 3dfx build needs one. */
    g_desktop_mode = (g_mode == MODE_DESKTOP);
    g_glide_mode   = (g_mode == MODE_FIGHT3);

    if (!video_init()) { printf("video init failed; see fightport.log\n");
                         return 1; }

    if (!le_load(exe, &g_img)) {
        printf("failed to load '%s'; see fightport.log\n", exe);
        return 1;
    }

    add_default_patches();
    apply_patches();
    relocate_isr_stack();

    /* Per-image: the game's own shadow copy of the PIT divisor it last
     * programmed, at a different address in each binary. Must be set before
     * timer_start(). */
    /* Every per-image address below belongs to one of the two software
     * builds. The 3dfx build is a different link with none of them known
     * yet, so it takes the generic path rather than a wrong constant: a
     * zero divisor address means "read no shadow, assume the default". */
    g_pit_divisor_addr = g_glide_mode   ? 0u
                       : g_desktop_mode ? GAME_PIT_DIVISOR_ADDR_DESKTOP
                                        : GAME_PIT_DIVISOR_ADDR_FIGHT;

    /* Ahead of audio_install_hooks() on purpose: for the 16 functions both
     * touch, audio's real replacement should win over calltrace's
     * logging-only detour, which only needs to run second to overwrite it. */
    if (want_calltrace && !g_glide_mode) calltrace_install();
    glide_install();
    cblit_install();

    /* Replaces the Miles driver stack with our own mixer. Must come after
     * relocation, since the hooks are written into the loaded image. */
    audio_init();
    if (!g_glide_mode) {
        audio_select_image(g_mode == MODE_DESKTOP);
        audio_install_hooks();
    }

    /* Object 1 is code. It needs to stay writable as well as executable: the
     * startup patches selector values into its own instruction stream. */
    if (!VirtualProtect(g_img.mem, g_img.code_size,
                        PAGE_EXECUTE_READWRITE, &old)) {
        LOGE("VirtualProtect(code) failed: %lu", GetLastError());
        return 1;
    }
    FlushInstructionCache(GetCurrentProcess(), g_img.mem, g_img.code_size);

    if (!emu_install()) { printf("emulator install failed\n"); return 1; }

    /* Started before control transfers: it idles until the game installs its
     * own handler on vector 8. */
    timer_start();

    LOGI("---- transferring control to the game ----");
    emu_enter_game();

    /* emu_enter_game does not return; the game leaves via int 21h AH=4Ch. */
    return 0;
}
