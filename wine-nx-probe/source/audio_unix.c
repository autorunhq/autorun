/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Native audout backend behind the packaged winenxaudio.drv PE module.
 * Shared render clients, stereo 48 kHz signed 16-bit PCM.
 * Wine's shared-mode converter handles other application formats. */
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <switch.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "mmsystem.h"
#include "mmddk.h"
#include "wine/unixlib.h"
#include "../../dlls/mmdevapi/unixlib.h"

#define NX_RATE 48000
#define NX_CHUNK 480
/* Buffers held by audout at once: the cushion that covers scheduling delays,
 * NX_BUFFERS * NX_CHUNK frames (40 ms) between the mixer and silence. Two of
 * them (20 ms) broke up when a frame of the game overran its slice. */
#define NX_BUFFERS 4
struct nx_audio_stream
{
    struct nx_audio_stream *next;
    BYTE *ring, *scratch;
    unsigned int capacity, held, submitted, read, locked;
    UINT64 played;
    unsigned int frames[NX_BUFFERS];
    float volume[2];
    HANDLE event;
    DWORD flags;
    unsigned int source_rate, source_channels, source_bits, source_tag, source_frame_bytes;
    unsigned int scratch_bytes;
    BOOL running, failed;
};
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;
static struct nx_audio_stream *active;
static AudioOutBuffer output[NX_BUFFERS];
static unsigned int output_frames[NX_BUFFERS];
static HANDLE audio_thread;
static BOOL audio_quit, audio_closing, output_running;
static BOOL initialized;
static struct nx_audio_stream *nx_stream(stream_handle handle) { return (void *)(UINT_PTR)handle; }

