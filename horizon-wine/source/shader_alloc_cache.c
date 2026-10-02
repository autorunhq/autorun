#include <errno.h>
#include <string.h>
#include <stdint.h>
#include "native_heap.h"
#include "shader_alloc_cache.h"

enum { CACHE_LIMIT = 1048576, BLOCK_LIMIT = 65536, BIN_COUNT = 13,
       SMALL_BIN_COUNT = 6, SMALL_BIN_LIMIT = 1024, BIN_LIMIT = 64,
       LARGE_LIMIT = CACHE_LIMIT / 4, CLASS_COUNT = 61, SEARCH_LIMIT = 8,
       CLASS_SEARCH_LIMIT = 4, DRAIN_BATCH = 32, WORKER_LIMIT = 8 };
_Static_assert(CLASS_COUNT < 64, "cache class bitmap");

struct cached_block
{
    struct cached_block *next;
    size_t usable;
};

static __thread struct
{
    struct cached_block *bins[CLASS_COUNT];
    uint64_t nonempty;
    unsigned int counts[BIN_COUNT], depth;
    size_t bytes, large_bytes;
} cache;
__thread unsigned int wine_nx_shader_alloc_active;
static unsigned int workers;

static unsigned int size_bin(size_t size)
{
    return size < 32 ? 0 : (unsigned int)(8 * sizeof(unsigned long long) - 1 -
                                        __builtin_clzll(size)) - 4;
}

static unsigned int bin_limit(unsigned int bin)
{
    return bin < SMALL_BIN_COUNT ? SMALL_BIN_LIMIT : BIN_LIMIT;
}

static unsigned int size_class(size_t size)
{
    if (size < 512) return size >> 4;
    unsigned int shift = size_bin(size) + 4;
    return 32 + (shift - 9) * 4 + ((size >> (shift - 2)) & 3);
}

static size_t reuse_limit(size_t size)
{
    size_t limit = size < 64 ? size + 64 : size * 2;
    return limit < BLOCK_LIMIT ? limit : BLOCK_LIMIT;
}

void wine_nx_shader_alloc_begin(void)
{
    if (cache.depth++) return;
    unsigned int count = __atomic_load_n(&workers, __ATOMIC_RELAXED);
    while (count < WORKER_LIMIT)
    {
        if (!__atomic_compare_exchange_n(&workers, &count, count + 1, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        wine_nx_shader_alloc_active = 1;
        return;
    }
}

void *wine_nx_shader_alloc_take(size_t size, size_t alignment)
{
    if (!size || size > BLOCK_LIMIT || !alignment || (alignment & (alignment - 1)) || alignment > 256)
        return NULL;
    unsigned int first = size_class(size), examined = 0;
    size_t limit = reuse_limit(size);
    uint64_t candidates = cache.nonempty & (UINT64_MAX << first) &
                          ((UINT64_C(1) << (size_class(limit) + 1)) - 1);
    while (candidates && examined < SEARCH_LIMIT)
    {
        unsigned int bin = __builtin_ctzll(candidates);
        candidates &= candidates - 1;
        struct cached_block **link = &cache.bins[bin];
        for (unsigned int i = 0; *link && i < CLASS_SEARCH_LIMIT && examined < SEARCH_LIMIT;
             i++, link = &(*link)->next)
        {
            struct cached_block *block = *link;
            examined++;
            if (block->usable < size || block->usable > limit || ((uintptr_t)block & (alignment - 1))) continue;
            *link = block->next;
            if (!cache.bins[bin]) cache.nonempty &= ~(UINT64_C(1) << bin);
            unsigned int group = size_bin(block->usable);
            cache.counts[group]--;
            cache.bytes -= block->usable + sizeof(size_t);
            if (group >= SMALL_BIN_COUNT) cache.large_bytes -= block->usable + sizeof(size_t);
            return block;
        }
    }
    return NULL;
}

int wine_nx_shader_alloc_put(void *pointer, size_t usable)
{
    if (usable < sizeof(struct cached_block))
        return 0;
    if (usable > BLOCK_LIMIT)
        return 0;
    unsigned int bin = size_bin(usable);
    if (bin >= SMALL_BIN_COUNT && cache.large_bytes > LARGE_LIMIT - usable - sizeof(size_t))
        return 0;
    if (cache.bytes > CACHE_LIMIT - usable - sizeof(size_t))
        return 0;
    if (cache.counts[bin] == bin_limit(bin))
        return 0;
    struct cached_block *block = pointer;
    unsigned int index = size_class(usable);
    block->usable = usable;
    block->next = cache.bins[index];
    cache.bins[index] = block;
    cache.nonempty |= UINT64_C(1) << index;
    cache.counts[bin]++;
    cache.bytes += usable + sizeof(size_t);
    if (bin >= SMALL_BIN_COUNT) cache.large_bytes += usable + sizeof(size_t);
    return 1;
}

static void drain(void)
{
    if (!cache.nonempty) return;
    int saved_errno = errno;
    void *batch[DRAIN_BATCH];
    unsigned int count = 0;
    size_t bytes = 0;
    for (uint64_t mask = cache.nonempty; mask; mask &= mask - 1)
    {
        unsigned int bin = __builtin_ctzll(mask);
        struct cached_block *block = cache.bins[bin];
        cache.bins[bin] = NULL;
        while (block)
        {
            struct cached_block *next = block->next;
            bytes += block->usable + sizeof(size_t);
            batch[count++] = block;
            if (count == DRAIN_BATCH)
            {
                wine_nx_native_free_cached(batch, count, bytes);
                count = 0;
                bytes = 0;
            }
            block = next;
        }
    }
    if (count) wine_nx_native_free_cached(batch, count, bytes);
    memset(cache.counts, 0, sizeof(cache.counts));
    cache.nonempty = 0;
    cache.bytes = 0;
    cache.large_bytes = 0;
    errno = saved_errno;
}

int wine_nx_shader_alloc_trim(void)
{
    if (!wine_nx_shader_alloc_active || !cache.bytes) return 0;
    drain();
    return 1;
}

void wine_nx_shader_alloc_end(void)
{
    if (--cache.depth || !wine_nx_shader_alloc_active) return;
    drain();
    wine_nx_shader_alloc_active = 0;
    __atomic_sub_fetch(&workers, 1, __ATOMIC_RELAXED);
}
