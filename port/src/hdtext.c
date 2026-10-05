/* Crisp text for the 3dfx build: the game's bitmap-font glyphs are drawn
 * again with a real font at the frame buffer's resolution.
 *
 * The game draws text one glyph at a time through its 16-bit RLE sprite
 * blitter (FIGHT3 orig 0x42492; found by its code, so DESKTOP3's copy is
 * hooked too), the same one it uses for every small sprite, into either the LFB
 * or an off-screen buffer that is later written with grLfbWriteRegion (the
 * cockpit monitors). The blitter gets no character code, so glyphs are told
 * apart by shape: every glyph of every GFX/FNT/*.FNT has its opaque-pixel mask
 * hashed at startup, and a sprite whose mask matches one is that character.
 * A sprite that merely looks like, say, a '-' is drawn as one, which is what
 * it was anyway.
 *
 * Each match is recorded with where it went (address, stride), its mask and
 * its colour per row. glide.c takes the records for a buffer when that buffer
 * reaches the frame buffer, fills the glyph's own pixels from the background
 * beside them and draws the character with GDI's outline font on top, fitted
 * to the original glyph's ink box and tinted with its colours.
 */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <GL/gl.h>

#define GMAX       16             /* largest glyph cell handled            */
#define NREC       8192           /* power of two */
#define NTAB       8192

typedef struct { uint32_t key; uint8_t ch, w, h, ambiguous; } tab_ent;
static tab_ent g_tab[NTAB];
static int     g_tab_n;

/* What was drawn where. A glyph stays known for as long as its pixels are
 * still in the buffer: a buffer the game writes text into once (the message
 * line) is uploaded again every frame. Indexed by destination address, two
 * ways; a glyph drawn at the same address again replaces the old record. */
static struct { hdtext_glyph g; uint32_t addr, stride, sum, seq; } g_rec[NREC];
static uint32_t g_seq, g_frame, g_n_blits, g_n_match, g_n_drawn;
int             g_hdtext = 1;              /* --nohdtext turns it off */

static uint32_t rec_slot(uint32_t addr)    /* the even one of two */
{
    return (((addr >> 1) * 2654435761u) >> 19) & (NREC - 2);
}

/* FNV over the glyph's pixels in raster order, as the blitter wrote them. */
static uint32_t pix_sum(const uint8_t *p, uint32_t stride, const hdtext_glyph *g)
{
    uint32_t k = 2166136261u;
    int x, y;
    for (y = 0; y < g->h; y++)
        for (x = 0; x < g->w; x++)
            if (g->mask[y * g->w + x])
                k = (k ^ ((const uint16_t *)(p + (size_t)y * stride))[x]) * 16777619u;
    return k;
}

static uint32_t mask_key(const uint8_t *m, int w, int h)
{
    uint32_t k = 2166136261u ^ (uint32_t)(w * 37 + h);
    int i;
    for (i = 0; i < w * h; i++) k = (k ^ m[i]) * 16777619u;
    return k ? k : 1;
}

static int upper(int ch)
{
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    return ch == 0x84 ? 0x8E : ch == 0x94 ? 0x99 : ch == 0x81 ? 0x9A : ch;   /* CP437 */
}

static void tab_add(uint32_t key, int ch, int w, int h)
{
    uint32_t i = key % NTAB, n;
    for (n = 0; n < NTAB; n++, i = (i + 1) % NTAB) {
        if (!g_tab[i].key) {
            g_tab[i].key = key; g_tab[i].ch = (uint8_t)ch;
            g_tab[i].w = (uint8_t)w; g_tab[i].h = (uint8_t)h;
            g_tab_n++;
            return;
        }
        if (g_tab[i].key == key && g_tab[i].w == w && g_tab[i].h == h) {
            /* caps-only fonts reuse the capital for the small letter: that
             * one shape is the capital; anything else (O/0, I/1) is unclear */
            int a = upper(g_tab[i].ch), b = upper(ch);
            if (a == b) g_tab[i].ch = (uint8_t)a;
            else g_tab[i].ambiguous = 1;
            return;
        }
    }
}

static const tab_ent *tab_find(uint32_t key, int w, int h)
{
    uint32_t i = key % NTAB, n;
    for (n = 0; n < NTAB && g_tab[i].key; n++, i = (i + 1) % NTAB)
        if (g_tab[i].key == key && g_tab[i].w == w && g_tab[i].h == h)
            return g_tab[i].ambiguous ? NULL : &g_tab[i];
    return NULL;
}

