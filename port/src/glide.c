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
#include <math.h>

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
int g_probe_x = -1, g_probe_y = -1;
static int g_probe_now;
static uint32_t g_probe_hits;
static uint32_t g_ps_in, g_ps_out, g_ps_zfail, g_ps_chroma;
int g_dumpsec;                  /* --dumpsec N: grab both buffers N s in */
int g_glidetrace;               /* --glidetrace N: trace frame N only       */
int g_glidecallers;             /* --glidecallers N: who draws in frame N   */

/* --glidecallers: every distinct chain of game-code return addresses found
 * on the stack under grDrawTriangle in one frame, with a count. The scan
 * also picks up stale code pointers, so a chain is a hint, not a proof. */
static void callers_note(const uint32_t *args, int dump)
{
    static struct { uint32_t r[6]; uint32_t n; } tab[512];
    static int ntab;
    uintptr_t cs = (uintptr_t)g_img.mem, ce = cs + g_img.code_size;
    uint32_t r[6] = {0};
    int i, k, n = 0;
    if (dump) {
        for (i = 0; i < ntab; i++)
            LOGI("callers: %6u x  %08X %08X %08X %08X %08X %08X", tab[i].n,
                 tab[i].r[0], tab[i].r[1], tab[i].r[2], tab[i].r[3], tab[i].r[4], tab[i].r[5]);
        ntab = 0;
        return;
    }
    for (k = -1; k < 200 && n < 6; k++)
        if (args[k] >= cs && args[k] < ce) r[n++] = args[k] - g_img.delta;
    for (i = 0; i < ntab && memcmp(tab[i].r, r, sizeof r); i++) {}
    if (i == ntab && ntab < 512) { memcpy(tab[i].r, r, sizeof r); tab[i].n = 0; ntab++; }
    if (i < ntab) tab[i].n++;
}

/* script "trace": the frame after next, whatever number it turns out to be */
void glide_trace_next(void) { g_glidetrace = (int)g_glide_frame + 2; }
void glide_callers_next(void) { g_glidecallers = (int)g_glide_frame + 2; g_objprobe_frame = g_glide_frame + 2; }

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

/* grSstPerfStats(GrSstPerfStats_t *) -- pixelsIn, chromaFail, zFuncFail,
 * aFuncFail, pixelsOut. The game samples these about once a second, and the
 * stub never wrote to the caller's struct at all, so it was reading whatever
 * happened to be on its stack. Report what the renderer actually did. */
static uint32_t gl_perf_stats(uint32_t *a)
{
    uint32_t *st = (uint32_t *)(uintptr_t)a[0];
    if (!st) return 0;
    st[0] = g_ps_in;
    st[1] = g_ps_chroma;
    st[2] = g_ps_zfail;
    st[3] = 0;
    st[4] = g_ps_out;
    return 0;
}

static uint32_t gl_reset_perf_stats(uint32_t *a)
{
    (void)a;
    g_ps_in = g_ps_out = g_ps_zfail = g_ps_chroma = 0;
    return 0;
}

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
#ifndef GL_FOG_COORD_SRC
#define GL_FOG_COORD_SRC 0x8450
#endif
#ifndef GL_FOG_COORD
#define GL_FOG_COORD 0x8451
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif

#define TEXCACHE 1024

typedef struct {
    uint32_t key;        /* 0 = empty */
    GLuint   tex;
    GLuint   tile[4];    /* the four quadrants, each its own texture */
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
static int      g_clamp_s, g_clamp_t;      /* grTexClampMode  */
static int      g_filter_min = 1, g_filter_mag = 1;  /* grTexFilterMode */
static int      g_chroma_on;
static uint32_t g_chroma_rgb;   /* chroma key, unpacked to match g_pal_argb */
static uint32_t g_chroma_val;
static int      g_cc_other = 1, g_cc_factor = 8, g_cc_local;
static uint32_t g_const_color = 0xFFFFFFFF;
static int      g_depth_mode, g_depth_mask = 1, g_depth_func = 7;
static uint32_t g_depth_bias;       /* grDepthBiasLevel, a 16-bit depth value */

/* Fog. GR_FOG_TABLE_SIZE is 64: the index-to-w mapping the reference gives,
 * pow(2, 3 + (i >> 2)) / (8 - (i & 3)), spans w = 1 .. 52429 over 64 entries,
 * which is the Voodoo's whole w range. Entries are 8-bit opacities, 0x00 no
 * fog and 0xFF full fog, and the blend is
 *
 *     C_out = f * C_fog + (1 - f) * C_in
 *
 * which is GL's own fog equation with the factor inverted. Feeding f in as a
 * fog coordinate under GL_LINEAR with start 0 and end 1 gives GL's factor as
 * 1 - f, so the two agree exactly and the game's table is honoured rather
 * than approximated by an exponential curve. */
#define GR_FOG_TABLE_SIZE 64
/* Depth range. The fog table spans w = 1..52429, but the game emits geometry
 * well past that -- measured 40..85188 -- so the far plane sits at 2^17 to
 * cover it with room to spare.
 *
 * Depth is NOT linear in w, even though Glide calls this a w buffer. A
 * triangle's w does not vary linearly across it in screen space; 1/w does.
 * Handing GL a per-vertex depth of w/far therefore interpolates along the
 * wrong curve: a triangle spanning w = 1000..50000 lands at 25500 half way
 * across where the true value is 1960. Terrain triangles cover exactly that
 * kind of range, so their interiors read as far more distant than they are
 * and lose the depth test to nearer geometry -- pipes drawing over the
 * terrain that should occlude them.
 *
 * So depth is the standard hyperbolic mapping, A + B/w. It is linear in 1/w
 * and therefore interpolates exactly under screen-space interpolation, and it
 * is monotonic in w, which is all the depth test needs. */
#define G_WNEAR  32.0f
/* The game's own far plane, measured: oow bottoms out at exactly 1/65536
 * across every vertex it submits, and the quad it clears the viewport with
 * sits precisely there. Mapping depth over that range rather than double it
 * spends no precision on distances the game never uses. */
#define G_WFAR   65536.0f
#define G_DEPTH_A (G_WFAR / (G_WFAR - G_WNEAR))
#define G_DEPTH_B (-G_WNEAR * G_WFAR / (G_WFAR - G_WNEAR))

static int      g_fog_mode, g_fog_valid;
static uint32_t g_fog_color;
static uint8_t  g_fog_table[GR_FOG_TABLE_SIZE];
static float    g_fog_w[GR_FOG_TABLE_SIZE];
static void (APIENTRY *p_glFogCoordf)(GLfloat);
/* ------------------------------------------------------- frame buffer
 *
 * The Glide frame buffer is an offscreen FBO at g_fbw x g_fbh -- 640x480
 * times the render scale -- and it is persistent, as the Voodoo's was: the
 * game paints its cockpit once and afterwards redraws only what moves. All
 * Glide coordinates stay in the game's own 640x480 space; the projection
 * maps them onto the FBO, so geometry gains resolution for free.
 *
 * The game also writes the frame buffer through the CPU (grLfbLock, then raw
 * 16-bit stores) and those writes are interleaved with 3D: in one frame it
 * draws the scene, blits the target monitor's background, draws the target
 * ship into it, and only then paints the HUD. So the CPU copy (g_vram) is a
 * 640x480 view of the FBO, refreshed on demand, and what the game writes into
 * it goes back into the FBO as soon as anything else would be drawn over it:
 * the copy is diffed against a snapshot taken when it was handed out, and
 * only changed pixels are uploaded. Ordering is then exactly the game's. */
static GLuint   g_fbo, g_fbo_tex, g_fbo_rb;      /* single-sample, resolved   */
static GLuint   g_fbo_ms, g_fbo_ms_col, g_fbo_ms_ds;  /* render target if MSAA */
static GLuint   g_fbo_small, g_fbo_small_rb;     /* 640x480 for read-back     */
static GLuint   g_overlay_tex, g_mask_tex;
static int      g_fbw = GLIDE_W, g_fbh = GLIDE_H;
static int      g_vram_valid;          /* g_vram mirrors the FBO            */
static int      g_lfb_pending;         /* game may have written g_vram      */
static int      g_lock_depth;
static int      g_frame_started;
static uint16_t g_lfb_snap[GLIDE_W * GLIDE_H];
int g_render_scale;                    /* --scale N, 0 = from the desktop   */
int g_wide_arg = -1;                   /* --wide N, -1 = from the desktop   */

/* Hor+ widescreen: the frame buffer is wider than the game's 640 columns by
 * g_ext on each side, and Glide x runs from -g_ext to 640 + g_ext. The game's
 * own 640x480 (2D, LFB, cockpit) sits unchanged in the middle; only the 3D
 * reaches the sides, once hplus_view() has widened the game's clip planes. */
static int g_ext, g_ext_known;
static int g_view_y0 = -1, g_view_y1;  /* rows of the game's 3D view (clip) */

/* Under Hor+ a cockpit image may have a 16:9 replacement with a far bigger
 * window, <hd>/cockpit/<name>.png (tools/cockpit_wide.py): drawn across the
 * whole frame in place of the 4:3 art, and its alpha is the cockpit mask in
 * place of the game's aux buffer. 0 while the screen shows anything else. */
static GLuint g_cockpit_cur;
static float  g_cockpit_hw;            /* half width, 640-space units       */
/* The outside views put the 3D in a letterbox: a 640x480 frame that is black
 * but for two grey lines (letterbox_select's test in gl_lfb_write_region),
 * with the bars stamped into the aux buffer. Under
 * Hor+ that frame is not drawn and nothing is masked, so the 3D fills the
 * whole screen there as it does in the cockpit. */
static int    g_letterbox;
#define GLIDE_VW (GLIDE_W + 2 * g_ext)

static int hplus_ext(void)
{
    SDL_DisplayMode dm;
    if (g_ext_known) return g_ext;
    g_ext_known = 1;
    if (g_wide_arg >= 0) g_ext = g_wide_arg;
    else if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h > 0 &&
             (long)dm.w * 3 > (long)dm.h * 4)
        g_ext = (int)(((long)GLIDE_H * dm.w / dm.h - GLIDE_W) / 2);
    if (g_ext > GLIDE_W) g_ext = GLIDE_W;
    LOGI("glide: Hor+ %d columns each side (%d x %d)", g_ext, GLIDE_VW, GLIDE_H);
    return g_ext;
}
int g_msaa = 4;                        /* --msaa N                          */

/* --fx LIST: what this renderer adds to the game's own look, all of it
 * computed, none of it shipped: the water absorbing light with distance
 * (red first), caustics on surfaces facing up, the boat's headlight, relief
 * for both taken from the texture's own brightness, and ambient occlusion
 * and bloom over the finished view.
 * Each needs the camera (objprobe_camera), so each pixel of the 3D view can
 * be put back where it is in the world. The game's lighting stays as the
 * base; these come on top. */
#define FX_FOG      1
#define FX_CAUSTICS 2
#define FX_LIGHT    4
#define FX_BUMP     128                    /* relief for LIGHT and CAUSTICS */
#define FX_TRI      (FX_FOG | FX_CAUSTICS | FX_LIGHT | FX_BUMP)   /* triangle shader */
#define FX_SSAO     32                                 /* passes, see post_run   */
#define FX_BLOOM    64
#define FX_ALL      (FX_TRI | FX_SSAO | FX_BLOOM)
int g_fx = FX_ALL;
static int g_blend_on, g_blend_add;     /* the game's blend, see gl_alpha_blend */
static GLenum g_blend_src = GL_ONE, g_blend_dst = GL_ZERO;
static void post_maybe(void);

