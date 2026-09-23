#include "../source/swap_ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct visits { unsigned int count, fail_at; uint64_t size, address; };
static int visit( void *context, const void *addr, size_t size )
{
    struct visits *v = context;
    v->count++;
    v->size += size;
    v->address ^= (uintptr_t)addr;
    return v->fail_at == v->count;
}

int main(void)
{
    _Alignas(16) unsigned char message[4096] = {0};
    HipcRequest request = hipcMakeRequestInline( message, .type = 4, .send_pid = 1,
        .num_copy_handles = 2, .num_move_handles = 1, .num_send_statics = 1,
        .num_send_buffers = 1, .num_recv_buffers = 1, .num_exch_buffers = 1,
        .num_recv_statics = HIPC_AUTO_RECV_STATIC, .num_data_words = 16 );
    struct visits v = {0};
    unsigned int i;
    request.send_statics[0] = hipcMakeSendStatic( (void *)0x7654000000ULL, 128, 0 );
    request.send_buffers[0] = hipcMakeBuffer( (void *)0x4555000000ULL, 4096, HipcBufferMode_Normal );
    request.recv_buffers[0] = hipcMakeBuffer( (void *)0x2333000000ULL, 8192, HipcBufferMode_Normal );
    request.exch_buffers[0] = hipcMakeBuffer( (void *)0x3444000000ULL, 16384, HipcBufferMode_Normal );
    request.recv_list[0] = hipcMakeRecvStatic( (void *)0x1222000000ULL, 256 );
    assert( !swap_ipc_visit( message, sizeof(message), visit, &v ) );
    assert( v.count == 5 && v.size == 29056 );
    assert( v.address == (0x7654000000ULL ^ 0x4555000000ULL ^ 0x2333000000ULL ^ 0x3444000000ULL ^ 0x1222000000ULL) );
    for (i = 0; i < (unsigned char *)(request.recv_list + 1) - message; i++)
    {
        v = (struct visits){0};
        assert( swap_ipc_visit( message, i, visit, &v ) == -1 && !v.count );
    }
    v = (struct visits){ .fail_at = 3 };
    assert( swap_ipc_visit( message, sizeof(message), visit, &v ) == -1 && v.count == 3 );
    ((HipcHeader *)message)->recv_list_offset = 128;
    memcpy( message + 512, request.recv_list, sizeof(*request.recv_list) );
    v = (struct visits){0};
    assert( !swap_ipc_visit( message, sizeof(message), visit, &v ) && v.count == 5 );
    ((HipcHeader *)message)->recv_list_offset = 2;
    v = (struct visits){0};
    assert( swap_ipc_visit( message, sizeof(message), visit, &v ) == -1 && !v.count );
    for (i = 0; i < 100000; i++)
    {
        size_t j, size = rand() % sizeof(message);
        for (j = 0; j < size; j++) message[j] = rand();
        v = (struct visits){0};
        swap_ipc_visit( message, size, visit, &v );
        assert( v.count <= 73 );
    }
    puts( "swap IPC: all descriptor types, truncation, explicit receive offsets and fuzz bounds passed" );
}
