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
    uint8_t  *mem;          /* one block holding both objects            */
    uint32_t  size;
    uint32_t  delta;        /* new_addr = old_addr + delta               */
    uint32_t  obj1_base, obj1_size;   /* original (pre-rebase) values    */
    uint32_t  obj2_base, obj2_size;
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
extern int g_dumpvram_frames;

/* DPMI "map physical address to linear" lands here. Returns NULL if the
 * address is not one we handed out. */
void *video_map_phys(uint32_t phys, uint32_t size);

void video_set_dac(int index, int r, int g, int b);
void video_set_bank(int window, int bank);
void video_present(void);
void video_pump(void);            /* SDL events -> keyboard/quit           */
int  video_want_quit(void);

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
void audio_select_image(int desktop);
void audio_install_hooks(void);
void audio_shutdown(void);

/* Polyphony. The game requests NumOfVoices from CONFIG.DES (ships as 8); this
 * raises the request. 16 is the most its own per-voice arrays can hold. */
void audio_set_voice_count(int n);

/* ------------------------------------------------------------------- host */

uint32_t dos_get_ivec(int vec);

/* The PIT divisor the game programs through port 0x40 is also kept in this
 * global by its own timer-rate routine, which is the easiest place to read it
 * back from. 0 means the full 65536. */
#define GAME_PIT_DIVISOR_ADDR 0x0015803Eu

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
int  kbd_pending(void);
uint32_t host_ticks_ms(void);

#endif /* PORT_H */
