/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "mfapi.h"
#include "mferror.h"
#include "../../dlls/winegstreamer/unixlib.h"

#define PACKET_LIMIT 16
static const AVRational media_time_base = {1, 10000000};

struct media_decoder
{
    pthread_mutex_t mutex;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packets[PACKET_LIMIT];
    unsigned int head, count, capacity;
    BOOL video, have_frame, draining, drain_sent, eof;
    struct wg_transform_attrs attrs;
    MFVIDEOFORMAT video_type;
    WAVEFORMATEXTENSIBLE audio_type;
    enum AVPixelFormat pixel_format;
    enum AVSampleFormat sample_format;
    struct SwsContext *scaler;
    SwrContext *resampler;
    AVChannelLayout input_layout;
    int input_rate, input_format;
    BYTE *pcm;
    unsigned int pcm_capacity, pcm_size, pcm_offset;
    INT64 pcm_time;
    INT64 next_audio_time;
};

static struct media_decoder *decoder(wg_transform_t handle)
{
    return (void *)(UINT_PTR)handle;
}

static void clear_decoder(struct media_decoder *d)
{
    for (unsigned int i = 0; i < PACKET_LIMIT; i++) av_packet_free(&d->packets[i]);
    if (d->codec && avcodec_is_open(d->codec)) avcodec_flush_buffers(d->codec);
    if (d->frame) av_frame_unref(d->frame);
    swr_free(&d->resampler);
    av_channel_layout_uninit(&d->input_layout);
    d->head = d->count = d->pcm_offset = d->pcm_size = 0;
    d->have_frame = d->draining = d->drain_sent = d->eof = FALSE;
    d->next_audio_time = AV_NOPTS_VALUE;
}

static void free_decoder(struct media_decoder *d)
{
    clear_decoder(d);
    avcodec_free_context(&d->codec);
    av_frame_free(&d->frame);
    sws_freeContext(d->scaler);
    av_free(d->pcm);
    pthread_mutex_destroy(&d->mutex);
    free(d);
}

static BOOL audio_format(const struct wg_media_type *type, WAVEFORMATEXTENSIBLE *format,
                         const BYTE **extra, unsigned int *extra_size)
{
    const WAVEFORMATEX *wfx = type->u.audio;
    unsigned int header = sizeof(*wfx);
    if (!IsEqualGUID(&type->major, &MFMediaType_Audio) || !wfx || type->format_size < header
            || wfx->cbSize > type->format_size - header || !wfx->nChannels || wfx->nChannels > 8
            || !wfx->nSamplesPerSec || wfx->nSamplesPerSec > 192000) return FALSE;
    memset(format, 0, sizeof(*format));
    format->Format = *wfx;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (wfx->cbSize < sizeof(*format) - header) return FALSE;
        *format = *(const WAVEFORMATEXTENSIBLE *)wfx;
        header = sizeof(*format);
        format->Format.wFormatTag = format->SubFormat.Data1;
    }
    *extra = (const BYTE *)wfx + header;
    *extra_size = sizeof(*wfx) + wfx->cbSize - header;
    return TRUE;
}

static enum AVPixelFormat pixel_format(const GUID *subtype)
{
    if (IsEqualGUID(subtype, &MFVideoFormat_NV12)) return AV_PIX_FMT_NV12;
    if (IsEqualGUID(subtype, &MFVideoFormat_I420) || IsEqualGUID(subtype, &MFVideoFormat_IYUV)
            || IsEqualGUID(subtype, &MFVideoFormat_YV12)) return AV_PIX_FMT_YUV420P;
    if (IsEqualGUID(subtype, &MFVideoFormat_YUY2)) return AV_PIX_FMT_YUYV422;
    return AV_PIX_FMT_NONE;
}

