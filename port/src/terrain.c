/* terrain.c -- a smooth seabed for glide.c's --fx terrain.
 *
 * The game's seabed is a height field on a 512-unit grid, two flat
 * triangles a cell, drawn in screen space with no lighting of its own.
 * glide.c puts every terrain vertex back into the world (the camera from
 * objprobe_camera); the grid corners among them are remembered here, and
 * terrain_eval gives a Catmull-Rom surface through them -- the same
 * heights at every corner, rounded in between -- with its gradient for the
 * effects' normals. The seabed does not move, so a corner seen once stays
 * valid until the heights there change (another mission).
 */
#include <math.h>
#include <string.h>

#include "port.h"

#define G     512.0f              /* grid spacing, game units */
#define NBITS 16
#define N     (1u << NBITS)

static struct { int32_t x, y; float h; int used; } g_node[N];
static unsigned g_used;

static unsigned slot(int32_t x, int32_t y)
{
    uint32_t k = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    return (k ^ (k >> 16)) & (N - 1);
}

static int get(int32_t x, int32_t y, float *h)
{
    unsigned i = slot(x, y);
    while (g_node[i].used) {
        if (g_node[i].x == x && g_node[i].y == y) { *h = g_node[i].h; return 1; }
        i = (i + 1) & (N - 1);
    }
    return 0;
}

void terrain_reset(void)
{
    memset(g_node, 0, sizeof g_node);
    g_used = 0;
}

/* a terrain vertex in the world; only grid corners are kept */
void terrain_note(const float p[3])
{
    float fx = p[0] / G, fy = p[1] / G;
    int32_t x = (int32_t)floorf(fx + 0.5f), y = (int32_t)floorf(fy + 0.5f);
    unsigned i;
    if (fabsf(fx - (float)x) * G > 1.5f || fabsf(fy - (float)y) * G > 1.5f) return;
    i = slot(x, y);
    while (g_node[i].used && (g_node[i].x != x || g_node[i].y != y)) i = (i + 1) & (N - 1);
    if (g_node[i].used) {
        /* the reconstruction is good to a few tenths of a unit: average
         * that out, but take a real change (another mission) at once */
        float d = p[2] - g_node[i].h;
        g_node[i].h += fabsf(d) < 4.0f ? 0.1f * d : d;
        return;
    }
    if (g_used > N / 4 * 3) terrain_reset(), i = slot(x, y);
    g_node[i].x = x; g_node[i].y = y; g_node[i].h = p[2]; g_node[i].used = 1;
    g_used++;
}

static float cr(const float q[4], float t)
{
    return 0.5f * (2.0f * q[1] + (q[2] - q[0]) * t
                   + (2.0f * q[0] - 5.0f * q[1] + 4.0f * q[2] - q[3]) * t * t
                   + (3.0f * (q[1] - q[2]) + q[3] - q[0]) * t * t * t);
}

static float crd(const float q[4], float t)
{
    return 0.5f * ((q[2] - q[0]) + 2.0f * (2.0f * q[0] - 5.0f * q[1] + 4.0f * q[2] - q[3]) * t
                   + 3.0f * (3.0f * (q[1] - q[2]) + q[3] - q[0]) * t * t);
}

/* Height and gradient at world (x, y), 0 if the cell's four corners are not
 * known yet. Unknown outer neighbours are extended linearly. */
int terrain_eval(float x, float y, float *z, float *dzdx, float *dzdy)
{
    float fx = x / G, fy = y / G, h[4][4], row[4], drow[4], tx, ty;
    int32_t ix = (int32_t)floorf(fx), iy = (int32_t)floorf(fy);
    int known[4][4], i, j;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++)
            known[j][i] = get(ix + i - 1, iy + j - 1, &h[j][i]);
    if (!known[1][1] || !known[1][2] || !known[2][1] || !known[2][2]) return 0;
    for (j = 1; j <= 2; j++) {
        if (!known[j][0]) h[j][0] = 2.0f * h[j][1] - h[j][2];
        if (!known[j][3]) h[j][3] = 2.0f * h[j][2] - h[j][1];
    }
    for (i = 0; i < 4; i++) {
        if (!known[0][i]) h[0][i] = 2.0f * h[1][i] - h[2][i];
        if (!known[3][i]) h[3][i] = 2.0f * h[2][i] - h[1][i];
    }
    tx = fx - (float)ix;
    ty = fy - (float)iy;
    for (j = 0; j < 4; j++) {
        row[j] = cr(h[j], tx);
        drow[j] = crd(h[j], tx);
    }
    *z = cr(row, ty);
    *dzdx = cr(drow, ty) / G;
    *dzdy = crd(row, ty) / G;
    return 1;
}

/* --selftest: the surface passes through every corner, a plane stays a plane */
int terrain_selftest(void)
{
    float z, dx, dy;
    int i, j, ok = 1;
    terrain_reset();
    for (j = -2; j < 4; j++)
        for (i = -2; i < 4; i++) {
            float p[3];
            p[0] = (float)i * G + 0.3f; p[1] = (float)j * G - 0.2f;
            p[2] = 100.0f + 0.25f * (float)i * G - 0.5f * (float)j * G;
            terrain_note(p);
        }
    ok &= terrain_eval(G, G, &z, &dx, &dy) && fabsf(z - (100.0f + 0.25f * G - 0.5f * G)) < 0.5f;
    ok &= terrain_eval(0.37f * G, 1.61f * G, &z, &dx, &dy) &&
          fabsf(z - (100.0f + 0.25f * 0.37f * G - 0.5f * 1.61f * G)) < 0.5f &&
          fabsf(dx - 0.25f) < 1e-3f && fabsf(dy + 0.5f) < 1e-3f;
    ok &= !terrain_eval(10.5f * G, 0.5f * G, &z, &dx, &dy);       /* unknown cell */
    terrain_reset();
    return ok;
}
