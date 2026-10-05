/* Audio file decoding through Media Foundation.
 *
 * The CD tracks are ripped to FLAC (or MP3, AAC, WAV -- whatever Windows can
 * decode), and Media Foundation has decoded FLAC since Windows 10, so no codec
 * library is needed. Output is always interleaved 16-bit stereo at the file's
 * own rate; the mixer resamples. */

#define COBJMACROS
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

static int mf_start(void)
{
    static volatile LONG state;               /* 0 untried, 1 ok, 2 failed */
    if (state == 0) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        InterlockedExchange(&state, SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))
                                    ? 1 : 2);
        if (state == 2) LOGE("media: MFStartup failed; no CD music");
    }
    return state == 1;
}

static IMFSourceReader *open_reader(const char *path, int *rate, int *channels)
{
    wchar_t wpath[MAX_PATH * 2];
    IMFSourceReader *rd = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    UINT32 r = 0, ch = 0;

    if (!mf_start()) return NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* per thread; repeats are fine */
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2);
    if (FAILED(MFCreateSourceReaderFromURL(wpath, NULL, &rd))) return NULL;

    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&want);
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(
            rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want)) ||
        FAILED(IMFSourceReader_GetCurrentMediaType(
            rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got)) ||
        FAILED(IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r)) ||
        FAILED(IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch)) ||
        !r || ch < 1 || ch > 8) {
        LOGW("media: %s has no decodable audio stream", path);
        IMFSourceReader_Release(rd);
        rd = NULL;
    }
    if (want) IMFMediaType_Release(want);
    if (got)  IMFMediaType_Release(got);
    *rate = (int)r;
    *channels = (int)ch;
    return rd;
}

/* Length in milliseconds, without decoding. 0 if the file is unusable. */
uint32_t media_audio_length_ms(const char *path)
{
    int rate, ch;
    PROPVARIANT v;
    uint32_t ms = 0;
    IMFSourceReader *rd = open_reader(path, &rate, &ch);
    if (!rd) return 0;
    PropVariantInit(&v);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
            rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &v)))
        ms = (uint32_t)(v.uhVal.QuadPart / 10000);
    PropVariantClear(&v);
    IMFSourceReader_Release(rd);
    return ms;
}

/* Decode `path` into a newly allocated out->pcm (interleaved stereo int16,
 * out->cap frames), publishing progress in out->ready so playback can start before
 * decoding finishes. Stops early when out->cancel is set. Returns 1 on
 * success. */
int media_decode_audio(const char *path, media_pcm *out)
{
    int ch, ok = 1;
    PROPVARIANT v;
    LONGLONG dur = 0;
    IMFSourceReader *rd = open_reader(path, &out->rate, &ch);
    if (!rd) return 0;

    PropVariantInit(&v);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
            rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &v)))
        dur = (LONGLONG)v.uhVal.QuadPart;
    PropVariantClear(&v);
    /* room for the announced length plus a second, in case it is short */
    out->cap = (size_t)((dur / 10000 + 1000) * out->rate / 1000);
    out->pcm = (int16_t *)malloc(out->cap * 4);
    if (!out->pcm) { IMFSourceReader_Release(rd); return 0; }

    while (!out->cancel) {
        DWORD flags = 0;
        IMFSample *smp = NULL;
        IMFMediaBuffer *buf = NULL;
        BYTE *p;
        DWORD bytes = 0, n, i;

        if (FAILED(IMFSourceReader_ReadSample(
                rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags,
                NULL, &smp))) { ok = 0; break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (smp) IMFSample_Release(smp); break; }
        if (!smp) continue;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf)) &&
            SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &bytes))) {
            const int16_t *s = (const int16_t *)p;
            n = bytes / (2u * (DWORD)ch);
            if ((size_t)out->ready + n > out->cap) {   /* longer than announced */
                n = (DWORD)(out->cap - (size_t)out->ready);
                out->cancel = 1;
            }
            for (i = 0; i < n; i++) {
                int16_t l = s[i * ch], r = ch > 1 ? s[i * ch + 1] : l;
                out->pcm[(out->ready + i) * 2]     = l;
                out->pcm[(out->ready + i) * 2 + 1] = r;
            }
            InterlockedExchange(&out->ready, out->ready + (LONG)n);
            IMFMediaBuffer_Unlock(buf);
        }
        if (buf) IMFMediaBuffer_Release(buf);
        IMFSample_Release(smp);
    }
    IMFSourceReader_Release(rd);
    return ok;
}

