/* Host services: logging, DOS (int 21h), DPMI (int 31h / int 2Fh),
 * mouse (int 33h) and port I/O.
 *
 * These are ABI contracts, not game logic -- each one is implemented against
 * the documented DOS/DPMI/VGA behaviour, so none of the game's own code has to
 * be understood to make them work.
 */

#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <intrin.h>     /* _ReadWriteBarrier */

/* ---------------------------------------------------------------- logging */

log_level g_log_level = LOG_INFO;
static FILE *g_logf;

void log_init(const char *path)
{
    g_logf = fopen(path, "w");
}

void log_msg(log_level lvl, const char *fmt, ...)
{
    static const char *tag[] = { "ERR ", "WARN", "INFO", "TRC " };
    va_list ap;
    if (lvl > g_log_level) return;
    if (!g_logf) return;
    fprintf(g_logf, "%s ", tag[lvl]);
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);                      /* survive a hard fault */
}

void log_shutdown(void)
{
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
}

/* ------------------------------------------------------------ DOS handles */

#define MAX_FH 64
static HANDLE g_fh[MAX_FH];
static int    g_fh_dev[MAX_FH];      /* CON / NUL / AUX / PRN -> log it */

static int is_device_name(const char *p)
{
    static const char *dev[] = { "CON", "NUL", "AUX", "PRN", "COM1", "LPT1" };
    char up[8];
    int i, n = 0;
    while (p[n] && n < 7) { up[n] = (char)toupper((unsigned char)p[n]); n++; }
    up[n] = '\0';
    for (i = 0; i < (int)(sizeof dev / sizeof dev[0]); i++)
        if (!strcmp(up, dev[i])) return 1;
    return 0;
}

static int fh_alloc(HANDLE h)
{
    int i;
    for (i = 5; i < MAX_FH; i++)
        if (!g_fh[i]) { g_fh[i] = h; return i; }
    return -1;
}

/* DOS paths already use backslashes and are relative to the asset directory,
 * which we make the process CWD, so no translation is needed. */
static const char *dos_path(CONTEXT *c) { return (const char *)(uintptr_t)c->Edx; }

static void set_cf(CONTEXT *c, int on)
{
    if (on) c->EFlags |= 1; else c->EFlags &= ~1u;
}
static int fail(CONTEXT *c, uint16_t err)
{
    c->Eax = (c->Eax & 0xFFFF0000u) | err;
    set_cf(c, 1);
    return 1;
}
static int ok16(CONTEXT *c, uint16_t v)
{
    c->Eax = (c->Eax & 0xFFFF0000u) | v;
    set_cf(c, 0);
    return 1;
}

/* Read and write return a byte count, and under a 32-bit extender that count
 * is EAX, not AX: DOS/4GW lets a single call move far more than 64 KB and the
 * game does exactly that. Returning it through ok16 truncated the result --
 * a 0x10000-byte read reported zero, which reads as end-of-file.
 *
 * On its own this changes nothing visible, and it was briefly dismissed on
 * that evidence. It is the second half of the lseek fix below: once the game
 * could size a file correctly it asked for the whole 285696-byte remainder of
 * title.pcx in one call, and ok16 would have answered 23552 -- the same wrong
 * number, arrived at a different way. Neither fix works without the other. */
static int ok32(CONTEXT *c, uint32_t v)
{
    c->Eax = v;
    set_cf(c, 0);
    return 1;
}

/* Interrupt vector table the game installs handlers into. */
static uint32_t g_ivec[256];

uint32_t dos_get_ivec(int vec) { return g_ivec[vec & 0xFF]; }

/* ------------------------------------------------------------- int 21h */

