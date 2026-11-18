/* Host test of the media parser in source/media_unix.c: the wg_parser_* entry
 * points that DirectShow and the Windows Media reader play files through.
 *
 * It is built and run by tests/check_media_parser.py, which links it against
 * FFmpeg configured as cmake/Media.cmake configures it and hands it files that
 * a second FFmpeg build wrote, with that build's own decoding of each file to
 * hold the parser's against. Files are read the way the Windows side reads
 * them: a thread asks which bytes are wanted and pushes them.
 *
 *     media_parser play FILE key=value...     what a program gets when it plays FILE
 *     media_parser packets FILE key=value...  the same with compressed output
 *     media_parser rebase FILE key=value...   time stamps with and without the rebase flag
 *     media_parser broken FILE key=value...   FILE must be refused
 *     media_parser abort FILE hold=N          disconnect while a read is not answered
 *     media_parser units                      the first MPEG packet after a move, on bytes made here
 */
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "initguid.h"
#include "../source/media_unix.c"
#include <libavutil/adler32.h>

#define SECOND 10000000ll
#define MILLISECOND 10000ll

static const char *stage = "start";
static const char *file_name = "";

static void fail(int line, const char *text, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "media_parser.c:%d: %s, %s: ", line, file_name, stage);
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
    static const char text[] = "media_parser.c: no answer after two minutes, hung in: ";
    (void)signal;
    if (write(2, text, sizeof(text) - 1) < 0 || write(2, stage, strlen(stage)) < 0 || write(2, "\n", 1) < 0) _exit(3);
    _exit(3);
}

/* What the file is, as check_media_parser.py made it. */
static struct
{
    unsigned int streams, width, height, fps_n, fps_d, frames, rate, channels, flags, chunk, wow64, partial;
    unsigned int shift;     /* the streams do not start at zero */
    unsigned int close;     /* the decoder gives another kind of picture: near the other build's, not equal */
    unsigned int nokeys;    /* the decoder marks no picture as a key frame */
    unsigned int late;      /* a move may land this many frames after the place: there is no index */
    unsigned int untimed;   /* not every packet of pictures has a time in the file */
    unsigned int samples;   /* the sound's packets are samples: cut wherever the reading starts */
    unsigned int prime;     /* after a move to the start this many samples are not the file's: the
                             * packet before the first, which the decoder warms up on, is not read again */
    long long fail_at;
    int hold;
    double duration;        /* what the file says it lasts */
    double length;          /* what it lasts */
    const char *yuv, *pcm, *vcrc, *acrc, *vtype, *atype, *vlang, *alang, *vname, *aname;
} want = {.fail_at = -1, .hold = -1, .length = -1};

static BYTE *file_data, *ref_yuv, *ref_pcm;
static size_t file_size, ref_yuv_size, ref_pcm_size;
static INT64 frame_time;
static BOOL moved;      /* the file has been moved in since it was opened */
static BOOL unordered;  /* packets are handed out, and some have no time */

static BYTE *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    BYTE *data;
    long length;

    ok(file && !fseek(file, 0, SEEK_END) && (length = ftell(file)) >= 0 && !fseek(file, 0, SEEK_SET),
       "cannot read %s", path);
    data = malloc(length + 1);
    ok(data && fread(data, 1, length, file) == (size_t)length, "cannot read %s", path);
    fclose(file);
    *size = length;
    return data;
}

/* The two tables. The second takes the 32-bit structures a 32-bit program
 * passes, whose pointers are 32 bits wide, so everything it is given lives in
 * memory below 4 GB. */
static const unixlib_entry_t *calls = wine_nx_media_unix_funcs;
static BYTE *low;

#define LOW_FORMAT   0x0000
#define LOW_INFO     0x1000
#define LOW_TAG      0x2000
#define LOW_TAG_SIZE 0x3000
#define LOW_READ     0x100000
#define LOW_DATA     0x200000
#define LOW_TOTAL    0x1000000

static UINT32 low32(unsigned int offset)
{
    return (UINT32)(UINT_PTR)(low + offset);
}

static void map_low(void)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_32BIT
    flags |= MAP_32BIT;
#endif
    low = mmap((void *)0x20000000, LOW_TOTAL, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (low == MAP_FAILED || (UINT_PTR)low + LOW_TOTAL > 0xffffffffu)
    {
        puts("media_parser: no memory below 4 GB here, the 32-bit table was not run");
        exit(77);
    }
    calls = wine_nx_media_wow64_unix_funcs;
}

static NTSTATUS do_connect(wg_parser_t parser, UINT64 size)
{
    if (low)
    {
        struct { wg_parser_t parser; UINT32 uri; UINT64 file_size; } params = {parser, 0, size};
        return calls[unix_wg_parser_connect](&params);
    }
    else
    {
        struct wg_parser_connect_params params = {parser, NULL, size};
        return calls[unix_wg_parser_connect](&params);
    }
}

static NTSTATUS do_push(wg_parser_t parser, const BYTE *data, UINT32 size)
{
    if (low)
    {
        struct { wg_parser_t parser; UINT32 data; UINT32 size; } params = {parser, data ? low32(LOW_READ) : 0, size};
        ok(size <= LOW_DATA - LOW_READ, "a read of %u bytes", size);
        if (data) memcpy(low + LOW_READ, data, size);
        return calls[unix_wg_parser_push_data](&params);
    }
    else
    {
        struct wg_parser_push_data_params params = {parser, data, size};
        return calls[unix_wg_parser_push_data](&params);
    }
}

static void get_format(wg_parser_stream_t stream, BOOL codec, struct wg_format *format)
{
    unsigned int call = codec ? unix_wg_parser_stream_get_codec_format : unix_wg_parser_stream_get_current_format;

    memset(format, 0xcc, sizeof(*format));
    if (low)
    {
        struct { wg_parser_stream_t stream; UINT32 format; } params = {stream, low32(LOW_FORMAT)};
        ok(!calls[call](&params), "the format of a stream");
        memcpy(format, low + LOW_FORMAT, sizeof(*format));
    }
    else
    {
        struct wg_parser_stream_get_current_format_params params = {stream, format};
        ok(!calls[call](&params), "the format of a stream");
    }
}

static void do_enable(wg_parser_stream_t stream, const struct wg_format *format)
{
    if (low)
    {
        struct { wg_parser_stream_t stream; UINT32 format; } params = {stream, low32(LOW_FORMAT)};
        memcpy(low + LOW_FORMAT, format, sizeof(*format));
        ok(!calls[unix_wg_parser_stream_enable](&params), "enabling a stream");
    }
    else
    {
        struct wg_parser_stream_enable_params params = {stream, format};
        ok(!calls[unix_wg_parser_stream_enable](&params), "enabling a stream");
    }
}

static NTSTATUS do_get_buffer(wg_parser_t parser, wg_parser_stream_t stream, struct wg_parser_buffer *info)
{
    NTSTATUS status;

    if (low)
    {
        struct { wg_parser_t parser; wg_parser_stream_t stream; UINT32 buffer; } params = {parser, stream, low32(LOW_INFO)};
        status = calls[unix_wg_parser_stream_get_buffer](&params);
        memcpy(info, low + LOW_INFO, sizeof(*info));
    }
    else
    {
        struct wg_parser_stream_get_buffer_params params = {parser, stream, info};
        status = calls[unix_wg_parser_stream_get_buffer](&params);
    }
    return status;
}

static NTSTATUS do_copy(wg_parser_stream_t stream, BYTE *data, UINT32 offset, UINT32 size)
{
    NTSTATUS status;

    if (low)
    {
        struct { wg_parser_stream_t stream; UINT32 data; UINT32 offset; UINT32 size; } params =
                {stream, low32(LOW_DATA), offset, size};
        ok(size <= LOW_TOTAL - LOW_DATA, "a buffer of %u bytes", size);
        if (!(status = calls[unix_wg_parser_stream_copy_buffer](&params))) memcpy(data, low + LOW_DATA, size);
    }
    else
    {
        struct wg_parser_stream_copy_buffer_params params = {stream, data, offset, size};
        status = calls[unix_wg_parser_stream_copy_buffer](&params);
    }
    return status;
}

static NTSTATUS do_tag(wg_parser_stream_t stream, wg_parser_tag tag, char *buffer, UINT32 *size)
{
    NTSTATUS status;

    if (low)
    {
        struct { wg_parser_stream_t stream; wg_parser_tag tag; UINT32 buffer; UINT32 size; } params =
                {stream, tag, low32(LOW_TAG), low32(LOW_TAG_SIZE)};
        ok(*size <= 0x1000, "a tag of %u bytes", *size);
        memcpy(low + LOW_TAG_SIZE, size, sizeof(*size));
        status = calls[unix_wg_parser_stream_get_tag](&params);
        memcpy(size, low + LOW_TAG_SIZE, sizeof(*size));
        if (!status) memcpy(buffer, low + LOW_TAG, *size);
    }
    else
    {
        struct wg_parser_stream_get_tag_params params = {stream, tag, buffer, size};
        status = calls[unix_wg_parser_stream_get_tag](&params);
    }
    return status;
}

/* The thread that reads the file for the parser, as the Windows side's does:
 * it is started before the file is connected, asks again while the parser
 * says it is not connected, and is told to stop before the disconnect. */
struct reader
{
    wg_parser_t parser;
    unsigned int reads;
    int stop;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    BOOL held;
};

static void *reader_main(void *arg)
{
    struct reader *reader = arg;

    while (!__atomic_load_n(&reader->stop, __ATOMIC_SEQ_CST))
    {
        struct wg_parser_get_next_read_offset_params params = {.parser = reader->parser};
        NTSTATUS status = calls[unix_wg_parser_get_next_read_offset](&params);
        UINT32 size;

        if (status)
        {
            ok(status == VFW_E_WRONG_STATE, "the reader was sent away with %#x", (unsigned int)status);
            sched_yield();
            continue;
        }
        ok(params.size && params.offset < file_size && params.size <= file_size - params.offset,
           "asked for %u bytes at %llu of %zu", params.size, (unsigned long long)params.offset, file_size);
        if (want.hold >= 0 && reader->reads >= (unsigned int)want.hold)
        {
            pthread_mutex_lock(&reader->mutex);
            reader->held = TRUE;
            pthread_cond_broadcast(&reader->cond);
            pthread_mutex_unlock(&reader->mutex);
            break;
        }
        reader->reads++;
        size = want.chunk ? min(params.size, want.chunk) : params.size;
        if (want.fail_at >= 0 && params.offset + size > (UINT64)want.fail_at) ok(!do_push(reader->parser, NULL, 0), "push");
        else ok(!do_push(reader->parser, file_data + params.offset, size), "push");
    }
    return NULL;
}

struct clip
{
    wg_parser_t parser;
    struct reader reader;
    pthread_t thread;
    UINT32 count;
    wg_parser_stream_t streams[8];
    int video, audio;
};

