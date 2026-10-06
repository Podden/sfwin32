/* --objprobe: M5 spike. Hooks FIGHT3's per-model render function (orig
 * 0x8EBD0: eax = model, edx = position, ebx = rotation, ecx = camera) and
 * logs what it is handed -- the first few distinct models, and every model of
 * the frame a script's "callers" command picks, with the triangles each one
 * draws. --objreplace NAME then draws one MOD itself instead of the game.
 * glide.c's --fx keeps the hook for the camera alone (objprobe_camera). */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int g_objprobe;
char g_objreplace[32];          /* --objreplace NAME: draw that MOD ourselves */
extern uint32_t g_glide_frame;

#define SITE     0x8EBD0u
/* 0x35370, the face rasteriser, adds this window origin (two ints) to the
 * projected point, snapped to 1/16 px, and turns camera depth y into Glide's
 * oow = 1 / (1 + (y - near) * 65535 / (far - near)), near and far being the
 * floats right after the projection constants. */
#define WIN_X 0x15F6C0u
#define HOOK_LEN 8
static const uint8_t k_entry[HOOK_LEN] = { 0x56, 0x57, 0x55, 0x89, 0xE5, 0x83, 0xEC, 0x24 };

static uint32_t orig(uint32_t p) { return p - g_img.delta; }

static int in_img(uint32_t p, uint32_t n)
{
    uintptr_t a = (uintptr_t)g_img.mem;
    return p >= a && p + n <= a + g_img.reserved;
}

static void dump(const char *what, uint32_t p, int n)
{
    char line[512];
    int i, off;
    if (!in_img(p, (uint32_t)n * 4)) { LOGI("  %s %08X not in image", what, p); return; }
    for (i = 0; i < n; i += 8) {
        int k;
        off = _snprintf(line, sizeof line, "  %s+%03X:", what, i * 4);
        for (k = i; k < i + 8 && k < n && off > 0 && off < (int)sizeof line - 24; k++) {
            uint32_t v = ((const uint32_t *)(uintptr_t)p)[k];
            float f;
            memcpy(&f, &v, 4);
            if (in_img(v, 4))
                off += _snprintf(line + off, sizeof line - off, " o%07X", orig(v));
            else if (v && (f > 1e-4f || f < -1e-4f) && f < 1e7f && f > -1e7f)
                off += _snprintf(line + off, sizeof line - off, " %.4g", f);
            else
                off += _snprintf(line + off, sizeof line - off, " %X", v);
        }
        LOGI("%s", line);
    }
}

static uint32_t g_cur_model;              /* the model being drawn, for objprobe_tri */
static uint32_t g_view_ctx;               /* the view it is drawn with                */
uint32_t g_objprobe_frame;                /* script "callers": the frame to log      */

static void floats(const char *what, uint32_t p, int n)
{
    char line[512];
    int i, off;
    if (!in_img(p, (uint32_t)n * 4)) { LOGI("  %s %08X not in image", what, p); return; }
    off = _snprintf(line, sizeof line, "  %s", what);
    for (i = 0; i < n && off > 0 && off < (int)sizeof line - 20; i++)
        off += _snprintf(line + off, sizeof line - off, " %.6g", ((const float *)(uintptr_t)p)[i]);
    LOGI("%s", line);
}

/* ---- --objreplace NAME: the spike's proof of an object-level hook --------
 *
 * VFX\MODULE\NAME.MOD is loaded here, recognised among the models the game
 * draws by its points (a model's points are the file's, times the size at
 * model+0xBC), and drawn by this code instead of the game: smooth-shaded from
 * per-point normals, in a test colour, through the game's own transform
 * (camera rotation C at ctx+0x74 times the object's R, translation
 * C(pos - campos)), its projection (ctx+0x1A4 fx, +0x1AC fy, +0x1B0 cx,
 * +0x1B8 cy, depth along camera y) and Glide's depth and fog. The game's
 * faces for it are skipped. ponytail: no near-plane clipping (a triangle
 * reaching behind the camera is dropped) and no texture; a real replacement
 * needs both, plus its own mesh format. */

static float (*g_rp)[6];                  /* point xyz, normal xyz       */
static uint32_t *g_rid;                   /* the file's point ids        */
static int g_nrp, (*g_rf)[3], g_nrf;
static struct { uint32_t model; int is; } g_known[256];
static int g_nknown;

static int rp_index(uint32_t id)
{
    int i;
    for (i = 0; i < g_nrp; i++) if (g_rid[i] == id) return i;
    return -1;
}

