/* Shared declarations for the FIGHT.EXE static port. */
#ifndef PORT_H
#define PORT_H

#include <windows.h>
#include <stdint.h>

/* ------------------------------------------------------------------- log */

typedef enum { LOG_ERR, LOG_WARN, LOG_INFO, LOG_TRACE } log_level;

extern log_level g_log_level;
void log_init(const char *path);
void log_msg(log_level lvl, const char *fmt, ...);
void log_shutdown(void);

#define LOGE(...) log_msg(LOG_ERR,   __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN,  __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO,  __VA_ARGS__)
#define LOGT(...) log_msg(LOG_TRACE, __VA_ARGS__)

/* Counts every distinct unimplemented site so the log stays readable but the
 * summary is still exhaustive. This is the real to-do list. */
void todo_hit(const char *kind, uint32_t detail, uint32_t eip);
void todo_dump(void);

/* --------------------------------------------------------------- LE image */

typedef struct {
    uint8_t  *mem;          /* one block holding every object            */
    uint32_t  size;
    uint32_t  delta;        /* new_addr = old_addr + delta               */
    uint32_t  img_lo, img_hi;         /* original (pre-rebase) extent    */
    uint32_t  code_size;              /* executable extent from mem      */
    uint32_t  entry;        /* already rebased                           */
    uint32_t  stack_top;    /* already rebased                           */
    uint32_t  heap_top;     /* the break we report to the C runtime       */
    uint32_t  reserved;     /* total reserved bytes from mem              */
} le_image;

/* The Watcom runtime takes the top of memory from the extender and grows its
 * heap towards it -- and then a little past it. So we reserve far more than we
 * advertise and commit pages on demand when the game first touches them. */
#define IMAGE_HEAP_SIZE    ( 32u * 1024 * 1024)   /* advertised break     */
#define IMAGE_HEAP_RESERVE (256u * 1024 * 1024)   /* room to grow into    */

extern le_image g_img;

int  le_load(const char *path, le_image *out);
void le_free(le_image *img);

/* Translate an original (link-time) address to where it now lives. */
static __inline void *gp(uint32_t orig_addr)
{
    return (void *)(uintptr_t)(orig_addr + g_img.delta);
}

/* --------------------------------------------------------------- emulator */

/* Command tail handed to the game through the PSP. FIGHT.EXE is normally
 * launched by SF.EXE and refuses to run unless argv[1] is "solo". */
extern const char *g_cmdline;

/* --watch <hex>: original (link-time) address of a global to poll and report
 * whenever it changes. Debug aid for tracking down init flags. */
extern uint32_t g_watch_addr;
extern int      g_watch_dwords;   /* --watch ADDR:N -- watch a whole struct */

/* --dumpkeys: periodically log which scancodes the game believes are held. */
extern int g_dump_keys;

/* --fbwatch OFF: guard-page one page of the framebuffer and log which
 * game instruction writes it. Finds the owner of a stride that static
 * reading cannot locate because the value is computed. */
extern int g_fbwatch;

/* --no-extern-irq: deliver interrupts only from inside the fault handler,
 * never via SuspendThread/SetThreadContext. */
extern int g_extern_irq;
extern uint32_t g_vram_faults;

/* DPMI descriptor table (implemented in emu.c, driven from services.c). */
uint16_t descr_alloc(int count);
void     descr_set_base(uint16_t sel, uint32_t linear);
uint32_t descr_get_base(uint16_t sel);
uint8_t *lin_to_ptr(uint32_t linear);
void     mem_ensure_committed(void *p, uint32_t n);

int  emu_install(void);
void emu_enter_game(void);

/* IRQ0 delivery: a separate thread injects a real interrupt frame into the
 * game thread at the rate the game programs into the PIT. */
void timer_start(void);
void timer_stats(uint32_t *delivered, uint32_t *skipped);
void emu_request_exit(int code);

/* Shadow segment registers. Win32 cannot honour a foreign selector, so every
 * `mov Sreg, x` is trapped, recorded here, and skipped. Accesses that then
 * fault get emulated against these. */
typedef struct {
    uint16_t es, ds, fs, gs, cs, ss;
} shadow_segs;
extern shadow_segs g_seg;

/* Selector values we hand out. Anything else is unknown and gets logged. */
#define SEL_FLAT   0x0000   /* meaning: leave the real flat selector alone */
#define SEL_DOSMEM 0x0024   /* PSP / low memory: zero-based                */
#define SEL_ENV    0x002C   /* environment block, based at DOSMEM_ENV      */
#define SEL_DOSBUF 0x0034   /* DPMI DOS-memory block, based at DOSMEM_XFER */
#define SEL_VIDEO  0x00A0   /* what we return for "segment 0xA000"         */