int dos_int21(CONTEXT *c)
{
    uint8_t ah = (uint8_t)(c->Eax >> 8);

    LOGT("int21 ah=%02X al=%02X bx=%08X cx=%08X dx=%08X @orig %08X",
         ah, (unsigned)(c->Eax & 0xFF), c->Ebx, c->Ecx, c->Edx,
         c->Eip - g_img.delta);

    switch (ah) {

    case 0x30:      /* get DOS version + extender identification.
                     * The startup passes EBX='RAHP' and tests the high word
                     * of EAX for an extender signature; answering with the
                     * DOS/4GW one keeps it on its native path. */
        c->Eax = 0x44580007u;                 /* 'XD' | DOS 7.0 */
        c->Ebx = 0;
        c->Ecx = 0;
        set_cf(c, 0);
        return 1;

    case 0x25:      /* set interrupt vector */
        g_ivec[c->Eax & 0xFF] = c->Edx;
        LOGI("set int vector 0x%02X -> orig 0x%08X",
             c->Eax & 0xFF, c->Edx - g_img.delta);
        set_cf(c, 0);
        return 1;

    case 0x35:      /* get interrupt vector */
        c->Ebx = g_ivec[c->Eax & 0xFF];
        set_cf(c, 0);
        return 1;

    case 0x19:      /* get current drive */
        return ok16(c, 2);                     /* C: */

    case 0x2C: {    /* get time  CH:CL = h:m, DH:DL = s:1/100 */
        SYSTEMTIME t; GetLocalTime(&t);
        c->Ecx = ((uint32_t)t.wHour << 8) | t.wMinute;
        c->Edx = ((uint32_t)t.wSecond << 8) | (t.wMilliseconds / 10);
        set_cf(c, 0);
        return 1;
    }
    case 0x2A: {    /* get date  CX = year, DH = month, DL = day */
        SYSTEMTIME t; GetLocalTime(&t);
        c->Ecx = t.wYear;
        c->Edx = ((uint32_t)t.wMonth << 8) | t.wDay;
        c->Eax = (c->Eax & 0xFFFFFF00u) | t.wDayOfWeek;
        set_cf(c, 0);
        return 1;
    }

    case 0x3C: {    /* create / truncate */
        HANDLE h = CreateFileA(dos_path(c), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        int fd;
        if (h == INVALID_HANDLE_VALUE) return fail(c, 5);
        fd = fh_alloc(h);
        if (fd < 0) { CloseHandle(h); return fail(c, 4); }
        LOGT("creat '%s' -> %d", dos_path(c), fd);
        return ok16(c, (uint16_t)fd);
    }

    case 0x3D: {    /* open */
        uint8_t mode = (uint8_t)(c->Eax & 0xFF) & 3;
        DWORD acc = (mode == 0) ? GENERIC_READ
                  : (mode == 1) ? GENERIC_WRITE
                                : (GENERIC_READ | GENERIC_WRITE);
        HANDLE h;
        int fd, dev = is_device_name(dos_path(c));
        if (dev) {
            /* A character device: give it a handle we only ever log. The slot
             * must still read as occupied, or the next open reuses it. */
            fd = fh_alloc(INVALID_HANDLE_VALUE);
            if (fd < 0) return fail(c, 4);
            g_fh_dev[fd] = 1;
            LOGI("open device '%s' -> %d", dos_path(c), fd);
            return ok16(c, (uint16_t)fd);
        }
        h = CreateFileA(dos_path(c), acc,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            LOGW("open '%s' FAILED (%lu)", dos_path(c), GetLastError());
            return fail(c, 2);
        }
        fd = fh_alloc(h);
        if (fd < 0) { CloseHandle(h); return fail(c, 4); }
        g_fh_dev[fd] = 0;
        LOGT("open '%s' -> %d", dos_path(c), fd);
        return ok16(c, (uint16_t)fd);
    }

    case 0x3E: {    /* close */
        uint16_t fd = (uint16_t)(c->Ebx & 0xFFFF);
        if (fd < MAX_FH && g_fh[fd]) {
            if (!g_fh_dev[fd]) CloseHandle(g_fh[fd]);
            g_fh[fd] = NULL;
            g_fh_dev[fd] = 0;
        }
        return ok16(c, 0);
    }

    case 0x3F: {    /* read */
        uint16_t fd = (uint16_t)(c->Ebx & 0xFFFF);
        DWORD n = 0;
        if (fd < MAX_FH && g_fh_dev[fd]) return ok16(c, 0);   /* device: EOF */
        if (fd >= MAX_FH || !g_fh[fd]) {
            LOGW("read from bad handle %u (%u bytes) @orig %08X",
                 fd, c->Ecx, c->Eip - g_img.delta);
            return fail(c, 6);
        }
        mem_ensure_committed((void *)(uintptr_t)c->Edx, c->Ecx);
        if (!ReadFile(g_fh[fd], (void *)(uintptr_t)c->Edx, c->Ecx, &n, NULL)) {
            LOGW("ReadFile on handle %u failed: %lu", fd, GetLastError());
            return fail(c, 5);
        }
        return ok32(c, (uint32_t)n);
    }

    case 0x40: {    /* write */
        uint16_t fd = (uint16_t)(c->Ebx & 0xFFFF);
        DWORD n = 0;
        if (fd <= 2 || (fd < MAX_FH && g_fh_dev[fd])) {
            LOGI("GAME OUTPUT (fd=%u): [%.*s]", fd, (int)c->Ecx,
                 (const char *)(uintptr_t)c->Edx);
            return ok16(c, (uint16_t)c->Ecx);
        }
        if (fd >= MAX_FH || !g_fh[fd]) return fail(c, 6);
        mem_ensure_committed((void *)(uintptr_t)c->Edx, c->Ecx);
        if (!WriteFile(g_fh[fd], (void *)(uintptr_t)c->Edx, c->Ecx, &n, NULL))
            return fail(c, 5);
        return ok32(c, (uint32_t)n);
    }

    case 0x41:      /* unlink */
        return DeleteFileA(dos_path(c)) ? ok16(c, 0) : fail(c, 2);

    case 0x42: {    /* lseek */
        uint16_t fd = (uint16_t)(c->Ebx & 0xFFFF);
        LONG hi = 0;
        DWORD pos;
        /* The offset arrives as CX:DX, high word in CX -- not as a flat EDX.
         * Seeks inside the first 64 KB of a file work either way, which is
         * why this went unnoticed. */
        LONG off = (LONG)(((c->Ecx & 0xFFFFu) << 16) | (c->Edx & 0xFFFFu));
        DWORD method = (c->Eax & 0xFF) == 1 ? FILE_CURRENT
                     : (c->Eax & 0xFF) == 2 ? FILE_END : FILE_BEGIN;
        if (fd >= MAX_FH || !g_fh[fd]) return fail(c, 6);
        pos = SetFilePointer(g_fh[fd], off, &hi, method);
        if (pos == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
            return fail(c, 5);
        /* And the result goes back the same way: DX:AX, high word in DX.
         * Returning the flat value in EAX with EDX left as the 64-bit high
         * half (always 0 here) meant a caller assembling DX:AX saw only the
         * low 16 bits. Seeking to the end to size a file therefore reported
         * 290231 as 28087, and the desktop's 290 KB title.pcx backdrop was
         * read to 28 KB and decoded to a band across the top of the screen
         * over a flat fill. */
        c->Eax = (c->Eax & 0xFFFF0000u) | (pos & 0xFFFFu);
        c->Edx = (c->Edx & 0xFFFF0000u) | ((pos >> 16) & 0xFFFFu);
        set_cf(c, 0);
        return 1;
    }

    case 0x44:      /* ioctl: report "character device" for handles <= 2 */
        c->Edx = (c->Ebx <= 2) ? 0x80 : 0x00;
        set_cf(c, 0);
        return 1;

    case 0x47: {    /* get current directory -> DS:ESI */
        char *dst = (char *)(uintptr_t)c->Esi;
        dst[0] = '\0';
        set_cf(c, 0);
        return 1;
    }

    case 0x48:      /* allocate DOS memory (paragraphs in BX) */
        c->Eax = 0x1000;                       /* a plausible segment */
        set_cf(c, 0);
        return 1;
    case 0x49:      /* free DOS memory */
        set_cf(c, 0);
        return 1;

    case 0x4A: {    /* resize memory block: BX = new top in 4K pages.
                     * This is how the Watcom heap grows, so silently agreeing
                     * to a top beyond our mapping would hand the game memory
                     * that cannot be committed later. */
        uint32_t top = (c->Ebx & 0x000FFFFF) << 12;
        uintptr_t lo = (uintptr_t)g_img.mem;
        uintptr_t hi = lo + g_img.reserved;
        if (top > hi) {
            LOGW("DOS resize to 0x%08X exceeds our region (top 0x%08zX)",
                 top, (size_t)hi);
            c->Ebx = (uint32_t)(hi >> 12);      /* largest we can offer */
            return fail(c, 8);
        }
        set_cf(c, 0);
        return 1;
    }

    case 0x4C:      /* terminate */
        emu_request_exit((int)(c->Eax & 0xFF));
        return 1;

    case 0x62:      /* get PSP segment */
        c->Ebx = 0x0100;
        set_cf(c, 0);
        return 1;

    case 0x33:      /* ctrl-break check */
        set_cf(c, 0);
        return 1;

    case 0x36:      /* free disk space */
        c->Eax = 4; c->Ebx = 0x8000; c->Ecx = 512; c->Edx = 0x8000;
        set_cf(c, 0);
        return 1;

    case 0x01: case 0x08: case 0x0B:   /* console input: never any */
        c->Eax &= 0xFFFFFF00u;
        set_cf(c, 0);
        return 1;

    case 0x09:      /* print $-terminated string */
        set_cf(c, 0);
        return 1;

    case 0x1A:      /* set DTA */
        set_cf(c, 0);
        return 1;

    default:
        todo_hit("int21", ah, c->Eip);
        return fail(c, 1);
    }
}

/* -------------------------------------------------------------- int 31h */

int dpmi_int31(CONTEXT *c)
{
    uint16_t ax = (uint16_t)(c->Eax & 0xFFFF);

    switch (ax) {
    case 0x0000: {  /* allocate LDT descriptors: CX of them */
        uint16_t sel = descr_alloc((int)(c->Ecx & 0xFFFF) ? (int)(c->Ecx & 0xFFFF) : 1);
        if (!sel) return fail(c, 0x8011);
        c->Eax = sel;
        set_cf(c, 0);
        return 1;
    }

    case 0x0002: {  /* real-mode segment -> descriptor */
        uint16_t seg = (uint16_t)(c->Ebx & 0xFFFF);
        uint16_t sel;
        if (seg == 0xA000)                  sel = SEL_VIDEO;
        else if (seg == DOSMEM_XFER_SEG)    sel = SEL_DOSBUF;
        else {
            sel = descr_alloc(1);
            if (!sel) return fail(c, 0x8011);
            descr_set_base(sel, (uint32_t)seg << 4);
        }
        LOGI("DPMI segment 0x%04X -> selector 0x%04X", seg, sel);
        c->Eax = sel;
        set_cf(c, 0);
        return 1;
    }

    case 0x0003:    /* get selector increment */
        c->Eax = 8;
        set_cf(c, 0);
        return 1;

    case 0x0006: {  /* get segment base address: BX = selector */
        uint32_t base = descr_get_base((uint16_t)(c->Ebx & 0xFFFF));
        c->Ecx = base >> 16;
        c->Edx = base & 0xFFFF;
        set_cf(c, 0);
        return 1;
    }

    case 0x0007:    /* set segment base address: BX = sel, CX:DX = linear */
        descr_set_base((uint16_t)(c->Ebx & 0xFFFF),
                       ((c->Ecx & 0xFFFF) << 16) | (c->Edx & 0xFFFF));
        set_cf(c, 0);
        return 1;

    case 0x0001:    /* free LDT descriptor      */
    case 0x0008:    /* set segment limit        */
    case 0x0009:    /* set descriptor access rights */
        set_cf(c, 0);
        return 1;

    case 0x0400:    /* get DPMI version */
        c->Eax = 0x005A;                   /* 0.90 */
        c->Ebx = 0x0005;                   /* 32-bit, no V86 */
        c->Ecx = 5;
        c->Edx = 0xFFFF;
        set_cf(c, 0);
        return 1;

    case 0x0200:    /* get real-mode interrupt vector */
        c->Ecx = 0; c->Edx = 0;
        set_cf(c, 0);
        return 1;
    case 0x0201:    /* set real-mode interrupt vector */
        set_cf(c, 0);
        return 1;

    case 0x0300:    /* simulate real-mode interrupt */
        return vbe_dispatch(c);

    case 0x0500: {  /* get free memory information */
        uint32_t *info = (uint32_t *)(uintptr_t)c->Edi;
        int i;
        for (i = 0; i < 12; i++) info[i] = 0xFFFFFFFFu;
        info[0] = 32u * 1024 * 1024;       /* largest free block */
        set_cf(c, 0);
        return 1;
    }

    case 0x0501: {  /* allocate memory block: BX:CX = size */
        uint32_t size = ((uint32_t)(c->Ebx & 0xFFFF) << 16) | (c->Ecx & 0xFFFF);
        void *p = VirtualAlloc(NULL, size ? size : 0x1000,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) { LOGW("DPMI alloc %u failed", size); return fail(c, 8); }
        c->Ebx = (uint32_t)(uintptr_t)p >> 16;
        c->Ecx = (uint32_t)(uintptr_t)p & 0xFFFF;
        c->Esi = (uint32_t)(uintptr_t)p >> 16;   /* handle == address */
        c->Edi = (uint32_t)(uintptr_t)p & 0xFFFF;
        set_cf(c, 0);
        return 1;
    }

    case 0x0502:    /* free memory block */
        set_cf(c, 0);
        return 1;

    case 0x0100: {  /* allocate DOS memory: AX = real-mode segment,
                     * DX = a selector for the same bytes. The two must agree,
                     * which is why this gets its own based selector rather
                     * than the zero-based low-memory one. */
        c->Eax = DOSMEM_XFER_SEG;
        c->Edx = SEL_DOSBUF;
        LOGI("DPMI alloc DOS memory -> seg 0x%04X / sel 0x%04X (linear 0x%X)",
             DOSMEM_XFER_SEG, SEL_DOSBUF, DOSMEM_XFER);
        set_cf(c, 0);
        return 1;
    }
    case 0x0101:
        set_cf(c, 0);
        return 1;

    case 0x0600:    /* lock linear region                */
    case 0x0601:    /* unlock linear region              */
    case 0x0602:    /* mark real-mode region pageable    */
    case 0x0603:    /* relock real-mode region           */
        set_cf(c, 0);
        return 1;

    case 0x0604:    /* get page size -> BX:CX */
        c->Ebx = 0;
        c->Ecx = 0x1000;
        set_cf(c, 0);
        return 1;

    case 0x0800: {  /* map physical address to linear: BX:CX addr, SI:DI size */
        uint32_t phys = ((uint32_t)(c->Ebx & 0xFFFF) << 16) | (c->Ecx & 0xFFFF);
        uint32_t size = ((uint32_t)(c->Esi & 0xFFFF) << 16) | (c->Edi & 0xFFFF);
        void *lin = video_map_phys(phys, size);
        if (!lin) return fail(c, 0x8021);
        c->Ebx = (uint32_t)(uintptr_t)lin >> 16;
        c->Ecx = (uint32_t)(uintptr_t)lin & 0xFFFF;
        set_cf(c, 0);
        return 1;
    }
    case 0x0801:    /* free physical mapping */
        set_cf(c, 0);
        return 1;

    case 0x0A00:    /* get vendor-specific API entry: none */
        return fail(c, 0x8001);

    default:
        todo_hit("int31", ax, c->Eip);
        return fail(c, 0x8001);
    }
}

/* -------------------------------------------------------------- int 2Fh */

int dpmi_int2f(CONTEXT *c)
{
    uint16_t ax = (uint16_t)(c->Eax & 0xFFFF);
    if (ax == 0x1687) {          /* get DPMI entry point: report none */
        c->Eax = (c->Eax & 0xFFFF0000u) | 1;
        return 1;
    }
    if (ax == 0x1684) {
        /* Get device API entry point. "None available" is ES:DI = 0000:0000,
         * and *both halves* matter: the timer ISR tests the stored segment as
         * well as the offset, and a non-zero segment sends it down a far call
         * through a null pointer. Zero the real ES, not just the shadow --
         * the game reads it back with `mov [mem], es`, which never faults and
         * so would otherwise store Win32's selector. */
        c->Edi = 0;
        c->SegEs = 0;
        g_seg.es = SEL_FLAT;
        LOGI("int 2Fh AX=1684 (get device API) -> none (ES:DI = 0:0)");
        return 1;
    }
    todo_hit("int2f", ax, c->Eip);
    c->Eax = (c->Eax & 0xFFFF0000u) | 1;
    return 1;
}

/* int 33h lives in mouse.c. */

/* ------------------------------------------------------------- port I/O */

/* Exact count, not a sampled one: a modulo-threshold log cannot distinguish
 * "polled three times at startup" from "polled every frame". */
volatile uint32_t g_joy_reads;

/* VGA DAC state: writes arrive as index then R,G,B triples. */
static int g_dac_index, g_dac_phase, g_dac_rgb[3];
/* Written by the SDL thread, drained by the game thread's int 9 handler.
 * Volatile and explicitly ordered: the byte must be visible before the tail
 * that publishes it, or the consumer can read a stale slot -- which for an
 * extended key means half of a two-byte sequence and a key that never comes
 * back up. */
static volatile uint8_t g_kbd_queue[64];
static volatile int g_kbd_head, g_kbd_tail;

/* The 8042 acknowledges every command byte with 0xFA, and the startup spins on
 * that ACK after sending "set LEDs" (0xED). Without it the game never gets
 * past keyboard init. */
static uint8_t g_kbd_reply;
static int     g_kbd_has_reply;

/* Pushed from the SDL thread, drained from the game thread's int 9 handler
 * via port 0x60. A single-producer/single-consumer ring, so the head and tail
 * updates need no locking. */
void kbd_push_scancode(uint8_t sc)
{
    int n = (g_kbd_tail + 1) % (int)sizeof g_kbd_queue;
    if (n == g_kbd_head) return;               /* full: drop, as hardware would */
    g_kbd_queue[g_kbd_tail] = sc;
    _ReadWriteBarrier();                       /* publish the byte, then the tail */
    g_kbd_tail = n;
}

int kbd_pending(void)
{
    return g_kbd_head != g_kbd_tail;
}

uint32_t host_ticks_ms(void) { return GetTickCount(); }

int ioport_in(uint16_t port, int width, uint32_t *out)
{
    *out = 0;
    switch (port) {
    case 0x60:                              /* keyboard data */
        if (g_kbd_has_reply) {              /* command ACK takes priority */
            *out = g_kbd_reply;
            g_kbd_has_reply = 0;
        } else if (g_kbd_head != g_kbd_tail) {
            *out = g_kbd_queue[g_kbd_head];
            g_kbd_head = (g_kbd_head + 1) % (int)sizeof g_kbd_queue;
            LOGT("game read scancode 0x%02X from port 0x60", *out);
        }
        return 1;
    case 0x64:                              /* keyboard status */
        *out = (g_kbd_has_reply || g_kbd_head != g_kbd_tail) ? 0x01 : 0x00;
        return 1;
    case 0x3DA: {                           /* VGA input status 1 */
        /* Toggle retrace bits so any spin-wait on them terminates. A game
         * polling this is between frames, which is the right moment to
         * present and pump events. */
        static int t;
        if (((++t) & 0x3F) == 0) video_pump();
        *out = (t & 1) ? 0x09 : 0x00;
        return 1;
    }
    case 0x3C9:                             /* DAC data read */
        *out = 0;
        return 1;
    case 0x40: case 0x41: case 0x42:        /* PIT counters */
        *out = (uint8_t)(GetTickCount() & 0xFF);
        return 1;
    case 0x201:
        g_joy_reads++;
        /* Game port, no joystick attached.
         *
         * An axis is read by strobing 0x201 to fire the four one-shots, then
         * polling until the matching bit falls; the number of iterations *is*
         * the axis value. Bits 4-7 are the buttons, active low.
         *
         * Returning the low bits clear says "the pulse already ended", so
         * every axis reads as zero -- a stick pegged into the bottom-left
         * corner, which the game dutifully flew into. With nothing plugged in
         * the one-shots never fire at all and the bits stay high, so the poll
         * loop hits its own timeout and the axis is discarded. */
        *out = 0xFF;
        return 1;
    default:
        todo_hit("in", port, 0);
        *out = 0xFF;
        return 1;
    }
}

int ioport_out(uint16_t port, int width, uint32_t val)
{
    switch (port) {
    case 0x3C8:                             /* DAC write index */
        g_dac_index = val & 0xFF;
        g_dac_phase = 0;
        return 1;
    case 0x3C9:                             /* DAC data: R, G, B */
        g_dac_rgb[g_dac_phase++] = val & 0x3F;
        if (g_dac_phase == 3) {
            video_set_dac(g_dac_index, g_dac_rgb[0], g_dac_rgb[1], g_dac_rgb[2]);
            g_dac_index = (g_dac_index + 1) & 0xFF;
            g_dac_phase = 0;
        }
        return 1;
    case 0x3C7:                             /* DAC read index */
        g_dac_index = val & 0xFF;
        g_dac_phase = 0;
        return 1;
    case 0x60:                              /* keyboard command -> ACK */
    case 0x64:
        g_kbd_reply = 0xFA;
        g_kbd_has_reply = 1;
        return 1;
    case 0x20: case 0xA0:                   /* PIC EOI */
    case 0x21: case 0xA1:                   /* PIC masks */
    case 0x40: case 0x41: case 0x42: case 0x43:   /* PIT */
    case 0x61:                              /* speaker */
    case 0x201:                             /* joystick strobe */
    case 0x3C0: case 0x3C2: case 0x3C4: case 0x3C5:   /* VGA attr / seq */
    case 0x3CE: case 0x3CF:                 /* VGA graphics controller */
    case 0x3D4: case 0x3D5:                 /* VGA CRTC */
        return 1;
    default:
        todo_hit("out", port, 0);
        return 1;
    }
}