/* Only characters an outline font draws the way the bitmap font meant them;
 * symbols and box-drawing codes keep their original pixels. */
static int drawable(int ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') || (ch && strchr(".,:;-+/()[]!?%'\"<>=*#&_", ch)) ||
           ch == 0x8E || ch == 0x99 || ch == 0x9A || ch == 0x84 || ch == 0x94 ||
           ch == 0x81 || ch == 0xE1;                     /* CP437 umlauts, sz */
}

/* GFX/FNT format: see Schleichfahrt2 docs/formats/FNT.md. */
static void load_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;
    uint32_t p, g, first, count;
    uint16_t info;
    if (!f) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n || n < 18) { fclose(f); free(d); return; }
    fclose(f);
    memcpy(&info, d, 2); memcpy(&first, d + 2, 4); memcpy(&count, d + 6, 4);
    for (p = 18, g = 0; g < count && p + 20 <= (uint32_t)n; g++) {
        uint32_t w, h, y;
        uint8_t m[GMAX * GMAX];
        int ok = 1;
        memcpy(&w, d + p, 4); memcpy(&h, d + p + 4, 4);
        p += 20;
        if (w > GMAX || h > GMAX) ok = 0;
        memset(m, 0, sizeof m);
        for (y = 0; y < h; y++) {
            uint32_t x = 0;
            for (;;) {
                uint16_t skip, cnt;
                if (p + 4 > (uint32_t)n) { free(d); return; }
                memcpy(&skip, d + p, 2); memcpy(&cnt, d + p + 2, 2);
                p += 4;
                x += skip;
                if (ok) { uint32_t k; for (k = 0; k < cnt && x + k < w; k++) m[y * w + x + k] = 1; }
                x += cnt;
                p += info == 0x0402 ? cnt : info == 0x0414 ? cnt * 2u : 0;
                if (!cnt) break;
            }
        }
        if (ok && w && h && drawable((int)(first + g))) {
            int i, any = 0;
            for (i = 0; i < (int)(w * h); i++) any |= m[i];
            if (any) tab_add(mask_key(m, (int)w, (int)h), (int)(first + g), (int)w, (int)h);
        }
    }
    free(d);
}

/* Called from the blitter, before it runs: eax sprite, edx view, ebx x, ecx y.
 * sprite: +4 w, +8 h, +0Ch x offset, +10h y offset, +14h RLE (skip, count,
 * count RGB565 words; count 0 ends a row). view: +0 bitmap (+4 stride,
 * +0Ch pixels), +4/+8 origin and clip minimum, +0Ch/+10h clip maximum. */
static void __cdecl blit_seen(const uint32_t *spr, const uint32_t *view, int x, int y)
{
    hdtext_glyph *r;
    const uint16_t *s;
    const uint32_t *bm;
    const tab_ent *t;
    int w, h, gx, gy, row;
    uint32_t addr, i, sum = 2166136261u;
    uint8_t m[GMAX * GMAX];
    uint32_t rgb[GMAX][3], cnt[GMAX];

    if (!g_hdtext) return;
    g_n_blits++;
    w = (int)spr[1]; h = (int)spr[2];
    if (w <= 0 || h <= 0 || w > GMAX || h > GMAX) return;
    s = (const uint16_t *)(uintptr_t)spr[5];
    memset(m, 0, sizeof m);
    memset(rgb, 0, sizeof rgb); memset(cnt, 0, sizeof cnt);
    for (row = 0; row < h; row++) {
        int px = 0, guard = 0;
        for (;;) {
            int skip = s[0], c = s[1], k;
            s += 2;
            if (++guard > w + 1 || skip > w || c > w) return;   /* not RLE */
            px += skip;
            for (k = 0; k < c; k++) {
                uint16_t v = s[k];
                if (px + k >= w) return;
                m[row * w + px + k] = 1;
                sum = (sum ^ v) * 16777619u;
                rgb[row][0] += (v >> 11) & 31; rgb[row][1] += (v >> 5) & 63;
                rgb[row][2] += v & 31; cnt[row]++;
            }
            s += c; px += c;
            if (!c) break;
        }
    }
    t = tab_find(mask_key(m, w, h), w, h);
    if (!t) return;

    gx = x + (int)spr[3] + (int)view[1];
    gy = y + (int)spr[4] + (int)view[2];
    if (gx < (int)view[1] || gy < (int)view[2] ||
        gx + w > (int)view[3] || gy + h > (int)view[4])
        return;                    /* clipped (the blitter's own test): leave it */
    bm = (const uint32_t *)(uintptr_t)view[0];
    addr = bm[3] + (uint32_t)gy * bm[1] + (uint32_t)gx * 2u;

    g_n_match++;
    i = rec_slot(addr);
    if (g_rec[i].addr != addr &&
        (g_rec[i + 1].addr == addr ||
         (g_rec[i].addr && (!g_rec[i + 1].addr || g_rec[i + 1].seq < g_rec[i].seq))))
        i++;
    g_rec[i].addr = addr;
    g_rec[i].stride = bm[1];
    g_rec[i].sum = sum;
    g_rec[i].seq = ++g_seq;
    r = &g_rec[i].g;
    r->ch = t->ch; r->w = w; r->h = h;
    memcpy(r->mask, m, sizeof m);
    for (row = 0; row < h; row++) {
        uint32_t c = cnt[row] ? cnt[row] : 1;
        r->row_rgb[row] = cnt[row]
            ? (uint32_t)((rgb[row][0] * 255 / 31 / c) | ((rgb[row][1] * 255 / 63 / c) << 8) |
                         ((rgb[row][2] * 255 / 31 / c) << 16) | 0xFF000000u)
            : 0;
    }
}

