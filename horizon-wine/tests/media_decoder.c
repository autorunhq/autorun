/* Host test of the decoder in source/media_unix.c: the wg_transform_* entry
 * points behind the H.264 and AAC decoders of Media Foundation.
 *
 * It is built and run by tests/check_media_decoder.py, which links it against
 * FFmpeg configured as cmake/Media.cmake configures it and hands it AAC files
 * that a second FFmpeg build wrote, with that build's own decoding of each to
 * hold the decoder's against. Every file holds a tone of 440 Hz that starts at
 * time zero, so each sample that comes out can also be held against the time
 * its buffer is stamped with.
 *
 *     media_decoder types                          what is taken and what is refused
 *     media_decoder play FILE key=value...         the sound of FILE, its times, and PCM of the other build
 *     media_decoder damaged FILE key=value...      some packets of FILE are not AAC; the rest plays
 *     media_decoder change FILE FILE... key=value  several files' packets through one decoder
 *     media_decoder file FILE                      any file with H.264 or AAC: three passes agree
 *
 * The last one is for a file brought from elsewhere. Nothing here can write
 * H.264 (FFmpeg has no encoder for it), so the check does not run a picture
 * through the decoder.
 */
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "initguid.h"
#include "../source/media_unix.c"
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/adler32.h>

#define SECOND 10000000ll
#define TONE 440.0
#define AAC_FRAME 1024

static const char *stage = "start";
static const char *file_name = "";

static void fail(int line, const char *text, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "media_decoder.c:%d: %s, %s: ", line, file_name, stage);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, " (not true: %s)\n", text);
    fflush(stderr);
    _exit(1);
}

#define ok(cond, ...) do { if (!(cond)) fail(__LINE__, #cond, __VA_ARGS__); } while (0)

static void hung(int signal)
{
    static const char text[] = "media_decoder.c: no answer after two minutes, hung in: ";
    (void)signal;
    if (write(2, text, sizeof(text) - 1) < 0 || write(2, stage, strlen(stage)) < 0 || write(2, "\n", 1) < 0) _exit(3);
    _exit(3);
}

/* What the run is asked to do, as check_media_decoder.py says it. */
static struct
{
    unsigned int rate, channels, bits, capacity, queue, eager, wow64, adts, raw, kind, at, run, seconds, join;
    const char *pcm;
} want = {.bits = 32, .capacity = 65536, .queue = 1, .run = 1, .seconds = 2};

/* Memory the calls point into. A 32-bit program's is below 4 GB, and the
 * second call table takes addresses of 32 bits. */
static BYTE *arena;
static size_t arena_used, arena_size = 16 << 20;

static void arena_open(void)
{
    if (!want.wow64)
    {
        ok((arena = malloc(arena_size)) != NULL, "no memory");
        return;
    }
#ifdef MAP_32BIT
    arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (arena != MAP_FAILED && (UINT_PTR)arena + arena_size <= 0xffffffffu) return;
#endif
    printf("not run: no memory below 4 GB on this machine\n");
    exit(77);
}

static void *take(size_t size)
{
    void *memory = arena + arena_used;
    size = (size + 15) & ~(size_t)15;
    ok(arena_used + size <= arena_size, "the test needs more than %zu bytes", arena_size);
    arena_used += size;
    memset(memory, 0, size);
    return memory;
}

/* The calls, through the table winegstreamer.dll calls through. */
struct create32 { wg_transform_t transform; struct media_type32 input, output; struct wg_transform_attrs attrs; };
struct type32_params { wg_transform_t transform; struct media_type32 type; };
struct sample32_params { wg_transform_t transform; UINT32 sample; HRESULT result; };

static NTSTATUS call(unsigned int code, void *params)
{
    return (want.wow64 ? wine_nx_media_wow64_unix_funcs : wine_nx_media_unix_funcs)[code](params);
}

static struct media_type32 low_type(struct wg_media_type type)
{
    return (struct media_type32){type.major, type.format_size, PtrToUlong(type.u.format)};
}

static NTSTATUS create_transform(struct wg_media_type input, struct wg_media_type output,
                                 struct wg_transform_attrs attrs, wg_transform_t *handle)
{
    NTSTATUS status;
    if (want.wow64)
    {
        struct create32 params = {1, low_type(input), low_type(output), attrs};
        status = call(unix_wg_transform_create, &params);
        *handle = params.transform;
    }
    else
    {
        struct wg_transform_create_params params = {1, input, output, attrs};
        status = call(unix_wg_transform_create, &params);
        *handle = params.transform;
    }
    ok(!status == !!*handle, "create says %08x and gives a decoder %#llx", (unsigned int)status,
       (unsigned long long)*handle);
    return status;
}

static HRESULT sample_call(unsigned int code, wg_transform_t handle, struct wg_sample *sample)
{
    NTSTATUS status;
    HRESULT result;
    if (want.wow64)
    {
        struct sample32_params params = {handle, PtrToUlong(sample), 0x55555555};
        status = call(code, &params);
        result = params.result;
    }
    else
    {
        struct wg_transform_push_data_params params = {handle, sample, 0x55555555};
        status = call(code, &params);
        result = params.result;
    }
    ok(!status, "the call itself fails with %08x", (unsigned int)status);
    return result;
}

static BOOL accepts_input(wg_transform_t handle)
{
    struct wg_transform_get_status_params params = {handle, 0x55};
    ok(!call(unix_wg_transform_get_status, &params), "the status is not told");
    ok(params.accepts_input <= 1, "the status is %u", params.accepts_input);
    return params.accepts_input;
}

/* One decoder and everything it has handed out. */
struct piece { size_t offset; INT64 pts; unsigned int frames; };

struct decoder_run
{
    wg_transform_t handle;
    unsigned int rate, channels, align, capacity, in_rate;
    BOOL video, resampled, frame_per_read;
    struct wg_sample *in, *out;
    BYTE *in_data, *out_data;
    BYTE *bytes;
    size_t size, room;
    struct piece *pieces;
    unsigned int piece_count, piece_room, pushed, taken_late, jumps, pictures;
    INT64 next_time, jump_from;
    unsigned long checksum;
};

#define IN_ROOM (1 << 20)
#define GUARD 32