static BOOL is_video(const struct wg_format *format)
{
    return format->major_type >= WG_MAJOR_TYPE_VIDEO;
}

static void clip_create(struct clip *c, UINT32 flags)
{
    struct wg_parser_create_params create = {.flags = flags};

    memset(c, 0, sizeof(*c));
    c->video = c->audio = -1;
    ok(!calls[unix_wg_parser_create](&create) && create.parser, "no parser");
    c->parser = c->reader.parser = create.parser;
    pthread_mutex_init(&c->reader.mutex, NULL);
    pthread_cond_init(&c->reader.cond, NULL);
    ok(!pthread_create(&c->thread, NULL, reader_main, &c->reader), "no thread");
}

static void clip_streams(struct clip *c)
{
    struct wg_parser_get_stream_count_params count = {.parser = c->parser};
    struct wg_parser_get_stream_params stream = {.parser = c->parser};
    struct wg_format format;
    unsigned int i;

    ok(!calls[unix_wg_parser_get_stream_count](&count), "the number of streams");
    ok(count.count <= ARRAY_SIZE(c->streams), "%u streams", count.count);
    c->count = count.count;
    for (i = 0; i < c->count; i++)
    {
        stream.index = i;
        ok(!calls[unix_wg_parser_get_stream](&stream) && stream.stream, "stream %u", i);
        c->streams[i] = stream.stream;
        get_format(stream.stream, FALSE, &format);
        if (is_video(&format) && c->video < 0) c->video = i;
        if (!is_video(&format) && c->audio < 0) c->audio = i;
    }
    stream.index = c->count;
    ok(!calls[unix_wg_parser_get_stream](&stream) && !stream.stream, "a stream past the last");
}

static NTSTATUS clip_open(struct clip *c, UINT32 flags)
{
    NTSTATUS status;

    clip_create(c, flags);
    if ((status = do_connect(c->parser, file_size))) return status;
    clip_streams(c);
    return S_OK;
}

static void clip_close(struct clip *c)
{
    __atomic_store_n(&c->reader.stop, 1, __ATOMIC_SEQ_CST);
    ok(!calls[unix_wg_parser_disconnect](&c->parser), "disconnect");
    pthread_join(c->thread, NULL);
    ok(!calls[unix_wg_parser_destroy](&c->parser), "destroy");
    pthread_mutex_destroy(&c->reader.mutex);
    pthread_cond_destroy(&c->reader.cond);
}

/* What came out, in the order it came. */
struct piece
{
    struct wg_parser_buffer info;
    BYTE *data;
};

struct pieces
{
    struct piece *list;
    unsigned int count, room;
};

static void pieces_free(struct pieces *pieces)
{
    unsigned int i;

    for (i = 0; i < pieces->count; i++) free(pieces->list[i].data);
    free(pieces->list);
    memset(pieces, 0, sizeof(*pieces));
}

/* Take buffers until there are no more: of every stream, earliest first, when
 * which is -1, of one stream otherwise. Each is taken the way the Windows
 * side takes it, and the rules of taking it are held on the way. */
static void pull(struct clip *c, int which, struct pieces *out)
{
    wg_parser_stream_t handle = which < 0 ? 0 : c->streams[which], stream;
    struct wg_parser_buffer info, again;
    NTSTATUS status;
    UINT32 half;
    BYTE *data;

    pieces_free(out);
    for (;;)
    {
        memset(&info, 0xcc, sizeof(info));
        if ((status = do_get_buffer(c->parser, handle, &info)) == S_FALSE) break;
        ok(status == S_OK, "get_buffer says %#x", (unsigned int)status);
        ok(info.stream < c->count && (which < 0 || info.stream == (UINT32)which),
           "a buffer of stream %u, asked for %d of %u", info.stream, which, c->count);
        ok(info.size && info.size <= LOW_TOTAL - LOW_DATA, "a buffer of %u bytes", info.size);
        ok(!do_get_buffer(c->parser, handle, &again) && !memcmp(&info, &again, sizeof(info)),
           "a buffer that was not released is not offered again");
        stream = c->streams[info.stream];
        ok((data = malloc(info.size)), "no memory");
        half = info.size / 2;
        ok(!do_copy(stream, data, 0, half), "the first half of a buffer");
        ok(!do_copy(stream, data + half, half, info.size - half), "the second half of a buffer");
        ok(do_copy(stream, data, info.size, 0) == STATUS_INVALID_PARAMETER, "a copy from the end of a buffer");
        ok(do_copy(stream, data, 1, info.size) == STATUS_INVALID_PARAMETER, "a copy past the end of a buffer");
        ok(!calls[unix_wg_parser_stream_release_buffer](&stream), "release");
        ok(do_copy(stream, data, 0, 1) == VFW_E_WRONG_STATE, "a copy of a released buffer");
        if (out->count == out->room)
        {
            out->room = out->room ? out->room * 2 : 64;
            ok((out->list = realloc(out->list, out->room * sizeof(*out->list))), "no memory");
        }
        out->list[out->count].info = info;
        out->list[out->count++].data = data;
    }
    ok(do_get_buffer(c->parser, handle, &info) == S_FALSE, "the end is not the end when asked again");
}

static void seek(struct clip *c, int index, UINT64 start, DWORD start_flags, UINT64 stop, DWORD stop_flags)
{
    struct wg_parser_stream_seek_params params = {c->streams[index], 1.0, start, stop, start_flags, stop_flags};

    moved = TRUE;
    ok(!calls[unix_wg_parser_stream_seek](&params), "seek");
}

static void rewind_clip(struct clip *c)
{
    seek(c, 0, 0, AM_SEEKING_AbsolutePositioning, 0, AM_SEEKING_NoPositioning);
}

static UINT64 duration_of(struct clip *c, int index)
{
    struct wg_parser_stream_get_duration_params params = {.stream = c->streams[index]};

    ok(!calls[unix_wg_parser_stream_get_duration](&params), "duration");
    return params.duration;
}

/* The buffers of all streams come in the order of their time, and the first
 * one a stream gives after it was opened or moved says so. Packets of MPEG
 * pictures come in the order they are decoded in, which is not the order they
 * are shown in, and not each has a time: such a stream's packets are then
 * within a few pictures of their place among the other streams' buffers. */
static void check_order(const struct clip *c, const struct pieces *all)
{
    BOOL seen[ARRAY_SIZE(c->streams)] = {0};
    UINT64 last[ARRAY_SIZE(c->streams)] = {0}, slack = unordered ? SECOND / 5 : 0;
    unsigned int i, j;

    for (i = 0; i < all->count; i++)
    {
        const struct wg_parser_buffer *info = &all->list[i].info;

        ok(info->discontinuity == !seen[info->stream], "buffer %u of stream %u: discontinuity %u", i, info->stream,
           info->discontinuity);
        seen[info->stream] = TRUE;
        ok(!info->preroll, "buffer %u is marked as preroll", i);
        if (!info->has_pts) continue;
        for (j = 0; j < ARRAY_SIZE(last); j++)
        {
            if (j == info->stream && unordered) continue;
            ok(info->pts + slack >= last[j], "buffer %u of stream %u at %llu comes after one of stream %u at %llu", i,
               info->stream, (unsigned long long)info->pts, j, (unsigned long long)last[j]);
        }
        last[info->stream] = max(last[info->stream], info->pts);
    }
}

static unsigned int count_of(const struct pieces *all, int stream)
{
    unsigned int i, count = 0;

    for (i = 0; i < all->count; i++) count += all->list[i].info.stream == (UINT32)stream;
    return count;
}

/* The layout Windows gives planar pictures: rows of a multiple of four bytes,
 * an even number of luma rows, then the two chroma planes. */
static void i420_layout(unsigned int width, unsigned int height, unsigned int *luma, unsigned int *chroma,
                        unsigned int *offset_u, unsigned int *offset_v, unsigned int *size)
{
    *luma = (width + 3) & ~3u;
    *chroma = ((width + 1) / 2 + 3) & ~3u;
    *offset_u = *luma * ((height + 1) & ~1u);
    *offset_v = *offset_u + *chroma * ((height + 1) / 2);
    *size = *offset_v + *chroma * ((height + 1) / 2);
}

static size_t ref_frame_size(void)
{
    return (size_t)want.width * want.height + 2 * (size_t)((want.width + 1) / 2) * ((want.height + 1) / 2);
}

/* One plane of a picture against the other build's. They are equal, but
 * where the decoder gives RGB or a palette: there each build turns it into
 * planes its own way, and they are within a level and a half on average. */
static void check_plane(const BYTE *data, unsigned int stride, const BYTE *ref, unsigned int width, unsigned int height,
                        unsigned int frame, const char *name)
{
    unsigned int x, y, off = 0;

    for (y = 0; y < height; y++)
    {
        if (!want.close) ok(!memcmp(data + y * stride, ref + y * width, width), "frame %u, %s row %u differs", frame, name, y);
        else for (x = 0; x < width; x++) off += abs(data[y * stride + x] - ref[y * width + x]);
    }
    ok(off <= width * height * 3 / 2, "frame %u, %s is %.2f levels off on average", frame, name, off / (double)(width * height));
}

/* One picture against the other build's decoding of the same frame. */
static void check_picture(const struct piece *piece, unsigned int frame)
{
    unsigned int luma, chroma, offset_u, offset_v, size, half_w = (want.width + 1) / 2, half_h = (want.height + 1) / 2;
    const BYTE *ref = ref_yuv + frame * ref_frame_size();

    i420_layout(want.width, want.height, &luma, &chroma, &offset_u, &offset_v, &size);
    ok(piece->info.size == size, "frame %u is %u bytes, a %ux%u picture takes %u", frame, piece->info.size,
       want.width, want.height, size);
    ok((frame + 1) * ref_frame_size() <= ref_yuv_size, "frame %u is past the %zu the file has", frame,
       ref_yuv_size / ref_frame_size());
    check_plane(piece->data, luma, ref, want.width, want.height, frame, "luma");
    ref += (size_t)want.width * want.height;
    check_plane(piece->data + offset_u, chroma, ref, half_w, half_h, frame, "U");
    ref += (size_t)half_w * half_h;
    check_plane(piece->data + offset_v, chroma, ref, half_w, half_h, frame, "V");
}

/* The time of the file's first picture, learned the first time it is played. */
static UINT64 video_origin;

static unsigned int frame_of(const struct wg_parser_buffer *info)
{
    INT64 offset = (INT64)info->pts - (INT64)video_origin;
    INT64 frame = (offset + frame_time / 2) / frame_time;

    ok(info->has_pts, "a picture without a time");
    ok(offset >= -MILLISECOND && llabs(offset - frame * frame_time) <= MILLISECOND,
       "a picture at %lld, which is no multiple of %lld", (long long)offset, (long long)frame_time);
    return frame;
}

