/* The span blitter, reimplemented in C.
 *
 * FIGHT.EXE composes its 3D viewport into an off-screen buffer and copies it
 * to the screen through a list of spans, executed by a `rep movsd` loop at
 * orig 0x394EB. Each 20-byte record is
 *
 *     +0x00  source address
 *     +0x04  destination address
 *     +0x08  alternate destination (second half of a bank-split span)
 *     +0x0C  byte count, as a word
 *     +0x0E  second byte count
 *
 * and the counts are shifted right by two -- ARITHMETICALLY, as sar does;
 * reading them through an unsigned type turns the 0x8000 terminator into a
 * positive 8192-dword span and corrupts memory -- to give dwords. A *negative* count
 * marks a span that crosses a 64 KB bank boundary: the tail of one call
 * copies the first part to +0x04, the caller switches banks, and the head of
 * the next call skips those dwords and copies the remainder to +0x08. The
 * list ends at a record whose count word is 0x8000 with a zero second count,
 * which is the same condition that makes this function return.
 *
 * None of that was inferred from the disassembly alone: the record layout and
 * the live contents were read out of a running game through a guard page (see
 * --fbwatch in emu.c), which is also how the destination stride was measured.
 *
 * Why reimplement it at all: the destination addresses are baked into the
 * records by a builder that computes base + y*stride + x*2, and at anything
 * other than 640x480 the stride it uses is wrong -- the viewport tears into
 * diagonal stripes. Here the destination can be recomputed from the source
 * offset instead of trusted, which fixes the geometry without having to find
 * and correct the builder. That is --cblit 2; --cblit 1 is a faithful copy of
 * the original, and --cblit 3 additionally proves it is faithful by running
 * both and comparing the framebuffer byte for byte.
 *
 * The address is FIGHT.EXE's. DESKTOP.EXE is a different link and would need
 * its own, so this installs in fight mode only.
 */

#include "port.h"
#include <string.h>
#include <stdlib.h>

#define BLIT_FIGHT   0x000394EBu
/* The next level up: walks the same list, switching the VBE bank window
 * between chunks, and calls the blitter for each run of spans that fits in
 * one bank. push ebx/ecx/esi/edi then `mov edi,eax` -- six bytes to a clean
 * boundary, so the five-byte detour needs one NOP of padding. */
#define FLUSH_FIGHT  0x00023040u
#define FLUSH_LEN    6
/* vbe_set_window(al = window index, dx = bank), the game's own wrapper. */
#define SETWIN_FIGHT 0x00038C38u
/* surface_init(eax = descriptor out, edx = VBE ModeInfoBlock). Fills the
 * surface the whole 2D library draws through -- window size and granularity,
 * the row stride, the base address and the bytes per pixel. This is where the
 * stride the span builder later uses comes from, which is what made it worth
 * having in C. push ebx/ecx/esi + `mov ecx,eax` is exactly five bytes. */
#define SURF_FIGHT   0x000206D0u
#define SURF_LEN     5
/* Maps the linear framebuffer and caches it; idempotent, so calling it once
 * from each side during verification is harmless. */
#define MAPLFB_FIGHT 0x00038FB4u
/* One of eight near-identical drawing primitives: clip, then walk the target
 * in bands, switching the VBE window per band, blitting each through 0x1BFD0.
 * Watcom regcall in EAX/EDX/EBX/ECX plus four stack arguments, and it cleans
 * those itself (`ret 0x10`).
 *
 * It shares a 0x84-byte stack frame with its callees, so the C version has to
 * lay that frame out exactly: 0x00..0x4F is the clip/blit descriptor the
 * clipper fills in, 0x50..0x83 the surface descriptor that surface_init
 * writes and the band iterator advances. Offsets below are quoted against the
 * frame base, not against ESP -- the original's [esp+..] displacements move
 * as it pushes call arguments, which is a good way to misread it. */
/* Variant 5 of the eight. --cdrawscan showed variants 4-7 are the ones this
 * game actually calls; 0-3 are never reached, so porting 0x212C0 produced a
 * replacement that was never invoked -- which is indistinguishable from
 * success unless the call is counted.
 *
 * The variants differ in exactly two constants: the bytes per pixel written
 * to the descriptor, and whether the first field is halved. Variant 0 is the
 * 8-bit case (1, no shift); variant 5 is the 16-bit one. */
#define DRAW_FIGHT   0x00022010u
#define DRAW_BPP     2u
#define DRAW_SHIFT   1u
/* push esi/edi/ebp (three bytes) then `sub esp,0x84` (six) -- nine to a
 * clean boundary, so the five-byte detour needs four NOPs after it. */
#define DRAW_LEN     9
/* The flat-shaded polygon span filler: the simplest of the three live
 * self-modifying rasterisers, and the one --srcwatch caught writing the
 * composition buffer. Its two 0xAFFEAFFE sentinels are the destination row
 * stride and the edge-list stride, written into its own instruction stream
 * before the loop runs -- so in C they are simply locals. push ebp/esi/edi
 * then `mov esi,[eax+0x28]` is six bytes to a clean boundary; the patch
 * targets (0x1AB79, 0x1AB83) lie well past that, so detouring the entry
 * leaves the self-modification intact for the original to keep using. */
#define RASTER_FIGHT 0x0001AB03u
#define RASTER_LEN   6
#define CLIP_FIGHT   0x0002DB60u
#define BAND_FIGHT   0x00020780u
#define PIXBLIT_FIGHT 0x0001BFD0u
#define NOP_FIGHT    0x0002DB50u
/* push ecx / push esi / push edi / xor ecx,ecx -- exactly five bytes, so the
 * detour lands on a real instruction boundary with nothing to pad. */