static NTSTATUS set_output(struct media_decoder *d, const struct wg_media_type *type)
{
    if (d->video)
    {
        const MFVIDEOFORMAT *format = type->u.video;
        enum AVPixelFormat pixel;
        if (!IsEqualGUID(&type->major, &MFMediaType_Video) || !format
                || type->format_size < sizeof(*format) || (pixel = pixel_format(&format->guidFormat)) == AV_PIX_FMT_NONE
                || !format->videoInfo.dwWidth || format->videoInfo.dwWidth > 8192
                || !format->videoInfo.dwHeight || format->videoInfo.dwHeight > 8192)
            return STATUS_NOT_SUPPORTED;
        d->video_type = *format;
        d->video_type.dwSize = sizeof(*format);
        d->pixel_format = pixel;
    }
    else
    {
        WAVEFORMATEXTENSIBLE format;
        const BYTE *extra;
        unsigned int size;
        if (!audio_format(type, &format, &extra, &size)) return STATUS_NOT_SUPPORTED;
        if (format.Format.wFormatTag == WAVE_FORMAT_PCM && format.Format.wBitsPerSample == 16)
            d->sample_format = AV_SAMPLE_FMT_S16;
        else if (format.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.Format.wBitsPerSample == 32)
            d->sample_format = AV_SAMPLE_FMT_FLT;
        else return STATUS_NOT_SUPPORTED;
        size = format.Format.nChannels * format.Format.wBitsPerSample / 8;
        if (format.Format.nBlockAlign && format.Format.nBlockAlign != size)
            return STATUS_INVALID_PARAMETER;
        format.Format.nBlockAlign = size;
        format.Format.nAvgBytesPerSec = size * format.Format.nSamplesPerSec;
        d->audio_type = format;
        d->audio_type.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        d->audio_type.Format.cbSize = sizeof(d->audio_type) - sizeof(WAVEFORMATEX);
        d->audio_type.SubFormat = d->sample_format == AV_SAMPLE_FMT_S16 ? MFAudioFormat_PCM : MFAudioFormat_Float;
        d->audio_type.Samples.wValidBitsPerSample = format.Format.wBitsPerSample;
        swr_free(&d->resampler);
        av_channel_layout_uninit(&d->input_layout);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS media_create(void *args)
{
    struct wg_transform_create_params *p = args;
    struct media_decoder *d;
    const AVCodec *codec;
    const BYTE *extra;
    unsigned int extra_size;
    enum AVCodecID id;
    NTSTATUS status;

    p->transform = 0;
    if (p->attrs.output_plane_align > 255
            || (p->attrs.output_plane_align & (p->attrs.output_plane_align + 1))) return STATUS_INVALID_PARAMETER;
    if (!(d = calloc(1, sizeof(*d)))) return STATUS_NO_MEMORY;
    if (pthread_mutex_init(&d->mutex, NULL)) { free(d); return STATUS_NO_MEMORY; }
    d->attrs = p->attrs;
    d->next_audio_time = AV_NOPTS_VALUE;
    d->capacity = min(p->attrs.input_queue_length, PACKET_LIMIT - 1) + 1;
    d->video = IsEqualGUID(&p->input_type.major, &MFMediaType_Video);
    if (!(d->codec = avcodec_alloc_context3(NULL)) || !(d->frame = av_frame_alloc()))
    { status = STATUS_NO_MEMORY; goto failed; }
    if ((status = set_output(d, &p->output_type))) goto failed;
    status = STATUS_NOT_SUPPORTED;
    if (d->video)
    {
        const MFVIDEOFORMAT *format = p->input_type.u.video;
        if (!format || p->input_type.format_size < sizeof(*format)
                || (!IsEqualGUID(&format->guidFormat, &MFVideoFormat_H264)
                    && !IsEqualGUID(&format->guidFormat, &MFVideoFormat_H264_ES))) goto failed;
        id = AV_CODEC_ID_H264;
        extra = (const BYTE *)(format + 1);
        extra_size = p->input_type.format_size - sizeof(*format);
        d->codec->thread_count = 2;
        d->codec->thread_type = FF_THREAD_SLICE;
        d->codec->max_pixels = 8192 * 8192;
    }
    else
    {
        WAVEFORMATEXTENSIBLE format;
        if (!audio_format(&p->input_type, &format, &extra, &extra_size)) goto failed;
        if (format.Format.wFormatTag == WAVE_FORMAT_MPEG_HEAAC)
        {
            unsigned int header = sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX);
            if (extra_size < header) goto failed;
            if (((const HEAACWAVEINFO *)p->input_type.u.audio)->wPayloadType > 1) goto failed;
            extra += header;
            extra_size -= header;
        }
        else if (format.Format.wFormatTag != WAVE_FORMAT_RAW_AAC1
                && format.Format.wFormatTag != WAVE_FORMAT_MPEG_ADTS_AAC) goto failed;
        id = AV_CODEC_ID_AAC;
        d->codec->sample_rate = format.Format.nSamplesPerSec;
        av_channel_layout_default(&d->codec->ch_layout, format.Format.nChannels);
        d->codec->thread_count = 1;
    }
    if (!(codec = avcodec_find_decoder(id))) goto failed;
    if (extra_size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) goto failed;
    if (extra_size)
    {
        if (!(d->codec->extradata = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE)))
        { status = STATUS_NO_MEMORY; goto failed; }
        memcpy(d->codec->extradata, extra, extra_size);
        d->codec->extradata_size = extra_size;
    }
    d->codec->pkt_timebase = media_time_base;
    if (avcodec_open2(d->codec, codec, NULL) < 0) goto failed;
    p->transform = (UINT_PTR)d;
    return STATUS_SUCCESS;
failed:
    free_decoder(d);
    return status;
}