int glide_fx_parse(const char *list)
{
    static const struct { const char *name; int bit; } k[] = {
        { "none", 0 }, { "all", FX_ALL }, { "fog", FX_FOG },
        { "caustics", FX_CAUSTICS }, { "light", FX_LIGHT },
        { "ssao", FX_SSAO }, { "bloom", FX_BLOOM }, { "bump", FX_BUMP } };
    char buf[128], *t, *ctx = NULL;
    int fx = 0, i;
    strncpy(buf, list, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (t = strtok_s(buf, ", ", &ctx); t; t = strtok_s(NULL, ", ", &ctx)) {
        for (i = 0; i < (int)(sizeof k / sizeof k[0]) && _stricmp(t, k[i].name); i++) {}
        if (i == (int)(sizeof k / sizeof k[0])) LOGW("fx: unknown effect '%s'", t);
        else fx |= k[i].bit;
    }
    LOGI("fx: %s -> 0x%X", list, fx);
    return fx;
}
int g_smooth2d;                        /* --smooth2d: bilinear 2D overlays  */

#define GL_FRAMEBUFFER_          0x8D40
#define GL_READ_FRAMEBUFFER_     0x8CA8
#define GL_DRAW_FRAMEBUFFER_     0x8CA9
#define GL_RENDERBUFFER_         0x8D41
#define GL_COLOR_ATTACHMENT0_    0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT_ 0x821A
#define GL_DEPTH24_STENCIL8_     0x88F0
#define GL_RGBA8_                0x8058
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_LINEAR_MIPMAP_LINEAR_ 0x2703
#define GL_TEXTURE_MAX_ANISOTROPY_ 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_ 0x84FF
#define GL_MAX_SAMPLES_          0x8D57

static void (APIENTRY *p_glGenFramebuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *p_glFramebufferTexture2D)(GLenum, GLenum, GLenum,
                                                 GLuint, GLint);
static void (APIENTRY *p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum,
                                                    GLuint);
static GLenum (APIENTRY *p_glCheckFramebufferStatus)(GLenum);
static void (APIENTRY *p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glBindRenderbuffer)(GLenum, GLuint);
static void (APIENTRY *p_glRenderbufferStorage)(GLenum, GLenum, GLsizei,
                                                GLsizei);
static void (APIENTRY *p_glRenderbufferStorageMultisample)(GLenum, GLsizei,
                                                           GLenum, GLsizei,
                                                           GLsizei);
static void (APIENTRY *p_glBlitFramebuffer)(GLint, GLint, GLint, GLint,
                                            GLint, GLint, GLint, GLint,
                                            GLbitfield, GLenum);
static void (APIENTRY *p_glGenerateMipmap)(GLenum);
static float g_max_aniso;

/* Where everything is drawn: the multisampled target when MSAA is on. */
static GLuint draw_fbo(void) { return g_fbo_ms ? g_fbo_ms : g_fbo; }
/* GR_BUFFER_AUXBUFFER: the depth buffer, which this game writes through
 * the LFB to stamp its cockpit mask. */
static uint16_t g_auxbuf[GLIDE_W * GLIDE_H];
static int      g_aux_locked;
static int      g_mask_ready;
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

/* ------------------------------------------------------------ w buffering
 *
 * Glide w-buffers: the depth value is w itself, spread uniformly, and the
 * Voodoo interpolates it per pixel. Neither half of that is expressible in
 * fixed-function GL.
 *
 * Depth linear in w cannot be interpolated by the rasteriser, because w does
 * not vary linearly across a triangle in screen space -- 1/w does. Handing GL
 * a per-vertex depth of w/far therefore follows the wrong curve entirely: a
 * triangle spanning w = 1000..50000 lands at 25500 half way across where the
 * true value is 1960, an error of +1200%. The usual answer is the hyperbolic
 * mapping A + B/w, which is linear in 1/w and so interpolates exactly -- but
 * it buys that by crushing everything distant into the last fraction of the
 * depth range. Measured here: the whole scene occupied 0.9978..0.9998, with
 * almost all the range spent on empty space in front of it.
 *
 * A fragment shader gives both properties at once. Under this ortho setup
 * gl_TexCoord[0].q carries oow and interpolates linearly in screen space,
 * which is exactly right for 1/w, so reconstructing w = 1/oow per fragment
 * and writing gl_FragDepth = w/far reproduces the Voodoo's w buffer: linear
 * in w, uniform precision at every distance, and correct across the whole
 * triangle. Writing gl_FragDepth also makes the depth test use that value.
 *
 * The rest of the shader is the fixed-function state it replaces -- the two
 * texture environments the game uses, the chroma-key alpha test, and Glide's
 * fog blend, which is mix(colour, fogColour, f) for the f the fog table
 * gives. If any of it fails to build the fixed-function path is kept.
 */

static const char *g_vs_src =
    "varying vec2 vScreen;\n"
    "void main()\n"
    "{\n"
    "    vScreen         = gl_Vertex.xy;\n"
    "    gl_Position     = ftransform();\n"
    "    gl_TexCoord[0]  = gl_MultiTexCoord0;\n"
    "    gl_FrontColor   = gl_Color;\n"
    "    gl_FogFragCoord = gl_FogCoord;\n"
    "}\n";

static const char *g_fs_src =
    "uniform sampler2D uTex;\n"
    "uniform int   uTexMode;      /* 0 none, 1 modulate, 2 replace */\n"
    "uniform int   uAlphaTest;\n"
    "uniform int   uFog;\n"
    "uniform vec3  uFogColor;\n"
    "uniform float uWFar;\n"
    "uniform int   uBackdrop;\n"
    /* --fx (see FX_*): the camera, so a fragment can be put back into the
     * world. uProj = fx, fy, cx, cy in screen units, uRange = near, far;
     * camera space is x right, y depth, z up, and uCamInv takes it to the
     * world. */
    "uniform int   uFx;\n"
    "uniform vec4  uProj;\n"
    "uniform vec2  uRange;\n"
    "uniform mat3  uCamInv;\n"
    "uniform vec3  uCamPos;\n"
    "uniform vec3  uWater;\n"
    "uniform float uTime;\n"
    "varying vec2  vScreen;\n"
    "bool fx(int b) { return mod(floor(float(uFx) / float(b)), 2.0) >= 1.0; }\n"
    "vec2 hash2(vec2 c)\n"
    "{\n"
    "    return fract(sin(vec2(dot(c, vec2(127.1, 311.7)), dot(c, vec2(269.5, 183.3)))) * 43758.5453);\n"
    "}\n"
    /* Caustics as the edges of drifting cells: bright where a point is about
     * as near to its second-nearest cell centre as to its nearest. */
    "float caustic(vec2 p, float t)\n"
    "{\n"
    "    vec2 c = floor(p), f = fract(p);\n"
    "    float d1 = 8.0, d2 = 8.0;\n"
    "    for (int j = -1; j <= 1; j++)\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "        vec2 g = vec2(float(i), float(j)), h = hash2(c + g);\n"
    "        vec2 o = 0.5 + 0.4 * sin(t * (0.5 + 0.5 * h) + 6.2831 * h);\n"
    "        float d = length(g + o - f);\n"
    "        if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;\n"
    "    }\n"
    "    return 1.0 - smoothstep(0.0, 0.3, d2 - d1);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    float oow = gl_TexCoord[0].q;\n"
    "    float w   = oow > 0.0 ? 1.0 / oow : uWFar;\n"
    /* The backdrop owns the far plane, so everything else must land in front
     * of it. The game clamps w at 65536 but draws its backdrop quad at 65535,
     * one unit nearer -- so clamped terrain sorts BEHIND the backdrop, fails
     * GR_CMP_LESS and is discarded, holing the horizon. Anything drawn there
     * earlier survives in the hole, which reads as an object the terrain
     * failed to occlude, and only where terrain reaches the far plane. */
    "    gl_FragDepth = uBackdrop != 0 ? 1.0 : min(w / uWFar, 0.99997);\n"
    "    vec4 c;\n"
    "    if (uTexMode == 0) {\n"
    "        c = gl_Color;\n"
    "    } else {\n"
    "        vec4 t = texture2DProj(uTex, gl_TexCoord[0]);\n"
    "        c = uTexMode == 2 ? t : t * gl_Color;\n"
    "        if (uAlphaTest != 0 && c.a <= 0.5) discard;\n"   /* GL's GREATER 0.5 */
    "    }\n"
    "    if (uFx != 0) {\n"
    "        vec3 albedo = uTexMode == 0 ? gl_Color.rgb : texture2DProj(uTex, gl_TexCoord[0]).rgb;\n"
    "        float y = uRange.x + (w - 1.0) * (uRange.y - uRange.x) / 65535.0;\n"
    "        vec3 pc = vec3((vScreen.x - uProj.z) * y / uProj.x, y, (vScreen.y - uProj.w) * y / uProj.y);\n"
    "        float d = length(pc);\n"
    "        vec3 n = normalize(cross(dFdx(pc), dFdy(pc)));\n"
    "        if (dot(n, pc) > 0.0) n = -n;\n"
    "        if (fx(128) && uTexMode != 0) {\n"     /* relief: dark is deep */
    "            float h = dot(albedo, vec3(0.30, 0.59, 0.11)) * 6.0;\n"
    "            vec3 dpx = dFdx(pc), dpy = dFdy(pc);\n"
    "            vec3 r1 = cross(dpy, n), r2 = cross(n, dpx);\n"
    "            float det = dot(dpx, r1);\n"
    "            vec3 g = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);\n"
    "            n = normalize(abs(det) * n - g);\n"
    "        }\n"
    "        vec3 add = vec3(0.0);\n"
    "        if (fx(4)) {\n"            /* headlight, from the camera */
    "            float spot = smoothstep(0.75, 0.96, y / d);\n"
    "            float att  = 1.0 / (1.0 + (d * d) / (1400.0 * 1400.0));\n"
    "            add += vec3(0.80, 0.92, 1.00) * spot * att * (0.25 + 0.75 * max(dot(n, -pc / d), 0.0));\n"
    "        }\n"
    "        if (fx(2)) {\n"            /* caustics, from above */
    "            vec3 pw = uCamPos + uCamInv * pc;\n"
    "            float up = max((uCamInv * n).z, 0.0);\n"
    "            float k = 0.6 * caustic(pw.xy / 95.0, uTime) + 0.4 * caustic(pw.xy / 61.0 + 7.3, 1.3 * uTime);\n"
    "            add += vec3(0.30, 0.45, 0.55) * k * k * up * (1.0 - smoothstep(0.30, 0.65, d / uRange.y));\n"
    "        }\n"
    "        if (!fx(8)) c.rgb += albedo * add;\n"   /* 8: blended */
    "        if (fx(1)) {\n"            /* absorption, red first */
    "            vec3 T = exp(-vec3(2.4, 1.3, 0.9) * d / uRange.y);\n"
    "            c.rgb = fx(16) ? c.rgb * sqrt(T) : c.rgb * T + uWater * (1.0 - T);\n"
    "        }\n"
    "    }\n"
    "    if (uFog != 0) c.rgb = mix(c.rgb, uFogColor, gl_FogFragCoord);\n"
    /* alpha: emission for the bloom pass (post_run), summed by the additive
     * blend; nothing else reads the frame buffer's alpha */
    "    gl_FragColor = vec4(c.rgb, fx(16) ? max(c.r, max(c.g, c.b)) : 0.0);\n"
    "}\n";

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
typedef char GLchar_;

static GLuint (APIENTRY *p_glCreateShader)(GLenum);
static void   (APIENTRY *p_glShaderSource)(GLuint, GLsizei, const GLchar_ **,
                                           const GLint *);
static void   (APIENTRY *p_glCompileShader)(GLuint);
static void   (APIENTRY *p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *,
                                               GLchar_ *);
static GLuint (APIENTRY *p_glCreateProgram)(void);
static void   (APIENTRY *p_glAttachShader)(GLuint, GLuint);
static void   (APIENTRY *p_glLinkProgram)(GLuint);
static void   (APIENTRY *p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_glUseProgram)(GLuint);
static GLint  (APIENTRY *p_glGetUniformLocation)(GLuint, const GLchar_ *);
static void   (APIENTRY *p_glUniform1i)(GLint, GLint);
static void   (APIENTRY *p_glUniform1f)(GLint, GLfloat);
static void   (APIENTRY *p_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
static void   (APIENTRY *p_glUniform2f)(GLint, GLfloat, GLfloat);
static void   (APIENTRY *p_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void   (APIENTRY *p_glUniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *);
static void   (APIENTRY *p_glActiveTexture)(GLenum);
#define GL_TEXTURE0_ 0x84C0
#define GL_TEXTURE1_ 0x84C1

static GLuint g_prog;
static GLint  g_u_texmode, g_u_alphatest, g_u_fog, g_u_fogcolor, g_u_wfar;
static GLint  g_u_backdrop;
static GLint  g_u_fx, g_u_proj, g_u_range, g_u_caminv, g_u_campos, g_u_water, g_u_time;

static GLuint shader_build(GLenum type, const char *src)
{
    GLuint sh = p_glCreateShader(type);
    GLint  ok = 0;
    p_glShaderSource(sh, 1, (const GLchar_ **)&src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        p_glGetShaderInfoLog(sh, sizeof log, &n, log);
        log[sizeof log - 1] = 0;
        LOGE("glide: %s shader failed: %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        return 0;
    }
    return sh;
}

static void shader_init(void)
{
    GLuint vs, fs;
    GLint  ok = 0;

    p_glCreateShader  = (GLuint (APIENTRY *)(GLenum))
                            SDL_GL_GetProcAddress("glCreateShader");
    p_glShaderSource  = (void (APIENTRY *)(GLuint, GLsizei, const GLchar_ **,
                                           const GLint *))
                            SDL_GL_GetProcAddress("glShaderSource");
    p_glCompileShader = (void (APIENTRY *)(GLuint))
                            SDL_GL_GetProcAddress("glCompileShader");
    p_glGetShaderiv   = (void (APIENTRY *)(GLuint, GLenum, GLint *))
                            SDL_GL_GetProcAddress("glGetShaderiv");
    p_glGetShaderInfoLog = (void (APIENTRY *)(GLuint, GLsizei, GLsizei *,
                                              GLchar_ *))
                            SDL_GL_GetProcAddress("glGetShaderInfoLog");
    p_glCreateProgram = (GLuint (APIENTRY *)(void))
                            SDL_GL_GetProcAddress("glCreateProgram");
    p_glAttachShader  = (void (APIENTRY *)(GLuint, GLuint))
                            SDL_GL_GetProcAddress("glAttachShader");
    p_glLinkProgram   = (void (APIENTRY *)(GLuint))
                            SDL_GL_GetProcAddress("glLinkProgram");
    p_glGetProgramiv  = (void (APIENTRY *)(GLuint, GLenum, GLint *))
                            SDL_GL_GetProcAddress("glGetProgramiv");
    p_glUseProgram    = (void (APIENTRY *)(GLuint))
                            SDL_GL_GetProcAddress("glUseProgram");
    p_glGetUniformLocation = (GLint (APIENTRY *)(GLuint, const GLchar_ *))
                            SDL_GL_GetProcAddress("glGetUniformLocation");
    p_glUniform1i     = (void (APIENTRY *)(GLint, GLint))
                            SDL_GL_GetProcAddress("glUniform1i");
    p_glUniform1f     = (void (APIENTRY *)(GLint, GLfloat))
                            SDL_GL_GetProcAddress("glUniform1f");
    p_glUniform3f     = (void (APIENTRY *)(GLint, GLfloat, GLfloat, GLfloat))
                            SDL_GL_GetProcAddress("glUniform3f");

    *(void **)&p_glUniform2f = SDL_GL_GetProcAddress("glUniform2f");
    *(void **)&p_glUniform4f = SDL_GL_GetProcAddress("glUniform4f");
    *(void **)&p_glUniformMatrix3fv = SDL_GL_GetProcAddress("glUniformMatrix3fv");
    *(void **)&p_glActiveTexture = SDL_GL_GetProcAddress("glActiveTexture");
    if (!p_glUniform2f || !p_glUniform4f || !p_glUniformMatrix3fv) g_fx = 0;

    if (!p_glCreateShader || !p_glUseProgram || !p_glGetUniformLocation) {
        LOGW("glide: no GLSL; depth stays hyperbolic in the fixed pipeline");
        return;
    }

    vs = shader_build(GL_VERTEX_SHADER, g_vs_src);
    fs = shader_build(GL_FRAGMENT_SHADER, g_fs_src);
    if (!vs || !fs) return;

    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { LOGE("glide: shader link failed"); g_prog = 0; return; }

    g_u_texmode   = p_glGetUniformLocation(g_prog, "uTexMode");
    g_u_alphatest = p_glGetUniformLocation(g_prog, "uAlphaTest");
    g_u_fog       = p_glGetUniformLocation(g_prog, "uFog");
    g_u_fogcolor  = p_glGetUniformLocation(g_prog, "uFogColor");
    g_u_wfar      = p_glGetUniformLocation(g_prog, "uWFar");
    g_u_backdrop  = p_glGetUniformLocation(g_prog, "uBackdrop");
    g_u_fx        = p_glGetUniformLocation(g_prog, "uFx");
    g_u_proj      = p_glGetUniformLocation(g_prog, "uProj");
    g_u_range     = p_glGetUniformLocation(g_prog, "uRange");
    g_u_caminv    = p_glGetUniformLocation(g_prog, "uCamInv");
    g_u_campos    = p_glGetUniformLocation(g_prog, "uCamPos");
    g_u_water     = p_glGetUniformLocation(g_prog, "uWater");
    g_u_time      = p_glGetUniformLocation(g_prog, "uTime");
    LOGI("glide: w-buffer shader active; depth is linear in w to %.0f",
         (double)G_WFAR);
}

/* Interpolate the game's fog table at a given w. The table is indexed by w
 * rather than by depth, and the entry spacing is geometric, so the bracketing
 * pair is found by binary search over the precomputed w values. */
static float fog_factor(float w)
{
    int lo = 0, hi = GR_FOG_TABLE_SIZE - 1, mid;
    float t;

    if (w <= g_fog_w[0])  return g_fog_table[0] * (1.0f / 255.0f);
    if (w >= g_fog_w[hi]) return g_fog_table[hi] * (1.0f / 255.0f);
    while (hi - lo > 1) {
        mid = (lo + hi) / 2;
        if (w < g_fog_w[mid]) hi = mid; else lo = mid;
    }
    t = (w - g_fog_w[lo]) / (g_fog_w[hi] - g_fog_w[lo]);
    return (g_fog_table[lo] + t * (g_fog_table[hi] - g_fog_table[lo]))
           * (1.0f / 255.0f);
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

#define GLPROC(var, name) \
    (*(void **)&(var) = SDL_GL_GetProcAddress(name))

/* The render scale: --scale N, or enough that the FBO is at least as tall as
 * the desktop, so a full-screen window is supersampled rather than
 * stretched. */
static int pick_scale(void)
{
    SDL_DisplayMode dm;
    int s = g_render_scale;
    if (s <= 0) {
        s = 2;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h > 0)
            s = (dm.h + GLIDE_H - 1) / GLIDE_H;
    }
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    return s;
}

static int fbo_create(void)
{
    int scale = pick_scale(), samples = g_msaa;
    GLint maxs = 0;

    if (!GLPROC(p_glGenFramebuffers, "glGenFramebuffers") ||
        !GLPROC(p_glBindFramebuffer, "glBindFramebuffer") ||
        !GLPROC(p_glFramebufferTexture2D, "glFramebufferTexture2D") ||
        !GLPROC(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer") ||
        !GLPROC(p_glCheckFramebufferStatus, "glCheckFramebufferStatus") ||
        !GLPROC(p_glGenRenderbuffers, "glGenRenderbuffers") ||
        !GLPROC(p_glBindRenderbuffer, "glBindRenderbuffer") ||
        !GLPROC(p_glRenderbufferStorage, "glRenderbufferStorage") ||
        !GLPROC(p_glRenderbufferStorageMultisample,
                "glRenderbufferStorageMultisample") ||
        !GLPROC(p_glBlitFramebuffer, "glBlitFramebuffer") ||
        !GLPROC(p_glGenerateMipmap, "glGenerateMipmap")) {
        LOGE("glide: OpenGL 3.0 framebuffer objects are required");
        return 0;
    }
    g_fbw = (GLIDE_W + 2 * hplus_ext()) * scale;
    g_fbh = GLIDE_H * scale;

    /* resolved colour + depth/stencil: the read-back and present source */
    glGenTextures(1, &g_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_, g_fbw, g_fbh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &g_fbo_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER_, g_fbo_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER_, GL_DEPTH24_STENCIL8_, g_fbw, g_fbh);
    p_glGenFramebuffers(1, &g_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, g_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                             GL_TEXTURE_2D, g_fbo_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_,
                                GL_RENDERBUFFER_, g_fbo_rb);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        LOGE("glide: frame buffer object incomplete");
        return 0;
    }

    glGetIntegerv(GL_MAX_SAMPLES_, &maxs);
    if (samples > maxs) samples = maxs;
    if (samples > 1) {
        p_glGenRenderbuffers(1, &g_fbo_ms_col);
        p_glBindRenderbuffer(GL_RENDERBUFFER_, g_fbo_ms_col);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER_, samples, GL_RGBA8_,
                                           g_fbw, g_fbh);
        p_glGenRenderbuffers(1, &g_fbo_ms_ds);
        p_glBindRenderbuffer(GL_RENDERBUFFER_, g_fbo_ms_ds);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER_, samples,
                                           GL_DEPTH24_STENCIL8_, g_fbw, g_fbh);
        p_glGenFramebuffers(1, &g_fbo_ms);
        p_glBindFramebuffer(GL_FRAMEBUFFER_, g_fbo_ms);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                                    GL_RENDERBUFFER_, g_fbo_ms_col);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_,
                                    GL_RENDERBUFFER_, g_fbo_ms_ds);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_) !=
            GL_FRAMEBUFFER_COMPLETE_) {
            LOGW("glide: %dx MSAA target incomplete; rendering without", samples);
            p_glBindFramebuffer(GL_FRAMEBUFFER_, g_fbo);
            g_fbo_ms = 0;
            samples = 0;
        }
    }

    /* 640x480 colour-only target the read-back downsamples into */
    p_glGenRenderbuffers(1, &g_fbo_small_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER_, g_fbo_small_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER_, GL_RGBA8_, GLIDE_W, GLIDE_H);
    p_glGenFramebuffers(1, &g_fbo_small);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, g_fbo_small);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                                GL_RENDERBUFFER_, g_fbo_small_rb);

    glGenTextures(1, &g_overlay_tex);
    glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLIDE_W, GLIDE_H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenTextures(1, &g_mask_tex);

    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_, &g_max_aniso);
    if (g_max_aniso > 16.0f) g_max_aniso = 16.0f;

    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    LOGI("glide: frame buffer %dx%d (scale %d), MSAA %dx, anisotropy %.0fx",
         g_fbw, g_fbh, scale, samples > 1 ? samples : 0, (double)g_max_aniso);
    return 1;
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

    if (!fbo_create()) return 0;

    /* Glide's own 640x480 space, y down, mapped onto the whole FBO (plus
     * the Hor+ columns either side) */
    glViewport(0, 0, g_fbw, g_fbh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-g_ext, GLIDE_W + g_ext, GLIDE_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glAlphaFunc(GL_GREATER, 0.5f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &g_blit_tex);

    /* Fog: GL's own equation with the factor inverted, so the game's table is
     * fed in per vertex as a fog coordinate rather than approximated by one
     * of GL's built-in curves. glFogCoordf is GL 1.4, hence the lookup. */
    p_glFogCoordf = (void (APIENTRY *)(GLfloat))
                        SDL_GL_GetProcAddress("glFogCoordf");
    if (!p_glFogCoordf)
        LOGW("glide: no glFogCoordf; fog will not be applied");
    else {
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 0.0f);
        glFogf(GL_FOG_END,   1.0f);
        glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
    }

    shader_init();

    g_gl_ready = 1;
    return 1;
}