static void load_replacement(void)
{
    char path[MAX_PATH];
    FILE *f;
    uint8_t *b;
    long len;
    size_t o = 18;
    int32_t nf, np, i, k;
    _snprintf(path, sizeof path, "VFX\\MODULE\\%s.MOD", g_objreplace);
    if (!(f = fopen(path, "rb"))) { LOGW("objreplace: cannot open %s", path); return; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return; }
    fclose(f);
    memcpy(&nf, b + 2, 4); memcpy(&np, b + 10, 4);
    if (np <= 0 || nf <= 0 || o + (size_t)np * 16 > (size_t)len) { free(b); return; }
    g_rp = calloc((size_t)np, sizeof *g_rp); g_rid = calloc((size_t)np, 4);
    g_rf = calloc((size_t)nf * 8, sizeof *g_rf);
    for (i = 0; i < np; i++, o += 16) { memcpy(&g_rid[i], b + o, 4); memcpy(g_rp[i], b + o + 4, 12); }
    g_nrp = np;
    for (k = 0; k < nf && o + 38 <= (size_t)len; k++) {
        int16_t kind; uint32_t alt; int32_t n, sl, id[16];
        memcpy(&kind, b + o + 4, 2); memcpy(&alt, b + o + 10, 4); o += 38;
        if (kind != 1) { o += (size_t)(alt - 1) * 12; continue; }
        if (o + 16 > (size_t)len) break;
        memcpy(&n, b + o + 8, 4); o += 16;
        o += (size_t)n * 8;
        memcpy(&sl, b + o, 4); o += 8 + (size_t)sl;
        if (n < 3 || n > 16 || o + (size_t)n * 12 > (size_t)len) break;
        for (i = 0; i < n; i++) memcpy(&id[i], b + o + (size_t)i * 12 + 4, 4);
        o += (size_t)n * 12;
        for (i = 1; i + 1 < n && g_nrf < nf * 8; i++) {
            int a = rp_index((uint32_t)id[0]), c = rp_index((uint32_t)id[i + 1]), d = rp_index((uint32_t)id[i]);
            if (a < 0 || c < 0 || d < 0) continue;
            g_rf[g_nrf][0] = a; g_rf[g_nrf][1] = c; g_rf[g_nrf][2] = d; g_nrf++;
        }
    }
    free(b);
    for (k = 0; k < g_nrf; k++) {         /* area-weighted point normals */
        float *p0 = g_rp[g_rf[k][0]], *p1 = g_rp[g_rf[k][1]], *p2 = g_rp[g_rf[k][2]];
        float u[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        float v[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (i = 0; i < 3; i++) {
            float *q = g_rp[g_rf[k][i]];
            q[3] += n[0]; q[4] += n[1]; q[5] += n[2];
        }
    }
    for (i = 0; i < g_nrp; i++) {
        float *q = g_rp[i], l = sqrtf(q[3] * q[3] + q[4] * q[4] + q[5] * q[5]);
        if (l > 0) { q[3] /= l; q[4] /= l; q[5] /= l; }
    }
    LOGI("objreplace: %s, %d points, %d triangles", g_objreplace, g_nrp, g_nrf);
}

static int replaced(uint32_t model)
{
    const uint32_t *m = (const uint32_t *)(uintptr_t)model;
    const float *q;
    float size;
    int i, k, is = 1, n;
    for (i = 0; i < g_nknown; i++) if (g_known[i].model == model) return g_known[i].is;
    if (!in_img(model, 0xC0)) return 0;
    n = (int)m[0x40 / 4];
    memcpy(&size, &m[0xBC / 4], 4);
    if (n <= 0 || n > 4096 || !(size > 0.0f) || !in_img(m[0x44 / 4], (uint32_t)n * 12)) is = 0;
    q = (const float *)(uintptr_t)m[0x44 / 4];
    for (i = 0; is && i < n && i < 4; i++) {
        for (k = 0; k < g_nrp; k++)
            if (fabsf(g_rp[k][0] * size - q[i * 3]) < 0.01f * size &&
                fabsf(g_rp[k][1] * size - q[i * 3 + 1]) < 0.01f * size &&
                fabsf(g_rp[k][2] * size - q[i * 3 + 2]) < 0.01f * size) break;
        if (k == g_nrp) is = 0;
    }
    if (g_nknown < 256) { g_known[g_nknown].model = model; g_known[g_nknown].is = is; g_nknown++; }
    if (is) LOGI("objreplace: model o%07X is %s (size %.1f)", orig(model), g_objreplace, size);
    return is;
}

static void draw_replacement(uint32_t model, const float *pos, const float *R, const uint8_t *ctx)
{
    const float *C = (const float *)(ctx + 0x74), *cp = (const float *)(ctx + 0x104);
    float fx, fy, cx, cy, zn, zf, size, M[9], t[3], d[3];
    static const float L[3] = { 0.30f, 0.40f, 0.866f };
    int i, j, k;
    memcpy(&fx, ctx + 0x1A4, 4); memcpy(&fy, ctx + 0x1AC, 4);
    memcpy(&cx, ctx + 0x1B0, 4); memcpy(&cy, ctx + 0x1B8, 4);
    memcpy(&zn, ctx + 0x1BC, 4); memcpy(&zf, ctx + 0x1C0, 4);
    cx += (float)*(int32_t *)(uintptr_t)(WIN_X + g_img.delta);
    cy += (float)*(int32_t *)(uintptr_t)(WIN_X + 4 + g_img.delta);
    if (!(zf > zn)) return;
    memcpy(&size, (const uint8_t *)(uintptr_t)model + 0xBC, 4);
    for (i = 0; i < 3; i++) {
        d[i] = pos[i] - cp[i];
        for (j = 0; j < 3; j++)
            M[i * 3 + j] = C[i * 3] * R[j] + C[i * 3 + 1] * R[3 + j] + C[i * 3 + 2] * R[6 + j];
    }
    for (i = 0; i < 3; i++) t[i] = C[i * 3] * d[0] + C[i * 3 + 1] * d[1] + C[i * 3 + 2] * d[2];
    for (k = 0; k < g_nrf; k++) {
        float tri[3][6];
        for (i = 0; i < 3; i++) {
            const float *q = g_rp[g_rf[k][i]];
            float x = (M[0] * q[0] + M[1] * q[1] + M[2] * q[2]) * size + t[0];
            float y = (M[3] * q[0] + M[4] * q[1] + M[5] * q[2]) * size + t[1];
            float z = (M[6] * q[0] + M[7] * q[1] + M[8] * q[2]) * size + t[2];
            float nw[3], lit;
            if (y < zn) break;
            for (j = 0; j < 3; j++) nw[j] = R[j * 3] * q[3] + R[j * 3 + 1] * q[4] + R[j * 3 + 2] * q[5];
            lit = nw[0] * L[0] + nw[1] * L[1] + nw[2] * L[2];
            lit = 0.30f + 0.70f * (lit > 0.0f ? lit : 0.0f);
            tri[i][0] = cx + fx * x / y;
            tri[i][1] = cy + fy * z / y;
            tri[i][2] = 1.0f / (1.0f + (y - zn) * 65535.0f / (zf - zn));
            tri[i][3] = 255.0f * lit; tri[i][4] = 150.0f * lit; tri[i][5] = 60.0f * lit;
        }
        if (i == 3) glide_tri_rgb(tri);
    }
}

static int __cdecl obj_seen(uint32_t model, uint32_t edx, uint32_t ebx, uint32_t ctx)
{
    static uint32_t seen[16];
    static int nseen;
    int i;
    g_cur_model = model;
    g_view_ctx = ctx;
    if (!g_objprobe) return 0;                   /* hooked for glide.c's --fx only */
    if (g_nrf && replaced(model) && in_img(edx, 12) && in_img(ebx, 36) && in_img(ctx, 0x1C4)) {
        draw_replacement(model, (const float *)(uintptr_t)edx, (const float *)(uintptr_t)ebx,
                         (const uint8_t *)(uintptr_t)ctx);
        return 1;
    }
    if (g_objprobe_frame && g_glide_frame == g_objprobe_frame && in_img(model, 0xC0)) {
        const uint32_t *m = (const uint32_t *)(uintptr_t)model;
        LOGI("objframe: model o%07X faces %u points %u ctx o%07X", orig(model), m[3], m[0x40 / 4], orig(ctx));
        floats("pos", edx, 3);
        floats("rot", ebx, 9);
        floats("campos", ctx + 0x104, 3);
        floats("camrot", ctx + 0x74, 9);
        floats("proj", ctx + 0x1A4, 8);
        LOGI("  window %d %d", *(int32_t *)(uintptr_t)(WIN_X + g_img.delta),
             *(int32_t *)(uintptr_t)(WIN_X + 4 + g_img.delta));
        floats("bounds", model + 0x9C, 9);
        if (in_img(m[0x44 / 4], 4 * 3 * 6)) floats("pts", m[0x44 / 4], 3 * 6);
    }
    for (i = 0; i < nseen && seen[i] != model; i++) {}
    if (i < nseen || nseen == 4) return 0;
    seen[nseen++] = model;
    LOGI("objprobe: model o%07X edx=%08X ebx=%08X ctx o%07X frame %u", orig(model), edx, ebx,
         orig(ctx), g_glide_frame);
    dump("model", model, 48);
    dump("ctx", ctx, 80);
    if (in_img(ctx, 4)) dump("ctx.vtbl", *(uint32_t *)(uintptr_t)ctx, 16);
    return 0;
}

/* glide.c's --fx: the camera the game is drawing with, read live from the
 * view the model render was last handed. proj = fx, fy, cx, cy (window origin
 * included, so in grDrawTriangle's own coordinates), near, far; C maps world
 * to camera (x right, y depth, z up), pos is the camera in the world. 0 until
 * a model has been drawn. */
int objprobe_camera(float proj[6], float C[9], float pos[3])
{
    const uint8_t *v = (const uint8_t *)(uintptr_t)g_view_ctx;
    if (!g_view_ctx || !in_img(g_view_ctx, 0x1C4)) return 0;
    memcpy(&proj[0], v + 0x1A4, 4);
    memcpy(&proj[1], v + 0x1AC, 4);
    memcpy(&proj[2], v + 0x1B0, 4);
    memcpy(&proj[3], v + 0x1B8, 4);
    memcpy(&proj[4], v + 0x1BC, 8);
    proj[2] += (float)*(int32_t *)(uintptr_t)(WIN_X + g_img.delta);
    proj[3] += (float)*(int32_t *)(uintptr_t)(WIN_X + 4 + g_img.delta);
    memcpy(C, v + 0x74, 36);
    memcpy(pos, v + 0x104, 12);
    return proj[0] != 0.0f && proj[1] != 0.0f && proj[5] > proj[4];
}

/* glide.c: every triangle of the logged frame, tagged with the model drawn last */
void objprobe_tri(const float *const v[3])
{
    if (!g_objprobe || g_glide_frame != g_objprobe_frame) return;
    LOGI("objtri: m o%07X  %.2f %.2f %.6g  %.2f %.2f %.6g  %.2f %.2f %.6g", orig(g_cur_model),
         v[0][0], v[0][1], v[0][8], v[1][0], v[1][1], v[1][8], v[2][0], v[2][1], v[2][8]);
}

static void hook(uint8_t *p)
{
    uint8_t *w = (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE), *slot = w;
    int32_t rel;
    if (!w) return;
    *w++ = 0x60;                                   /* pushad             */
    *w++ = 0x9C;                                   /* pushfd             */
    *w++ = 0x51;                                   /* push ecx  (ctx)    */
    *w++ = 0x53;                                   /* push ebx           */
    *w++ = 0x52;                                   /* push edx           */
    *w++ = 0x50;                                   /* push eax  (model)  */
    *w++ = 0xE8;
    rel = (int32_t)((uint8_t *)(void *)obj_seen - (w + 4));
    memcpy(w, &rel, 4); w += 4;
    *w++ = 0x83; *w++ = 0xC4; *w++ = 0x10;         /* add esp,16         */
    *w++ = 0x85; *w++ = 0xC0;                      /* test eax,eax       */
    *w++ = 0x74; *w++ = 0x03;                      /* jz   +3            */
    *w++ = 0x9D;                                   /* popfd              */
    *w++ = 0x61;                                   /* popad              */
    *w++ = 0xC3;                                   /* ret: drawn by us   */
    *w++ = 0x9D;                                   /* popfd              */
    *w++ = 0x61;                                   /* popad              */
    memcpy(w, p, HOOK_LEN); w += HOOK_LEN;         /* replayed           */
    *w++ = 0xE9;
    rel = (int32_t)((p + HOOK_LEN) - (w + 4));
    memcpy(w, &rel, 4);
    p[0] = 0xE9;
    rel = (int32_t)(slot - (p + 5));
    memcpy(p + 1, &rel, 4);
    memset(p + 5, 0x90, HOOK_LEN - 5);
}

void objprobe_install(void)
{
    uint8_t *p;
    if (!g_objprobe && !g_fx) return;
    p = (uint8_t *)(uintptr_t)(SITE + g_img.delta);
    if (memcmp(p, k_entry, HOOK_LEN)) { LOGW("objprobe: 0x%X is not the expected code", SITE); return; }
    if (g_objreplace[0]) load_replacement();
    hook(p);
    LOGI("objprobe: hooked model render at orig 0x%X", SITE);
}
