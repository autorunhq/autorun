#ifndef WINE_NX_NATIVE_HEAP_H
#define WINE_NX_NATIVE_HEAP_H

#include <stddef.h>
#include <stdint.h>

struct _reent;
struct wine_nx_native_heap_stats
{
    size_t reserved, used, free, largest, gap, holes;
    size_t small_free, small_largest, small_holes;
    int complete;
};

void *wine_nx_native_malloc( struct _reent *, size_t size );
void *wine_nx_native_calloc( struct _reent *, size_t count, size_t size );
void *wine_nx_native_memalign( struct _reent *, size_t alignment, size_t size );
void *wine_nx_native_realloc( struct _reent *, void *pointer, size_t size );
void wine_nx_native_free_cached( void *const *pointers, unsigned int count, size_t bytes );
size_t wine_nx_native_heap_free_lower_bound(void);
size_t wine_nx_native_heap_free_estimate( size_t *lower_bound );
size_t wine_nx_native_heap_free_exact(void);
void wine_nx_native_heap_note_small_allocation( size_t size );
void wine_nx_native_heap_stats( struct wine_nx_native_heap_stats *stats );
/* Takes pages a program before this one left lent out of the heap for good:
 * 1 when it did, 0 when they are not where it gives out large blocks, -1 when
 * part of them is given out already. */
int wine_nx_native_heap_keep_out( uintptr_t address, size_t size );
/* The same, only when newlib's heap, which stops below what is taken out, still
 * has small_room bytes to grow in below it; -2, taking nothing, when not. */
int wine_nx_native_heap_keep_out_leaving( uintptr_t address, size_t size, size_t small_room );

#endif
