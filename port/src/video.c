/* VBE emulation backed by SDL2.
 *
 * The game drives graphics entirely through documented VESA BIOS Extensions
 * calls, so none of its rendering code has to be understood: we answer 4F00 /
 * 4F01 / 4F02 / 4F05 / 4F07 with a mode of our choosing, hand it a buffer to
 * paint into, and present that buffer with SDL.
 *
 * We advertise VBE 2.0 with *both* a banked window and a linear framebuffer.
 * Whichever path the game takes, it ends up writing into memory we own with a
 * flat ES, so no selector ever has to be honoured on the hot path.
 */

#include "port.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>

uint8_t *g_vram;
uint32_t g_vram_size;
uint8_t *g_window;
int g_vid_w = 640, g_vid_h = 480, g_vid_bpp = 8, g_vid_pitch = 640;
int g_vid_active;

/* The physical address we claim the linear framebuffer lives at. The game
 * only ever passes it back to us through DPMI "map physical to linear". */
#define FAKE_LFB_PHYS 0xE0000000u

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static uint32_t      g_pal[256];
static uint32_t     *g_conv;
static int           g_quit;
static uint32_t      g_last_present;
static int           g_cur_mode;

/* ---------------------------------------------------------- mode table */

typedef struct { uint16_t mode, w, h; uint8_t bpp; } vbe_mode;

/* The standard VESA mode numbers, matching the set SETUP.INI enumerates. The
 * game looks up an internal mode index and then searches this list for a
 * matching resolution and depth, so offering the full range is more robust
 * than guessing which one it wants. */
static const vbe_mode g_modes[] = {
    { 0x100,  640, 400,  8 },
    { 0x101,  640, 480,  8 },
    { 0x103,  800, 600,  8 },
    { 0x105, 1024, 768,  8 },
    { 0x10D,  320, 200, 15 }, { 0x10E,  320, 200, 16 }, { 0x10F,  320, 200, 24 },
    { 0x110,  640, 480, 15 }, { 0x111,  640, 480, 16 }, { 0x112,  640, 480, 24 },
    { 0x113,  800, 600, 15 }, { 0x114,  800, 600, 16 }, { 0x115,  800, 600, 24 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
};
#define NMODES ((int)(sizeof g_modes / sizeof g_modes[0]))

static const vbe_mode *find_mode(uint16_t m)
{
    int i;
    m &= 0x1FF;                            /* strip LFB / no-clear bits */
    for (i = 0; i < NMODES; i++)
        if (g_modes[i].mode == m) return &g_modes[i];
    return NULL;
}

/* ------------------------------------------------------------------ SDL */

/* Once the game has a linear framebuffer it paints into it directly and stops
 * making VBE calls -- and since the emulator only regains control on a fault,
 * there is then nothing left to drive presentation from. So SDL gets its own
 * thread: it owns the window, renderer and event queue, while the game thread
 * only ever writes pixels into g_vram. */
int g_pixfmt_555;
int g_dumpvram_frames;                      /* --pixfmt 555: read 16bpp as 5:5:5 */

static HANDLE        g_vid_thread;
static volatile LONG g_vid_ready;      /* 1 = up, -1 = failed */
static volatile LONG g_mode_dirty;

static void render_frame(void);
static void queue_key(SDL_Scancode sc, int pressed);
static void release_all_keys(const char *why);

static DWORD WINAPI vid_thread_main(LPVOID arg)
{
    (void)arg;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        InterlockedExchange(&g_vid_ready, -1);
        return 1;
    }
    g_win = SDL_CreateWindow("Schleichfahrt (ASMPort)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             g_vid_w * 2, g_vid_h * 2, SDL_WINDOW_RESIZABLE);
    g_ren = g_win ? SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED)
                  : NULL;
    if (g_win && !g_ren)
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_win || !g_ren) {
        LOGE("SDL window/renderer failed: %s", SDL_GetError());
        InterlockedExchange(&g_vid_ready, -1);
        return 1;
    }
    SDL_RenderSetLogicalSize(g_ren, g_vid_w, g_vid_h);
    InterlockedExchange(&g_vid_ready, 1);
    LOGI("SDL thread up");

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                /* Alt+F4 / the close button quit; the game gets every key,
                 * including F12, so nothing is stolen from it. */
                if (e.key.repeat) continue;      /* the hardware has no repeat */
                LOGT("SDL key %s scancode=%d",
                     e.type == SDL_KEYDOWN ? "down" : "up",
                     (int)e.key.keysym.scancode);
                queue_key(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
            }
            else if (e.type == SDL_MOUSEMOTION) {
                /* Logical-size scaling is on, so these deltas are already in
                 * the game's pixels. */
                mouse_sdl_motion(e.motion.xrel, e.motion.yrel);
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN ||
                     e.type == SDL_MOUSEBUTTONUP) {
                int b = e.button.button == SDL_BUTTON_LEFT   ? 0 :
                        e.button.button == SDL_BUTTON_RIGHT  ? 1 :
                        e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
                if (b >= 0)
                    mouse_sdl_button(b, e.type == SDL_MOUSEBUTTONDOWN);
            }
            else if (e.type == SDL_WINDOWEVENT) {
                LOGT("SDL window event %d", (int)e.window.event);
                /* Deliberately not LEAVE: the mouse leaving the window is
                 * normal during play and must not drop held keys. */
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                    e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    release_all_keys("focus loss");
                    mouse_sdl_release_all();
                }
            }
        }
        if (g_quit) {
            LOGI("window closed; shutting down");
            todo_dump();
            calltrace_dump();
            log_shutdown();
            ExitProcess(0);
        }
        if (InterlockedExchange(&g_mode_dirty, 0)) {
            if (g_tex) SDL_DestroyTexture(g_tex);
            g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      g_vid_w, g_vid_h);
            SDL_RenderSetLogicalSize(g_ren, g_vid_w, g_vid_h);
            SDL_SetWindowSize(g_win, g_vid_w * 2, g_vid_h * 2);
        }
        render_frame();
        SDL_Delay(16);
    }
}