/* The real-mode transfer buffer handed out by DPMI "allocate DOS memory".
 * It is addressed two ways -- as real-mode segment 0x1000 inside a real-mode
 * call structure, and through a selector for the copy back -- so both must
 * resolve to the same bytes. */
#define DOSMEM_XFER      0x10000
#define DOSMEM_XFER_SEG  0x1000
#define DOSMEM_XFER_SIZE 0x10000

/* Where the environment block lives inside g_dosmem. Segment 0x0800 in the
 * PSP's environment field is the same location expressed the DOS way. */
#define DOSMEM_ENV      0x8000
#define DOSMEM_ENV_SIZE 0x2000

/* A 1 MB window standing in for the low DOS/BIOS address space, reachable
 * through SEL_DOSMEM. Accesses through a shadow selector are redirected here. */
extern uint8_t *g_dosmem;
#define DOSMEM_SIZE 0x100000

/* ------------------------------------------------------------------ video */

int  video_init(void);
void video_shutdown(void);

/* VBE service entry: called from the int 10h AH=4Fh path and from the hooks
 * placed over the game's own VBE dispatcher. `regs` is a DPMI-style real-mode
 * register block when non-NULL. Returns 1 if handled. */
int  vbe_dispatch(CONTEXT *c);

/* --pixfmt 555: interpret a 16bpp framebuffer as 5:5:5 instead of 5:6:5. */
extern int g_pixfmt_555;

/* --nograb: leave the host pointer alone. On by default -- the game steers
 * its own cursor from relative motion, so without a capture the host pointer
 * runs into the edge of the screen while the game's is still mid-travel. */
extern int g_mouse_grab;

/* --autokeys N: synthesise a keypress every N ms so runs need no operator.
 * Zero (the default) leaves input entirely to the user. */
extern int g_autokeys;
extern int g_dumpvram_frames;

/* DPMI "map physical address to linear" lands here. Returns NULL if the
 * address is not one we handed out. */
void *video_map_phys(uint32_t phys, uint32_t size);

void video_set_dac(int index, int r, int g, int b);
void video_set_bank(int window, int bank);
void video_present(void);
void video_pump(void);            /* SDL events -> keyboard/quit           */
int  video_want_quit(void);
void video_request_quit(void);
void video_script_key(int sdl_scancode, int pressed);

/* --script: unattended input and screenshots, see script.c */
void script_load(const char *path, const char *mode, int index);
void script_set_shot_dir(const char *dir);
void script_tick(void);
const char *script_shot_wanted(void);
void script_shot_done(void);
int  write_bmp24(const char *path, int w, int h, const uint8_t *rows,
                 int stride);
int  write_bmp_argb(const char *path, int w, int h, const uint32_t *px);
void mouse_script_move(int x, int y);
void glide_trace_next(void);
extern int g_render_scale;   /* --scale N: frame buffer is 640x480 * N  */
extern int g_msaa;           /* --msaa N: samples, 0/1 off              */
extern int g_smooth2d;       /* --smooth2d: bilinear 2D in 3dfx mode    */

/* The 64 KB banked window the game blits through, plus the shadow surface. */
extern uint8_t *g_vram;           /* full virtual framebuffer              */
extern uint32_t g_vram_size;
extern uint8_t *g_window;         /* 64 KB window into g_vram              */
extern int      g_vid_w, g_vid_h, g_vid_bpp, g_vid_pitch;
extern int      g_vid_active;

/* ------------------------------------------------------------------ audio */

/* Miles Sound System reimplemented over SDL audio. `audio_init` opens the
 * device; `audio_install_hooks` patches the AIL entry points and must run
 * after the image is loaded and relocated. */
int  audio_init(void);
/* Picks the per-image AIL address table. Must precede audio_install_hooks. */
#define AUDIO_IMAGE_FIGHT   0
#define AUDIO_IMAGE_DESKTOP 1
#define AUDIO_IMAGE_FIGHT3  2
void audio_select_image(int image);
void audio_install_hooks(void);
void audio_shutdown(void);

/* Polyphony. The game requests NumOfVoices from CONFIG.DES (ships as 8); this
 * raises the request. 16 is the most its own per-voice arrays can hold. */
void audio_set_voice_count(int n);

/* --calltrace: detour-log every call into every identified Miles entry point
 * (not just the 16 audio.c replaces) to calltrace.csv, without altering
 * behaviour. Install after the image is relocated; dump at either exit path. */