/* Expand a paletted texture into RGBA and cache it. The key covers the
 * texture data, the palette and the chroma key, because all three change
 * independently and any of them changes the pixels. */
/* Mipmapped, anisotropic texture parameters for the bound texture. */
static void tex_params(GLint wrap)
{
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (g_max_aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_, g_max_aniso);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

/* The seabed is an atlas of four 128x128 tiles, one per terrain quad, laid
 * down rotated and mirrored. Sampled out of the atlas, every filter reaches
 * across a tile's edge into its neighbour -- bilinear by half a texel, and
 * each mip level and anisotropic tap twice as far as the last -- which draws
 * the tile grid across the whole seabed. Each quadrant therefore also gets a
 * texture of its own, clamped at its edges, for the triangles that stay
 * inside one (see draw_triangle). */
static void tex_make_tiles(texcache_ent *e, const uint32_t *px, int w, int h)
{
    int q;
    if (!e->tile[0]) glGenTextures(4, e->tile);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    for (q = 0; q < 4; q++) {
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, (q & 1) * (w / 2));
        glPixelStorei(GL_UNPACK_SKIP_ROWS, (q >> 1) * (h / 2));
        glBindTexture(GL_TEXTURE_2D, e->tile[q]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w / 2, h / 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, px);
        tex_params(GL_CLAMP_TO_EDGE);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
}

static texcache_ent *tex_upload(const uint8_t *data, int size, uint32_t key)
{
    static uint32_t rgba[256 * 256];
    uint32_t slot = key % TEXCACHE;
    int i, n = size * size;
    GLuint id;

    if (g_texcache[slot].key == key) { g_tex_hit++; return &g_texcache[slot]; }
    g_tex_miss++;

    if (n > 256 * 256) n = 256 * 256;
    for (i = 0; i < n; i++) {
        uint32_t c = g_pal_argb[data[i]];
        /* chroma-keyed texels become transparent and are dropped by the
         * alpha test, which is what Glide's chroma key does */
        rgba[i] = (g_chroma_on && (c & 0x00FFFFFF) == (g_chroma_rgb & 0x00FFFFFF))
                    ? (c & 0x00FFFFFF)
                    : (c | 0xFF000000);
    }

    if (g_texcache[slot].tex) id = g_texcache[slot].tex;
    else glGenTextures(1, &id);

    glBindTexture(GL_TEXTURE_2D, id);
    {   /* Named by content, not by address: the same atlas lands at a
         * different address in every run. A replacement may be any size --
         * texture coordinates are normalised by the original size. */
        char name[48], dname[24];
        uint64_t dh = hd_hash(data, (size_t)n, 0);
        uint32_t *hd = NULL;
        int hw = 0, hh = 0;
        _snprintf(dname, sizeof dname, "%016llx", (unsigned long long)dh);
        _snprintf(name, sizeof name, "%s_%08x%s", dname, g_pal_hash,
                  g_chroma_on ? "k" : "");
        if (hd_dump("tex", name, size, size, rgba))
            LOGI("hd: dumped texture %s (%dx%d)", name, size, size);
        if (g_hd_dir[0] &&
            ((hd = hd_load("tex", name, &hw, &hh)) != NULL ||
             (hd = hd_load("tex", dname, &hw, &hh)) != NULL)) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hw, hh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, hd);
            /* Trilinear and anisotropic: the seabed recedes at a grazing
             * angle for its whole extent, which is exactly where plain
             * bilinear shimmers and smears. */
            tex_params(GL_REPEAT);
            if (size == 256) tex_make_tiles(&g_texcache[slot], hd, hw, hh);
            free(hd);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            tex_params(GL_REPEAT);
            if (size == 256) tex_make_tiles(&g_texcache[slot], rgba, size, size);
        }
    }

    g_texcache[slot].key = key;
    g_texcache[slot].tex = id;
    return &g_texcache[slot];
}

/* Depth starts fresh every frame. Within the frame the ordering is all this
 * game needs, and the stencil (the cockpit mask) is left alone. */
static void back_replay(void);
static int  back_defer(const uint32_t *a, int clear);

static void frame_begin(void)
{
    if (g_frame_started) return;
    back_replay();
    g_frame_started = 1;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

/* The FBO holding the finished pixels: resolve the multisampled target into
 * the single-sample one first if there is one. Leaves the draw FBO bound. */
static GLuint resolve_fbo(void)
{
    if (g_fbo_ms) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_ms);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo);
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, g_fbw, g_fbh, 0, 0, g_fbw, g_fbh,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_FRAMEBUFFER_, g_fbo_ms);
    }
    return g_fbo;
}

/* Draw a rectangle of 640x480-space pixels into the frame buffer. `px` is
 * 0xAABBGGRR, `stride` in pixels; alpha 0 leaves the frame buffer as it was.
 * Plain fixed-function blit: no depth, no stencil, no fog, no clip. */
static void overlay_draw(const uint32_t *px, int stride, int x, int y, int w,
                         int h, int filter_linear)
{
    GLint f = filter_linear ? GL_LINEAR : GL_NEAREST;
    float u0 = x / (float)GLIDE_W, v0 = y / (float)GLIDE_H;
    float u1 = (x + w) / (float)GLIDE_W, v1 = (y + h) / (float)GLIDE_H;

    if (w <= 0 || h <= 0) return;
    if (g_prog) p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex3f((float)x,       (float)y,       0);
        glTexCoord2f(u1, v0); glVertex3f((float)(x + w), (float)y,       0);
        glTexCoord2f(u1, v1); glVertex3f((float)(x + w), (float)(y + h), 0);
        glTexCoord2f(u0, v1); glVertex3f((float)x,       (float)(y + h), 0);
    glEnd();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
}

static uint32_t rgb565_to_abgr(uint16_t v, uint32_t alpha)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) |
           (((b << 3) | (b >> 2)) << 16) | alpha;
}

/* Bring g_vram up to date with the FBO, downsampled to 640x480. */
static void lfb_sync_from_gl(void)
{
    static uint16_t px[GLIDE_W * GLIDE_H];
    int y;

    if (g_vram_valid || !g_gl_ready || !g_vram) return;
    g_vram_valid = 1;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, resolve_fbo());
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo_small);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(g_ext * (g_fbh / GLIDE_H), 0,
                        (g_ext + GLIDE_W) * (g_fbh / GLIDE_H), g_fbh,
                        0, 0, GLIDE_W, GLIDE_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_small);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, GLIDE_W, GLIDE_H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    glEnable(GL_SCISSOR_TEST);

    /* GL reads bottom-up; the game's frame buffer is top-down. */
    for (y = 0; y < GLIDE_H; y++)
        memcpy((uint16_t *)g_vram + (size_t)y * GLIDE_W,
               px + (size_t)(GLIDE_H - 1 - y) * GLIDE_W, GLIDE_W * 2);
    memcpy(g_lfb_snap, g_vram, sizeof g_lfb_snap);
}

/* After an overlay upload at x, y: the bitmap-font glyphs in it, drawn again
 * crisp over their erased originals (hdtext.c). Leaves the state as
 * overlay_draw does. */
static hdtext_glyph g_glyphs[512];

static void hdtext_overlay(int n, int x, int y)
{
    int i;
    if (!n) return;
    glDisable(GL_SCISSOR_TEST);
    for (i = 0; i < n; i++)
        hdtext_draw(&g_glyphs[i], (float)(x + g_glyphs[i].x), (float)(y + g_glyphs[i].y));
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
}

/* Push what the game wrote into g_vram since it was handed out back into the
 * FBO: only the pixels that differ from the snapshot, so 3D the game did not
 * paint over keeps its full resolution. */
