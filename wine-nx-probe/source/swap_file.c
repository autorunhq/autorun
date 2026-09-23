#include "swap_file.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned int unit_count( size_t size )
{
    return size / SWAP_FILE_UNIT + !!(size % SWAP_FILE_UNIT);
}

int swap_file_open( struct swap_file *file, const char *directory, unsigned int megabytes )
{
    memset( file, 0, sizeof(*file) );
    if (swap_store_open_existing( &file->store, directory, megabytes )) return -1;
    file->units = file->store.size / SWAP_FILE_UNIT;
    file->used = calloc( (file->units + 7) / 8, 1 );
    if (!file->used)
    {
        swap_store_close( &file->store );
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void swap_file_close( struct swap_file *file )
{
    swap_store_close( &file->store );
    free( file->used );
    file->used = NULL;
    file->units = 0;
}

static int occupied( const struct swap_file *file, unsigned int unit )
{
    return !!(file->used[unit / 8] & (1u << (unit % 8)));
}

void swap_file_discard( void *context, unsigned int token, size_t size )
{
    struct swap_file *file = context;
    unsigned int first = token - 1, count = unit_count( size ), i;
    assert( token && size && first < file->units && count <= file->units - first );
    for (i = first; i < first + count; i++)
    {
        assert( occupied( file, i ) );
        file->used[i / 8] &= ~(1u << (i % 8));
    }
    if (first < file->hint) file->hint = first;
}

int swap_file_save( void *context, const void *data, size_t size, unsigned int *token )
{
    struct swap_file *file = context;
    unsigned int count, i, run = 0, first;
    *token = 0;
    if (!size || size > file->store.size || !file->used) { errno = EINVAL; return -1; }
    count = unit_count( size );
    for (i = file->hint; i < file->units; i++)
    {
        run = occupied( file, i ) ? 0 : run + 1;
        if (run == count) break;
    }
    if (i == file->units) { errno = ENOSPC; return -1; }
    first = i + 1 - count;
    for (i = first; i < first + count; i++) file->used[i / 8] |= 1u << (i % 8);
    if (swap_store_write( &file->store, (uint64_t)first * SWAP_FILE_UNIT, data, size ))
    {
        int error = errno;
        swap_file_discard( file, first + 1, size );
        errno = error;
        return -1;
    }
    while (file->hint < file->units && occupied( file, file->hint )) file->hint++;
    *token = first + 1;
    return 0;
}

int swap_file_load( void *context, unsigned int token, void *data, size_t size )
{
    struct swap_file *file = context;
    unsigned int first = token - 1, i, count;
    if (!token || !size || size > file->store.size || first >= file->units)
    { errno = EINVAL; return -1; }
    count = unit_count( size );
    if (count > file->units - first) { errno = EINVAL; return -1; }
    for (i = first; i < first + count; i++)
        if (!occupied( file, i )) { errno = EINVAL; return -1; }
    return swap_store_read( &file->store, (uint64_t)first * SWAP_FILE_UNIT, data, size );
}
