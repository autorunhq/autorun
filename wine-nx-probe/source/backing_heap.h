#ifndef WINE_NX_BACKING_HEAP_H
#define WINE_NX_BACKING_HEAP_H

#include <stdint.h>
#include <stddef.h>

#define BACKING_UNIT ((size_t)65536)
#define BACKING_BINS 18
#define BACKING_NONE UINT32_MAX
#define BACKING_USED (1u << 31)

/* Boundary tags and free links stay outside pages lent to Horizon or the GPU. */
struct backing_block { uint32_t length, next, previous; };
struct backing_heap
{
    struct backing_block *blocks;
    uintptr_t base;
    uint32_t count, bottom, bins[BACKING_BINS], holes;
    size_t used;
};

static inline unsigned int backing_bin( uint32_t length )
{
    return 31 - __builtin_clz( length );
}

static inline void backing_init( struct backing_heap *heap, struct backing_block *blocks,
                                 uintptr_t base, uint32_t count )
{
    unsigned int i;
    *heap = (struct backing_heap){ .blocks = blocks, .base = base, .count = count, .bottom = count };
    for (i = 0; i < BACKING_BINS; i++) heap->bins[i] = BACKING_NONE;
}

static inline void backing_tag( struct backing_heap *heap, uint32_t first, uint32_t length, int used )
{
    heap->blocks[first].length = heap->blocks[first + length - 1].length = length | (used ? BACKING_USED : 0);
}

static inline void backing_insert( struct backing_heap *heap, uint32_t first, uint32_t length )
{
    unsigned int bin = backing_bin( length );
    struct backing_block *block = &heap->blocks[first];
    backing_tag( heap, first, length, 0 );
    block->previous = BACKING_NONE;
    block->next = heap->bins[bin];
    if (block->next != BACKING_NONE) heap->blocks[block->next].previous = first;
    heap->bins[bin] = first;
    heap->holes++;
}

static inline void backing_remove( struct backing_heap *heap, uint32_t first )
{
    struct backing_block *block = &heap->blocks[first];
    if (block->previous != BACKING_NONE) heap->blocks[block->previous].next = block->next;
    else heap->bins[backing_bin( block->length )] = block->next;
    if (block->next != BACKING_NONE) heap->blocks[block->next].previous = block->previous;
    heap->holes--;
}

static inline void *backing_alloc( struct backing_heap *heap, uintptr_t floor, size_t alignment, size_t size )
{
    uint32_t length, first, end, at, best;
    unsigned int bin;
    uintptr_t address;

    if (!size || !alignment || (alignment & (alignment - 1)) || size > SIZE_MAX - (BACKING_UNIT - 1)) return NULL;
    size = (size + BACKING_UNIT - 1) & ~(BACKING_UNIT - 1);
    if (alignment < BACKING_UNIT) alignment = BACKING_UNIT;
    if (size / BACKING_UNIT > heap->count) goto failed;
    length = size / BACKING_UNIT;
    for (bin = backing_bin( length ); bin < BACKING_BINS; bin++)
    {
        best = BACKING_NONE;
        for (first = heap->bins[bin]; first != BACKING_NONE; first = heap->blocks[first].next)
        {
            if (heap->blocks[first].length < length) continue;
            end = first + heap->blocks[first].length;
            address = (heap->base + (size_t)end * BACKING_UNIT - size) & ~(uintptr_t)(alignment - 1);
            if (address < heap->base + (size_t)first * BACKING_UNIT) continue;
            if (best == BACKING_NONE || heap->blocks[first].length < heap->blocks[best].length)
                best = first;
            if (heap->blocks[first].length == length) break;
        }
        if (best == BACKING_NONE) continue;
        first = best;
        end = first + heap->blocks[first].length;
        address = (heap->base + (size_t)end * BACKING_UNIT - size) & ~(uintptr_t)(alignment - 1);
        at = (address - heap->base) / BACKING_UNIT;
        backing_remove( heap, first );
        if (at > first) backing_insert( heap, first, at - first );
        if (at + length < end) backing_insert( heap, at + length, end - at - length );
        goto allocated;
    }
    end = heap->bottom;
    if (length > end) goto failed;
    address = (heap->base + (size_t)end * BACKING_UNIT - size) & ~(uintptr_t)(alignment - 1);
    if (address < heap->base || address < floor) goto failed;
    at = (address - heap->base) / BACKING_UNIT;
    heap->bottom = at;
    if (at + length < end) backing_insert( heap, at + length, end - at - length );
allocated:
    backing_tag( heap, at, length, 1 );
    heap->used += size;
    return (void *)address;
failed:
    return NULL;
}

