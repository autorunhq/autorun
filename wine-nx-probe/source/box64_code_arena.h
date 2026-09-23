/*
 * Allocation inside a Box64 code arena (wow64_box64_dynarec.c).
 *
 * Horizon hands out only ten kernel code memory objects for the whole system
 * (Atmosphere's SlabCountKCodeMemory), so an arena cannot simply be replaced
 * when it fills: the ten a process may hold are all the translated code it
 * will ever have. Box64 frees a translation whenever the guest code behind it
 * is unmapped or changed, and again for every block it starts and cancels, so
 * a run that never reuses that space fills the arenas with dead code. The
 * Sims 2 reached the end of the tenth arena after seven minutes, and from
 * there every entry into untranslated code ran the translator's passes in
 * full, failed to find room, threw the work away and interpreted the code --
 * one core doing nothing but translating the same blocks again and again.
 *
 * Chunks carry a 16 byte header and their payload is 16 byte aligned, which
 * is what Box64 wants of a block. Freed chunks join a list for their size and
 * are merged with a free neighbour, so an arena that has held a lot of short
 * lived blocks still has room for a long one. Everything is an offset from
 * the start of the arena: a block is written through the arena's writable
 * alias and runs from its executable alias, and only the caller knows which.
 *
 * The caller locks. There is no test for a pointer that was never allocated:
 * only the dynarec's own allocations come back here.
 */
#ifndef WINE_NX_BOX64_CODE_ARENA_H
#define WINE_NX_BOX64_CODE_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CODE_ARENA_ALIGN  16u
#define CODE_ARENA_HEADER 16u                             /* keeps the payload aligned */
#define CODE_ARENA_MIN    (CODE_ARENA_HEADER + CODE_ARENA_ALIGN)
#define CODE_ARENA_BINS   24u                             /* 32 bytes to 256 MB */
#define CODE_ARENA_WALK   32u                             /* chunks tried in one bin */
#define CODE_ARENA_NONE   ((uint32_t)-1)

struct code_chunk
{
    uint32_t size;       /* this chunk, header included */
    uint32_t prev_size;  /* the chunk before it, 0 for the first */
    uint32_t free;
    uint32_t next_free;  /* while free: the next chunk in its bin, or NONE */
    uint32_t prev_free;  /* while free: the one before it */
    uint32_t pad[3];
};

struct code_arena
{
    unsigned char *base;   /* the writable alias: headers are written through it */
    uint32_t size;
    uint32_t live;         /* handed out, headers included */
    uint32_t bins[CODE_ARENA_BINS];
};

static inline struct code_chunk *code_chunk_at( const struct code_arena *arena, uint32_t offset )
{
    return (struct code_chunk *)(arena->base + offset);
}

/* Bins hold chunks of at least 32 << bin bytes, so any chunk from a higher bin
 * fits a request that landed in this one. */
static inline unsigned code_arena_bin( uint32_t size )
{
    unsigned bin = 0;

    for (size >>= 5; size > 1 && bin < CODE_ARENA_BINS - 1; size >>= 1) bin++;
    return bin;
}

static inline void code_arena_unlink( struct code_arena *arena, uint32_t offset )
{
    struct code_chunk *chunk = code_chunk_at( arena, offset );

    if (chunk->prev_free != CODE_ARENA_NONE)
        code_chunk_at( arena, chunk->prev_free )->next_free = chunk->next_free;
    else
        arena->bins[code_arena_bin( chunk->size )] = chunk->next_free;
    if (chunk->next_free != CODE_ARENA_NONE)
        code_chunk_at( arena, chunk->next_free )->prev_free = chunk->prev_free;
    chunk->free = 0;
    chunk->next_free = chunk->prev_free = CODE_ARENA_NONE;
}

static inline void code_arena_link( struct code_arena *arena, uint32_t offset )
{
    struct code_chunk *chunk = code_chunk_at( arena, offset );
    unsigned bin = code_arena_bin( chunk->size );

    chunk->free = 1;
    chunk->prev_free = CODE_ARENA_NONE;
    chunk->next_free = arena->bins[bin];
    if (chunk->next_free != CODE_ARENA_NONE) code_chunk_at( arena, chunk->next_free )->prev_free = offset;
    arena->bins[bin] = offset;
}

/* size is at least CODE_ARENA_MIN and a multiple of the alignment. */
static inline uint32_t code_arena_round( size_t size )
{
    uint32_t want = (uint32_t)size + CODE_ARENA_HEADER;

    want = (want + CODE_ARENA_ALIGN - 1) & ~(CODE_ARENA_ALIGN - 1);
    return want < CODE_ARENA_MIN ? CODE_ARENA_MIN : want;
}

static inline void code_arena_init( struct code_arena *arena, void *base, size_t size )
{
    struct code_chunk *first;
    unsigned bin;

    memset( arena, 0, sizeof(*arena) );
    arena->base = base;
    arena->size = (uint32_t)(size & ~(size_t)(CODE_ARENA_ALIGN - 1));
    for (bin = 0; bin < CODE_ARENA_BINS; bin++) arena->bins[bin] = CODE_ARENA_NONE;
    if (arena->size < CODE_ARENA_MIN) return;
    first = code_chunk_at( arena, 0 );
    memset( first, 0, sizeof(*first) );
    first->size = arena->size;
    code_arena_link( arena, 0 );
}

/* The chunk a request would take, or NONE. */
static inline uint32_t code_arena_search( const struct code_arena *arena, uint32_t want )
{
    unsigned bin;

    for (bin = code_arena_bin( want ); bin < CODE_ARENA_BINS; bin++)
    {
        uint32_t offset = arena->bins[bin];
        unsigned tried;

        /* Every chunk in a higher bin is large enough; this one holds both. */
        for (tried = 0; offset != CODE_ARENA_NONE && tried < CODE_ARENA_WALK; tried++)
        {
            const struct code_chunk *chunk = code_chunk_at( arena, offset );

            if (chunk->size >= want) return offset;
            offset = chunk->next_free;
        }
    }
    return CODE_ARENA_NONE;
}

