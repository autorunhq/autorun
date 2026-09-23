/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Bounded storage for mapping objects and page-aligned backing memory.
 * The caller holds mapping_mutex. No live mapped pages may be returned. */
#ifndef HORIZON_POOL_H
#define HORIZON_POOL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct horizon_object_pool
{
    void *storage, *free;
    size_t size, count, used;
};

static inline void *horizon_object_alloc( struct horizon_object_pool *pool )
{
    void *ptr;
    if ((ptr = pool->free)) memcpy( &pool->free, ptr, sizeof(pool->free) );
    else if (pool->used < pool->count) ptr = (char *)pool->storage + pool->size * pool->used++;
    else return calloc( 1, pool->size );
    memset( ptr, 0, pool->size );
    return ptr;
}

static inline void horizon_object_free( struct horizon_object_pool *pool, void *ptr )
{
    uintptr_t offset;
    if (!ptr) return;
    offset = (uintptr_t)ptr - (uintptr_t)pool->storage;
    if (offset < pool->size * pool->count && !(offset % pool->size))
    {
        memcpy( ptr, &pool->free, sizeof(pool->free) );
        pool->free = ptr;
    }
    else free( ptr );
}

#define HORIZON_POOL_PAGE 4096
#define HORIZON_POOL_PAGES 512
#define HORIZON_POOL_ARENAS 512
#define HORIZON_POOL_RETAIN_EMPTY 8
/* One arena is one kernel memory block, and is aligned like one: the pages an
 * alias takes as its source lose their mapping until the alias goes away, and
 * the kernel splits the block they sit in to do it, so a block must hold
 * nothing but arena pages. Sharing one with the general heap leaves another
 * thread's allocation unmapped under it. */
#define HORIZON_POOL_ARENA ((size_t)HORIZON_POOL_PAGES * HORIZON_POOL_PAGE)
struct horizon_page_arena
{
    unsigned char *memory;
    unsigned short free_pages;
    unsigned char used[HORIZON_POOL_PAGES];
};
struct horizon_page_pool
{
    struct horizon_page_arena arenas[HORIZON_POOL_ARENAS];
    unsigned int active_arenas, peak_arenas;
    unsigned long long hits, misses, reclaims, blocks, shared;
};

/* Arena pages, or NULL for a request larger than an arena or a full pool,
 * which horizon_pages_alloc_any serves instead. Each arena is independent of
 * the general heap's small allocation metadata. */
static inline void *horizon_pages_alloc( struct horizon_page_pool *pool, size_t size )
{
    size_t pages = size / HORIZON_POOL_PAGE;
    unsigned int i, j, run;
    if (!size || size % HORIZON_POOL_PAGE || pages > HORIZON_POOL_PAGES) return NULL;
    for (i = 0; i < HORIZON_POOL_ARENAS; i++)
    {
        struct horizon_page_arena *arena = &pool->arenas[i];
        if (!arena->memory) continue;
        if (arena->free_pages < pages) continue;
        for (j = run = 0; j < HORIZON_POOL_PAGES; j++)
        {
            run = arena->used[j] ? 0 : run + 1;
            if (run == pages)
            {
                unsigned int first = j + 1 - run;
                memset( arena->used + first, 1, pages );
                arena->free_pages -= pages;
                pool->hits++;
                return arena->memory + first * HORIZON_POOL_PAGE;
            }
        }
    }
    for (i = 0; i < HORIZON_POOL_ARENAS; i++)
    {
        struct horizon_page_arena *arena = &pool->arenas[i];

        if (arena->memory) continue;
        arena->memory = aligned_alloc( HORIZON_POOL_ARENA, HORIZON_POOL_ARENA );
        if (!arena->memory) return NULL;
        arena->free_pages = HORIZON_POOL_PAGES - pages;
        memset( arena->used, 1, pages );
        pool->active_arenas++;
        if (pool->active_arenas > pool->peak_arenas) pool->peak_arenas = pool->active_arenas;
        pool->misses++;
        pool->hits++;
        return arena->memory;
    }
    return NULL;
}

static inline void *horizon_pages_alloc_dedicated( struct horizon_page_pool *pool, size_t size )
{
    void *ptr;

    if (!size || size % HORIZON_POOL_PAGE) return NULL;
    if ((ptr = horizon_pages_alloc( pool, size ))) return ptr;

    ptr = aligned_alloc( HORIZON_POOL_ARENA,
                         ((size + HORIZON_POOL_ARENA - 1) / HORIZON_POOL_ARENA) * HORIZON_POOL_ARENA );
    if (ptr) pool->blocks++;
    return ptr;
}

/* The legacy fallback may share a kernel block with unrelated heap allocations. */
static inline void *horizon_pages_alloc_any( struct horizon_page_pool *pool, size_t size )
{
    void *ptr = horizon_pages_alloc_dedicated( pool, size );
    if (!ptr && size && !(size % HORIZON_POOL_PAGE) && (ptr = aligned_alloc( HORIZON_POOL_PAGE, size )))
        pool->shared++;
    return ptr;
}

/* Returns zero for a direct allocation, which the caller must free normally. */
static inline int horizon_pages_free( struct horizon_page_pool *pool, void *ptr, size_t size )
{
    unsigned int i;

    if (!ptr || !size || size % HORIZON_POOL_PAGE) return 0;
    for (i = 0; i < HORIZON_POOL_ARENAS; i++)
    {
        struct horizon_page_arena *arena = &pool->arenas[i];
        uintptr_t offset;

        if (!arena->memory) continue;
        offset = (uintptr_t)ptr - (uintptr_t)arena->memory;
        if (offset < HORIZON_POOL_ARENA && !(offset % HORIZON_POOL_PAGE) &&
            size <= HORIZON_POOL_ARENA - offset)
        {
            size_t pages = size / HORIZON_POOL_PAGE;
            memset( arena->used + offset / HORIZON_POOL_PAGE, 0, pages );
            arena->free_pages += pages;
            if (arena->free_pages == HORIZON_POOL_PAGES &&
                pool->active_arenas > HORIZON_POOL_RETAIN_EMPTY)
            {
                free( arena->memory );
                memset( arena, 0, sizeof(*arena) );
                pool->active_arenas--;
                pool->reclaims++;
            }
            return 1;
        }
    }
    return 0;
}

static inline size_t horizon_pages_trim( struct horizon_page_pool *pool )
{
    unsigned int i;
    size_t freed = 0;
    for (i = 0; i < HORIZON_POOL_ARENAS; i++)
    {
        struct horizon_page_arena *arena = &pool->arenas[i];
        if (!arena->memory || arena->free_pages != HORIZON_POOL_PAGES) continue;
        free( arena->memory );
        memset( arena, 0, sizeof(*arena) );
        pool->active_arenas--;
        pool->reclaims++;
        freed += HORIZON_POOL_ARENA;
    }
    return freed;
}
#endif
