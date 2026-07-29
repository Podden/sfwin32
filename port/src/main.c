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

static void usage(const char *argv0)
{
    printf("usage: %s [--exe FIGHT.EXE] [--assets DIR] [--args \"solo\"]\n"
           "          [--patch ADDR=HEXBYTES] [--watch ADDR] [--trace]\n"
           "\n"
           "  --patch  overwrite bytes at an original (link-time) address,\n"
           "           e.g. --patch 1021F=EB23 . This is the hook point for\n"
           "           changing behaviour without touching the rest of the\n"
           "           image; may be given more than once.\n", argv0);
}

/* --patch <hexaddr>=<hexbytes> : applied after relocation, before the code is
 * made executable. */
#define MAX_PATCH 32
static struct { uint32_t addr; uint8_t b[32]; int n; } g_patch[MAX_PATCH];
static int g_patches;

static int parse_patch(const char *s)
{
    const char *eq = strchr(s, '=');
    uint32_t addr;
    int n = 0;

    if (!eq || g_patches >= MAX_PATCH) return 0;
    addr = (uint32_t)strtoul(s, NULL, 16);
    for (eq++; eq[0] && eq[1] && n < 32; eq += 2) {
        char h[3];
        h[0] = eq[0]; h[1] = eq[1]; h[2] = '\0';
        g_patch[g_patches].b[n++] = (uint8_t)strtoul(h, NULL, 16);
    }
    if (!n) return 0;
    g_patch[g_patches].addr = addr;
    g_patch[g_patches].n = n;
    g_patches++;
    return 1;
}

/* Patches the port always needs, because the host environment differs from
 * DOS/4GW in a way the original code cannot know about. Kept separate from
 * user --patch options so the reasoning stays visible. */
static void add_default_patches(void)
{
    /* Watcom's stack-overflow probe at 0x124551 tests ESP against bounds the
     * DOS/4GW loader established. We run the game on the host thread's 16 MB
     * stack instead, so those bounds describe nothing and the probe reports a
     * spurious "Stack Overflow!". Turn the probe into an immediate return. */
    parse_patch("124551=C3");

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
    parse_patch("A8F1B=EB37");

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
    parse_patch("A9008=EB47");
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
    uint32_t *imm = (uint32_t *)gp(0x000A8F17);      /* mov esp, imm32 */
    uint32_t old = *imm;
    *imm = (uint32_t)(uintptr_t)gp(0x00176400);
    LOGI("ISR stack relocated: 0x%08X -> 0x%08X (orig 0x158A60 -> 0x176400)",
         old, *imm);
}

static void apply_patches(void)
{
    int i, j;
    for (i = 0; i < g_patches; i++) {
        uint8_t *p = (uint8_t *)gp(g_patch[i].addr);
        char before[80], after[80];
        int o = 0;
        for (j = 0; j < g_patch[i].n; j++) o += sprintf(before + o, "%02X", p[j]);
        memcpy(p, g_patch[i].b, g_patch[i].n);
        o = 0;
        for (j = 0; j < g_patch[i].n; j++) o += sprintf(after + o, "%02X", p[j]);
        LOGI("patched orig 0x%08X: %s -> %s", g_patch[i].addr, before, after);
    }
}

int main(int argc, char **argv)
{
    const char *exe = "FIGHT.EXE";
    const char *assets = NULL;
    DWORD old;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc)         exe = argv[++i];
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
        else if (!strcmp(argv[i], "--trace"))  g_log_level = LOG_TRACE;
        else { usage(argv[0]); return 2; }
    }

    log_init("fightport.log");
    LOGI("FIGHT.EXE static port starting");

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
