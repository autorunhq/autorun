#ifndef WINE_NX_NATIVE_HEAP_H
#define WINE_NX_NATIVE_HEAP_H

#include <stddef.h>

struct _reent;
struct wine_nx_native_heap_stats
{
    size_t reserved, used, free, largest, gap, holes;
    size_t small_free, small_largest, small_holes;
    int complete;
};

void *wine_nx_native_memalign( struct _reent *, size_t alignment, size_t size );
void *wine_nx_native_realloc( struct _reent *, void *pointer, size_t size );
size_t wine_nx_native_heap_free_lower_bound(void);
size_t wine_nx_native_heap_free_estimate( size_t *lower_bound );
size_t wine_nx_native_heap_free_exact(void);
void wine_nx_native_heap_note_small_allocation( size_t size );
void wine_nx_native_heap_stats( struct wine_nx_native_heap_stats *stats );

#endif
