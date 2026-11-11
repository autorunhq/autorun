/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
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

#define PARSER_IO_SIZE (128 * 1024)
#define PARSER_QUEUE_LIMIT (32 * 1024 * 1024)

struct parser_packet
{
    AVPacket *packet;
    struct parser_packet *next;
};

struct media_parser;

struct parser_stream
{
    struct media_parser *parser;
    UINT32 number;
    AVStream *av_stream;
    AVCodecContext *codec;
    AVFrame *frame;
    struct parser_packet *head, *tail;
    size_t queued;
    struct wg_format codec_format, current_format, desired;
    BOOL has_codec_format, video, passthrough, enabled, eos, drain_sent, flushed, discontinuity;
    enum AVSampleFormat pcm_format;
    unsigned int errors;
    UINT64 duration;
    INT64 drop_before, next_time;
    struct SwsContext *scaler;
    SwrContext *resampler;
    AVChannelLayout resampler_layout;
    int resampler_rate, resampler_format;
    BYTE *data;
    unsigned int capacity;
    struct wg_parser_buffer buffer;
    BOOL has_buffer, returned;
    char *tags[WG_PARSER_TAG_COUNT];
};

struct media_parser
{
    pthread_mutex_t mutex, demux;
    pthread_cond_t read_cond, done_cond;
    BOOL output_compressed, pts_rebased, connected, demux_eof, fresh;
    struct
    {
        UINT64 offset;
        UINT32 size;
        BYTE *data;
        int result;
        BOOL done;
    } request;
    UINT64 file_size, position;
    INT64 base_pts, stop;
    AVFormatContext *context;
    AVIOContext *io;
    AVPacket *packet;
    struct parser_stream **streams, **map;
    unsigned int stream_count, map_count;
};

static struct media_parser *parser_from_handle(wg_parser_t handle)
{
    return (void *)(UINT_PTR)handle;
}

static struct parser_stream *parser_stream_from_handle(wg_parser_stream_t handle)
{
    return (void *)(UINT_PTR)handle;
}

static INT64 parser_time(const struct parser_stream *s, INT64 time)
{
    const struct media_parser *p = s->parser;
    if (time == AV_NOPTS_VALUE) return time;
    time = av_rescale_q(time, s->av_stream->time_base, media_time_base);
    if (p->pts_rebased && p->base_pts != AV_NOPTS_VALUE) time -= p->base_pts;
    return time;
}

static int parser_io_read(void *opaque, uint8_t *data, int size)
{
    struct media_parser *p = opaque;
    int result;

    pthread_mutex_lock(&p->mutex);
    if (!p->connected) result = AVERROR_EXIT;
    else if (size <= 0) result = AVERROR(EINVAL);
    else if (p->position >= p->file_size) result = AVERROR_EOF;
    else
    {
        p->request.offset = p->position;
        p->request.size = min((UINT64)size, p->file_size - p->position);
        p->request.data = data;
        p->request.done = FALSE;
        pthread_cond_broadcast(&p->read_cond);
        while (p->connected && !p->request.done)
            pthread_cond_wait(&p->done_cond, &p->mutex);
        result = p->request.done ? p->request.result : AVERROR_EXIT;
        p->request.size = 0;
        p->request.data = NULL;
        p->request.done = FALSE;
        if (result > 0) p->position += result;
    }
    pthread_mutex_unlock(&p->mutex);
    return result;
}

static int64_t parser_io_seek(void *opaque, int64_t offset, int whence)
{
    struct media_parser *p = opaque;
    int64_t position;

    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return p->file_size;
    if (whence == SEEK_SET) position = offset;
    else if (whence == SEEK_CUR) position = p->position + offset;
    else if (whence == SEEK_END) position = p->file_size + offset;
    else return AVERROR(EINVAL);
    if (position < 0) return AVERROR(EINVAL);
    pthread_mutex_lock(&p->mutex);
    p->position = position;
    pthread_mutex_unlock(&p->mutex);
    return position;
}

static void parser_stop_io(struct media_parser *p)
{
    pthread_mutex_lock(&p->mutex);
    p->connected = FALSE;
    pthread_cond_broadcast(&p->read_cond);
    pthread_cond_broadcast(&p->done_cond);
    pthread_mutex_unlock(&p->mutex);
}

static void parser_queue_clear(struct parser_stream *s)
{
    struct parser_packet *node;
    while ((node = s->head))
    {
        s->head = node->next;
        av_packet_free(&node->packet);
        free(node);
    }
    s->tail = NULL;
    s->queued = 0;
}

static AVPacket *parser_queue_pop(struct parser_stream *s)
{
    struct parser_packet *node = s->head;
    AVPacket *packet;
    if (!node) return NULL;
    if (!(s->head = node->next)) s->tail = NULL;
    packet = node->packet;
    s->queued -= packet->size;
    free(node);
    return packet;
}

static BOOL parser_read_packet(struct media_parser *p)
{
    struct parser_stream *s = NULL;
    struct parser_packet *node;
    int ret;

    if (p->demux_eof) return FALSE;
    if ((ret = av_read_frame(p->context, p->packet)) < 0)
    {
        if (ret == AVERROR(EAGAIN)) return TRUE;
        p->demux_eof = TRUE;
        return FALSE;
    }
    p->fresh = FALSE;
    if (p->packet->stream_index >= 0 && (unsigned int)p->packet->stream_index < p->map_count)
        s = p->map[p->packet->stream_index];
    if (!s || !s->enabled || s->eos || p->packet->size <= 0
            || s->queued + p->packet->size > PARSER_QUEUE_LIMIT || !(node = calloc(1, sizeof(*node))))
    {
        av_packet_unref(p->packet);
        return TRUE;
    }
    if (!(node->packet = av_packet_alloc()))
    {
        free(node);
        av_packet_unref(p->packet);
        return TRUE;
    }
    av_packet_move_ref(node->packet, p->packet);
    if (s->tail) s->tail->next = node;
    else s->head = node;
    s->tail = node;
    s->queued += node->packet->size;
    return TRUE;
}

static BYTE *parser_reserve(struct parser_stream *s, size_t size)
{
    if (!size || size > UINT_MAX) return NULL;
    av_fast_malloc(&s->data, &s->capacity, size);
    return s->data;
}