static NTSTATUS media_destroy(void *args)
{
    free_decoder(decoder(*(wg_transform_t *)args));
    return STATUS_SUCCESS;
}

static NTSTATUS media_push(void *args)
{
    struct wg_transform_push_data_params *p = args;
    struct media_decoder *d = decoder(p->transform);
    struct wg_sample *s = p->sample;
    AVPacket *packet;
    pthread_mutex_lock(&d->mutex);
    p->result = MF_E_NOTACCEPTING;
    if (d->draining && d->eof && !d->pcm_size) clear_decoder(d);
    if (d->draining || d->count == d->capacity) goto done;
    p->result = E_INVALIDARG;
    if (!s->data || !s->size || s->size > s->max_size || s->size > 64 * 1024 * 1024) goto done;
    p->result = E_OUTOFMEMORY;
    if (!(packet = av_packet_alloc())) goto done;
    if (av_new_packet(packet, s->size) < 0) { av_packet_free(&packet); goto done; }
    memcpy(packet->data, (const void *)(UINT_PTR)s->data, s->size);
    packet->pts = (s->flags & WG_SAMPLE_FLAG_HAS_PTS) ? s->pts : AV_NOPTS_VALUE;
    packet->dts = AV_NOPTS_VALUE;
    packet->duration = (s->flags & WG_SAMPLE_FLAG_HAS_DURATION) ? s->duration : 0;
    if (s->flags & WG_SAMPLE_FLAG_SYNC_POINT) packet->flags |= AV_PKT_FLAG_KEY;
    d->packets[(d->head + d->count++) % PACKET_LIMIT] = packet;
    p->result = S_OK;
done:
    pthread_mutex_unlock(&d->mutex);
    return STATUS_SUCCESS;
}