static void open_run(struct decoder_run *r, const AVCodecParameters *par, unsigned int out_rate,
                     unsigned int out_channels)
{
    struct wg_transform_attrs attrs = {.input_queue_length = want.queue};
    struct wg_media_type input, output;
    BYTE *type = take(4096);

    memset(r, 0, sizeof(*r));
    ok(par->extradata_size < 3000, "%d bytes of codec data", par->extradata_size);
    r->video = par->codec_type == AVMEDIA_TYPE_VIDEO;
    r->next_time = AV_NOPTS_VALUE;
    r->checksum = 1;
    if (r->video)
    {
        MFVIDEOFORMAT *format = (void *)type, *plain = take(sizeof(*plain));
        format->dwSize = sizeof(*format) + par->extradata_size;
        format->guidFormat = MFVideoFormat_H264;
        format->videoInfo.dwWidth = par->width;
        format->videoInfo.dwHeight = par->height;
        memcpy(format + 1, par->extradata, par->extradata_size);
        plain->dwSize = sizeof(*plain);
        plain->guidFormat = MFVideoFormat_NV12;
        plain->videoInfo.dwWidth = par->width;
        plain->videoInfo.dwHeight = par->height;
        input = (struct wg_media_type){MFMediaType_Video, format->dwSize, {.video = format}};
        output = (struct wg_media_type){MFMediaType_Video, sizeof(*plain), {.video = plain}};
        r->capacity = 1920 * 1088 * 2;
    }
    else
    {
        WAVEFORMATEX *plain = take(sizeof(*plain));
        unsigned int size;
        r->rate = out_rate;
        r->channels = out_channels;
        r->align = out_channels * want.bits / 8;
        r->in_rate = par->sample_rate;
        r->resampled = out_rate != r->in_rate;
        r->capacity = want.capacity;
        if (want.adts || want.raw)
        {
            /* Packets with a header of their own carry no codec data; raw AAC has it after the plain header. */
            WAVEFORMATEX *format = (void *)type;
            format->wFormatTag = want.adts ? WAVE_FORMAT_MPEG_ADTS_AAC : WAVE_FORMAT_RAW_AAC1;
            format->nChannels = par->ch_layout.nb_channels;
            format->nSamplesPerSec = par->sample_rate;
            format->cbSize = want.adts ? 0 : par->extradata_size;
            if (!want.adts) memcpy(format + 1, par->extradata, par->extradata_size);
            size = sizeof(*format) + format->cbSize;
        }
        else
        {
            HEAACWAVEINFO *format = (void *)type;
            format->wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
            format->wfx.nChannels = par->ch_layout.nb_channels;
            format->wfx.nSamplesPerSec = par->sample_rate;
            format->wfx.cbSize = sizeof(*format) - sizeof(WAVEFORMATEX) + par->extradata_size;
            memcpy(format + 1, par->extradata, par->extradata_size);
            size = sizeof(*format) + par->extradata_size;
        }
        *plain = (WAVEFORMATEX){want.bits == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_IEEE_FLOAT, out_channels, out_rate,
                                out_rate * r->align, r->align, want.bits, 0};
        input = (struct wg_media_type){MFMediaType_Audio, size, {.audio = (void *)type}};
        output = (struct wg_media_type){MFMediaType_Audio, sizeof(*plain), {.audio = plain}};
    }
    ok(!create_transform(input, output, attrs, &r->handle), "no decoder for the stream");
    r->in = take(sizeof(*r->in));
    r->out = take(sizeof(*r->out));
    r->in_data = take(IN_ROOM);
    r->out_data = take(r->capacity + GUARD);
    ok(accepts_input(r->handle), "a new decoder takes no input");
}

static void keep(struct decoder_run *r, const struct wg_sample *sample)
{
    if (r->size + sample->size > r->room)
    {
        r->room = (r->room + sample->size) * 2;
        ok((r->bytes = realloc(r->bytes, r->room)) != NULL, "no memory");
    }
    if (r->piece_count == r->piece_room)
    {
        r->piece_room = r->piece_room * 2 + 64;
        ok((r->pieces = realloc(r->pieces, r->piece_room * sizeof(*r->pieces))) != NULL, "no memory");
    }
    memcpy(r->bytes + r->size, r->out_data, sample->size);
    r->pieces[r->piece_count++] = (struct piece){r->size, sample->pts, r->video ? 1 : sample->size / r->align};
    r->size += sample->size;
}

/* Everything the decoder has ready. Nothing but a buffer or "more input" is
 * an answer: a failure here is what a program sees as a broken decoder. */
static unsigned int read_all(struct decoder_run *r)
{
    unsigned int buffers = 0, changes = 0;
    BOOL more = FALSE;

    for (;;)
    {
        struct wg_sample *out = r->out;
        HRESULT result;

        memset(out, 0, sizeof(*out));
        out->data = (UINT_PTR)r->out_data;
        out->max_size = r->capacity;
        memset(r->out_data + r->capacity, 0xa5, GUARD);
        result = sample_call(unix_wg_transform_read_data, r->handle, out);
        for (unsigned int i = 0; i < GUARD; i++)
            ok(r->out_data[r->capacity + i] == 0xa5, "the decoder wrote %u bytes past the buffer", i + 1);
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            ok(!more, "the buffer before was marked as not the whole frame, and nothing follows it");
            ok(!out->size, "%u bytes come with the wish for more input", out->size);
            return buffers;
        }
        if (r->video && result == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            MFVIDEOFORMAT *format = take(sizeof(*format));
            struct wg_media_type type = {.format_size = 0};
            NTSTATUS status;
            ok(++changes < 8, "the picture changes its size at every read");
            if (want.wow64)
            {
                struct type32_params params = {r->handle, low_type(type)};
                ok(call(unix_wg_transform_get_output_type, &params) == STATUS_BUFFER_TOO_SMALL, "no size is told");
                ok(params.type.format_size == sizeof(*format), "a format of %u bytes", params.type.format_size);
                params.type.format = PtrToUlong(format);
                ok(!call(unix_wg_transform_get_output_type, &params), "the new format is not given");
                status = call(unix_wg_transform_set_output_type, &params);
            }
            else
            {
                struct wg_transform_get_output_type_params get = {r->handle, type};
                struct wg_transform_set_output_type_params set = {r->handle};
                ok(call(unix_wg_transform_get_output_type, &get) == STATUS_BUFFER_TOO_SMALL, "no size is told");
                ok(get.media_type.format_size == sizeof(*format), "a format of %u bytes", get.media_type.format_size);
                get.media_type.u.video = format;
                ok(!call(unix_wg_transform_get_output_type, &get), "the new format is not given");
                set.media_type = get.media_type;
                status = call(unix_wg_transform_set_output_type, &set);
            }
            ok(!status, "the format the decoder names is refused: %08x", (unsigned int)status);
            continue;
        }
        ok(result == S_OK, "reading fails with %08x after %u packets and %zu bytes", (unsigned int)result,
           r->pushed, r->size);
        ok(out->size && out->size <= r->capacity, "a buffer of %u bytes, room for %u", out->size, r->capacity);
        buffers++;
        if (r->video)
        {
            r->checksum = av_adler32_update(r->checksum, r->out_data, out->size);
            r->pictures++;
            continue;
        }
        ok(!(out->size % r->align), "%u bytes is not whole samples of %u bytes", out->size, r->align);
        ok(out->flags & WG_SAMPLE_FLAG_HAS_PTS, "a buffer without a time");
        ok(out->flags & WG_SAMPLE_FLAG_HAS_DURATION, "a buffer without a length");
        ok(out->flags & WG_SAMPLE_FLAG_SYNC_POINT, "sound that is not a point to start from");
        ok(out->duration == (UINT64)av_rescale(out->size / r->align, SECOND, r->rate),
           "%u samples last %llu", out->size / r->align, (unsigned long long)out->duration);
        /* A buffer starts where the one before ended. Sound at another rate than it was written at
         * has its samples between those of the file, so there the ends meet within one sample. */
        if (r->next_time != AV_NOPTS_VALUE && llabs(out->pts - r->next_time) > (r->resampled ? SECOND / r->rate + 2 : 2))
        {
            /* Sound is missing: it is where a packet was left out, or the times are wrong. */
            r->jumps++;
            r->jump_from = r->next_time;
            ok(out->pts > r->next_time, "time goes back from %lld to %lld", (long long)r->next_time,
               (long long)out->pts);
        }
        r->next_time = out->pts + out->duration;
        /* Read after every packet, what is read at once is one frame: each part of it but the
         * last says that more of it is to come. */
        if (r->frame_per_read && buffers > 1) ok(more, "the buffer before did not say that the frame goes on");
        more = !!(out->flags & WG_SAMPLE_FLAG_INCOMPLETE);
        keep(r, out);
    }
}