/* ---- HD films ----------------------------------------------------------
 *
 * While the game has GFX\MVI\<CATEGORY>\<NAME>.MVI open, video.c draws
 * <hd>/mvi/<CATEGORY>/<NAME>.mp4 (tools/upscale_video.py) over it. The game
 * keeps decoding and playing its own film underneath, sound included, and
 * the HD picture follows that sound -- the cinematic stream's played time --
 * so it stays in step through any stall and starts when the film does.
 * ponytail: a film without sound would hold its first frame; every FILM
 * entry has sound. Frames are decoded on a thread into a few slots ahead of
 * time; the slots are allocated once and kept, as every film is one size. */

#define MV_SLOTS 4
static CRITICAL_SECTION g_mv_cs;
static HANDLE           g_mv_thread;
static volatile LONG    g_mv_stop;
static volatile int     g_mv_fd = -1;
static int              g_mv_w, g_mv_h, g_mv_have;
static double           g_mv_base;           /* stream seconds at open        */
static wchar_t          g_mv_path[MAX_PATH * 2];
static struct { uint32_t *px; LONGLONG ts; int full; } g_mv[MV_SLOTS];
static uint32_t        *g_mv_cur;

static DWORD WINAPI mv_main(LPVOID arg)
{
    IMFAttributes *at = NULL;
    IMFSourceReader *rd = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    UINT64 fs = 0;
    int w, h, i;
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFCreateAttributes(&at, 1);
    IMFAttributes_SetUINT32(at, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromURL(g_mv_path, at, &rd))) goto out;
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    MFCreateMediaType(&want);
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(
            rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want)) ||
        FAILED(IMFSourceReader_GetCurrentMediaType(
            rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got)) ||
        FAILED(IMFMediaType_GetUINT64(got, &MF_MT_FRAME_SIZE, &fs))) {
        LOGW("media: film has no decodable video stream");
        goto out;
    }
    w = (int)(fs >> 32); h = (int)(UINT32)fs;
    EnterCriticalSection(&g_mv_cs);
    if (!g_mv_cur) {
        g_mv_w = w; g_mv_h = h;
        g_mv_cur = (uint32_t *)malloc((size_t)w * h * 4);
        for (i = 0; i < MV_SLOTS; i++) g_mv[i].px = (uint32_t *)malloc((size_t)w * h * 4);
    }
    LeaveCriticalSection(&g_mv_cs);
    if (w != g_mv_w || h != g_mv_h || !g_mv_cur) {
        LOGW("media: film is %dx%d, the first was %dx%d; not shown", w, h, g_mv_w, g_mv_h);
        goto out;
    }

    while (!g_mv_stop) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample *smp = NULL;
        IMFMediaBuffer *buf = NULL;
        IMF2DBuffer *b2 = NULL;
        BYTE *p = NULL;
        LONG pitch = 0;
        int slot = -1;

        EnterCriticalSection(&g_mv_cs);
        for (i = 0; i < MV_SLOTS && slot < 0; i++) if (!g_mv[i].full) slot = i;
        LeaveCriticalSection(&g_mv_cs);
        if (slot < 0) { Sleep(3); continue; }

        if (FAILED(IMFSourceReader_ReadSample(
                rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts, &smp)))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (smp) IMFSample_Release(smp); break; }
        if (!smp) continue;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf))) {
            /* Lock2D gives the first row and a signed pitch: RGB32 from Media
             * Foundation is usually bottom-up. */
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch))) {
                for (i = 0; i < h; i++)
                    memcpy(g_mv[slot].px + (size_t)i * w, p + (LONG_PTR)i * pitch, (size_t)w * 4);
                IMF2DBuffer_Unlock2D(b2);
            } else if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
                memcpy(g_mv[slot].px, p, (size_t)w * h * 4);
                IMFMediaBuffer_Unlock(buf);
            }
            if (b2) IMF2DBuffer_Release(b2);
            IMFMediaBuffer_Release(buf);
            EnterCriticalSection(&g_mv_cs);
            g_mv[slot].ts = ts;
            g_mv[slot].full = 1;
            LeaveCriticalSection(&g_mv_cs);
        }
        IMFSample_Release(smp);
    }