static void parser_emit(struct parser_stream *s, UINT32 size, INT64 pts, INT64 duration, BOOL delta)
{
    struct wg_parser_buffer *buffer = &s->buffer;

    pthread_mutex_lock(&s->parser->mutex);
    memset(buffer, 0, sizeof(*buffer));
    if ((buffer->has_pts = pts != AV_NOPTS_VALUE)) buffer->pts = max(pts, 0);
    if ((buffer->has_duration = duration > 0)) buffer->duration = duration;
    buffer->size = size;
    buffer->stream = s->number;
    buffer->delta = !!delta;
    buffer->discontinuity = !!s->discontinuity;
    s->discontinuity = FALSE;
    s->has_buffer = TRUE;
    s->returned = FALSE;
    pthread_mutex_unlock(&s->parser->mutex);
}

static BOOL parser_keep(struct parser_stream *s, INT64 pts, INT64 duration)
{
    if (pts == AV_NOPTS_VALUE) return TRUE;
    if (pts >= s->parser->stop)
    {
        s->eos = TRUE;
        return FALSE;
    }
    if (s->drop_before == AV_NOPTS_VALUE) return TRUE;
    if (duration > 0 ? pts + duration <= s->drop_before : pts < s->drop_before) return FALSE;
    s->drop_before = AV_NOPTS_VALUE;
    return TRUE;
}

static unsigned int parser_video_layout(wg_video_format format, unsigned int width, unsigned int height,
                                        enum AVPixelFormat *pixel, unsigned int offsets[3], int strides[3])
{
    unsigned int luma = (width + 3) & ~3, chroma = ((width + 1) / 2 + 3) & ~3, rows = (height + 1) & ~1;
    unsigned int bytes, swap;

    offsets[0] = offsets[1] = offsets[2] = 0;
    strides[0] = strides[1] = strides[2] = 0;
    switch (format)
    {
        case WG_VIDEO_FORMAT_BGRA: *pixel = AV_PIX_FMT_BGRA; bytes = 4; break;
        case WG_VIDEO_FORMAT_BGRx: *pixel = AV_PIX_FMT_BGR0; bytes = 4; break;
        case WG_VIDEO_FORMAT_RGBA: *pixel = AV_PIX_FMT_RGBA; bytes = 4; break;
        case WG_VIDEO_FORMAT_AYUV: *pixel = AV_PIX_FMT_VUYA; bytes = 4; break;
        case WG_VIDEO_FORMAT_BGR: *pixel = AV_PIX_FMT_BGR24; bytes = 3; break;
        case WG_VIDEO_FORMAT_RGB15: *pixel = AV_PIX_FMT_RGB555LE; bytes = 2; break;
        case WG_VIDEO_FORMAT_RGB16: *pixel = AV_PIX_FMT_RGB565LE; bytes = 2; break;
        case WG_VIDEO_FORMAT_YUY2: *pixel = AV_PIX_FMT_YUYV422; bytes = 2; break;
        case WG_VIDEO_FORMAT_UYVY: *pixel = AV_PIX_FMT_UYVY422; bytes = 2; break;
        case WG_VIDEO_FORMAT_YVYU: *pixel = AV_PIX_FMT_YVYU422; bytes = 2; break;
        case WG_VIDEO_FORMAT_I420:
        case WG_VIDEO_FORMAT_YV12:
            *pixel = AV_PIX_FMT_YUV420P;
            strides[0] = luma;
            strides[1] = strides[2] = chroma;
            offsets[1] = luma * rows;
            offsets[2] = offsets[1] + chroma * ((height + 1) / 2);
            if (format == WG_VIDEO_FORMAT_YV12)
            {
                swap = offsets[1];
                offsets[1] = offsets[2];
                offsets[2] = swap;
            }
            return luma * rows + 2 * chroma * ((height + 1) / 2);
        case WG_VIDEO_FORMAT_NV12:
            *pixel = AV_PIX_FMT_NV12;
            strides[0] = strides[1] = luma;
            offsets[1] = luma * rows;
            return luma * rows + luma * ((height + 1) / 2);
        default:
            return 0;
    }
    strides[0] = (width * bytes + 3) & ~3;
    return strides[0] * height;
}

static BOOL parser_output_video(struct parser_stream *s, const AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    const struct wg_format *format = &s->desired;
    unsigned int width = format->u.video.width, height = abs(format->u.video.height), offsets[3], size;
    INT64 pts = parser_time(s, frame->best_effort_timestamp), duration = 0;
    AVRational rate = s->av_stream->avg_frame_rate;
    int strides[4] = {0}, source_strides[4], planes, i;
    const uint8_t *source[4];
    uint8_t *target[4] = {0};
    enum AVPixelFormat pixel;

    if (frame->duration > 0) duration = av_rescale_q(frame->duration, s->av_stream->time_base, media_time_base);
    else if (rate.num > 0 && rate.den > 0) duration = av_rescale(10000000, rate.den, rate.num);
    if (!desc || frame->width <= 0 || frame->height <= 0 || !parser_keep(s, pts, duration)) return FALSE;
    if (!width || !height)
    {
        width = frame->width;
        height = frame->height;
    }
    if (!(size = parser_video_layout(format->u.video.format, width, height, &pixel, offsets, strides))
            || !parser_reserve(s, size)) return FALSE;
    planes = av_pix_fmt_count_planes(frame->format);
    for (i = 0; i < 4; i++)
    {
        source[i] = frame->data[i];
        source_strides[i] = frame->linesize[i];
        if (format->u.video.height < 0 && i < planes)
        {
            int rows = (i == 1 || i == 2) ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
            source[i] += (ptrdiff_t)frame->linesize[i] * (rows - 1);
            source_strides[i] = -frame->linesize[i];
        }
    }
    for (i = 0; i < 3; i++) target[i] = s->data + offsets[i];
    s->scaler = sws_getCachedContext(s->scaler, frame->width, frame->height, frame->format,
                                     width, height, pixel, SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->scaler || sws_scale(s->scaler, source, source_strides, 0, frame->height, target, strides) <= 0)
        return FALSE;
    parser_emit(s, size, pts, duration, !(frame->flags & AV_FRAME_FLAG_KEY));
    return TRUE;
}