/* The pictures in what came out: each is the frame its time says, they follow
 * each other without a gap, and each lasts one frame. Returns how many there
 * were and the number of the first. The last of a file that was cut short may
 * be anything. */
static unsigned int check_pictures(const struct clip *c, const struct pieces *all, unsigned int *first)
{
    unsigned int i, frame, count = 0, total = count_of(all, c->video);

    for (i = 0; i < all->count; i++)
    {
        const struct piece *piece = &all->list[i];

        if (piece->info.stream != (UINT32)c->video) continue;
        frame = frame_of(&piece->info);
        if (!count) *first = frame;
        ok(frame == *first + count, "frame %u follows frame %u", frame, *first + count - 1);
        ok(piece->info.has_duration && llabs((INT64)piece->info.duration - frame_time) <= MILLISECOND,
           "frame %u lasts %llu, a frame is %lld", frame, (unsigned long long)piece->info.duration, (long long)frame_time);
        if (!want.partial || count + 1 < total) check_picture(piece, frame);
        count++;
    }
    return count;
}

/* The sound in what came out, joined: the buffers follow each other in time
 * without a gap. */
static BYTE *join_sound(const struct clip *c, const struct pieces *all, unsigned int sample, unsigned int rate,
                        size_t *size, UINT64 *start)
{
    UINT64 next = 0;
    BYTE *data = NULL;
    unsigned int i;

    *size = 0;
    for (i = 0; i < all->count; i++)
    {
        const struct piece *piece = &all->list[i];
        INT64 length;

        if (piece->info.stream != (UINT32)c->audio) continue;
        ok(!(piece->info.size % sample), "sound of %u bytes in samples of %u", piece->info.size, sample);
        length = (INT64)(piece->info.size / sample) * SECOND / rate;
        ok(piece->info.has_pts && piece->info.has_duration, "sound without a time or a length");
        ok(llabs((INT64)piece->info.duration - length) <= MILLISECOND, "%u bytes of sound last %llu", piece->info.size,
           (unsigned long long)piece->info.duration);
        if (!*size) *start = piece->info.pts;
        else ok(llabs((INT64)piece->info.pts - (INT64)next) <= MILLISECOND, "sound at %llu follows sound that ended at %llu",
                (unsigned long long)piece->info.pts, (unsigned long long)next);
        next = piece->info.pts + piece->info.duration;
        ok((data = realloc(data, *size + piece->info.size)), "no memory");
        memcpy(data + *size, piece->data, piece->info.size);
        *size += piece->info.size;
    }
    return data;
}

static void check_sound(const struct clip *c, const struct pieces *all)
{
    size_t size, i, same;
    UINT64 start = 0;
    BYTE *data;

    data = join_sound(c, all, 2 * want.channels, want.rate, &size, &start);
    if (want.partial)
    {
        same = min(size, ref_pcm_size) * 4 / 5;
        ok(size <= ref_pcm_size + 65536, "%zu bytes of sound from a file cut short, the whole has %zu", size, ref_pcm_size);
    }
    else
    {
        same = ref_pcm_size;
        ok(size == ref_pcm_size, "%zu bytes of sound, the file has %zu", size, ref_pcm_size);
        ok(start <= video_origin + SECOND / 2, "sound starts at %llu", (unsigned long long)start);
    }
    for (i = moved ? (size_t)want.prime * 2 * want.channels : 0; i < same; i++)
        ok(data[i] == ref_pcm[i], "sound differs at byte %zu of %zu: %u, the file has %u", i, size, data[i], ref_pcm[i]);
    free(data);
}

static const char *video_type_name(const struct wg_format *format)
{
    switch (format->major_type)
    {
        case WG_MAJOR_TYPE_VIDEO: return "none";
        case WG_MAJOR_TYPE_VIDEO_CINEPAK: return "cinepak";
        case WG_MAJOR_TYPE_VIDEO_MPEG1: return "mpeg1";
        case WG_MAJOR_TYPE_VIDEO_H264: return "h264";
        case WG_MAJOR_TYPE_VIDEO_INDEO: return "indeo";
        case WG_MAJOR_TYPE_VIDEO_WMV:
            switch (format->u.video.format)
            {
                case WG_VIDEO_FORMAT_WMV1: return "wmv1";
                case WG_VIDEO_FORMAT_WMV2: return "wmv2";
                case WG_VIDEO_FORMAT_WMV3: return "wmv3";
                default: return "wmv?";
            }
        default: return "?";
    }
}

static const char *audio_type_name(const struct wg_format *format)
{
    switch (format->major_type)
    {
        case WG_MAJOR_TYPE_AUDIO: return "none";
        case WG_MAJOR_TYPE_AUDIO_MPEG4: return "aac";
        case WG_MAJOR_TYPE_AUDIO_MPEG1:
            return format->u.audio.layer == 1 ? "mpeg1-1" : format->u.audio.layer == 2 ? "mpeg1-2" : "mpeg1-3";
        case WG_MAJOR_TYPE_AUDIO_WMA:
            return format->u.audio.version == 1 ? "wma1" : format->u.audio.version == 2 ? "wma2" : "wma?";
        default: return "?";
    }
}

static void check_tag(struct clip *c, int index, wg_parser_tag tag, const char *value)
{
    char buffer[256];
    UINT32 size = sizeof(buffer);
    NTSTATUS status;

    memset(buffer, 0xcc, sizeof(buffer));
    status = do_tag(c->streams[index], tag, buffer, &size);
    if (!value)
    {
        ok(status == STATUS_NOT_FOUND, "tag %u of stream %d is there: %#x", (unsigned int)tag, index, (unsigned int)status);
        return;
    }
    ok(!status && !strcmp(buffer, value), "tag %u of stream %d: %#x, \"%.32s\", the file says \"%s\"", (unsigned int)tag,
       index, (unsigned int)status, status ? "" : buffer, value);
    size = strlen(value);
    ok(do_tag(c->streams[index], tag, buffer, &size) == STATUS_BUFFER_TOO_SMALL && size == strlen(value) + 1,
       "a tag that does not fit asks for %u bytes", size);
}

/* What the file says it holds, before anything is played. */
static void check_streams(struct clip *c, BOOL compressed)
{
    struct wg_format format, codec;
    UINT32 size = 16;
    char buffer[16];
    unsigned int i;

    ok(c->count == want.streams, "%u streams, the file has %u", c->count, want.streams);
    ok((c->video >= 0) == !!want.width && (c->audio >= 0) == !!want.rate, "video stream %d, audio stream %d", c->video,
       c->audio);
    for (i = 0; i < c->count; i++)
    {
        INT64 duration = duration_of(c, i);
        BOOL video = (int)i == c->video;
        const char *type = video ? want.vtype : want.atype;
        const char *name;

        get_format(c->streams[i], FALSE, &format);
        get_format(c->streams[i], TRUE, &codec);
        name = video ? video_type_name(&codec) : audio_type_name(&codec);
        ok(!strcmp(name, type), "stream %u is of codec type %s, the file is %s", i, name, type);
        if (!compressed || !strcmp(type, "none"))
        {
            if (video)
                ok(format.major_type == WG_MAJOR_TYPE_VIDEO && format.u.video.format == WG_VIDEO_FORMAT_I420,
                   "stream %u gives type %u, format %u", i, format.major_type, format.u.video.format);
            else
                ok(format.major_type == WG_MAJOR_TYPE_AUDIO
                   && (compressed || format.u.audio.format == WG_AUDIO_FORMAT_S16LE),
                   "stream %u gives type %u, format %u", i, format.major_type, format.u.audio.format);
        }
        else ok(!memcmp(&format, &codec, sizeof(format)), "stream %u does not give what it holds", i);
        if (video)
        {
            ok(format.u.video.width == (int)want.width && format.u.video.height == (int)want.height,
               "a picture of %dx%d", (int)format.u.video.width, (int)format.u.video.height);
            ok(format.u.video.fps_n * (UINT64)want.fps_d == format.u.video.fps_d * (UINT64)want.fps_n,
               "%u/%u pictures a second", format.u.video.fps_n, format.u.video.fps_d);
            ok(codec.u.video.width == (int)want.width && codec.u.video.height == (int)want.height,
               "holds a picture of %dx%d", (int)codec.u.video.width, (int)codec.u.video.height);
        }
        else
        {
            ok(format.u.audio.rate == want.rate && format.u.audio.channels == want.channels, "%u Hz, %u channels",
               format.u.audio.rate, format.u.audio.channels);
            if (format.major_type == WG_MAJOR_TYPE_AUDIO)
                ok(format.u.audio.channel_mask == (want.channels == 1 ? 4u : 3u), "speakers %#x", format.u.audio.channel_mask);
            ok(codec.u.audio.rate == want.rate && codec.u.audio.channels == want.channels, "holds %u Hz, %u channels",
               codec.u.audio.rate, codec.u.audio.channels);
            if (codec.major_type == WG_MAJOR_TYPE_AUDIO_WMA)
                ok(codec.u.audio.block_align && codec.u.audio.bitrate && codec.u.audio.depth == 16
                   && codec.u.audio.codec_data_len >= 4, "a WMA stream of block %u, %u bits a second, %u bytes of setup",
                   codec.u.audio.block_align, codec.u.audio.bitrate, codec.u.audio.codec_data_len);
            if (codec.major_type == WG_MAJOR_TYPE_AUDIO_MPEG4)
                ok(codec.u.audio.codec_data_len > sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX),
                   "an AAC stream with %u bytes of setup", codec.u.audio.codec_data_len);
        }
        /* What a file that was cut short says of its length is the demuxer's guess. */
        if (!want.partial)
            ok(fabs(duration / (double)SECOND - want.duration) <= 0.15, "stream %u lasts %.3f s, the file %.3f s", i,
           duration / (double)SECOND, want.duration);
        check_tag(c, i, WG_PARSER_TAG_LANGUAGE, video ? want.vlang : want.alang);
        check_tag(c, i, WG_PARSER_TAG_NAME, video ? want.vname : want.aname);
        ok(do_tag(c->streams[i], WG_PARSER_TAG_COUNT, buffer, &size) == STATUS_INVALID_PARAMETER, "a tag there is not");
    }
}

/* Everything, once, as it comes. */
static void check_whole(struct clip *c, const struct pieces *all)
{
    unsigned int first = 0, frames;

    check_order(c, all);
    if (c->video >= 0)
    {
        frames = check_pictures(c, all, &first);
        ok(!first, "the first picture is frame %u", first);
        if (want.partial) ok(frames && frames <= want.frames, "%u pictures from a file cut short of %u", frames, want.frames);
        else ok(frames == want.frames, "%u pictures, the file has %u", frames, want.frames);
    }
    if (c->audio >= 0) check_sound(c, all);
}