int video_init(void)
{
    int i;
    DWORD t0;

    /* Allocated here rather than on the SDL thread: the game may touch the
     * framebuffer before that thread has finished coming up.
     * Enough for the deepest mode we advertise (1024x768x24) plus a bank of
     * slack for the 64 KB window to slide over. */
    g_vram_size = 1024u * 768u * 4u + 0x10000u;
    g_vram = (uint8_t *)VirtualAlloc(NULL, g_vram_size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_conv = (uint32_t *)malloc(1024 * 768 * 4);
    if (!g_vram || !g_conv) { LOGE("framebuffer alloc failed"); return 0; }
    memset(g_vram, 0, g_vram_size);
    g_window = g_vram;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u | (i * 0x010101u);

    g_vid_thread = CreateThread(NULL, 0, vid_thread_main, NULL, 0, NULL);
    if (!g_vid_thread) { LOGE("CreateThread failed: %lu", GetLastError());
                         return 0; }

    t0 = GetTickCount();
    while (!g_vid_ready && GetTickCount() - t0 < 5000) Sleep(10);
    if (g_vid_ready != 1) { LOGE("SDL thread failed to start"); return 0; }

    LOGI("video up; vram=%p (%u bytes), fake LFB phys=0x%08X",
         g_vram, g_vram_size, FAKE_LFB_PHYS);
    return 1;
}

void video_shutdown(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_tex = NULL; g_ren = NULL; g_win = NULL;
    SDL_Quit();
}

static void ensure_texture(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, g_vid_w, g_vid_h);
    SDL_RenderSetLogicalSize(g_ren, g_vid_w, g_vid_h);
}

void *video_map_phys(uint32_t phys, uint32_t size)
{
    if (phys == FAKE_LFB_PHYS) {
        LOGI("mapping fake LFB phys 0x%08X (%u bytes) -> %p",
             phys, size, g_vram);
        return g_vram;
    }
    /* Real-mode video aperture: hand back the banked window. */
    if (phys >= 0xA0000u && phys < 0xC0000u) {
        LOGI("mapping VGA aperture 0x%08X -> %p", phys, g_window);
        return g_window;
    }
    LOGW("unmapped physical address 0x%08X (%u bytes)", phys, size);
    return NULL;
}

void video_set_dac(int index, int r, int g, int b)
{
    /* VGA DAC is 6-bit per channel; widen to 8. */
    g_pal[index & 0xFF] = 0xFF000000u
                        | ((uint32_t)((r & 0x3F) * 255 / 63) << 16)
                        | ((uint32_t)((g & 0x3F) * 255 / 63) << 8)
                        |  (uint32_t)((b & 0x3F) * 255 / 63);
}

void video_set_bank(int window, int bank)
{
    uint32_t off = (uint32_t)bank * 0x10000u;
    if (off + 0x10000u > g_vram_size) off = 0;
    g_window = g_vram + off;
    LOGT("bank window=%d bank=%d -> offset 0x%X", window, bank, off);
}

/* Thread-side only: converts g_vram through the palette and presents it.
 * Never call this from the game thread -- SDL owns its window's thread. */