/* The glyphs still in a w x h pixel (16-bit) buffer at buf, with x, y
 * relative to buf. Ones the game has drawn over since are forgotten. */
int hdtext_take(const void *buf, uint32_t stride, int w, int h, hdtext_glyph *out, int max)
{
    uint32_t lo = (uint32_t)(uintptr_t)buf, hi = lo + (uint32_t)h * stride;
    int i, n = 0;
    if (!g_hdtext) return 0;
    for (i = 0; i < NREC && n < max; i++) {
        uint32_t a = g_rec[i].addr;
        int x, y;
        if (!a || a < lo || a >= hi || g_rec[i].stride != stride) continue;
        x = (int)((a - lo) % stride) / 2;
        y = (int)((a - lo) / stride);
        if (x + g_rec[i].g.w > w || y + g_rec[i].g.h > h) continue;   /* not all here */
        if (pix_sum((const uint8_t *)buf + (size_t)y * stride + (size_t)x * 2, stride,
                    &g_rec[i].g) != g_rec[i].sum) {
            g_rec[i].addr = 0;                                     /* drawn over */
            continue;
        }
        out[n] = g_rec[i].g;
        out[n].x = x; out[n].y = y;
        n++;
    }
    return n;
}

void hdtext_frame(void)
{
    if (++g_frame % 1800 == 0 && g_n_blits)
        LOGI("hdtext: %u sprites, %u were glyphs, %u drawn", g_n_blits, g_n_match, g_n_drawn);
}

/* ---- the outline font: one alpha texture per character, from GDI ---- */

#define TEX_PX 40                   /* em height the glyphs are rendered at */
static struct { GLuint tex; float aspect; int done; } g_cache[256];   /* aspect: w / h */
static HDC     g_dc;
static HFONT   g_font;
static uint8_t *g_bits;
static HBITMAP g_bmp;

static int gdi_init(void)
{
    BITMAPINFO bi;
    char face[64];
    if (g_dc) return 1;
    g_dc = CreateCompatibleDC(NULL);
    if (!g_dc) return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = TEX_PX * 2;
    bi.bmiHeader.biHeight = -TEX_PX * 2;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    if (!g_bmp) return 0;
    SelectObject(g_dc, g_bmp);
    /* Bahnschrift (Windows 10+) is a DIN face, the look of instrument
     * lettering; Consolas wherever it is missing. */
    g_font = CreateFontA(-TEX_PX, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         FF_DONTCARE, "Bahnschrift");
    SelectObject(g_dc, g_font);
    GetTextFaceA(g_dc, sizeof face, face);
    if (_stricmp(face, "Bahnschrift")) {
        DeleteObject(g_font);
        g_font = CreateFontA(-TEX_PX, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH, "Consolas");
        SelectObject(g_dc, g_font);
        GetTextFaceA(g_dc, sizeof face, face);
    }
    SetTextColor(g_dc, RGB(255, 255, 255));
    SetBkColor(g_dc, RGB(0, 0, 0));
    SetBkMode(g_dc, OPAQUE);
    LOGI("hdtext: %d glyph shapes from GFX/FNT, drawn with %s", g_tab_n, face);
    return 1;
}

