/* pad.c -- a game controller (Xbox, PlayStation, Switch Pro: anything SDL
 * knows) as the game's joystick, plus keys.
 *
 * The game has its own analog joystick support through the PC game port: it
 * strobes port 0x201 and counts how many reads it takes until the X and Y
 * one-shots fall, then maps the counts to -100..100 with the calibration in
 * JOYSTICK.CFG (12 dwords: min/max X, min/max Y, dead zone X lo/hi and Y
 * lo/hi, divisors X-/X+/Y-/Y+; FIGHT3 0x3C390). The left stick becomes that
 * joystick, counts made from the very calibration the game loaded, so the
 * game steers with its own analog code and needs no calibrating.
 *
 * Fight steers by joystick whenever its start-up test finds one (FIGHT3
 * 0x3C36B: the presence flag is the mode), and then ignores the arrow keys;
 * so the joystick is only there while a controller is, and the arrow keys
 * are added to its stick. Fight reads the joystick's result every frame but
 * refreshes it from the port only while a "press a key" screen polls
 * (0x5DCA0, every 20 timer ticks; interval 0 once the mission runs), so
 * during the mission the stick is written into that result directly.
 *
 * Everything else on the controller presses a key, taken from the original's
 * Thrustmaster FLCS profile (DEV\SF.M50). In the station the left stick
 * moves the mouse pointer instead.
 *
 * SDL thread: pad_init, pad_event, pad_tick, pad_script. Game thread (port
 * I/O): pad_port_in, pad_port_out.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <SDL.h>

#include "port.h"

int g_virtual_pad;                        /* --virtualpad: for test scripts */

static SDL_GameController *g_ctl;
static volatile LONG g_present;           /* a controller is open */
static SDL_Joystick *g_virt;
static volatile LONG g_stick_x, g_stick_y;   /* -1000..1000, for the game port */
static int32_t *joy_result(void);

/* ------------------------------------------------------------ buttons */

#define MOUSE_L  (-1)
#define MOUSE_R  (-2)

static const struct { int b; int fight, desk; } k_map[] = {
    /* FIGHT                                   station                  */
    { SDL_CONTROLLER_BUTTON_A,             SDL_SCANCODE_Q,      MOUSE_L },              /* Booster       */
    { SDL_CONTROLLER_BUTTON_B,             SDL_SCANCODE_D,      SDL_SCANCODE_ESCAPE },  /* Buzzer        */
    { SDL_CONTROLLER_BUTTON_X,             SDL_SCANCODE_RETURN, MOUSE_R },              /* next torpedo  */
    { SDL_CONTROLLER_BUTTON_Y,             SDL_SCANCODE_L,      SDL_SCANCODE_RETURN },  /* lock on front */
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  SDL_SCANCODE_J,      0 },                    /* next attacker */
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_SCANCODE_T,      0 },                    /* next object   */
    { SDL_CONTROLLER_BUTTON_DPAD_UP,       SDL_SCANCODE_A,      SDL_SCANCODE_UP },      /* full ahead    */
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     SDL_SCANCODE_Z,      SDL_SCANCODE_DOWN },    /* reverse ("y" on a German keyboard) */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     SDL_SCANCODE_G,      SDL_SCANCODE_LEFT },    /* torpedo lure  */
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    SDL_SCANCODE_N,      SDL_SCANCODE_RIGHT },   /* next nav point */
    { SDL_CONTROLLER_BUTTON_BACK,          SDL_SCANCODE_B,      0 },                    /* briefing      */
    { SDL_CONTROLLER_BUTTON_START,         SDL_SCANCODE_ESCAPE, SDL_SCANCODE_ESCAPE },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK,     SDL_SCANCODE_R,      0 },                    /* sonar         */
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    SDL_SCANCODE_KP_0,   0 },                    /* chase camera  */
};
#define NMAP ((int)(sizeof k_map / sizeof k_map[0]))

