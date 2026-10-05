/* --script FILE: drive a run unattended and take screenshots.
 *
 * A script is a list of timed commands executed on the SDL thread, one per
 * line, '#' starts a comment:
 *
 *     wait MS              pause the script
 *     key NAME [HOLD_MS]   press and release an SDL key name ("Return", "Up")
 *     down NAME / up NAME  hold or release a key until told otherwise
 *     text STRING          type characters into DOS console input
 *     mouse X Y            put the game's pointer at screen pixel X,Y
 *     click [left|right]   press and release a mouse button
 *     shot NAME            save the presented frame as SHOTDIR/NAME.bmp
 *     log TEXT             write a marker into the log
 *     quit                 end this process the way the close button does
 *
 * Section headers pick which child of a --chain run executes the lines below
 * them: [desktop], [fight], [3] (third child), [*] (every child). Lines before
 * the first header run in every child. */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#define MAX_LINES 1024

static char    *g_lines[MAX_LINES];
static int      g_nlines, g_pc;
static Uint32   g_wait_until;
static int      g_active;
static char     g_shot_dir[MAX_PATH] = "shots";

/* The renderer that owns the frame takes the shot: render_frame for the
 * software build, gl_buffer_swap for the 3dfx build. */
static volatile LONG g_shot_pending;
static char          g_shot_path[MAX_PATH + 64];
static Uint32        g_shot_deadline;

void script_set_shot_dir(const char *dir)
{
    strncpy(g_shot_dir, dir, sizeof g_shot_dir - 1);
}

static int section_matches(const char *hdr, const char *mode, int index)
{
    char buf[64];
    const char *p = hdr + 1;
    size_t n = strcspn(p, "]");
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, p, n); buf[n] = 0;
    if (!strcmp(buf, "*") || !strcmp(buf, mode)) return 1;
    return index > 0 && atoi(buf) == index && buf[0] >= '0' && buf[0] <= '9';
}

void script_load(const char *path, const char *mode, int index)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int take = 1;

    if (!f) { LOGE("script: cannot open %s", path); return; }
    while (fgets(line, sizeof line, f) && g_nlines < MAX_LINES) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!*s || *s == '#') continue;
        if (*s == '[') { take = section_matches(s, mode, index); continue; }
        if (take) g_lines[g_nlines++] = _strdup(s);
    }
    fclose(f);
    {   /* absolute now: the game chdirs into its asset directory later */
        char full[MAX_PATH];
        if (GetFullPathNameA(g_shot_dir, sizeof full, full, NULL))
            strncpy(g_shot_dir, full, sizeof g_shot_dir - 1);
    }
    CreateDirectoryA(g_shot_dir, NULL);
    g_active = g_nlines > 0;
    LOGI("script: %d command(s) for %s #%d from %s", g_nlines, mode, index,
         path);
}

/* SDL's names, with '_' for a space ("Keypad_0") since arguments split on
 * whitespace. */
static SDL_Scancode key_named(const char *name)
{
    char buf[64];
    SDL_Scancode sc;
    int i;
    for (i = 0; name[i] && i < (int)sizeof buf - 1; i++)
        buf[i] = name[i] == '_' ? ' ' : name[i];
    buf[i] = 0;
    sc = SDL_GetScancodeFromName(buf);
    if (sc == SDL_SCANCODE_UNKNOWN) LOGW("script: unknown key '%s'", name);
    return sc;
}