static void lfb_flush(void)
{
    static uint32_t rgba[GLIDE_W * GLIDE_H];
    const uint16_t *cur = (const uint16_t *)g_vram;
    int x, y, x0 = GLIDE_W, y0 = GLIDE_H, x1 = -1, y1 = -1, ng, n, i;

    if (!g_lfb_pending || !g_gl_ready || !g_vram) return;
    g_lfb_pending = g_lock_depth > 0;      /* still handed out: keep watching */

    for (y = 0; y < GLIDE_H; y++) {
        const uint16_t *c = cur + (size_t)y * GLIDE_W;
        const uint16_t *s = g_lfb_snap + (size_t)y * GLIDE_W;
        if (!memcmp(c, s, GLIDE_W * 2)) continue;
        if (y < y0) y0 = y;
        y1 = y;
        for (x = 0; x < GLIDE_W; x++)
            if (c[x] != s[x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
    }
    if (x1 < 0) return;
    post_maybe();

    /* one pixel of margin, so a filtered upload samples current neighbours */
    if (x0 > 0) x0--;
    if (y0 > 0) y0--;
    if (x1 < GLIDE_W - 1) x1++;
    if (y1 < GLIDE_H - 1) y1++;
    ng = hdtext_take(g_vram, GLIDE_W * 2, GLIDE_W, GLIDE_H, g_glyphs,
                     (int)(sizeof g_glyphs / sizeof g_glyphs[0]));
    for (i = n = 0; i < ng; i++) {         /* only the ones written just now */
        const hdtext_glyph *g = &g_glyphs[i];
        int k = 0;
        for (y = 0; y < g->h && !k; y++)
            for (x = 0; x < g->w && !k; x++)
                k = g->mask[y * g->w + x] &&
                    cur[(size_t)(g->y + y) * GLIDE_W + g->x + x] !=
                    g_lfb_snap[(size_t)(g->y + y) * GLIDE_W + g->x + x];
        if (k) g_glyphs[n++] = *g;
    }
    ng = n;
    for (i = 0; i < ng; i++) {             /* whole cells: erasing reads them */
        if (g_glyphs[i].x < x0) x0 = g_glyphs[i].x;
        if (g_glyphs[i].y < y0) y0 = g_glyphs[i].y;
        if (g_glyphs[i].x + g_glyphs[i].w - 1 > x1) x1 = g_glyphs[i].x + g_glyphs[i].w - 1;
        if (g_glyphs[i].y + g_glyphs[i].h - 1 > y1) y1 = g_glyphs[i].y + g_glyphs[i].h - 1;
    }
    for (y = y0; y <= y1; y++) {
        const uint16_t *c = cur + (size_t)y * GLIDE_W;
        const uint16_t *s = g_lfb_snap + (size_t)y * GLIDE_W;
        uint32_t *d = rgba + (size_t)y * GLIDE_W;
        for (x = x0; x <= x1; x++)
            d[x] = rgb565_to_abgr(c[x], c[x] != s[x] ? 0xFF000000u : 0);
    }
    for (i = 0; i < ng; i++) hdtext_erase(rgba, GLIDE_W, &g_glyphs[i]);
    overlay_draw(rgba + (size_t)y0 * GLIDE_W + x0, GLIDE_W, x0, y0,
                 x1 + 1 - x0, y1 + 1 - y0, g_smooth2d);
    hdtext_overlay(ng, 0, 0);
    for (y = y0; y <= y1; y++)
        memcpy(g_lfb_snap + (size_t)y * GLIDE_W + x0,
               cur + (size_t)y * GLIDE_W + x0, (size_t)(x1 + 1 - x0) * 2);
}

/* Scissor in 640x480 space, upper-left origin, onto the FBO. */
static void scissor_virtual(int x0, int y0, int x1, int y1)
{
    float sx = g_fbw / (float)GLIDE_VW, sy = g_fbh / (float)GLIDE_H;
    glScissor((GLint)((x0 + g_ext) * sx + 0.5f), (GLint)((GLIDE_H - y1) * sy + 0.5f),
              (GLsizei)((x1 - x0) * sx + 0.5f), (GLsizei)((y1 - y0) * sy + 0.5f));
}

/* The game's clip window. Under Hor+ the full-width 3D view is opened to
 * the whole frame: the sides belong to the 3D alone, and in the middle the
 * stencil keeps the 3D off the rows the 4:3 view never covered (see
 * aux_upload_mask). */
static void scissor_clip(void)
{
    if (g_ext && g_clip_x0 == 0 && g_clip_x1 == GLIDE_W)
        scissor_virtual(-g_ext, 0, GLIDE_W + g_ext, GLIDE_H);
    else
        scissor_virtual(g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1);
}

/* The quadrant (0..3) of a 256 atlas that holds all of the triangle's
 * texture coordinates, or -1 if it spans more than one (models, the tiled
 * backdrop). Coordinates are in texels, s = sow/oow. */
static int triangle_tile(const float *const v[3], const texcache_ent *e)
{
    float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
    int i, qx, qy;
    if (g_tex_size != 256 || !e->tile[0]) return -1;
    for (i = 0; i < 3; i++) {
        float oow = v[i][VOOW], s, t;
        if (oow <= 0.0f) return -1;
        s = v[i][VSOW] / oow;
        t = v[i][VTOW] / oow;
        if (s < s0) s0 = s;
        if (s > s1) s1 = s;
        if (t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    qx = (int)floorf((s0 + s1) * 0.5f / 128.0f);
    qy = (int)floorf((t0 + t1) * 0.5f / 128.0f);
    if (qx < 0 || qx > 1 || qy < 0 || qy > 1) return -1;
    if (s0 < qx * 128.0f - 0.05f || s1 > qx * 128.0f + 128.05f ||
        t0 < qy * 128.0f - 0.05f || t1 > qy * 128.0f + 128.05f) return -1;
    return qy * 2 + qx;
}

/* ---- --fx ssao, bloom: passes over the finished 3D view ------------------
 *
 * Run once a frame, at the first 2D the game draws after its main 3D view
 * (post_maybe), so the HUD and the cockpit never go through them. Both read
 * the depth the w-buffer shader wrote, which is linear in w: with the camera
 * that is a position in camera space for every pixel.
 *
 *   SSAO   a disc of samples around each pixel, at a radius fixed in the
 *          world, counted where they rise above the surface's own plane;
 *          half resolution, blurred, multiplied in.
 *   bloom  what is bright, at a quarter of the resolution, blurred twice
 *          and added.
 *
 * Only pixels the 3D view drew take part (depth below the clear value), and
 * the composite keeps to the cockpit stencil, so the cockpit neither darkens
 * nor glows into the scene. */

static GLuint g_pp_fbo, g_pp_depth_fbo, g_pp_depth, g_pp_tex[4], g_pp_fogtex;
static int    g_pp_w2, g_pp_h2, g_pp_w4, g_pp_h4;
static GLuint g_pp_ao, g_pp_blur, g_pp_bright;
static int    g_pp_failed;
static float  g_cam_proj[6];               /* fx_bits' camera, for the passes */
static int    g_post_3d;
static uint32_t g_post_frame = 0xFFFFFFFFu;

#define GL_DEPTH_COMPONENT_       0x1902
#define GL_DEPTH_STENCIL_        0x84F9
#define GL_UNSIGNED_INT_24_8_    0x84FA
#define GL_DEPTH_ATTACHMENT_     0x8D00

static const char *g_pp_vs =
    "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n";

/* camera-space position of a pixel; uv over the whole frame buffer */
#define PP_VIEWPOS \
    "uniform sampler2D uDepth;\n" \
    "uniform vec4  uProj;\n" \
    "uniform vec2  uRange;\n" \
    "uniform float uExt;\n" \
    "vec3 viewpos(vec2 uv, out float dz)\n" \
    "{\n" \
    "    dz = texture2D(uDepth, uv).r;\n" \
    "    float y = uRange.x + (dz * 65536.0 - 1.0) * (uRange.y - uRange.x) / 65535.0;\n" \
    "    vec2 s = vec2(uv.x * (640.0 + 2.0 * uExt) - uExt, (1.0 - uv.y) * 480.0);\n" \
    "    return vec3((s.x - uProj.z) * y / uProj.x, y, (s.y - uProj.w) * y / uProj.y);\n" \
    "}\n"

static const char *g_pp_ao_fs =
    PP_VIEWPOS
    "uniform vec2 uTexel;\n"                   /* one full-resolution pixel in uv */
    "uniform sampler2D uFogT;\n"               /* the game's fog over depth */
    "uniform int  uAbsorb;\n"                  /* the water's, FX_FOG */
    "void main()\n"
    "{\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    float dz, dq, occ = 0.0;\n"
    "    vec3 p = viewpos(uv, dz);\n"
    "    if (dz >= 0.9999) { gl_FragColor = vec4(1.0); return; }\n"
    "    vec3 n = normalize(cross(viewpos(uv + vec2(uTexel.x, 0.0), dq) - p,\n"
    "                             viewpos(uv + vec2(0.0, uTexel.y), dq) - p));\n"
    "    if (dot(n, p) > 0.0) n = -n;\n"
    "    float R = 40.0 + 0.08 * p.y;\n"           /* world units; a boat is ~200 */
    "    float rad = R * uProj.x / p.y;\n"         /* ... in 640-space pixels */
    "    float a0 = 6.2831 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    for (int i = 0; i < 12; i++) {\n"
    "        float t = (float(i) + 0.5) / 12.0;\n"
    "        float a = a0 + float(i) * 2.39996;\n"
    "        vec2 o = vec2(cos(a), sin(a)) * rad * sqrt(t);\n"
    "        vec3 q = viewpos(uv + o / vec2(640.0 + 2.0 * uExt, 480.0), dq);\n"
    "        vec3 v = q - p;\n"
    "        float l = length(v) + 0.001;\n"
    "        occ += max(dot(v / l, n) - 0.10, 0.0) * (1.0 - smoothstep(R, 2.0 * R, l));\n"
    "    }\n"
    /* The AO multiplies the finished, fogged colour, so it keeps to what
     * the fog and the water left of the surface: a ridge the fog has eaten
     * must not come back as its dark outline. */
    "    float vis = 1.0 - texture2D(uFogT, vec2(dz, 0.5)).a;\n"
    "    if (uAbsorb != 0) vis *= exp(-1.3 * length(p) / uRange.y);\n"
    "    gl_FragColor = vec4(vec3(mix(1.0, clamp(1.0 - 2.2 * occ / 12.0, 0.0, 1.0), vis)), 1.0);\n"
    "}\n";

static const char *g_pp_blur_fs =
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec2 uDir;\n"                     /* one source texel along the axis */
    "uniform int  uBilat;\n"                   /* weigh by depth: AO, not bloom */
    "vec4 tap(vec2 uv, float k, float zc, inout float sum)\n"
    "{\n"
    "    if (uBilat != 0) k *= exp(-abs(texture2D(uDepth, uv).r - zc) / (0.02 * zc));\n"
    "    sum += k;\n"
    "    return texture2D(uSrc, uv) * k;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    float zc = texture2D(uDepth, uv).r, sum = 0.0;\n"
    "    vec4 c = tap(uv, 0.227, zc, sum);\n"
    "    c += tap(uv + 1.385 * uDir, 0.316, zc, sum) + tap(uv - 1.385 * uDir, 0.316, zc, sum);\n"
    "    c += tap(uv + 3.231 * uDir, 0.070, zc, sum) + tap(uv - 3.231 * uDir, 0.070, zc, sum);\n"
    "    gl_FragColor = c / sum;\n"
    "}\n";

static const char *g_pp_bright_fs =
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uDepth;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = gl_TexCoord[0].xy;\n"
    "    vec4 s = texture2D(uColor, uv);\n"
    "    vec3 c = s.rgb;\n"
    "    if (texture2D(uDepth, uv).r >= 0.9999) c = vec3(0.0);\n"
    "    float l = max(c.r, max(c.g, c.b));\n"
    "    gl_FragColor = vec4(c * max(smoothstep(0.70, 1.00, l), 3.0 * s.a), 1.0);\n"
    "}\n";

static GLuint pp_program(const char *fs_src)
{
    GLuint vs = shader_build(GL_VERTEX_SHADER, g_pp_vs);
    GLuint fs = shader_build(GL_FRAGMENT_SHADER, fs_src);
    GLuint pr;
    GLint ok = 0;
    if (!vs || !fs) return 0;
    pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glAttachShader(pr, fs);
    p_glLinkProgram(pr);
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    return ok ? pr : 0;
}

#define GL_RGBA16F_ 0x881A

static GLuint pp_texture(int w, int h, GLint fmt)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int pp_init(void)
{
    if (g_pp_fbo) return 1;
    if (g_pp_failed || !g_prog) return 0;
    g_pp_failed = 1;
    g_pp_ao = pp_program(g_pp_ao_fs);
    g_pp_blur = pp_program(g_pp_blur_fs);
    g_pp_bright = pp_program(g_pp_bright_fs);
    if (!g_pp_ao || !g_pp_blur || !g_pp_bright) { LOGE("fx: post shaders failed"); return 0; }

    /* the depth, resolved: a texture the passes can read */
    glGenTextures(1, &g_pp_depth);
    glBindTexture(GL_TEXTURE_2D, g_pp_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8_, g_fbw, g_fbh, 0,
                 GL_DEPTH_STENCIL_, GL_UNSIGNED_INT_24_8_, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glGenFramebuffers(1, &g_pp_depth_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, g_pp_depth_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, GL_TEXTURE_2D,
                             g_pp_depth, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        LOGE("fx: depth target incomplete");
        p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
        return 0;
    }

    g_pp_w2 = g_fbw / 2; g_pp_h2 = g_fbh / 2;
    g_pp_w4 = g_fbw / 4; g_pp_h4 = g_fbh / 4;
    g_pp_tex[0] = pp_texture(g_pp_w2, g_pp_h2, GL_RGBA8_);    /* AO           */
    g_pp_tex[1] = pp_texture(g_pp_w2, g_pp_h2, GL_RGBA8_);    /* AO, blurring */
    g_pp_tex[2] = pp_texture(g_pp_w4, g_pp_h4, GL_RGBA16F_);  /* bloom        */
    g_pp_tex[3] = pp_texture(g_pp_w4, g_pp_h4, GL_RGBA16F_);  /* bloom, blurring */
    p_glGenFramebuffers(1, &g_pp_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    g_pp_failed = 0;
    LOGI("fx: post passes ready (%dx%d)", g_fbw, g_fbh);
    return 1;
}

/* A quad over the whole of 640x480 space (Hor+ columns included), texture
 * coordinates 0..1 with v up, as textures rendered by GL are stored. */
static void pp_quad(void)
{
    float x0 = (float)-g_ext, x1 = (float)(GLIDE_W + g_ext);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(x0, (float)GLIDE_H, 0);
        glTexCoord2f(1, 0); glVertex3f(x1, (float)GLIDE_H, 0);
        glTexCoord2f(1, 1); glVertex3f(x1, 0,              0);
        glTexCoord2f(0, 1); glVertex3f(x0, 0,              0);
    glEnd();
}

/* Draw `prog` (0: fixed function, texture `src`) into texture `dst` of w x h. */
static void pp_pass(GLuint prog, GLuint dst, int w, int h)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER_, g_pp_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, dst, 0);
    glViewport(0, 0, w, h);
    p_glUseProgram(prog);
    pp_quad();
}

static void pp_blur(GLuint a, GLuint tmp, int w, int h, int bilat, float step)
{
    p_glUseProgram(g_pp_blur);
    p_glUniform1i(p_glGetUniformLocation(g_pp_blur, "uSrc"), 0);
    p_glUniform1i(p_glGetUniformLocation(g_pp_blur, "uDepth"), 1);
    p_glUniform1i(p_glGetUniformLocation(g_pp_blur, "uBilat"), bilat);
    p_glActiveTexture(GL_TEXTURE1_);
    glBindTexture(GL_TEXTURE_2D, g_pp_depth);
    p_glActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, a);
    p_glUniform2f(p_glGetUniformLocation(g_pp_blur, "uDir"), step / w, 0.0f);
    pp_pass(g_pp_blur, tmp, w, h);
    glBindTexture(GL_TEXTURE_2D, tmp);
    p_glUniform2f(p_glGetUniformLocation(g_pp_blur, "uDir"), 0.0f, step / h);
    pp_pass(g_pp_blur, a, w, h);
}

static void post_run(void)
{
    int ssao = (g_fx & FX_SSAO) != 0, bloom = (g_fx & FX_BLOOM) != 0;
    if (!pp_init() || !p_glActiveTexture) return;

    resolve_fbo();                                /* colour, into g_fbo_tex */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_ms ? g_fbo_ms : g_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_pp_depth_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, g_fbw, g_fbh, 0, 0, g_fbw, g_fbh,
                        GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);

    if (ssao) {
        GLubyte fog[256];
        int i;
        for (i = 0; i < 256; i++)              /* depth is w / G_WFAR */
            fog[i] = g_fog_mode == 2 && g_fog_valid
                   ? (GLubyte)(255.0f * fog_factor((i + 0.5f) * (G_WFAR / 256.0f))) : 0;
        if (!g_pp_fogtex) {
            glGenTextures(1, &g_pp_fogtex);
            glBindTexture(GL_TEXTURE_2D, g_pp_fogtex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        p_glActiveTexture(GL_TEXTURE1_);
        glBindTexture(GL_TEXTURE_2D, g_pp_fogtex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 256, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, fog);
        p_glActiveTexture(GL_TEXTURE0_);
        p_glUseProgram(g_pp_ao);
        p_glUniform1i(p_glGetUniformLocation(g_pp_ao, "uFogT"), 1);
        p_glUniform1i(p_glGetUniformLocation(g_pp_ao, "uAbsorb"), (g_fx & FX_FOG) != 0);
        p_glUniform1i(p_glGetUniformLocation(g_pp_ao, "uDepth"), 0);
        p_glUniform4f(p_glGetUniformLocation(g_pp_ao, "uProj"),
                      g_cam_proj[0], g_cam_proj[1], g_cam_proj[2], g_cam_proj[3]);
        p_glUniform2f(p_glGetUniformLocation(g_pp_ao, "uRange"), g_cam_proj[4], g_cam_proj[5]);
        p_glUniform1f(p_glGetUniformLocation(g_pp_ao, "uExt"), (float)g_ext);
        p_glUniform2f(p_glGetUniformLocation(g_pp_ao, "uTexel"), 1.0f / g_fbw, 1.0f / g_fbh);
        glBindTexture(GL_TEXTURE_2D, g_pp_depth);
        pp_pass(g_pp_ao, g_pp_tex[0], g_pp_w2, g_pp_h2);
        pp_blur(g_pp_tex[0], g_pp_tex[1], g_pp_w2, g_pp_h2, 1, 1.0f);
    }
    if (bloom) {
        p_glUseProgram(g_pp_bright);
        p_glUniform1i(p_glGetUniformLocation(g_pp_bright, "uColor"), 0);
        p_glUniform1i(p_glGetUniformLocation(g_pp_bright, "uDepth"), 1);
        p_glActiveTexture(GL_TEXTURE1_);
        glBindTexture(GL_TEXTURE_2D, g_pp_depth);
        p_glActiveTexture(GL_TEXTURE0_);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
        pp_pass(g_pp_bright, g_pp_tex[2], g_pp_w4, g_pp_h4);
        pp_blur(g_pp_tex[2], g_pp_tex[3], g_pp_w4, g_pp_h4, 0, 1.0f);
        pp_blur(g_pp_tex[2], g_pp_tex[3], g_pp_w4, g_pp_h4, 0, 2.5f);
    }

    /* composite into the scene, off the cockpit */
    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    glViewport(0, 0, g_fbw, g_fbh);
    p_glUseProgram(0);
    if (g_mask_ready) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    glEnable(GL_BLEND);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    if (ssao) {
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glBindTexture(GL_TEXTURE_2D, g_pp_tex[0]);
        pp_quad();
    }
    if (bloom) {
        glBlendFunc(GL_ONE, GL_ONE);
        glColor4f(0.7f, 0.7f, 0.7f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, g_pp_tex[2]);
        pp_quad();
        glColor4f(1, 1, 1, 1);
    }

    /* back to the game's state: its blend is re-applied by the next
     * grAlphaBlendFunction, everything else per triangle */
    glBlendFunc(g_blend_src, g_blend_dst);
    if (!g_blend_on) glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    scissor_clip();
    g_vram_valid = 0;
}

/* At the first 2D after the frame's main 3D view: see the section above. */
static void post_maybe(void)
{
    if (!g_post_3d || g_post_frame == g_glide_frame ||
        !(g_fx & (FX_SSAO | FX_BLOOM)))
        return;
    g_post_3d = 0;
    g_post_frame = g_glide_frame;
    post_run();
}

/* --fx for the triangle about to be drawn: the FX_* bits it gets, with the
 * camera uploaded once a frame. Only the main 3D view qualifies -- not the
 * backdrop, not the 60x60 target monitor, which has a view of its own. Bit 8
 * marks blended geometry (no added light), bit 16 additive (absorbed to
 * black rather than to the water's colour). */
static float g_water[3] = { 0.0f, 0.05f, 0.25f };

static int fx_bits(int bias_mode)
{
    static uint32_t frame = 0xFFFFFFFFu;
    static int ok;
    float proj[6], C[9], pos[3];
    int fx;
    if (!g_fx || !g_prog || bias_mode || !g_depth_mode || g_depth_func == 7 ||
        g_clip_x1 - g_clip_x0 < 320)
        return 0;
    if (frame != g_glide_frame) {
        frame = g_glide_frame;
        ok = objprobe_camera(proj, C, pos);
        if (ok) {
            memcpy(g_cam_proj, proj, sizeof g_cam_proj);
            p_glUniform4f(g_u_proj, proj[0], proj[1], proj[2], proj[3]);
            p_glUniform2f(g_u_range, proj[4], proj[5]);
            p_glUniformMatrix3fv(g_u_caminv, 1, GL_FALSE, C);   /* C row-major = C^T */
            p_glUniform3f(g_u_campos, pos[0], pos[1], pos[2]);
            p_glUniform3f(g_u_water, g_water[0], g_water[1], g_water[2]);
            p_glUniform1f(g_u_time, (float)SDL_GetTicks() * 0.001f);
        }
    }
    if (!ok) return 0;
    g_post_3d = 1;
    fx = g_fx & FX_TRI;
    if (!g_mask_ready || g_letterbox) fx &= ~FX_LIGHT;   /* outside views: the camera is not the boat */
    if (g_blend_on) fx |= 8;
    if (g_blend_add) fx |= 16;
    return fx;
}

static uint32_t gl_draw_triangle(uint32_t *a)
{
    const float *v[3];
    int i, bias_mode, tile = -1;

    g_tris++;
    {   /* Rough area, which is all a fill-rate sample needs. */
        const float *q0 = (const float *)(uintptr_t)a[0];
        const float *q1 = (const float *)(uintptr_t)a[1];
        const float *q2 = (const float *)(uintptr_t)a[2];
        float ar = (q1[VX] - q0[VX]) * (q2[VY] - q0[VY])
                 - (q2[VX] - q0[VX]) * (q1[VY] - q0[VY]);
        if (ar < 0.0f) ar = -ar;
        g_ps_in  += (uint32_t)(ar * 0.5f);
        g_ps_out += (uint32_t)(ar * 0.5f);
    }
    if (!gl_ensure_context()) return 0;
    lfb_flush();               /* the game's 2D so far lies under this */
    frame_begin();
    g_vram_valid = 0;

    v[0] = (const float *)(uintptr_t)a[0];
    v[1] = (const float *)(uintptr_t)a[1];
    v[2] = (const float *)(uintptr_t)a[2];
    if (g_objprobe) objprobe_tri(v);

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

    /* --probe X,Y: log every triangle that covers one pixel, in draw order,
     * with the w it interpolates there. The depth buffer only ever shows the
     * surface that won; this shows what else was submitted at that pixel and
     * what depth each one claimed, which is the only way to tell "the
     * occluder was never drawn" from "the occluder lost the test". */
    /* During a traced frame, report the depth span of every triangle: a
     * change of near/far handling between terrain and objects would show up
     * here as a discontinuity that no state call announces. */
    if (g_glidetrace > 0 && (uint32_t)g_glidetrace == g_glide_frame) {
        float w0 = v[0][VOOW] > 0.0f ? 1.0f / v[0][VOOW] : -1.0f;
        float w1 = v[1][VOOW] > 0.0f ? 1.0f / v[1][VOOW] : -1.0f;
        float w2 = v[2][VOOW] > 0.0f ? 1.0f / v[2][VOOW] : -1.0f;
        LOGI("   tri w=%.1f/%.1f/%.1f  xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)  "
             "tex=%08X rgb=%.0f,%.0f,%.0f mode=%d func=%d "
             "st=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)",
             w0, w1, w2, v[0][VX], v[0][VY], v[1][VX], v[1][VY],
             v[2][VX], v[2][VY], (uint32_t)(uintptr_t)g_tex_data,
             v[0][VR], v[0][VG], v[0][VB], g_depth_mode, g_depth_func,
             v[0][VSOW] * w0, v[0][VTOW] * w0, v[1][VSOW] * w1,
             v[1][VTOW] * w1, v[2][VSOW] * w2, v[2][VTOW] * w2);
    }

    if (g_probe_now) {
        float x0 = v[0][VX], y0 = v[0][VY], x1 = v[1][VX], y1 = v[1][VY];
        float x2 = v[2][VX], y2 = v[2][VY];
        float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
        int gx, gy;
        /* A grid rather than one pixel: aiming a specific object onto one
         * exact pixel is not something a human can be asked to do. The
         * texture address identifies which object each surface belongs to,
         * so terrain and models can be told apart in the output. */
        if (area != 0.0f)
        for (gy = 0; gy < 13; gy++) for (gx = 0; gx < 20; gx++) {
            float px = (float)(gx * 32 + 16) + 0.5f;
            float py = (float)(gy * 30 + 25) + 0.5f;
            float l0 = ((x1-px)*(y2-py) - (x2-px)*(y1-py)) / area;
            float l1 = ((x2-px)*(y0-py) - (x0-px)*(y2-py)) / area;
            float l2 = 1.0f - l0 - l1;
            if (l0 >= 0.0f && l1 >= 0.0f && l2 >= 0.0f) {
                float oow = l0*v[0][VOOW] + l1*v[1][VOOW] + l2*v[2][VOOW];
                LOGI("probe (%4d,%4d) w=%9.1f tex=%08X mode=%d func=%d "
                     "mask=%d order=%u",
                     (int)px, (int)py, oow > 0.0f ? 1.0 / oow : -1.0,
                     (uint32_t)(uintptr_t)g_tex_data, g_depth_mode,
                     g_depth_func, g_depth_mask, ++g_probe_hits);
            }
        }
    }

    /* Report each distinct depth state once.    /* Report each distinct depth state once. COMPARE_TO_BIAS is handled as
     * GL_ALWAYS below, which is right for the full-viewport quad the game
     * clears with but would make anything else draw unconditionally, so it
     * matters which geometry actually asks for it. */
    {
        static uint32_t seen[64], nseen;
        uint32_t key = (uint32_t)(g_depth_mode * 1000 + g_depth_func * 100
                                  + (g_depth_mask ? 10 : 0))
                     + (g_depth_bias ? 1u : 0u);
        uint32_t k;
        for (k = 0; k < nseen; k++) if (seen[k] == key) break;
        if (k == nseen && nseen < 64) {
            seen[nseen++] = key;
            LOGI("glide: depth state #%u: mode=%d func=%d mask=%d bias=%u"
                 "%s", nseen, g_depth_mode, g_depth_func, g_depth_mask,
                 g_depth_bias,
                 (g_depth_mode == 3 || g_depth_mode == 4)
                     ? "   <-- COMPARE_TO_BIAS, drawn with GL_ALWAYS" : "");
        }
    }

    glDepthMask(g_depth_mask ? GL_TRUE : GL_FALSE);
    if (g_depth_mode) { glEnable(GL_DEPTH_TEST);
                        glDepthFunc(depth_func_gl(g_depth_func)); }
    else glDisable(GL_DEPTH_TEST);

    /* The cockpit mask applies to every triangle. Those pixels hold depth 0,
     * which rejects the clear quad (bias 0, GR_CMP_NOTEQUAL) and the scene
     * alike (GR_CMP_LESS against 0 never passes), so nothing 3D reaches them. */
    /* ...except where the depth test cannot reject anything. The target
     * monitor's own view is drawn with GR_CMP_ALWAYS into a window that lies
     * entirely inside the masked cockpit; on a Voodoo the zero depth there
     * simply never gets compared. */
    if (g_mask_ready && g_depth_mode && g_depth_func != 7) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    /* GR_DEPTHBUFFER_*_COMPARE_TO_BIAS (3 and 4): per the Glide reference the
     * bias "is used as a pixel's depth value for comparison purposes only...
     * and is not added to the source depth value when writing to the depth
     * buffer" -- so the test is bias-vs-stored while the fragment still
     * writes its own depth. GL has no single-pass equivalent, but the only
     * thing that test decides here is staying off the cockpit, which the
     * stencil now carries; with depth cleared to far each frame the remaining
     * comparison passes everywhere in the viewport. */
    bias_mode = (g_depth_mode == 3 || g_depth_mode == 4);
    if (bias_mode) glDepthFunc(GL_ALWAYS);

    /* grColorCombine(..., other): TEXTURE (2) is a textured surface, anything
     * else is flat/Gouraud. That is the whole distinction this game makes. */
    if (g_cc_other == 1 && g_tex_bound && g_pal_valid) {
        uint32_t key = hash32(hash32(hash32(2166136261u,
                                            (uint32_t)(uintptr_t)g_tex_data),
                                     g_pal_hash),
                              g_chroma_on ? g_chroma_rgb | 0x1000000u : 0u);
        texcache_ent *e;
        if (!key) key = 1;
        glEnable(GL_TEXTURE_2D);
        e = tex_upload(g_tex_data, g_tex_size, key);
        tile = triangle_tile(v, e);
        glBindTexture(GL_TEXTURE_2D, tile >= 0 ? e->tile[tile] : e->tex);
        glEnable(GL_ALPHA_TEST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                  g_cc_factor == 8 ? GL_REPLACE : GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
    }

    /* Fog is applied after colour combining and before alpha blending, so it
     * rides along with the geometry rather than being a separate pass. */
    if (g_fog_mode == 2 && g_fog_valid && p_glFogCoordf) {
        float fr, fg, fb;
        GLfloat c[4];
        glide_unpack(g_fog_color, &fr, &fg, &fb);
        c[0] = fr; c[1] = fg; c[2] = fb; c[3] = 1.0f;
        glFogfv(GL_FOG_COLOR, c);
        glEnable(GL_FOG);
    } else {
        glDisable(GL_FOG);
    }

    /* With the shader active the fixed-function texture environment, alpha
     * test and fog are all replaced by uniforms, so the same state is simply
     * described differently. */
    if (g_prog) {
        int mode = 0;
        if (g_cc_other == 1 && g_tex_bound && g_pal_valid)
            mode = (g_cc_factor == 8) ? 2 : 1;
        p_glUseProgram(g_prog);
        glDisable(GL_ALPHA_TEST);          /* the shader keys, see uAlphaTest */
        p_glUniform1i(g_u_texmode, mode);
        /* --glidedebug 16: no alpha test. If the leak disappears, terrain
         * fragments are being keyed away -- they then write neither colour
         * nor depth, and whatever sits behind them shows through, which no
         * amount of depth correctness can prevent. */
        p_glUniform1i(g_u_alphatest,
                      (mode && g_glidedebug != 16) ? 1 : 0);
        p_glUniform1f(g_u_wfar, G_WFAR);
        p_glUniform1i(g_u_backdrop, bias_mode ? 1 : 0);
        /* the water's colour is the backdrop's: what fades out must fade to it */
        if (bias_mode && (g_cc_local == 1 || g_cc_other == 2))
            glide_unpack(g_const_color, &g_water[0], &g_water[1], &g_water[2]);
        p_glUniform1i(g_u_fx, fx_bits(bias_mode));
        if (g_fog_mode == 2 && g_fog_valid) {
            float fr, fg, fb;
            glide_unpack(g_fog_color, &fr, &fg, &fb);
            p_glUniform1i(g_u_fog, 1);
            p_glUniform3f(g_u_fogcolor, fr, fg, fb);
        } else {
            p_glUniform1i(g_u_fog, 0);
        }
    }

    glBegin(GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        const float *p = v[i];
        float oow = p[VOOW];
        float w   = oow > 0.0f ? 1.0f / oow : G_WFAR;
        /* Hyperbolic in w, i.e. linear in oow -- see G_WNEAR/G_WFAR above.
         * This is the term that has to interpolate correctly across the
         * triangle, and oow is the quantity that does. */
        float z   = G_DEPTH_A + G_DEPTH_B * (oow > 0.0f ? oow : 1.0f / G_WFAR);
        if (z > 1.0f) z = 1.0f;
        if (z < 0.0f) z = 0.0f;
        if (g_fog_mode == 2 && g_fog_valid && p_glFogCoordf)
            p_glFogCoordf(fog_factor(w));
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
        if (tile >= 0)                       /* the quadrant's own texture */
            glTexCoord4f((p[VSOW] - (float)((tile & 1) * 128) * oow) / 128.0f,
                         (p[VTOW] - (float)((tile >> 1) * 128) * oow) / 128.0f,
                         0.0f, oow);
        else
            glTexCoord4f(p[VSOW] / (float)g_tex_size, p[VTOW] / (float)g_tex_size,
                         0.0f, oow);
        /* glOrtho(.., -1, 1) gives z_ndc = -z_eye, so the eye-space value has
         * to be negated to land at window depth z. Getting this backwards put
         * the black full-viewport quad at depth 0 -- nearest -- where it
         * occluded the entire scene behind it. */
        /* The quad the viewport is cleared with spans exactly the game's
         * 640 columns; under Hor+ it has to clear the sides as well. */
        if (bias_mode && g_ext && (p[VX] <= 0.0f || p[VX] >= (float)GLIDE_W))
            glVertex3f(p[VX] <= 0.0f ? (float)-g_ext : (float)(GLIDE_W + g_ext),
                       p[VY] <= (float)g_clip_y0 ? 0.0f :
                       p[VY] >= (float)g_clip_y1 ? (float)GLIDE_H : p[VY],
                       1.0f - z * 2.0f);
        else
            glVertex3f(p[VX], p[VY], 1.0f - z * 2.0f);
    }
    glEnd();
    return 0;
}

/* objprobe.c's replacement models: one Gouraud triangle in screen space
 * (x, y, oow, r, g, b), through the game's own path -- depth, fog, cockpit
 * stencil -- untextured and unblended whatever the game last set. */
void glide_tri_rgb(const float p[3][6])
{
    float gv[3][15];
    uint32_t a[3];
    int cc_other = g_cc_other, cc_local = g_cc_local, i;
    GLboolean blend = glIsEnabled(GL_BLEND);
    memset(gv, 0, sizeof gv);
    for (i = 0; i < 3; i++) {
        gv[i][VX] = p[i][0]; gv[i][VY] = p[i][1]; gv[i][VOOW] = p[i][2];
        gv[i][VR] = p[i][3]; gv[i][VG] = p[i][4]; gv[i][VB] = p[i][5];
        gv[i][VA] = 255.0f;
        a[i] = (uint32_t)(uintptr_t)gv[i];
    }
    g_cc_other = 0; g_cc_local = 0;
    glDisable(GL_BLEND);
    gl_draw_triangle(a);
    if (blend) glEnable(GL_BLEND);
    g_cc_other = cc_other; g_cc_local = cc_local;
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

static void aux_upload_mask(void);

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
    scissor_clip();
    if (g_ext && g_clip_x0 == 0 && g_clip_x1 == GLIDE_W &&
        (g_view_y0 != g_clip_y0 || g_view_y1 != g_clip_y1)) {
        g_view_y0 = g_clip_y0;
        g_view_y1 = g_clip_y1;
        if (g_mask_ready) aux_upload_mask();
    }
    LOGI("glide: clip window %u,%u .. %u,%u", a[0], a[1], a[2], a[3]);
    return 0;
}

static uint32_t gl_depth_mode(uint32_t *a)  { g_depth_mode = (int)a[0]; return 0; }
static uint32_t gl_depth_func(uint32_t *a)  { g_depth_func = (int)a[0]; return 0; }
static uint32_t gl_depth_mask(uint32_t *a)  { g_depth_mask = (int)a[0]; return 0; }
static uint32_t gl_depth_bias(uint32_t *a)  { g_depth_bias = a[0] & 0xFFFF; return 0; }
static uint32_t gl_fog_mode(uint32_t *a)    { g_fog_mode = (int)(a[0] & 0xFF); return 0; }
static uint32_t gl_fog_color(uint32_t *a)   { g_fog_color = a[0]; return 0; }

static uint32_t gl_fog_table(uint32_t *a)
{
    const uint8_t *t = (const uint8_t *)(uintptr_t)a[0];
    if (!t) return 0;
    memcpy(g_fog_table, t, GR_FOG_TABLE_SIZE);
    g_fog_valid = getenv("SFP_NOFOG") == NULL;
    {   char line[GR_FOG_TABLE_SIZE * 4 + 1];
        int i, n = 0;
        for (i = 0; i < GR_FOG_TABLE_SIZE; i++)
            n += sprintf(line + n, "%u ", g_fog_table[i]);
        LOGI("glide: fog table: %s", line);
    }
    LOGI("glide: fog table loaded (%u..%u over w %.0f..%.0f)",
         g_fog_table[0], g_fog_table[GR_FOG_TABLE_SIZE - 1],
         g_fog_w[0], g_fog_w[GR_FOG_TABLE_SIZE - 1]);
    return 0;
}
static uint32_t gl_chroma_mode(uint32_t *a) { g_chroma_on  = (int)a[0]; return 0; }

/* grTexClampMode(tmu, s_clampmode, t_clampmode) */
static uint32_t gl_tex_clamp(uint32_t *a)
{
    g_clamp_s = (int)a[1]; g_clamp_t = (int)a[2];
    return 0;
}

/* grTexFilterMode(tmu, minfilter, magfilter) */
static uint32_t gl_tex_filter(uint32_t *a)
{
    g_filter_min = (int)a[1]; g_filter_mag = (int)a[2];
    return 0;
}
/* grChromakeyValue takes a GrColor_t, so it is in the colour format
 * grSstWinOpen selected -- the same unpacking every other colour needs.
 * Comparing the raw dword against palette entries matches nothing. */
static uint32_t gl_chroma_value(uint32_t *a)
{
    float r, g, b;
    g_chroma_val = a[0];
    glide_unpack(a[0], &r, &g, &b);
    {   static uint32_t last = 0xDEADBEEFu;
        if (a[0] != last) {
            last = a[0];
            LOGI("glide: chroma key value 0x%08X (fmt %d)", a[0], g_color_fmt);
        }
    }
    g_chroma_rgb = ((uint32_t)(r * 255.0f + 0.5f))
                 | ((uint32_t)(g * 255.0f + 0.5f) << 8)
                 | ((uint32_t)(b * 255.0f + 0.5f) << 16);
    return 0;
}
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
    g_blend_on = !(a[0] == 4 && a[1] == 0);
    g_blend_add = g_blend_on && a[1] == 4;               /* dst ONE */
    if (a[0] == 4 && a[1] == 0) glDisable(GL_BLEND);    /* ONE, ZERO */
    else {
        g_blend_src = blend_gl((int)a[0]);
        g_blend_dst = blend_gl((int)a[1]);
        glEnable(GL_BLEND);
        glBlendFunc(g_blend_src, g_blend_dst);
    }
    return 0;
}

static void aux_init(void);
static void aux_upload_mask(void);

/* grBufferClear(color, alpha, depth) */
static uint32_t gl_buffer_clear(uint32_t *a)
{
    if (!gl_ensure_context()) return 0;
    if (back_defer(a, 1)) return 0;
    glDepthMask(GL_TRUE);
    {   float cr, cg, cb;
        glide_unpack(a[0], &cr, &cg, &cb);
        glClearColor(cr, cg, cb, 1.0f);
    }
    glClearDepth(1.0);
    lfb_flush();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g_vram_valid = 0;

    /* A real clear writes the depth value over the aux buffer as well, and
     * with it over the cockpit mask's zeros. The outside views (chase, look
     * at, death) clear this way every frame instead of drawing the masked
     * clear quad, and must not keep the cockpit's silhouette. */
    if (g_depth_mask) {
        int x, y;
        aux_init();
        for (y = g_clip_y0; y < g_clip_y1; y++)
            for (x = g_clip_x0; x < g_clip_x1; x++)
                g_auxbuf[y * GLIDE_W + x] = (uint16_t)a[2];
        if (a[2] && g_clip_x0 == 0 && g_clip_y0 == 0 &&
            g_clip_x1 == GLIDE_W && g_clip_y1 == GLIDE_H)
            g_mask_ready = 0;
        else if (g_mask_ready)
            aux_upload_mask();
    }
    return 0;
}


/* A window wider than the 16:9 cockpit art: the columns beyond its edges
 * are masked off (aux_upload_mask) and drawn black (hd_image_draw), so the
 * scene does not run on past the cockpit. */
static void beyond_art_quads(float qx0, float qx1)
{
    float e0 = (float)-g_ext, e1 = (float)(GLIDE_W + g_ext);
    glBegin(GL_QUADS);
    if (qx0 > e0) {
        glVertex3f(e0, 0, 0);  glVertex3f(qx0, 0, 0);
        glVertex3f(qx0, GLIDE_H, 0);  glVertex3f(e0, GLIDE_H, 0);
    }
    if (qx1 < e1) {
        glVertex3f(qx1, 0, 0);  glVertex3f(e1, 0, 0);
        glVertex3f(e1, GLIDE_H, 0);  glVertex3f(qx1, GLIDE_H, 0);
    }
    glEnd();
}

/* Turn the aux buffer the game wrote into a stencil mask.
 *
 * This is the cockpit mask, and the Glide reference spells out how it works.
 * grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER_COMPARE_TO_BIAS): "the bias
 * specified with grDepthBiasLevel is used as a pixel's depth value for
 * comparison purposes only... and is not added to the source depth value when
 * writing to the depth buffer." So the quad the game clears the viewport with
 * is tested as bias-vs-stored (NOTEQUAL against bias 0) while still writing
 * its own depth.
 *
 * The game sets that up by locking GR_BUFFER_AUXBUFFER once and writing 0
 * over the cockpit -- measured: 172172 pixels zeroed, the rest left at the
 * far value, nothing in between. Depth 0 rejects the clear quad (0 != 0 is
 * false) and equally rejects the scene, which tests GR_CMP_LESS against it.
 * So those pixels take no 3D at all, which is precisely a stencil mask.
 *
 * Expressing it as one is what makes it work here: GL tests incoming against
 * stored, so "test against a constant while writing the fragment's depth"
 * has no single-pass equivalent, whereas a stencil does exactly the right
 * thing and leaves depth free to be cleared per frame -- which double
 * buffering requires anyway. */
static void aux_init(void)
{
    static int done;
    size_t i;
    if (done) return;
    done = 1;
    for (i = 0; i < (size_t)GLIDE_W * GLIDE_H; i++) g_auxbuf[i] = 0xFFFF;
}

static void aux_upload_mask(void)
{
    static GLubyte mask[GLIDE_W * GLIDE_H];
    uint32_t zero = 0;
    int i;

    if (!g_gl_ready) return;
    for (i = 0; i < GLIDE_W * GLIDE_H; i++) {
        mask[i] = g_auxbuf[i] == 0 ? 255 : 0;      /* 255 = masked off */
        zero += g_auxbuf[i] == 0;
    }
    if (g_ext && g_letterbox)               /* outside views: see letterbox_select */
        memset(mask, 0, sizeof mask);
    else if (g_ext && g_view_y0 >= 0 && !g_cockpit_cur) /* Hor+: see scissor_clip */
        for (i = 0; i < GLIDE_W * GLIDE_H; i++)
            if (i / GLIDE_W < g_view_y0 || i / GLIDE_W >= g_view_y1) mask[i] = 255;

    /* Stamped into the stencil at the frame buffer's own resolution, by a
     * full-screen quad that only survives the alpha test where masked. */
    glBindTexture(GL_TEXTURE_2D, g_mask_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, GLIDE_W, GLIDE_H, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, mask);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    {   /* a 16:9 cockpit brings its own mask: its alpha, over its own span */
        float qx0 = 0.0f, qx1 = (float)GLIDE_W;
        if (g_cockpit_cur) {
            glBindTexture(GL_TEXTURE_2D, g_cockpit_cur);
            qx0 = GLIDE_W * 0.5f - g_cockpit_hw;
            qx1 = GLIDE_W * 0.5f + g_cockpit_hw;
        }

    if (g_prog) p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_FOG);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(qx0, 0,       0);
        glTexCoord2f(1, 0); glVertex3f(qx1, 0,       0);
        glTexCoord2f(1, 1); glVertex3f(qx1, GLIDE_H, 0);
        glTexCoord2f(0, 1); glVertex3f(qx0, GLIDE_H, 0);
    glEnd();
    if (g_cockpit_cur) {
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_TEXTURE_2D);
        beyond_art_quads(qx0, qx1);
        glEnable(GL_TEXTURE_2D);
    }
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    g_mask_ready = 1;
    LOGI("glide: cockpit mask from aux buffer: %u of %u pixels masked off",
         zero, (uint32_t)(GLIDE_W * GLIDE_H));
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
        /* The aux buffer is persistent, as the Voodoo's depth buffer is. It
         * starts at the far value (see aux_init), so a lock the game never
         * writes to keeps the depth grBufferClear established. Refilling it on
         * every lock was wrong: with a target locked the game stamps zeros
         * over just the 60x60 target monitor each frame and expects the
         * cockpit zeros from startup to still be there -- refilling erased
         * them, and the viewport clear then painted over the whole cockpit. */
        aux_init();
        g_aux_locked = 1;
        info[1] = (uint32_t)(uintptr_t)g_auxbuf;
        info[2] = GLIDE_W * 2;
        info[3] = a[2];
        info[4] = a[3];
        return 1;
    }

    if (!g_vram) return 0;
    post_maybe();
    lfb_flush();
    lfb_sync_from_gl();
    g_lfb_pending = 1;
    g_lock_depth++;
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
        aux_upload_mask();
    } else if (a[1] != 2 && g_lock_depth > 0) {
        g_lock_depth--;
        lfb_flush();
    }
    return 1;
}

/* The FBO is the finished frame: scale it into the window, 4:3 and centred,
 * and swap. Nothing is cleared -- the frame buffer is persistent. */
/* Widescreen cockpit art: <hd>/wide/<name>.png is a full-screen image widened
 * to 16:9 (tools/outpaint_cockpit.py). Its sides are drawn beside the 4:3
 * frame while that image is the cockpit on screen; alpha fades it into the
 * frame at the seam. g_wide_cur is the one for the last full-screen image. */
static GLuint g_wide_cur;
static int    g_wide_w, g_wide_h;

/* A script shot also saves the whole window (<shot>_win.bmp) when the
 * widescreen sides are on it, since the frame buffer alone is 4:3. */
static char g_wide_shot[MAX_PATH + 8];

static void shot_window(const char *path, int w, int h)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 3);
    if (!px) return;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, 0x80E0 /* GL_BGR */, GL_UNSIGNED_BYTE, px);
    write_bmp24(path, w, h, px, w * 3);
    free(px);
}

