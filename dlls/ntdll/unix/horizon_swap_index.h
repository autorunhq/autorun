#ifndef HORIZON_SWAP_INDEX_H
#define HORIZON_SWAP_INDEX_H

#include <stdint.h>
#include <stdlib.h>

/* Lazily allocated 4 KiB bitmaps, each covering 128 MiB of guest address space. */
struct horizon_swap_index { uint64_t *regions[4096]; };

static inline uint64_t swap_index_mask( unsigned int first, unsigned int last )
{
    return (UINT64_MAX << first) & (UINT64_MAX >> (63 - last));
}

/* Writers hold mapping_mutex; published bitmaps remain allocated until process exit. */
static inline int swap_index_update( struct horizon_swap_index *index, uintptr_t addr, size_t size, int set )
{
    uint64_t page, end;
    if (!size) return 0;
    if (addr >= (UINT64_C(1) << 39) || size > (UINT64_C(1) << 39) - addr) return -1;
    page = set ? addr >> 12 : (addr + 4095) >> 12;
    end = set ? (addr + size + 4095) >> 12 : (addr + size) >> 12;
    while (page < end)
    {
        unsigned int region = page >> 15, word = (page >> 6) & 511;
        uint64_t next = (page | 63) + 1, mask, *bits;
        if (next > end) next = end;
        bits = __atomic_load_n( &index->regions[region], __ATOMIC_ACQUIRE );
        if (!bits && set)
        {
            if (!(bits = calloc( 512, sizeof(*bits) ))) return -1;
            __atomic_store_n( &index->regions[region], bits, __ATOMIC_RELEASE );
        }
        mask = swap_index_mask( page & 63, (next - 1) & 63 );
        if (bits)
        {
            if (set) __atomic_fetch_or( &bits[word], mask, __ATOMIC_RELEASE );
            else __atomic_fetch_and( &bits[word], ~mask, __ATOMIC_RELEASE );
        }
        page = next;
    }
    return 0;
}

static inline int swap_index_contains( const struct horizon_swap_index *index, uintptr_t addr, size_t size )
{
    uint64_t page, end;
    if (!size) return 0;
    if (size - 1 > UINTPTR_MAX - addr) return 1;
    if (addr >= (UINT64_C(1) << 39)) return 0;
    if (size > (UINT64_C(1) << 39) - addr) size = (UINT64_C(1) << 39) - addr;
    page = addr >> 12;
    end = (addr + size + 4095) >> 12;
    while (page < end)
    {
        uint64_t *bits = __atomic_load_n( &index->regions[page >> 15], __ATOMIC_ACQUIRE );
        uint64_t next = bits ? (page | 63) + 1 : (page | 32767) + 1;
        if (next > end) next = end;
        if (bits && (__atomic_load_n( &bits[(page >> 6) & 511], __ATOMIC_ACQUIRE ) &
                     swap_index_mask( page & 63, (next - 1) & 63 ))) return 1;
        page = next;
    }
    return 0;
}

#endif