static void push(struct decoder_run *r, const BYTE *data, unsigned int size, INT64 pts, INT64 duration)
{
    struct wg_sample *in = r->in;
    BOOL accepts = accepts_input(r->handle);
    HRESULT result;

    ok(size <= IN_ROOM, "a packet of %u bytes", size);
    memcpy(r->in_data, data, size);
    *in = (struct wg_sample){.data = (UINT_PTR)r->in_data, .size = size, .max_size = size,
                             .flags = WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION | WG_SAMPLE_FLAG_SYNC_POINT,
                             .pts = pts, .duration = duration};
    result = sample_call(unix_wg_transform_push_data, r->handle, in);
    ok(result == S_OK || result == MF_E_NOTACCEPTING, "packet %u is answered with %08x", r->pushed,
       (unsigned int)result);
    ok((result == S_OK) == accepts, "the status says input is %s, packet %u is answered with %08x",
       accepts ? "taken" : "not taken", r->pushed, (unsigned int)result);
    if (result != S_OK)
    {
        /* Full: a program reads what is ready and offers the packet again. */
        r->taken_late++;
        read_all(r);
        ok(accepts_input(r->handle), "everything is read and input is still not taken, at packet %u", r->pushed);
        result = sample_call(unix_wg_transform_push_data, r->handle, in);
        ok(result == S_OK, "packet %u is refused with %08x though everything was read", r->pushed,
           (unsigned int)result);
    }
    /* The packet is the decoder's own copy from here on. */
    memset(r->in_data, 0xdd, size);
    r->pushed++;
    if (want.eager || r->video) read_all(r);
}

/* The end of the stream: what is held comes out, and until it has, nothing
 * new is taken. After that the decoder starts on whatever comes next. */
static void drain(struct decoder_run *r)
{
    ok(!call(unix_wg_transform_drain, &r->handle), "the end is not taken");
    ok(!accepts_input(r->handle), "input is taken while the end is being reached");
    if (r->in)
    {
        *r->in = (struct wg_sample){.data = (UINT_PTR)r->in_data, .size = 16, .max_size = 16};
        ok(sample_call(unix_wg_transform_push_data, r->handle, r->in) == MF_E_NOTACCEPTING,
           "a packet is taken while the end is being reached");
    }
    read_all(r);
    ok(!read_all(r), "more comes out after the end was reached");
    ok(accepts_input(r->handle), "the end is reached and input is still not taken");
}

/* Calls a program can get wrong. Each is refused, and nothing is lost by it. */
static void bad_calls(struct decoder_run *r)
{
    struct wg_sample *in = r->in, *out = r->out;
    HRESULT result;

    /* No room for one sample: with sound waiting that is said, and the sound waits on. */
    *out = (struct wg_sample){.data = (UINT_PTR)r->out_data, .max_size = r->align - 1, .size = 77};
    memset(r->out_data, 0xa5, r->align);
    result = sample_call(unix_wg_transform_read_data, r->handle, out);
    ok(result == (want.eager ? MF_E_TRANSFORM_NEED_MORE_INPUT : MF_E_BUFFERTOOSMALL),
       "a buffer of %u bytes is answered with %08x", r->align - 1, (unsigned int)result);
    ok(!out->size, "%u bytes are said to be in a buffer too small for a sample", out->size);
    for (unsigned int i = 0; i < r->align; i++) ok(r->out_data[i] == 0xa5, "a buffer too small is written to");
    *out = (struct wg_sample){.data = 0, .max_size = r->capacity};
    ok(sample_call(unix_wg_transform_read_data, r->handle, out) == E_INVALIDARG, "sound is read to nowhere");

    read_all(r);
    *in = (struct wg_sample){.data = 0, .size = 16, .max_size = 16};
    ok(sample_call(unix_wg_transform_push_data, r->handle, in) == E_INVALIDARG, "a packet nowhere is taken");
    *in = (struct wg_sample){.data = (UINT_PTR)r->in_data, .size = 0, .max_size = 16};
    ok(sample_call(unix_wg_transform_push_data, r->handle, in) == E_INVALIDARG, "a packet of nothing is taken");
    *in = (struct wg_sample){.data = (UINT_PTR)r->in_data, .size = 17, .max_size = 16};
    ok(sample_call(unix_wg_transform_push_data, r->handle, in) == E_INVALIDARG, "a packet longer than its buffer is taken");
    ok(accepts_input(r->handle), "after packets that were refused no input is taken");
}

static void close_run(struct decoder_run *r)
{
    ok(!call(unix_wg_transform_destroy, &r->handle), "the decoder is not freed");
    free(r->bytes);
    free(r->pieces);
    memset(r, 0, sizeof(*r));
}

/* A file, read from memory: the console's FFmpeg is built without the code
 * that opens a file by its name. */