static void present(void)
{
    int dw = g_fbw, dh = g_fbh, vw, vh, vx, vy;
    GLuint src = resolve_fbo();

    SDL_GL_GetDrawableSize(video_window(), &dw, &dh);
    if ((long)dw * g_fbh > (long)dh * g_fbw) { vh = dh; vw = (int)((long)dh * g_fbw / g_fbh); }
    else                                     { vw = dw; vh = (int)((long)dw * g_fbh / g_fbw); }
    vx = (dw - vw) / 2;
    vy = (dh - vh) / 2;

    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, src);
    p_glBlitFramebuffer(0, 0, g_fbw, g_fbh, vx, vy, vx + vw, vy + vh,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Cockpit sides, over the bars and faded into the frame's edges. Only
     * while the cockpit mask is up: the outside views have no cockpit. And
     * only for a 4:3 frame: under Hor+ the cockpit is in the frame already,
     * and a window wider still would get a second one over it. */
    if (g_wide_cur && g_mask_ready && dw > vw && g_wide_h && !g_ext) {
        int ww = (int)((long)vh * g_wide_w / g_wide_h);
        if (g_prog) p_glUseProgram(0);
        glViewport((dw - ww) / 2, vy, ww, vh);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_wide_cur);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(0,       0,       0);
            glTexCoord2f(1, 0); glVertex3f(GLIDE_W, 0,       0);
            glTexCoord2f(1, 1); glVertex3f(GLIDE_W, GLIDE_H, 0);
            glTexCoord2f(0, 1); glVertex3f(0,       GLIDE_H, 0);
        glEnd();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glDisable(GL_BLEND);
        glViewport(0, 0, g_fbw, g_fbh);
    }
    if (g_wide_shot[0]) shot_window(g_wide_shot, dw, dh);
    g_wide_shot[0] = 0;
    SDL_GL_SwapWindow(video_window());
    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    glEnable(GL_SCISSOR_TEST);
}