/* the triggers as buttons: guns and torpedo */
static const struct { int axis; int fight; } k_trig[] = {
    { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_SCANCODE_SPACE },
    { SDL_CONTROLLER_AXIS_TRIGGERLEFT,  SDL_SCANCODE_TAB },
};
static int g_trig_down[2];

static int g_held[SDL_CONTROLLER_BUTTON_MAX];  /* what each button pressed */

static void press(int what, int pressed)
{
    if (what == MOUSE_L || what == MOUSE_R)
        mouse_sdl_button(what == MOUSE_L ? 0 : 1, pressed);
    else if (what > 0)
        video_pad_key(what, pressed);
}

static void button(int b, int pressed)
{
    int i;
    if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX) return;
    if (!pressed) {                      /* release what the press pressed */
        if (g_held[b]) press(g_held[b], 0);
        g_held[b] = 0;
        return;
    }
    for (i = 0; i < NMAP; i++)
        if (k_map[i].b == b) {
            g_held[b] = main_is_desktop() ? k_map[i].desk : k_map[i].fight;
            press(g_held[b], 1);
            return;
        }
}

static void release_all(void)
{
    int b;
    for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) button(b, 0);
    for (b = 0; b < 2; b++)
        if (g_trig_down[b]) video_pad_key(k_trig[b].fight, 0), g_trig_down[b] = 0;
    InterlockedExchange(&g_stick_x, 0);
    InterlockedExchange(&g_stick_y, 0);
}

/* ------------------------------------------------------------ devices */

static void open_first(void)
{
    int i;
    if (g_ctl) return;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (g_ctl = SDL_GameControllerOpen(i)) != NULL) {
            LOGI("pad: %s", SDL_GameControllerName(g_ctl));
            InterlockedExchange(&g_present, 1);
            return;
        }
}

void pad_init(void)
{
    /* scripted runs play in a window without focus, where SDL would drop
     * the virtual pad's input; a real pad keeps the default */
    if (g_virtual_pad) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOGW("pad: no game controller support: %s", SDL_GetError());
        return;
    }
    if (g_virtual_pad) {
        SDL_VirtualJoystickDesc d;
        int idx;
        memset(&d, 0, sizeof d);
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = SDL_CONTROLLER_AXIS_MAX;
        d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
        d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
        d.name = "virtual pad";
        idx = SDL_JoystickAttachVirtualEx(&d);
        g_virt = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
        if (!g_virt) LOGW("pad: virtual pad failed: %s", SDL_GetError());
        else {
            SDL_JoystickSetVirtualAxis(g_virt, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            SDL_JoystickSetVirtualAxis(g_virt, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        }
    }
    if (!pad_selftest()) LOGW("pad: self-test failed, stick counts are off");
    open_first();
}

void pad_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED:
        open_first();
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_ctl && e->cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_ctl))) {
            LOGI("pad: disconnected");
            release_all();
            SDL_GameControllerClose(g_ctl);
            g_ctl = NULL;
            InterlockedExchange(&g_present, 0);
            open_first();
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (g_ctl && e->cbutton.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_ctl)))
            button(e->cbutton.button, e->type == SDL_CONTROLLERBUTTONDOWN);
        break;
    }
}

void pad_release_all(void) { release_all(); }

/* -1..1 with a round dead zone (worn sticks drift) and a gentler middle
 * for aiming; full deflection stays full. */
static void stick(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y), k;
    const float dz = 0.15f;
    if (m < dz) { *x = *y = 0.0f; return; }
    k = (m > 1.0f ? 1.0f : (m - dz) / (1.0f - dz));
    k = powf(k, 1.5f) / m;
    *x *= k;
    *y *= k;
}