struct memory_file
{
    BYTE *data;
    size_t size, at;
};

static int memory_read(void *opaque, uint8_t *buffer, int size)
{
    struct memory_file *file = opaque;
    if (file->at >= file->size) return AVERROR_EOF;
    size = min((size_t)size, file->size - file->at);
    memcpy(buffer, file->data + file->at, size);
    file->at += size;
    return size;
}

static int64_t memory_seek(void *opaque, int64_t offset, int whence)
{
    struct memory_file *file = opaque;
    if (whence & AVSEEK_SIZE) return file->size;
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_CUR) offset += file->at;
    else if (whence == SEEK_END) offset += file->size;
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset < 0) return AVERROR(EINVAL);
    file->at = offset;
    return offset;
}

static AVFormatContext *open_file(const char *path)
{
    struct memory_file *memory = calloc(1, sizeof(*memory));
    FILE *file = fopen(path, "rb");
    AVFormatContext *format = avformat_alloc_context();
    BYTE *buffer = av_malloc(4096);

    ok(file != NULL, "%s does not open", path);
    ok(memory && format && buffer, "no memory");
    fseek(file, 0, SEEK_END);
    memory->size = ftell(file);
    rewind(file);
    ok((memory->data = malloc(memory->size + 1)) != NULL, "no memory");
    ok(fread(memory->data, 1, memory->size, file) == memory->size, "%s is not read", path);
    fclose(file);
    format->pb = avio_alloc_context(buffer, 4096, 0, memory, memory_read, NULL, memory_seek);
    ok(format->pb != NULL, "no memory");
    ok(!avformat_open_input(&format, NULL, NULL, NULL), "%s is no file this FFmpeg reads", path);
    ok(avformat_find_stream_info(format, NULL) >= 0, "%s has no streams to tell of", path);
    return format;
}

static void close_file(AVFormatContext **format)
{
    AVIOContext *io = (*format)->pb;
    struct memory_file *memory = io->opaque;
    avformat_close_input(format);
    av_freep(&io->buffer);
    avio_context_free(&io);
    free(memory->data);
    free(memory);
}

/* A file's packets of AAC. */
struct source
{
    AVFormatContext *format;
    AVStream *stream;
    AVPacket *packet;
    unsigned int index;
};

static void open_source(struct source *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->format = open_file(path);
    for (unsigned int i = 0; i < s->format->nb_streams && !s->stream; i++)
        if (s->format->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC) s->stream = s->format->streams[i];
    ok(s->stream != NULL, "%s has no AAC", path);
    ok((s->packet = av_packet_alloc()) != NULL, "no memory");
}

static BOOL next_packet(struct source *s)
{
    av_packet_unref(s->packet);
    while (av_read_frame(s->format, s->packet) >= 0)
    {
        if (s->packet->stream_index == s->stream->index) { s->index++; return TRUE; }
        av_packet_unref(s->packet);
    }
    return FALSE;
}

static void close_source(struct source *s)
{
    av_packet_free(&s->packet);
    close_file(&s->format);
}

/* The seven bytes an AAC frame has in front of it when it travels alone. */
static unsigned int with_header(BYTE *out, const AVCodecParameters *par, const BYTE *data, unsigned int size)
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    unsigned int index = 0, channels = par->ch_layout.nb_channels, length = size + 7;
    while (index < 12 && rates[index] != par->sample_rate) index++;
    ok(index < 12 && channels <= 6, "no header says %d Hz and %u channels", par->sample_rate, channels);
    out[0] = 0xff;
    out[1] = 0xf1;
    out[2] = 1 << 6 | index << 2 | channels >> 2;
    out[3] = (channels & 3) << 6 | length >> 11;
    out[4] = length >> 3;
    out[5] = (length & 7) << 5 | 0x1f;
    out[6] = 0xfc;
    memcpy(out + 7, data, size);
    return length;
}

static INT64 packet_time(const struct source *s)
{
    return av_rescale_q(s->packet->pts, s->stream->time_base, media_time_base);
}

static INT64 packet_length(const struct source *s)
{
    return av_rescale_q(s->packet->duration, s->stream->time_base, media_time_base);
}

/* The packet as the decoder is to get it: with its header, where the run is one of frames that have them. */
static unsigned int framed(BYTE *out, size_t room, const struct source *s)
{
    ok(s->packet->size + 7u <= room, "a packet of %d bytes", s->packet->size);
    if (want.adts) return with_header(out, s->stream->codecpar, s->packet->data, s->packet->size);
    memcpy(out, s->packet->data, s->packet->size);
    return s->packet->size;
}

static double sample_at(const struct decoder_run *r, size_t frame, unsigned int channel)
{
    const BYTE *at = r->bytes + frame * r->align + channel * (want.bits / 8);
    if (want.bits == 16) { short value; memcpy(&value, at, 2); return value / 32768.0; }
    float value;
    memcpy(&value, at, 4);
    return value;
}

/* The tone. A sample's time is its buffer's time and its place in the buffer;
 * the tone began at `origin`, so the sample is a point of that sine and of no
 * other. One sample too many or too few anywhere before it, or a time stamp
 * that is off by as much, and every sample from there on is 0.06 of a radian
 * from where it belongs. Written at 320 kbit/s, what AAC itself changes is a
 * hundredth of that; the bound is a sixth of it over a stretch of samples.
 * Between `from` and `to` seconds of the tone, outside `skip`. */
#define STRETCH 256

static double check_tone(const struct decoder_run *r, double origin, double from, double to,
                         double skip_from, double skip_to, const char *what)
{
    double first = 0;
    for (unsigned int channel = 0; channel < r->channels; channel++)
    {
        double square = 0, level = 0, worst = 0, worst_time = 0, far = 0, far_time = 0, stretch = 0;
        size_t count = 0, in_stretch = 0;
        for (unsigned int pass = 0; pass < 2; pass++)
        {
            if (pass)
            {
                level = count ? sqrt(2 * square / count) : 0;
                ok(count > r->rate / 4 && level > 0.01, "%s: channel %u has %zu samples of the tone at level %.4f",
                   what, channel, count, level);
            }
            for (unsigned int p = 0; p < r->piece_count; p++)
            {
                const struct piece *piece = &r->pieces[p];
                for (unsigned int i = 0; i < piece->frames; i++)
                {
                    double time = (double)piece->pts / SECOND + (double)i / r->rate - origin;
                    double value, wrong;
                    if (time < from || time > to || (time >= skip_from && time < skip_to)) continue;
                    value = sample_at(r, piece->offset / r->align + i, channel);
                    if (!pass) { square += value * value; count++; continue; }
                    wrong = fabs(value - level * sin(2 * M_PI * TONE * time)) / level;
                    if (wrong > far) { far = wrong; far_time = time; }
                    stretch += wrong * wrong;
                    if (++in_stretch < STRETCH) continue;
                    stretch = sqrt(stretch / STRETCH);
                    if (stretch > worst) { worst = stretch; worst_time = time; }
                    stretch = 0;
                    in_stretch = 0;
                }
            }
        }
        if (getenv("MEDIA_DECODER_SAYS"))
            printf("%s: channel %u level %.4f, a stretch is %.4f from the tone at %.4f s, a sample %.4f at %.4f s\n",
                   what, channel, level, worst, worst_time, far, far_time);
        ok(worst < 0.01, "%s: channel %u is %.3f of the tone's height away from the tone around %.4f s", what, channel,
           worst, worst_time);
        ok(far < 0.05, "%s: channel %u has a sample %.3f of the tone's height away from the tone at %.4f s", what,
           channel, far, far_time);
        if (!channel) first = level;
    }
    return first;
}

