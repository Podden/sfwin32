/* Glide 2.x interception for the 3dfx build (FIGHT3.EXE / DESKTOP3.EXE).
 *
 * The 3dfx build talks to a Voodoo card through GLIDE2X.OVL, an LE library
 * module it loads at runtime. It does not link to it: object 2 ends with a
 * table of 130 five-byte `call rel32` stubs, one per imported Glide function,
 * every one of them calling the same resolver at orig 0xA7CEC. The resolver
 * loads the overlay on first use, looks the name up, and then overwrites the
 * stub that called it with a direct `jmp` to the real entry point, so each
 * import costs the indirection exactly once.
 *
 *     orig 0x10D08C   stub table, 5 bytes each (E8 rel32 -> 0xA7CEC)
 *     orig 0x10D318   parallel table of name pointers
 *     orig 0x148718   the overlay's module handle, preset to 0xFFFF
 *
 * That resolver cannot work here and should not: it asks the DOS/4GW extender
 * for a DLL-loading service this port does not provide (orig 0xFBA08 reads a
 * far pointer at offset 0x42 of an extender structure and returns failure),
 * and an unpatched run duly reports "Fatal error: unable to initialize DLL
 * loader" by way of the `int3` at orig 0xA7D70. Providing that service would
 * only earn the right to run the real Glide, which drives Voodoo hardware
 * over PCI and MMIO -- of no use on this host.
 *
 * So the overlay is never loaded. Every stub is rewritten at startup to jump
 * straight into this file, which is the same thing the resolver would have
 * done, just to a different destination. The DLL loader is then dead code.
 *
 * The names carry their own calling convention: Glide is stdcall and the
 * decoration spells out the argument bytes, so `_GRDRAWTRIANGLE@12` is three
 * dwords, callee-cleaned. That is parsed rather than tabulated, which is why
 * a function this port has never heard of still returns cleanly instead of
 * unbalancing the stack.
 */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <GL/gl.h>

/* All three tables are FIGHT3.EXE addresses. DESKTOP3.EXE is a separate link
 * and would need its own, so this installs in fight3 mode only. */
#define STUB_TABLE   0x0010D08Cu
#define NAME_TABLE   0x0010D318u
#define RESOLVER     0x000A7CECu
#define STUB_LEN     5
#define MAX_IMPORTS  256

int g_glide = 1;                /* --glide 0 off, 1 intercept (default)     */
uint32_t g_glide_frame;         /* grBufferSwap count, for frame tracing    */
int g_glidedebug;
int g_glidetrace;               /* --glidetrace N: trace frame N only       */

/* Glide's own 16-bit format is 5:6:5, which is also what this port's
 * framebuffer already is, so the linear-frame-buffer calls can hand the game
 * g_vram directly instead of copying through a staging surface. */
/* GrColorFormat_t from grSstWinOpen; used before the back end is set up. */
static int      g_color_fmt = 2;

#define GLIDE_W 640
#define GLIDE_H 480

typedef uint32_t (*glide_fn)(uint32_t *a);

typedef struct {
    const char *name;           /* points into the game's own image         */
    uint32_t    argbytes;       /* from the @N decoration                   */
    uint32_t    calls;
    glide_fn    fn;             /* NULL until implemented: logs, returns 0  */
} glide_import;

static glide_import g_imp[MAX_IMPORTS];
static uint32_t     g_nimp;
static uint8_t     *g_thunks;
static uint32_t     g_argbytes[MAX_IMPORTS];

/* Does this name look like one of the overlay's exports? The table is walked
 * until it stops doing so, which is how the import count is discovered rather
 * than assumed. */
static int plausible_export(const char *s)
{
    const char *p;
    if (!s || (s[0] != '_' && s[0] != '?')) return 0;
    for (p = s; *p; p++) {
        if (p - s > 64) return 0;
        if (*p == '@') return p[1] >= '0' && p[1] <= '9';
    }
    return 0;
}

static uint32_t parse_argbytes(const char *name)
{
    const char *at = strrchr(name, '@');
    return at ? (uint32_t)atoi(at + 1) : 0;
}

/* ------------------------------------------------------------ entry points
 *
 * Only the calls whose *return value or output buffer* the game reads need a
 * body; the pure state-setters can go on returning zero until there is a
 * renderer for them to configure. Getting that distinction wrong is what an
 * unimplemented run looks like -- the game took a null back from a query and
 * dereferenced it at orig 0x39745.
 */

static uint32_t gl_query_hardware(uint32_t *a)
{
    /* GrHwConfiguration: num_sst, then per-board type + VoodooConfig
     * (fbRam, fbiRev, nTexelfx, sliDetect, tmuConfig[3]{tmuRev,tmuRam}). */
    uint32_t *hw = (uint32_t *)(uintptr_t)a[0];
    if (!hw) return 0;
    memset(hw, 0, 4 + 44);
    hw[0] = 1;          /* num_sst                        */
    hw[1] = 0;          /* SSTs[0].type = GR_SSTTYPE_VOODOO */
    hw[2] = 2;          /* fbRam, MB                      */
    hw[3] = 2;          /* fbiRev                         */
    hw[4] = 1;          /* nTexelfx                       */
    hw[5] = 0;          /* sliDetect                      */
    hw[6] = 1;          /* tmuConfig[0].tmuRev            */
    hw[7] = 2;          /* tmuConfig[0].tmuRam, MB        */
    return 1;           /* FXTRUE                         */
}

static uint32_t gl_win_open(uint32_t *a)
{
    /* (hWnd, resolution, refresh, format, origin, nColBuf, nAuxBuf). Only
     * 640x480 is offered; the game asks for GR_RESOLUTION_640x480 (3). */
    g_color_fmt = (int)a[3];
    LOGI("glide: grSstWinOpen res=%u fmt=%u origin=%u col=%u aux=%u",
         a[1], a[3], a[4], a[5], a[6]);
    video_set_linear_mode(GLIDE_W, GLIDE_H, 16);
    return 1;
}