/* The screenshot key also notes what the renderer was doing, as <shot>.txt. */
static void shot_note(const char *bmp)
{
    char path[MAX_PATH + 16];
    float proj[6], C[9], pos[3];
    int ok = objprobe_camera(proj, C, pos), dw = 0, dh = 0;
    size_t n = strlen(bmp);
    FILE *f;
    if (n < 4 || n + 1 > sizeof path) return;
    memcpy(path, bmp, n - 4);
    strcpy(path + n - 4, ".txt");
    if (!(f = fopen(path, "w"))) return;
    SDL_GL_GetDrawableSize(video_window(), &dw, &dh);
    fprintf(f, "frame %u\nfx 0x%X\nwindow %dx%d\nframe buffer %dx%d, Hor+ %d\n",
            g_glide_frame, g_fx, dw, dh, g_fbw, g_fbh, g_ext);
    fprintf(f, "letterbox %d, mask %d, cockpit art %d, side art %d\n",
            g_letterbox, g_mask_ready, g_cockpit_cur != 0, g_wide_cur != 0);
    fprintf(f, "water %.3f %.3f %.3f\n", g_water[0], g_water[1], g_water[2]);
    if (ok)
        fprintf(f, "camera fx %.1f fy %.1f cx %.1f cy %.1f near %.1f far %.1f\n"
                   "position %.1f %.1f %.1f\n"
                   "matrix %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f\n",
                proj[0], proj[1], proj[2], proj[3], proj[4], proj[5], pos[0], pos[1], pos[2],
                C[0], C[1], C[2], C[3], C[4], C[5], C[6], C[7], C[8]);
    else
        fprintf(f, "camera none\n");
    fclose(f);
}

