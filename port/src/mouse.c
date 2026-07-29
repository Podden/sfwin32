/* int 33h mouse driver, backed by SDL.
 *
 * Split the way audio.c is: the SDL thread feeds raw events in through
 * mouse_sdl_motion/mouse_sdl_button, and the game thread reads the result
 * through mouse_int33. The state between them is the DOS driver's own model,
 * because that is what the game is written against.
 *
 * The position is integrated from *relative* motion and clamped to the range
 * the game sets, rather than sampled from the host cursor. That is what a real
 * driver does, and it is the only version where function 0x04 (set position)
 * means anything -- a game that recentres the pointer every frame to read
 * deflection from it would otherwise fight the OS cursor and win nothing.
 */

#include "port.h"

#include <stdio.h>

/* Driver state. Written by the SDL thread, read by the game thread; the
 * quantities are single words and a torn read costs at most one frame of
 * pointer motion, so this deliberately does not lock. */
static volatile int g_mx, g_my;             /* virtual position              */
static volatile int g_buttons;              /* bit 0 left, 1 right, 2 middle */
static volatile int g_mickey_x, g_mickey_y; /* since the last 0x0B           */

static int g_min_x, g_max_x, g_min_y, g_max_y;
static int g_cursor_shown;                  /* driver-drawn cursor counter   */
static int g_installed;                     /* has the game reset us yet     */

/* Per-button press/release bookkeeping for functions 0x05 and 0x06. */
static volatile int g_press_count[3], g_release_count[3];
static volatile int g_press_x[3], g_press_y[3];
static volatile int g_release_x[3], g_release_y[3];

/* Mickeys per 8 pixels, as functions 0x0F/0x1A describe it. The defaults are
 * the driver's own: 8 horizontally, 16 vertically. */
static int g_mickeys_x = 8, g_mickeys_y = 16;
static int g_double_speed = 64;

static void mouse_set_default_range(void)
{
    g_min_x = 0;
    g_min_y = 0;
    g_max_x = (g_vid_w > 0 ? g_vid_w : 640) - 1;
    g_max_y = (g_vid_h > 0 ? g_vid_h : 480) - 1;
}

static void clamp_position(void)
{
    if (g_mx < g_min_x) g_mx = g_min_x;
    if (g_mx > g_max_x) g_mx = g_max_x;
    if (g_my < g_min_y) g_my = g_min_y;
    if (g_my > g_max_y) g_my = g_max_y;
}

/* ------------------------------------------------------- SDL thread side */

/* dx/dy arrive in game pixels: SDL_RenderSetLogicalSize makes SDL scale mouse
 * event coordinates into logical space for us, so no conversion is needed. */
void mouse_sdl_motion(int dx, int dy)
{
    if (!dx && !dy) return;
    g_mx += dx;
    g_my += dy;
    clamp_position();
    /* Function 0x0B reports mickeys, not pixels. */
    g_mickey_x += dx * g_mickeys_x / 8;
    g_mickey_y += dy * g_mickeys_y / 8;
}

void mouse_sdl_button(int button, int pressed)
{
    int mask;
    if (button < 0 || button > 2) return;
    mask = 1 << button;
    if (pressed) {
        g_buttons |= mask;
        g_press_count[button]++;
        g_press_x[button] = g_mx;
        g_press_y[button] = g_my;
    } else {
        g_buttons &= ~mask;
        g_release_count[button]++;
        g_release_x[button] = g_mx;
        g_release_y[button] = g_my;
    }
}

/* Buttons must not stay stuck down when the window loses focus, for exactly
 * the same reason held keys must not -- see release_all_keys in video.c. */
void mouse_sdl_release_all(void)
{
    int b;
    for (b = 0; b < 3; b++)
        if (g_buttons & (1 << b)) mouse_sdl_button(b, 0);
}

int mouse_cursor_visible(void) { return g_installed && g_cursor_shown >= 0; }

/* ------------------------------------------------------ game thread side */

#define LO16(x) ((uint16_t)((x) & 0xFFFF))

static void set16(DWORD *reg, int val)
{
    /* Sign-extend so a game reading the full 32-bit register sees the same
     * number as one reading the 16-bit half. Mickeys are signed. */
    *reg = (DWORD)(int32_t)(int16_t)LO16(val);
}