/* Called once per SDL-thread iteration. */
void script_tick(void)
{
    Uint32 now;
    if (!g_active) return;
    now = SDL_GetTicks();

    if (g_shot_pending) {
        if ((Sint32)(now - g_shot_deadline) < 0) return;   /* still waiting */
        LOGW("script: shot %s timed out; no frame was presented", g_shot_path);
        InterlockedExchange(&g_shot_pending, 0);
    }
    if ((Sint32)(now - g_wait_until) < 0) return;

    while (g_pc < g_nlines) {
        char cmd[512], arg[512];
        const char *line = g_lines[g_pc++];
        int n = 0;
        cmd[0] = arg[0] = 0;
        sscanf(line, "%63s %n", cmd, &n);
        strncpy(arg, line + n, sizeof arg - 1);
        arg[sizeof arg - 1] = 0;

        if (!strcmp(cmd, "wait")) {
            g_wait_until = now + (Uint32)atoi(arg);
            return;
        } else if (!strcmp(cmd, "key")) {
            char name[64]; int hold = 80;
            if (sscanf(arg, "%63s %d", name, &hold) >= 1) {
                SDL_Scancode sc = key_named(name);
                video_script_key(sc, 1);
                SDL_Delay((Uint32)hold);      /* the ISR samples state, not edges */
                video_script_key(sc, 0);
                g_wait_until = now + (Uint32)hold + 40;
                return;
            }
        } else if (!strcmp(cmd, "down")) {
            video_script_key(key_named(arg), 1);
        } else if (!strcmp(cmd, "up")) {
            video_script_key(key_named(arg), 0);
        } else if (!strcmp(cmd, "text")) {
            const char *t;
            for (t = arg; *t; t++) dos_key_push((uint8_t)*t);
        } else if (!strcmp(cmd, "mouse")) {
            int x = 0, y = 0;
            sscanf(arg, "%d %d", &x, &y);
            mouse_script_move(x, y);
        } else if (!strcmp(cmd, "click")) {
            int b = !strcmp(arg, "right") ? 1 : 0;
            mouse_sdl_button(b, 1);
            SDL_Delay(90);
            mouse_sdl_button(b, 0);
            g_wait_until = now + 150;
            return;
        } else if (!strcmp(cmd, "shot")) {
            _snprintf(g_shot_path, sizeof g_shot_path, "%s\\%s.bmp",
                      g_shot_dir, arg);
            g_shot_deadline = now + 3000;
            InterlockedExchange(&g_shot_pending, 1);
            return;
        } else if (!strcmp(cmd, "callers")) {
            glide_callers_next();
        } else if (!strcmp(cmd, "trace")) {
            glide_trace_next();
        } else if (!strcmp(cmd, "log")) {
            LOGI("script: %s", arg);
        } else if (!strcmp(cmd, "watchlfb")) {
            /* watchlfb X Y: watch writes to that pixel of the CPU frame buffer */
            int px, py;
            if (sscanf(arg, "%d %d", &px, &py) == 2 && g_vram) {
                g_hwwatch = (uint32_t)(uintptr_t)(g_vram + ((size_t)py * 640 + px) * 2) -
                            g_img.delta;
                LOGI("script: watching LFB %d,%d (orig %08X)", px, py, g_hwwatch);
            }
        } else if (!strcmp(cmd, "watchfind")) {
            /* watchfind FILE OFFSET: find 32 bytes of a game file in memory
             * (the heap moves between runs) and watch reads and writes of
             * them, as --hwwatch ADDR:rw would */
            char fn[MAX_PATH];
            unsigned off = 0;
            uint8_t pat[32];
            FILE *f;
            if (sscanf(arg, "%259s %u", fn, &off) == 2 && (f = fopen(fn, "rb")) != NULL) {
                size_t n;
                fseek(f, (long)off, SEEK_SET);
                n = fread(pat, 1, sizeof pat, f);
                fclose(f);
                if (n == sizeof pat) {
                    MEMORY_BASIC_INFORMATION mi;
                    uint8_t *p = g_img.mem, *end = g_img.mem + g_img.reserved;
                    while (p < end && VirtualQuery(p, &mi, sizeof mi)) {
                        uint8_t *e = (uint8_t *)mi.BaseAddress + mi.RegionSize, *q;
                        if (mi.State == MEM_COMMIT)
                            for (q = p; q + sizeof pat <= e; q++)
                                if (!memcmp(q, pat, sizeof pat)) {
                                    g_hwwatch_rw = 1;
                                    g_hwwatch = (uint32_t)(uintptr_t)q - g_img.delta;
                                    LOGI("script: %s+%u at orig %08X, watched", fn, off,
                                         g_hwwatch);
                                    p = end;
                                    break;
                                }
                        if (p < end) p = e;
                    }
                }
            }
            if (!g_hwwatch) LOGW("script: watchfind %s: not found", arg);
        } else if (!strcmp(cmd, "memdump")) {   /* image + heap, fixups applied */
            char path[MAX_PATH];
            FILE *f;
            _snprintf(path, sizeof path, "%s\\%s.bin", g_shot_dir, arg);
            if ((f = fopen(path, "wb")) != NULL) {
                /* the whole reservation up to its last committed page; pages
                 * never committed come out as zeros */
                static const uint8_t zero[4096];
                MEMORY_BASIC_INFORMATION mi;
                uint8_t *p = g_img.mem, *end = g_img.mem + g_img.reserved, *top = p;
                while (p < end && VirtualQuery(p, &mi, sizeof mi)) {
                    uint8_t *e = (uint8_t *)mi.BaseAddress + mi.RegionSize;
                    if (mi.State == MEM_COMMIT) top = e < end ? e : end;
                    p = e;
                }
                for (p = g_img.mem; p < top; p += 4096) {
                    VirtualQuery(p, &mi, sizeof mi);
                    fwrite(mi.State == MEM_COMMIT ? p : zero, 1, 4096, f);
                }
                fclose(f);
                LOGI("script: image dumped to %s (delta 0x%08X, %u bytes)", path,
                     g_img.delta, (unsigned)(top - g_img.mem));
            }
        } else if (!strcmp(cmd, "quit")) {
            LOGI("script: quit");
            video_request_quit();
            return;
        } else {
            LOGW("script: unknown command '%s'", line);
        }
    }
    g_active = 0;
    LOGI("script: finished");
}