/* script "shot": the frame buffer at its own resolution, not the window. */
static void shot_from_fbo(const char *path)
{
    uint8_t *px = (uint8_t *)malloc((size_t)g_fbw * g_fbh * 3);
    if (!px) return;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, resolve_fbo());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_fbw, g_fbh, 0x80E0 /* GL_BGR */, GL_UNSIGNED_BYTE, px);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, draw_fbo());
    write_bmp24(path, g_fbw, g_fbh, px, g_fbw * 3);   /* both bottom-up */
    free(px);
}

/* Show the frame buffer as it stands: a buffer swap, or a write to the
 * front buffer (see gl_lfb_write_region). */
static void frame_present(void)
{
    lfb_flush();
    if (script_shot_wanted()) {
        const char *p = script_shot_wanted();
        size_t n = strlen(p);
        shot_from_fbo(p);
        if (script_shot_is_hotkey()) shot_note(p);
        if (n > 4 && n < sizeof g_wide_shot - 4) {
            memcpy(g_wide_shot, p, n - 4);                /* drop ".bmp" */
            strcpy(g_wide_shot + n - 4, "_win.bmp");
        }
        script_shot_done();
    }
    /* A frame without 3D (menus, briefings) leaves the Hor+ columns
     * holding the last scene; they belong to no 2D the game draws, unless
     * a 16:9 cockpit is up, which is drawn once and owns them. */
    if (g_ext && !g_frame_started && !g_cockpit_cur) {
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        scissor_virtual(-g_ext, 0, 0, GLIDE_H);
        glClear(GL_COLOR_BUFFER_BIT);
        scissor_virtual(GLIDE_W, 0, GLIDE_W + g_ext, GLIDE_H);
        glClear(GL_COLOR_BUFFER_BIT);
        scissor_clip();
    }
    present();
}

static uint32_t gl_buffer_swap(uint32_t *a)
{
    (void)a;
    back_replay();
    post_maybe();
    g_glide_frame++;
    hdtext_frame();
    if (!gl_ensure_context()) return 0;
    frame_present();

    /* End of frame -- deliberately not grBufferClear. This game calls
     * grBufferClear exactly once for the entire run and clears each frame by
     * drawing a full-viewport quad with GR_CMP_NOTEQUAL instead. */
    if ((g_glide_frame % 500) == 0)
        LOGI("glide: frame %u, %u triangles, tex %u hit / %u miss, pal=%d",
             g_glide_frame, g_tris, g_tex_hit, g_tex_miss, g_pal_valid);
    g_frame_started = 0;
    return 0;
}

/* A whole image the game blits (cockpit art, monitor backgrounds) may have a
 * high-resolution replacement, named by a hash of its pixels and size. Those
 * are drawn into the frame buffer at full resolution instead. Returns 0 if
 * there is none, and the caller draws the original. */
#define HD2D_CACHE 4096
static struct { uint64_t key; GLuint tex, wide, cockpit; float cockpit_hw;
                int missing, seen; } g_hd2d[HD2D_CACHE];

/* Widescreen cockpit art (g_wide_cur, see present): one texture per
 * full-screen image that has a <hd>/wide/ version. */
static struct { GLuint tex; int w, h; } g_wide_size[16];

static GLuint hd_texture(const uint32_t *px, int w, int h)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

static void wide_select(GLuint tex)
{
    int i;
    g_wide_cur = tex;
    for (i = 0; i < 16; i++)
        if (g_wide_size[i].tex == tex) { g_wide_w = g_wide_size[i].w; g_wide_h = g_wide_size[i].h; }
}

static void cockpit_select(GLuint tex, float hw);

static void letterbox_select(int on)
{
    if (on == g_letterbox) return;
    g_letterbox = on;
    if (g_mask_ready) aux_upload_mask();
}

static void cockpit_select(GLuint tex, float hw)
{
    if (tex == g_cockpit_cur) return;
    g_cockpit_cur = tex;
    g_cockpit_hw = hw;
    if (g_mask_ready) aux_upload_mask();       /* the mask follows the art */
}

/* A screen rectangle that keeps showing new pictures is a live display (the
 * cockpit monitors are composed in software each frame), not art: after a
 * few distinct contents it is no longer hashed, dumped or replaced. */
#define HD2D_REGIONS 64
#define HD2D_LIVE    8
static struct { int x, y, w, h, distinct; } g_hd2d_rgn[HD2D_REGIONS];

static int *hd_region(int x, int y, int w, int h)
{
    static int overflow = HD2D_LIVE + 1;
    int i;
    for (i = 0; i < HD2D_REGIONS; i++) {
        if (!g_hd2d_rgn[i].w) {
            g_hd2d_rgn[i].x = x; g_hd2d_rgn[i].y = y;
            g_hd2d_rgn[i].w = w; g_hd2d_rgn[i].h = h;
        }
        if (g_hd2d_rgn[i].x == x && g_hd2d_rgn[i].y == y &&
            g_hd2d_rgn[i].w == w && g_hd2d_rgn[i].h == h)
            return &g_hd2d_rgn[i].distinct;
    }
    return &overflow;
}