static NTSTATUS nx_not_implemented(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS nx_process_attach(void *args) { (void)args; return STATUS_SUCCESS; }
static NTSTATUS nx_test_connect(void *args)
{
    struct test_connect_params *p = args;
    pthread_mutex_lock(&audio_lock);
    if (!initialized && R_SUCCEEDED(audoutInitialize())) initialized = TRUE;
    p->priority = initialized ? Priority_Preferred : Priority_Unavailable;
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *p = args;
    static const WCHAR name[] = {'N','i','n','t','e','n','d','o',' ','S','w','i','t','c','h',0};
    static const char device[] = "audout";
    unsigned int size = sizeof(struct endpoint) + sizeof(name) + sizeof(device);
    p->num = p->flow == eRender ? 1 : 0;
    p->default_idx = 0;
    if (!p->num) { p->size = 0; p->result = S_OK; return STATUS_SUCCESS; }
    if (p->size < size) p->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    else
    {
        p->endpoints[0].name = sizeof(struct endpoint);
        p->endpoints[0].device = sizeof(struct endpoint) + sizeof(name);
        memcpy((BYTE *)p->endpoints + p->endpoints[0].name, name, sizeof(name));
        memcpy((BYTE *)p->endpoints + p->endpoints[0].device, device, sizeof(device));
        p->result = S_OK;
    }
    p->size = size;
    return STATUS_SUCCESS;
}
static const GUID nx_subtype_pcm = {1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static const GUID nx_subtype_float = {3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
/* The sample coding: WAVE_FORMAT_PCM or WAVE_FORMAT_IEEE_FLOAT, looking through
 * WAVE_FORMAT_EXTENSIBLE to its subtype, or 0 for anything else. DirectSound
 * mixes in 32-bit float and asks for it as EXTENSIBLE. */
static unsigned int nx_format_tag(const WAVEFORMATEX *f)
{
    const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
    if (f->wFormatTag == WAVE_FORMAT_PCM || f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return f->wFormatTag;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE || f->cbSize < 22) return 0;
    if (!memcmp(&e->SubFormat, &nx_subtype_pcm, sizeof(GUID))) return WAVE_FORMAT_PCM;
    if (!memcmp(&e->SubFormat, &nx_subtype_float, sizeof(GUID))) return WAVE_FORMAT_IEEE_FLOAT;
    return 0;
}
static BOOL nx_format(const WAVEFORMATEX *f)
{
    unsigned int tag;
    if (!f || !f->nSamplesPerSec || f->nChannels < 1 || f->nChannels > 2) return FALSE;
    if (!(tag = nx_format_tag(f))) return FALSE;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample != 32) return FALSE;
    return (f->wBitsPerSample == 8 || f->wBitsPerSample == 16 || f->wBitsPerSample == 24 || f->wBitsPerSample == 32) &&
        f->nBlockAlign == f->nChannels * (f->wBitsPerSample / 8) &&
        f->nAvgBytesPerSec == f->nSamplesPerSec * f->nBlockAlign;
}
static NTSTATUS nx_is_format_supported(void *args)
{
    struct is_format_supported_params *p = args;
    p->result = p->flow == eRender && nx_format(p->fmt_in) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (p->share != AUDCLNT_SHAREMODE_SHARED) p->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_get_mix_format(void *args)
{
    struct get_mix_format_params *p = args;
    /* EXTENSIBLE, as Wine's other drivers report it: DirectSound copies this
     * and only changes the subtype and sample size, so a plain WAVEFORMATEX
     * (cbSize 0) made its Initialize fail with E_INVALIDARG. */
    memset(p->fmt, 0, sizeof(*p->fmt));
    p->fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    p->fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    p->fmt->Format.nChannels = 2;
    p->fmt->Format.nSamplesPerSec = NX_RATE;
    p->fmt->Format.nAvgBytesPerSec = NX_RATE * 4;
    p->fmt->Format.nBlockAlign = 4;
    p->fmt->Format.wBitsPerSample = 16;
    p->fmt->Samples.wValidBitsPerSample = 16;
    p->fmt->dwChannelMask = 0x3;  /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    p->fmt->SubFormat = nx_subtype_pcm;
    p->result = p->flow == eRender ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_get_device_period(void *args)
{
    struct get_device_period_params *p = args;
    if (p->def_period) *p->def_period = 100000;
    if (p->min_period) *p->min_period = 100000;
    p->result = S_OK;
    return STATUS_SUCCESS;
}
static void nx_free(struct nx_audio_stream *s)
{
    SIZE_T size = 0;
    if (s->scratch) NtFreeVirtualMemory(GetCurrentProcess(), (void **)&s->scratch, &size, MEM_RELEASE);
    free(s->ring);
    free(s);
}
static NTSTATUS nx_create_stream(void *args)
{
    struct create_stream_params *p = args;
    struct nx_audio_stream *s;
    SIZE_T bytes;
    unsigned int i;
    p->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (p->flow != eRender || !nx_format(p->fmt)) return STATUS_SUCCESS;
    p->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    if (p->share != AUDCLNT_SHAREMODE_SHARED) return STATUS_SUCCESS;
    p->result = E_INVALIDARG;
    if (p->flags & AUDCLNT_STREAMFLAGS_RATEADJUST) return STATUS_SUCCESS;
    if (p->duration < 0 || p->duration > 20000000) return STATUS_SUCCESS;
    pthread_mutex_lock(&audio_lock);
    while (audio_closing) pthread_cond_wait(&audio_cond, &audio_lock);
    p->result = AUDCLNT_E_DEVICE_INVALIDATED;
    if (!initialized) goto done;
    p->result = E_OUTOFMEMORY;
    if (!(s = calloc(1, sizeof(*s)))) goto done;
    s->capacity = (p->duration * NX_RATE + 9999999) / 10000000;
    if (s->capacity < 2 * NX_BUFFERS * NX_CHUNK) s->capacity = 2 * NX_BUFFERS * NX_CHUNK;
    s->source_rate = p->fmt->nSamplesPerSec;
    s->source_channels = p->fmt->nChannels;
    s->source_bits = p->fmt->wBitsPerSample;
    s->source_tag = nx_format_tag(p->fmt);
    s->source_frame_bytes = p->fmt->nBlockAlign;
    s->scratch_bytes = (s->capacity * s->source_rate / NX_RATE + 2) * s->source_frame_bytes;
    s->ring = malloc(s->capacity * 4);
    bytes = s->scratch_bytes;
    /* Buffers exposed to a 32-bit client must reside below 4 GB. */
    if (!s->ring || NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&s->scratch,
                                            0xffffffff00000000ULL, &bytes,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
    { nx_free(s); goto done; }
    for (i = 0; i < NX_BUFFERS; i++) if (!output[i].buffer)
    {
        output[i].buffer = memalign(0x1000, 0x1000);
        output[i].buffer_size = 0x1000;
        if (!output[i].buffer) { nx_free(s); goto done; }
    }
    s->volume[0] = s->volume[1] = 1.0f;
    s->flags = p->flags;
    s->next = active;
    active = s;
    *p->stream = (UINT_PTR)s;
    *p->channel_count = 2;
    p->result = S_OK;
done:
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
unsigned int wine_nx_audio_underruns;

static BOOL nx_any_running(void)
{
    struct nx_audio_stream *s;
    for (s = active; s; s = s->next) if (s->running) return TRUE;
    return FALSE;
}

static void nx_reopen_output(void)
{
    struct nx_audio_stream *s;
    if (output_running) audoutStopAudioOut();
    audoutExit();
    initialized = R_SUCCEEDED(audoutInitialize());
    output_running = FALSE;
    memset(output_frames, 0, sizeof(output_frames));
    for (s = active; s; s = s->next)
    {
        memset(s->frames, 0, sizeof(s->frames));
        s->submitted = 0;
        s->failed = !initialized;
    }
}

static short nx_sample(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

static void nx_pump(void)
{
    AudioOutBuffer *released;
    struct nx_audio_stream *s;
    u32 count;
    unsigned int i, j, frames, pending;
    int mix[NX_CHUNK * 2];
    if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count))) goto failed;
    while (count && released)
    {
        for (i = 0; i < NX_BUFFERS; i++) if (released == &output[i])
        {
            output_frames[i] = 0;
            for (s = active; s; s = s->next) if (s->frames[i])
            {
                frames = s->frames[i];
                s->held -= frames;
                s->submitted -= frames;
                s->played += frames;
                s->read = (s->read + frames) % s->capacity;
                s->frames[i] = 0;
            }
            break;
        }
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count))) goto failed;
    }
    if (output_running)
    {
        BOOL queued = FALSE;
        for (i = 0; i < NX_BUFFERS; i++) if (output_frames[i]) { queued = TRUE; break; }
        if (!queued) for (s = active; s; s = s->next)
        {
            if (s->running && s->played && s->held > s->submitted)
            { wine_nx_audio_underruns++; break; }
        }
    }
    for (i = 0; i < NX_BUFFERS; i++) if (!output_frames[i])
    {
        frames = 0;
        for (s = active; s; s = s->next) if (s->running)
        {
            pending = s->held - s->submitted;
            if (pending > frames) frames = pending;
        }
        if (!frames) break;
        if (frames > NX_CHUNK) frames = NX_CHUNK;
        memset(mix, 0, frames * 2 * sizeof(mix[0]));
        for (s = active; s; s = s->next) if (s->running)
        {
            pending = s->held - s->submitted;
            if (pending > frames) pending = frames;
            s->frames[i] = pending;
            for (j = 0; j < pending; j++)
            {
                const short *src = (const short *)s->ring + ((s->read + s->submitted + j) % s->capacity) * 2;
                mix[j * 2] += src[0] * s->volume[0];
                mix[j * 2 + 1] += src[1] * s->volume[1];
            }
        }
        for (j = 0; j < frames * 2; j++) ((short *)output[i].buffer)[j] = nx_sample(mix[j]);
        output[i].data_size = frames * 4;
        armDCacheFlush(output[i].buffer, output[i].data_size);
        if (R_FAILED(audoutAppendAudioOutBuffer(&output[i])))
        {
            for (s = active; s; s = s->next) s->frames[i] = 0;
            goto failed;
        }
        output_frames[i] = frames;
        for (s = active; s; s = s->next) s->submitted += s->frames[i];
    }
    return;
failed:
    for (s = active; s; s = s->next) s->failed = TRUE;
}
static void nx_timer_loop(void *args)
{
    struct nx_audio_stream *s;
    LARGE_INTEGER delay;

    (void)args;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x38);
    delay.QuadPart = -50000;
    for (;;)
    {
        pthread_mutex_lock(&audio_lock);
        if (audio_quit) { pthread_mutex_unlock(&audio_lock); break; }
        if (output_running)
        {
            nx_pump();
            for (s = active; s; s = s->next) if (s->running && s->event) NtSetEvent(s->event, NULL);
        }
        pthread_mutex_unlock(&audio_lock);
        NtDelayExecution(FALSE, &delay);
    }
}
static NTSTATUS nx_release_stream(void *args)
{
    struct release_stream_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    struct nx_audio_stream **cursor;
    HANDLE thread = NULL;
    unsigned int i;
    BOOL last;
    pthread_mutex_lock(&audio_lock);
    for (cursor = &active; *cursor && *cursor != s; cursor = &(*cursor)->next) {}
    if (*cursor == s) *cursor = s->next;
    s->running = FALSE;
    last = !active;
    if (last)
    {
        audio_closing = audio_quit = TRUE;
        thread = audio_thread;
    }
    else
    {
        if (!nx_any_running() && output_running) nx_reopen_output();
        nx_free(s);
    }
    pthread_mutex_unlock(&audio_lock);
    if (!last) { p->result = S_OK; return STATUS_SUCCESS; }
    if (thread) { NtWaitForSingleObject(thread, FALSE, NULL); NtClose(thread); }
    pthread_mutex_lock(&audio_lock);
    nx_reopen_output();
    for (i = 0; i < NX_BUFFERS; i++) { free(output[i].buffer); memset(&output[i], 0, sizeof(output[i])); }
    nx_free(s);
    audio_thread = NULL;
    audio_quit = audio_closing = FALSE;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_lock);
    p->result = S_OK;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_start(void *args)
{
    static const WCHAR name[] = {'a','u','d','o','u','t',0};
    struct start_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    pthread_mutex_lock(&audio_lock);
    if (s->running) p->result = AUDCLNT_E_NOT_STOPPED;
    else if (s->failed || !initialized) p->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if ((s->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !s->event) p->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else
    {
        if (!audio_thread && create_unix_thread(&audio_thread, name, nx_timer_loop, NULL))
        {
            p->result = AUDCLNT_E_DEVICE_INVALIDATED;
            pthread_mutex_unlock(&audio_lock);
            return STATUS_SUCCESS;
        }
        s->running = TRUE;
        nx_pump();
        if (!output_running)
        {
            if (!s->failed && R_SUCCEEDED(audoutStartAudioOut())) output_running = TRUE;
            else s->failed = TRUE;
        }
        p->result = s->failed ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
        if (FAILED(p->result)) s->running = FALSE;
    }
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_stop(void *args)
{
    struct stop_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    pthread_mutex_lock(&audio_lock);
    p->result = s->running ? S_OK : S_FALSE;
    s->running = FALSE;
    if (!nx_any_running() && output_running) nx_reopen_output();
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_reset(void *args)
{
    struct reset_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    pthread_mutex_lock(&audio_lock);
    p->result = AUDCLNT_E_NOT_STOPPED;
    if (!s->running)
    {
        if (s->locked) p->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        else
        {
            s->read = s->held = s->submitted = 0;
            memset(s->frames, 0, sizeof(s->frames));
            s->played = 0;
            p->result = initialized ? S_OK : AUDCLNT_E_DEVICE_INVALIDATED;
        }
    }
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_get_render_buffer(void *args)
{
    struct get_render_buffer_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    pthread_mutex_lock(&audio_lock);
    if (s->locked) p->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (s->failed) p->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (p->frames > s->capacity - s->held) p->result = AUDCLNT_E_BUFFER_TOO_LARGE;
    else { s->locked = p->frames; *p->data = p->frames ? s->scratch : NULL; p->result = S_OK; }
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_release_render_buffer(void *args)
{
    struct release_render_buffer_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    unsigned int i;
    pthread_mutex_lock(&audio_lock);
    if (p->flags & ~AUDCLNT_BUFFERFLAGS_SILENT) p->result = E_INVALIDARG;
    else if (p->written_frames && !s->locked) p->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (p->written_frames > s->locked) p->result = AUDCLNT_E_INVALID_SIZE;
    else
    {
        unsigned int out_frames = p->written_frames * NX_RATE / s->source_rate;
        if (p->written_frames && !out_frames) out_frames = 1;
        if (out_frames > s->capacity - s->held) { p->result = AUDCLNT_E_BUFFER_TOO_LARGE; goto release_done; }
        for (i = 0; i < out_frames; i++)
        {
            BYTE *dst = s->ring + ((s->read + s->held + i) % s->capacity) * 4;
            unsigned int src_frame = (unsigned long long)i * s->source_rate / NX_RATE;
            const BYTE *src = s->scratch + src_frame * s->source_frame_bytes;
            int left = 0, right = 0, c;
            if (!(p->flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                for (c = 0; c < (int)s->source_channels; c++)
                {
                    int sample;
                    if (s->source_tag == WAVE_FORMAT_IEEE_FLOAT)
                    {
                        float value; memcpy(&value, src + c * (s->source_bits / 8), sizeof(value));
                        if (value > 1.0f) value = 1.0f;
                        if (value < -1.0f) value = -1.0f;
                        sample = (int)(value * 32767.0f);
                    }
                    else if (s->source_bits == 8) sample = ((int)src[c] - 128) << 8;
                    else if (s->source_bits == 16) { short value; memcpy(&value, src + c * 2, 2); sample = value; }
                    else if (s->source_bits == 24) { sample = (int)((src[c*3] | (src[c*3+1]<<8) | (src[c*3+2]<<16)) << 8) >> 8; }
                    else { int value; memcpy(&value, src + c * 4, 4); sample = value >> 16; }
                    if (!c) left = sample; else right = sample;
                }
                if (s->source_channels == 1) right = left;
            }
            ((short *)dst)[0] = left * s->volume[0];
            ((short *)dst)[1] = right * s->volume[1];
        }
        s->held += out_frames;
        s->locked = 0;
        p->result = S_OK;
    }
release_done:
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
#define NX_QUERY(name, member, value) \
static NTSTATUS nx_##name(void *args) { struct name##_params *p = args; \
struct nx_audio_stream *s = nx_stream(p->stream); (void)s; pthread_mutex_lock(&audio_lock); \
*p->member = (value); p->result = S_OK; pthread_mutex_unlock(&audio_lock); return STATUS_SUCCESS; }
NX_QUERY(get_buffer_size, frames, s->capacity)
NX_QUERY(get_latency, latency, (REFERENCE_TIME)NX_BUFFERS * NX_CHUNK * 10000000 / NX_RATE)
NX_QUERY(get_current_padding, padding, s->held)
NX_QUERY(get_next_packet_size, frames, 0)
NX_QUERY(get_frequency, freq, NX_RATE * 4)
static NTSTATUS nx_get_position(void *args)
{
    struct get_position_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    LARGE_INTEGER now;
    if (p->device) { p->result = E_NOTIMPL; return STATUS_SUCCESS; }
    pthread_mutex_lock(&audio_lock);
    *p->pos = s->played * 4;
    NtQueryPerformanceCounter(&now, NULL);
    if (p->qpctime) *p->qpctime = now.QuadPart;
    p->result = S_OK;
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_set_volumes(void *args)
{
    struct set_volumes_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    unsigned int i;
    pthread_mutex_lock(&audio_lock);
    for (i = 0; i < 2; i++)
    {
        float v = p->master_volume * p->volumes[i] * p->session_volumes[i];
        s->volume[i] = v >= 0 && v <= 1 ? v : 0;
    }
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_set_event_handle(void *args)
{
    struct set_event_handle_params *p = args;
    struct nx_audio_stream *s = nx_stream(p->stream);
    pthread_mutex_lock(&audio_lock);
    if (!(s->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) p->result = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (s->event) p->result = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else { s->event = p->event; p->result = S_OK; }
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_is_started(void *args)
{
    struct is_started_params *p = args;
    pthread_mutex_lock(&audio_lock);
    p->result = nx_stream(p->stream)->running ? S_OK : S_FALSE;
    pthread_mutex_unlock(&audio_lock);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_get_prop_value(void *args)
{ ((struct get_prop_value_params *)args)->result = E_NOTIMPL; return STATUS_SUCCESS; }
static NTSTATUS nx_get_capture_buffer(void *args)
{ ((struct get_capture_buffer_params *)args)->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE; return STATUS_SUCCESS; }
static NTSTATUS nx_release_capture_buffer(void *args)
{ ((struct release_capture_buffer_params *)args)->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE; return STATUS_SUCCESS; }

static NTSTATUS nx_midi_get_driver(void *args) { *(WCHAR *)args = 0; return STATUS_SUCCESS; }
static NTSTATUS nx_midi_init(void *args)
{
    struct midi_init_params *p = args;
    *p->err = 0;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_midi_release(void *args) { (void)args; return STATUS_SUCCESS; }
static void nx_midi_message_result(UINT msg, UINT *err, struct notify_context *notify, UINT get_num_devs)
{
    *err = msg == get_num_devs ? 0 : MMSYSERR_BADDEVICEID;
    if (notify) notify->send_notify = FALSE;
}
static NTSTATUS nx_midi_out_message(void *args)
{
    struct midi_out_message_params *p = args;
    nx_midi_message_result(p->msg, p->err, p->notify, MODM_GETNUMDEVS);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_midi_in_message(void *args)
{
    struct midi_in_message_params *p = args;
    nx_midi_message_result(p->msg, p->err, p->notify, MIDM_GETNUMDEVS);
    return STATUS_SUCCESS;
}
static NTSTATUS nx_midi_wait(void *args)
{
    struct midi_notify_wait_params *p = args;
    *p->quit = TRUE;
    if (p->notify) p->notify->send_notify = FALSE;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_aux_message(void *args)
{
    struct aux_message_params *p = args;
    *p->err = p->msg == AUXDM_GETNUMDEVS || p->msg == DRVM_INIT || p->msg == DRVM_EXIT ?
              0 : MMSYSERR_BADDEVICEID;
    return STATUS_SUCCESS;
}

const unixlib_entry_t wine_nx_audio_unix_funcs[] =
{
    nx_process_attach, nx_not_implemented, nx_process_attach, nx_process_attach, nx_get_endpoint_ids,
    nx_create_stream, nx_release_stream, nx_start, nx_stop, nx_reset,
    nx_get_render_buffer, nx_release_render_buffer, nx_get_capture_buffer,
    nx_release_capture_buffer, nx_is_format_supported, nx_not_implemented,
    nx_get_mix_format, nx_get_device_period, nx_get_buffer_size, nx_get_latency,
    nx_get_current_padding, nx_get_next_packet_size, nx_get_frequency, nx_get_position,
    nx_set_volumes, nx_set_event_handle, nx_not_implemented, nx_test_connect, nx_is_started,
    nx_get_prop_value, nx_midi_get_driver, nx_midi_init, nx_midi_release,
    nx_midi_out_message, nx_midi_in_message, nx_midi_wait, nx_aux_message
};
C_ASSERT(ARRAY_SIZE(wine_nx_audio_unix_funcs) == funcs_count);
const unsigned int wine_nx_audio_unix_count = ARRAY_SIZE(wine_nx_audio_unix_funcs);

/* WoW64 thunks adapted from dlls/wineoss.drv/oss.c, LGPL-2.1-or-later,
 * Copyright 2021 Jacek Caban; 2021-2022 Huw Davies. */
typedef UINT PTR32;

static NTSTATUS nx_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    nx_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    nx_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        UINT flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    nx_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    nx_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    nx_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    nx_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS nx_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    nx_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    nx_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    nx_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    nx_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    nx_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    nx_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    nx_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    nx_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    nx_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return nx_set_volumes(&params);
}

static NTSTATUS nx_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };

    nx_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS nx_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    nx_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            break;
        }
    }
    return STATUS_SUCCESS;
}

/* MIDI/aux enumeration must report zero devices, not leave output fields
 * untouched when winmm probes the selected driver. */
static NTSTATUS nx_wow64_midi_init(void *args)
{ *(UINT *)ULongToPtr(*(PTR32 *)args) = 0; return STATUS_SUCCESS; }
static NTSTATUS nx_wow64_midi_message(void *args)
{
    struct { UINT dev, msg; PTR32 user, p1, p2, err, notify; } *p = args;
    *(UINT *)ULongToPtr(p->err) = (p->msg == MODM_GETNUMDEVS || p->msg == MIDM_GETNUMDEVS) ? 0 : MMSYSERR_BADDEVICEID;
    if (p->notify) *(BOOL *)ULongToPtr(p->notify) = FALSE;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_wow64_midi_wait(void *args)
{
    struct { PTR32 quit, notify; } *p = args;
    *(BOOL *)ULongToPtr(p->quit) = TRUE;
    *(BOOL *)ULongToPtr(p->notify) = FALSE;
    return STATUS_SUCCESS;
}
static NTSTATUS nx_wow64_aux_message(void *args)
{
    struct { UINT dev, msg; PTR32 user, p1, p2, err; } *p = args;
    *(UINT *)ULongToPtr(p->err) = p->msg == AUXDM_GETNUMDEVS || p->msg == DRVM_INIT ||
        p->msg == DRVM_EXIT ? 0 : MMSYSERR_BADDEVICEID;
    return STATUS_SUCCESS;
}
const unixlib_entry_t wine_nx_audio_wow64_unix_funcs[] =
{
    nx_process_attach, nx_not_implemented, nx_process_attach, nx_process_attach, nx_wow64_get_endpoint_ids,
    nx_wow64_create_stream, nx_wow64_release_stream, nx_start, nx_stop, nx_reset,
    nx_wow64_get_render_buffer, nx_release_render_buffer, nx_wow64_get_capture_buffer,
    nx_release_capture_buffer, nx_wow64_is_format_supported, nx_not_implemented,
    nx_wow64_get_mix_format, nx_wow64_get_device_period, nx_wow64_get_buffer_size,
    nx_wow64_get_latency, nx_wow64_get_current_padding, nx_wow64_get_next_packet_size,
    nx_wow64_get_frequency, nx_wow64_get_position, nx_wow64_set_volumes,
    nx_wow64_set_event_handle, nx_not_implemented, nx_wow64_test_connect, nx_is_started,
    nx_wow64_get_prop_value, nx_midi_get_driver, nx_wow64_midi_init, nx_midi_release,
    nx_wow64_midi_message, nx_wow64_midi_message, nx_wow64_midi_wait, nx_wow64_aux_message
};
C_ASSERT(ARRAY_SIZE(wine_nx_audio_wow64_unix_funcs) == funcs_count);
const unsigned int wine_nx_audio_wow64_unix_count = ARRAY_SIZE(wine_nx_audio_wow64_unix_funcs);