static enum AVSampleFormat parser_sample_format(wg_audio_format format)
{
    switch (format)
    {
        case WG_AUDIO_FORMAT_U8: return AV_SAMPLE_FMT_U8;
        case WG_AUDIO_FORMAT_S16LE: return AV_SAMPLE_FMT_S16;
        case WG_AUDIO_FORMAT_S24LE:
        case WG_AUDIO_FORMAT_S32LE: return AV_SAMPLE_FMT_S32;
        case WG_AUDIO_FORMAT_F32LE: return AV_SAMPLE_FMT_FLT;
        case WG_AUDIO_FORMAT_F64LE: return AV_SAMPLE_FMT_DBL;
        default: return AV_SAMPLE_FMT_NONE;
    }
}

static unsigned int parser_sample_size(wg_audio_format format)
{
    switch (format)
    {
        case WG_AUDIO_FORMAT_U8: return 1;
        case WG_AUDIO_FORMAT_S16LE: return 2;
        case WG_AUDIO_FORMAT_S24LE: return 3;
        case WG_AUDIO_FORMAT_S32LE:
        case WG_AUDIO_FORMAT_F32LE: return 4;
        case WG_AUDIO_FORMAT_F64LE: return 8;
        default: return 0;
    }
}

static BOOL parser_open_resampler(struct parser_stream *s, int input_format, int input_rate,
                                  const AVChannelLayout *input_layout)
{
    const struct wg_format *format = &s->desired;
    AVChannelLayout output = {0}, input = {0};
    int ret;

    if (format->u.audio.channel_mask && av_popcount(format->u.audio.channel_mask) == format->u.audio.channels)
        av_channel_layout_from_mask(&output, format->u.audio.channel_mask);
    else av_channel_layout_default(&output, format->u.audio.channels);
    if (input_layout->order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&input, input_layout->nb_channels);
    else if (av_channel_layout_copy(&input, input_layout) < 0)
    {
        av_channel_layout_uninit(&output);
        return FALSE;
    }
    ret = swr_alloc_set_opts2(&s->resampler, &output, parser_sample_format(format->u.audio.format),
                              format->u.audio.rate, &input, input_format, input_rate, 0, NULL);
    av_channel_layout_uninit(&output);
    av_channel_layout_uninit(&input);
    av_channel_layout_uninit(&s->resampler_layout);
    if (ret < 0 || swr_init(s->resampler) < 0 || av_channel_layout_copy(&s->resampler_layout, input_layout) < 0)
    {
        swr_free(&s->resampler);
        return FALSE;
    }
    s->resampler_rate = input_rate;
    s->resampler_format = input_format;
    return TRUE;
}

static BOOL parser_output_audio(struct parser_stream *s, const uint8_t **input, int samples, int input_format,
                                int input_rate, const AVChannelLayout *input_layout, INT64 pts)
{
    const struct wg_format *format = &s->desired;
    unsigned int channels = format->u.audio.channels, rate = format->u.audio.rate;
    unsigned int sample_size = parser_sample_size(format->u.audio.format), count, i;
    INT64 time = s->next_time, duration;
    UINT32 value;
    BYTE *data;
    int ret;

    if (!channels || !rate || !sample_size) return FALSE;
    if (input)
    {
        if (samples <= 0 || input_rate <= 0) return FALSE;
        if (s->resampler && (input_rate != s->resampler_rate || input_format != s->resampler_format
                || av_channel_layout_compare(input_layout, &s->resampler_layout)))
            swr_free(&s->resampler);
        if (!s->resampler && !parser_open_resampler(s, input_format, input_rate, input_layout)) return FALSE;
        if (pts != AV_NOPTS_VALUE)
            time = pts - av_rescale(swr_get_delay(s->resampler, input_rate), 10000000, input_rate);
    }
    else if (!s->resampler) return FALSE;
    if ((ret = swr_get_out_samples(s->resampler, input ? samples : 0)) <= 0) return FALSE;
    count = ret;
    if (!(data = parser_reserve(s, (size_t)count * channels * max(sample_size, 4)))) return FALSE;
    if ((ret = swr_convert(s->resampler, &data, count, input, input ? samples : 0)) <= 0) return FALSE;
    count = ret;
    if (format->u.audio.format == WG_AUDIO_FORMAT_S24LE)
    {
        for (i = 0; i < count * channels; i++)
        {
            memcpy(&value, data + i * 4, sizeof(value));
            data[i * 3] = value >> 8;
            data[i * 3 + 1] = value >> 16;
            data[i * 3 + 2] = value >> 24;
        }
    }
    duration = av_rescale(count, 10000000, rate);
    s->next_time = time == AV_NOPTS_VALUE ? time : time + duration;
    if (!parser_keep(s, time, duration)) return FALSE;
    parser_emit(s, count * channels * sample_size, time, duration, FALSE);
    return TRUE;
}

static BOOL parser_output_pcm(struct parser_stream *s, const AVPacket *packet)
{
    const AVCodecParameters *par = s->av_stream->codecpar;
    const uint8_t *input[1] = {packet->data};
    int block = par->ch_layout.nb_channels * av_get_bytes_per_sample(s->pcm_format);

    if (block <= 0 || packet->size < block) return FALSE;
    return parser_output_audio(s, input, packet->size / block, s->pcm_format, par->sample_rate,
                               &par->ch_layout, parser_time(s, packet->pts));
}

static BOOL parser_output_packet(struct parser_stream *s, const AVPacket *packet)
{
    INT64 pts = parser_time(s, packet->pts), duration = 0;

    if (packet->duration > 0) duration = av_rescale_q(packet->duration, s->av_stream->time_base, media_time_base);
    if (!parser_keep(s, pts, duration) || !parser_reserve(s, packet->size)) return FALSE;
    memcpy(s->data, packet->data, packet->size);
    parser_emit(s, packet->size, pts, duration, !(packet->flags & AV_PKT_FLAG_KEY));
    return TRUE;
}