static size_t frames_of(const struct decoder_run *r)
{
    return r->size / r->align;
}

static void play_file(struct decoder_run *r, const char *path, int damaged_at, unsigned int *packets)
{
    static BYTE data[IN_ROOM];
    struct source s;
    unsigned int out_rate, out_channels;

    open_source(&s, path);
    out_rate = want.rate ? want.rate : (unsigned int)s.stream->codecpar->sample_rate;
    out_channels = want.channels ? want.channels : 2;
    open_run(r, s.stream->codecpar, out_rate, out_channels);
    r->frame_per_read = want.eager && !want.join && damaged_at < 0;
    while (next_packet(&s))
    {
        unsigned int size = framed(data, sizeof(data), &s);
        INT64 time = packet_time(&s), length = packet_length(&s);
        if (s.index == 4 && damaged_at < 0) bad_calls(r);
        if (damaged_at >= 0 && s.index - 1 >= (unsigned int)damaged_at && s.index - 1 < damaged_at + want.run)
        {
            /* What is in the packet's place: 0 all ones, 1 noise, 2 its first half, 3 nothing but
             * zeroes, 4 its first byte. */
            unsigned int seed = 96 + s.index;
            if (want.kind == 0) memset(data, 0xff, size);
            else if (want.kind == 1) for (unsigned int i = 0; i < size; i++) data[i] = (seed = seed * 1103515245 + 12345) >> 16;
            else if (want.kind == 2) size = (size + 1) / 2;
            else if (want.kind == 3) memset(data, 0, size);
            else size = 1;
        }
        else if (want.join && s.index - 1 == want.join)
        {
            /* Two frames in one packet, under the time of the first. */
            ok(next_packet(&s), "packet %u is the last", want.join);
            size += framed(data + size, sizeof(data) - size, &s);
            length += packet_length(&s);
        }
        push(r, data, size, time, length);
    }
    *packets = s.index;
    drain(r);
    close_source(&s);
}

/* After the end the same decoder plays the file's start again and gives the
 * samples it gave the first time. So it does when it was emptied in the
 * middle of something: nothing of that comes out, before or after. */
static void play_again(struct decoder_run *r, const char *path)
{
    static BYTE data[IN_ROOM];
    size_t before = r->size, expect;

    for (unsigned int round = 0; round < 3; round++)
    {
        struct source s;
        stage = round ? "after being emptied" : "after the end";
        open_source(&s, path);
        r->next_time = AV_NOPTS_VALUE;
        r->size = before;
        r->piece_count = 0;
        for (unsigned int i = 0; i < (round == 1 ? 3 : 6) && next_packet(&s); i++)
            push(r, data, framed(data, sizeof(data), &s), packet_time(&s), packet_length(&s));
        close_source(&s);
        if (round == 1)
        {
            /* Three packets in, some of them decoded and partly read. */
            *r->out = (struct wg_sample){.data = (UINT_PTR)r->out_data, .max_size = r->align};
            sample_call(unix_wg_transform_read_data, r->handle, r->out);
            ok(!call(unix_wg_transform_flush, &r->handle), "the decoder is not emptied");
            ok(accepts_input(r->handle), "an emptied decoder takes no input");
            ok(!read_all(r), "an emptied decoder still has sound");
            continue;
        }
        drain(r);
        ok(!r->jumps, "the times leave holes the second time");
        expect = r->size - before;
        ok(llabs((long long)(expect / r->align) - av_rescale(6 * AAC_FRAME, r->rate, r->in_rate)) <= 2,
           "six packets give %zu samples the second time", expect / r->align);
        if (r->resampled) continue;
        /* FFmpeg's decoder does not forget everything when it is emptied: the first frame after
         * differs from a new decoder's at the fifth decimal. From the second frame on the samples
         * are the same ones. */
        for (size_t i = 0; i < (size_t)AAC_FRAME * r->channels; i++)
            ok(fabs(sample_at(r, i / r->channels, i % r->channels)
                    - sample_at(r, before / r->align + i / r->channels, i % r->channels)) < 0.001,
               "the second time sample %zu of the first frame is another", i / r->channels);
        ok(!memcmp(r->bytes + AAC_FRAME * r->align, r->bytes + before + AAC_FRAME * r->align,
                   expect - AAC_FRAME * r->align), "the second time packets two to six sound different");
    }
    r->size = before;
}

static void test_play(const char *path)
{
    struct decoder_run r;
    unsigned int packets;
    size_t primed;

    stage = "playing";
    play_file(&r, path, -1, &packets);
    ok(packets > 8, "%u packets", packets);
    ok(!r.jumps, "the times of the buffers leave %u holes, the first at %lld", r.jumps, (long long)r.jump_from);
    ok(r.pieces[0].pts <= 0, "the sound starts at %lld", (long long)r.pieces[0].pts);
    /* Every packet is a frame of 1024 samples. */
    ok(llabs((long long)frames_of(&r) - av_rescale(packets * AAC_FRAME, r.rate, r.in_rate)) <= (r.resampled ? 2 : 0),
       "%u packets of %u Hz give %zu samples at %u Hz", packets, r.in_rate, frames_of(&r), r.rate);
    check_tone(&r, 0, 0.05, want.seconds - 0.05, 0, 0, "the sound");

    if (want.pcm)
    {
        /* The other build's decoding leaves out what the encoder put in front of the tone, as a
         * player does; that is the part before time zero here. */
        FILE *file = fopen(want.pcm, "rb");
        size_t size, got;
        BYTE *theirs;
        stage = "against the other build";
        ok(file != NULL, "%s does not open", want.pcm);
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);
        ok((theirs = malloc(size + 1)) != NULL, "no memory");
        got = fread(theirs, 1, size, file);
        fclose(file);
        ok(got == size && size && !(size % r.align), "%zu bytes of PCM", size);
        primed = (size_t)llround((double)-r.pieces[0].pts * r.rate / SECOND);
        ok(primed * r.align + size <= r.size, "the other build has %zu samples after time zero, here are %zu",
           size / r.align, frames_of(&r) - primed);
        ok(frames_of(&r) - primed - size / r.align < 3 * AAC_FRAME, "the other build has %zu samples, here are %zu",
           size / r.align, frames_of(&r) - primed);
        for (size_t i = 0; i < size; i++)
            ok(r.bytes[primed * r.align + i] == theirs[i], "sample %zu, channel %zu differs from the other build's",
               i / r.align, i % r.align / (want.bits / 8));
        free(theirs);
    }
    printf("%u packets, %zu samples in %u buffers, %u packets taken after a read\n", packets, frames_of(&r),
           r.piece_count, r.taken_late);
    play_again(&r, path);
    close_run(&r);
}

