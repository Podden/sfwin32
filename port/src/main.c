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

const char *g_cmdline = "solo";

/* Schleichfahrt ships as two DOS/4GW binaries that SF.EXE chains between:
 * FIGHT.EXE is the combat module, DESKTOP.EXE the station -- story, dialogue
 * and trading. They are the same format, compiler and middleware, so the port
 * loads both; what differs is the patch set, since every automatic patch is an
 * address in one specific image. The mode selects that set explicitly rather
 * than having the port guess. */
typedef enum { MODE_FIGHT, MODE_DESKTOP } game_mode;
static game_mode g_mode = MODE_FIGHT;
static const char *mode_name(void)
{
    return g_mode == MODE_DESKTOP ? "desktop" : "fight";
}

static void usage(const char *argv0)
{
    printf("usage: %s [--mode fight|desktop] [--exe IMAGE] [--assets DIR]\n"
           "          [--args \"solo\"] [--patch ADDR=HEXBYTES] [--watch ADDR]\n"
           "          [--dumpkeys] [--trace]\n"
           "\n"
           "  --mode   which of the game's two DOS/4GW binaries to run.\n"
           "           fight = the combat module (default, FIGHT.EXE),\n"
           "           desktop = the station (DESKTOP.EXE). Selects the\n"
           "           automatic patch set, which is per-image.\n"
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
    uint32_t site   = (g_mode == MODE_DESKTOP) ? 0x000914D6u : 0x000A8F16u;
    uint32_t was    = (g_mode == MODE_DESKTOP) ? 0x000FA708u : 0x00158A60u;
    uint32_t moveto = (g_mode == MODE_DESKTOP) ? 0x0011F000u : 0x00176400u;

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

int main(int argc, char **argv)
{
    const char *exe = NULL;
    const char *assets = NULL;
    DWORD old;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "fight"))        g_mode = MODE_FIGHT;
            else if (!strcmp(m, "desktop")) g_mode = MODE_DESKTOP;
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
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc)
            g_watch_addr = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--dumpkeys")) g_dump_keys = 1;
        else if (!strcmp(argv[i], "--no-extern-irq")) g_extern_irq = 0;
        else if (!strcmp(argv[i], "--trace"))  g_log_level = LOG_TRACE;
        else { usage(argv[0]); return 2; }
    }

    /* The mode picks the binary unless one was named explicitly. */
    if (!exe) exe = (g_mode == MODE_DESKTOP) ? "DESKTOP.EXE" : "FIGHT.EXE";

    /* One log per mode: running both binaries in turn into a single file
     * makes the two runs indistinguishable after the fact. */
    log_init(g_mode == MODE_DESKTOP ? "desktop.log" : "fight.log");
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

    if (!video_init()) { printf("video init failed; see fightport.log\n");
                         return 1; }

    if (!le_load(exe, &g_img)) {
        printf("failed to load '%s'; see fightport.log\n", exe);
        return 1;
    }

    add_default_patches();
    apply_patches();
    relocate_isr_stack();

    /* Replaces the Miles driver stack with our own mixer. Must come after
     * relocation, since the hooks are written into the loaded image. */
    audio_init();
    audio_select_image(g_mode == MODE_DESKTOP);
    audio_install_hooks();

    /* Object 1 is code. It needs to stay writable as well as executable: the
     * startup patches selector values into its own instruction stream. */
    if (!VirtualProtect(g_img.mem, g_img.obj1_size,
                        PAGE_EXECUTE_READWRITE, &old)) {
        LOGE("VirtualProtect(code) failed: %lu", GetLastError());
        return 1;
    }
    FlushInstructionCache(GetCurrentProcess(), g_img.mem, g_img.obj1_size);

    if (!emu_install()) { printf("emulator install failed\n"); return 1; }

    /* Started before control transfers: it idles until the game installs its
     * own handler on vector 8. */
    timer_start();

    LOGI("---- transferring control to the game ----");
    emu_enter_game();

    /* emu_enter_game does not return; the game leaves via int 21h AH=4Ch. */
    return 0;
}