static HRESULT receive_frame(struct media_decoder *d)
{
    int ret;
    if (d->have_frame) return S_OK;
    for (;;)
    {
        ret = avcodec_receive_frame(d->codec, d->frame);
        if (!ret) { d->have_frame = TRUE; return S_OK; }
        if (ret == AVERROR_EOF)
        {
            d->eof = TRUE;
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
        if (ret != AVERROR(EAGAIN)) return E_FAIL;
        if (d->count)
        {
            AVPacket **packet = &d->packets[d->head];
            ret = avcodec_send_packet(d->codec, *packet);
            if (ret < 0) return E_FAIL;
            av_packet_free(packet);
            d->head = (d->head + 1) % PACKET_LIMIT;
            d->count--;
        }
        else if (d->draining && !d->drain_sent)
        {
            if (avcodec_send_packet(d->codec, NULL) < 0) return E_FAIL;
            d->drain_sent = TRUE;
        }
        else return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
}

static HRESULT read_video(struct media_decoder *d, struct wg_sample *sample)
{
    AVFrame *frame = d->frame;
    MFVideoInfo *info = &d->video_type.videoInfo;
    unsigned int align = d->attrs.output_plane_align;
    unsigned int width = (frame->width + align) & ~align, height = (frame->height + align) & ~align;
    uint8_t *planes[4];
    int strides[4], size, stride_width = width;
    if (info->dwWidth != width || info->dwHeight != height)
    {
        info->dwWidth = width;
        info->dwHeight = height;
        info->MinimumDisplayAperture = (MFVideoArea){{0}, {0}, {frame->width, frame->height}};
        info->GeometricAperture = info->MinimumDisplayAperture;
        info->PanScanAperture = info->MinimumDisplayAperture;
        return MF_E_TRANSFORM_STREAM_CHANGE;
    }
    if (sample->stride)
    {
        if (sample->stride < 0) return E_INVALIDARG;
        stride_width = d->pixel_format == AV_PIX_FMT_YUYV422 ? sample->stride / 2 : sample->stride;
        if (stride_width < (int)width) return E_INVALIDARG;
    }
    size = av_image_fill_arrays(planes, strides, (void *)(UINT_PTR)sample->data,
                               d->pixel_format, stride_width, height, 1);
    if (size < 0 || (unsigned int)size > sample->max_size) return MF_E_BUFFERTOOSMALL;
    if (stride_width != frame->width || (int)height != frame->height)
        memset((void *)(UINT_PTR)sample->data, 0, size);
    if (IsEqualGUID(&d->video_type.guidFormat, &MFVideoFormat_YV12))
    {
        BYTE *tmp = planes[1]; planes[1] = planes[2]; planes[2] = tmp;
    }
    d->scaler = sws_getCachedContext(d->scaler, frame->width, frame->height, frame->format,
                                   frame->width, frame->height, d->pixel_format, SWS_BILINEAR, NULL, NULL, NULL);
    if (!d->scaler) return E_OUTOFMEMORY;
    if (sws_scale(d->scaler, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                  planes, strides) != frame->height) return E_FAIL;
    sample->size = size;
    sample->flags = WG_SAMPLE_FLAG_SYNC_POINT | WG_SAMPLE_FLAG_PRESERVE_TIMESTAMPS;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_PTS;
        sample->pts = frame->best_effort_timestamp;
    }
    if (frame->duration > 0)
    {
        sample->flags |= WG_SAMPLE_FLAG_HAS_DURATION;
        sample->duration = frame->duration;
    }
    av_frame_unref(frame);
    d->have_frame = FALSE;
    return S_OK;
}

static HRESULT convert_audio(struct media_decoder *d, BOOL flush)
{
    AVFrame *frame = d->frame;
    WAVEFORMATEX *out = &d->audio_type.Format;
    int count, ret, samples = flush ? 0 : frame->nb_samples;
    INT64 time = d->next_audio_time;
    if (flush && !d->resampler) return S_OK;
    if (!d->resampler)
    {
        AVChannelLayout layout;
        if (d->audio_type.dwChannelMask)
            av_channel_layout_from_mask(&layout, d->audio_type.dwChannelMask);
        else av_channel_layout_default(&layout, out->nChannels);
        ret = swr_alloc_set_opts2(&d->resampler, &layout, d->sample_format, out->nSamplesPerSec,
                                 &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
        av_channel_layout_uninit(&layout);
        if (ret < 0 || swr_init(d->resampler) < 0) { swr_free(&d->resampler); return E_FAIL; }
        if (av_channel_layout_copy(&d->input_layout, &frame->ch_layout) < 0)
        { swr_free(&d->resampler); return E_OUTOFMEMORY; }
        d->input_rate = frame->sample_rate;
        d->input_format = frame->format;
    }
    if (!flush)
    {
        if (frame->sample_rate != d->input_rate || frame->format != d->input_format
                || av_channel_layout_compare(&frame->ch_layout, &d->input_layout)) return MF_E_INVALIDMEDIATYPE;
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
            time = frame->best_effort_timestamp - av_rescale(swr_get_delay(d->resampler, d->input_rate),
                                                           10000000, d->input_rate);
    }
    count = swr_get_out_samples(d->resampler, samples);
    if (count < 0 || count > INT_MAX / out->nBlockAlign) return E_FAIL;
    av_fast_malloc(&d->pcm, &d->pcm_capacity, count * out->nBlockAlign);
    if (!d->pcm) return E_OUTOFMEMORY;
    ret = swr_convert(d->resampler, &d->pcm, count, flush ? NULL : (const uint8_t **)frame->extended_data, samples);
    if (ret < 0) return E_FAIL;
    d->pcm_time = time;
    d->next_audio_time = time == AV_NOPTS_VALUE ? time : time + av_rescale(ret, 10000000, out->nSamplesPerSec);
    d->pcm_size = ret * out->nBlockAlign;
    d->pcm_offset = 0;
    d->have_frame = FALSE;
    av_frame_unref(frame);
    return S_OK;
}

static HRESULT read_audio(struct media_decoder *d, struct wg_sample *s)
{
    WAVEFORMATEX *out = &d->audio_type.Format;
    unsigned int size;
    HRESULT hr;
    while (!d->pcm_size)
    {
        hr = receive_frame(d);
        if (FAILED(hr) && !d->eof) return hr;
        if (FAILED(hr = convert_audio(d, d->eof))) return hr;
        if (d->eof && !d->pcm_size) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    size = min(d->pcm_size - d->pcm_offset, s->max_size);
    size -= size % out->nBlockAlign;
    if (!size) return MF_E_BUFFERTOOSMALL;
    memcpy((void *)(UINT_PTR)s->data, d->pcm + d->pcm_offset, size);
    s->size = size;
    s->flags = WG_SAMPLE_FLAG_SYNC_POINT | WG_SAMPLE_FLAG_HAS_DURATION;
    s->duration = av_rescale(size / out->nBlockAlign, 10000000, out->nSamplesPerSec);
    if (d->pcm_time != AV_NOPTS_VALUE)
    {
        s->flags |= WG_SAMPLE_FLAG_HAS_PTS;
        s->pts = d->pcm_time + av_rescale(d->pcm_offset / out->nBlockAlign, 10000000, out->nSamplesPerSec);
    }
    d->pcm_offset += size;
    if (d->pcm_offset == d->pcm_size) d->pcm_offset = d->pcm_size = 0;
    else s->flags |= WG_SAMPLE_FLAG_INCOMPLETE;
    return S_OK;
}

static NTSTATUS media_read(void *args)
{
    struct wg_transform_read_data_params *p = args;
    struct media_decoder *d = decoder(p->transform);
    pthread_mutex_lock(&d->mutex);
    p->sample->size = 0;
    if (!p->sample->data) p->result = E_INVALIDARG;
    else if (!d->video) p->result = read_audio(d, p->sample);
    else if (SUCCEEDED(p->result = receive_frame(d))) p->result = read_video(d, p->sample);
    pthread_mutex_unlock(&d->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS media_get_type(void *args)
{
    struct wg_transform_get_output_type_params *p = args;
    struct media_decoder *d = decoder(p->transform);
    UINT32 capacity = p->media_type.format_size;
    NTSTATUS status = STATUS_BUFFER_TOO_SMALL;
    pthread_mutex_lock(&d->mutex);
    p->media_type.major = d->video ? MFMediaType_Video : MFMediaType_Audio;
    p->media_type.format_size = d->video ? sizeof(d->video_type) : sizeof(d->audio_type);
    if (capacity >= p->media_type.format_size && p->media_type.u.format)
    {
        memcpy(p->media_type.u.format, d->video ? (void *)&d->video_type : (void *)&d->audio_type,
               p->media_type.format_size);
        status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&d->mutex);
    return status;
}

static NTSTATUS media_set_type(void *args)
{
    struct wg_transform_set_output_type_params *p = args;
    struct media_decoder *d = decoder(p->transform);
    NTSTATUS status;
    pthread_mutex_lock(&d->mutex);
    status = set_output(d, &p->media_type);
    pthread_mutex_unlock(&d->mutex);
    return status;
}

static NTSTATUS media_status(void *args)
{
    struct wg_transform_get_status_params *p = args;
    struct media_decoder *d = decoder(p->transform);
    pthread_mutex_lock(&d->mutex);
    p->accepts_input = (!d->draining || (d->eof && !d->pcm_size)) && d->count < d->capacity;
    pthread_mutex_unlock(&d->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS media_drain(void *args)
{
    struct media_decoder *d = decoder(*(wg_transform_t *)args);
    pthread_mutex_lock(&d->mutex);
    d->draining = TRUE;
    pthread_mutex_unlock(&d->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS media_flush(void *args)
{
    struct media_decoder *d = decoder(*(wg_transform_t *)args);
    pthread_mutex_lock(&d->mutex);
    clear_decoder(d);
    pthread_mutex_unlock(&d->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS media_init(void *args) { return STATUS_SUCCESS; }
static NTSTATUS media_unsupported(void *args) { return STATUS_NOT_SUPPORTED; }

struct media_type32 { GUID major; UINT32 format_size, format; };
static struct wg_media_type type32(const struct media_type32 *type)
{
    return (struct wg_media_type){type->major, type->format_size, {.format = ULongToPtr(type->format)}};
}

static NTSTATUS wow64_create(void *args)
{
    struct { wg_transform_t transform; struct media_type32 input, output; struct wg_transform_attrs attrs; } *p = args;
    struct wg_transform_create_params params = {0, type32(&p->input), type32(&p->output), p->attrs};
    NTSTATUS status = media_create(&params);
    p->transform = params.transform;
    return status;
}

static NTSTATUS wow64_get_type(void *args)
{
    struct { wg_transform_t transform; struct media_type32 type; } *p = args;
    struct wg_transform_get_output_type_params params = {p->transform, type32(&p->type)};
    NTSTATUS status = media_get_type(&params);
    p->type.major = params.media_type.major;
    p->type.format_size = params.media_type.format_size;
    return status;
}

static NTSTATUS wow64_set_type(void *args)
{
    struct { wg_transform_t transform; struct media_type32 type; } *p = args;
    struct wg_transform_set_output_type_params params = {p->transform, type32(&p->type)};
    return media_set_type(&params);
}

static NTSTATUS wow64_push(void *args)
{
    struct { wg_transform_t transform; UINT32 sample; HRESULT result; } *p = args;
    struct wg_transform_push_data_params params = {p->transform, ULongToPtr(p->sample)};
    NTSTATUS status = media_push(&params);
    p->result = params.result;
    return status;
}

static NTSTATUS wow64_read(void *args)
{
    struct { wg_transform_t transform; UINT32 sample; HRESULT result; } *p = args;
    struct wg_transform_read_data_params params = {p->transform, ULongToPtr(p->sample)};
    NTSTATUS status = media_read(&params);
    p->result = params.result;
    return status;
}

#define MEDIA_TABLE(create, get_type, set_type, push, read) \
    [0 ... unix_wg_funcs_count - 1] = media_unsupported, \
    [unix_wg_init_gstreamer] = media_init, \
    [unix_wg_transform_create] = create, \
    [unix_wg_transform_destroy] = media_destroy, \
    [unix_wg_transform_get_output_type] = get_type, \
    [unix_wg_transform_set_output_type] = set_type, \
    [unix_wg_transform_push_data] = push, \
    [unix_wg_transform_read_data] = read, \
    [unix_wg_transform_get_status] = media_status, \
    [unix_wg_transform_drain] = media_drain, \
    [unix_wg_transform_flush] = media_flush, \
    [unix_wg_transform_notify_qos] = media_init

const unixlib_entry_t wine_nx_media_unix_funcs[] =
{ MEDIA_TABLE(media_create, media_get_type, media_set_type, media_push, media_read) };
const unixlib_entry_t wine_nx_media_wow64_unix_funcs[] =
{ MEDIA_TABLE(wow64_create, wow64_get_type, wow64_set_type, wow64_push, wow64_read) };
const unsigned int wine_nx_media_unix_count = unix_wg_funcs_count;