out:
    if (want) IMFMediaType_Release(want);
    if (got) IMFMediaType_Release(got);
    if (rd) IMFSourceReader_Release(rd);
    if (at) IMFAttributes_Release(at);
    CoUninitialize();
    return 0;
}

void movie_open(const char *dos_path, int fd)
{
    static int cs_ready;
    char path[MAX_PATH * 2], cat[64], name[64];
    const char *p, *q;
    size_t n;

    if (!g_hd_dir[0]) return;
    for (p = dos_path; *p && _strnicmp(p, "mvi\\", 4); p++) {}
    if (!*p) return;
    p += 4;
    q = strchr(p, '\\');
    if (!q || (size_t)(q - p) >= sizeof cat) return;
    memcpy(cat, p, (size_t)(q - p)); cat[q - p] = 0;
    /* ponytail: the overlay is the FILM band; the other categories play in
     * smaller windows and would need their own placement */
    if (_stricmp(cat, "film")) return;
    n = strcspn(q + 1, ".");
    if (n >= sizeof name || _stricmp(q + 1 + n, ".mvi")) return;
    memcpy(name, q + 1, n); name[n] = 0;
    _snprintf(path, sizeof path, "%s\\mvi\\%s\\%s.mp4", g_hd_dir, cat, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !mf_start()) return;

    if (!cs_ready) { InitializeCriticalSection(&g_mv_cs); cs_ready = 1; }
    movie_close(g_mv_fd);                       /* one film at a time */
    MultiByteToWideChar(CP_ACP, 0, path, -1, g_mv_path, MAX_PATH * 2);
    g_mv_base = audio_stream_seconds();
    g_mv_stop = 0;
    g_mv_have = 0;
    g_mv_fd = fd;
    g_mv_thread = CreateThread(NULL, 0, mv_main, NULL, 0, NULL);
    LOGI("media: HD film %s for handle %d", path, fd);
}

void movie_close(int fd)
{
    int i;
    if (fd < 0 || fd != g_mv_fd) return;
    g_mv_fd = -1;
    g_mv_stop = 1;
    if (g_mv_thread) {
        WaitForSingleObject(g_mv_thread, 2000);
        CloseHandle(g_mv_thread);
        g_mv_thread = NULL;
    }
    EnterCriticalSection(&g_mv_cs);
    for (i = 0; i < MV_SLOTS; i++) g_mv[i].full = 0;
    g_mv_have = 0;
    LeaveCriticalSection(&g_mv_cs);
}

/* The frame due now, or NULL while no film is up or its first frame is not
 * decoded yet. *fresh: it differs from the last one returned. */
const uint32_t *movie_frame(int *w, int *h, int *fresh)
{
    LONGLONG now;
    int i, best = -1;
    *fresh = 0;
    if (g_mv_fd < 0) return NULL;
    now = (LONGLONG)((audio_stream_seconds() - g_mv_base) * 1e7);
    if (now <= 0) return NULL;                  /* the film has not started */
    EnterCriticalSection(&g_mv_cs);
    for (i = 0; i < MV_SLOTS; i++)
        if (g_mv[i].full && g_mv[i].ts <= now &&
            (best < 0 || g_mv[i].ts > g_mv[best].ts))
            best = i;
    if (best >= 0) {
        uint32_t *t = g_mv_cur;
        for (i = 0; i < MV_SLOTS; i++)          /* overtaken: never shown */
            if (g_mv[i].full && g_mv[i].ts < g_mv[best].ts) g_mv[i].full = 0;
        g_mv_cur = g_mv[best].px;
        g_mv[best].px = t;
        g_mv[best].full = 0;
        g_mv_have = 1;
        *fresh = 1;
    }
    LeaveCriticalSection(&g_mv_cs);
    *w = g_mv_w; *h = g_mv_h;
    return g_mv_have ? g_mv_cur : NULL;
}
