#include <assert.h>
#include <stdio.h>
#include "initguid.h"
#include "../source/media_unix.c"
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/adler32.h>

static void test_audio_formats(void)
{
    HEAACWAVEINFO input = {
        .wfx = {WAVE_FORMAT_MPEG_HEAAC, 6, 48000, 1152000, 24, 32,
                sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX)}
    };
    WAVEFORMATEXTENSIBLE output = {
        .Format = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 0, 0, 32,
                   sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}
    };
    struct wg_transform_create_params p = {
        .input_type = {MFMediaType_Audio, sizeof(input), {.audio = &input.wfx}},
        .output_type = {MFMediaType_Audio, sizeof(output), {.audio = &output.Format}}
    };

    for (unsigned int i = 0; i < 2; i++)
    {
        unsigned int align = output.Format.nChannels * output.Format.wBitsPerSample / 8;
        struct media_decoder *d;
        assert(!media_create(&p) && p.transform);
        d = decoder(p.transform);
        assert(d->audio_type.Format.nBlockAlign == align);
        assert(d->audio_type.Format.nAvgBytesPerSec == align * output.Format.nSamplesPerSec);
        assert(!media_destroy(&p.transform));
        output.Format.nBlockAlign = align + 1;
        assert(media_create(&p) == STATUS_INVALID_PARAMETER && !p.transform);
        output.Format.nBlockAlign = align;
        assert(!media_create(&p) && p.transform);
        assert(!media_destroy(&p.transform));
        output.Format.nBlockAlign = 0;
        output.Format.wFormatTag = WAVE_FORMAT_PCM;
        output.Format.wBitsPerSample = 16;
    }
}