#define BLIT_LEN   5

int g_cblit;                    /* 0 off, 1 C, 2 C+restride, 3 C+verify */
int g_cflush;                   /* 0 off, 1 C, 3 C+verify                */
int g_csurf;                    /* 0 off, 1 C, 3 C+verify                */
int g_cdraw;                    /* 0 off, 1 C, 3 C+verify                */
int g_cdrawscan;                /* --cdrawscan: which variant is live    */
int g_srcwatch;                 /* --srcwatch: find who fills the source  */
int g_rasterscan;               /* --rasterscan: which rasterisers run    */
int g_craster;                  /* 0 off, 1 C, 3 C+verify                 */

/* Guard-page slot owned by emu.c. Pointing it at the composition buffer
 * answers "who rasterises the 3D scene": the span sources say where that
 * buffer is, but only at runtime -- it is heap and moves between runs. */
extern void *g_listwatch_page;

static uint8_t *g_tramp;        /* original prologue + jmp back: callable */
static uint8_t *g_ftramp;       /* the same, for the flusher              */
static uint8_t *g_stramp;       /* the same, for surface_init             */
static uint8_t *g_dtramp;       /* the same, for the drawing primitive    */
static uint8_t *g_rtramp;       /* the same, for the flat rasteriser      */
static uint8_t *g_shadow, *g_cout;
static uint32_t g_verified, g_fverified, g_sverified, g_dverified, g_rverified;

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The row length the destination addresses were built with. The span builder
 * (orig 0x87BDD) computes base + y*stride + x*2 using the *composition
 * buffer's* stride, which is its width -- 640 pixels -- times two. At
 * 640x480 that is also the screen's pitch and everything lines up; at any
 * wider mode it does not, and the viewport shears.
 *
 * Measured, not assumed: at 800x600 consecutive rows in the live list were
 * 0x500 apart for both source and destination (see --fbwatch). */
#define BAKED_PITCH 1280u

/* One span. In restride mode the baked destination is unpicked back into
 * (row, column) using the pitch it was built with, and rebuilt against the
 * real screen pitch. Spans are longer than one row -- counts of 536 dwords
 * against a 1280-byte row -- so the copy is split at row boundaries, or the
 * tail of a long span would land a row early. */
static void span_copy(uint32_t dst, uint32_t src, uint32_t dwords)
{
    uint32_t fb    = (uint32_t)(uintptr_t)g_vram;
    uint32_t real  = (uint32_t)g_vid_pitch;
    uint32_t bytes = dwords * 4;

    if (g_cblit == 2 && real != BAKED_PITCH && real &&
        dst >= fb && dst < fb + g_vram_size) {
        uint32_t off = dst - fb;
        while (bytes) {
            uint32_t row   = off / BAKED_PITCH;
            uint32_t col   = off % BAKED_PITCH;
            uint32_t chunk = BAKED_PITCH - col;
            uint32_t nd;
            if (chunk > bytes) chunk = bytes;
            nd = fb + row * real + col;
            if (nd + chunk > fb + g_vram_size) return;   /* off the end */
            memcpy((void *)(uintptr_t)nd, (const void *)(uintptr_t)src, chunk);
            src += chunk; off += chunk; bytes -= chunk;
        }
        return;
    }
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, bytes);
}

/* A transcription of orig 0x394EB, structured the way the original branches
 * rather than the way it would be written from scratch, so the two can be
 * compared instruction-for-instruction if the verification ever disagrees. */
static uint8_t *cblit_spans(uint8_t *rec)
{
    int16_t  c;
    uint16_t c2;

    c = (int16_t)((int16_t)rd16(rec + 0x0C) >> 2);
    if (c < 0) {
        c2 = rd16(rec + 0x0E);
        c  = (int16_t)-c;              /* the original negates before testing */
        if (c2 != 0) {
            span_copy(rd32(rec + 0x08),
                      rd32(rec + 0x00) + (uint32_t)(uint16_t)c * 4,
                      (uint16_t)((int16_t)c2 >> 2));
            rec += 0x14;
            goto next;
        }
    }

    for (;;) {
        span_copy(rd32(rec + 0x04), rd32(rec + 0x00), (uint16_t)c);
        rec += 0x14;
    next:
        c = (int16_t)((int16_t)rd16(rec + 0x0C) >> 2);
        if (c >= 0) continue;
        c2 = rd16(rec + 0x0E);
        if (c2 == 0) return rec;       /* also the list terminator */
        c = (int16_t)-c;
        span_copy(rd32(rec + 0x04), rd32(rec + 0x00), (uint16_t)c);
        return rec;
    }
}

/* Call the untouched original through the trampoline. Watcom register
 * convention: the record pointer arrives and returns in EAX. Everything is
 * read into registers before the pushes and written back after the pops, so
 * no local is touched while ESP is displaced. */
static uint8_t *orig_spans(uint8_t *rec)
{
    uint8_t *res;
    void *t = g_tramp;
    __asm {
        mov  eax, rec
        mov  edx, t
        push ebx
        push ecx
        push esi
        push edi
        call edx
        pop  edi
        pop  esi
        pop  ecx
        pop  ebx
        mov  res, eax
    }
    return res;
}