/* The character's ink, cropped, as a GL_ALPHA texture. */
static int glyph_tex(int ch)
{
    WCHAR wc;
    char c = (char)ch;
    RECT rc = { 0, 0, TEX_PX * 2, TEX_PX * 2 };
    int x, y, x0 = TEX_PX * 2, y0 = TEX_PX * 2, x1 = -1, y1 = -1, tw, th;
    uint8_t *a;
    if (g_cache[ch].done) return g_cache[ch].tex != 0;
    g_cache[ch].done = 1;
    if (!gdi_init()) return 0;
    if (!MultiByteToWideChar(437, 0, &c, 1, &wc, 1)) return 0;
    FillRect(g_dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    TextOutW(g_dc, TEX_PX / 4, TEX_PX / 4, &wc, 1);
    GdiFlush();
    for (y = 0; y < TEX_PX * 2; y++)
        for (x = 0; x < TEX_PX * 2; x++)
            if (g_bits[(y * TEX_PX * 2 + x) * 4 + 1] > 40) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return 0;
    tw = x1 - x0 + 1; th = y1 - y0 + 1;
    g_cache[ch].aspect = (float)tw / th;
    a = (uint8_t *)malloc((size_t)tw * th);
    if (!a) return 0;
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++)
            a[y * tw + x] = g_bits[((y0 + y) * TEX_PX * 2 + x0 + x) * 4 + 1];
    glGenTextures(1, &g_cache[ch].tex);
    glBindTexture(GL_TEXTURE_2D, g_cache[ch].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, tw, th, 0, GL_ALPHA, GL_UNSIGNED_BYTE, a);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    free(a);
    return 1;
}

/* Fill the glyph's own pixels in a 0xAABBGGRR image (px = the buffer's
 * origin) from the nearest pixel of its cell that is not glyph, alpha
 * included, so the low-resolution letter is gone from under the one drawn on
 * top. */
void hdtext_erase(uint32_t *px, int stride, const hdtext_glyph *g)
{
    int x, y, d;
    for (y = 0; y < g->h; y++) {
        uint32_t *row = px + (size_t)(g->y + y) * stride + g->x;
        const uint8_t *m = g->mask + y * g->w;
        for (x = 0; x < g->w; x++) {
            if (!m[x]) continue;
            for (d = 1; d < g->w; d++) {
                if (x - d >= 0 && !m[x - d]) { row[x] = row[x - d]; break; }
                if (x + d < g->w && !m[x + d]) { row[x] = row[x + d]; break; }
            }
        }
    }
}

/* Draw it at (x, y) in 640x480 space, fitted to the original's ink box,
 * coloured top to bottom like the original rows. Fixed-function state is
 * left as glide.c's overlay drawing leaves it. */