static uint32_t gl_screen_width(uint32_t *a)  { (void)a; return GLIDE_W; }
static uint32_t gl_screen_height(uint32_t *a) { (void)a; return GLIDE_H; }
static uint32_t gl_zero(uint32_t *a)          { (void)a; return 0; }

static uint32_t gl_tex_min_addr(uint32_t *a)  { (void)a; return 0; }
static uint32_t gl_tex_max_addr(uint32_t *a)  { (void)a; return 0x1FFFFFu; }

static uint32_t gl_glide_get_version(uint32_t *a)
{
    char *p = (char *)(uintptr_t)a[0];
    if (p) memcpy(p, "Glide 2.43", 11);
    return 0;
}

/* ------------------------------------------------------- OpenGL back end
 *
 * Glide's triangle pipeline, rendered with OpenGL. Everything below was
 * measured from a live frame (--glidetrace N plus the vertex probe) rather
 * than taken from a header:
 *
 *   GrVertex is 60 bytes -- the spacing between the three pointers
 *   grDrawTriangle receives -- so GLIDE_NUM_TMU is 2 and the layout is 15
 *   floats: x y z r g b ooz a oow, then two sets of (sow, tow, oow).
 *
 *   x,y are screen coordinates. The first quad of every frame is exactly
 *   (0,17)-(640,417), which is also what grClipWindow was given.
 *
 *   Texture coordinates are texel-space over w, so s = sow/oow. That maps
 *   onto OpenGL for free: glTexCoord4f(sow/size, tow/size, 0, oow) makes the
 *   hardware do the perspective divide it already does for q.
 *
 *   Textures are 256x256 GR_TEXFMT_P_8, which is why grTexDownloadTable --
 *   the palette -- is the second busiest call in the trace.
 *
 *   Depth is W-buffered (grDepthBufferMode 2), so depth is w = 1/oow, and
 *   nearer means smaller, which is what GR_CMP_LESS already expects.
 *
 * The one place this cannot be pure OpenGL is the frame buffer itself. The
 * game draws its cockpit and HUD by taking the raw pointer grLfbLock hands
 * back and writing 16-bit pixels straight into it, and a GL back buffer
 * cannot be handed out as a CPU pointer. The frame order settles it: the
 * trace shows every triangle is drawn before the first lock, so the 3D result
 * is read back once per frame, at the first lock, and the game's 2D then
 * composites over it exactly as it did on a Voodoo -- which is also what
 * makes grLfbReadRegion work without a special case.
 */

/* GrVertex, in floats */
#define VX 0
#define VY 1
#define VR 3
#define VG 4
#define VB 5
#define VA 7
#define VOOW 8
#define VSOW 9
#define VTOW 10

/* Not in the Windows GL 1.1 header, but every driver has had them for
 * decades; these are plain enumerants, not entry points. */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif

#define TEXCACHE 1024

typedef struct {
    uint32_t key;        /* 0 = empty */
    GLuint   tex;
} texcache_ent;

static SDL_GLContext g_glctx;
static int      g_gl_ready;
static texcache_ent g_texcache[TEXCACHE];
static GLuint   g_blit_tex;
static uint32_t g_pal_argb[256];
static uint32_t g_pal_hash = 1;
static int      g_pal_valid;
static const uint8_t *g_tex_data;   /* expanded lazily, at draw time */
static int      g_tex_size = 256;
static int      g_tex_bound;
static int      g_chroma_on;
static uint32_t g_chroma_val;
static int      g_cc_other = 1, g_cc_factor = 8, g_cc_local;
static uint32_t g_const_color = 0xFFFFFFFF;
static int      g_depth_mode, g_depth_mask = 1, g_depth_func = 7;
static uint32_t g_depth_bias;       /* grDepthBiasLevel, a 16-bit depth value */
static int      g_lfb_synced, g_lfb_dirty, g_frame_seeded;
/* GR_BUFFER_AUXBUFFER: the depth buffer, which this game writes through
 * the LFB to stamp its cockpit mask. */
static uint16_t g_auxbuf[GLIDE_W * GLIDE_H];
static int      g_aux_locked;
static int      g_clip_x0, g_clip_y0, g_clip_x1 = GLIDE_W, g_clip_y1 = GLIDE_H;
static uint32_t g_tex_hit, g_tex_miss, g_tris, g_texsrc_ok, g_texsrc_bad;

static uint32_t hash32(uint32_t h, uint32_t v)
{
    h ^= v; h *= 16777619u; return h;
}

/* GrColor_t packing depends on the colour format grSstWinOpen selected. This
 * game asks for GR_COLORFORMAT_RGBA (2), so a colour is 0xRRGGBBAA -- reading
 * it as 0x00RRGGBB turns the dark blue underwater background into green. */
static void glide_unpack(uint32_t c, float *r, float *g, float *b)
{
    uint32_t rr, gg, bb;
    switch (g_color_fmt) {
    case 1:  rr = (c >> 8) & 0xFF;  gg = (c >> 16) & 0xFF;   /* ABGR */
             bb = (c >> 24) & 0xFF; break;
    case 2:  rr = (c >> 24) & 0xFF; gg = (c >> 16) & 0xFF;   /* RGBA */
             bb = (c >> 8) & 0xFF;  break;
    case 3:  rr = (c >> 8) & 0xFF;  gg = (c >> 16) & 0xFF;   /* BGRA */
             bb = (c >> 24) & 0xFF; break;
    default: rr = (c >> 16) & 0xFF; gg = (c >> 8) & 0xFF;    /* ARGB */
             bb = c & 0xFF;         break;
    }
    *r = rr / 255.0f; *g = gg / 255.0f; *b = bb / 255.0f;
}