/* Packets that are something else, `run` of them from packet `at` on. What
 * they held is lost and nothing more: the packets after them play, in their
 * places. */
static void test_damaged(const char *path)
{
    struct decoder_run whole, r;
    unsigned int packets, again;
    double from, frame;
    size_t same = 0, lost;

    stage = "the whole file";
    play_file(&whole, path, -1, &packets);
    ok(want.at + want.run <= packets, "packets %u to %u of %u", want.at, want.at + want.run - 1, packets);
    stage = "a damaged packet";
    play_file(&r, path, want.at, &again);
    ok(again == packets, "%u packets, then %u", packets, again);
    frame = (double)AAC_FRAME / r.in_rate;
    lost = av_rescale(want.run * AAC_FRAME, r.rate, r.in_rate) + (r.resampled ? 8 : 0);
    ok(frames_of(&r) + lost >= frames_of(&whole) && frames_of(&r) <= frames_of(&whole),
       "%zu samples of %zu are left", frames_of(&r), frames_of(&whole));
    ok(r.jumps <= 1, "the times leave %u holes", r.jumps);
    /* The frames of the packets, and the one after them, which is built on the last. */
    from = (double)whole.pieces[0].pts / SECOND + want.at * frame;
    /* The hole is where the packet was. At another rate the converter holds the last few samples
     * from before the hole and hands them out with the first after it, so the hole starts that
     * much sooner; they are inside the stretch left out below. */
    if (r.jumps && want.at)
        ok(from - (double)r.jump_from / SECOND < (r.resampled ? 0.005 : 0.0001) && (double)r.jump_from / SECOND < from + 0.0001,
           "the hole is at %.4f s, the packet at %.4f s", (double)r.jump_from / SECOND, from);
    check_tone(&r, 0, 0.05, want.seconds - 0.05, from - frame, from + (want.run + 2) * frame, "around the damage");
    /* Outside that the samples are the ones of the whole file. (At another rate they lie between
     * the file's, and not at the same places after a hole as without it.) */
    for (unsigned int p = 0; p < r.piece_count && !r.resampled; p++)
    {
        const struct piece *piece = &r.pieces[p];
        for (unsigned int i = 0; i < piece->frames; i++)
        {
            double time = (double)piece->pts / SECOND + (double)i / r.rate;
            long long place = llround((time - (double)whole.pieces[0].pts / SECOND) * r.rate);
            if (time >= from - frame && time < from + (want.run + 2) * frame) continue;
            if (place < 0 || (size_t)place >= frames_of(&whole)) continue;
            ok(!memcmp(r.bytes + piece->offset + (size_t)i * r.align, whole.bytes + (size_t)place * r.align, r.align),
               "the sample at %.5f s is not the one of the whole file", time);
            same++;
        }
    }
    ok(r.resampled || same + (want.run + 5) * AAC_FRAME >= frames_of(&whole), "only %zu samples of %zu could be held against the whole file",
       same, frames_of(&whole));
    printf("%zu of %zu samples, %u holes\n", frames_of(&r), frames_of(&whole), r.jumps);
    close_run(&whole);
    close_run(&r);
}

/* The packets of several files one after another, as a stream does that
 * changes its channels or its rate. The output was asked for once and stays:
 * every file's tone comes out whole, in it. Frames with a header say what they
 * are; without one a stream can only gain channels, since nothing tells the
 * decoder that one of them is gone. */
static void test_change(char **paths, unsigned int count)
{
    struct decoder_run r;
    INT64 shift = 0;
    double origin[8], sum = 0;
    unsigned int total = 0;

    ok(count >= 2 && count <= 8, "%u files", count);
    for (unsigned int f = 0; f < count; f++)
    {
        struct source s;
        INT64 end = 0;
        stage = "changing";
        open_source(&s, paths[f]);
        if (!f) open_run(&r, s.stream->codecpar, want.rate ? want.rate : (unsigned int)s.stream->codecpar->sample_rate,
                         want.channels ? want.channels : 2);
        if ((unsigned int)s.stream->codecpar->sample_rate != r.rate) r.resampled = TRUE;
        origin[f] = (double)shift / SECOND;
        while (next_packet(&s))
        {
            INT64 start = packet_time(&s);
            static BYTE data[IN_ROOM];
            if (s.index == 1) { shift -= start; origin[f] = (double)shift / SECOND; }
            push(&r, data, framed(data, sizeof(data), &s), start + shift, packet_length(&s));
            /* A whole frame comes out of the last packet too, though the file gives it less time. */
            end = start + av_rescale(AAC_FRAME, SECOND, s.stream->codecpar->sample_rate);
        }
        sum += (double)s.index * AAC_FRAME / s.stream->codecpar->sample_rate;
        total += s.index;
        shift += end;
        close_source(&s);
    }
    drain(&r);
    ok(!r.jumps, "the times of the buffers leave %u holes, the first at %lld", r.jumps, (long long)r.jump_from);
    ok(fabs((double)frames_of(&r) - sum * r.rate) <= (r.resampled ? 2 * count : 0),
       "%u packets of %.4f s give %zu samples at %u Hz", total, sum, frames_of(&r), r.rate);
    for (unsigned int f = 0; f < count; f++)
    {
        /* As loud as the file is alone, in the same output: channels mixed as its own are. */
        struct decoder_run alone;
        unsigned int packets;
        double level, own;
        char what[64];
        snprintf(what, sizeof(what), "file %u of the stream", f + 1);
        level = check_tone(&r, origin[f], 0.06, want.seconds - 0.06, 0, 0, what);
        stage = "a file of the stream alone";
        if (!want.rate) want.rate = r.rate;
        play_file(&alone, paths[f], -1, &packets);
        own = check_tone(&alone, 0, 0.06, want.seconds - 0.06, 0, 0, "alone");
        ok(fabs(level - own) < 0.01 * own, "file %u of the stream comes out at level %.4f, alone at %.4f", f + 1,
           level, own);
        close_run(&alone);
    }
    printf("%u packets, %zu samples\n", total, frames_of(&r));
    close_run(&r);
}