static BOOL parser_fill(struct media_parser *p, struct parser_stream *s)
{
    AVPacket *packet;
    BOOL ready;
    int ret;

    for (;;)
    {
        pthread_mutex_lock(&p->mutex);
        ready = s->has_buffer;
        pthread_mutex_unlock(&p->mutex);
        if (!s->enabled) return FALSE;
        if (ready) return TRUE;
        if (s->eos) return FALSE;
        if (!s->codec)
        {
            if ((packet = parser_queue_pop(s)))
            {
                if (s->passthrough) parser_output_packet(s, packet);
                else parser_output_pcm(s, packet);
                av_packet_free(&packet);
            }
            else if (parser_read_packet(p)) continue;
            else if (!s->passthrough && !s->flushed)
            {
                s->flushed = TRUE;
                parser_output_audio(s, NULL, 0, 0, 0, NULL, AV_NOPTS_VALUE);
            }
            else s->eos = TRUE;
            continue;
        }
        if (!(ret = avcodec_receive_frame(s->codec, s->frame)))
        {
            s->errors = 0;
            if (s->video) parser_output_video(s, s->frame);
            else parser_output_audio(s, (const uint8_t **)s->frame->extended_data, s->frame->nb_samples,
                                     s->frame->format, s->frame->sample_rate, &s->frame->ch_layout,
                                     parser_time(s, s->frame->best_effort_timestamp));
            av_frame_unref(s->frame);
        }
        else if (ret == AVERROR(EAGAIN) && !s->drain_sent)
        {
            if ((packet = parser_queue_pop(s)))
            {
                avcodec_send_packet(s->codec, packet);
                av_packet_free(&packet);
            }
            else if (!parser_read_packet(p))
            {
                avcodec_send_packet(s->codec, NULL);
                s->drain_sent = TRUE;
            }
        }
        else if (ret == AVERROR_EOF && !s->video && !s->flushed)
        {
            s->flushed = TRUE;
            parser_output_audio(s, NULL, 0, 0, 0, NULL, AV_NOPTS_VALUE);
        }
        else if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN) || ++s->errors >= 64) s->eos = TRUE;
    }
}

static void parser_flush_stream(struct parser_stream *s)
{
    parser_queue_clear(s);
    if (s->codec) avcodec_flush_buffers(s->codec);
    if (s->frame) av_frame_unref(s->frame);
    swr_free(&s->resampler);
    s->eos = s->drain_sent = s->flushed = FALSE;
    s->errors = 0;
    s->discontinuity = TRUE;
    s->next_time = AV_NOPTS_VALUE;
    pthread_mutex_lock(&s->parser->mutex);
    s->has_buffer = s->returned = FALSE;
    pthread_mutex_unlock(&s->parser->mutex);
}

static void parser_video_size(const AVStream *st, struct wg_format *format)
{
    format->u.video.width = st->codecpar->width;
    format->u.video.height = st->codecpar->height;
    format->u.video.fps_n = 0;
    format->u.video.fps_d = 1;
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
    {
        format->u.video.fps_n = st->avg_frame_rate.num;
        format->u.video.fps_d = st->avg_frame_rate.den;
    }
}

static UINT32 parser_channel_mask(const AVChannelLayout *layout)
{
    AVChannelLayout fallback = {0};
    UINT64 mask = 0;

    if (layout->order == AV_CHANNEL_ORDER_NATIVE && av_popcount64(layout->u.mask) == layout->nb_channels)
        mask = layout->u.mask;
    else if (layout->nb_channels > 0)
    {
        av_channel_layout_default(&fallback, layout->nb_channels);
        if (fallback.order == AV_CHANNEL_ORDER_NATIVE) mask = fallback.u.mask;
        av_channel_layout_uninit(&fallback);
    }
    return mask >> 32 ? 0 : mask;
}

static void parser_native_format(const AVStream *st, struct wg_format *format)
{
    const AVCodecParameters *par = st->codecpar;

    memset(format, 0, sizeof(*format));
    if (par->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        format->major_type = WG_MAJOR_TYPE_VIDEO;
        format->u.video.format = WG_VIDEO_FORMAT_I420;
        parser_video_size(st, format);
    }
    else
    {
        format->major_type = WG_MAJOR_TYPE_AUDIO;
        format->u.audio.format = WG_AUDIO_FORMAT_S16LE;
        format->u.audio.channels = par->ch_layout.nb_channels;
        format->u.audio.channel_mask = parser_channel_mask(&par->ch_layout);
        format->u.audio.rate = par->sample_rate;
    }
}

static BOOL parser_wma_format(const AVCodecParameters *par, UINT32 version, struct wg_format *format)
{
    if (par->extradata_size <= 0 || par->extradata_size > sizeof(format->u.audio.codec_data)) return FALSE;
    format->major_type = WG_MAJOR_TYPE_AUDIO_WMA;
    format->u.audio.version = version;
    format->u.audio.bitrate = par->bit_rate;
    format->u.audio.rate = par->sample_rate;
    format->u.audio.depth = par->bits_per_coded_sample > 0 ? par->bits_per_coded_sample : 16;
    format->u.audio.channels = par->ch_layout.nb_channels;
    format->u.audio.block_align = par->block_align;
    format->u.audio.codec_data_len = par->extradata_size;
    memcpy(format->u.audio.codec_data, par->extradata, par->extradata_size);
    return TRUE;
}

static BOOL parser_wmv_format(const AVStream *st, wg_video_format video_format, struct wg_format *format)
{
    const AVCodecParameters *par = st->codecpar;

    format->major_type = WG_MAJOR_TYPE_VIDEO_WMV;
    format->u.video.format = video_format;
    parser_video_size(st, format);
    if (par->extradata_size > 0 && par->extradata_size <= sizeof(format->u.video.codec_data))
    {
        format->u.video.codec_data_len = par->extradata_size;
        memcpy(format->u.video.codec_data, par->extradata, par->extradata_size);
    }
    return TRUE;
}

