/* Asset dumping and high-resolution replacement.
 *
 * --dump DIR writes every distinct texture and every distinct full-image 2D
 * blit the game hands to Glide, named by a hash of its content, so an
 * offline pipeline can produce better versions. --hd DIR loads those
 * versions back: DIR/tex/<name>.png replaces a texture, DIR/2d/<name>.png a
 * 2D image, at any resolution. PNG (or anything else WIC decodes) is read
 * through the Windows Imaging Component, so no image library is needed.
 *
 * Texture names are <data>_<palette>: the same 8-bit atlas is uploaded with
 * different palettes in different scenarios, and the pixels differ with it.
 * A replacement named <data> alone covers every palette. */

#define COBJMACROS
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

char g_dump_dir[MAX_PATH];
char g_hd_dir[MAX_PATH];

uint64_t hd_hash(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    if (!h) h = 1469598103934665603ull;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static void ensure_dir(const char *base, const char *sub)
{
    char path[MAX_PATH + 16];
    CreateDirectoryA(base, NULL);
    _snprintf(path, sizeof path, "%s\\%s", base, sub);
    path[sizeof path - 1] = 0;
    CreateDirectoryA(path, NULL);
}

/* 32-bit TGA, top-down, from 0xAABBGGRR pixels. */
static int write_tga(const char *path, int w, int h, const uint32_t *px)
{
    uint8_t hdr[18] = { 0 };
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return 0;
    hdr[2] = 2;                               /* uncompressed true colour */
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28;                           /* top-left origin, 8 alpha bits */
    fwrite(hdr, 1, sizeof hdr, f);
    for (i = 0; i < w * h; i++) {
        uint32_t c = px[i];
        uint8_t bgra[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8),
                            (uint8_t)c, (uint8_t)(c >> 24) };
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
    return 1;
}

/* Write DIR/<kind>/<name>.tga once per name. Returns 1 if it was new. */
int hd_dump(const char *kind, const char *name, int w, int h,
            const uint32_t *px)
{
    char path[MAX_PATH * 2];
    if (!g_dump_dir[0]) return 0;
    ensure_dir(g_dump_dir, kind);
    _snprintf(path, sizeof path, "%s\\%s\\%s.tga", g_dump_dir, kind, name);
    path[sizeof path - 1] = 0;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 0;
    return write_tga(path, w, h, px);
}

/* Decode DIR/<kind>/<name>.png into malloc'd 0xAABBGGRR pixels. */
uint32_t *hd_load(const char *kind, const char *name, int *w, int *h)
{
    static IWICImagingFactory *fac;
    static int init_failed;
    wchar_t wpath[MAX_PATH * 2];
    char path[MAX_PATH * 2];
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *conv = NULL;
    uint32_t *px = NULL;
    UINT uw = 0, uh = 0;

    if (!g_hd_dir[0] || init_failed) return NULL;
    _snprintf(path, sizeof path, "%s\\%s\\%s.png", g_hd_dir, kind, name);
    path[sizeof path - 1] = 0;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return NULL;

    if (!fac) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        (void)hr;                             /* S_FALSE/RPC_E_CHANGED are fine */
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                    CLSCTX_INPROC_SERVER,
                                    &IID_IWICImagingFactory, (void **)&fac))) {
            LOGE("hd: WIC unavailable; replacements disabled");
            init_failed = 1;
            return NULL;
        }
    }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2);
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(
            fac, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            &dec)) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
        FAILED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppRGBA,
                                      (IWICBitmapSource *)frame, &conv)) ||
        FAILED(IWICBitmapSource_GetSize(conv, &uw, &uh)) ||
        !uw || !uh || uw > 16384 || uh > 16384 ||
        (px = (uint32_t *)malloc((size_t)uw * uh * 4)) == NULL ||
        FAILED(IWICBitmapSource_CopyPixels(conv, NULL, uw * 4, uw * uh * 4,
                                           (BYTE *)px))) {
        LOGW("hd: cannot decode %s", path);
        free(px);
        px = NULL;
    } else {
        *w = (int)uw;
        *h = (int)uh;
        LOGI("hd: loaded %s (%ux%u)", path, uw, uh);
    }
    if (conv)  IWICBitmapSource_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec)   IWICBitmapDecoder_Release(dec);
    return px;
}