static void test_types(void)
{
    HEAACWAVEINFO *input = take(sizeof(*input) + 2);
    WAVEFORMATEXTENSIBLE *output = take(sizeof(*output));
    MFVIDEOFORMAT *video = take(sizeof(*video)), *picture = take(sizeof(*picture));
    struct wg_media_type in = {MFMediaType_Audio, sizeof(*input) + 2, {.audio = &input->wfx}};
    struct wg_media_type out = {MFMediaType_Audio, sizeof(*output), {.audio = &output->Format}};
    struct wg_media_type vin = {MFMediaType_Video, sizeof(*video), {.video = video}};
    struct wg_media_type vout = {MFMediaType_Video, sizeof(*picture), {.video = picture}};
    struct wg_transform_attrs attrs = {.input_queue_length = 1};
    static const BYTE config[2] = {0x11, 0x90}; /* AAC LC, 48000 Hz, two channels */
    wg_transform_t handle;
    struct media_decoder *d;

    stage = "types";
    input->wfx = (WAVEFORMATEX){WAVE_FORMAT_MPEG_HEAAC, 2, 48000, 0, 0, 0,
                                sizeof(*input) - sizeof(WAVEFORMATEX) + 2};
    memcpy(input + 1, config, 2);
    output->Format = (WAVEFORMATEX){WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 0, 0, 32, 0};

    for (unsigned int i = 0; i < 2; i++)
    {
        unsigned int align = output->Format.nChannels * output->Format.wBitsPerSample / 8;
        ok(!create_transform(in, out, attrs, &handle), "AAC to %u bits is refused", output->Format.wBitsPerSample);
        d = decoder(handle);
        ok(d->audio_type.Format.nBlockAlign == align, "a sample of %u bytes", d->audio_type.Format.nBlockAlign);
        ok(d->audio_type.Format.nAvgBytesPerSec == align * 48000, "%u bytes a second",
           (unsigned int)d->audio_type.Format.nAvgBytesPerSec);
        ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");
        output->Format.nBlockAlign = align + 1;
        ok(create_transform(in, out, attrs, &handle) == STATUS_INVALID_PARAMETER, "a sample size that cannot be is taken");
        output->Format.nBlockAlign = align;
        ok(!create_transform(in, out, attrs, &handle), "the right sample size is refused");
        ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");
        output->Format.nBlockAlign = 0;
        output->Format.wFormatTag = WAVE_FORMAT_PCM;
        output->Format.wBitsPerSample = 16;
    }
    /* Output that is neither 16 bits nor float, no channels, no rate. */
    output->Format.wBitsPerSample = 24;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "24 bits are taken");
    output->Format.wBitsPerSample = 16;
    output->Format.nChannels = 0;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "no channels are taken");
    output->Format.nChannels = 2;
    output->Format.nSamplesPerSec = 0;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "no rate is taken");
    output->Format.nSamplesPerSec = 48000;
    /* Input that is not AAC, a format cut short, a payload kind that does not exist. */
    input->wfx.wFormatTag = WAVE_FORMAT_PCM;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "PCM is taken for AAC");
    input->wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
    {
        /* The same with a format that would do for raw AAC but for its name. */
        WAVEFORMATEX *raw = take(sizeof(*raw) + 2);
        struct wg_media_type raw_in = {MFMediaType_Audio, sizeof(*raw) + 2, {.audio = raw}};
        *raw = (WAVEFORMATEX){WAVE_FORMAT_RAW_AAC1, 2, 48000, 0, 0, 0, 2};
        memcpy(raw + 1, config, 2);
        ok(!create_transform(raw_in, out, attrs, &handle), "raw AAC is refused");
        ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");
        raw->wFormatTag = WAVE_FORMAT_MPEG_ADTS_AAC;
        ok(!create_transform(raw_in, out, attrs, &handle), "AAC in frames with headers is refused");
        ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");
        raw->wFormatTag = WAVE_FORMAT_PCM;
        ok(create_transform(raw_in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "PCM is taken for raw AAC");
        raw->wFormatTag = WAVE_FORMAT_MPEGLAYER3;
        ok(create_transform(raw_in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "MP3 is taken for raw AAC");
    }
    in.format_size = sizeof(WAVEFORMATEX) - 1;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "half a format is taken");
    in.format_size = sizeof(*input) + 1;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "a format shorter than it says is taken");
    in.format_size = sizeof(*input) + 2;
    input->wPayloadType = 2;
    ok(create_transform(in, out, attrs, &handle) == STATUS_NOT_SUPPORTED, "payload kind 2 is taken");
    input->wPayloadType = 0;
    attrs.output_plane_align = 2;
    ok(create_transform(in, out, attrs, &handle) == STATUS_INVALID_PARAMETER, "an alignment of 3 bytes is taken");
    attrs.output_plane_align = 0;
    ok(!create_transform(in, out, attrs, &handle), "after all that the plain format is refused");
    ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");

    /* H.264: the console's FFmpeg has the decoder, and pictures have a size and a known format. */
    video->dwSize = sizeof(*video);
    video->guidFormat = MFVideoFormat_H264;
    video->videoInfo.dwWidth = 1280;
    video->videoInfo.dwHeight = 720;
    *picture = *video;
    picture->guidFormat = MFVideoFormat_NV12;
    ok(!create_transform(vin, vout, attrs, &handle), "H.264 to NV12 is refused");
    {
        struct decoder_run r = {.handle = handle, .video = TRUE, .capacity = 4096};
        r.out = take(sizeof(*r.out));
        r.out_data = take(r.capacity + GUARD);
        ok(accepts_input(handle), "a new decoder takes no input");
        ok(!read_all(&r), "a picture comes out of nothing");
        drain(&r);
        ok(!call(unix_wg_transform_flush, &handle), "the decoder is not emptied");
        ok(accepts_input(handle), "an emptied decoder takes no input");
    }
    ok(!call(unix_wg_transform_destroy, &handle), "the decoder is not freed");
    picture->guidFormat = MFVideoFormat_H264;
    ok(create_transform(vin, vout, attrs, &handle) == STATUS_NOT_SUPPORTED, "H.264 is taken as a picture format");
    picture->guidFormat = MFVideoFormat_NV12;
    picture->videoInfo.dwWidth = 0;
    ok(create_transform(vin, vout, attrs, &handle) == STATUS_NOT_SUPPORTED, "pictures of no width are taken");
    picture->videoInfo.dwWidth = 8193;
    ok(create_transform(vin, vout, attrs, &handle) == STATUS_NOT_SUPPORTED, "pictures 8193 wide are taken");
    picture->videoInfo.dwWidth = 1280;
    video->guidFormat = MFVideoFormat_NV12;
    ok(create_transform(vin, vout, attrs, &handle) == STATUS_NOT_SUPPORTED, "NV12 is taken for H.264");
    printf("types: done\n");
}

