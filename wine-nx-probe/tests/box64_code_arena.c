/* Host test for allocation inside a code arena (source/box64_code_arena.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/box64_code_arena.h"

#define ARENA_SIZE (4u * 1024 * 1024)

/* Walks the chain: sizes add up to the arena, each chunk agrees with the one
 * before it, no two free chunks are neighbours, and live matches what is
 * handed out. */
static void check_chain( const struct code_arena *arena )
{
    uint32_t offset = 0, previous = 0, live = 0;
    int was_free = 0;

    while (offset < arena->size)
    {
        const struct code_chunk *chunk = code_chunk_at( arena, offset );

        assert( chunk->size >= CODE_ARENA_MIN && !(chunk->size % CODE_ARENA_ALIGN) );
        assert( offset + chunk->size <= arena->size );
        assert( chunk->prev_size == previous );
        assert( !(chunk->free && was_free) );  /* neighbours would have been merged */
        if (!chunk->free) live += chunk->size;
        was_free = chunk->free;
        previous = chunk->size;
        offset += chunk->size;
    }
    assert( offset == arena->size );
    assert( live == arena->live );
}

/* Every live allocation keeps its own bytes: the pattern written into one is
 * still there after other allocations have come and gone. */
struct held
{
    uint32_t payload;
    uint32_t size;
    unsigned char tag;
};

static void fill( struct code_arena *arena, struct held *h )
{
    memset( arena->base + h->payload, h->tag, h->size );
}

static void verify( const struct code_arena *arena, const struct held *h )
{
    uint32_t i;

    for (i = 0; i < h->size; i++) assert( arena->base[h->payload + i] == h->tag );
}

static void test_alloc_and_free(void)
{
    struct code_arena arena;
    void *memory = malloc( ARENA_SIZE );
    struct held a, b, c;

    assert( memory );
    code_arena_init( &arena, memory, ARENA_SIZE );
    check_chain( &arena );

    a.size = 1000; a.tag = 0xa1; a.payload = code_arena_alloc( &arena, a.size );
    b.size = 64;   b.tag = 0xb2; b.payload = code_arena_alloc( &arena, b.size );
    c.size = 5000; c.tag = 0xc3; c.payload = code_arena_alloc( &arena, c.size );
    assert( a.payload && b.payload && c.payload );
    assert( !(a.payload % CODE_ARENA_ALIGN) && !(b.payload % CODE_ARENA_ALIGN) && !(c.payload % CODE_ARENA_ALIGN) );
    assert( a.payload + a.size <= b.payload && b.payload + b.size <= c.payload );  /* no overlap */
    fill( &arena, &a ); fill( &arena, &b ); fill( &arena, &c );
    check_chain( &arena );

    /* Freeing reports the hole it leaves, which is what tells the dynarec a
     * translation is worth trying again. */
    assert( code_arena_free( &arena, b.payload ) >= b.size + CODE_ARENA_HEADER );
    check_chain( &arena );
    verify( &arena, &a );
    verify( &arena, &c );
    /* The hole b left is where a request that fits goes. */
    b.size = 48; b.tag = 0xb4; b.payload = code_arena_alloc( &arena, b.size );
    assert( b.payload && b.payload + b.size <= c.payload );
    fill( &arena, &b );
    verify( &arena, &a );
    verify( &arena, &c );

    /* Neighbours merge, so each free reports more room than the block held. */
    assert( code_arena_free( &arena, a.payload ) >= a.size );
    assert( code_arena_free( &arena, b.payload ) > a.size + b.size );
    assert( code_arena_free( &arena, c.payload ) == arena.size );
    check_chain( &arena );
    assert( !arena.live );
    /* Everything merged back into one chunk: the arena takes its whole size. */
    assert( code_chunk_at( &arena, 0 )->size == arena.size );
    assert( code_arena_alloc( &arena, arena.size - CODE_ARENA_HEADER ) == CODE_ARENA_HEADER );
    free( memory );
}

static void test_full_arena(void)
{
    struct code_arena arena;
    void *memory = malloc( ARENA_SIZE );
    uint32_t taken[64];
    unsigned count = 0;

    assert( memory );
    code_arena_init( &arena, memory, ARENA_SIZE );
    /* 64 KB at a time until it is full: the last request fails, and the arena
     * says so before being asked. */
    while (count < 64)
    {
        uint32_t payload;

        if (!code_arena_fits( &arena, 64 * 1024 )) break;
        payload = code_arena_alloc( &arena, 64 * 1024 );
        assert( payload );
        taken[count++] = payload;
    }
    assert( count == 63 );  /* 4 MB of 64 KB chunks, one header each */
    assert( !code_arena_fits( &arena, 64 * 1024 ) && !code_arena_alloc( &arena, 64 * 1024 ) );
    check_chain( &arena );
    /* Freeing one makes room for exactly one more. */
    code_arena_free( &arena, taken[7] );
    assert( code_arena_fits( &arena, 64 * 1024 ) );
    taken[7] = code_arena_alloc( &arena, 64 * 1024 );
    assert( taken[7] && !code_arena_alloc( &arena, 64 * 1024 ) );
    check_chain( &arena );
    free( memory );
}

