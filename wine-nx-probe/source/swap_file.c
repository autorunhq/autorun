#include "swap_file.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef __SWITCH__
#include <time.h>
#endif

static uint64_t swap_tick(void)
{
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
#endif
}

static void update_max( uint64_t *target, uint64_t value )
{
    uint64_t previous = __atomic_load_n( target, __ATOMIC_RELAXED );
    while (previous < value && !__atomic_compare_exchange_n( target, &previous, value, 0,
                                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
}

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
    __atomic_sub_fetch( &file->used_units, count, __ATOMIC_RELAXED );
    if (first < file->hint) file->hint = first;
}

int swap_file_save( void *context, const void *data, size_t size, unsigned int *token )
{
    struct swap_file *file = context;
    unsigned int count, i, run = 0, first;
    uint64_t tick, elapsed;
    int result;
    *token = 0;
    if (!size || size > file->store.size || !file->used) { errno = EINVAL; return -1; }
    count = unit_count( size );
    tick = swap_tick();
    for (i = file->hint; i < file->units; i++)
    {
        run = occupied( file, i ) ? 0 : run + 1;
        if (run == count) break;
    }
    elapsed = swap_tick() - tick;
    __atomic_add_fetch( &file->scan_units, i - file->hint + (i < file->units), __ATOMIC_RELAXED );
    __atomic_add_fetch( &file->scan_ticks, elapsed, __ATOMIC_RELAXED );
    if (i == file->units)
    {
        __atomic_add_fetch( &file->no_run, 1, __ATOMIC_RELAXED );
        errno = ENOSPC;
        return -1;
    }
    first = i + 1 - count;
    for (i = first; i < first + count; i++) file->used[i / 8] |= 1u << (i % 8);
    __atomic_add_fetch( &file->used_units, count, __ATOMIC_RELAXED );
    tick = swap_tick();
    result = swap_store_write( &file->store, (uint64_t)first * SWAP_FILE_UNIT, data, size );
    elapsed = swap_tick() - tick;
    __atomic_add_fetch( &file->writes, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &file->write_ticks, elapsed, __ATOMIC_RELAXED );
    update_max( &file->max_write_ticks, elapsed );
    if (result)
    {
        int error = errno;
        __atomic_add_fetch( &file->write_errors, 1, __ATOMIC_RELAXED );
        swap_file_discard( file, first + 1, size );
        errno = error;
        return -1;
    }
    __atomic_add_fetch( &file->write_bytes, size, __ATOMIC_RELAXED );
    while (file->hint < file->units && occupied( file, file->hint )) file->hint++;
    *token = first + 1;
    return 0;
}

int swap_file_load( void *context, unsigned int token, void *data, size_t size )
{
    struct swap_file *file = context;
    unsigned int first = token - 1, i, count;
    uint64_t tick, elapsed;
    int result;
    if (!token || !size || size > file->store.size || first >= file->units)
    { errno = EINVAL; return -1; }
    count = unit_count( size );
    if (count > file->units - first) { errno = EINVAL; return -1; }
    for (i = first; i < first + count; i++)
        if (!occupied( file, i )) { errno = EINVAL; return -1; }
    tick = swap_tick();
    result = swap_store_read( &file->store, (uint64_t)first * SWAP_FILE_UNIT, data, size );
    elapsed = swap_tick() - tick;
    __atomic_add_fetch( &file->reads, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &file->read_ticks, elapsed, __ATOMIC_RELAXED );
    update_max( &file->max_read_ticks, elapsed );
    if (result) __atomic_add_fetch( &file->read_errors, 1, __ATOMIC_RELAXED );
    else __atomic_add_fetch( &file->read_bytes, size, __ATOMIC_RELAXED );
    return result;
}