/* Renderer side: is a shot wanted, and where should it go? */
const char *script_shot_wanted(void)
{
    return g_shot_pending ? g_shot_path : NULL;
}

void script_shot_done(void)
{
    LOGI("script: saved %s", g_shot_path);
    InterlockedExchange(&g_shot_pending, 0);
}

/* 24-bit BMP from rows of BGR bytes. BMP stores bottom-up, so callers
 * holding a top-down image pass a negative stride from its last row. */
int write_bmp24(const char *path, int w, int h, const uint8_t *rows,
                int stride)
{
    FILE *f = fopen(path, "wb");
    int pad = (4 - (w * 3) % 4) % 4, y;
    uint32_t data = (uint32_t)((w * 3 + pad) * h);
    uint8_t hdr[54] = { 'B', 'M' };
    static const uint8_t zero[3];

    if (!f) return 0;
    *(uint32_t *)(hdr + 2)  = 54 + data;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = w;
    *(int32_t  *)(hdr + 22) = h;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = data;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++) {
        fwrite(rows + (ptrdiff_t)y * stride, 1, (size_t)w * 3, f);
        fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    return 1;
}

/* Top-down 0xAARRGGBB pixels, as render_frame holds them. */
int write_bmp_argb(const char *path, int w, int h, const uint32_t *px)
{
    uint8_t *bgr = (uint8_t *)malloc((size_t)w * h * 3);
    int i, ok;
    if (!bgr) return 0;
    for (i = 0; i < w * h; i++) {
        bgr[i * 3 + 0] = (uint8_t)px[i];
        bgr[i * 3 + 1] = (uint8_t)(px[i] >> 8);
        bgr[i * 3 + 2] = (uint8_t)(px[i] >> 16);
    }
    ok = write_bmp24(path, w, h, bgr + (size_t)(h - 1) * w * 3, -w * 3);
    free(bgr);
    return ok;
}