static void first_picture_time(const struct clip *c, const struct pieces *all)
{
    unsigned int i;

    video_origin = 0;
    for (i = 0; i < all->count; i++)
    {
        if (all->list[i].info.stream != (UINT32)c->video) continue;
        if (unordered && !all->list[i].info.has_pts) return;
        ok(all->list[i].info.has_pts, "the first picture has no time");
        video_origin = all->list[i].info.pts;
        ok(want.nokeys || !all->list[i].info.delta, "the first picture is not a key frame");
        return;
    }
}

static BOOL first_delta(const struct clip *c, const struct pieces *all)
{
    unsigned int i;

    for (i = 0; i < all->count; i++)
        if (all->list[i].info.stream == (UINT32)c->video) return all->list[i].info.delta;
    return FALSE;
}

/* Moving in the file: what comes after is the picture that is on screen at
 * that time and every one after it, or from the key frame before it when the
 * program asks for that. */
static void check_seek(struct clip *c, UINT64 time, BOOL key)
{
    int index = c->video >= 0 ? c->video : c->audio;
    unsigned int first = 0, frames, i, wanted;
    struct pieces all = {0};

    seek(c, index, time, AM_SEEKING_AbsolutePositioning | (key ? AM_SEEKING_SeekToKeyFrame : 0), 0, AM_SEEKING_NoPositioning);
    pull(c, -1, &all);
    check_order(c, &all);
    if (c->video >= 0)
    {
        frames = check_pictures(c, &all, &first);
        wanted = (time - video_origin) / frame_time;
        if (time >= video_origin + want.frames * (UINT64)frame_time)
            ok(!frames, "%u pictures after the end, from frame %u", frames, first);
        else
        {
            if (key && frames) ok(want.nokeys || !first_delta(c, &all), "the first picture after the move is not a key frame");
            ok(frames && first + frames == want.frames, "%u pictures from frame %u, the file has %u", frames, first,
               want.frames);
            if (key) ok(first <= wanted + want.late, "the key frame before frame %u is not frame %u", wanted, first);
            else ok(first >= wanted && first <= wanted + want.late, "frame %u is on screen at that time, frame %u came",
                    wanted, first);
        }
    }
    if (c->audio >= 0)
    {
        for (i = 0; i < all.count && all.list[i].info.stream != (UINT32)c->audio; i++) ;
        if (time >= (UINT64)(want.length * SECOND))
            ok(i == all.count || all.list[i].info.pts + all.list[i].info.duration + SECOND / 2 >= time,
               "sound from %llu after a move past the end", (unsigned long long)all.list[i].info.pts);
        else
        {
            const struct wg_parser_buffer *info;

            ok(i < all.count, "no sound after the move");
            info = &all.list[i].info;
            if (!key) ok(info->pts + info->duration > time, "sound that ended at %llu, before the place moved to",
                         (unsigned long long)(info->pts + info->duration));
            ok(info->pts <= time + SECOND / 2, "sound only from %llu", (unsigned long long)info->pts);
        }
    }
    pieces_free(&all);
}

/* A stop position: nothing at or after it comes out. */
static void check_stop(struct clip *c, UINT64 stop)
{
    int index = c->video >= 0 ? c->video : c->audio;
    unsigned int first = 0, frames, i, before = 0;
    struct pieces all = {0};
    UINT64 exact = 0;

    seek(c, index, 0, AM_SEEKING_AbsolutePositioning, stop, AM_SEEKING_AbsolutePositioning);
    pull(c, -1, &all);
    check_order(c, &all);
    ok(all.count, "nothing before the stop position");
    for (i = 0; i < all.count; i++)
        ok(all.list[i].info.pts < stop, "a buffer at %llu, the stop position is %llu",
           (unsigned long long)all.list[i].info.pts, (unsigned long long)stop);
    if (c->video >= 0)
    {
        frames = check_pictures(c, &all, &first);
        ok(!first && frames == (stop - video_origin + frame_time - 1) / frame_time, "%u pictures from frame %u before %llu",
           frames, first, (unsigned long long)stop);
    }
    /* A buffer that starts at the stop position is past it. */
    for (i = 0; i < all.count; i++)
        if (all.list[i].info.stream == (UINT32)index) exact = max(exact, all.list[i].info.pts);
    for (i = 0; i < all.count; i++)
        if (all.list[i].info.stream == (UINT32)index && all.list[i].info.pts < exact) before++;
    if (before)
    {
        seek(c, index, 0, AM_SEEKING_AbsolutePositioning, exact, AM_SEEKING_AbsolutePositioning);
        pull(c, -1, &all);
        for (i = 0; i < all.count; i++)
            ok(all.list[i].info.pts < exact, "a buffer at %llu, the stop position is %llu",
               (unsigned long long)all.list[i].info.pts, (unsigned long long)exact);
        ok(count_of(&all, index) == before, "%u buffers of stream %d before the one at the stop position, there are %u",
           count_of(&all, index), index, before);
    }
    /* The stream's own length as the stop position means to the end. */
    seek(c, index, 0, AM_SEEKING_AbsolutePositioning, duration_of(c, index), AM_SEEKING_AbsolutePositioning);
    pull(c, -1, &all);
    check_whole(c, &all);
    pieces_free(&all);
}

/* One stream at a time: what the other stream had in the file meanwhile is
 * kept for it. */
static void check_one_by_one(struct clip *c)
{
    struct pieces all = {0};
    unsigned int i;

    for (i = 0; i < c->count; i++)
    {
        if (!i) rewind_clip(c);
        pull(c, i, &all);
        ok(all.count, "nothing from stream %u alone", i);
        if ((int)i == c->video || (int)i == c->audio)
        {
            int video = c->video, audio = c->audio;
            if ((int)i == video) c->audio = -1;
            else c->video = -1;
            check_whole(c, &all);
            c->video = video;
            c->audio = audio;
        }
    }
    pieces_free(&all);
}

/* A stream that is switched off gives nothing and holds nothing up. */
static void check_disable(struct clip *c)
{
    struct wg_format format;
    struct pieces all = {0};
    int audio = c->audio;

    if (c->video < 0 || c->audio < 0) return;
    ok(!calls[unix_wg_parser_stream_disable](&c->streams[audio]), "disable");
    rewind_clip(c);
    pull(c, -1, &all);
    ok(!count_of(&all, audio), "%u buffers of a stream that is off", count_of(&all, audio));
    c->audio = -1;
    check_whole(c, &all);
    c->audio = audio;
    pull(c, audio, &all);
    ok(!all.count, "%u buffers when asking a stream that is off", all.count);
    get_format(c->streams[audio], FALSE, &format);
    do_enable(c->streams[audio], &format);
    /* What the file had for it meanwhile was not kept: the file is at its
     * end, and so is the stream. */
    pull(c, -1, &all);
    ok(!all.count, "%u buffers kept for a stream while it was off", all.count);
    rewind_clip(c);
    pull(c, -1, &all);
    check_whole(c, &all);
    pieces_free(&all);
}