static int hd_image_draw(const uint32_t *rgba, int x, int y, int w, int h)
{
    char name[48];
    uint64_t key;
    uint32_t slot, probe;
    GLuint tex = 0;
    int *distinct;

    if (w * h < 32 * 32) return 0;            /* glyphs and lamps: not worth it */
    distinct = hd_region(x, y, w, h);
    if (*distinct > HD2D_LIVE) return 0;
    key = hd_hash(rgba, (size_t)w * h * 4, (uint64_t)w * 65536u + (uint64_t)h);
    _snprintf(name, sizeof name, "%016llx_%dx%d", (unsigned long long)key, w, h);

    for (probe = 0; probe < HD2D_CACHE; probe++) {
        slot = (uint32_t)((key + probe) % HD2D_CACHE);
        if (g_hd2d[slot].key == key) {
            /* Only images that come back are art; the monitors redraw a
             * different picture every frame and would flood the dump. */
            if (++g_hd2d[slot].seen == 3 && hd_dump("2d", name, w, h, rgba))
                LOGI("hd: dumped image %s at %d,%d", name, x, y);
            if (w == GLIDE_W && h == GLIDE_H) {
                wide_select(g_hd2d[slot].wide);
                cockpit_select(g_hd2d[slot].cockpit, g_hd2d[slot].cockpit_hw);
            }
            if (g_hd2d[slot].missing && !g_hd2d[slot].cockpit) return 0;
            tex = g_hd2d[slot].tex;
            break;
        }
        if (!g_hd2d[slot].key) {
            int iw = 0, ih = 0;
            uint32_t *px = g_hd_dir[0] ? hd_load("2d", name, &iw, &ih) : NULL;
            if (++*distinct > HD2D_LIVE) {
                LOGI("hd: %dx%d at %d,%d is a live display; not replaced",
                     w, h, x, y);
                free(px);
                return 0;
            }
            g_hd2d[slot].key = key;
            g_hd2d[slot].seen = 1;
            if (w * h >= 320 * 240 && hd_dump("2d", name, w, h, rgba))
                LOGI("hd: dumped image %s at %d,%d", name, x, y);
            if (w == GLIDE_W && h == GLIDE_H) {
                int ww = 0, wh = 0, i;
                uint32_t *wp = g_hd_dir[0] ? hd_load("wide", name, &ww, &wh) : NULL;
                if (wp) {
                    g_hd2d[slot].wide = hd_texture(wp, ww, wh);
                    free(wp);
                    for (i = 0; i < 16 && g_wide_size[i].tex; i++) {}
                    if (i < 16) {
                        g_wide_size[i].tex = g_hd2d[slot].wide;
                        g_wide_size[i].w = ww; g_wide_size[i].h = wh;
                    }
                }
                wide_select(g_hd2d[slot].wide);
                wp = g_ext && g_hd_dir[0] ? hd_load("cockpit", name, &ww, &wh) : NULL;
                if (wp) {
                    g_hd2d[slot].cockpit = hd_texture(wp, ww, wh);
                    g_hd2d[slot].cockpit_hw = GLIDE_H * 0.5f * ww / wh;
                    free(wp);
                    LOGI("hd: 16:9 cockpit for %s (%dx%d)", name, ww, wh);
                }
                cockpit_select(g_hd2d[slot].cockpit, g_hd2d[slot].cockpit_hw);
            }
            if (!px) {
                g_hd2d[slot].missing = 1;
                if (!g_hd2d[slot].cockpit) return 0;
                break;
            }
            tex = hd_texture(px, iw, ih);
            free(px);
            g_hd2d[slot].tex = tex;
            break;
        }
    }
    {   float qx0 = (float)x, qx1 = (float)(x + w);
    if (w == GLIDE_W && h == GLIDE_H && g_cockpit_cur) {
        tex = g_cockpit_cur;
        qx0 = GLIDE_W * 0.5f - g_cockpit_hw;
        qx1 = GLIDE_W * 0.5f + g_cockpit_hw;
    }
    if (!tex) return 0;

    if (g_prog) p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(qx0, (float)y,       0);
        glTexCoord2f(1, 0); glVertex3f(qx1, (float)y,       0);
        glTexCoord2f(1, 1); glVertex3f(qx1, (float)(y + h), 0);
        glTexCoord2f(0, 1); glVertex3f(qx0, (float)(y + h), 0);
    glEnd();
    if (tex == g_cockpit_cur) {
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        glColor4f(0, 0, 0, 1);
        beyond_art_quads(qx0, qx1);
        glColor4f(1, 1, 1, 1);
        glEnable(GL_TEXTURE_2D);
    }
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    return 1;
}

/* (dst_buffer, dst_x, dst_y, src_format, w, h, src_stride, src_data). The
 * full-screen ones are the cockpit art and are drawn filtered. */
/* There is one frame buffer here, the Voodoo has two. While the game shows a
 * front-buffer image (the briefing, see the end of gl_lfb_write_region) it
 * also prepares the next frame in the back buffer -- the briefing clears it
 * and paints the plain cockpit there -- which here would land on the image
 * on screen. Those clears and writes wait until the game swaps or starts its
 * 3D.
 * ponytail: back-buffer reads in the meantime see the front image; the game
 * makes none. */
#define BACKQ 8
static struct { uint32_t a[8]; uint8_t *px; int clear; } g_backq[BACKQ];
static int g_nbackq, g_front_shown;

static uint32_t gl_lfb_write_region(uint32_t *a);

static void back_replay(void)
{
    int i, n = g_nbackq;
    g_front_shown = 0;
    g_nbackq = 0;
    for (i = 0; i < n; i++) {
        if (g_backq[i].clear) gl_buffer_clear(g_backq[i].a);
        else                  gl_lfb_write_region(g_backq[i].a);
        free(g_backq[i].px);
    }
}

/* a: grBufferClear's arguments if clear, else grLfbWriteRegion's */
static int back_defer(const uint32_t *a, int clear)
{
    uint32_t w = a[4], h = a[5], r;
    const uint8_t *src = (const uint8_t *)(uintptr_t)a[7];
    uint8_t *px;
    if (!g_front_shown) return 0;
    if (!clear && (a[0] != 1 || !src || w > GLIDE_W || h > GLIDE_H)) return 0;
    if (g_nbackq == BACKQ) { back_replay(); return 0; }
    if (clear) {
        memcpy(g_backq[g_nbackq].a, a, 3 * sizeof a[0]);
        g_backq[g_nbackq].px = NULL;
        g_backq[g_nbackq++].clear = 1;
        return 1;
    }
    if (!(px = (uint8_t *)malloc((size_t)w * h * 2))) return 0;
    for (r = 0; r < h; r++)
        memcpy(px + (size_t)r * w * 2, src + (int32_t)r * (int32_t)a[6], w * 2);
    memcpy(g_backq[g_nbackq].a, a, sizeof g_backq[0].a);
    g_backq[g_nbackq].a[6] = w * 2;
    g_backq[g_nbackq].a[7] = (uint32_t)(uintptr_t)px;
    g_backq[g_nbackq].clear = 0;
    g_backq[g_nbackq++].px = px;
    return 1;
}

static uint32_t gl_lfb_write_region(uint32_t *a)
{
    static uint32_t rgba[GLIDE_W * GLIDE_H];
    const uint8_t *src = (const uint8_t *)(uintptr_t)a[7];
    uint32_t x = a[1], y = a[2], w = a[4], h = a[5], r, c;
    int32_t  stride = (int32_t)a[6];

    if (!src || !g_vram || !gl_ensure_context()) return 0;
    if (back_defer(a, 0)) return 1;
    if (x >= GLIDE_W || y >= GLIDE_H) return 0;
    if (w > GLIDE_W - x) w = GLIDE_W - x;
    if (h > GLIDE_H - y) h = GLIDE_H - y;
    post_maybe();
    lfb_flush();               /* earlier CPU writes lie under this one */

    for (r = 0; r < h; r++) {
        const uint16_t *s = (const uint16_t *)(src + (int32_t)r * stride);
        for (c = 0; c < w; c++) rgba[r * w + c] = rgb565_to_abgr(s[c], 0xFF000000u);
    }
    if (g_ext && w == GLIDE_W && h == GLIDE_H) {
        /* Lit only along a few full-width lines. Briefing text on black is
         * as dark, but spread over many rows, and must be drawn. */
        uint32_t rows = 0, lines = 0, n;
        for (r = 0; r < h; r++) {
            for (c = 0, n = 0; c < w; c++) n += (rgba[r * w + c] & 0xFFFFFFu) != 0;
            rows += n != 0;
            lines += n > w / 2;
        }
        if ((rows && rows <= 8 && lines == rows) != g_letterbox)
            LOGI("glide: letterbox %s (%u lit rows, %u of them lines)",
                 g_letterbox ? "off" : "on", rows, lines);
        letterbox_select(rows && rows <= 8 && lines == rows);
        if (g_letterbox) {
            wide_select(0);
            cockpit_select(0, 0.0f);
        }
    }
    if (g_letterbox && w == GLIDE_W && h == GLIDE_H) {
        /* not drawn: see g_letterbox */
    } else if (!hd_image_draw(rgba, (int)x, (int)y, (int)w, (int)h)) {
        int ng = stride > 0 ? hdtext_take(src, (uint32_t)stride, (int)w, (int)h, g_glyphs,
                                          (int)(sizeof g_glyphs / sizeof g_glyphs[0])) : 0;
        for (r = 0; r < (uint32_t)ng; r++) hdtext_erase(rgba, (int)w, &g_glyphs[r]);
        overlay_draw(rgba, (int)w, (int)x, (int)y, (int)w, (int)h,
                     g_smooth2d || (w == GLIDE_W && h == GLIDE_H));
        hdtext_overlay(ng, (int)x, (int)y);
    }

    /* keep the CPU view current if it is, so the next lock need not re-read */
    if (g_vram_valid)
        for (r = 0; r < h; r++) {
            memcpy(g_vram + ((size_t)(y + r) * GLIDE_W + x) * 2,
                   src + (int32_t)r * stride, w * 2);
            memcpy(g_lfb_snap + (size_t)(y + r) * GLIDE_W + x,
                   src + (int32_t)r * stride, w * 2);
        }

    /* On the Voodoo a front-buffer write is on screen at once. The mission
     * briefing is drawn that way, line by line, and then waits for a key
     * without ever swapping -- shown only at a swap, it stayed black. */
    if (a[0] == 0 && !g_frame_started) { frame_present(); g_front_shown = 1; }
    return 1;
}

/* (src_buffer, src_x, src_y, w, h, dst_stride, dst_data) */
static uint32_t gl_lfb_read_region(uint32_t *a)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)a[6];
    uint32_t x = a[1], y = a[2], w = a[3], h = a[4], stride = a[5], r;

    if (!dst || !g_vram || !gl_ensure_context()) return 0;
    lfb_flush();
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
    { "_GRFOGMODE@4",              gl_fog_mode             },
    { "_GRFOGCOLORVALUE@4",        gl_fog_color            },
    { "_GRFOGTABLE@4",             gl_fog_table            },
    { "_GRCHROMAKEYMODE@4",        gl_chroma_mode          },
    { "_GRCHROMAKEYVALUE@4",       gl_chroma_value         },
    { "_GRTEXCLAMPMODE@12",        gl_tex_clamp            },
    { "_GRTEXFILTERMODE@12",       gl_tex_filter           },
    { "_GRCOLORCOMBINE@20",        gl_color_combine        },
    { "_GRCONSTANTCOLORVALUE@4",   gl_constant_color       },
    { "_GRALPHABLENDFUNCTION@16",  gl_alpha_blend          },
    { "_GRSSTPERFSTATS@4",         gl_perf_stats           },
    { "_GRSSTRESETPERFSTATS@0",    gl_reset_perf_stats     },
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
    if (g_glidecallers > 0 && g_glide_frame >= (uint32_t)g_glidecallers) {
        if (g_glide_frame > (uint32_t)g_glidecallers) { callers_note(args, 1); g_glidecallers = 0; }
        else if (im->fn == gl_draw_triangle) callers_note(args, 0);
    }
    /* --glidetrace N traces one frame and stops. Tracing the whole run is
     * useless here -- the render loop issues millions of calls -- and what
     * actually needs answering is the ORDER within a single frame. */
    want = g_glidetrace > 0 ? (uint32_t)g_glidetrace == g_glide_frame
                            : im->calls == 1;
    if (want) {
        char buf[512];
        uint32_t n = im->argbytes / 4, i;
        int off = _snprintf(buf, sizeof buf, "glide: %s(", im->name);
        if (n > 16) n = 16;                 /* all of them, not the first 6 */
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
 * the caller's stdcall frame, so the argument bytes are dropped here. A
 * pending hardware interrupt is taken on the way back (irq_take_at_call):
 * its frame is pushed as if it had arrived at the return address. */
static uint32_t g_irq_enter;            /* game thread only */

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
        push eax                        /* the game's EAX, across this call */
        push dword ptr [esp + 20]       /* the return address */
        call irq_take_at_call
        add  esp, 4
        mov  dword ptr [g_irq_enter], eax
        pop  eax
        mov  ecx, dword ptr [g_argbytes + ebx * 4]
        pop  ebp
        pop  edi
        pop  esi
        pop  ebx
        pop  edx                        /* return address */
        add  esp, ecx                   /* callee cleanup, stdcall */
        cmp  dword ptr [g_irq_enter], 0
        jne  deliver_irq
        jmp  edx
    deliver_irq:
        pushfd                          /* the frame IRETD pops */
        push cs
        push edx
        jmp  dword ptr [g_irq_enter]
    }
}

/* Hor+ in the game itself. FIGHT3 clips every polygon in 3D against four
 * planes through the view's screen bounds (view+0x1C4: x0, y0, x1, y1; the
 * clipper is orig 0xEFC10), and derives the focal length from the full 640
 * width once per mission, before taking the cockpit window's margins off
 * those bounds (orig 0x60D29..0x60D52). Widening x0/x1 right after that, at
 * orig 0x60D58, keeps the focal length -- the 4:3 picture is unchanged --
 * and lets the scene through into the Hor+ columns. */
#define HPLUS_SITE 0x00060D58u
static void __cdecl hplus_view(uint8_t *view)
{
    float *b = (float *)(view + 0x1C4);
    int e = hplus_ext();
    LOGI("hplus: view bounds %.2f,%.2f .. %.2f,%.2f, centre %.1f,%.1f, f %.1f",
         b[0], b[1], b[2], b[3], *(float *)(view + 0x1B0),
         *(float *)(view + 0x1B8), *(float *)(view + 0x1A4));
    if (!e) return;
    b[0] = -0.5f - (float)e;
    b[2] = (float)GLIDE_W - 0.5f + (float)e;
    /* and the full height: the bounds are relative to the view's top row,
     * which is not known yet at this point, so a whole frame either way */
    b[1] = -0.5f - (float)GLIDE_H;
    b[3] = (float)GLIDE_H * 2.0f - 0.5f;
}

static void hplus_install(void)
{
    static const uint8_t before[6] = { 0xD9, 0x98, 0xCC, 0x01, 0x00, 0x00 };
    uint8_t *p = (uint8_t *)gp(HPLUS_SITE), *w;
    int32_t rel;
    if (memcmp(p - 6, before, 6) || p[0] != 0xB8 || p[5] != 0xE8) {
        LOGW("hplus: orig 0x%08X is not the expected code; no Hor+", HPLUS_SITE);
        return;
    }
    w = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                PAGE_EXECUTE_READWRITE);
    if (!w) return;
    {   uint8_t *slot = w;
        *w++ = 0x60;                                   /* pushad            */
        *w++ = 0x9C;                                   /* pushfd            */
        *w++ = 0x50;                                   /* push eax (view)   */
        *w++ = 0xE8;
        rel = (int32_t)((uint8_t *)(void *)hplus_view - (w + 4));
        memcpy(w, &rel, 4); w += 4;
        *w++ = 0x83; *w++ = 0xC4; *w++ = 0x04;         /* add esp,4         */
        *w++ = 0x9D;                                   /* popfd             */
        *w++ = 0x61;                                   /* popad             */
        memcpy(w, p, 5); w += 5;                       /* mov eax, imm32    */
        *w++ = 0xE9;
        rel = (int32_t)((p + 5) - (w + 4));
        memcpy(w, &rel, 4);
        p[0] = 0xE9;
        rel = (int32_t)(slot - (p + 5));
        memcpy(p + 1, &rel, 4);
    }
    LOGI("hplus: view-bounds hook at orig 0x%08X", HPLUS_SITE);
}

void glide_install(void)
{
    uint8_t  *thunk;
    uint32_t  i, k, impl = 0;

    if (!g_glide_mode || !g_glide) return;

    /* Fog table index -> w, per the Glide reference. Computed here rather
     * than alongside the GL context, because grFogTable arrives during the
     * game's own init -- long before the first triangle brings the context
     * up -- and an empty w table makes every lookup collapse to one entry. */
    for (i = 0; i < GR_FOG_TABLE_SIZE; i++)
        g_fog_w[i] = (float)(pow(2.0, 3.0 + (i >> 2)) / (8 - (i & 3)));

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

    hplus_install();
    hdtext_install();
    objprobe_install();
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
