#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../dlls/winedmo/unix_private.h"

unsigned char __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel) { return 0; }
const char *__wine_dbg_strdup(const char *s)
{
    static char strings[8][1024];
    static unsigned int index;
    char *dst = strings[index++ % 8];
    snprintf(dst, 1024, "%s", s);
    return dst;
}
int __wine_dbg_output(const char *s) { return fputs(s, stderr); }
int __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function) { return -1; }
NTSTATUS WINAPI KeUserModeCallback(ULONG id, const void *args, ULONG size, void **ret, ULONG *ret_size)
{
    const struct dispatch_callback_params *dispatch = args;
    static UINT64 position;
    static ULONG read_size;
    assert(id == NtUserCallDispatchCallback);
    if (dispatch->callback == 1)
    {
        const struct seek_callback_params *p = args;
        struct stream_context *ctx = (void *)(UINT_PTR)p->context;
        assert(!fseek((FILE *)(UINT_PTR)ctx->stream, p->offset, SEEK_SET));
        position = p->offset;
        *ret = &position; *ret_size = sizeof(position);
    }
    else
    {
        const struct read_callback_params *p = args;
        struct stream_context *ctx = (void *)(UINT_PTR)p->context;
        assert(dispatch->callback == 2 && p->size <= ctx->capacity);
        read_size = fread(ctx->buffer, 1, p->size, (FILE *)(UINT_PTR)ctx->stream);
        *ret = &read_size; *ret_size = sizeof(read_size);
    }
    return STATUS_SUCCESS;
}

int main(int argc, char **argv)
{
    struct process_attach_params attach = {1, 2};
    struct stream_context *ctx = calloc(1, sizeof(*ctx) + 32768);
    struct demuxer_create_params create = {.context = ctx};
    unsigned int first[2] = {0};
    FILE *file;
    assert(argc == 2);
    create.url = argv[1];
    file = fopen(argv[1], "rb");
    assert(file && ctx);
    assert(!__wine_unix_call_funcs[unix_process_attach](&attach));
    fseek(file, 0, SEEK_END); ctx->length = ftell(file); rewind(file);
    ctx->stream = (UINT_PTR)file; ctx->capacity = 32768;
    assert(!demuxer_create(&create) && create.stream_count == 2);
    assert(create.duration > 0);
    for (unsigned int i = 0; i < 2; i++)
    {
        BYTE type[4096];
        struct demuxer_stream_type_params p = {.demuxer = create.demuxer, .stream = i};
        assert(demuxer_stream_type(&p) == STATUS_BUFFER_TOO_SMALL);
        assert(p.media_type.format_size <= sizeof(type));
        p.media_type.format = type;
        assert(!demuxer_stream_type(&p));
        if (!i) assert(p.media_type.video->guidFormat.Data1 == MAKEFOURCC('H','2','6','4'));
        else assert(p.media_type.audio->wFormatTag == WAVE_FORMAT_MPEG_HEAAC);
    }
    for (unsigned int pass = 0; pass < 2; pass++)
    {
        unsigned int packets[2] = {0};
        struct demuxer_read_params p = {.demuxer = create.demuxer};
        BYTE *buffer = NULL;
        unsigned int capacity = 0;
        NTSTATUS status;
        for (;;)
        {
            p.sample.size = capacity;
            p.sample.flags = 0;
            status = demuxer_read(&p);
            if (status == STATUS_END_OF_FILE) break;
            if (status == STATUS_BUFFER_TOO_SMALL)
            {
                capacity = p.sample.size;
                buffer = realloc(buffer, capacity);
                assert(buffer);
                p.sample.data = (UINT_PTR)buffer;
                continue;
            }
            assert(!status && p.stream < 2 && p.sample.size <= capacity);
            packets[p.stream]++;
        }
        assert(packets[0] && packets[1]);
        if (!pass) memcpy(first, packets, sizeof(first));
        else assert(!memcmp(first, packets, sizeof(first)));
        printf("demux pass %u: %u video, %u audio packets\n", pass, packets[0], packets[1]);
        free(buffer);
        struct demuxer_seek_params seek = {create.demuxer, 0};
        assert(!demuxer_seek(&seek));
    }
    struct demuxer_destroy_params destroy = {.demuxer = create.demuxer};
    assert(!demuxer_destroy(&destroy) && destroy.context == ctx);
    rewind(file); ctx->length = ctx->size = ctx->position = 0;
    assert(demuxer_create(&create));
    fclose(file); free(ctx);
    return 0;
}