static BOOL parser_codec_format(const AVStream *st, struct wg_format *format)
{
    const AVCodecParameters *par = st->codecpar;
    unsigned int header = sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX);
    unsigned int size = max(par->extradata_size, 0);

    memset(format, 0, sizeof(*format));
    switch (par->codec_id)
    {
        case AV_CODEC_ID_MP1:
        case AV_CODEC_ID_MP2:
        case AV_CODEC_ID_MP3:
            format->major_type = WG_MAJOR_TYPE_AUDIO_MPEG1;
            format->u.audio.layer = par->codec_id == AV_CODEC_ID_MP1 ? 1 : par->codec_id == AV_CODEC_ID_MP2 ? 2 : 3;
            format->u.audio.channels = par->ch_layout.nb_channels;
            format->u.audio.rate = par->sample_rate;
            return TRUE;
        case AV_CODEC_ID_AAC:
            if (!size || size + header > sizeof(format->u.audio.codec_data)) return FALSE;
            format->major_type = WG_MAJOR_TYPE_AUDIO_MPEG4;
            format->u.audio.channels = par->ch_layout.nb_channels;
            format->u.audio.rate = par->sample_rate;
            format->u.audio.codec_data_len = size + header;
            memcpy(format->u.audio.codec_data + header, par->extradata, size);
            return TRUE;
        case AV_CODEC_ID_WMAV1: return parser_wma_format(par, 1, format);
        case AV_CODEC_ID_WMAV2: return parser_wma_format(par, 2, format);
        case AV_CODEC_ID_WMAPRO: return parser_wma_format(par, 3, format);
        case AV_CODEC_ID_WMALOSSLESS: return parser_wma_format(par, 4, format);
        case AV_CODEC_ID_WMV1: return parser_wmv_format(st, WG_VIDEO_FORMAT_WMV1, format);
        case AV_CODEC_ID_WMV2: return parser_wmv_format(st, WG_VIDEO_FORMAT_WMV2, format);
        case AV_CODEC_ID_WMV3: return parser_wmv_format(st, WG_VIDEO_FORMAT_WMV3, format);
        case AV_CODEC_ID_VC1:
            return parser_wmv_format(st, par->codec_tag == MKTAG('W','M','V','A')
                                     ? WG_VIDEO_FORMAT_WMVA : WG_VIDEO_FORMAT_WVC1, format);
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
            format->major_type = WG_MAJOR_TYPE_VIDEO_MPEG1;
            parser_video_size(st, format);
            if ((format->u.video.codec_data_len = min(size, sizeof(format->u.video.codec_data))))
                memcpy(format->u.video.codec_data, par->extradata, format->u.video.codec_data_len);
            return TRUE;
        case AV_CODEC_ID_CINEPAK:
            format->major_type = WG_MAJOR_TYPE_VIDEO_CINEPAK;
            parser_video_size(st, format);
            return TRUE;
        case AV_CODEC_ID_INDEO5:
            format->major_type = WG_MAJOR_TYPE_VIDEO_INDEO;
            format->u.video.version = 5;
            parser_video_size(st, format);
            return TRUE;
        case AV_CODEC_ID_H264:
            if (size && par->extradata[0] == 1) return FALSE;
            format->major_type = WG_MAJOR_TYPE_VIDEO_H264;
            parser_video_size(st, format);
            if (par->profile == 77 || par->profile == 100 || par->profile == 244)
                format->u.video.profile = par->profile;
            format->u.video.level = par->level > 0 ? par->level : 10;
            if (size && size <= sizeof(format->u.video.codec_data))
            {
                format->u.video.codec_data_len = size;
                memcpy(format->u.video.codec_data, par->extradata, size);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static wg_audio_format parser_pcm_format(enum AVCodecID id)
{
    switch (id)
    {
        case AV_CODEC_ID_PCM_U8: return WG_AUDIO_FORMAT_U8;
        case AV_CODEC_ID_PCM_S16LE: return WG_AUDIO_FORMAT_S16LE;
        case AV_CODEC_ID_PCM_S24LE: return WG_AUDIO_FORMAT_S24LE;
        case AV_CODEC_ID_PCM_S32LE: return WG_AUDIO_FORMAT_S32LE;
        case AV_CODEC_ID_PCM_F32LE: return WG_AUDIO_FORMAT_F32LE;
        case AV_CODEC_ID_PCM_F64LE: return WG_AUDIO_FORMAT_F64LE;
        default: return WG_AUDIO_FORMAT_UNKNOWN;
    }
}

static UINT64 parser_stream_duration(const AVFormatContext *context, const AVStream *st)
{
    if (context->duration != AV_NOPTS_VALUE && context->duration > 0)
        return av_rescale_q(context->duration, AV_TIME_BASE_Q, media_time_base);
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return av_rescale_q(st->duration, st->time_base, media_time_base);
    return 0;
}

static void parser_read_tags(struct media_parser *p, struct parser_stream *s)
{
    const AVDictionaryEntry *entry;

    if ((entry = av_dict_get(s->av_stream->metadata, "language", NULL, 0)) && entry->value[0]
            && strcmp(entry->value, "und"))
        s->tags[WG_PARSER_TAG_LANGUAGE] = strdup(entry->value);
    if (strstr(p->context->iformat->name, "mov")
            && (entry = av_dict_get(s->av_stream->metadata, "title", NULL, 0)) && entry->value[0])
        s->tags[WG_PARSER_TAG_NAME] = strdup(entry->value);
}

static BOOL parser_init_stream(struct media_parser *p, struct parser_stream *s, AVStream *st)
{
    const AVCodecParameters *par = st->codecpar;
    wg_audio_format pcm = parser_pcm_format(par->codec_id);
    const AVCodec *codec;

    s->parser = p;
    s->av_stream = st;
    s->video = par->codec_type == AVMEDIA_TYPE_VIDEO;
    s->pcm_format = AV_SAMPLE_FMT_NONE;
    s->drop_before = s->next_time = AV_NOPTS_VALUE;
    s->has_codec_format = parser_codec_format(st, &s->codec_format);
    if (p->output_compressed && (s->has_codec_format || pcm != WG_AUDIO_FORMAT_UNKNOWN))
    {
        s->passthrough = TRUE;
        if (s->has_codec_format) s->current_format = s->codec_format;
        else
        {
            parser_native_format(st, &s->current_format);
            s->current_format.u.audio.format = pcm;
        }
    }
    else
    {
        parser_native_format(st, &s->current_format);
        if (pcm != WG_AUDIO_FORMAT_UNKNOWN && pcm != WG_AUDIO_FORMAT_S24LE)
            s->pcm_format = parser_sample_format(pcm);
        else
        {
            if (!(codec = avcodec_find_decoder(par->codec_id)) || !(s->codec = avcodec_alloc_context3(codec))
                    || avcodec_parameters_to_context(s->codec, par) < 0) return FALSE;
            s->codec->pkt_timebase = st->time_base;
            s->codec->thread_count = s->video ? 2 : 1;
            if (s->video) s->codec->thread_type = FF_THREAD_SLICE;
            if (avcodec_open2(s->codec, codec, NULL) < 0) return FALSE;
        }
        if (!(s->frame = av_frame_alloc())) return FALSE;
    }
    s->desired = s->current_format;
    s->enabled = TRUE;
    s->discontinuity = TRUE;
    s->duration = parser_stream_duration(p->context, st);
    parser_read_tags(p, s);
    return TRUE;
}

static void parser_free_stream(struct parser_stream *s)
{
    unsigned int i;

    parser_queue_clear(s);
    avcodec_free_context(&s->codec);
    av_frame_free(&s->frame);
    sws_freeContext(s->scaler);
    swr_free(&s->resampler);
    av_channel_layout_uninit(&s->resampler_layout);
    av_free(s->data);
    for (i = 0; i < WG_PARSER_TAG_COUNT; i++) free(s->tags[i]);
    free(s);
}

static BOOL parser_create_streams(struct media_parser *p)
{
    unsigned int i, count = p->context->nb_streams;
    INT64 start;

    if (!count || !(p->streams = calloc(count, sizeof(*p->streams))) || !(p->map = calloc(count, sizeof(*p->map))))
        return FALSE;
    p->map_count = count;
    p->base_pts = AV_NOPTS_VALUE;
    for (i = 0; i < count; i++)
    {
        AVStream *st = p->context->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        struct parser_stream *s;

        st->discard = AVDISCARD_ALL;
        if ((type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO)
                || (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || !(s = calloc(1, sizeof(*s))))
            continue;
        if (!parser_init_stream(p, s, st))
        {
            parser_free_stream(s);
            continue;
        }
        st->discard = AVDISCARD_DEFAULT;
        s->number = p->stream_count;
        p->streams[p->stream_count++] = s;
        p->map[i] = s;
        if (st->start_time == AV_NOPTS_VALUE) continue;
        start = av_rescale_q(st->start_time, st->time_base, media_time_base);
        if (p->base_pts == AV_NOPTS_VALUE || start < p->base_pts) p->base_pts = start;
    }
    return p->stream_count > 0;
}

static void parser_close(struct media_parser *p)
{
    unsigned int i;

    parser_stop_io(p);
    for (i = 0; i < p->stream_count; i++) parser_free_stream(p->streams[i]);
    free(p->streams);
    free(p->map);
    p->streams = p->map = NULL;
    p->stream_count = p->map_count = 0;
    av_packet_free(&p->packet);
    avformat_close_input(&p->context);
    if (p->io) av_freep(&p->io->buffer);
    avio_context_free(&p->io);
}

static NTSTATUS parser_create(void *args)
{
    struct wg_parser_create_params *params = args;
    struct media_parser *p;

    if (!(p = calloc(1, sizeof(*p)))) return E_OUTOFMEMORY;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_mutex_init(&p->demux, NULL);
    pthread_cond_init(&p->read_cond, NULL);
    pthread_cond_init(&p->done_cond, NULL);
    p->output_compressed = !!(params->flags & WG_PARSER_CREATE_FLAG_OUTPUT_COMPRESSED);
    p->pts_rebased = !!(params->flags & WG_PARSER_CREATE_FLAG_PTS_REBASED);
    p->base_pts = AV_NOPTS_VALUE;
    p->stop = INT64_MAX;
    params->parser = (UINT_PTR)p;
    return S_OK;
}

static NTSTATUS parser_destroy(void *args)
{
    struct media_parser *p = parser_from_handle(*(wg_parser_t *)args);

    pthread_mutex_lock(&p->demux);
    parser_close(p);
    pthread_mutex_unlock(&p->demux);
    pthread_cond_destroy(&p->read_cond);
    pthread_cond_destroy(&p->done_cond);
    pthread_mutex_destroy(&p->demux);
    pthread_mutex_destroy(&p->mutex);
    free(p);
    return S_OK;
}

static NTSTATUS parser_connect(void *args)
{
    const struct wg_parser_connect_params *params = args;
    struct media_parser *p = parser_from_handle(params->parser);
    BYTE *buffer = NULL;

    pthread_mutex_lock(&p->demux);
    pthread_mutex_lock(&p->mutex);
    p->file_size = params->file_size;
    p->position = 0;
    p->connected = TRUE;
    pthread_mutex_unlock(&p->mutex);
    if (!(p->context = avformat_alloc_context()) || !(buffer = av_malloc(PARSER_IO_SIZE))
            || !(p->io = avio_alloc_context(buffer, PARSER_IO_SIZE, 0, p, parser_io_read, NULL, parser_io_seek)))
    {
        av_free(buffer);
        goto failed;
    }
    p->context->pb = p->io;
    if (avformat_open_input(&p->context, NULL, NULL, NULL) < 0) goto failed;
    avformat_find_stream_info(p->context, NULL);
    if (!(p->packet = av_packet_alloc()) || !parser_create_streams(p)) goto failed;
    p->demux_eof = FALSE;
    p->fresh = TRUE;
    p->stop = INT64_MAX;
    pthread_mutex_unlock(&p->demux);
    return S_OK;
failed:
    parser_close(p);
    pthread_mutex_unlock(&p->demux);
    return E_FAIL;
}

static NTSTATUS parser_disconnect(void *args)
{
    struct media_parser *p = parser_from_handle(*(wg_parser_t *)args);

    parser_stop_io(p);
    pthread_mutex_lock(&p->demux);
    parser_close(p);
    pthread_mutex_unlock(&p->demux);
    return S_OK;
}

static NTSTATUS parser_get_next_read_offset(void *args)
{
    struct wg_parser_get_next_read_offset_params *params = args;
    struct media_parser *p = parser_from_handle(params->parser);

    pthread_mutex_lock(&p->mutex);
    while (p->connected && !p->request.size)
        pthread_cond_wait(&p->read_cond, &p->mutex);
    if (!p->connected)
    {
        pthread_mutex_unlock(&p->mutex);
        return VFW_E_WRONG_STATE;
    }
    params->offset = p->request.offset;
    params->size = p->request.size;
    pthread_mutex_unlock(&p->mutex);
    return S_OK;
}

static NTSTATUS parser_push_data(void *args)
{
    const struct wg_parser_push_data_params *params = args;
    struct media_parser *p = parser_from_handle(params->parser);
    UINT32 size;

    pthread_mutex_lock(&p->mutex);
    if (p->request.size && !p->request.done)
    {
        if (!params->data) p->request.result = AVERROR(EIO);
        else if (!params->size) p->request.result = AVERROR_EOF;
        else
        {
            size = min(params->size, p->request.size);
            memcpy(p->request.data, params->data, size);
            p->request.result = size;
        }
        p->request.done = TRUE;
        p->request.size = 0;
        pthread_cond_broadcast(&p->done_cond);
    }
    pthread_mutex_unlock(&p->mutex);
    return S_OK;
}

static NTSTATUS parser_get_stream_count(void *args)
{
    struct wg_parser_get_stream_count_params *params = args;

    params->count = parser_from_handle(params->parser)->stream_count;
    return S_OK;
}

static NTSTATUS parser_get_stream(void *args)
{
    struct wg_parser_get_stream_params *params = args;
    struct media_parser *p = parser_from_handle(params->parser);

    params->stream = params->index < p->stream_count ? (UINT_PTR)p->streams[params->index] : 0;
    return S_OK;
}

static NTSTATUS parser_get_current_format(void *args)
{
    const struct wg_parser_stream_get_current_format_params *params = args;

    *params->format = parser_stream_from_handle(params->stream)->current_format;
    return S_OK;
}

static NTSTATUS parser_get_codec_format(void *args)
{
    const struct wg_parser_stream_get_codec_format_params *params = args;
    struct parser_stream *s = parser_stream_from_handle(params->stream);

    *params->format = s->has_codec_format ? s->codec_format : s->current_format;
    return S_OK;
}

static BOOL parser_format_supported(const struct parser_stream *s, const struct wg_format *format)
{
    enum AVPixelFormat pixel;
    unsigned int offsets[3];
    int strides[3];

    if (s->video)
        return format->major_type == WG_MAJOR_TYPE_VIDEO && format->u.video.width >= 0
                && format->u.video.width <= 16384 && format->u.video.height >= -16384
                && format->u.video.height <= 16384
                && parser_video_layout(format->u.video.format, 2, 2, &pixel, offsets, strides);
    return format->major_type == WG_MAJOR_TYPE_AUDIO && parser_sample_size(format->u.audio.format)
            && format->u.audio.channels && format->u.audio.channels <= 32
            && format->u.audio.rate && format->u.audio.rate <= 768000;
}

static NTSTATUS parser_enable(void *args)
{
    const struct wg_parser_stream_enable_params *params = args;
    struct parser_stream *s = parser_stream_from_handle(params->stream);
    struct media_parser *p = s->parser;

    pthread_mutex_lock(&p->demux);
    if (!s->passthrough && parser_format_supported(s, params->format))
    {
        s->desired = *params->format;
        swr_free(&s->resampler);
    }
    s->enabled = TRUE;
    pthread_mutex_unlock(&p->demux);
    return S_OK;
}

static NTSTATUS parser_disable(void *args)
{
    struct parser_stream *s = parser_stream_from_handle(*(wg_parser_stream_t *)args);
    struct media_parser *p = s->parser;

    pthread_mutex_lock(&p->demux);
    s->enabled = FALSE;
    parser_queue_clear(s);
    pthread_mutex_unlock(&p->demux);
    return S_OK;
}

static UINT64 parser_sort_time(const struct parser_stream *s)
{
    return s->buffer.has_pts ? s->buffer.pts : UINT64_MAX;
}

static NTSTATUS parser_get_buffer(void *args)
{
    const struct wg_parser_stream_get_buffer_params *params = args;
    struct media_parser *p = parser_from_handle(params->parser);
    struct parser_stream *stream = parser_stream_from_handle(params->stream), *best = NULL;
    unsigned int i;

    pthread_mutex_lock(&p->demux);
    if (p->context && stream)
    {
        if (parser_fill(p, stream)) best = stream;
    }
    else if (p->context)
    {
        for (i = 0; i < p->stream_count; i++)
        {
            struct parser_stream *s = p->streams[i];
            if (parser_fill(p, s) && (!best || parser_sort_time(s) < parser_sort_time(best))) best = s;
        }
    }
    if (best)
    {
        pthread_mutex_lock(&p->mutex);
        *params->buffer = best->buffer;
        best->returned = TRUE;
        pthread_mutex_unlock(&p->mutex);
    }
    pthread_mutex_unlock(&p->demux);
    return best ? S_OK : S_FALSE;
}

static NTSTATUS parser_copy_buffer(void *args)
{
    const struct wg_parser_stream_copy_buffer_params *params = args;
    struct parser_stream *s = parser_stream_from_handle(params->stream);
    struct media_parser *p = s->parser;
    NTSTATUS status = VFW_E_WRONG_STATE;

    pthread_mutex_lock(&p->mutex);
    if (s->has_buffer && s->returned)
    {
        status = STATUS_INVALID_PARAMETER;
        if (params->offset < s->buffer.size && params->size <= s->buffer.size - params->offset)
        {
            memcpy(params->data, s->data + params->offset, params->size);
            status = S_OK;
        }
    }
    pthread_mutex_unlock(&p->mutex);
    return status;
}

static NTSTATUS parser_release_buffer(void *args)
{
    struct parser_stream *s = parser_stream_from_handle(*(wg_parser_stream_t *)args);
    struct media_parser *p = s->parser;

    pthread_mutex_lock(&p->mutex);
    s->has_buffer = s->returned = FALSE;
    pthread_mutex_unlock(&p->mutex);
    return S_OK;
}

static NTSTATUS parser_get_duration(void *args)
{
    struct wg_parser_stream_get_duration_params *params = args;

    params->duration = parser_stream_from_handle(params->stream)->duration;
    return S_OK;
}

static NTSTATUS parser_get_tag(void *args)
{
    struct wg_parser_stream_get_tag_params *params = args;
    struct parser_stream *s = parser_stream_from_handle(params->stream);
    UINT32 size;

    if (params->tag >= WG_PARSER_TAG_COUNT) return STATUS_INVALID_PARAMETER;
    if (!s->tags[params->tag]) return STATUS_NOT_FOUND;
    if ((size = strlen(s->tags[params->tag]) + 1) > *params->size)
    {
        *params->size = size;
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(params->buffer, s->tags[params->tag], size);
    return STATUS_SUCCESS;
}

static NTSTATUS parser_seek(void *args)
{
    const struct wg_parser_stream_seek_params *params = args;
    struct parser_stream *stream = parser_stream_from_handle(params->stream);
    struct media_parser *p = stream->parser;
    BOOL key = !!(params->start_flags & AM_SEEKING_SeekToKeyFrame);
    INT64 start = params->start_pos, position;
    unsigned int i;

    pthread_mutex_lock(&p->demux);
    if (p->context && (params->stop_flags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning)
    {
        if (params->stop_pos == stream->duration || params->stop_pos > INT64_MAX) p->stop = INT64_MAX;
        else p->stop = params->stop_pos;
    }
    if (p->context && (params->start_flags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning)
    {
        if (!p->fresh || start)
        {
            position = start;
            if (p->pts_rebased && p->base_pts != AV_NOPTS_VALUE) position += p->base_pts;
            position = av_rescale_q(position, media_time_base, AV_TIME_BASE_Q);
            p->io->error = 0;
            if (avformat_seek_file(p->context, -1, INT64_MIN, position, position, 0) < 0)
                av_seek_frame(p->context, -1, position, AVSEEK_FLAG_BACKWARD);
            p->fresh = FALSE;
        }
        p->demux_eof = FALSE;
        for (i = 0; i < p->stream_count; i++)
        {
            struct parser_stream *s = p->streams[i];
            parser_flush_stream(s);
            s->drop_before = s->passthrough || key ? AV_NOPTS_VALUE : start;
        }
    }
    pthread_mutex_unlock(&p->demux);
    return S_OK;
}

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

static NTSTATUS wow64_parser_connect(void *args)
{
    struct { wg_parser_t parser; UINT32 uri; UINT64 file_size; } *p = args;
    struct wg_parser_connect_params params = {p->parser, ULongToPtr(p->uri), p->file_size};
    return parser_connect(&params);
}

static NTSTATUS wow64_parser_push_data(void *args)
{
    struct { wg_parser_t parser; UINT32 data; UINT32 size; } *p = args;
    struct wg_parser_push_data_params params = {p->parser, ULongToPtr(p->data), p->size};
    return parser_push_data(&params);
}

static NTSTATUS wow64_parser_get_current_format(void *args)
{
    struct { wg_parser_stream_t stream; UINT32 format; } *p = args;
    struct wg_parser_stream_get_current_format_params params = {p->stream, ULongToPtr(p->format)};
    return parser_get_current_format(&params);
}

static NTSTATUS wow64_parser_get_codec_format(void *args)
{
    struct { wg_parser_stream_t stream; UINT32 format; } *p = args;
    struct wg_parser_stream_get_codec_format_params params = {p->stream, ULongToPtr(p->format)};
    return parser_get_codec_format(&params);
}

static NTSTATUS wow64_parser_enable(void *args)
{
    struct { wg_parser_stream_t stream; UINT32 format; } *p = args;
    struct wg_parser_stream_enable_params params = {p->stream, ULongToPtr(p->format)};
    return parser_enable(&params);
}

static NTSTATUS wow64_parser_get_buffer(void *args)
{
    struct { wg_parser_t parser; wg_parser_stream_t stream; UINT32 buffer; } *p = args;
    struct wg_parser_stream_get_buffer_params params = {p->parser, p->stream, ULongToPtr(p->buffer)};
    return parser_get_buffer(&params);
}

static NTSTATUS wow64_parser_copy_buffer(void *args)
{
    struct { wg_parser_stream_t stream; UINT32 data; UINT32 offset; UINT32 size; } *p = args;
    struct wg_parser_stream_copy_buffer_params params = {p->stream, ULongToPtr(p->data), p->offset, p->size};
    return parser_copy_buffer(&params);
}

static NTSTATUS wow64_parser_get_tag(void *args)
{
    struct { wg_parser_stream_t stream; wg_parser_tag tag; UINT32 buffer; UINT32 size; } *p = args;
    struct wg_parser_stream_get_tag_params params = {p->stream, p->tag, ULongToPtr(p->buffer), ULongToPtr(p->size)};
    return parser_get_tag(&params);
}

#define PARSER_TABLE(connect, push_data, current_format, codec_format, enable, get_buffer, copy_buffer, get_tag) \
    [unix_wg_parser_create] = parser_create, \
    [unix_wg_parser_destroy] = parser_destroy, \
    [unix_wg_parser_connect] = connect, \
    [unix_wg_parser_disconnect] = parser_disconnect, \
    [unix_wg_parser_get_next_read_offset] = parser_get_next_read_offset, \
    [unix_wg_parser_push_data] = push_data, \
    [unix_wg_parser_get_stream_count] = parser_get_stream_count, \
    [unix_wg_parser_get_stream] = parser_get_stream, \
    [unix_wg_parser_stream_get_current_format] = current_format, \
    [unix_wg_parser_stream_get_codec_format] = codec_format, \
    [unix_wg_parser_stream_enable] = enable, \
    [unix_wg_parser_stream_disable] = parser_disable, \
    [unix_wg_parser_stream_get_buffer] = get_buffer, \
    [unix_wg_parser_stream_copy_buffer] = copy_buffer, \
    [unix_wg_parser_stream_release_buffer] = parser_release_buffer, \
    [unix_wg_parser_stream_notify_qos] = media_init, \
    [unix_wg_parser_stream_get_duration] = parser_get_duration, \
    [unix_wg_parser_stream_get_tag] = get_tag, \
    [unix_wg_parser_stream_seek] = parser_seek

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
{
    MEDIA_TABLE(media_create, media_get_type, media_set_type, media_push, media_read),
    PARSER_TABLE(parser_connect, parser_push_data, parser_get_current_format, parser_get_codec_format,
                 parser_enable, parser_get_buffer, parser_copy_buffer, parser_get_tag)
};
const unixlib_entry_t wine_nx_media_wow64_unix_funcs[] =
{
    MEDIA_TABLE(wow64_create, wow64_get_type, wow64_set_type, wow64_push, wow64_read),
    PARSER_TABLE(wow64_parser_connect, wow64_parser_push_data, wow64_parser_get_current_format,
                 wow64_parser_get_codec_format, wow64_parser_enable, wow64_parser_get_buffer,
                 wow64_parser_copy_buffer, wow64_parser_get_tag)
};
const unsigned int wine_nx_media_unix_count = unix_wg_funcs_count;
