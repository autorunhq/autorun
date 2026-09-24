#ifndef WINE_NX_HEAP_SNAPSHOT_H
#define WINE_NX_HEAP_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* devkitPro newlib _mallocr.c: 128 bins, two links per bin. Hold __malloc_lock. */
struct horizon_heap_chunk
{
    size_t previous, size;
    struct horizon_heap_chunk *next, *previous_free;
};
extern struct horizon_heap_chunk *__malloc_av_[258];

static inline int horizon_heap_largest( uintptr_t start, uintptr_t end, size_t holes, size_t *largest )
{
    unsigned int bin;
    size_t visited = 0;

    for (bin = 1; bin < 128; bin++)
    {
        const void *sentinel = (const char *)&__malloc_av_[2 * bin + 2] - 2 * sizeof(size_t);
        struct horizon_heap_chunk *chunk = __malloc_av_[2 * bin + 2];
        while ((const void *)chunk != sentinel)
        {
            struct horizon_heap_chunk item;
            uintptr_t address = (uintptr_t)chunk;
            size_t size;

            if (++visited > holes || address < start || address >= end ||
                end - address < sizeof(item) || address % sizeof(size_t)) return 0;
            memcpy( &item, chunk, sizeof(item) );
            size = item.size & ~(size_t)3;
            if (size < sizeof(item) || size > end - address) return 0;
            if (size > *largest) *largest = size;
            chunk = item.next;
        }
    }
    return 1;
}

#endif