static double level(double value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* How far two planes of colour are apart on average, in levels. */
static double plane_distance(const BYTE *data, const BYTE *ref, unsigned int count)
{
    unsigned int i, off = 0;

    for (i = 0; i < count; i++) off += abs(data[i] - ref[i]);
    return off / (double)count;
}

/* One picture in another shape against the same frame: the planar shapes and
 * the brightness of the packed ones against the other build's planes, the
 * colour of the packed ones near them (they hold it for every row or every
 * point, the file for every other), and the shapes of red, green and blue
 * against the BGRx picture, which was held against the file's colours. */
static void check_shape(wg_video_format shape, const char *name, const struct piece *piece, const struct piece *bgrx,
                        unsigned int frame)
{
    unsigned int width = want.width, height = want.height, half_w = (width + 1) / 2, half_h = (height + 1) / 2;
    const BYTE *ref = ref_yuv + frame * ref_frame_size(), *ref_u = ref + width * height, *ref_v = ref_u + half_w * half_h;
    unsigned int luma, chroma, offset_u, offset_v, size, stride = 0, bytes = 0, x, y, at_y = 0, at_u = 0, at_v = 0, step = 0;
    const BYTE *data = piece->data, *row, *other;
    BYTE *bright, *u, *v, *near_u, *near_v;
    double off_u, off_v;

    i420_layout(width, height, &luma, &chroma, &offset_u, &offset_v, &size);
    ok((bright = malloc(width * height)) && (u = malloc(width * height)) && (v = malloc(width * height))
       && (near_u = malloc(width * height)) && (near_v = malloc(width * height)), "no memory");
    switch (shape)
    {
        case WG_VIDEO_FORMAT_YV12:
            ok(piece->info.size == size, "a %s picture of %u bytes, it takes %u", name, piece->info.size, size);
            check_plane(data, luma, ref, width, height, frame, "YV12 luma");
            check_plane(data + offset_u, chroma, ref_v, half_w, half_h, frame, "YV12 V");
            check_plane(data + offset_v, chroma, ref_u, half_w, half_h, frame, "YV12 U");
            break;

        case WG_VIDEO_FORMAT_NV12:
            ok(piece->info.size == offset_u + luma * half_h, "a %s picture of %u bytes, it takes %u", name, piece->info.size,
               offset_u + luma * half_h);
            for (y = 0; y < half_h; y++)
            {
                for (x = 0; x < half_w; x++)
                {
                    u[y * half_w + x] = data[offset_u + y * luma + x * 2];
                    v[y * half_w + x] = data[offset_u + y * luma + x * 2 + 1];
                }
            }
            check_plane(data, luma, ref, width, height, frame, "NV12 luma");
            check_plane(u, half_w, ref_u, half_w, half_h, frame, "NV12 U");
            check_plane(v, half_w, ref_v, half_w, half_h, frame, "NV12 V");
            break;

        case WG_VIDEO_FORMAT_YUY2: at_y = 0; at_u = 1; at_v = 3; step = 2; goto packed;
        case WG_VIDEO_FORMAT_UYVY: at_y = 1; at_u = 0; at_v = 2; step = 2; goto packed;
        case WG_VIDEO_FORMAT_YVYU: at_y = 0; at_u = 3; at_v = 1; step = 2; goto packed;
        case WG_VIDEO_FORMAT_AYUV: at_y = 2; at_u = 1; at_v = 0; step = 4;
        packed:
            /* step bytes a point; colour for each pair of points, or for each. */
            stride = (width * step + 3) & ~3u;
            ok(piece->info.size == stride * height, "a %s picture of %u bytes, it takes %u", name, piece->info.size,
               stride * height);
            for (y = 0; y < height; y++)
            {
                row = data + y * stride;
                for (x = 0; x < width; x++)
                {
                    unsigned int pair = step == 4 ? x * 4 : (x / 2) * 4;

                    bright[y * width + x] = row[x * step + at_y];
                    if (step == 4) ok(row[x * 4 + 3] == 255, "frame %u: %s point %u,%u is see-through: %u", frame, name, x, y,
                                      row[x * 4 + 3]);
                    if (step == 2 && pair + 3 >= width * 2) pair = ((width - 1) / 2) * 4;
                    u[y * width + x] = row[pair + at_u];
                    v[y * width + x] = row[pair + at_v];
                    near_u[y * width + x] = ref_u[(y / 2) * half_w + x / 2];
                    near_v[y * width + x] = ref_v[(y / 2) * half_w + x / 2];
                }
            }
            check_plane(bright, width, ref, width, height, frame, name);
            /* Near the file's, and nowhere near it with the two planes of
             * colour taken for each other. */
            off_u = plane_distance(u, near_u, width * height);
            off_v = plane_distance(v, near_v, width * height);
            ok(off_u < 8 && off_v < 8 && off_u * 3 < plane_distance(u, near_v, width * height)
               && off_v * 3 < plane_distance(v, near_u, width * height),
               "frame %u: the colour of the %s picture is %.2f and %.2f levels off the file's, %.2f and %.2f off the other plane",
               frame, name, off_u, off_v, plane_distance(u, near_v, width * height), plane_distance(v, near_u, width * height));
            break;

        case WG_VIDEO_FORMAT_BGRA:
        case WG_VIDEO_FORMAT_RGBA:
        case WG_VIDEO_FORMAT_BGR:
            bytes = shape == WG_VIDEO_FORMAT_BGR ? 3 : 4;
            stride = (width * bytes + 3) & ~3u;
            ok(piece->info.size == stride * height, "a %s picture of %u bytes, it takes %u", name, piece->info.size,
               stride * height);
            for (y = 0; y < height; y++)
            {
                for (x = 0; x < width; x++)
                {
                    row = data + y * stride + x * bytes;
                    other = bgrx->data + (y * width + x) * 4;
                    if (shape == WG_VIDEO_FORMAT_RGBA)
                        ok(row[0] == other[2] && row[1] == other[1] && row[2] == other[0], "frame %u: %s point %u,%u is %u %u %u,"
                           " as BGRx it is %u %u %u", frame, name, x, y, row[0], row[1], row[2], other[0], other[1], other[2]);
                    else
                        ok(!memcmp(row, other, 3), "frame %u: %s point %u,%u is %u %u %u, as BGRx it is %u %u %u", frame, name,
                           x, y, row[0], row[1], row[2], other[0], other[1], other[2]);
                    if (bytes == 4) ok(row[3] == 255, "frame %u: %s point %u,%u is see-through: %u", frame, name, x, y, row[3]);
                }
            }
            break;

        case WG_VIDEO_FORMAT_RGB15:
        case WG_VIDEO_FORMAT_RGB16:
            stride = (width * 2 + 3) & ~3u;
            ok(piece->info.size == stride * height, "a %s picture of %u bytes, it takes %u", name, piece->info.size,
               stride * height);
            off_u = off_v = 0;
            for (y = 0; y < height; y++)
            {
                for (x = 0; x < width; x++)
                {
                    unsigned int word = data[y * stride + x * 2] | data[y * stride + x * 2 + 1] << 8;
                    int six = shape == WG_VIDEO_FORMAT_RGB16;
                    int red = six ? word >> 11 : (word >> 10) & 31, green = six ? (word >> 5) & 63 : (word >> 5) & 31;
                    int blue = word & 31;

                    /* Five or six bits of each colour, the lowest of them
                     * rounded one way or the other. Where the decoder gives
                     * red, green and blue, FFmpeg goes through planes to get
                     * here and loses colour at edges: near on average then. */
                    other = bgrx->data + (y * width + x) * 4;
                    if (!want.close)
                        ok(abs(red - (other[2] >> 3)) <= 1 && abs(green - (other[1] >> (six ? 2 : 3))) <= 1
                           && abs(blue - (other[0] >> 3)) <= 1, "frame %u: %s point %u,%u is red %d, green %d, blue %d; as BGRx it"
                           " is %u %u %u", frame, name, x, y, red, green, blue, other[2], other[1], other[0]);
                    off_u += abs(red * 8 + 4 - other[2]) + abs(blue * 8 + 4 - other[0]);
                    off_v += abs(red * 8 + 4 - other[0]) + abs(blue * 8 + 4 - other[2]);
                }
            }
            off_u /= 2.0 * width * height;
            off_v /= 2.0 * width * height;
            ok(off_u < 8 && off_u * 3 < off_v, "frame %u: red and blue of the %s picture are %.2f levels off the BGRx one, and"
               " %.2f with one taken for the other", frame, name, off_u, off_v);
            break;

        default:
            ok(0, "shape %u", shape);
    }
    free(bright);
    free(u);
    free(v);
    free(near_u);
    free(near_v);
}

/* Other shapes of picture a program can ask for. */
static void check_picture_formats(struct clip *c)
{
    unsigned int width = want.width, height = want.height, stride = width * 4, x, y, i, n = 0, frames = 0;
    unsigned int luma, chroma, offset_u, offset_v, size, half = (width + 1) / 2, plane = width * height;
    unsigned int quarter = half * ((height + 1) / 2);
    static const struct { wg_video_format shape; const char *name; } shapes[] =
    {
        {WG_VIDEO_FORMAT_BGRA, "BGRA"}, {WG_VIDEO_FORMAT_RGBA, "RGBA"}, {WG_VIDEO_FORMAT_BGR, "BGR"},
        {WG_VIDEO_FORMAT_RGB15, "RGB15"}, {WG_VIDEO_FORMAT_RGB16, "RGB16"}, {WG_VIDEO_FORMAT_AYUV, "AYUV"},
        {WG_VIDEO_FORMAT_YUY2, "YUY2"}, {WG_VIDEO_FORMAT_UYVY, "UYVY"}, {WG_VIDEO_FORMAT_YVYU, "YVYU"},
        {WG_VIDEO_FORMAT_YV12, "YV12"}, {WG_VIDEO_FORMAT_NV12, "NV12"},
    };
    struct wg_format format, natural;
    struct pieces down = {0}, up = {0}, halved = {0}, shaped = {0};
    double error = 0;
    unsigned int j;
    BYTE *turned;

    if (c->video < 0) return;
    get_format(c->streams[c->video], FALSE, &natural);
    format = natural;
    format.u.video.format = WG_VIDEO_FORMAT_BGRx;
    do_enable(c->streams[c->video], &format);
    rewind_clip(c);
    pull(c, c->video, &down);
    format.u.video.height = -(int)height;
    do_enable(c->streams[c->video], &format);
    rewind_clip(c);
    pull(c, c->video, &up);
    ok(down.count == want.frames && up.count == want.frames, "%u and %u pictures as BGRx", down.count, up.count);
    for (i = 0; i < down.count; i++)
    {
        const BYTE *ref = ref_yuv + i * ref_frame_size();

        ok(down.list[i].info.size == stride * height && up.list[i].info.size == stride * height,
           "BGRx pictures of %u and %u bytes", down.list[i].info.size, up.list[i].info.size);
        for (y = 0; y < height; y++)
        {
            const BYTE *row = down.list[i].data + y * stride;

            ok(!memcmp(row, up.list[i].data + (height - 1 - y) * stride, stride),
               "frame %u: a negative height does not turn the picture over at row %u", i, y);
            /* Each point's blue, green and red, in that order, against
             * the file's planes turned into them by the television formula. */
            for (x = 0; x < width; x++, n += 3)
            {
                double luma_at = 1.164 * (ref[y * width + x] - 16);
                double u = ref[plane + (y / 2) * half + x / 2] - 128.0, v = ref[plane + quarter + (y / 2) * half + x / 2] - 128.0;

                error += fabs(row[x * 4] - level(luma_at + 2.017 * u)) + fabs(row[x * 4 + 1] - level(luma_at - 0.392 * u - 0.813 * v))
                         + fabs(row[x * 4 + 2] - level(luma_at + 1.596 * v));
            }
        }
    }
    /* Where the decoder gives RGB, the planes this is held against have lost
     * colour the BGRx picture still has. With red and blue the other way
     * round the pictures are twenty levels and more off. */
    ok(error / n < (want.close ? 8 : 1.5), "BGRx pictures are %.2f levels off the file's colours on average", error / n);

    /* Every other shape, each picture of it. */
    for (j = 0; j < ARRAY_SIZE(shapes); j++)
    {
        format = natural;
        format.u.video.format = shapes[j].shape;
        do_enable(c->streams[c->video], &format);
        rewind_clip(c);
        pull(c, c->video, &shaped);
        ok(shaped.count == want.frames, "%u pictures as %s", shaped.count, shapes[j].name);
        for (i = 0; i < shaped.count; i++) check_shape(shapes[j].shape, shapes[j].name, &shaped.list[i], &down.list[i], i);
    }
    /* The planes turned over as well. */
    format = natural;
    format.u.video.height = -(int)height;
    do_enable(c->streams[c->video], &format);
    rewind_clip(c);
    pull(c, c->video, &shaped);
    ok(shaped.count == want.frames, "%u pictures of planes turned over", shaped.count);
    i420_layout(width, height, &luma, &chroma, &offset_u, &offset_v, &size);
    ok((turned = malloc(size)), "no memory");
    for (i = 0; i < shaped.count; i++)
    {
        struct piece upright = {shaped.list[i].info, turned};

        ok(shaped.list[i].info.size == size, "a picture of planes turned over has %u bytes, it takes %u",
           shaped.list[i].info.size, size);
        for (y = 0; y < height; y++)
            memcpy(turned + y * luma, shaped.list[i].data + (height - 1 - y) * luma, luma);
        for (y = 0; y < (height + 1) / 2; y++)
        {
            memcpy(turned + offset_u + y * chroma, shaped.list[i].data + offset_u + ((height + 1) / 2 - 1 - y) * chroma, chroma);
            memcpy(turned + offset_v + y * chroma, shaped.list[i].data + offset_v + ((height + 1) / 2 - 1 - y) * chroma, chroma);
        }
        check_picture(&upright, i);
    }
    free(turned);
    pieces_free(&shaped);

    /* Half the size, planar again; then something that cannot be given,
     * which changes nothing. */
    format = natural;
    format.u.video.width = width / 2;
    format.u.video.height = height / 2;
    do_enable(c->streams[c->video], &format);
    format.u.video.width = 20000;
    do_enable(c->streams[c->video], &format);
    format = natural;
    format.major_type = WG_MAJOR_TYPE_AUDIO;
    do_enable(c->streams[c->video], &format);
    rewind_clip(c);
    pull(c, c->video, &halved);
    i420_layout(width / 2, height / 2, &luma, &chroma, &offset_u, &offset_v, &size);
    for (i = 0; i < halved.count; i++, frames++)
        ok(halved.list[i].info.size == size, "a %ux%u picture of %u bytes, it takes %u", width / 2, height / 2,
           halved.list[i].info.size, size);
    ok(frames == want.frames, "%u pictures at half the size", frames);

    do_enable(c->streams[c->video], &natural);
    pieces_free(&down);
    pieces_free(&up);
    pieces_free(&halved);
}

/* Other shapes of sound: floats at another rate on one channel, where the
 * test tone must still be the tone, and 24 bits, which are the 16 with a
 * byte below them. */
static void check_sound_formats(struct clip *c)
{
    struct wg_format format, natural;
    struct pieces all = {0};
    double seconds, total = 0, in[2] = {0}, across[2] = {0}, part[2];
    UINT64 start = 0;
    unsigned int j;
    size_t size, i, skip;
    BYTE *data;
    float a;

    if (c->audio < 0 || !want.pcm) return;
    get_format(c->streams[c->audio], FALSE, &natural);

    format = natural;
    format.u.audio.format = WG_AUDIO_FORMAT_F32LE;
    format.u.audio.channels = 1;
    format.u.audio.channel_mask = 4;
    format.u.audio.rate = 44100;
    do_enable(c->streams[c->audio], &format);
    rewind_clip(c);
    pull(c, c->audio, &all);
    data = join_sound(c, &all, 4, 44100, &size, &start);
    seconds = ref_pcm_size / (2.0 * want.channels) / want.rate;
    ok(fabs(size / 4 - seconds * 44100) <= 2, "%zu samples of sound at 44100 Hz, the file has %.3f s: %.1f samples",
       size / 4, seconds, seconds * 44100);
    /* How much of the sound is a 440 Hz tone: all of it, when the rate is
     * right. At 430 Hz there is next to nothing. */
    for (i = 0; i < size / 4; i++)
    {
        memcpy(&a, data + i * 4, 4);
        ok(a >= -1.01f && a <= 1.01f, "a sample of %f", a);
        total += (double)a * a;
        for (j = 0; j < 2; j++)
        {
            double angle = 2 * M_PI * (j ? 430 : 440) * i / 44100.0;
            in[j] += a * cos(angle);
            across[j] += a * sin(angle);
        }
    }
    for (j = 0; j < 2; j++) part[j] = 2 * (in[j] * in[j] + across[j] * across[j]) / (size / 4) / total;
    ok(part[0] > 0.9 && part[1] < 0.2, "%.0f%% of the sound is the file's 440 Hz tone, %.0f%% a 430 Hz one", part[0] * 100,
       part[1] * 100);
    free(data);

    format = natural;
    format.u.audio.format = WG_AUDIO_FORMAT_S24LE;
    do_enable(c->streams[c->audio], &format);
    rewind_clip(c);
    pull(c, c->audio, &all);
    data = join_sound(c, &all, 3 * want.channels, want.rate, &size, &start);
    ok(size / 3 == ref_pcm_size / 2, "%zu samples of 24 bits, the file has %zu", size / 3, ref_pcm_size / 2);
    for (i = 0; i < size / 3; i++)
    {
        int wide = (data[i * 3] | data[i * 3 + 1] << 8 | (signed char)data[i * 3 + 2] * 65536) >> 8;
        int narrow = (short)(ref_pcm[i * 2] | ref_pcm[i * 2 + 1] << 8);
        if (i < (size_t)want.prime * want.channels) continue;
        ok(abs(wide - narrow) <= 1, "sample %zu is %d in 24 bits and %d in 16", i, wide, narrow);
    }
    free(data);

    /* Nothing that cannot be given changes what is given. */
    format = natural;
    format.u.audio.rate = 0;
    do_enable(c->streams[c->audio], &format);
    format = natural;
    format.u.audio.channels = 33;
    do_enable(c->streams[c->audio], &format);
    do_enable(c->streams[c->audio], &natural);
    rewind_clip(c);
    pull(c, c->audio, &all);
    data = join_sound(c, &all, 2 * want.channels, want.rate, &size, &start);
    skip = (size_t)want.prime * 2 * want.channels;
    ok(size == ref_pcm_size && !memcmp(data + skip, ref_pcm + skip, size - skip), "the sound is not what it was after the formats");
    free(data);
    pieces_free(&all);
}

static void play(void)
{
    struct pieces all = {0};
    NTSTATUS status;
    struct clip c;
    UINT64 middle;

    stage = "connecting";
    status = clip_open(&c, want.flags);
    if (status == E_FAIL && want.fail_at >= 0)
    {
        /* A demuxer that reads the end of the file first, for its index,
         * meets the failing read there. */
        printf("%s: refused, a read failed while it was opened\n", file_name);
        clip_close(&c);
        return;
    }
    ok(!status, "the file is refused");
    stage = "the streams";
    check_streams(&c, FALSE);
    stage = "playing it";
    pull(&c, -1, &all);
    first_picture_time(&c, &all);
    check_whole(&c, &all);
    if (!want.partial)
    {
        ok(video_origin <= (want.shift ? 2 * SECOND : SECOND / 10), "the first picture is at %llu",
           (unsigned long long)video_origin);
        middle = video_origin + (UINT64)(want.length * SECOND * 0.55);
        stage = "moving to the middle";
        check_seek(&c, middle, FALSE);
        stage = "moving to the key frame before the middle";
        check_seek(&c, middle, TRUE);
        stage = "moving past the end";
        check_seek(&c, video_origin + (UINT64)((want.length + 1) * SECOND), FALSE);
        stage = "a stop position";
        check_stop(&c, video_origin + (UINT64)(want.length * SECOND * 0.51));
        stage = "one stream at a time";
        check_one_by_one(&c);
        stage = "a stream switched off";
        check_disable(&c);
        stage = "other picture formats";
        check_picture_formats(&c);
        stage = "other sound formats";
        check_sound_formats(&c);
        stage = "playing it again";
        rewind_clip(&c);
        pull(&c, -1, &all);
        check_whole(&c, &all);
    }
    stage = "closing";
    pieces_free(&all);
    clip_close(&c);
}

/* The other build's packets of one stream: a size and a checksum a line. */
struct packet_list
{
    unsigned int count;
    struct { unsigned int size, sum; } *list;
};

static void load_packets(const char *path, struct packet_list *packets)
{
    FILE *file = fopen(path, "r");
    unsigned int size, sum, room = 0;

    ok(file, "cannot read %s", path);
    memset(packets, 0, sizeof(*packets));
    while (fscanf(file, "%u %x", &size, &sum) == 2)
    {
        if (packets->count == room)
        {
            room = room ? room * 2 : 64;
            ok((packets->list = realloc(packets->list, room * sizeof(*packets->list))), "no memory");
        }
        packets->list[packets->count].size = size;
        packets->list[packets->count++].sum = sum;
    }
    fclose(file);
    ok(packets->count, "%s names no packets", path);
}

/* The packets of one stream in what came out are the file's from some packet
 * on, to the last. Returns which. */
static unsigned int check_packets(const struct pieces *all, int stream, const struct packet_list *packets)
{
    unsigned int i, at = 0, count = count_of(all, stream), first;

    ok(count && count <= packets->count, "%u packets of stream %d, the file has %u", count, stream, packets->count);
    first = at = packets->count - count;
    for (i = 0; i < all->count; i++)
    {
        const struct piece *piece = &all->list[i];
        unsigned int sum;

        if (piece->info.stream != (UINT32)stream) continue;
        sum = av_adler32_update(0, piece->data, piece->info.size);
        ok(piece->info.size == packets->list[at].size && sum == packets->list[at].sum,
           "packet %u of stream %d is %u bytes, sum %08x; the file's is %u bytes, sum %08x", at, stream,
           piece->info.size, sum, packets->list[at].size, packets->list[at].sum);
        at++;
    }
    return first;
}

/* The bytes of one stream in what came out, joined. */
static BYTE *join_bytes(const struct pieces *all, int stream, size_t *size)
{
    BYTE *data = NULL;
    unsigned int i;

    *size = 0;
    for (i = 0; i < all->count; i++)
    {
        if (all->list[i].info.stream != (UINT32)stream) continue;
        ok((data = realloc(data, *size + all->list[i].info.size)), "no memory");
        memcpy(data + *size, all->list[i].data, all->list[i].info.size);
        *size += all->list[i].info.size;
    }
    return data;
}

static BYTE *all_samples;
static size_t all_samples_size;

static void check_compressed(struct clip *c, const struct pieces *all, const struct packet_list *video,
                             const struct packet_list *audio, BOOL whole)
{
    int video_index = c->video, audio_index = c->audio;
    unsigned int first;
    size_t size;
    BYTE *data;

    check_order(c, all);
    if (c->video >= 0 && want.vcrc)
    {
        first = check_packets(all, c->video, video);
        if (whole) ok(!first, "the pictures start at packet %u", first);
        c->video = -1;
    }
    if (c->audio >= 0 && want.acrc && want.samples && !whole)
    {
        /* Samples after a move: the end of the file's, from wherever. */
        data = join_bytes(all, c->audio, &size);
        ok(size && size <= all_samples_size && !memcmp(data, all_samples + all_samples_size - size, size),
           "%zu bytes of samples after a move are not the last of the file's %zu", size, all_samples_size);
        free(data);
        c->audio = -1;
    }
    else if (c->audio >= 0 && want.acrc)
    {
        first = check_packets(all, c->audio, audio);
        /* The packet a decoder warms up on lies before the start, and a
         * move to the start does not go back to it. */
        if (whole) ok(first <= (moved && want.prime ? 1u : 0u), "the sound starts at packet %u", first);
        c->audio = -1;
    }
    /* A stream Windows has no name for is decoded here all the same. */
    if (whole) check_whole(c, all);
    c->video = video_index;
    c->audio = audio_index;
}

static void packets(void)
{
    struct packet_list video = {0}, audio = {0};
    struct wg_format format, other;
    struct pieces all = {0}, whole = {0};
    unsigned int i, j, places;
    UINT64 place[8];
    struct clip c;

    if (want.vcrc) load_packets(want.vcrc, &video);
    if (want.acrc) load_packets(want.acrc, &audio);
    unordered = want.vcrc && want.untimed;
    stage = "connecting";
    ok(!clip_open(&c, want.flags | WG_PARSER_CREATE_FLAG_OUTPUT_COMPRESSED), "the file is refused");
    stage = "the streams";
    check_streams(&c, TRUE);
    stage = "its packets";
    pull(&c, -1, &all);
    first_picture_time(&c, &all);
    check_compressed(&c, &all, &video, &audio, TRUE);
    if (c.audio >= 0) all_samples = join_bytes(&all, c.audio, &all_samples_size);
    for (i = 0; i < all.count; i++)
        if ((int)all.list[i].info.stream == c.video && want.vcrc && !want.untimed)
            ok(all.list[i].info.has_pts, "a packet of pictures without a time");

    /* A program cannot ask a stream that hands out packets for another
     * format. */
    stage = "packets after asking for a format";
    for (i = 0; i < c.count; i++)
    {
        if (((int)i == c.video && !want.vcrc) || ((int)i == c.audio && !want.acrc)) continue;
        get_format(c.streams[i], FALSE, &format);
        other = format;
        memset(&other, 0, sizeof(other));
        other.major_type = (int)i == c.video ? WG_MAJOR_TYPE_VIDEO : WG_MAJOR_TYPE_AUDIO;
        other.u.video.format = WG_VIDEO_FORMAT_BGRx;
        if ((int)i == c.audio)
        {
            other.u.audio.format = WG_AUDIO_FORMAT_F32LE;
            other.u.audio.channels = 1;
            other.u.audio.rate = 8000;
        }
        do_enable(c.streams[i], &other);
        get_format(c.streams[i], FALSE, &other);
        ok(!memcmp(&format, &other, sizeof(format)), "stream %u changed its format", i);
    }
    rewind_clip(&c);
    pull(&c, -1, &all);
    check_compressed(&c, &all, &video, &audio, TRUE);

    /* Moving: the packets from a key frame at or before the place. */
    stage = "packets after a move";
    seek(&c, c.video >= 0 ? c.video : c.audio, (UINT64)(want.length * SECOND * 0.55), AM_SEEKING_AbsolutePositioning,
         0, AM_SEEKING_NoPositioning);
    pull(&c, -1, &all);
    check_compressed(&c, &all, &video, &audio, FALSE);
    for (i = 0; i < all.count; i++)
    {
        const struct wg_parser_buffer *info = &all.list[i].info;

        if ((int)info->stream != c.video || !want.vcrc) continue;
        /* Without an index the move lands wherever the time is, and the
         * key frame is the next one. */
        ok(!info->delta, "the first packet of pictures after a move is not a key frame");
        ok(!info->has_pts || info->pts <= video_origin + (UINT64)(want.length * SECOND * 0.55) + MILLISECOND
           + want.late * (UINT64)frame_time,
           "the first packet of pictures after a move is at %llu", (unsigned long long)info->pts);
        break;
    }

    /* To each key frame's own time: the packets from a key frame on, each
     * whole, whether or not reading starts in the middle of a packet. */
    stage = "packets after a move to a key frame";
    rewind_clip(&c);
    pull(&c, -1, &whole);
    for (i = 0, places = 0; i < whole.count && places < ARRAY_SIZE(place); i++)
    {
        const struct wg_parser_buffer *info = &whole.list[i].info;

        if ((int)info->stream == c.video && want.vcrc && !info->delta && info->has_pts) place[places++] = info->pts;
    }
    for (i = 0; i < places; i++)
    {
        seek(&c, c.video, place[i], AM_SEEKING_AbsolutePositioning, 0, AM_SEEKING_NoPositioning);
        pull(&c, -1, &all);
        for (j = 0; j < all.count && (int)all.list[j].info.stream != c.video; j++) ;
        /* Without an index the move may land past the last key frame. */
        if (j == all.count && place[i] + want.late * (UINT64)frame_time >= video_origin + want.frames * (UINT64)frame_time)
            continue;
        ok(j < all.count, "no pictures after a move to the key frame at %llu", (unsigned long long)place[i]);
        check_compressed(&c, &all, &video, &audio, FALSE);
        if (j < all.count)
            ok(!all.list[j].info.delta && (!all.list[j].info.has_pts
                    || all.list[j].info.pts <= place[i] + want.late * (UINT64)frame_time),
               "after a move to the key frame at %llu the first packet is at %llu, key frame: %u",
               (unsigned long long)place[i], (unsigned long long)all.list[j].info.pts, !all.list[j].info.delta);
    }
    pieces_free(&whole);
    stage = "closing";
    pieces_free(&all);
    free(video.list);
    free(audio.list);
    free(all_samples);
    clip_close(&c);
}

/* A file whose streams do not start at zero: the times as they are in the
 * file, and with the earliest start taken off when the program asks. */
static void rebase(void)
{
    struct pieces plain = {0}, rebased = {0}, plain_moved = {0}, moved_to = {0};
    UINT64 shift = 0, least = ~(UINT64)0, start = ~(UINT64)0, place = (UINT64)(want.length * SECOND * 0.55);
    unsigned int i;
    struct clip c;

    stage = "times as in the file";
    ok(!clip_open(&c, want.flags), "the file is refused");
    pull(&c, -1, &plain);
    for (i = 0; i < plain.count; i++) start = min(start, plain.list[i].info.pts);
    seek(&c, 0, start + place, AM_SEEKING_AbsolutePositioning | AM_SEEKING_SeekToKeyFrame, 0, AM_SEEKING_NoPositioning);
    pull(&c, -1, &plain_moved);
    clip_close(&c);
    stage = "times from zero";
    ok(!clip_open(&c, want.flags | WG_PARSER_CREATE_FLAG_PTS_REBASED), "the file is refused");
    pull(&c, -1, &rebased);
    ok(plain.count && plain.count == rebased.count, "%u buffers, and %u when times start at zero", plain.count,
       rebased.count);
    for (i = 0; i < plain.count; i++)
    {
        const struct wg_parser_buffer *a = &plain.list[i].info, *b = &rebased.list[i].info;

        ok(a->stream == b->stream && a->size == b->size && a->has_pts && b->has_pts
           && !memcmp(plain.list[i].data, rebased.list[i].data, a->size), "buffer %u is not the same buffer", i);
        ok(a->pts >= b->pts, "buffer %u moved from %llu to %llu", i, (unsigned long long)a->pts, (unsigned long long)b->pts);
        if (!i) shift = a->pts - b->pts;
        ok(llabs((INT64)(a->pts - b->pts) - (INT64)shift) <= 1, "buffer %u moved by %llu, the first by %llu", i,
           (unsigned long long)(a->pts - b->pts), (unsigned long long)shift);
        least = min(least, b->pts);
    }
    /* Not at zero to the tick: a decoder may hand out nothing for its first packet. */
    ok(least <= SECOND / 10, "with times from zero the earliest buffer is at %llu", (unsigned long long)least);
    if (want.shift) ok(shift >= SECOND / 100, "the file's streams start at %llu", (unsigned long long)shift);
    else ok(!shift, "the file's streams start at zero, and moved by %llu", (unsigned long long)shift);

    /* A place in the file is a time from zero as well: the place that many
     * seconds after the start of the file. */
    stage = "moving to a key frame with times from zero";
    seek(&c, 0, least + place, AM_SEEKING_AbsolutePositioning | AM_SEEKING_SeekToKeyFrame, 0, AM_SEEKING_NoPositioning);
    pull(&c, -1, &moved_to);
    ok(plain_moved.count && plain_moved.count == moved_to.count, "%u buffers after a move, and %u when times start at zero",
       plain_moved.count, moved_to.count);
    for (i = 0; i < moved_to.count; i++)
    {
        const struct wg_parser_buffer *a = &plain_moved.list[i].info, *b = &moved_to.list[i].info;

        ok(a->stream == b->stream && a->size == b->size && llabs((INT64)(a->pts - b->pts) - (INT64)shift) <= 1,
           "buffer %u after a move is at %llu, and at %llu when times start at zero", i, (unsigned long long)a->pts,
           (unsigned long long)b->pts);
    }
    pieces_free(&plain_moved);
    pieces_free(&moved_to);
    if (c.video >= 0)
    {
        unsigned int first = 0, frames;
        struct pieces all = {0};

        stage = "moving with times from zero";
        video_origin = 0;
        for (i = 0; i < rebased.count && (int)rebased.list[i].info.stream != c.video; i++) ;
        ok(i < rebased.count, "no pictures");
        video_origin = rebased.list[i].info.pts;
        seek(&c, c.video, video_origin + 10 * frame_time + frame_time / 2, AM_SEEKING_AbsolutePositioning, 0,
             AM_SEEKING_NoPositioning);
        pull(&c, -1, &all);
        frames = check_pictures(&c, &all, &first);
        ok(first >= 10 && first <= 10 + want.late && first + frames == want.frames, "%u pictures from frame %u", frames,
           first);
        pieces_free(&all);
    }
    stage = "closing";
    clip_close(&c);
    pieces_free(&plain);
    pieces_free(&rebased);
}

/* A file that cannot be played is refused, and the parser is as it was. */
static void broken(void)
{
    struct wg_parser_get_stream_count_params count;
    struct wg_parser_buffer info;
    struct clip c;

    stage = "connecting";
    ok(clip_open(&c, want.flags) == E_FAIL, "the file is taken");
    count.parser = c.parser;
    count.count = 1;
    ok(!calls[unix_wg_parser_get_stream_count](&count) && !count.count, "%u streams in a refused file", count.count);
    ok(do_get_buffer(c.parser, 0, &info) == S_FALSE, "a buffer from a refused file");
    stage = "closing";
    clip_close(&c);
}

/* The program goes away while a read is not answered: nothing waits for the
 * bytes that will not come. The thread that waits in connect or get_buffer
 * meanwhile uses no stream once the disconnect is on its way: the streams
 * go with it, as a program's own threads know. */
struct waiter
{
    struct clip *clip;
    NTSTATUS status;
    unsigned int buffers;
    pthread_mutex_t streams;
    BOOL done, gone;
};

static void *connect_main(void *arg)
{
    struct waiter *waiter = arg;
    struct clip *c = waiter->clip;
    struct wg_parser_buffer info;

    BOOL gone;

    if (!(waiter->status = do_connect(c->parser, file_size)))
    {
        pthread_mutex_lock(&waiter->streams);
        if (!(gone = waiter->gone)) clip_streams(c);
        pthread_mutex_unlock(&waiter->streams);
        while (!gone && do_get_buffer(c->parser, 0, &info) == S_OK)
        {
            pthread_mutex_lock(&waiter->streams);
            if (!(gone = waiter->gone))
            {
                ok(!calls[unix_wg_parser_stream_release_buffer](&c->streams[info.stream]), "release");
                waiter->buffers++;
            }
            pthread_mutex_unlock(&waiter->streams);
        }
    }
    /* Everything was read before the reader stopped answering. */
    pthread_mutex_lock(&c->reader.mutex);
    waiter->done = TRUE;
    pthread_cond_broadcast(&c->reader.cond);
    pthread_mutex_unlock(&c->reader.mutex);
    return NULL;
}

static void abort_read(void)
{
    struct waiter waiter = {0};
    pthread_t thread;
    struct clip c;
    BOOL held;

    stage = "a read that is not answered";
    clip_create(&c, want.flags);
    waiter.clip = &c;
    pthread_mutex_init(&waiter.streams, NULL);
    ok(!pthread_create(&thread, NULL, connect_main, &waiter), "no thread");
    pthread_mutex_lock(&c.reader.mutex);
    while (!c.reader.held && !waiter.done) pthread_cond_wait(&c.reader.cond, &c.reader.mutex);
    held = c.reader.held;
    pthread_mutex_unlock(&c.reader.mutex);
    /* Long enough for the parser to be waiting for the bytes. */
    usleep(20000);
    stage = "disconnecting with a read not answered";
    pthread_mutex_lock(&waiter.streams);
    waiter.gone = TRUE;
    pthread_mutex_unlock(&waiter.streams);
    __atomic_store_n(&c.reader.stop, 1, __ATOMIC_SEQ_CST);
    ok(!calls[unix_wg_parser_disconnect](&c.parser), "disconnect");
    pthread_join(thread, NULL);
    pthread_join(c.thread, NULL);
    ok(waiter.status == S_OK || waiter.status == E_FAIL, "connect says %#x", (unsigned int)waiter.status);
    printf("%s: %s after %u reads, %s, %u buffers\n", file_name, held ? "no answer" : "all read", c.reader.reads,
           waiter.status ? "refused" : "opened", waiter.buffers);
    stage = "closing";
    ok(!calls[unix_wg_parser_destroy](&c.parser), "destroy");
    pthread_mutex_destroy(&waiter.streams);
}

/* The first packet of an MPEG stream after a move, on bytes made here: no
 * file can be told to put a key picture right behind the place a move lands
 * in. */
static void units(void)
{
    static const struct { BYTE header[4]; int size; } frames[] =
    {
        {{0xff, 0xfd, 0xa0, 0x00}, 626},    /* MPEG-1 layer II, 192 kbit/s, 44100 Hz */
        {{0xff, 0xfd, 0xa2, 0x00}, 627},    /* the same with a padding byte */
        {{0xff, 0xfb, 0x90, 0x00}, 417},    /* MPEG-1 layer III, 128 kbit/s, 44100 Hz */
        {{0xff, 0xfb, 0x92, 0x00}, 418},
        {{0xff, 0xff, 0xc8, 0x00}, 576},    /* MPEG-1 layer I, 384 kbit/s, 32000 Hz */
        {{0xff, 0xff, 0xc2, 0x00}, 420},    /* MPEG-1 layer I, 384 kbit/s, 44100 Hz, a padding slot of four bytes */
        {{0xff, 0xf3, 0x80, 0x00}, 208},    /* MPEG-2 layer III, 64 kbit/s, 22050 Hz */
        {{0xff, 0xf5, 0x84, 0x00}, 384},    /* MPEG-2 layer II, 64 kbit/s, 24000 Hz */
        {{0xff, 0xe3, 0x48, 0x00}, 288},    /* MPEG-2.5 layer III, 32 kbit/s, 8000 Hz */
        {{0xff, 0xf7, 0x94, 0x00}, 288},    /* MPEG-2 layer I, 144 kbit/s, 24000 Hz */
        {{0xff, 0xfd, 0x00, 0x00}, 0},      /* no bit rate named */
        {{0xff, 0xfd, 0xf0, 0x00}, 0},      /* a bit rate that is none */
        {{0xff, 0xfd, 0xac, 0x00}, 0},      /* a sample rate that is none */
        {{0xff, 0xf9, 0xa0, 0x00}, 0},      /* a layer that is none */
        {{0xff, 0xed, 0xa0, 0x00}, 0},      /* a version that is none */
        {{0xff, 0xdd, 0xa0, 0x00}, 0},      /* eleven bits are set at the start of a frame */
        {{0xfe, 0xfd, 0xa0, 0x00}, 0},
    };
    static const BYTE slices[] = {0x11, 0x22, 0, 0, 1, 0x05, 0x33, 0, 0, 1, 0xb5, 0x44, 0, 0, 1, 0xb2, 0x55, 0, 0, 1, 0xaf, 0x66};
    static const BYTE starts[] = {0xb3, 0xb8, 0x00};
    struct parser_stream stream;
    BYTE packet[1024];
    const uint8_t *data;
    unsigned int i;
    int size;

    stage = "the length of a frame of MPEG sound";
    for (i = 0; i < ARRAY_SIZE(frames); i++)
        ok(parser_mpeg_audio_frame_size(frames[i].header) == frames[i].size, "header %02x %02x %02x: %d bytes, not %d",
           frames[i].header[0], frames[i].header[1], frames[i].header[2], parser_mpeg_audio_frame_size(frames[i].header),
           frames[i].size);

    stage = "the first packet of MPEG pictures after a move";
    for (i = 0; i < ARRAY_SIZE(starts); i++)
    {
        /* The end of one picture, then the next one: it starts at its
         * sequence header, its group header or its own. */
        memset(packet, 0x77, sizeof(packet));
        memcpy(packet, slices, sizeof(slices));
        packet[sizeof(slices)] = packet[sizeof(slices) + 1] = 0;
        packet[sizeof(slices) + 2] = 1;
        packet[sizeof(slices) + 3] = starts[i];
        memcpy(packet + sizeof(slices) + 40, "\0\0\1\0\0\0\1\xb8\0\0\1\xb3", 12);
        ok(parser_mpeg_picture_start(packet, 200) == (int)sizeof(slices), "start code %02x: the picture starts at %d",
           starts[i], parser_mpeg_picture_start(packet, 200));

        memset(&stream, 0, sizeof(stream));
        stream.video = stream.resync = TRUE;
        data = packet;
        size = 200;
        ok(!parser_resync(&stream, FALSE, &data, &size) && stream.resync, "a picture that needs another is handed out");
        ok(parser_resync(&stream, TRUE, &data, &size) && !stream.resync, "a key picture is not handed out");
        ok(data == packet + sizeof(slices) && size == 200 - (int)sizeof(slices), "start code %02x: the packet starts at %d and has %d bytes",
           starts[i], (int)(data - packet), size);
        /* Only the first one. */
        data = packet;
        size = 200;
        ok(parser_resync(&stream, FALSE, &data, &size) && data == packet && size == 200, "a later packet is cut");
    }
    /* The end of a picture and nothing behind it. */
    memset(&stream, 0, sizeof(stream));
    stream.video = stream.resync = TRUE;
    data = slices;
    size = sizeof(slices);
    ok(parser_mpeg_picture_start(slices, sizeof(slices)) == (int)sizeof(slices), "a picture starts in the end of one");
    ok(!parser_resync(&stream, TRUE, &data, &size) && stream.resync, "the end of a picture is handed out");
    /* A picture from its first byte. */
    memcpy(packet, "\0\0\1\xb3", 4);
    data = packet;
    size = 100;
    ok(parser_resync(&stream, TRUE, &data, &size) && data == packet && size == 100 && !stream.resync, "a whole picture is cut");
    /* A stream that was not moved. */
    data = slices;
    size = sizeof(slices);
    ok(parser_resync(&stream, FALSE, &data, &size) && data == slices && size == (int)sizeof(slices), "a packet is held back");

    stage = "the first packet of MPEG sound after a move";
    /* The end of one frame, then a whole one. */
    memset(packet, 0x55, sizeof(packet));
    memcpy(packet + 100, frames[0].header, 4);
    memset(&stream, 0, sizeof(stream));
    stream.resync = TRUE;
    data = packet;
    size = 100 + 626;
    ok(parser_resync(&stream, FALSE, &data, &size) && !stream.resync, "sound is held back");
    ok(data == packet + 100 && size == 626, "the frame starts at %d and has %d bytes", (int)(data - packet), size);
    /* A frame from its first byte, with what looks like a frame in it. */
    memcpy(packet, frames[2].header, 4);
    stream.resync = TRUE;
    data = packet;
    size = 100 + 626;
    ok(parser_resync(&stream, FALSE, &data, &size) && data == packet && size == 100 + 626 && !stream.resync,
       "a packet that starts with a frame is cut");
    /* Nothing that ends in a frame of its length: left alone. */
    memset(packet, 0x55, 100);
    stream.resync = TRUE;
    data = packet;
    size = 100 + 625;
    ok(parser_resync(&stream, FALSE, &data, &size) && data == packet && size == 100 + 625 && !stream.resync,
       "a packet with no whole frame at its end is cut");
    /* Too short to hold a header. */
    stream.resync = TRUE;
    data = packet;
    size = 3;
    ok(parser_resync(&stream, FALSE, &data, &size) && data == packet && size == 3, "three bytes are cut");
}

int main(int argc, char **argv)
{
    const char *mode;
    int i;

    if (argc == 2 && !strcmp(argv[1], "units"))
    {
        file_name = "no file";
        units();
        return 0;
    }
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s units | play|packets|rebase|broken|abort FILE key=value...\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    file_name = strrchr(argv[2], '/') ? strrchr(argv[2], '/') + 1 : argv[2];
    for (i = 3; i < argc; i++)
    {
        const char *value = strchr(argv[i], '=');
        size_t length = value ? (size_t)(value - argv[i]) : 0;

        ok(value, "argument %s", argv[i]);
        value++;
#define NUMBER(name) if (length == strlen(#name) && !strncmp(argv[i], #name, length)) { want.name = strtoul(value, NULL, 0); continue; }
#define STRING(name) if (length == strlen(#name) && !strncmp(argv[i], #name, length)) { want.name = value; continue; }
        NUMBER(streams) NUMBER(width) NUMBER(height) NUMBER(fps_n) NUMBER(fps_d) NUMBER(frames) NUMBER(rate)
        NUMBER(channels) NUMBER(flags) NUMBER(chunk) NUMBER(wow64) NUMBER(partial) NUMBER(shift)
        NUMBER(close) NUMBER(nokeys) NUMBER(late) NUMBER(prime) NUMBER(untimed) NUMBER(samples)
        STRING(yuv) STRING(pcm) STRING(vcrc) STRING(acrc) STRING(vtype) STRING(atype)
        STRING(vlang) STRING(alang) STRING(vname) STRING(aname)
#undef NUMBER
#undef STRING
        if (!strncmp(argv[i], "fail_at=", 8)) { want.fail_at = strtoll(value, NULL, 0); continue; }
        if (!strncmp(argv[i], "hold=", 5)) { want.hold = atoi(value); continue; }
        if (!strncmp(argv[i], "duration=", 9)) { want.duration = atof(value); continue; }
        if (!strncmp(argv[i], "length=", 7)) { want.length = atof(value); continue; }
        ok(0, "argument %s", argv[i]);
    }
    if (!want.vtype) want.vtype = "none";
    if (!want.atype) want.atype = "none";
    if (want.length < 0) want.length = want.duration;
    if (want.fps_n) frame_time = SECOND * want.fps_d / want.fps_n;
    signal(SIGALRM, hung);
    alarm(120);
    av_log_set_level(AV_LOG_QUIET);
    file_data = load(argv[2], &file_size);
    if (want.yuv) ref_yuv = load(want.yuv, &ref_yuv_size);
    if (want.pcm) ref_pcm = load(want.pcm, &ref_pcm_size);
    if (want.wow64) map_low();

    if (!strcmp(mode, "play")) play();
    else if (!strcmp(mode, "packets")) packets();
    else if (!strcmp(mode, "rebase")) rebase();
    else if (!strcmp(mode, "broken")) broken();
    else if (!strcmp(mode, "abort")) abort_read();
    else ok(0, "mode %s", mode);
    free(file_data);
    free(ref_yuv);
    free(ref_pcm);
    return 0;
}