/* A file from elsewhere, H.264 in MP4 and AAC: played three times, every
 * stream comes out the same, and a decoder that is emptied has nothing left. */
static void test_file(const char *path)
{
    unsigned long first[8] = {0};
    unsigned int counts[8] = {0};

    for (unsigned int pass = 0; pass < 3; pass++)
    {
        AVFormatContext *format = open_file(path);
        AVPacket *packet = av_packet_alloc(), *filtered = av_packet_alloc();
        AVBSFContext *filter = NULL;
        struct decoder_run runs[8];
        BOOL used[8] = {0};
        size_t mark = arena_used;

        stage = "a file from elsewhere";
        ok(packet && filtered, "no memory");
        ok(format->nb_streams <= 8, "%u streams", format->nb_streams);
        for (unsigned int i = 0; i < format->nb_streams; i++)
        {
            AVCodecParameters *par = format->streams[i]->codecpar;
            if (par->codec_id == AV_CODEC_ID_H264 && !filter)
            {
                const AVBitStreamFilter *annexb = av_bsf_get_by_name("h264_mp4toannexb");
                ok(annexb != NULL, "this FFmpeg cannot take H.264 out of MP4");
                ok(!av_bsf_alloc(annexb, &filter), "no memory");
                ok(avcodec_parameters_copy(filter->par_in, par) >= 0, "no memory");
                filter->time_base_in = format->streams[i]->time_base;
                ok(!av_bsf_init(filter), "the H.264 of %s is not as MP4 holds it", path);
                open_run(&runs[i], filter->par_out, 0, 0);
                used[i] = TRUE;
            }
            else if (par->codec_id == AV_CODEC_ID_AAC)
            {
                open_run(&runs[i], par, par->sample_rate, 2);
                used[i] = TRUE;
            }
        }
        while (av_read_frame(format, packet) >= 0)
        {
            unsigned int index = packet->stream_index;
            AVPacket *p = packet;
            if (index < 8 && used[index])
            {
                if (runs[index].video)
                {
                    ok(!av_bsf_send_packet(filter, packet), "a packet of H.264 is not taken apart");
                    ok(!av_bsf_receive_packet(filter, filtered), "a packet of H.264 is not taken apart");
                    p = filtered;
                }
                push(&runs[index], p->data, p->size,
                     av_rescale_q(p->pts, format->streams[index]->time_base, media_time_base),
                     av_rescale_q(p->duration, format->streams[index]->time_base, media_time_base));
            }
            av_packet_unref(packet);
            av_packet_unref(filtered);
        }
        for (unsigned int i = 0; i < format->nb_streams; i++)
        {
            struct decoder_run *r = &runs[i];
            unsigned long sum;
            unsigned int count;
            if (!used[i]) continue;
            drain(r);
            sum = r->video ? r->checksum : av_adler32_update(1, r->bytes, r->size);
            count = r->video ? r->pictures : (unsigned int)frames_of(r);
            printf("pass %u, stream %u: %u %s, checksum %08lx\n", pass, i, count, r->video ? "pictures" : "samples", sum);
            ok(count, "stream %u gives nothing", i);
            if (!pass) { first[i] = sum; counts[i] = count; }
            ok(sum == first[i] && count == counts[i], "stream %u is not the same the second time", i);
            /* Emptied, the decoder starts again. */
            ok(!call(unix_wg_transform_flush, &r->handle), "the decoder is not emptied");
            ok(accepts_input(r->handle), "an emptied decoder takes no input");
            ok(!read_all(r), "an emptied decoder still has something");
            close_run(r);
        }
        av_bsf_free(&filter);
        av_packet_free(&packet);
        av_packet_free(&filtered);
        close_file(&format);
        arena_used = mark;
    }
}

int main(int argc, char **argv)
{
    char **files = argv + 2;
    unsigned int file_count = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, hung);
    alarm(120);
    av_log_set_level(getenv("MEDIA_DECODER_SAYS") ? AV_LOG_WARNING : AV_LOG_QUIET);
    ok(argc >= 2, "media_decoder types|play|damaged|change|file ...");
    for (int i = 2; i < argc; i++)
    {
        const char *equals = strchr(argv[i], '=');
        size_t length = equals ? (size_t)(equals - argv[i]) : 0;
        if (!equals) { ok(file_count == (unsigned int)i - 2, "files come first"); file_count++; continue; }
#define NUMBER(name) if (length == strlen(#name) && !strncmp(argv[i], #name, length)) { want.name = strtoul(equals + 1, NULL, 0); continue; }
        NUMBER(rate) NUMBER(channels) NUMBER(bits) NUMBER(capacity) NUMBER(queue) NUMBER(eager) NUMBER(wow64)
        NUMBER(adts) NUMBER(raw) NUMBER(kind) NUMBER(at) NUMBER(run) NUMBER(seconds) NUMBER(join)
#undef NUMBER
        if (length == 3 && !strncmp(argv[i], "pcm", 3)) { want.pcm = equals + 1; continue; }
        ok(0, "what is %s", argv[i]);
    }
    ok(want.bits == 16 || want.bits == 32, "%u bits", want.bits);
    arena_open();
    if (file_count) file_name = strrchr(files[0], '/') ? strrchr(files[0], '/') + 1 : files[0];

    if (!strcmp(argv[1], "types")) test_types();
    else if (!strcmp(argv[1], "play") && file_count == 1) test_play(files[0]);
    else if (!strcmp(argv[1], "damaged") && file_count == 1) test_damaged(files[0]);
    else if (!strcmp(argv[1], "change")) test_change(files, file_count);
    else if (!strcmp(argv[1], "file") && file_count == 1) test_file(files[0]);
    else ok(0, "media_decoder types|play|damaged|change|file ...");
    return 0;
}
