#define _XOPEN_SOURCE 700
#include "../source/swap_file.h"
#include "../../dlls/ntdll/unix/horizon_swap.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fixture
{
    struct swap_file file;
    int mapped, fail_map, fail_unmap, fail_alloc, fail_read, fail_write;
    unsigned int allocations, releases;
};

static int save( void *context, const void *data, size_t size, unsigned int *token )
{
    struct fixture *f = context;
    if (f->fail_write) { errno = EIO; return -1; }
    return swap_file_save( &f->file, data, size, token );
}
static int load( void *context, unsigned int token, void *data, size_t size )
{
    struct fixture *f = context;
    if (f->fail_read) { errno = EIO; return -1; }
    return swap_file_load( &f->file, token, data, size );
}
static void discard( void *context, unsigned int token, size_t size )
{
    swap_file_discard( &((struct fixture *)context)->file, token, size );
}
static int unmap( void *context, void *memory )
{
    struct fixture *f = context;
    assert( memory && f->mapped );
    if (f->fail_unmap) { errno = EBUSY; return -1; }
    f->mapped = 0;
    return 0;
}
static int map( void *context, void *memory )
{
    struct fixture *f = context;
    assert( memory );
    if (f->fail_map) { errno = EIO; return -1; }
    f->mapped = 1;
    return 0;
}
static void *allocate( void *context, size_t size )
{
    struct fixture *f = context;
    if (f->fail_alloc) return NULL;
    f->allocations++;
    return malloc( size );
}
static void release( void *context, void *memory )
{
    struct fixture *f = context;
    assert( !f->mapped );
    f->releases++;
    free( memory );
}

int main(void)
{
    char directory[] = "/tmp/autorun-extents.XXXXXX";
    struct swap_store prepared;
    struct fixture f = {0};
    struct horizon_swap_entry entry = {0};
    const struct horizon_swap_ops ops = { unmap, map, allocate, release };
    const struct horizon_swap_storage storage = { &f, save, load, discard, 4 * 1048576 };
    unsigned int tokens[1024] = {0}, i, token;
    unsigned char input[4096], output[4096];
    void *memory;
    assert( mkdtemp( directory ) );
    assert( swap_file_open( &f.file, directory, 4 ) == -1 && errno == ENOENT );
    assert( !swap_store_open( &prepared, directory, 4, NULL, NULL ) );
    swap_store_close( &prepared );
    assert( !swap_file_open( &f.file, directory, 4 ) );
    for (i = 0; i < 1024; i++)
    {
        memset( input, i, sizeof(input) );
        assert( !swap_file_save( &f.file, input, sizeof(input), &tokens[i] ) );
        assert( tokens[i] == i + 1 );
    }
    assert( swap_file_save( &f.file, input, sizeof(input), &token ) == -1 && errno == ENOSPC );
    for (i = 0; i < 1024; i++)
    {
        memset( input, i, sizeof(input) );
        assert( !swap_file_load( &f.file, tokens[i], output, sizeof(output) ) );
        assert( !memcmp( input, output, sizeof(input) ) );
    }
    for (i = 0; i < 1024; i += 2) swap_file_discard( &f.file, tokens[i], 4096 );
    assert( swap_file_save( &f.file, input, 8192, &token ) == -1 && errno == ENOSPC );
    for (i = 0; i < 1024; i += 2)
    {
        assert( !swap_file_save( &f.file, input, 4096, &token ) && token == tokens[i] );
    }
    for (i = 0; i < 1024; i++) swap_file_discard( &f.file, tokens[i], 4096 );
    assert( swap_file_load( &f.file, tokens[0], output, 4096 ) == -1 && errno == EINVAL );

    memory = allocate( &f, 4096 );
    memset( memory, 0x83, 4096 );
    f.mapped = 1;
    f.fail_unmap = 1;
    assert( horizon_swap_out( &entry, &memory, 4096, &ops, &f, &storage ) == -1 );
    assert( memory && f.mapped && !entry.token && !entry.detached );
    f.fail_unmap = 0;
    f.fail_write = 1;
    assert( horizon_swap_out( &entry, &memory, 4096, &ops, &f, &storage ) == -1 );
    assert( memory && f.mapped && !entry.token && !entry.detached );
    f.fail_map = 1;
    assert( horizon_swap_out( &entry, &memory, 4096, &ops, &f, &storage ) == -1 );
    assert( memory && !f.mapped && entry.detached && !entry.token );
    f.fail_map = f.fail_write = 0;
    assert( !horizon_swap_in( &entry, &memory, 4096, &ops, &f, &storage ) );
    assert( f.mapped && !entry.detached && ((unsigned char *)memory)[14] == 0x83 );
    assert( !horizon_swap_out( &entry, &memory, 4096, &ops, &f, &storage ) );
    assert( !memory && entry.token && entry.detached && !f.mapped );
    f.fail_alloc = 1;
    assert( horizon_swap_in( &entry, &memory, 4096, &ops, &f, &storage ) == -1 && errno == ENOMEM );
    assert( !memory && entry.token );
    f.fail_alloc = 0;
    f.fail_read = 1;
    assert( horizon_swap_in( &entry, &memory, 4096, &ops, &f, &storage ) == -1 );
    assert( !memory && entry.token && f.allocations == f.releases );
    f.fail_read = 0;
    f.fail_map = 1;
    assert( horizon_swap_in( &entry, &memory, 4096, &ops, &f, &storage ) == -1 );
    assert( memory && entry.token && entry.detached );
    /* A partially restored alias may have been modified before the retry. */
    ((unsigned char *)memory)[14] = 0x49;
    f.fail_map = 0;
    assert( !horizon_swap_in( &entry, &memory, 4096, &ops, &f, &storage ) );
    assert( !entry.token && !entry.detached && ((unsigned char *)memory)[14] == 0x49 );
    f.mapped = 0;
    release( &f, memory );
    assert( f.allocations == f.releases );
    for (i = 0; i < f.file.units / 8; i++) assert( !f.file.used[i] );
    swap_file_close( &f.file );
    assert( !swap_store_remove( directory ) && !rmdir( directory ) );
    puts( "swap extents/lifecycle: reuse, fragmentation, disk full and failure rollback passed" );
}