static void render_frame(void)
{
    int x, y;
    const uint8_t *src;
    uint32_t *dst;

    if (!g_vid_active || !g_ren) return;
    if (!g_tex) ensure_texture();

    src = g_vram;
    dst = g_conv;
    for (y = 0; y < g_vid_h; y++) {
        const uint8_t *row = src + (size_t)y * g_vid_pitch;
        switch (g_vid_bpp) {
        case 8:
            for (x = 0; x < g_vid_w; x++) *dst++ = g_pal[row[x]];
            break;
        case 15:
            for (x = 0; x < g_vid_w; x++) {
                uint16_t p = ((const uint16_t *)row)[x];
                uint32_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
                *dst++ = 0xFF000000u | (r * 255 / 31 << 16)
                                     | (g * 255 / 31 << 8) | (b * 255 / 31);
            }
            break;
        case 16:
            if (g_pixfmt_555) {          /* --pixfmt 555: diagnostic */
                for (x = 0; x < g_vid_w; x++) {
                    uint16_t p = ((const uint16_t *)row)[x];
                    uint32_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F,
                             b = p & 0x1F;
                    *dst++ = 0xFF000000u | (r * 255 / 31 << 16)
                                         | (g * 255 / 31 << 8) | (b * 255 / 31);
                }
            } else {
                for (x = 0; x < g_vid_w; x++) {
                    uint16_t p = ((const uint16_t *)row)[x];
                    uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F,
                             b = p & 0x1F;
                    *dst++ = 0xFF000000u | (r * 255 / 31 << 16)
                                         | (g * 255 / 63 << 8) | (b * 255 / 31);
                }
            }
            break;
        case 24:
            for (x = 0; x < g_vid_w; x++) {
                const uint8_t *p = row + x * 3;
                *dst++ = 0xFF000000u | ((uint32_t)p[2] << 16)
                                     | ((uint32_t)p[1] << 8) | p[0];
            }
            break;
        default:
            for (x = 0; x < g_vid_w; x++) *dst++ = 0xFF000000u;
            break;
        }
    }
    /* --dumpvram N: write the raw framebuffer once, N frames in. Whether the
     * dark areas hold zeros or real pixels decides between "nothing was drawn"
     * and "drawn but converted wrong", which no amount of looking can. */
    if (g_dumpvram_frames > 0 && --g_dumpvram_frames == 0) {
        FILE *f = fopen("vram.bin", "wb");
        if (f) {
            fwrite(g_vram, 1, (size_t)g_vid_pitch * g_vid_h, f);
            fclose(f);
            LOGI("dumped %dx%d %dbpp pitch=%d framebuffer to vram.bin",
                 g_vid_w, g_vid_h, g_vid_bpp, g_vid_pitch);
        }
    }

    SDL_UpdateTexture(g_tex, NULL, g_conv, g_vid_w * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    g_last_present = SDL_GetTicks();

    {   /* The SDL thread also drains the event queue, so its loop rate is the
         * ceiling on mouse responsiveness. */
        static uint32_t frames; static Uint32 t0;
        frames++;
        if (!t0) t0 = SDL_GetTicks();
        else if (SDL_GetTicks() - t0 >= 2000) {
            LOGI("sdl thread: %.1f loops/s, %u framebuffer accesses faulted",
                 frames * 1000.0 / (SDL_GetTicks() - t0), g_vram_faults);
            g_vram_faults = 0;
            frames = 0; t0 = SDL_GetTicks();
        }
    }
}

/* Called from the game thread when it flips a page. The SDL thread presents
 * continuously anyway, so this only needs to exist, not to do anything. */
void video_present(void) { }

/* SDL scancode -> IBM PC set-1 scancode.
 *
 * The layout is positional, so this maps SDL's physical scancodes (which are
 * themselves USB positional codes) rather than the characters produced. That
 * keeps a German keyboard working the way the game expects without any locale
 * handling: the game does its own translation through the tables at
 * 0x15DD57 / 0x15DE25.
 *
 * `ext` marks keys the controller reports with a 0xE0 prefix. The game's ISR
 * handles that prefix explicitly (it latches it at 0x2BC48), so those have to
 * be sent as a two-byte sequence or the arrow keys land on the numeric keypad
 * codes instead. */
typedef struct { SDL_Scancode sdl; uint8_t code; uint8_t ext; } keymap_ent;

static const keymap_ent g_keymap[] = {
    { SDL_SCANCODE_ESCAPE, 0x01, 0 },
    { SDL_SCANCODE_1, 0x02, 0 }, { SDL_SCANCODE_2, 0x03, 0 },
    { SDL_SCANCODE_3, 0x04, 0 }, { SDL_SCANCODE_4, 0x05, 0 },
    { SDL_SCANCODE_5, 0x06, 0 }, { SDL_SCANCODE_6, 0x07, 0 },
    { SDL_SCANCODE_7, 0x08, 0 }, { SDL_SCANCODE_8, 0x09, 0 },
    { SDL_SCANCODE_9, 0x0A, 0 }, { SDL_SCANCODE_0, 0x0B, 0 },
    { SDL_SCANCODE_MINUS, 0x0C, 0 }, { SDL_SCANCODE_EQUALS, 0x0D, 0 },
    { SDL_SCANCODE_BACKSPACE, 0x0E, 0 }, { SDL_SCANCODE_TAB, 0x0F, 0 },

    { SDL_SCANCODE_Q, 0x10, 0 }, { SDL_SCANCODE_W, 0x11, 0 },
    { SDL_SCANCODE_E, 0x12, 0 }, { SDL_SCANCODE_R, 0x13, 0 },
    { SDL_SCANCODE_T, 0x14, 0 }, { SDL_SCANCODE_Y, 0x15, 0 },
    { SDL_SCANCODE_U, 0x16, 0 }, { SDL_SCANCODE_I, 0x17, 0 },
    { SDL_SCANCODE_O, 0x18, 0 }, { SDL_SCANCODE_P, 0x19, 0 },
    { SDL_SCANCODE_LEFTBRACKET, 0x1A, 0 },
    { SDL_SCANCODE_RIGHTBRACKET, 0x1B, 0 },
    { SDL_SCANCODE_RETURN, 0x1C, 0 }, { SDL_SCANCODE_LCTRL, 0x1D, 0 },

    { SDL_SCANCODE_A, 0x1E, 0 }, { SDL_SCANCODE_S, 0x1F, 0 },
    { SDL_SCANCODE_D, 0x20, 0 }, { SDL_SCANCODE_F, 0x21, 0 },
    { SDL_SCANCODE_G, 0x22, 0 }, { SDL_SCANCODE_H, 0x23, 0 },
    { SDL_SCANCODE_J, 0x24, 0 }, { SDL_SCANCODE_K, 0x25, 0 },
    { SDL_SCANCODE_L, 0x26, 0 }, { SDL_SCANCODE_SEMICOLON, 0x27, 0 },
    { SDL_SCANCODE_APOSTROPHE, 0x28, 0 }, { SDL_SCANCODE_GRAVE, 0x29, 0 },
    { SDL_SCANCODE_LSHIFT, 0x2A, 0 }, { SDL_SCANCODE_BACKSLASH, 0x2B, 0 },

    { SDL_SCANCODE_Z, 0x2C, 0 }, { SDL_SCANCODE_X, 0x2D, 0 },
    { SDL_SCANCODE_C, 0x2E, 0 }, { SDL_SCANCODE_V, 0x2F, 0 },
    { SDL_SCANCODE_B, 0x30, 0 }, { SDL_SCANCODE_N, 0x31, 0 },
    { SDL_SCANCODE_M, 0x32, 0 }, { SDL_SCANCODE_COMMA, 0x33, 0 },
    { SDL_SCANCODE_PERIOD, 0x34, 0 }, { SDL_SCANCODE_SLASH, 0x35, 0 },
    { SDL_SCANCODE_RSHIFT, 0x36, 0 },
    { SDL_SCANCODE_KP_MULTIPLY, 0x37, 0 },
    { SDL_SCANCODE_LALT, 0x38, 0 }, { SDL_SCANCODE_SPACE, 0x39, 0 },
    { SDL_SCANCODE_CAPSLOCK, 0x3A, 0 },

    { SDL_SCANCODE_F1, 0x3B, 0 }, { SDL_SCANCODE_F2, 0x3C, 0 },
    { SDL_SCANCODE_F3, 0x3D, 0 }, { SDL_SCANCODE_F4, 0x3E, 0 },
    { SDL_SCANCODE_F5, 0x3F, 0 }, { SDL_SCANCODE_F6, 0x40, 0 },
    { SDL_SCANCODE_F7, 0x41, 0 }, { SDL_SCANCODE_F8, 0x42, 0 },
    { SDL_SCANCODE_F9, 0x43, 0 }, { SDL_SCANCODE_F10, 0x44, 0 },
    { SDL_SCANCODE_F11, 0x57, 0 }, { SDL_SCANCODE_F12, 0x58, 0 },

    { SDL_SCANCODE_NUMLOCKCLEAR, 0x45, 0 },
    { SDL_SCANCODE_SCROLLLOCK, 0x46, 0 },
    { SDL_SCANCODE_KP_7, 0x47, 0 }, { SDL_SCANCODE_KP_8, 0x48, 0 },
    { SDL_SCANCODE_KP_9, 0x49, 0 }, { SDL_SCANCODE_KP_MINUS, 0x4A, 0 },
    { SDL_SCANCODE_KP_4, 0x4B, 0 }, { SDL_SCANCODE_KP_5, 0x4C, 0 },
    { SDL_SCANCODE_KP_6, 0x4D, 0 }, { SDL_SCANCODE_KP_PLUS, 0x4E, 0 },
    { SDL_SCANCODE_KP_1, 0x4F, 0 }, { SDL_SCANCODE_KP_2, 0x50, 0 },
    { SDL_SCANCODE_KP_3, 0x51, 0 }, { SDL_SCANCODE_KP_0, 0x52, 0 },
    { SDL_SCANCODE_KP_PERIOD, 0x53, 0 },
    { SDL_SCANCODE_NONUSBACKSLASH, 0x56, 0 },

    /* extended (0xE0-prefixed) */
    { SDL_SCANCODE_KP_ENTER, 0x1C, 1 }, { SDL_SCANCODE_RCTRL, 0x1D, 1 },
    { SDL_SCANCODE_KP_DIVIDE, 0x35, 1 }, { SDL_SCANCODE_RALT, 0x38, 1 },
    { SDL_SCANCODE_HOME, 0x47, 1 },      { SDL_SCANCODE_UP, 0x48, 1 },
    { SDL_SCANCODE_PAGEUP, 0x49, 1 },    { SDL_SCANCODE_LEFT, 0x4B, 1 },
    { SDL_SCANCODE_RIGHT, 0x4D, 1 },     { SDL_SCANCODE_END, 0x4F, 1 },
    { SDL_SCANCODE_DOWN, 0x50, 1 },      { SDL_SCANCODE_PAGEDOWN, 0x51, 1 },
    { SDL_SCANCODE_INSERT, 0x52, 1 },    { SDL_SCANCODE_DELETE, 0x53, 1 },
};
#define NKEYMAP ((int)(sizeof g_keymap / sizeof g_keymap[0]))

/* Which keys we have reported as held. The game's ISR keeps a key-state array
 * and only clears a key on its break code, so a make without a matching break
 * leaves it down forever. */
static uint8_t g_key_down[SDL_NUM_SCANCODES];

/* Queue a key event as the controller would report it: the 0xE0 prefix first
 * where applicable, then make code, or break code with bit 7 set. */
static void queue_key(SDL_Scancode sc, int pressed)
{
    int i;
    if (sc < 0 || sc >= SDL_NUM_SCANCODES) return;
    for (i = 0; i < NKEYMAP; i++) {
        if (g_keymap[i].sdl != sc) continue;
        if (pressed == g_key_down[sc]) return;      /* no redundant edges */
        if (g_keymap[i].ext) kbd_push_scancode(0xE0);
        kbd_push_scancode(pressed ? g_keymap[i].code
                                  : (uint8_t)(g_keymap[i].code | 0x80));
        g_key_down[sc] = (uint8_t)pressed;
        return;
    }
}

/* Losing focus swallows the key-up: alt-tab away mid-turn and the game steers
 * in that direction forever, because as far as its ISR is concerned the key
 * was never released. Send break codes for everything we believe is held. */
static void release_all_keys(const char *why)
{
    int sc, n = 0;
    for (sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
        if (!g_key_down[sc]) continue;
        queue_key((SDL_Scancode)sc, 0);
        n++;
    }
    if (n) LOGI("input: released %d held key(s) on %s", n, why);
}

/* Game-thread side. Events and presentation belong to the SDL thread, so all
 * this does is keep the BIOS tick count (40:6C) moving at the classic
 * 18.2 Hz -- code that waits on it would otherwise spin forever. */
void video_pump(void)
{
    if (g_dosmem)
        *(uint32_t *)(g_dosmem + 0x46C) =
            (uint32_t)((uint64_t)GetTickCount() * 182 / 10000);
}

int video_want_quit(void) { return g_quit; }

/* -------------------------------------------------------------- VBE core */

#pragma pack(push, 1)
typedef struct {
    char     sig[4];
    uint16_t version;
    uint32_t oem_string_ptr;
    uint8_t  caps[4];
    uint32_t video_mode_ptr;
    uint16_t total_memory;          /* in 64 KB blocks */
    uint16_t oem_sw_rev;
    uint32_t oem_vendor_ptr, oem_product_ptr, oem_revision_ptr;
    uint8_t  reserved[222];
    uint8_t  oem_data[256];
} vbe_info_block;

typedef struct {
    uint16_t mode_attr;             /* 0x00 */
    uint8_t  wina_attr, winb_attr;  /* 0x02 */
    uint16_t win_granularity;       /* 0x04 (KB) */
    uint16_t win_size;              /* 0x06 (KB) */
    uint16_t wina_segment;          /* 0x08 */
    uint16_t winb_segment;          /* 0x0A */
    uint32_t win_func_ptr;          /* 0x0C */
    uint16_t bytes_per_scanline;    /* 0x10 */
    uint16_t x_res, y_res;          /* 0x12, 0x14 */
    uint8_t  x_char, y_char, planes, bits_per_pixel;   /* 0x16..0x19 */
    uint8_t  banks, memory_model, bank_size, image_pages;  /* 0x1A..0x1D */
    uint8_t  reserved1;             /* 0x1E */
    uint8_t  red_mask, red_pos, green_mask, green_pos;     /* 0x1F..0x22 */
    uint8_t  blue_mask, blue_pos, rsvd_mask, rsvd_pos;     /* 0x23..0x26 */
    uint8_t  direct_color_info;     /* 0x27 */
    uint32_t phys_base_ptr;         /* 0x28 */
    uint32_t reserved2;             /* 0x2C */
    uint16_t reserved3;             /* 0x30 */
    uint8_t  pad[206];
} vbe_mode_info;
#pragma pack(pop)

/* The pointers inside a VbeInfoBlock are real-mode far pointers
 * (segment:offset), not flat addresses -- the caller resolves them as
 * (seg << 4) + off. So the mode list and OEM string have to live in the low
 * megabyte that g_dosmem models, at addresses expressible that way.
 * Handing back a flat pointer here is what made the game bail out. */
#define VBE_MODELIST_LIN 0x9000u
#define VBE_OEM_LIN      0x9100u
#define FARPTR(lin)      ((((uint32_t)(lin) >> 4) << 16) | ((lin) & 0xF))

static void do_4f00(void *buf)
{
    vbe_info_block *v = (vbe_info_block *)buf;
    uint16_t *list = (uint16_t *)(g_dosmem + VBE_MODELIST_LIN);
    /* SETUP.INI records what the original setup program detected, and the
     * game checks the running BIOS against it. Report exactly what is in the
     * shipped SETUP.INI so the two agree. */
    static const char oem[] = "S3 Incorporated. Trio64";
    int i;
    int vbe2 = (memcmp(v->sig, "VBE2", 4) == 0);

    for (i = 0; i < NMODES; i++) list[i] = g_modes[i].mode;
    list[NMODES] = 0xFFFF;
    memcpy(g_dosmem + VBE_OEM_LIN, oem, sizeof oem);

    memset(v, 0, vbe2 ? sizeof *v : 256);
    memcpy(v->sig, "VESA", 4);
    v->version = 0x0200;                    /* VESA_VERSION_NUMBER=2, SUB=0 */
    v->total_memory = 2097152u / 65536u;    /* MEMORY=2097152 -> 32 blocks  */
    v->video_mode_ptr = FARPTR(VBE_MODELIST_LIN);
    v->oem_string_ptr = FARPTR(VBE_OEM_LIN);
    LOGI("VBE 4F00 filled (VBE2=%d), %d modes at far 0x%08X",
         vbe2, NMODES, v->video_mode_ptr);
}

static int do_4f01(uint16_t mode, void *buf)
{
    const vbe_mode *m = find_mode(mode);
    vbe_mode_info *mi = (vbe_mode_info *)buf;
    if (!m) { LOGW("VBE 4F01: mode 0x%X unsupported", mode); return 0; }

    memset(mi, 0, sizeof *mi);
    mi->mode_attr = 0x009B;   /* supported | colour | graphics | LFB available */
    mi->wina_attr = 0x07;     /* exists, readable, writable */
    mi->win_granularity = 64;
    mi->win_size = 64;
    mi->wina_segment = 0xA000;
    mi->win_func_ptr = 0;     /* no protected-mode bank function: use 4F05 */
    mi->bytes_per_scanline = (uint16_t)(m->w * ((m->bpp + 7) / 8));
    mi->x_res = m->w; mi->y_res = m->h;
    mi->x_char = 8; mi->y_char = 16;
    mi->planes = 1;
    mi->bits_per_pixel = m->bpp;
    mi->banks = 1;
    mi->bank_size = 0;
    mi->image_pages = 0;
    mi->phys_base_ptr = FAKE_LFB_PHYS;

    if (m->bpp <= 8) {
        mi->memory_model = 4;                  /* packed pixel */
    } else {
        mi->memory_model = 6;                  /* direct colour */
        switch (m->bpp) {
        case 15:
            mi->red_mask = 5; mi->red_pos   = 10;
            mi->green_mask = 5; mi->green_pos = 5;
            mi->blue_mask = 5; mi->blue_pos  = 0;
            mi->rsvd_mask = 1; mi->rsvd_pos  = 15;
            break;
        case 16:
            mi->red_mask = 5; mi->red_pos   = 11;
            mi->green_mask = 6; mi->green_pos = 5;
            mi->blue_mask = 5; mi->blue_pos  = 0;
            break;
        default:                                /* 24 */
            mi->red_mask = 8; mi->red_pos   = 16;
            mi->green_mask = 8; mi->green_pos = 8;
            mi->blue_mask = 8; mi->blue_pos  = 0;
            break;
        }
    }
    LOGI("VBE 4F01 mode 0x%X -> %dx%dx%d pitch=%d",
         mode, m->w, m->h, m->bpp, mi->bytes_per_scanline);
    return 1;
}

static int do_4f02(uint16_t mode)
{
    const vbe_mode *m = find_mode(mode);
    if (!m) { LOGW("VBE 4F02: mode 0x%X unsupported", mode); return 0; }
    g_vid_w = m->w; g_vid_h = m->h; g_vid_bpp = m->bpp;
    g_vid_pitch = m->w * ((m->bpp + 7) / 8);
    g_cur_mode = mode;
    g_window = g_vram;
    g_vid_active = 1;
    InterlockedExchange(&g_mode_dirty, 1);   /* SDL thread rebuilds the texture */
    LOGI("VBE 4F02 set mode 0x%X -> %dx%dx%d (LFB bit %s)",
         mode, m->w, m->h, m->bpp, (mode & 0x4000) ? "set" : "clear");
    return 1;
}

/* Service a VBE call. Registers come either straight from the CONTEXT (a real
 * `int 10h`) or from a DPMI real-mode call structure (int 31h AX=0300). */
static int vbe_service(uint32_t *peax, uint32_t ebx, uint32_t ecx,
                       uint32_t edx, void *es_edi)
{
    uint16_t ax = (uint16_t)(*peax & 0xFFFF);
    uint8_t  fn = (uint8_t)(ax & 0xFF);

    if ((ax >> 8) != 0x4F) return 0;

    switch (fn) {
    case 0x00: do_4f00(es_edi); *peax = 0x004F; return 1;
    case 0x01: *peax = do_4f01((uint16_t)ecx, es_edi) ? 0x004F : 0x014F; return 1;
    case 0x02: *peax = do_4f02((uint16_t)ebx) ? 0x004F : 0x014F; return 1;
    case 0x03: *peax = 0x004F; return 1;      /* caller reads BX */
    case 0x05:                                 /* window control */
        if (((ebx >> 8) & 0xFF) == 0) {        /* BH=0 set */
            video_set_bank(ebx & 0xFF, (int)(edx & 0xFFFF));
            *peax = 0x004F;
        } else {                               /* BH=1 get */
            *peax = 0x004F;
        }
        return 1;
    case 0x07:                                 /* set/get display start */
        video_present();
        *peax = 0x004F;
        return 1;
    case 0x08: *peax = 0x004F; return 1;       /* DAC width: 6 bits */
    case 0x0A: *peax = 0x014F; return 1;       /* no PM interface */
    default:
        todo_hit("vbe", fn, 0);
        *peax = 0x014F;
        return 1;
    }
}

/* DPMI real-mode call structure (int 31h AX=0300). */
#pragma pack(push, 1)
typedef struct {
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
} dpmi_rmcs;
#pragma pack(pop)

int vbe_dispatch(CONTEXT *c)
{
    video_pump();

    /* int 31h AX=0300: BL = vector, ES:EDI -> real-mode register block */
    if ((c->Eax & 0xFFFF) == 0x0300) {
        dpmi_rmcs *r = (dpmi_rmcs *)(uintptr_t)c->Edi;
        uint8_t vec = (uint8_t)(c->Ebx & 0xFF);
        MEMORY_BASIC_INFORMATION mbi;
        void *buf;
        uint32_t eax;

        if (!VirtualQuery(r, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
            LOGW("DPMI 0300: register block at %p is not mapped", (void *)r);
            c->EFlags |= 1u;
            return 1;
        }

        /* ES:DI *inside* the block is a real-mode far pointer, not a flat one.
         * Low memory is modelled by g_dosmem, so resolve it there -- treating
         * it as a flat address is how this used to corrupt the heap. */
        buf = g_dosmem + ((((uint32_t)r->es << 4) + (r->edi & 0xFFFF)) & 0xFFFFF);
        eax = r->eax;

        switch (vec) {
        case 0x10:
            if (((eax >> 8) & 0xFF) == 0x4F) {
                vbe_service(&eax, r->ebx, r->ecx, r->edx, buf);
            } else if (((eax >> 8) & 0xFF) == 0x0F) {   /* get video mode */
                eax = 0x0300u | (g_vid_active ? 0x03 : 0x03);
                r->ebx = 0;
            } else if (((eax >> 8) & 0xFF) == 0x00) {   /* set video mode */
                LOGI("real-mode int 10h set mode 0x%02X (ignored)", eax & 0xFF);
            } else {
                todo_hit("rmint10", (uint16_t)((eax >> 8) & 0xFF), c->Eip);
            }
            break;

        case 0x2F:                                       /* multiplex */
            /* Residents the game probes for -- MSCDEX, a network redirector,
             * and friends. None are present: AL = 0 is "not installed", and
             * BX = 0 covers the MSCDEX drive-count convention. */
            LOGI("real-mode int 2Fh AX=%04X -> not installed",
                 (uint16_t)(eax & 0xFFFF));
            eax &= 0xFFFFFF00u;
            r->ebx = 0;
            break;

        case 0x33: {                                     /* mouse */
            /* DESKTOP.EXE does not issue `int 33h` -- it goes through DPMI
             * "simulate real-mode interrupt" with 0x33 in the call structure
             * (the dispatcher at 0x70B3C builds the packet and calls int 31h
             * AX=0300). This arm used to answer "no mouse" here, which meant
             * the driver in mouse.c was never consulted no matter what it
             * reported, and DESKTOP exited 102 -- its mouse object at
             * 0x1060E8 never got its "installed" field set.
             *
             * Route it to the same implementation the direct path uses, by
             * borrowing a CONTEXT for the register subset int 33h defines. */
            CONTEXT m;
            LOGT("rm int 33h fn 0x%04X", (uint16_t)(eax & 0xFFFF));
            if ((eax & 0xFFFF) == 0x000C || (eax & 0xFFFF) == 0x0014)
                LOGI("int 33h fn 0x%04X: handler at %04X:%04X, mask 0x%04X",
                     (uint16_t)(eax & 0xFFFF), r->es, (uint16_t)(r->edx & 0xFFFF),
                     (uint16_t)(r->ecx & 0xFFFF));
            memset(&m, 0, sizeof m);
            m.Eax = eax;
            m.Ebx = r->ebx;
            m.Ecx = r->ecx;
            m.Edx = r->edx;
            m.Esi = r->esi;
            m.Edi = r->edi;
            mouse_int33(&m);
            eax    = m.Eax;
            r->ebx = m.Ebx;
            r->ecx = m.Ecx;
            r->edx = m.Edx;
            r->esi = m.Esi;
            break;
        }

        case 0x66:
            /* Miles' classic low-level digital driver (DIGPAK) lives on this
             * fixed vector, with documented functions at AH=06 (0688h-069Fh)
             * and AH=07 (0701h, a MIDI-driver's digital-capability check
             * called from inside DIGPAK's own CheckIn). The one call
             * DESKTOP.EXE actually makes is AX=0401h, matching neither, and
             * answering it either way (confirmed empirically: "present" and
             * "absent" were both tried) produces no further calls through
             * this vector -- so whatever it is, it is not the gate for the
             * cutscene audio. Answered as DIGPAK's own CheckIn failure
             * convention (AX=0) since that is what a real system missing
             * this optional legacy driver would also report. */
            LOGI("real-mode int 66h (Miles low-level driver): ax=%04X "
                 "bx=%04X cx=%04X dx=%04X si=%04X di=%04X -- reporting "
                 "no driver present",
                 (uint16_t)eax, (uint16_t)r->ebx, (uint16_t)r->ecx,
                 (uint16_t)r->edx, (uint16_t)r->esi, (uint16_t)r->edi);
            eax &= 0xFFFF0000u;
            break;

        default:
            todo_hit("rmint", vec, c->Eip);
            /* todo_hit's own WARN flags the gap once; the register state is a
             * --trace-level detail for digging into a specific one further. */
            LOGT("real-mode int %02Xh: ax=%04X bx=%04X cx=%04X dx=%04X "
                 "si=%04X di=%04X",
                 vec, (uint16_t)eax, (uint16_t)r->ebx, (uint16_t)r->ecx,
                 (uint16_t)r->edx, (uint16_t)r->esi, (uint16_t)r->edi);
            break;
        }

        r->eax = eax;
        r->flags &= ~1u;
        c->EFlags &= ~1u;
        return 1;
    }

    /* a direct int 10h */
    {
        uint32_t eax = c->Eax;
        void *buf = (void *)(uintptr_t)c->Edi;
        if ((eax & 0xFF00) == 0x4F00) {
            int r = vbe_service(&eax, c->Ebx, c->Ecx, c->Edx, buf);
            c->Eax = eax;
            return r;
        }
        if (((eax >> 8) & 0xFF) == 0x00) {    /* set text/VGA mode */
            LOGI("int 10h set mode 0x%02X (ignored)", eax & 0xFF);
            return 1;
        }
        todo_hit("int10", (uint16_t)((eax >> 8) & 0xFF), c->Eip);
        return 1;
    }
}