static unsigned int output(wg_transform_t handle, unsigned int capacity, FILE *file, unsigned long *checksum)
{
    BYTE *buffer = malloc(capacity + 32);
    struct wg_sample sample = {.data = (UINT_PTR)buffer, .max_size = capacity};
    struct wg_transform_read_data_params read = {handle, &sample};
    unsigned int frames = 0;
    assert(buffer);
    memset(buffer + capacity, 0xa5, 32);
    for (;;)
    {
        assert(!media_read(&read));
        if (read.result == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (read.result == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            MFVIDEOFORMAT format;
            struct wg_transform_get_output_type_params get = {handle};
            assert(media_get_type(&get) == STATUS_BUFFER_TOO_SMALL);
            assert(get.media_type.format_size == sizeof(format));
            get.media_type.u.video = &format;
            assert(!media_get_type(&get));
            struct wg_transform_set_output_type_params set = {handle, get.media_type};
            assert(!media_set_type(&set));
            continue;
        }
        if (read.result != S_OK) fprintf(stderr, "read failed: %08x\n", (unsigned int)read.result);
        assert(read.result == S_OK && sample.size <= capacity);
        for (unsigned int i = 0; i < 32; i++) assert(buffer[capacity + i] == 0xa5);
        if (file) assert(fwrite(buffer, 1, sample.size, file) == sample.size);
        *checksum = av_adler32_update(*checksum, buffer, sample.size);
        frames++;
    }
    free(buffer);
    return frames;
}

static wg_transform_t create(AVCodecParameters *par, int video, unsigned int rate)
{
    BYTE input[4096] = {0};
    MFVIDEOFORMAT video_out = {.dwSize = sizeof(video_out), .guidFormat = MFVideoFormat_NV12,
                              .videoInfo = {.dwWidth = 1280, .dwHeight = 720}};
    WAVEFORMATEX audio_out = {WAVE_FORMAT_IEEE_FLOAT, 2, rate, rate * 8, 8, 32, 0};
    struct wg_transform_create_params p = {.attrs = {.input_queue_length = 1}};
    assert(par->extradata_size < 3000);
    if (video)
    {
        MFVIDEOFORMAT *format = (void *)input;
        format->dwSize = sizeof(*format) + par->extradata_size;
        format->guidFormat = MFVideoFormat_H264;
        format->videoInfo.dwWidth = par->width;
        format->videoInfo.dwHeight = par->height;
        memcpy(format + 1, par->extradata, par->extradata_size);
        p.input_type = (struct wg_media_type){MFMediaType_Video, format->dwSize, {.video = format}};
        p.output_type = (struct wg_media_type){MFMediaType_Video, sizeof(video_out), {.video = &video_out}};
    }
    else
    {
        HEAACWAVEINFO *format = (void *)input;
        format->wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
        format->wfx.nChannels = par->ch_layout.nb_channels;
        format->wfx.nSamplesPerSec = par->sample_rate;
        format->wfx.cbSize = sizeof(*format) - sizeof(WAVEFORMATEX) + par->extradata_size;
        memcpy(format + 1, par->extradata, par->extradata_size);
        p.input_type = (struct wg_media_type){MFMediaType_Audio, sizeof(*format) + par->extradata_size, {.audio = &format->wfx}};
        p.output_type = (struct wg_media_type){MFMediaType_Audio, sizeof(audio_out), {.audio = &audio_out}};
    }
    assert(!media_create(&p) && p.transform);
    return p.transform;
}

int main(int argc, char **argv)
{
    test_audio_formats();
    if (argc == 1) return 0;
    AVFormatContext *format = NULL;
    AVPacket *packet = av_packet_alloc(), *filtered = av_packet_alloc();
    AVBSFContext *filter = NULL;
    wg_transform_t handles[2] = {0};
    unsigned long checksum[2] = {1, 1}, first[2] = {0};
    unsigned int count[2] = {0};
    FILE *files[2] = {0};
    BOOL video[2] = {0};
    unsigned int rate = argc == 5 ? atoi(argv[4]) : 44100;
    assert(argc == 4 || argc == 5);
    assert(!avformat_open_input(&format, argv[1], NULL, NULL));
    assert(!avformat_find_stream_info(format, NULL));
    assert(format->nb_streams <= 2);
    for (unsigned int i = 0; i < format->nb_streams; i++)
    {
        AVCodecParameters *par = format->streams[i]->codecpar;
        video[i] = par->codec_type == AVMEDIA_TYPE_VIDEO;
        if (video[i])
        {
            assert(!filter && !av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &filter));
            assert(!avcodec_parameters_copy(filter->par_in, par));
            filter->time_base_in = format->streams[i]->time_base;
            assert(!av_bsf_init(filter));
            par = filter->par_out;
        }
        handles[i] = create(par, video[i], rate);
    }
    for (unsigned int pass = 0; pass < 3; pass++)
    {
        if (!pass) for (unsigned int i = 0; i < format->nb_streams; i++)
        { files[i] = fopen(argv[2 + i], "wb"); assert(files[i]); }
        while (av_read_frame(format, packet) >= 0)
        {
            unsigned int index = packet->stream_index;
            AVPacket *p = packet;
            if (video[index])
            {
                assert(!av_bsf_send_packet(filter, packet));
                assert(!av_bsf_receive_packet(filter, filtered));
                p = filtered;
            }
            struct wg_sample sample = {.data = (UINT_PTR)p->data, .size = p->size, .max_size = p->size,
                .flags = WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION,
                .pts = av_rescale_q(p->pts, format->streams[index]->time_base, media_time_base),
                .duration = av_rescale_q(p->duration, format->streams[index]->time_base, media_time_base)};
            struct wg_transform_push_data_params push = {handles[index], &sample};
            assert(!media_push(&push) && push.result == S_OK);
            count[index] += output(handles[index], video[index] ? 1920 * 1088 * 2 : (pass ? 256 : 65536),
                                   files[index], &checksum[index]);
            av_packet_unref(packet);
            av_packet_unref(filtered);
        }
        for (unsigned int i = 0; i < format->nb_streams; i++)
        {
            assert(!media_drain(&handles[i]));
            count[i] += output(handles[i], video[i] ? 1920 * 1088 * 2 : (pass ? 256 : 65536), files[i], &checksum[i]);
            printf("pass %u stream %u: buffers %u, checksum %08lx\n", pass, i, count[i], checksum[i]);
            fflush(stdout);
            assert(count[i]);
            if (!pass) { first[i] = checksum[i]; fclose(files[i]); files[i] = NULL; }
            else if (pass == 1 || video[i]) assert(checksum[i] == first[i]);
            checksum[i] = 1; count[i] = 0;
            assert(!media_flush(&handles[i]));
            if (!pass && !video[i])
            {
                assert(!media_destroy(&handles[i]));
                handles[i] = create(format->streams[i]->codecpar, FALSE, rate);
            }
        }
        assert(!avformat_seek_file(format, -1, INT64_MIN, 0, INT64_MAX, 0));
        if (filter) av_bsf_flush(filter);
    }
    for (unsigned int i = 0; i < format->nb_streams; i++) assert(!media_destroy(&handles[i]));
    av_bsf_free(&filter);
    av_packet_free(&packet);
    av_packet_free(&filtered);
    avformat_close_input(&format);
    return 0;
}