/* What a long run does: blocks of many sizes, a third of them thrown away
 * again, going on far past the arena's own size. With nothing reused this
 * stopped at 4 MB; the Sims 2 stopped at 116 MB and spent the rest of the run
 * translating blocks it then had to interpret. */
static void test_churn(void)
{
    struct code_arena arena;
    void *memory = malloc( ARENA_SIZE );
    struct held live[256];
    unsigned long long allocated = 0;
    unsigned i, held = 0, seed = 12345;

    assert( memory );
    code_arena_init( &arena, memory, ARENA_SIZE );
    for (i = 0; i < 20000; i++)
    {
        seed = seed * 1103515245u + 12345u;
        if (held == 256 || (held && !(seed >> 16 & 3)))  /* a quarter go back */
        {
            unsigned pick = (seed >> 8) % held;

            verify( &arena, &live[pick] );
            code_arena_free( &arena, live[pick].payload );
            live[pick] = live[--held];
            continue;
        }
        live[held].size = 64 + (seed >> 12) % 8192;
        live[held].tag = (unsigned char)i;
        if (!(live[held].payload = code_arena_alloc( &arena, live[held].size ))) continue;
        fill( &arena, &live[held] );
        allocated += live[held].size;
        held++;
    }
    /* Far more than the arena holds passed through it, and what is still held
     * is still intact. */
    assert( allocated > 8ull * ARENA_SIZE );
    for (i = 0; i < held; i++) verify( &arena, &live[i] );
    check_chain( &arena );
    while (held) code_arena_free( &arena, live[--held].payload );
    check_chain( &arena );
    assert( !arena.live && code_chunk_at( &arena, 0 )->size == arena.size );
    free( memory );
}

/* The same block freed twice, and an offset that never came from the arena:
 * both leave the chain as it was. */
static void test_double_free(void)
{
    struct code_arena arena;
    void *memory = malloc( ARENA_SIZE );
    struct held a, b;

    assert( memory );
    code_arena_init( &arena, memory, ARENA_SIZE );
    a.size = 2048; a.tag = 0x5a; a.payload = code_arena_alloc( &arena, a.size );
    b.size = 2048; b.tag = 0x6b; b.payload = code_arena_alloc( &arena, b.size );
    assert( a.payload && b.payload );
    fill( &arena, &b );
    code_arena_free( &arena, a.payload );
    check_chain( &arena );
    assert( !code_arena_free( &arena, a.payload ) );  /* nothing to free twice */
    assert( !code_arena_free( &arena, 0 ) && !code_arena_free( &arena, arena.size ) );
    check_chain( &arena );
    verify( &arena, &b );
    code_arena_free( &arena, b.payload );
    assert( !arena.live && code_chunk_at( &arena, 0 )->size == arena.size );
    free( memory );
}

/* Freed chunks leave the quarantine oldest first, and the list grows past its
 * first size without losing its order. */
static void test_quarantine(void)
{
    struct code_quarantine quarantine = {0};
    const struct code_quarantine_entry *oldest;
    uint32_t i;

    assert( !code_quarantine_oldest( &quarantine ) );
    for (i = 0; i < 1000; i++) assert( code_quarantine_push( &quarantine, i % 3, i * 16 + 16, 100 + i ) );
    for (i = 0; i < 400; i++)
    {
        assert( (oldest = code_quarantine_oldest( &quarantine )) );
        assert( oldest->arena == i % 3 && oldest->payload == i * 16 + 16 && oldest->stamp == 100 + i );
        code_quarantine_drop_oldest( &quarantine );
    }
    /* Pushed while the ring has wrapped: still behind everything older. */
    for (i = 1000; i < 1600; i++) assert( code_quarantine_push( &quarantine, i % 3, i * 16 + 16, 100 + i ) );
    for (i = 400; i < 1600; i++)
    {
        assert( (oldest = code_quarantine_oldest( &quarantine )) && oldest->stamp == 100 + i );
        code_quarantine_drop_oldest( &quarantine );
    }
    assert( !code_quarantine_oldest( &quarantine ) );
    free( quarantine.entries );
}

int main(void)
{
    test_quarantine();
    test_double_free();
    test_alloc_and_free();
    test_full_arena();
    test_churn();
    puts( "Box64 code arena: alignment, holes reused, a full arena, merging neighbours, a block freed twice, "
          "the quarantine's order and 20000 blocks through an arena a fraction of their size passed" );
    return 0;
}