/* once per SDL-thread iteration: triggers, the stick */
void pad_tick(void)
{
    static Uint32 last;
    static float frac_x, frac_y;
    Uint32 now = SDL_GetTicks();
    float dt = last ? (float)(now - last) / 1000.0f : 0.0f, x = 0.0f, y = 0.0f;
    int i;
    last = now;
    if (g_ctl) {
        x = SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        y = SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        stick(&x, &y);
        for (i = 0; i < 2 && !main_is_desktop(); i++) {
            float t = SDL_GameControllerGetAxis(g_ctl, (SDL_GameControllerAxis)k_trig[i].axis) / 32767.0f;
            int on = g_trig_down[i] ? t > 0.3f : t > 0.5f;     /* with hysteresis */
            if (on != g_trig_down[i]) video_pad_key(k_trig[i].fight, on);
            g_trig_down[i] = on;
        }
    }
    if (main_is_desktop()) {
        /* the pointer, about a screen width a second at full deflection */
        frac_x += x * 700.0f * dt;
        frac_y += y * 700.0f * dt;
        if ((int)frac_x || (int)frac_y) {
            mouse_sdl_motion((int)frac_x, (int)frac_y);
            frac_x -= (int)frac_x;
            frac_y -= (int)frac_y;
        }
        return;
    }
    {   /* the arrow keys on top: Down pulls back, as on the keyboard */
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        x += (float)ks[SDL_SCANCODE_RIGHT] - (float)ks[SDL_SCANCODE_LEFT];
        y += (float)ks[SDL_SCANCODE_DOWN] - (float)ks[SDL_SCANCODE_UP];
        x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
        y = y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;
    }
    InterlockedExchange(&g_stick_x, (LONG)(x * 1000.0f));
    InterlockedExchange(&g_stick_y, (LONG)(y * 1000.0f));
    {
        int32_t *r = joy_result();
        if (r && r[6] == 1) {
            r[0] = (int32_t)floorf(x * 100.0f + 0.5f);
            r[1] = (int32_t)floorf(y * 100.0f + 0.5f);
        }
    }
}

/* script: pad axis NAME -1..1 | pad button NAME 0|1 (--virtualpad) */
void pad_script(const char *arg)
{
    char what[16], name[32];
    float v = 0.0f;
    if (!g_virt) { LOGW("script: pad needs --virtualpad"); return; }
    if (sscanf(arg, "%15s %31s %f", what, name, &v) != 3) return;
    if (!strcmp(what, "axis")) {
        /* a virtual trigger's raw axis runs -32768 (released) to 32767 */
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name);
        float raw = a >= SDL_CONTROLLER_AXIS_TRIGGERLEFT ? -32768.0f + v * 65535.0f : v * 32767.0f;
        if (a != SDL_CONTROLLER_AXIS_INVALID)
            SDL_JoystickSetVirtualAxis(g_virt, a, (Sint16)raw);
    } else if (!strcmp(what, "button")) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name);
        if (b != SDL_CONTROLLER_BUTTON_INVALID)
            SDL_JoystickSetVirtualButton(g_virt, b, (Uint8)(v != 0.0f));
    }
}

/* ------------------------------------------------------------ game port */

/* JOYSTICK.CFG as the game ships it, for when it cannot be read */
static int32_t g_cal[12] = { 14, 128, 1, 128, 64, 65, 64, 65, 16, 20, 20, 20 };
static LARGE_INTEGER g_strobe_t, g_qpf;
static unsigned g_reads, g_cnt_x, g_cnt_y;
static int g_detect;
static volatile LONG g_port_used;         /* the game has touched the port */

/* FIGHT3's joystick result: x, y (-100..100), four buttons, the enable flag
 * (+0x18). Found through the read routine's own `mov [x], ebx` after its
 * strobe (orig 0x32C63), so only where that code is what it should be. */
static int32_t *joy_result(void)
{
    static int32_t *r;
    static int tried;
    const uint8_t *p;
    if (tried || !g_port_used) return r;
    tried = 1;
    p = (const uint8_t *)gp(0x00032C63u);
    if (p[0] == 0xEE && p[1] == 0x89 && p[2] == 0x1D)
        r = (int32_t *)(uintptr_t)*(const uint32_t *)(p + 3);
    else if (!main_is_desktop())
        LOGW("pad: not FIGHT3's joystick code at o0032C63, the stick only steers through the port");
    return r;
}
static LONGLONG g_last_strobe;

