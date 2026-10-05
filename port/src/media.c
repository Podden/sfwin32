/* Audio file decoding through Media Foundation.
 *
 * The CD tracks are ripped to FLAC (or MP3, AAC, WAV -- whatever Windows can
 * decode), and Media Foundation has decoded FLAC since Windows 10, so no codec
 * library is needed. Output is always interleaved 16-bit stereo at the file's
 * own rate; the mixer resamples. */

#define COBJMACROS
#include "port.h"

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