int mouse_int33(CONTEXT *c)
{
    uint16_t ax = LO16(c->Eax);
    int b;

    switch (ax) {
    case 0x0000:                            /* reset and status            */
    case 0x0021:                            /* software reset              */
        mouse_set_default_range();
        g_mx = (g_min_x + g_max_x) / 2;     /* centred, never in a corner  */
        g_my = (g_min_y + g_max_y) / 2;
        g_buttons = 0;
        g_mickey_x = g_mickey_y = 0;
        g_cursor_shown = -1;                /* hidden until 0x01           */
        g_mickeys_x = 8; g_mickeys_y = 16;
        for (b = 0; b < 3; b++) {
            g_press_count[b] = g_release_count[b] = 0;
            g_press_x[b] = g_press_y[b] = 0;
            g_release_x[b] = g_release_y[b] = 0;
        }
        g_installed = 1;
        c->Eax = 0xFFFF;                    /* driver present              */
        c->Ebx = 2;                         /* two buttons                 */
        LOGI("int 33h reset: mouse present, range %d..%d x %d..%d, centred",
             g_min_x, g_max_x, g_min_y, g_max_y);
        return 1;

    case 0x0001:                            /* show cursor                 */
        if (g_cursor_shown < 0) g_cursor_shown++;
        return 1;
    case 0x0002:                            /* hide cursor                 */
        g_cursor_shown--;
        return 1;

    case 0x0003:                            /* get position and buttons    */
        set16(&c->Ebx, g_buttons);
        set16(&c->Ecx, g_mx);
        set16(&c->Edx, g_my);
        return 1;

    case 0x0004:                            /* set position                */
        g_mx = (int16_t)LO16(c->Ecx);
        g_my = (int16_t)LO16(c->Edx);
        clamp_position();
        return 1;

    case 0x0005:                            /* button press info           */
    case 0x0006: {                          /* button release info         */
        int press = (ax == 0x0005);
        b = (int)LO16(c->Ebx);
        if (b < 0 || b > 2) b = 0;
        set16(&c->Eax, g_buttons);
        set16(&c->Ebx, press ? g_press_count[b] : g_release_count[b]);
        set16(&c->Ecx, press ? g_press_x[b] : g_release_x[b]);
        set16(&c->Edx, press ? g_press_y[b] : g_release_y[b]);
        if (press) g_press_count[b] = 0; else g_release_count[b] = 0;
        return 1;
    }

    case 0x0007:                            /* set horizontal range        */
        g_min_x = (int16_t)LO16(c->Ecx);
        g_max_x = (int16_t)LO16(c->Edx);
        if (g_min_x > g_max_x) { int t = g_min_x; g_min_x = g_max_x; g_max_x = t; }
        clamp_position();
        LOGT("int 33h set X range %d..%d", g_min_x, g_max_x);
        return 1;
    case 0x0008:                            /* set vertical range          */
        g_min_y = (int16_t)LO16(c->Ecx);
        g_max_y = (int16_t)LO16(c->Edx);
        if (g_min_y > g_max_y) { int t = g_min_y; g_min_y = g_max_y; g_max_y = t; }
        clamp_position();
        LOGT("int 33h set Y range %d..%d", g_min_y, g_max_y);
        return 1;

    case 0x0009:                            /* set graphics cursor shape   */
    case 0x000A:                            /* set text cursor             */
    case 0x0010:                            /* conditional off             */
        return 1;                           /* we do not draw the cursor   */

    case 0x000B:                            /* read motion counters        */
        set16(&c->Ecx, g_mickey_x);
        set16(&c->Edx, g_mickey_y);
        g_mickey_x = 0;
        g_mickey_y = 0;
        return 1;

    case 0x000C:                            /* set event handler           */
    case 0x0014:                            /* swap event handler          */
        /* A far call from the driver into game code on every event. Nothing
         * here can issue one, and a game that steers from it would sit
         * perfectly still rather than misbehave -- so this is worth knowing
         * about explicitly rather than silently returning success. */
        todo_hit("int33_callback", ax, c->Eip);
        LOGW("int 33h AX=%04X: event handler requested; not supported. "
             "If the mouse does nothing, this is why.", ax);
        return 1;

    case 0x000F:                            /* set mickeys per 8 pixels    */
        g_mickeys_x = (int16_t)LO16(c->Ecx);
        g_mickeys_y = (int16_t)LO16(c->Edx);
        if (g_mickeys_x <= 0) g_mickeys_x = 8;
        if (g_mickeys_y <= 0) g_mickeys_y = 16;
        return 1;

    case 0x001A:                            /* set sensitivity             */
        g_mickeys_x = (int16_t)LO16(c->Ebx);
        g_mickeys_y = (int16_t)LO16(c->Ecx);
        g_double_speed = (int16_t)LO16(c->Edx);
        if (g_mickeys_x <= 0) g_mickeys_x = 8;
        if (g_mickeys_y <= 0) g_mickeys_y = 16;
        LOGT("int 33h set sensitivity %d/%d, double at %d",
             g_mickeys_x, g_mickeys_y, g_double_speed);
        return 1;
    case 0x001B:                            /* get sensitivity             */
        set16(&c->Ebx, g_mickeys_x);
        set16(&c->Ecx, g_mickeys_y);
        set16(&c->Edx, g_double_speed);
        return 1;

    case 0x0024:                            /* get driver version/type     */
        c->Eax = 0;
        set16(&c->Ebx, 0x0800);             /* report 8.00                 */
        set16(&c->Ecx, 0x0400);             /* CH=4 PS/2, CL=0 no IRQ      */
        return 1;

    default:
        todo_hit("int33", ax, c->Eip);
        return 1;
    }
}