static inline size_t backing_size( const struct backing_heap *heap, const void *pointer )
{
    uintptr_t address = (uintptr_t)pointer;
    uint32_t at, tag;
    if (address < heap->base || address - heap->base >= (size_t)heap->count * BACKING_UNIT ||
        (address - heap->base) % BACKING_UNIT) return 0;
    at = (address - heap->base) / BACKING_UNIT;
    if (at < heap->bottom) return 0;
    tag = heap->blocks[at].length;
    return tag & BACKING_USED ? (size_t)(tag & ~BACKING_USED) * BACKING_UNIT : 0;
}

static inline void backing_free( struct backing_heap *heap, void *pointer )
{
    uint32_t first = ((uintptr_t)pointer - heap->base) / BACKING_UNIT;
    uint32_t length = heap->blocks[first].length & ~BACKING_USED, adjacent;
    heap->used -= (size_t)length * BACKING_UNIT;
    if (first > heap->bottom && !((adjacent = heap->blocks[first - 1].length) & BACKING_USED))
    {
        first -= adjacent;
        backing_remove( heap, first );
        length += adjacent;
    }
    if (first + length < heap->count && !((adjacent = heap->blocks[first + length].length) & BACKING_USED))
    {
        backing_remove( heap, first + length );
        length += adjacent;
    }
    if (first == heap->bottom) heap->bottom += length;
    else backing_insert( heap, first, length );
}

static inline int backing_resize( struct backing_heap *heap, void *pointer, size_t size )
{
    uint32_t first = ((uintptr_t)pointer - heap->base) / BACKING_UNIT;
    uint32_t previous = heap->blocks[first].length & ~BACKING_USED, length, adjacent;
    if (!size || size > SIZE_MAX - (BACKING_UNIT - 1)) return 0;
    size = (size + BACKING_UNIT - 1) & ~(BACKING_UNIT - 1);
    if (size / BACKING_UNIT > heap->count) return 0;
    length = size / BACKING_UNIT;
    if (length == previous) return 1;
    if (length < previous)
    {
        backing_tag( heap, first, length, 1 );
        backing_tag( heap, first + length, previous - length, 1 );
        backing_free( heap, (void *)(heap->base + (size_t)(first + length) * BACKING_UNIT) );
        return 1;
    }
    if (first + previous == heap->count) return 0;
    adjacent = heap->blocks[first + previous].length;
    if ((adjacent & BACKING_USED) || length - previous > adjacent) return 0;
    backing_remove( heap, first + previous );
    if (length - previous < adjacent) backing_insert( heap, first + length, previous + adjacent - length );
    backing_tag( heap, first, length, 1 );
    heap->used += (size_t)(length - previous) * BACKING_UNIT;
    return 1;
}

static inline size_t backing_largest( const struct backing_heap *heap )
{
    unsigned int bin = BACKING_BINS;
    uint32_t largest = 0, at;
    if (!heap->blocks) return 0;
    while (bin--)
    {
        for (at = heap->bins[bin]; at != BACKING_NONE; at = heap->blocks[at].next)
            if (heap->blocks[at].length > largest) largest = heap->blocks[at].length;
        if (largest) break;
    }
    return (size_t)largest * BACKING_UNIT;
}

#endif