static uint8_t *__cdecl cblit_run(uint8_t *rec)
{
    uint8_t *r;

    if (g_srcwatch && !g_listwatch_page) {
        uint32_t src = rd32(rec + 0x00);
        if (src) {
            g_listwatch_page = (void *)((uintptr_t)src & ~(uintptr_t)0xFFF);
            LOGI("srcwatch: composition buffer near %08X; guarding page %p",
                 src, g_listwatch_page);
        }
    }

    if (g_cblit != 3 || !g_shadow) return cblit_spans(rec);

    /* Verification: run ours, capture the result, put the framebuffer back,
     * run the original over the same list, and compare. This proves
     * equivalence inside a single run, which two runs could not -- the scene
     * moves, so frame N of one run is not frame N of another. */
    memcpy(g_shadow, g_vram, g_vram_size);
    r = cblit_spans(rec);
    memcpy(g_cout, g_vram, g_vram_size);
    memcpy(g_vram, g_shadow, g_vram_size);
    {
        uint8_t *ro = orig_spans(rec);
        if (memcmp(g_vram, g_cout, g_vram_size) != 0 || ro != r) {
            static int shouted;
            if (!shouted++)
                LOGE("cblit: MISMATCH against the original (ret %p vs %p)",
                     (void *)r, (void *)ro);
        } else if (++g_verified == 200) {
            LOGI("cblit: 200 calls verified byte-identical to the original");
        }
        r = ro;                        /* leave the original's output standing */
    }
    return r;
}

/* Call whatever currently lives at the blitter's address -- the original, or
 * our thunk if --cblit installed one. Going through the address rather than
 * calling cblit_spans directly keeps the two levels independent: the flusher
 * can be ported and verified while the blitter underneath is either version. */
static uint8_t *call_blit(uint8_t *rec)
{
    uint8_t *res;
    void *f = gp(BLIT_FIGHT);
    __asm {
        mov  eax, rec
        mov  edx, f
        push ebx
        push ecx
        push esi
        push edi
        call edx
        pop  edi
        pop  esi
        pop  ecx
        pop  ebx
        mov  res, eax
    }
    return res;
}

static void call_setwindow(uint32_t win, uint32_t bank)
{
    void *f = gp(SETWIN_FIGHT);
    __asm {
        mov  eax, win
        mov  edx, bank
        mov  ecx, f
        push ebx
        push esi
        push edi
        call ecx
        pop  edi
        pop  esi
        pop  ebx
    }
}

/* orig 0x23040. The descriptor holds the span list at +4 and, from +8, one
 * signed bank byte per chunk; -1 means "no bank change needed". The currently
 * selected bank is cached in the video context at +0x309 so an unchanged bank
 * costs nothing. Each pass hands the list to the blitter, which returns where
 * it stopped -- at a bank boundary or at the terminator. */
static void cflush_spans(uint8_t *ctx, const uint8_t *desc)
{
    uint8_t      *list  = (uint8_t *)(uintptr_t)rd32(desc + 4);
    const int8_t *banks = (const int8_t *)(desc + 8);

    for (;;) {
        int32_t cur;
        int     bank;

        /* movsx of the count word compared against 0xFFFF8000: the list ends
         * at a count of exactly 0x8000. Checked before any shifting. */
        if (rd16(list + 0x0C) == 0x8000) return;

        bank = *banks++;
        memcpy(&cur, ctx + 0x309, 4);
        if (bank != -1 && bank != cur) {
            call_setwindow(0, (uint32_t)bank);
            call_setwindow(1, (uint32_t)bank);
            cur = bank;
            memcpy(ctx + 0x309, &cur, 4);
        }
        list = call_blit(list);
    }
}

static void orig_flush(uint8_t *ctx, const uint8_t *desc)
{
    void *t = g_ftramp;
    __asm {
        mov  eax, ctx
        mov  edx, desc
        mov  ecx, t
        push ebx
        push esi
        push edi
        call ecx
        pop  edi
        pop  esi
        pop  ebx
    }
}

static void __cdecl cflush_run(uint8_t *ctx, const uint8_t *desc)
{
    int32_t c_bank, o_bank;

    if (g_cflush != 3 || !g_shadow) { cflush_spans(ctx, desc); return; }

    /* Same shadow-and-compare as the blitter, plus the cached bank in the
     * context: this level has a side effect beyond the framebuffer. */
    memcpy(g_shadow, g_vram, g_vram_size);
    cflush_spans(ctx, desc);
    memcpy(g_cout, g_vram, g_vram_size);
    memcpy(&c_bank, ctx + 0x309, 4);
    memcpy(g_vram, g_shadow, g_vram_size);

    orig_flush(ctx, desc);
    memcpy(&o_bank, ctx + 0x309, 4);
    if (memcmp(g_vram, g_cout, g_vram_size) != 0 || c_bank != o_bank) {
        static int shouted;
        if (!shouted++)
            LOGE("cflush: MISMATCH against the original (bank %d vs %d)",
                 (int)c_bank, (int)o_bank);
    } else if (++g_fverified == 200) {
        LOGI("cflush: 200 calls verified byte-identical to the original");
    }
}

static __declspec(naked) void cflush_thunk(void)
{
    __asm {
        push ebx
        push ecx
        push esi
        push edi
        push edx                       /* descriptor */
        push eax                       /* video context */
        call cflush_run
        add  esp, 8
        pop  edi
        pop  esi
        pop  ecx
        pop  ebx
        ret
    }
}

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t call_map_lfb(const uint8_t *mi)
{
    uint32_t res;
    void *f = gp(MAPLFB_FIGHT);
    __asm {
        mov  eax, mi
        mov  edx, f
        push ebx
        push ecx
        push esi
        push edi
        call edx
        pop  edi
        pop  esi
        pop  ecx
        pop  ebx
        mov  res, eax
    }
    return res;
}