static GLenum depth_func_gl(int f)
{
    switch (f) {
    case 0:  return GL_NEVER;
    case 1:  return GL_LESS;
    case 2:  return GL_EQUAL;
    case 3:  return GL_LEQUAL;
    case 4:  return GL_GREATER;
    case 5:  return GL_NOTEQUAL;
    case 6:  return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

/* GR_BLEND_*: only the factors this game actually asks for are mapped; the
 * rest fall back to something visible rather than silently black. */
static GLenum blend_gl(int f)
{
    switch (f) {
    case 0:  return GL_ZERO;
    case 1:  return GL_SRC_ALPHA;
    case 2:  return GL_SRC_COLOR;
    case 3:  return GL_DST_ALPHA;
    case 4:  return GL_ONE;
    case 5:  return GL_ONE_MINUS_SRC_ALPHA;
    case 6:  return GL_ONE_MINUS_SRC_COLOR;
    case 7:  return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ONE;
    }
}

static int gl_ensure_context(void)
{
    if (g_gl_ready) return 1;
    if (!video_window()) return 0;

    g_glctx = SDL_GL_CreateContext(video_window());
    if (!g_glctx) { LOGE("glide: SDL_GL_CreateContext: %s", SDL_GetError());
                    return 0; }
    if (SDL_GL_MakeCurrent(video_window(), g_glctx) != 0) {
        LOGE("glide: SDL_GL_MakeCurrent: %s", SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(0);          /* the game paces itself */

    {   int db = -1, dw = 0, dh = 0;
        SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &db);
        SDL_GL_GetDrawableSize(video_window(), &dw, &dh);
        LOGI("glide: OpenGL %s / %s", (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_RENDERER));
        LOGI("glide: drawable %dx%d, double-buffered=%d", dw, dh, db);
    }

    glViewport(0, 0, GLIDE_W, GLIDE_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, GLIDE_W, GLIDE_H, 0, -1, 1);   /* y down, as Glide's origin */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glAlphaFunc(GL_GREATER, 0.5f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &g_blit_tex);
    g_gl_ready = 1;
    return 1;
}

/* Expand a paletted texture into RGBA and cache it. The key covers the
 * texture data, the palette and the chroma key, because all three change
 * independently and any of them changes the pixels. */
static GLuint tex_upload(const uint8_t *data, int size, uint32_t key)
{
    static uint32_t rgba[256 * 256];
    uint32_t slot = key % TEXCACHE;
    int i, n = size * size;
    GLuint id;

    if (g_texcache[slot].key == key) { g_tex_hit++; return g_texcache[slot].tex; }
    g_tex_miss++;

    if (n > 256 * 256) n = 256 * 256;
    for (i = 0; i < n; i++) {
        uint32_t c = g_pal_argb[data[i]];
        /* chroma-keyed texels become transparent and are dropped by the
         * alpha test, which is what Glide's chroma key does */
        rgba[i] = (g_chroma_on && (c & 0x00FFFFFF) == (g_chroma_val & 0x00FFFFFF))
                    ? (c & 0x00FFFFFF)
                    : (c | 0xFF000000);
    }

    if (g_texcache[slot].tex) id = g_texcache[slot].tex;
    else glGenTextures(1, &id);

    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    g_texcache[slot].key = key;
    g_texcache[slot].tex = id;
    return id;
}

/* Draw g_vram over the colour buffer without touching depth.
 *
 * The Glide frame buffer is persistent: the cockpit is painted once and only
 * the moving parts are redrawn, and the per-frame "clear" is a full-viewport
 * quad drawn with GR_CMP_NOTEQUAL -- which makes the *depth* buffer a cockpit
 * mask. Pixels the 3D never touches keep the far value the initial
 * grBufferClear left, so the quad is rejected there and the cockpit survives.
 * That only holds if GL's colour buffer really is the persistent frame
 * buffer, so it is seeded from g_vram once per frame before any triangle.
 * Depth is deliberately left alone: it has to carry across frames for the
 * mask to mean anything. */
static void gl_seed_color_from_vram(void)
{
    if (g_frame_seeded || !g_vram) return;
    g_frame_seeded = 1;

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_blit_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, GLIDE_W, GLIDE_H, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_vram);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(0,       0,       0);
        glTexCoord2f(1, 0); glVertex3f(GLIDE_W, 0,       0);
        glTexCoord2f(1, 1); glVertex3f(GLIDE_W, GLIDE_H, 0);
        glTexCoord2f(0, 1); glVertex3f(0,       GLIDE_H, 0);
    glEnd();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    /* Depth starts fresh every frame. The back buffer's depth is undefined
     * after a swap, so nothing may carry across it; within the frame the
     * ordering is all this game needs. */
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static uint32_t gl_draw_triangle(uint32_t *a)
{
    const float *v[3];
    int i, bias_mode;

    g_tris++;
    if (!gl_ensure_context()) return 0;
    gl_seed_color_from_vram();

    v[0] = (const float *)(uintptr_t)a[0];
    v[1] = (const float *)(uintptr_t)a[1];
    v[2] = (const float *)(uintptr_t)a[2];

    /* --glidedebug: draw every triangle flat, untextured, with no depth or
     * blend, so "is the geometry arriving and transforming correctly" can be
     * answered separately from "is some piece of state discarding it". */
    /* --glidedebug 2: skip the flat full-viewport quad, to test whether it is
     * what erases the cockpit inside the clip window. */
    if (g_glidedebug == 2) {
        if (g_cc_other == 2 || g_cc_local == 1) return 0;
    } else if (g_glidedebug == 1) {
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glBegin(GL_TRIANGLES);
        for (i = 0; i < 3; i++) {
            glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
            glVertex3f(v[i][VX], v[i][VY], 0.0f);
        }
        glEnd();
        return 0;
    }

    glDepthMask(g_depth_mask ? GL_TRUE : GL_FALSE);
    if (g_depth_mode) { glEnable(GL_DEPTH_TEST);
                        glDepthFunc(depth_func_gl(g_depth_func)); }
    else glDisable(GL_DEPTH_TEST);

    /* GR_DEPTHBUFFER_*_COMPARE_TO_BIAS (3 and 4) compare the *stored* depth
     * against the bias register rather than against the incoming value. That
     * is the cockpit mask: the game stamps depth 0 over the whole screen
     * through the aux buffer, then "clears" each frame with a full-viewport
     * quad set to GR_CMP_NOTEQUAL, so the quad is rejected wherever the depth
     * is still 0 -- the cockpit -- and passes only where the 3D has written
     * real depths. GL compares incoming against stored, but NOTEQUAL is
     * symmetric, so feeding the bias in as the incoming depth expresses the
     * same test. */
    /* GR_DEPTHBUFFER_*_COMPARE_TO_BIAS (3 and 4) test the *stored* depth
     * against the bias register rather than the incoming value, which
     * fixed-function GL cannot express while still writing the fragment's own
     * depth. The game uses it only for the quad it clears the viewport with,
     * and with depth cleared per frame that test would reject the quad
     * everywhere and leave the background unpainted, so it passes instead. */
    bias_mode = (g_depth_mode == 3 || g_depth_mode == 4);
    if (bias_mode) glDepthFunc(GL_ALWAYS);

    /* grColorCombine(..., other): TEXTURE (2) is a textured surface, anything
     * else is flat/Gouraud. That is the whole distinction this game makes. */
    if (g_cc_other == 1 && g_tex_bound && g_pal_valid) {
        uint32_t key = hash32(hash32(hash32(2166136261u,
                                            (uint32_t)(uintptr_t)g_tex_data),
                                     g_pal_hash),
                              g_chroma_on ? g_chroma_val | 1u : 0u);
        if (!key) key = 1;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex_upload(g_tex_data, g_tex_size, key));
        glEnable(GL_ALPHA_TEST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                  g_cc_factor == 8 ? GL_REPLACE : GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
    }

    glBegin(GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        const float *p = v[i];
        float oow = p[VOOW];
        float w   = oow > 0.0f ? 1.0f / oow : 65536.0f;
        float z   = w * (1.0f / 65536.0f);
        if (z > 1.0f) z = 1.0f;
        if (z < 0.0f) z = 0.0f;
        /* Vertex alpha is deliberately NOT fed to GL. Glide only iterates
         * alpha when grAlphaCombine selects it, and this game leaves a=0 in
         * every vertex while still expecting opaque surfaces -- feeding that
         * through makes SRC_ALPHA blending and the chroma-key alpha test
         * discard the entire scene, which is exactly what a black viewport
         * with 50 triangles a frame looks like. */
        if (g_cc_local == 1 || g_cc_other == 2) {        /* CONSTANT */
            float cr, cg, cb;
            glide_unpack(g_const_color, &cr, &cg, &cb);
            glColor4f(cr, cg, cb, 1.0f);
        } else
            glColor4f(p[VR] * (1.0f / 255.0f), p[VG] * (1.0f / 255.0f),
                      p[VB] * (1.0f / 255.0f), 1.0f);
        /* homogeneous texture coords: the hardware does the s/q divide that
         * Glide expresses as sow/oow */
        glTexCoord4f(p[VSOW] / (float)g_tex_size, p[VTOW] / (float)g_tex_size,
                     0.0f, oow);
        /* glOrtho(.., -1, 1) gives z_ndc = -z_eye, so the eye-space value has
         * to be negated to land at window depth z. Getting this backwards put
         * the black full-viewport quad at depth 0 -- nearest -- where it
         * occluded the entire scene behind it. */
        glVertex3f(p[VX], p[VY], 1.0f - z * 2.0f);
    }
    glEnd();
    return 0;
}

/* grTexSource(tmu, startAddress, evenOdd, GrTexInfo*), where GrTexInfo is
 * { smallLod, largeLod, aspectRatio, format, data }.
 *
 * The texture is identified by GrTexInfo->data. On a Voodoo the selector
 * would be startAddress -- where grTexDownloadMipmap put the texels in
 * texture RAM -- but this game passes 0 for every single download and every
 * source, so that address distinguishes nothing. It keeps one persistent
 * GrTexInfo per texture instead, and the data pointers handed to grTexSource
 * are exactly the ones its downloads used. Keying off startAddress collapses
 * all 32 textures onto one, which looks like the whole world wearing a single
 * texture.
 *
 * Records only: the frame trace shows the order is grTexSource, then
 * grTexDownloadTable, then the triangles, so this object's palette has not
 * arrived yet. Expanding here would bake in the previous object's palette, so
 * it is deferred to the first triangle that uses it. */
static uint32_t gl_tex_source(uint32_t *a)
{
    const uint32_t *ti = (const uint32_t *)(uintptr_t)a[3];
    static int warned_fmt = -1;

    g_tex_bound = 0;
    if (!ti) return 0;

    if (ti[3] != 5) {                       /* GR_TEXFMT_P_8 */
        if (warned_fmt != (int)ti[3]) {
            warned_fmt = (int)ti[3];
            LOGW("glide: texture format %u is not P_8; skipped", ti[3]);
        }
        return 0;
    }
    g_tex_size = 256 >> (ti[1] < 9 ? ti[1] : 8);
    if (g_tex_size < 1) g_tex_size = 1;
    g_tex_data = (const uint8_t *)(uintptr_t)ti[4];
    g_tex_bound = g_tex_data != NULL;
    return 0;
}

/* grTexDownloadTable(tmu, type, data). Type 2 is GR_TEXTABLE_PALETTE: 256
 * entries of 0x00RRGGBB. */
static uint32_t gl_tex_download_table(uint32_t *a)
{
    const uint32_t *p = (const uint32_t *)(uintptr_t)a[2];
    uint32_t h = 2166136261u;
    int i;

    if (a[1] != 2 || !p) return 0;
    for (i = 0; i < 256; i++) {
        g_pal_argb[i] = ((p[i] & 0xFF) << 16) |          /* B -> ARGB byte 2 */
                        (p[i] & 0xFF00) |                /* G                */
                        ((p[i] >> 16) & 0xFF);           /* R                */
        h = hash32(h, p[i]);
    }
    g_pal_hash = h;
    g_pal_valid = 1;
    if (g_glidedebug == 3) {           /* one-shot: dump palette + texels */
        static int done;
        if (!done && g_tex_data) {
            FILE *f;
            done = 1;
            if ((f = fopen("pal.raw", "wb")) != NULL) {
                fwrite(p, 4, 256, f); fclose(f);
            }
            if ((f = fopen("tex.raw", "wb")) != NULL) {
                fwrite(g_tex_data, 1, (size_t)g_tex_size * g_tex_size, f);
                fclose(f);
            }
            LOGI("glide: dumped pal.raw + tex.raw (%dx%d)", g_tex_size,
                 g_tex_size);
        }
    }
    return 0;
}

static uint32_t gl_clip_window(uint32_t *a)
{
    g_clip_x0 = (int)a[0]; g_clip_y0 = (int)a[1];
    g_clip_x1 = (int)a[2]; g_clip_y1 = (int)a[3];
    if (g_clip_x0 < 0) g_clip_x0 = 0;
    if (g_clip_y0 < 0) g_clip_y0 = 0;
    if (g_clip_x1 > GLIDE_W) g_clip_x1 = GLIDE_W;
    if (g_clip_y1 > GLIDE_H) g_clip_y1 = GLIDE_H;
    if (!gl_ensure_context()) return 0;
    glEnable(GL_SCISSOR_TEST);
    /* Glide's origin is upper-left; GL scissors from the lower-left. */
    glScissor((GLint)a[0], (GLint)(GLIDE_H - a[3]),
              (GLsizei)(a[2] - a[0]), (GLsizei)(a[3] - a[1]));
    LOGI("glide: clip window %u,%u .. %u,%u", a[0], a[1], a[2], a[3]);
    return 0;
}

static uint32_t gl_depth_mode(uint32_t *a)  { g_depth_mode = (int)a[0]; return 0; }
static uint32_t gl_depth_func(uint32_t *a)  { g_depth_func = (int)a[0]; return 0; }
static uint32_t gl_depth_mask(uint32_t *a)  { g_depth_mask = (int)a[0]; return 0; }
static uint32_t gl_depth_bias(uint32_t *a)  { g_depth_bias = a[0] & 0xFFFF; return 0; }
static uint32_t gl_chroma_mode(uint32_t *a) { g_chroma_on  = (int)a[0]; return 0; }
static uint32_t gl_chroma_value(uint32_t *a){ g_chroma_val = a[0];      return 0; }
/* grColorCombine(function, factor, local, other, invert).
 *
 * These enumerants are easy to get backwards and doing so is invisible until
 * the picture is wrong: GR_COMBINE_OTHER_TEXTURE is 0x1 -- 0x2 is
 * GR_COMBINE_OTHER_CONSTANT -- while GR_COMBINE_FACTOR_ONE is 0x8. The game
 * uses exactly two settings:
 *
 *   (3, 1, 0, 1, 0)  SCALE_OTHER, factor LOCAL, local ITERATED, other TEXTURE
 *                    -- textured surfaces, modulated by the iterated colour,
 *                    which is where the per-vertex lighting lives.
 *   (3, 8, 1, 2, 0)  SCALE_OTHER, factor ONE, local CONSTANT, other CONSTANT
 *                    -- the flat full-viewport quad, in the constant colour.
 */
static uint32_t gl_color_combine(uint32_t *a)
{
    g_cc_factor = (int)a[1];
    g_cc_local  = (int)a[2];
    g_cc_other  = (int)a[3];
    return 0;
}

static uint32_t gl_constant_color(uint32_t *a) { g_const_color = a[0]; return 0; }

static uint32_t gl_alpha_blend(uint32_t *a)
{
    if (!gl_ensure_context()) return 0;
    if (a[0] == 4 && a[1] == 0) glDisable(GL_BLEND);    /* ONE, ZERO */
    else {
        glEnable(GL_BLEND);
        glBlendFunc(blend_gl((int)a[0]), blend_gl((int)a[1]));
    }
    return 0;
}

/* grBufferClear(color, alpha, depth) */
static uint32_t gl_buffer_clear(uint32_t *a)
{
    if (!gl_ensure_context()) return 0;
    glDepthMask(GL_TRUE);
    {   float cr, cg, cb;
        glide_unpack(a[0], &cr, &cg, &cb);
        glClearColor(cr, cg, cb, 1.0f);
    }
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return 0;
}

/* The 3D result has to exist as CPU pixels before the game writes its HUD on
 * top of it, because it writes through the pointer this hands back. Reading
 * back once, on the frame's first lock, is what keeps the two layers in the
 * one buffer Glide's callers assume. */
static void lfb_sync_from_gl(void)
{
    static uint16_t band[GLIDE_W * GLIDE_H];
    int w = g_clip_x1 - g_clip_x0, h = g_clip_y1 - g_clip_y0, y;

    if (g_lfb_synced || !g_gl_ready || !g_vram) return;
    g_lfb_synced = 1;
    if (w <= 0 || h <= 0) return;

    /* Only the clip rectangle. The frame buffer is persistent as far as this
     * game is concerned: it paints the cockpit once and thereafter redraws
     * only the moving parts of the HUD, so a full-screen read-back would
     * erase the cockpit every frame -- which is exactly what it did. The 3D
     * never renders outside the clip window anyway. */
    glDisable(GL_SCISSOR_TEST);
    glReadPixels(g_clip_x0, GLIDE_H - g_clip_y1, w, h,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, band);
    glEnable(GL_SCISSOR_TEST);

    /* GL reads bottom-up; the game's frame buffer is top-down. Pixels the
     * game owns -- the cockpit it painted through the LFB and then never
     * repaints -- are skipped. Without that the 3D erases them: the game
     * "clears" each frame with a quad spanning the whole clip window, and on
     * this build the cockpit is not redrawn afterwards, so it has to survive
     * in the frame buffer rather than be regenerated. */
    for (y = 0; y < h; y++)
        memcpy((uint16_t *)g_vram + (size_t)(g_clip_y0 + y) * GLIDE_W + g_clip_x0,
               band + (size_t)(h - 1 - y) * w, (size_t)w * 2);
}

/* Push the aux (depth) surface into GL's depth buffer.
 *
 * This is what makes the cockpit survive. The game "clears" each frame by
 * drawing a quad over the whole clip window with GR_CMP_NOTEQUAL, and relies
 * on the depth buffer to reject it wherever the cockpit is -- a mask it
 * stamps once at startup by locking the aux buffer and writing depth values
 * directly. Ignoring the buffer argument sent those writes into the colour
 * frame buffer, the mask never existed, and the clear quad wiped the cockpit
 * every frame. */
static void aux_upload_to_gl(void)
{
    static GLfloat depth[GLIDE_W * GLIDE_H];
    int x, y;

    if (!g_gl_ready) return;

    /* Glide's aux buffer is 16-bit and top-down; GL wants normalised floats
     * bottom-up. 0xFFFF is the far plane, which is also where the clear
     * quad's own w lands, so the compare matches and the quad is rejected. */
    for (y = 0; y < GLIDE_H; y++) {
        const uint16_t *src = g_auxbuf + (size_t)(GLIDE_H - 1 - y) * GLIDE_W;
        GLfloat        *dst = depth + (size_t)y * GLIDE_W;
        for (x = 0; x < GLIDE_W; x++) dst[x] = src[x] * (1.0f / 65535.0f);
    }

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    /* Ortho has y growing downwards, so y=GLIDE_H is the bottom of the
     * window, which is where glDrawPixels starts filling upwards from. */
    glRasterPos2i(0, GLIDE_H);
    glDrawPixels(GLIDE_W, GLIDE_H, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    LOGI("glide: aux buffer uploaded to the GL depth buffer");
}

/* GrLfbInfo_t: size, lfbPtr, strideInBytes, writeMode, origin.
 *
 * GR_BUFFER_AUXBUFFER (2) is the depth buffer; 0 and 1 are the colour
 * buffers. Handing back the colour frame buffer for all three is what left
 * the cockpit unmasked. */
static uint32_t gl_lfb_lock(uint32_t *a)
{
    uint32_t *info = (uint32_t *)(uintptr_t)a[5];
    if (!info) return 0;

    if (a[1] == 2) {                       /* GR_BUFFER_AUXBUFFER */
        /* Pre-filled with the far value so a lock the game never writes to
         * uploads the depth grBufferClear already established, rather than
         * stamping zeros. This game does exactly that -- it locks the aux
         * buffer once at startup and writes nothing -- and zeros read as
         * "nearest everywhere", which makes every scene triangle fail
         * GR_CMP_LESS and renders an empty viewport. */
        size_t i;
        for (i = 0; i < (size_t)GLIDE_W * GLIDE_H; i++) g_auxbuf[i] = 0xFFFF;
        g_aux_locked = 1;
        info[1] = (uint32_t)(uintptr_t)g_auxbuf;
        info[2] = GLIDE_W * 2;
        info[3] = a[2];
        info[4] = a[3];
        return 1;
    }

    if (!g_vram) return 0;
    lfb_sync_from_gl();
    g_lfb_dirty = 1;
    info[1] = (uint32_t)(uintptr_t)g_vram;
    info[2] = GLIDE_W * 2;
    info[3] = a[2];
    info[4] = a[3];
    return 1;
}

static uint32_t gl_lfb_unlock(uint32_t *a)
{
    if (a[1] == 2 && g_aux_locked) {
        g_aux_locked = 0;
        aux_upload_to_gl();
    }
    return 1;
}

/* Whatever the game composited on the CPU is the finished frame, so it goes
 * back over the 3D as a full-screen blit. Frames that never locked the buffer
 * are pure GL and just get swapped. */
static uint32_t gl_buffer_swap(uint32_t *a)
{
    (void)a;
    g_glide_frame++;
    if (!gl_ensure_context()) return 0;

    if (g_lfb_dirty) {
        /* The window is larger than Glide's 640x480 -- video_init opens it at
         * twice that -- and everything above renders into a 640x480 viewport
         * so the read-backs line up with the frame buffer. Only this final
         * present covers the whole window; without it GL paints one quarter
         * of the window and the rest is never drawn at all, which is what an
         * apparently blank window turned out to be. */
        int dw = GLIDE_W * 2, dh = GLIDE_H * 2;
        SDL_GL_GetDrawableSize(video_window(), &dw, &dh);
        glViewport(0, 0, dw, dh);

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_blit_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, GLIDE_W, GLIDE_H, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_vram);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(0,       0,       0);
            glTexCoord2f(1, 0); glVertex3f(GLIDE_W, 0,       0);
            glTexCoord2f(1, 1); glVertex3f(GLIDE_W, GLIDE_H, 0);
            glTexCoord2f(0, 1); glVertex3f(0,       GLIDE_H, 0);
        glEnd();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glViewport(0, 0, GLIDE_W, GLIDE_H);   /* back to Glide's own size */
        glEnable(GL_SCISSOR_TEST);
    }

    /* --dumpvram N: the normal path lives in render_frame, which glide mode
     * never runs. Read the whole drawable after the present, so what lands in
     * the file is exactly what the window shows -- reading a 640x480 corner
     * of a 1280x960 window is how a blank window went unnoticed. */
    if (g_dumpvram_frames > 0 && --g_dumpvram_frames == 0) {
        int dw = GLIDE_W, dh = GLIDE_H;
        uint16_t *shot;
        FILE *f;
        SDL_GL_GetDrawableSize(video_window(), &dw, &dh);
        shot = (uint16_t *)malloc((size_t)dw * dh * 2);
        if (shot) {
            int y;
            glReadPixels(0, 0, dw, dh, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, shot);
            if ((f = fopen("vram.bin", "wb")) != NULL) {
                for (y = dh - 1; y >= 0; y--)          /* GL reads bottom-up */
                    fwrite(shot + (size_t)y * dw, 2, dw, f);
                fclose(f);
                LOGI("glide: dumped %dx%d window to vram.bin", dw, dh);
            }
            free(shot);
        }
    }

    SDL_GL_SwapWindow(video_window());

    /* End of frame -- deliberately not grBufferClear. This game calls
     * grBufferClear exactly once for the entire run and clears each frame by
     * drawing a full-viewport quad with GR_CMP_NOTEQUAL instead, which resets
     * the depth buffer as a side effect. Resetting the read-back flag there
     * syncs the frame buffer once and then leaves the 3D frozen for the rest
     * of the run, which is exactly what it looked like. */
    if ((g_glide_frame % 500) == 0)
        LOGI("glide: frame %u, %u triangles, tex %u hit / %u miss, pal=%d",
             g_glide_frame, g_tris, g_tex_hit, g_tex_miss, g_pal_valid);
    g_lfb_synced   = 0;
    g_lfb_dirty    = 0;
    g_frame_seeded = 0;
    return 0;
}

/* (dst_buffer, dst_x, dst_y, src_format, w, h, src_stride, src_data) */
static uint32_t gl_lfb_write_region(uint32_t *a)
{
    const uint8_t *src = (const uint8_t *)(uintptr_t)a[7];
    uint32_t x = a[1], y = a[2], w = a[4], h = a[5], r;
    int32_t  stride = (int32_t)a[6];

    if (!src || !g_vram) return 0;
    lfb_sync_from_gl();
    g_lfb_dirty = 1;
    if (x >= GLIDE_W || y >= GLIDE_H) return 0;
    if (w > GLIDE_W - x) w = GLIDE_W - x;
    if (h > GLIDE_H - y) h = GLIDE_H - y;
    for (r = 0; r < h; r++)
        memcpy(g_vram + ((size_t)(y + r) * GLIDE_W + x) * 2,
               src + (int32_t)r * stride, w * 2);
    return 1;
}

/* (src_buffer, src_x, src_y, w, h, dst_stride, dst_data) */
static uint32_t gl_lfb_read_region(uint32_t *a)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)a[6];
    uint32_t x = a[1], y = a[2], w = a[3], h = a[4], stride = a[5], r;

    if (!dst || !g_vram) return 0;
    lfb_sync_from_gl();
    if (x >= GLIDE_W || y >= GLIDE_H) return 0;
    if (w > GLIDE_W - x) w = GLIDE_W - x;
    if (h > GLIDE_H - y) h = GLIDE_H - y;
    for (r = 0; r < h; r++)
        memcpy(dst + (size_t)r * stride,
               g_vram + ((size_t)(y + r) * GLIDE_W + x) * 2, w * 2);
    return 1;
}

static const struct { const char *name; glide_fn fn; } g_impl[] = {
    { "_GRSSTQUERYHARDWARE@4",     gl_query_hardware       },
    { "_GRSSTWINOPEN@28",          gl_win_open             },
    { "_GRSSTSCREENWIDTH@0",       gl_screen_width         },
    { "_GRSSTSCREENHEIGHT@0",      gl_screen_height        },
    { "_GRGLIDEGETVERSION@4",      gl_glide_get_version    },
    { "_GRTEXMINADDRESS@4",        gl_tex_min_addr         },
    { "_GRTEXMAXADDRESS@4",        gl_tex_max_addr         },
    { "_GRLFBLOCK@24",             gl_lfb_lock             },
    { "_GRLFBUNLOCK@8",            gl_lfb_unlock           },
    { "_GRLFBWRITEREGION@32",      gl_lfb_write_region     },
    { "_GRLFBREADREGION@28",       gl_lfb_read_region      },
    { "_GRBUFFERSWAP@4",           gl_buffer_swap          },
    { "_GRBUFFERCLEAR@12",         gl_buffer_clear         },
    { "_GRDRAWTRIANGLE@12",        gl_draw_triangle        },
    { "_GRTEXSOURCE@16",           gl_tex_source           },
    { "_GRTEXDOWNLOADTABLE@12",    gl_tex_download_table   },
    { "_GRCLIPWINDOW@16",          gl_clip_window          },
    { "_GRDEPTHBUFFERMODE@4",      gl_depth_mode           },
    { "_GRDEPTHBUFFERFUNCTION@4",  gl_depth_func           },
    { "_GRDEPTHMASK@4",            gl_depth_mask           },
    { "_GRDEPTHBIASLEVEL@4",       gl_depth_bias           },
    { "_GRCHROMAKEYMODE@4",        gl_chroma_mode          },
    { "_GRCHROMAKEYVALUE@4",       gl_chroma_value         },
    { "_GRCOLORCOMBINE@20",        gl_color_combine        },
    { "_GRCONSTANTCOLORVALUE@4",   gl_constant_color       },
    { "_GRALPHABLENDFUNCTION@16",  gl_alpha_blend          },
    { "_GRSSTISBUSY@0",            gl_zero                 },
    { "_GRBUFFERNUMPENDING@0",     gl_zero                 },
};
/* The one place a Glide call actually arrives. Returns the value the game
 * will see in EAX; the stdcall cleanup is done by the caller thunk, which
 * knows the argument size. */
uint32_t __cdecl glide_call(uint32_t idx, uint32_t *args)
{
    glide_import *im = &g_imp[idx];
    int want;

    im->calls++;
    /* --glidetrace N traces one frame and stops. Tracing the whole run is
     * useless here -- the render loop issues millions of calls -- and what
     * actually needs answering is the ORDER within a single frame. */
    want = g_glidetrace > 0 ? (uint32_t)g_glidetrace == g_glide_frame
                            : im->calls == 1;
    if (want) {
        char buf[160];
        uint32_t n = im->argbytes / 4, i;
        int off = _snprintf(buf, sizeof buf, "glide: %s(", im->name);
        if (n > 6) n = 6;
        for (i = 0; i < n && off > 0 && off < (int)sizeof buf; i++)
            off += _snprintf(buf + off, sizeof buf - off, "%s0x%X",
                             i ? ", " : "", args[i]);
        if (off > 0 && off < (int)sizeof buf)
            _snprintf(buf + off, sizeof buf - off, "%s)",
                      im->argbytes / 4 > 6 ? ", ..." : "");
        buf[sizeof buf - 1] = 0;
        LOGI("f%u %s%s%s", g_glide_frame, buf, im->calls == 1 ? "   [first]" : "",
             im->fn ? "" : "   [stub]");
    }
    return im->fn ? im->fn(args) : 0;
}

/* eax = import index on entry. Everything below the return address belongs to
 * the caller's stdcall frame, so the argument bytes are dropped here. */
static __declspec(naked) void glide_common(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov  ebx, eax                   /* index, across the call */
        lea  ecx, [esp + 20]            /* four pushes + return address */
        push ecx
        push ebx
        call glide_call
        add  esp, 8
        mov  ecx, dword ptr [g_argbytes + ebx * 4]
        pop  ebp
        pop  edi
        pop  esi
        pop  ebx
        pop  edx                        /* return address */
        add  esp, ecx                   /* callee cleanup, stdcall */
        jmp  edx
    }
}