static inline int code_arena_fits( const struct code_arena *arena, size_t size )
{
    return code_arena_search( arena, code_arena_round( size ) ) != CODE_ARENA_NONE;
}

/* Returns the offset of the payload, or 0 when the arena has no room: offset 0
 * always holds a header, so it is never a payload. */
static inline uint32_t code_arena_alloc( struct code_arena *arena, size_t size )
{
    uint32_t want = code_arena_round( size ), offset = code_arena_search( arena, want );
    struct code_chunk *chunk;
    uint32_t rest;

    if (offset == CODE_ARENA_NONE) return 0;
    code_arena_unlink( arena, offset );
    chunk = code_chunk_at( arena, offset );
    rest = chunk->size - want;
    if (rest >= CODE_ARENA_MIN)  /* the tail is worth keeping on its own */
    {
        struct code_chunk *tail = code_chunk_at( arena, offset + want );
        uint32_t after = offset + chunk->size;

        memset( tail, 0, sizeof(*tail) );
        tail->size = rest;
        tail->prev_size = want;
        chunk->size = want;
        if (after < arena->size) code_chunk_at( arena, after )->prev_size = rest;
        code_arena_link( arena, offset + want );
    }
    arena->live += chunk->size;
    return offset + CODE_ARENA_HEADER;
}

/* Frees the chunk and returns what is free there now, its header included, or
 * 0 if there was nothing to free. payload is an offset from code_arena_alloc.
 * A chunk that is already free is
 * left alone: Box64 hands the same block to FreeDynarecMap twice if a
 * translation is canceled after the block it belonged to went, and merging a
 * free chunk into its neighbour twice would leave the chain naming a chunk
 * that is inside another one. */
static inline uint32_t code_arena_free( struct code_arena *arena, uint32_t payload )
{
    uint32_t offset = payload - CODE_ARENA_HEADER;
    struct code_chunk *chunk = code_chunk_at( arena, offset );
    uint32_t after;

    if (payload < CODE_ARENA_HEADER || payload >= arena->size || chunk->free) return 0;
    arena->live -= chunk->size;
    /* Merge with the chunk after it, then with the one before: a block that
     * outlived its neighbours leaves one hole, not three. */
    after = offset + chunk->size;
    if (after < arena->size && code_chunk_at( arena, after )->free)
    {
        struct code_chunk *next = code_chunk_at( arena, after );

        code_arena_unlink( arena, after );
        chunk->size += next->size;
    }
    if (chunk->prev_size)
    {
        uint32_t before = offset - chunk->prev_size;
        struct code_chunk *prev = code_chunk_at( arena, before );

        if (prev->free)
        {
            code_arena_unlink( arena, before );
            prev->size += chunk->size;
            offset = before;
            chunk = prev;
        }
    }
    after = offset + chunk->size;
    if (after < arena->size) code_chunk_at( arena, after )->prev_size = chunk->size;
    code_arena_link( arena, offset );
    /* What the caller can hand out again without searching: asking the bins
     * costs a walk through chunk headers spread over the whole arena. */
    return chunk->size;
}

/* Freed chunks wait here before they can be handed out again. Box64 reads a
 * block's own fields through pointers it took without the translator's lock:
 * an invalidation finds a block in the jump table and only then locks to free
 * it, and FreeDynablock checks gone first. Before build 225 nothing was reused
 * and such a pointer met the block it had found, marked gone; reused at once,
 * it could meet another thread's new block and free that. A block that is
 * purged may also still be entered by a thread that read its jump table entry
 * just before: kept whole, that thread runs the old code and leaves.
 *
 * Entries leave in the order they came, which is the order of their stamps. */
struct code_quarantine_entry
{
    uint32_t arena;              /* which arena, as the caller numbers them */
    uint32_t payload;            /* the offset code_arena_alloc returned */
    unsigned long long stamp;    /* when it was freed, in the caller's clock */
};

struct code_quarantine
{
    struct code_quarantine_entry *entries;
    uint32_t head, count, cap;
};

/* Returns 0 without memory, and the caller then frees the chunk at once. */
static inline int code_quarantine_push( struct code_quarantine *quarantine, uint32_t arena, uint32_t payload,
                                        unsigned long long stamp )
{
    if (quarantine->count == quarantine->cap)
    {
        uint32_t cap = quarantine->cap ? quarantine->cap * 2 : 256, i;
        struct code_quarantine_entry *entries = malloc( cap * sizeof(*entries) );

        if (!entries) return 0;
        for (i = 0; i < quarantine->count; i++)
            entries[i] = quarantine->entries[(quarantine->head + i) % quarantine->cap];
        free( quarantine->entries );
        quarantine->entries = entries;
        quarantine->cap = cap;
        quarantine->head = 0;
    }
    quarantine->entries[(quarantine->head + quarantine->count) % quarantine->cap] =
        (struct code_quarantine_entry){ arena, payload, stamp };
    quarantine->count++;
    return 1;
}

/* The entry freed longest ago, or NULL. */
static inline const struct code_quarantine_entry *code_quarantine_oldest( const struct code_quarantine *quarantine )
{
    return quarantine->count ? &quarantine->entries[quarantine->head] : NULL;
}

static inline void code_quarantine_drop_oldest( struct code_quarantine *quarantine )
{
    quarantine->head = (quarantine->head + 1) % quarantine->cap;
    if (!--quarantine->count) quarantine->head = 0;
}

#endif