void hdtext_draw(const hdtext_glyph *g, float x, float y)
{
    int i, x0 = g->w, x1 = -1, y0 = g->h, y1 = -1, ytop = -1, ybot = -1;
    uint32_t ct = 0, cb = 0;
    float qx0, qx1;
    if (!glyph_tex(g->ch)) return;
    for (i = 0; i < g->w * g->h; i++)
        if (g->mask[i]) {
            int gx = i % g->w, gy = i / g->w;
            if (gx < x0) x0 = gx;
            if (gx > x1) x1 = gx;
            if (gy < y0) y0 = gy;
            if (gy > y1) y1 = gy;
        }
    for (i = 0; i < g->h; i++) if (g->row_rgb[i]) { if (ytop < 0) ytop = i; ybot = i; }
    if (x1 < 0 || ytop < 0) return;
    g_n_drawn++;
    ct = g->row_rgb[ytop]; cb = g->row_rgb[ybot];
    {   /* the original's ink box, except that a narrow character (I, 1, :)
         * keeps its own width there rather than turning into a block */
        float bw = (float)(x1 + 1 - x0), nat = g_cache[g->ch].aspect * (y1 + 1 - y0);
        float cx = x + (x0 + x1 + 1) * 0.5f;
        if (nat < 0.6f * bw) bw = nat < 1.0f ? 1.0f : nat;
        qx0 = cx - bw * 0.5f; qx1 = cx + bw * 0.5f;
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_cache[g->ch].tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
        glColor4ub(ct & 255, (ct >> 8) & 255, (ct >> 16) & 255, 255);
        glTexCoord2f(0, 0); glVertex3f(qx0, y + y0,     0);
        glTexCoord2f(1, 0); glVertex3f(qx1, y + y0,     0);
        glColor4ub(cb & 255, (cb >> 8) & 255, (cb >> 16) & 255, 255);
        glTexCoord2f(1, 1); glVertex3f(qx1, y + y1 + 1, 0);
        glTexCoord2f(0, 1); glVertex3f(qx0, y + y1 + 1, 0);
    glEnd();
    glColor4f(1, 1, 1, 1);
}

/* The blitter's entry (push esi/edi/es, ds -> es, sub esp,30h, mov esi,eax,
 * mov edi,edx, mov eax,[edi+14h], mov [esp+2Ch],eax); the 8-bit twin of it
 * starts the same, the 16-bit one adds x twice (add eax,[esp+14h] x2). */
static const uint8_t k_entry[23] = {
    0x56, 0x57, 0x06, 0x66, 0x8C, 0xDE, 0x66, 0x8E, 0xC6, 0x83, 0xEC, 0x30,
    0x8B, 0xF0, 0x8B, 0xFA, 0x8B, 0x47, 0x14, 0x89, 0x44, 0x24, 0x2C };
static const uint8_t k_x2[8] = { 0x03, 0x44, 0x24, 0x14, 0x03, 0x44, 0x24, 0x14 };
#define HOOK_AT 12                 /* mov esi,eax / mov edi,edx / mov eax,[edi+14h] */
#define HOOK_LEN 7

static void hook(uint8_t *p)
{
    uint8_t *w = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE), *slot = w;
    int32_t rel;
    if (!w) return;
    *w++ = 0x60;                                   /* pushad             */
    *w++ = 0x9C;                                   /* pushfd             */
    *w++ = 0x51;                                   /* push ecx  (y)      */
    *w++ = 0x53;                                   /* push ebx  (x)      */
    *w++ = 0x52;                                   /* push edx  (view)   */
    *w++ = 0x50;                                   /* push eax  (sprite) */
    *w++ = 0xE8;
    rel = (int32_t)((uint8_t *)(void *)blit_seen - (w + 4));
    memcpy(w, &rel, 4); w += 4;
    *w++ = 0x83; *w++ = 0xC4; *w++ = 0x10;         /* add esp,16         */
    *w++ = 0x9D;                                   /* popfd              */
    *w++ = 0x61;                                   /* popad              */
    memcpy(w, p, HOOK_LEN); w += HOOK_LEN;         /* replayed           */
    *w++ = 0xE9;
    rel = (int32_t)((p + HOOK_LEN) - (w + 4));
    memcpy(w, &rel, 4);
    p[0] = 0xE9;
    rel = (int32_t)(slot - (p + 5));
    memcpy(p + 1, &rel, 4);
    p[5] = 0x90; p[6] = 0x90;
}

void hdtext_install(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    uint8_t *p = g_img.mem, *end = g_img.mem + g_img.code_size - sizeof k_entry, *site = NULL;
    int n = 0;

    if (!g_hdtext || !g_glide_mode) return;
    for (; p < end; p++)
        if (p[0] == 0x56 && !memcmp(p, k_entry, sizeof k_entry)) {
            uint8_t *q;
            for (q = p; q < p + 0x100 && q < end; q++)
                if (!memcmp(q, k_x2, sizeof k_x2)) { site = p + HOOK_AT; n++; break; }
        }
    if (n != 1) {
        LOGW("hdtext: %d 16-bit sprite blitters found, need exactly one", n);
        return;
    }
    h = FindFirstFileA("GFX\\FNT\\*.FNT", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH];
            _snprintf(path, sizeof path, "GFX\\FNT\\%s", fd.cFileName);
            load_font(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!g_tab_n) { LOGW("hdtext: no fonts found"); return; }
    hook(site);
    LOGI("hdtext: %d glyph shapes, blitter hook at orig 0x%08X", g_tab_n,
         (uint32_t)(uintptr_t)site - g_img.delta);
}