static void load_cal(void)
{
    int32_t c[12];
    FILE *f = fopen("JOYSTICK.CFG", "rb");
    size_t n = f ? fread(c, 4, 12, f) : 0;
    if (f) fclose(f);
    if (n == 12 && c[0] < c[1] && c[2] < c[3] && c[8] > 0 && c[9] > 0 && c[10] > 0 && c[11] > 0) {
        memcpy(g_cal, c, sizeof c);
        LOGI("pad: JOYSTICK.CFG x %d..%d (%d|%d) y %d..%d (%d|%d)",
             c[0], c[1], c[4], c[5], c[2], c[3], c[6], c[7]);
    } else
        LOGW("pad: JOYSTICK.CFG unreadable, using the shipped calibration");
}

/* The count the game turns back into v * 100 (see the file comment). */
static unsigned count(float v, int32_t mn, int32_t mx, int32_t lo, int32_t hi, int32_t dneg, int32_t dpos)
{
    float c = v > 0.0f ? (float)hi + v * 100.0f * (float)dpos / 32.0f
            : v < 0.0f ? (float)lo + v * 100.0f * (float)dneg / 32.0f : (float)lo;
    int n = (int)floorf(c + 0.5f);
    n = n < mn ? mn : n > mx ? mx : n;
    return n < 1 ? 1u : (unsigned)n;
}

void pad_port_out(uint32_t val)
{
    LARGE_INTEGER t;
    /* the presence test strobes 5 (the reads 3), waits and reads once */
    g_detect = (val & 0xFF) == 5;
    InterlockedExchange(&g_port_used, 1);
    QueryPerformanceCounter(&t);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    /* not polling every frame: detection, or back from calibrating */
    if (t.QuadPart - g_last_strobe > g_qpf.QuadPart) load_cal();
    g_last_strobe = t.QuadPart;
    g_strobe_t = t;
    g_reads = 0;
    g_cnt_x = count(g_stick_x / 1000.0f, g_cal[0], g_cal[1], g_cal[4], g_cal[5], g_cal[8], g_cal[9]);
    g_cnt_y = count(g_stick_y / 1000.0f, g_cal[2], g_cal[3], g_cal[6], g_cal[7], g_cal[10], g_cal[11]);
}

/* Bits 0/1 stay high for as many reads as the axis is worth, and like the
 * real one-shots end on their own after a while. Joystick B is absent;
 * buttons are up. The presence test finds the pulses over -- or, without a
 * controller, never started. */
uint8_t pad_port_in(void)
{
    LARGE_INTEGER t;
    uint8_t v = 0xFC;
    if (g_detect) {
        LOGI("pad: the game looks for a joystick: %s", g_present ? "there" : "none");
        return g_present ? 0xFC : 0xFF;
    }
    QueryPerformanceCounter(&t);
    g_reads++;
    if (g_qpf.QuadPart && t.QuadPart - g_strobe_t.QuadPart < g_qpf.QuadPart / 50) {
        if (g_reads < g_cnt_x) v |= 1;
        if (g_reads < g_cnt_y) v |= 2;
    }
    return v;
}

/* --selftest: counts turn back into the stick through the game's formula */
int pad_selftest(void)
{
    static const float vs[] = { -1.0f, -0.5f, -0.01f, 0.0f, 0.01f, 0.37f, 1.0f };
    int i, ok = 1;
    for (i = 0; i < (int)(sizeof vs / sizeof vs[0]); i++) {
        int32_t x = (int32_t)count(vs[i], 14, 128, 64, 65, 16, 20), g;
        g = x < 65 && x > 64 ? 0 : x >= 65 ? (x - 65) * 32 / 20 : (x - 64) * 32 / 16;
        g = g > 100 ? 100 : g < -100 ? -100 : g;
        ok &= fabsf((float)g - vs[i] * 100.0f) <= 2.0f;
    }
    return ok;
}