void glide_install(void)
{
    uint8_t  *thunk;
    uint32_t  i, k, impl = 0;

    if (!g_glide_mode || !g_glide) return;

    /* Walk the name table until it stops looking like exports. */
    for (i = 0; i < MAX_IMPORTS; i++) {
        uint32_t p = *(const uint32_t *)gp(NAME_TABLE + i * 4);
        const char *nm;
        if (p < (uint32_t)(uintptr_t)g_img.mem ||
            p >= (uint32_t)(uintptr_t)g_img.mem + g_img.size) break;
        nm = (const char *)(uintptr_t)p;
        if (!plausible_export(nm)) break;
        g_imp[i].name     = nm;
        g_imp[i].argbytes = parse_argbytes(nm);
        g_argbytes[i]     = g_imp[i].argbytes;
        for (k = 0; k < sizeof g_impl / sizeof g_impl[0]; k++)
            if (!strcmp(nm, g_impl[k].name)) { g_imp[i].fn = g_impl[k].fn; break; }
    }
    g_nimp = i;
    if (!g_nimp) { LOGE("glide: no imports found at orig 0x%08X", NAME_TABLE);
                   return; }

    /* Confirm the stubs really are calls to the resolver before rewriting
     * them -- the same byte check every other patch in this port makes. */
    for (i = 0; i < g_nimp; i++) {
        const uint8_t *s = (const uint8_t *)gp(STUB_TABLE + i * STUB_LEN);
        int32_t rel;
        memcpy(&rel, s + 1, 4);
        if (s[0] != 0xE8 || s + 5 + rel != (const uint8_t *)gp(RESOLVER)) {
            LOGE("glide: stub %u (%s) is not a call to the resolver: %02X",
                 i, g_imp[i].name, s[0]);
            return;
        }
    }

    g_thunks = (uint8_t *)VirtualAlloc(NULL, g_nimp * 16,
                                       MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
    if (!g_thunks) { LOGE("glide: thunk alloc failed"); return; }

    for (i = 0; i < g_nimp; i++) {
        uint8_t *stub = (uint8_t *)gp(STUB_TABLE + i * STUB_LEN);
        int32_t  rel;

        /* mov eax, <index> ; jmp glide_common */
        thunk = g_thunks + i * 16;
        thunk[0] = 0xB8;
        memcpy(thunk + 1, &i, 4);
        thunk[5] = 0xE9;
        rel = (int32_t)((uint8_t *)glide_common - (thunk + 10));
        memcpy(thunk + 6, &rel, 4);

        /* and point the game's stub at it, exactly as the resolver would */
        stub[0] = 0xE9;
        rel = (int32_t)(thunk - (stub + 5));
        memcpy(stub + 1, &rel, 4);
    }

    for (i = 0; i < g_nimp; i++) if (g_imp[i].fn) impl++;
    LOGI("glide: %u imports redirected (%u implemented, %u stubbed);"
         " GLIDE2X.OVL is never loaded", g_nimp, impl, g_nimp - impl);
    LOGI("glide: first=%s last=%s", g_imp[0].name, g_imp[g_nimp - 1].name);
}

void glide_summary(void)
{
    uint32_t i, used = 0;
    if (!g_nimp) return;
    LOGI("---- Glide entry points actually called ----");
    for (i = 0; i < g_nimp; i++)
        if (g_imp[i].calls) {
            LOGI("  %-40s x%u", g_imp[i].name, g_imp[i].calls);
            used++;
        }
    LOGI("---- %u of %u imports used ----", used, g_nimp);
    LOGI("glide: %u triangles, texture cache %u hit / %u miss",
         g_tris, g_tex_hit, g_tex_miss);
}