/* --cblit N: replace FIGHT's span blitter (orig 0x394EB) with the C
 * version in cblit.c. 1 = faithful, 2 = also recompute destinations from
 * the real screen pitch, 3 = faithful plus byte-for-byte verification
 * against the original on every call. */
extern int g_cblit;
extern int g_cflush;
extern int g_csurf;
extern int g_cdraw;
extern int g_cdrawscan;
extern int g_srcwatch;
extern int g_rasterscan;
extern int g_craster;
extern int g_spanprobe;
extern int g_ctex;
int  cblit_bp_hit(EXCEPTION_POINTERS *ep);
void cdrawscan_report(void);
void cblit_install(void);

void calltrace_install(void);
void calltrace_dump(void);

/* ------------------------------------------------------------------- host */

uint32_t dos_get_ivec(int vec);

/* The PIT divisor the game programs through port 0x40 is also kept in a
 * global by its own timer-rate routine, which is the easiest place to read it
 * back from. 0 means the full 65536. The address is per-image -- FIGHT.EXE and
 * DESKTOP.EXE are different link addresses -- and reading FIGHT's address in
 * DESKTOP left the timer thread always seeing 0, so DESKTOP silently ran at
 * the base 18.2 Hz tick despite programming the identical 200 Hz divisor
 * FIGHT does (confirmed with --watch f9ce6: 0x0000 -> 0x174E, and
 * 1193182/0x174E is 200.0 Hz). g_pit_divisor_addr is set from the resolved
 * mode in main() before the timer thread starts. */
#define GAME_PIT_DIVISOR_ADDR_FIGHT   0x0015803Eu
#define GAME_PIT_DIVISOR_ADDR_DESKTOP 0x000F9CE6u
extern uint32_t g_pit_divisor_addr;

/* Set from the resolved --mode in main(), before emu_install()/timer_start().
 * A general per-mode signal for anything else in the emulator that turns out
 * to need one -- the ISR-span constants below were the next thing that did. */
extern int g_desktop_mode;
/* Running FIGHT3.EXE/DESKTOP3.EXE, the 3dfx build: a different link that
 * shares none of the software builds' hooked addresses, and that renders
 * through Glide rather than VESA. */
extern int g_glide_mode;

/* ------------------------------------------------------------------ glide */
/* Redirects the 3dfx build's 130 Glide imports into our own code instead
 * of GLIDE2X.OVL. Must run after the image is loaded and relocated. */
extern int g_glide;        /* --glide 0 off, 1 intercept (default) */
extern int g_glidetrace;
extern int g_glidedebug;
extern int g_dumpsec;
extern int g_probe_x, g_probe_y;   /* --glidetrace: log every call          */
int  video_set_linear_mode(int w, int h, int bpp);
struct SDL_Window *video_window(void);
void glide_install(void);
void glide_summary(void);

int  dos_int21(CONTEXT *c);
int  dpmi_int31(CONTEXT *c);
int  dpmi_int2f(CONTEXT *c);
/* int 33h mouse driver (mouse.c). The SDL thread feeds motion and buttons in;
 * the game thread reads them back through the int 33h handler. */
int  mouse_int33(CONTEXT *c);
void mouse_sdl_motion(int dx, int dy);      /* deltas in game pixels */
void mouse_sdl_button(int button, int pressed);
void mouse_sdl_release_all(void);
int  mouse_cursor_visible(void);
int  mouse_driver_installed(void);

/* int 33h function 0x0C event handler. DESKTOP.EXE never polls for position,
 * so the pointer only moves if the driver calls back into the game. Delivered
 * from the same vetted point as a hardware interrupt; `mouse_cb_returning`
 * catches the planted return address and restores the interrupted registers. */
int  mouse_cb_ready(void);
int  mouse_cb_deliver(CONTEXT *c, int (*stack_ok)(uintptr_t, SIZE_T));
int  mouse_cb_returning(CONTEXT *c);
int  ioport_in(uint16_t port, int width, uint32_t *out);
int  ioport_out(uint16_t port, int width, uint32_t val);

/* How many times the game has read the game port. */
extern volatile uint32_t g_joy_reads;

void kbd_push_scancode(uint8_t sc);
/* Characters for DOS console input (int 21h AH=01/06/07/08/0B/0C). Fed from
 * SDL text input, which is layout-correct; the scancode ring above belongs to
 * the game's own int 9 ISR and must not be drained for this. */
void dos_key_push(uint8_t ch);
int  kbd_pending(void);
uint32_t host_ticks_ms(void);

#endif /* PORT_H */