/* orig 0x206D0. Bit 7 of ModeAttributes says the mode has a linear
 * framebuffer; with one there is no window to describe, so window size and
 * granularity are zero and the base comes from mapping the LFB. Without one
 * the base is the real-mode window segment shifted left four, and the window
 * geometry is filled in from the mode info.
 *
 * Both paths take the stride verbatim from BytesPerScanLine, and the bytes
 * per pixel is (bpp + 1) / 8 -- the original spells that as a signed divide,
 * but bpp + 1 is never negative here. */
static void csurface_init(uint8_t *out, const uint8_t *mi)
{
    uint32_t base;

    if ((rd16(mi + 0x00) & 0x80) == 0x80) {
        wr32(out + 0x20, 0);
        wr32(out + 0x24, 0);
        wr32(out + 0x28, (uint32_t)(int32_t)(int16_t)rd16(mi + 0x10));
        base = call_map_lfb(mi);
    } else {
        int32_t winsz = (int16_t)rd16(mi + 0x06);
        int32_t gran  = (int16_t)rd16(mi + 0x04);
        wr32(out + 0x20, (uint32_t)(winsz << 10));
        /* The original divides without checking; a zero granularity would
         * fault there too, so this only avoids taking the process down. */
        wr32(out + 0x24, (uint32_t)(gran ? winsz / gran : 0));
        wr32(out + 0x28, (uint32_t)(int32_t)(int16_t)rd16(mi + 0x10));
        base = (uint32_t)rd16(mi + 0x08) << 4;
    }
    wr32(out + 0x2C, base);
    wr32(out + 0x30, ((uint32_t)mi[0x19] + 1) >> 3);
}

static void orig_surface(uint8_t *dsc, const uint8_t *mi)
{
    void *t = g_stramp;
    __asm {
        mov  eax, dsc
        mov  edx, mi
        mov  ecx, t
        push ebx
        push esi
        push edi
        call ecx
        pop  edi
        pop  esi
        pop  ebx
    }
}

static void __cdecl csurf_run(uint8_t *dsc, const uint8_t *mi)
{
    /* The descriptor is small, so verification compares the struct itself
     * rather than the framebuffer -- a tighter check than either level
     * below, and it catches a wrong field even when nothing is drawn yet. */
    uint8_t saved[0x40], cres[0x40];

    if (g_csurf != 3) { csurface_init(dsc, mi); return; }

    memcpy(saved, dsc, sizeof saved);
    csurface_init(dsc, mi);
    memcpy(cres, dsc, sizeof cres);
    memcpy(dsc, saved, sizeof saved);

    orig_surface(dsc, mi);
    if (memcmp(dsc, cres, sizeof cres) != 0) {
        static int shouted;
        if (!shouted++) {
            int k;
            LOGE("csurf: MISMATCH against the original");
            for (k = 0x20; k < 0x34; k += 4)
                LOGE("   +0x%02X  C=%08X  orig=%08X", k,
                     *(uint32_t *)(cres + k), *(uint32_t *)(dsc + k));
        }
    } else if (++g_sverified == 50) {
        LOGI("csurf: 50 calls verified identical to the original");
    }
}

static __declspec(naked) void csurf_thunk(void)
{
    __asm {
        push ebx
        push ecx
        push esi
        push edi
        push edx                       /* ModeInfoBlock */
        push eax                       /* surface descriptor */
        call csurf_run
        add  esp, 8
        pop  edi
        pop  esi
        pop  ecx
        pop  ebx
        ret
    }
}

/* --- the drawing primitive's helpers, called as the originals ---------- */

static int32_t call_clip(void *frame, uint32_t a3, uint32_t a4, uint32_t sa1,
                         uint32_t p_first, uint32_t p_second, uint32_t p_third)
{
    int32_t res;
    void *fn = gp(CLIP_FIGHT);
    __asm {
        push ebx
        push esi
        push edi
        push p_first                   /* the original pushes 4th, 3rd, 2nd */
        push p_second
        push p_third
        mov  eax, frame
        mov  edx, a3
        mov  ebx, a4
        mov  ecx, sa1
        call fn                        /* cleans its own three arguments */
        pop  edi
        pop  esi
        pop  ebx
        mov  res, eax
    }
    return res;
}

static int32_t call_band(void *surf)
{
    int32_t res;
    void *fn = gp(BAND_FIGHT);
    __asm {
        mov  eax, surf
        push ebx
        push ecx
        push edx
        push esi
        push edi
        call fn
        pop  edi
        pop  esi
        pop  edx
        pop  ecx
        pop  ebx
        mov  res, eax
    }
    return res;
}

static void call_pixblit(uint32_t eax_, uint32_t edx_, uint32_t ebx_,
                         uint32_t ecx_, uint32_t p1, uint32_t p2, uint32_t p3)
{
    void *fn = gp(PIXBLIT_FIGHT);
    __asm {
        push esi
        push edi
        push p1
        push p2
        push p3
        mov  eax, eax_
        mov  edx, edx_
        mov  ebx, ebx_
        mov  ecx, ecx_
        call fn                        /* ret 0xC: cleans its own arguments */
        pop  edi
        pop  esi
    }
}

/* orig 0x212C0. */
static void cdraw_prim(uint8_t *ctx, uint8_t *obj, uint32_t a3, uint32_t a4,
                       uint32_t sa1, uint32_t sa2, uint32_t sa3, uint32_t sa4)
{
    uint8_t  f[0x84];
    uint32_t inner, src, step;

    memset(f, 0, sizeof f);

    /* 0x212E3..0x21323: describe the source and the destination extent. The
     * width and height come from the mode info the game already holds, so a
     * mode change is picked up here without anything else being told. */
    inner = rd32(obj);
    wr32(f + 0x00, rd32((const uint8_t *)(uintptr_t)inner + 4) >> DRAW_SHIFT);
    wr32(f + 0x04, rd32((const uint8_t *)(uintptr_t)inner + 4));
    wr32(f + 0x08, rd32((const uint8_t *)(uintptr_t)inner + 8));
    wr32(f + 0x0C, DRAW_BPP);
    wr32(f + 0x10, (uint32_t)(int32_t)(int16_t)rd16(ctx + 0x212));   /* XRes  */
    wr32(f + 0x14, (uint32_t)(int32_t)(int16_t)rd16(ctx + 0x210));   /* pitch */
    wr32(f + 0x1C, DRAW_BPP);
    wr32(f + 0x18, (uint32_t)(int32_t)(int16_t)rd16(ctx + 0x214));   /* YRes  */

    if (call_clip(f, a3, a4, sa1, sa4, sa3, sa2) == 0) return;

    /* 0x21354: the surface the bands are walked over. Goes through the
     * patched address, so this uses the C surface_init when that is enabled. */
    {
        void *sfn = gp(SURF_FIGHT);
        uint8_t *sd = f + 0x50;
        uint8_t *mi = ctx + 0x200;
        __asm {
            mov  eax, sd
            mov  edx, mi
            mov  ecx, sfn
            push ebx
            push esi
            push edi
            call ecx
            pop  edi
            pop  esi
            pop  ebx
        }
    }

    wr32(f + 0x50, 0); wr32(f + 0x54, 0);
    wr32(f + 0x58, 0); wr32(f + 0x5C, 0);
    wr32(f + 0x64, rd32(f + 0x3C));
    wr32(f + 0x68, rd32(f + 0x40));
    wr32(f + 0x60, 0);
    wr32(f + 0x6C, rd32(f + 0x4C));

    step = rd32(f + 0x34);
    src  = rd32(obj + 0x24);

    for (;;) {
        int32_t  bank;
        uint32_t lfb;

        src += step;                                   /* 0x2139C */

        if (call_band(f + 0x50) != 1) return;          /* 0x213A2 */

        /* 0x213B0: with a linear framebuffer there is no window to page, and
         * the game counts the nesting in the context instead. */
        lfb = (rd16(ctx + 0x200) & 0x80) == 0x80;
        if (!lfb) {
            int32_t d; memcpy(&d, ctx + 0x30D, 4); d++; memcpy(ctx + 0x30D, &d, 4);
        }

        memcpy(&bank, f + 0x50, 4);
        if (bank != -1) {
            int32_t cur; memcpy(&cur, ctx + 0x309, 4);
            if (bank != cur) {
                call_setwindow(0, (uint32_t)bank);
                call_setwindow(1, (uint32_t)bank);
                memcpy(ctx + 0x309, &bank, 4);
            }
        }

        /* Each push moves ESP, so the original's displacements name
         * different frame slots as it goes: +0x48, then +0x30, +0x5C for the
         * pushed arguments, and +0x54 / +0x58 for EBX / ECX. */
        call_pixblit(0x15E244u + (uint32_t)g_img.delta, src,
                     rd32(f + 0x54), rd32(f + 0x58),
                     rd32(f + 0x48), rd32(f + 0x30), rd32(f + 0x5C));

        if (!lfb) {
            int32_t d; memcpy(&d, ctx + 0x30D, 4); d--; memcpy(ctx + 0x30D, &d, 4);
        }

        /* 0x2144F: how far the source advances before the next band. */
        step = rd32(f + 0x58);
        if (step == rd32(f + 0x24))
            step = rd32(f + 0x5C) * rd32(f + 0x04);
        else if (rd32(f + 0x60) != 1)
            step += rd32(f + 0x30);
    }
}

static void orig_draw(uint8_t *ctx, uint8_t *obj, uint32_t a3, uint32_t a4,
                      uint32_t sa1, uint32_t sa2, uint32_t sa3, uint32_t sa4)
{
    void *t = g_dtramp;
    __asm {
        push ebx
        push esi
        push edi
        push sa4
        push sa3
        push sa2
        push sa1
        mov  eax, ctx
        mov  edx, obj
        mov  ebx, a3
        mov  ecx, a4
        call t                         /* ret 0x10: cleans its own arguments */
        pop  edi
        pop  esi
        pop  ebx
    }
}

static void __cdecl cdraw_run(uint8_t *ctx, uint8_t *obj, uint32_t a3,
                              uint32_t a4, uint32_t sa1, uint32_t sa2,
                              uint32_t sa3, uint32_t sa4)
{
    if (g_cdraw != 3 || !g_shadow) {
        cdraw_prim(ctx, obj, a3, a4, sa1, sa2, sa3, sa4);
        return;
    }
    memcpy(g_shadow, g_vram, g_vram_size);
    cdraw_prim(ctx, obj, a3, a4, sa1, sa2, sa3, sa4);
    memcpy(g_cout, g_vram, g_vram_size);
    memcpy(g_vram, g_shadow, g_vram_size);

    orig_draw(ctx, obj, a3, a4, sa1, sa2, sa3, sa4);
    if (memcmp(g_vram, g_cout, g_vram_size) != 0) {
        static int shouted;
        if (!shouted++) LOGE("cdraw: MISMATCH against the original");
    } else if (++g_dverified <= 3 || (g_dverified % 25) == 0) {
        LOGI("cdraw: %u call(s) verified byte-identical to the original",
             g_dverified);
    }
}

static __declspec(naked) void cdraw_thunk(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov  ebp, esp                  /* +16 return address, +20.. arguments */
        push dword ptr [ebp + 32]      /* sa4 */
        push dword ptr [ebp + 28]      /* sa3 */
        push dword ptr [ebp + 24]      /* sa2 */
        push dword ptr [ebp + 20]      /* sa1 */
        push ecx                       /* a4  */
        push ebx                       /* a3  */
        push edx                       /* obj */
        push eax                       /* ctx */
        call cdraw_run
        add  esp, 32
        pop  ebp
        pop  edi
        pop  esi
        pop  ebx
        ret  16
    }
}

static __declspec(naked) void cblit_thunk(void)
{
    __asm {
        mov  eax, eax                  /* record pointer already in EAX */
        push ebx
        push ecx
        push edx
        push esi
        push edi
        push eax
        call cblit_run
        add  esp, 4
        pop  edi
        pop  esi
        pop  edx
        pop  ecx
        pop  ebx
        ret
    }
}

/* --cdrawscan: which of the eight drawing primitives does this scene use?
 *
 * They are format-specific variants dispatched indirectly, so static reading
 * does not say which one is live -- and porting the wrong one produces a
 * replacement that is never called, which looks exactly like success. Each
 * gets a counting detour that falls through to the untouched original. */
static const uint32_t g_draw_variants[8] = {
    0x000212C0u, 0x000214A0u, 0x00021680u, 0x00021C30u,
    0x00021E20u, 0x00022010u, 0x000221F0u, 0x000223D0u
};
static volatile LONG g_draw_hits[8];

void __cdecl cdrawscan_hit(uint32_t idx)
{
    if (idx >= 8) return;
    /* Report the first call to each variant as it happens: a run ended by a
     * timeout never reaches the exit-time summary. */
    if (InterlockedIncrement(&g_draw_hits[idx]) == 1)
        LOGI("cdrawscan: variant %u (orig 0x%08X) is live",
             idx, g_draw_variants[idx]);
}

void cdrawscan_report(void)
{
    int i, any = 0;
    if (!g_cdrawscan) return;
    for (i = 0; i < 8; i++)
        if (g_draw_hits[i]) {
            LOGI("cdrawscan: variant %d (orig 0x%08X) called %ld time(s)",
                 i, g_draw_variants[i], g_draw_hits[i]);
            any = 1;
        }
    if (!any) LOGI("cdrawscan: none of the eight variants was called");
}

static void cdrawscan_install(void)
{
    /* Same shape as calltrace.c: save the displaced prologue, log, replay it,
     * jump back. Nine bytes for all eight -- they share a prologue. */
    uint8_t *buf = (uint8_t *)VirtualAlloc(NULL, 8 * 64, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    int i, k;
    if (!buf) { LOGE("cdrawscan: alloc failed"); return; }
    for (i = 0; i < 8; i++) {
        uint8_t *p = (uint8_t *)gp(g_draw_variants[i]);
        uint8_t *w = buf + i * 64;
        uint8_t *slot = w;
        int32_t rel;
        *w++ = 0x60;                                   /* pushad            */
        *w++ = 0x9C;                                   /* pushfd            */
        *w++ = 0x68; memcpy(w, &i, 4); w += 4;         /* push idx          */
        *w++ = 0xE8;
        rel = (int32_t)((uint8_t *)(void *)cdrawscan_hit - (w + 4));
        memcpy(w, &rel, 4); w += 4;
        *w++ = 0x83; *w++ = 0xC4; *w++ = 0x04;         /* add esp,4         */
        *w++ = 0x9D;                                   /* popfd             */
        *w++ = 0x61;                                   /* popad             */
        memcpy(w, p, DRAW_LEN); w += DRAW_LEN;         /* replayed prologue */
        *w++ = 0xE9;
        rel = (int32_t)((p + DRAW_LEN) - (w + 4));
        memcpy(w, &rel, 4);
        p[0] = 0xE9;
        rel = (int32_t)(slot - (p + 5));
        memcpy(p + 1, &rel, 4);
        for (k = 5; k < DRAW_LEN; k++) p[k] = 0x90;
    }
    LOGI("cdrawscan: counting detours on all 8 drawing primitives");
}

/* --rasterscan: which of the 3D rasterisers does the game actually run?
 *
 * There are 19 functions carrying 0xAFFEAFFE patch sentinels -- self-modifying
 * span fillers that write strides and colours straight into their own
 * instruction stream before running the loop. That is a family of flat,
 * shaded, textured and transparent variants, and only some are reachable with
 * a given CONFIG.DES. Counting them is the only honest way to know which are
 * worth porting: a replacement for a dead one verifies perfectly and does
 * nothing, which is the trap the drawing-primitive port already fell into.
 *
 * Each entry carries its own safe prologue length, measured from real
 * instruction boundaries, because these do not share a prologue the way the
 * eight drawing primitives did. */
static const struct { uint32_t addr; int len; } g_rasters[] = {
    {0x0176ECu,10}, {0x017DF0u,10}, {0x01867Cu, 5}, {0x018961u, 6},
    {0x018A43u, 7}, {0x018C2Cu, 5}, {0x018F3Cu, 6}, {0x01922Cu, 5},
    {0x019499u, 6}, {0x019A3Au, 7}, {0x019CE0u, 5}, {0x019FE5u, 6},
    {0x01A2E0u, 5}, {0x01A6A8u, 5}, {0x01AB03u, 6}
};
#define NRASTER ((int)(sizeof g_rasters / sizeof g_rasters[0]))
static volatile LONG g_raster_hits[NRASTER];

void __cdecl rasterscan_hit(uint32_t idx)
{
    if (idx >= (uint32_t)NRASTER) return;
    if (InterlockedIncrement(&g_raster_hits[idx]) == 1)
        LOGI("rasterscan: 0x%08X is live", g_rasters[idx].addr);
}

static void rasterscan_install(void)
{
    uint8_t *buf = (uint8_t *)VirtualAlloc(NULL, NRASTER * 64,
                                           MEM_RESERVE | MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    int i, k;
    if (!buf) { LOGE("rasterscan: alloc failed"); return; }
    for (i = 0; i < NRASTER; i++) {
        uint8_t *p = (uint8_t *)gp(g_rasters[i].addr);
        uint8_t *w = buf + i * 64, *slot = w;
        int len = g_rasters[i].len;
        int32_t rel;
        *w++ = 0x60; *w++ = 0x9C;                      /* pushad / pushfd   */
        *w++ = 0x68; memcpy(w, &i, 4); w += 4;
        *w++ = 0xE8;
        rel = (int32_t)((uint8_t *)(void *)rasterscan_hit - (w + 4));
        memcpy(w, &rel, 4); w += 4;
        *w++ = 0x83; *w++ = 0xC4; *w++ = 0x04;
        *w++ = 0x9D; *w++ = 0x61;                      /* popfd / popad     */
        memcpy(w, p, len); w += len;
        *w++ = 0xE9;
        rel = (int32_t)((p + len) - (w + 4));
        memcpy(w, &rel, 4);
        p[0] = 0xE9;
        rel = (int32_t)(slot - (p + 5));
        memcpy(p + 1, &rel, 4);
        for (k = 5; k < len; k++) p[k] = 0x90;
    }
    LOGI("rasterscan: counting detours on %d self-modifying rasterisers",
         NRASTER);
}

/* orig 0x1AB03. Walks a linked edge list once per destination row, filling
 * the span between each node and its successor whenever the successor's key
 * at +8 matches, then steps both the list pointer and the destination down by
 * their strides. `mode == 1` is the 8-bit case; anything else doubles both
 * strides and halves the row count, which is the 16-bit variant.
 *
 * The alignment dance is transcribed as-is: an odd leading pixel, then dwords
 * through `rep stosd`, then an odd trailing one. The original writes the
 * leading pixel without first checking the count is non-zero, and that is
 * kept -- diverging there would make the comparison meaningless. */
static void craster_flat(uint8_t *ctx, uint32_t mode, uint32_t key,
                         uint32_t colour, const uint8_t *spanlist)
{
    uint32_t stride = rd32(ctx + 0x28);
    uint32_t rows   = rd32(ctx + 0x18);
    uint32_t step   = 0xC;
    uint32_t fill;
    uint8_t *dst;

    if (mode != 1) { rows >>= 1; stride += stride; step += step; }

    dst  = (uint8_t *)(uintptr_t)rd32(ctx + 0x24);
    fill = (colour << 16) | (colour & 0xFFFFu);

    while (rows) {
        const uint8_t *cur = spanlist;
        for (;;) {
            const uint8_t *nxt = (const uint8_t *)(uintptr_t)rd32(cur + 4);
            uint8_t *p;
            uint32_t cnt;

            if (!nxt) break;
            if (rd32(nxt + 8) != key) { cur = nxt; continue; }

            cnt = rd32(nxt) - rd32(cur);
            p   = dst + rd32(cur) * 2;

            if (((uintptr_t)p & 2) != 0) {
                memcpy(p, &fill, 2); p += 2;
                if (--cnt == 0) { cur = nxt; continue; }
            }
            {
                uint32_t dw = cnt >> 1, k;
                for (k = 0; k < dw; k++) { memcpy(p, &fill, 4); p += 4; }
                if (cnt & 1) { memcpy(p, &fill, 2); p += 2; }
            }
            cur = nxt;
        }
        spanlist += step;
        dst      += stride;
        rows--;
    }
}

static void orig_raster(uint8_t *ctx, uint32_t mode, uint32_t key,
                        uint32_t colour, const uint8_t *spanlist)
{
    void *t = g_rtramp;
    __asm {
        mov  eax, spanlist
        push eax                       /* the one stack argument */
        mov  eax, ctx
        mov  edx, mode
        mov  ebx, key
        mov  ecx, colour
        mov  esi, t
        call esi                       /* callee pops the argument (ret 4) */
    }
}

static void __cdecl craster_run(uint8_t *ctx, uint32_t mode, uint32_t key,
                                uint32_t colour, const uint8_t *spanlist)
{
    uint32_t stride, rows, span;
    uint8_t *dst;

    if (g_craster != 3) { craster_flat(ctx, mode, key, colour, spanlist); return; }

    /* The target here is the composition buffer, not the framebuffer, so the
     * region to compare is computed the same way the rasteriser walks it. */
    stride = rd32(ctx + 0x28);
    rows   = rd32(ctx + 0x18);
    if (mode != 1) { rows >>= 1; stride += stride; }
    dst  = (uint8_t *)(uintptr_t)rd32(ctx + 0x24);
    span = stride * rows;

    if (!dst || !span || span > (1u << 22)) {
        craster_flat(ctx, mode, key, colour, spanlist);
        return;
    }
    {
        uint8_t *save = (uint8_t *)malloc(span);
        uint8_t *cres = (uint8_t *)malloc(span);
        if (!save || !cres) {
            free(save); free(cres);
            craster_flat(ctx, mode, key, colour, spanlist);
            return;
        }
        memcpy(save, dst, span);
        craster_flat(ctx, mode, key, colour, spanlist);
        memcpy(cres, dst, span);
        memcpy(dst, save, span);

        orig_raster(ctx, mode, key, colour, spanlist);
        if (memcmp(dst, cres, span) != 0) {
            static int shouted;
            if (!shouted++) LOGE("craster: MISMATCH against the original");
        } else if (++g_rverified <= 3 || (g_rverified % 50) == 0) {
            LOGI("craster: %u call(s) verified byte-identical to the original",
                 g_rverified);
        }
        free(save); free(cres);
    }
}

static __declspec(naked) void craster_thunk(void)
{
    __asm {
        push ebp
        push esi
        push edi
        push ebx
        mov  edi, dword ptr [esp + 0x14]   /* the stack argument */
        push edi
        push ecx
        push ebx
        push edx
        push eax
        call craster_run
        add  esp, 20
        pop  ebx
        pop  edi
        pop  esi
        pop  ebp
        ret  4
    }
}

void cblit_install(void)
{
    uint8_t *p, *w;
    int32_t rel;

    if (g_cdrawscan && !g_desktop_mode) cdrawscan_install();
    if (g_rasterscan && !g_desktop_mode) rasterscan_install();
    if (!g_cblit && !g_cflush && !g_csurf && !g_cdraw && !g_craster) return;
    if (g_desktop_mode) {
        LOGW("cblit: 0x%08X is a FIGHT.EXE address; not installing in desktop",
             BLIT_FIGHT);
        return;
    }

    g_tramp  = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    g_ftramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    g_stramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    g_dtramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    g_rtramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    if (!g_tramp || !g_ftramp || !g_stramp || !g_dtramp) {
        LOGE("cblit: trampoline alloc failed"); return;
    }

    p = (uint8_t *)gp(BLIT_FIGHT);
    w = g_tramp;
    memcpy(w, p, BLIT_LEN); w += BLIT_LEN;
    *w++ = 0xE9;
    rel = (int32_t)((p + BLIT_LEN) - (w + 4));
    memcpy(w, &rel, 4);

    p = (uint8_t *)gp(FLUSH_FIGHT);
    w = g_ftramp;
    memcpy(w, p, FLUSH_LEN); w += FLUSH_LEN;
    *w++ = 0xE9;
    rel = (int32_t)((p + FLUSH_LEN) - (w + 4));
    memcpy(w, &rel, 4);

    p = (uint8_t *)gp(RASTER_FIGHT);
    w = g_rtramp;
    memcpy(w, p, RASTER_LEN); w += RASTER_LEN;
    *w++ = 0xE9;
    rel = (int32_t)((p + RASTER_LEN) - (w + 4));
    memcpy(w, &rel, 4);

    p = (uint8_t *)gp(SURF_FIGHT);
    w = g_stramp;
    memcpy(w, p, SURF_LEN); w += SURF_LEN;
    *w++ = 0xE9;
    rel = (int32_t)((p + SURF_LEN) - (w + 4));
    memcpy(w, &rel, 4);

    p = (uint8_t *)gp(DRAW_FIGHT);
    w = g_dtramp;
    memcpy(w, p, DRAW_LEN); w += DRAW_LEN;
    *w++ = 0xE9;
    rel = (int32_t)((p + DRAW_LEN) - (w + 4));
    memcpy(w, &rel, 4);

    if (g_cblit == 3 || g_cflush == 3 || g_cdraw == 3) {
        g_shadow = (uint8_t *)malloc(g_vram_size);
        g_cout   = (uint8_t *)malloc(g_vram_size);
        if (!g_shadow || !g_cout) {
            LOGE("cblit: verification buffers failed; dropping to mode 1");
            if (g_cblit == 3) g_cblit = 1;
            if (g_cflush == 3) g_cflush = 1;
        }
    }

    if (g_cblit) {
        p = (uint8_t *)gp(BLIT_FIGHT);
        p[0] = 0xE9;
        rel = (int32_t)((uint8_t *)(void *)cblit_thunk - (p + 5));
        memcpy(p + 1, &rel, 4);
        LOGI("cblit: span blitter at orig 0x%08X replaced (mode %d)",
             BLIT_FIGHT, g_cblit);
    }
    if (g_cdraw) {
        int k;
        p = (uint8_t *)gp(DRAW_FIGHT);
        p[0] = 0xE9;
        rel = (int32_t)((uint8_t *)(void *)cdraw_thunk - (p + 5));
        memcpy(p + 1, &rel, 4);
        for (k = 5; k < DRAW_LEN; k++) p[k] = 0x90;
        LOGI("cblit: drawing primitive at orig 0x%08X replaced (mode %d)",
             DRAW_FIGHT, g_cdraw);
    }
    if (g_craster) {
        int k;
        p = (uint8_t *)gp(RASTER_FIGHT);
        p[0] = 0xE9;
        rel = (int32_t)((uint8_t *)(void *)craster_thunk - (p + 5));
        memcpy(p + 1, &rel, 4);
        for (k = 5; k < RASTER_LEN; k++) p[k] = 0x90;
        LOGI("cblit: flat rasteriser at orig 0x%08X replaced (mode %d)",
             RASTER_FIGHT, g_craster);
    }
    if (g_csurf) {
        p = (uint8_t *)gp(SURF_FIGHT);
        p[0] = 0xE9;
        rel = (int32_t)((uint8_t *)(void *)csurf_thunk - (p + 5));
        memcpy(p + 1, &rel, 4);
        LOGI("cblit: surface_init at orig 0x%08X replaced (mode %d)",
             SURF_FIGHT, g_csurf);
    }
    if (g_cflush) {
        int k;
        p = (uint8_t *)gp(FLUSH_FIGHT);
        p[0] = 0xE9;
        rel = (int32_t)((uint8_t *)(void *)cflush_thunk - (p + 5));
        memcpy(p + 1, &rel, 4);
        for (k = 5; k < FLUSH_LEN; k++) p[k] = 0x90;
        LOGI("cblit: span flusher at orig 0x%08X replaced (mode %d)",
             FLUSH_FIGHT, g_cflush);
    }
}
