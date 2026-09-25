#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include "horizon_swap.h"
#include "horizon_swap_index.h"
#include "wine/rbtree.h"

#define WINE_NX_SWAP_POC
#define TRUE 1
#define FALSE 0
#define R_FAILED(rc) ((rc) != 0)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define HORIZON_LAZY_MAPPING_CHUNK 0x200000u
#define HORIZON_POOL_ARENA 0x200000u
#define HORIZON_POOL_PAGE 4096
#define HORIZON_POOL_ARENAS 1
typedef int BOOL;
typedef unsigned int Result;
typedef uint64_t u64;
typedef void VirtmemReservation;
struct horizon_page_pool
{
    unsigned int active_arenas;
    struct { void *memory; unsigned int free_pages; } arenas[HORIZON_POOL_ARENAS];
};
static struct horizon_page_pool backing_pages;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static int allocations, fail_alloc, fail_map, fail_perm, fail_unmap, fail_store;
static uintptr_t fail_unmap_at;
static unsigned int stores, loads;
static size_t fast_free = UINT64_C(4) << 30, memory_free = UINT64_C(4) << 30;
static size_t cached_credit;
static size_t trim_available;
static unsigned int pressure_checks, memory_scans;
static uint64_t clock_tick;
static unsigned int slow_store;
static unsigned char guest[8][8192], disk[64][8192], used[64];
static unsigned int permissions[16], aliases[16];
struct horizon_mapping;
static int replace_reservation_mapping( struct horizon_mapping *, char *, size_t, int, BOOL );
static size_t slot( uintptr_t addr ) { assert( addr >= 0x10000000 && addr < 0x10010000 ); return (addr - 0x10000000) / 4096; }
static void *guest_at( uintptr_t addr ) { return (char *)guest + (addr - 0x10000000); }
static void *horizon_pages_alloc_any( struct horizon_page_pool *pool, size_t size )
{
    (void)pool;
    if (fail_alloc) return NULL;
    allocations++;
    return malloc( size );
}
#define horizon_pages_alloc_dedicated horizon_pages_alloc_any
static int horizon_pages_free( struct horizon_page_pool *pool, void *memory, size_t size )
{
    (void)pool; (void)size;
    allocations--;
    free( memory );
    return 1;
}
static size_t horizon_pages_trim( struct horizon_page_pool *pool )
{
    size_t freed = trim_available;
    (void)pool;
    trim_available = 0;
    return freed;
}
static void horizon_get_memory_info( unsigned long long *total, unsigned long long *used_memory )
{ *total = UINT64_C(8) << 30; *used_memory = *total - memory_free; }
static size_t wine_nx_native_heap_free_estimate( size_t *lower_bound )
{ pressure_checks++; *lower_bound = fast_free; return fast_free + cached_credit; }
static size_t wine_nx_native_heap_free_exact(void)
{
    memory_scans++;
    cached_credit = memory_free > fast_free ? memory_free - fast_free : 0;
    return memory_free;
}
static void wine_nx_native_heap_note_small_allocation( size_t size )
{ cached_credit = size >= cached_credit ? 0 : cached_credit - size; }
static void wine_nx_runtime_trace( const char *line ) { (void)line; }
static unsigned int get_horizon_perm( int prot ) { return prot; }
static unsigned int envGetOwnProcessHandle(void) { return 1; }
static uint64_t armGetSystemTick(void)
{
    return __atomic_add_fetch( &clock_tick, 1, __ATOMIC_RELAXED );
}
static uint64_t armTicksToNs( uint64_t ticks ) { return ticks; }
static void note_alias_source( void *memory, void *addr, size_t size, BOOL add )
{
    size_t i;
    (void)memory;
    for (i = 0; i < size; i += 4096)
    {
        size_t n = slot( (uintptr_t)addr + i );
        assert( aliases[n] == !add );
        aliases[n] = add;
    }
}
static Result svcMapProcessCodeMemory( unsigned int process, u64 addr, u64 memory, size_t size )
{
    size_t i;
    (void)process;
    if (fail_map) return 1;
    memcpy( guest_at( addr ), (void *)(uintptr_t)memory, size );
    for (i = 0; i < size; i += 4096) permissions[slot( addr + i )] = 0;
    return 0;
}
static Result svcUnmapProcessCodeMemory( unsigned int process, u64 addr, u64 memory, size_t size )
{
    size_t i;
    (void)process;
    if (fail_unmap || addr == fail_unmap_at) return 1;
    memcpy( (void *)(uintptr_t)memory, guest_at( addr ), size );
    for (i = 0; i < size; i += 4096) permissions[slot( addr + i )] = 0;
    return 0;
}
static Result svcSetProcessMemoryPermission( unsigned int process, u64 addr, size_t size, unsigned int prot )
{
    size_t i;
    (void)process;
    if (fail_perm) return 1;
    for (i = 0; i < size; i += 4096) permissions[slot( addr + i )] = prot;
    return 0;
}
static int save( void *context, const void *data, size_t size, unsigned int *token )
{
    unsigned int i;
    (void)context;
    if (fail_store) { errno = EIO; return -1; }
    for (i = 0; i < 64; i++) if (!used[i]) break;
    assert( i != 64 && size <= sizeof(disk[i]) );
    used[i] = 1;
    memcpy( disk[i], data, size );
    *token = i + 1;
    stores++;
    if (slow_store) __atomic_add_fetch( &clock_tick, 10000000, __ATOMIC_RELAXED );
    return 0;
}
static int load( void *context, unsigned int token, void *data, size_t size )
{
    (void)context;
    assert( token && token <= 64 && used[token - 1] );
    memcpy( data, disk[token - 1], size );
    loads++;
    return 0;
}
static void discard( void *context, unsigned int token, size_t size )
{
    (void)context; (void)size;
    assert( token && token <= 64 && used[token - 1] );
    used[token - 1] = 0;
}

/* RUNTIME_IMPLEMENTATION */

static int replace_reservation_mapping( struct horizon_mapping *mapping, char *start, size_t size, int prot, BOOL map_range )
{
    (void)mapping; (void)start; (void)size; (void)prot; (void)map_range;
    abort();
}
static struct horizon_backing backings[8];
static struct horizon_mapping regions[8];
static void create( unsigned int n )
{
    struct horizon_backing *b = &backings[n];
    struct horizon_mapping *m = &regions[n];
    *b = (struct horizon_backing){ .code_addr = (void *)(uintptr_t)(0x10000000 + n * 8192), .size = 8192,
                                 .refs = 1, .fd = -1, .swap_private = 1 };
    b->heap_addr = horizon_pages_alloc_any( &backing_pages, b->size );
    *m = (struct horizon_mapping){ .addr = b->code_addr, .size = b->size,
                                  .backing = b, .prot = PROT_READ | PROT_WRITE };
    memset( guest[n], n + 10, 8192 );
    memcpy( b->heap_addr, guest[n], 8192 );
    aliases[2 * n] = aliases[2 * n + 1] = 1;
    permissions[2 * n] = permissions[2 * n + 1] = m->prot;
    assert( !rb_put( &mappings, m->addr, &m->entry ) );
    horizon_swap_track( m->addr, m->size );
}
static void *worker( void *arg )
{
    unsigned int n = (uintptr_t)arg, i;
    for (i = 0; i < 3000; i++)
    {
        struct horizon_swap_pin pin;
        assert( !horizon_swap_pin_begin( &pin, regions[n].addr, regions[n].size ) );
        assert( permissions[2 * n] == (PROT_READ | PROT_WRITE) );
        memset( guest[n], i % 251, sizeof(guest[n]) );
        horizon_swap_pin_end( &pin );
    }
    return NULL;
}

static void fragmented_backing(void)
{
    struct horizon_mapping *m = &regions[4], half;
    struct horizon_backing *b = &backings[4];
    create( 4 );
    assert( !horizon_swap_fault( (uintptr_t)m->addr, (0x24u << 26) | 4 ) );
    assert( !swap_profile.faults );
    m->size = 4096;
    half = *m;
    half.addr = (char *)m->addr + 4096;
    half.source_offset = 4096;
    half.prot = PROT_READ;
    b->refs = 2;
    assert( !rb_put( &mappings, half.addr, &half.entry ) );
    permissions[9] = PROT_READ;
    fail_unmap_at = (uintptr_t)half.addr;
    assert( !horizon_swap_reclaim( 8192 ) );
    assert( !b->swap.detached && !m->swap_unmapped && !half.swap_unmapped );
    assert( permissions[8] == 3 && permissions[9] == 1 );
    fail_map = 1;
    assert( !horizon_swap_reclaim( 8192 ) );
    assert( b->swap.detached && m->swap_unmapped == 1 && !half.swap_unmapped );
    fail_map = fail_unmap_at = 0;
    assert( horizon_swap_fault( (uintptr_t)m->addr, (0x24u << 26) | 4 ) );
    assert( !b->swap.detached && !m->swap_unmapped && !half.swap_unmapped );
    assert( !horizon_swap_fault( (uintptr_t)m->addr, (0x24u << 26) | 4 ) );
    assert( horizon_swap_reclaim( 8192 ) == 8192 );
    assert( !b->heap_addr && m->swap_unmapped == 1 && half.swap_unmapped == 1 );
    assert( swap_index_contains( &swap_fault_index, (uintptr_t)m->addr, 8192 ) );
    assert( horizon_swap_fault( (uintptr_t)half.addr, (0x24u << 26) | 4 ) );
    assert( permissions[8] == 3 && permissions[9] == 1 );
    assert( !swap_index_contains( &swap_fault_index, (uintptr_t)m->addr, 8192 ) );
    assert( guest[4][0] == 14 && guest[4][8191] == 14 );
    assert( !svcUnmapProcessCodeMemory( 1, (uintptr_t)half.addr, (uintptr_t)b->heap_addr + 4096, 4096 ) );
    note_alias_source( (char *)b->heap_addr + 4096, half.addr, 4096, FALSE );
    rb_remove( &mappings, &half.entry );
    b->refs = 1;
    assert( horizon_swap_reclaim( 8192 ) == 8192 );
    assert( horizon_swap_fault( (uintptr_t)m->addr, (0x24u << 26) | 4 ) );
    assert( permissions[8] == 3 && !aliases[9] && guest[4][0] == 14 );
    rb_remove( &mappings, &m->entry );
    horizon_pages_free( &backing_pages, b->heap_addr, b->size );
    assert( !allocations && !swap_stored_bytes );
}

static void *reclaim_worker( void *arg )
{
    unsigned int i;
    (void)arg;
    for (i = 0; i < 3000; i++) horizon_swap_reclaim( 16384 );
    return NULL;
}

static void bounded_reclaim(void)
{
    struct horizon_swap_pin pin;
    unsigned int i;
    create( 4 );
    create( 5 );
    slow_store = 1;
    assert( horizon_swap_reclaim( 16384 ) == 8192 );
    slow_store = 0;
    for (i = 4; i < 6; i++)
    {
        assert( !horizon_swap_pin_begin( &pin, regions[i].addr, regions[i].size ) );
        horizon_swap_pin_end( &pin );
        pthread_mutex_lock( &mapping_mutex );
        assert( !horizon_swap_fault( (uintptr_t)regions[i].addr, (0x24u << 26) | 4 ) );
        pthread_mutex_unlock( &mapping_mutex );
        /* A fault which observed the bitmap before another thread restored it. */
        assert( !swap_index_update( &swap_fault_index, (uintptr_t)regions[i].addr, 8192, 1 ) );
        assert( horizon_swap_fault( (uintptr_t)regions[i].addr, (0x24u << 26) | 4 ) );
        assert( !swap_index_update( &swap_fault_index, (uintptr_t)regions[i].addr, 8192, 0 ) );
        rb_remove( &mappings, &regions[i].entry );
        horizon_pages_free( &backing_pages, backings[i].heap_addr, 8192 );
    }
    assert( !allocations && !swap_stored_bytes && swap_profile.quick_refaults );
}

static void pressure_bounds(void)
{
    const size_t target = 8192 + 128 * 1048576;
    fast_free = target;
    swap_pressure_locked( 8192 );
    assert( pressure_checks == 1 && !memory_scans && swap_profile.pressure_fast == 1 );
    fast_free = target - 1;
    memory_free = target;
    swap_pressure_locked( 8192 );
    assert( memory_scans == 1 && !swap_profile.pressure && !swap_profile.reclaim_calls );
    swap_pressure_locked( 8192 );
    assert( memory_scans == 1 && swap_profile.pressure_cached == 1 );
    wine_nx_native_heap_note_small_allocation( 1 );
    trim_available = 8192;
    memory_free = target - 1;
    swap_pressure_locked( 8192 );
    assert( memory_scans == 2 && swap_profile.pressure == 1 && !swap_profile.reclaim_calls );
    assert( swap_profile.native_trim_bytes == 8192 );
    swap_pressure_locked( 8192 );
    assert( memory_scans == 3 && swap_profile.pressure == 2 && swap_profile.reclaim_calls == 1 );
    assert( swap_profile.pressure_scans == 3 && swap_profile.pressure_scan_ticks && swap_profile.pressure_scan_max_ticks );
    fast_free = memory_free = UINT64_C(8) << 30;
    swap_pressure_locked( UINT64_C(5) << 30 );
    assert( memory_scans == 3 && swap_profile.pressure_fast == 2 );
    fast_free = memory_free = UINT64_C(4) << 30;
}

int main(void)
{
    const struct horizon_swap_storage storage = { NULL, save, load, discard, sizeof(disk) };
    struct horizon_swap_pin pin, nested;
    pthread_t threads[3];
    unsigned long long total, available;
    unsigned int i;
    swap_pressure_locked( 8192 );
    assert( !pressure_checks && !memory_scans );
    horizon_swap_configure( &storage );
    pressure_bounds();
    fragmented_backing();
    bounded_reclaim();
    for (i = 0; i < 4; i++) create( i );
    assert( !horizon_swap_pin_begin( &pin, regions[0].addr, 1 ) );
    assert( !horizon_swap_pin_begin( &nested, regions[0].addr, 8192 ) );
    assert( horizon_swap_reclaim( 32768 ) == 24576 );
    assert( backings[0].heap_addr && !backings[1].heap_addr );
    horizon_swap_pin_end( &pin );
    assert( !horizon_swap_reclaim( 8192 ) );
    horizon_swap_pin_end( &nested );
    assert( horizon_swap_reclaim( 8192 ) == 8192 );
    horizon_swap_get_memory_info( &total, &available );
    assert( !total && !available );
    fail_alloc = 1;
    assert( horizon_swap_pin_begin( &pin, regions[0].addr, 8192 ) == -1 && !swap_pins );
    fail_alloc = 0;
    fail_perm = 1;
    assert( horizon_swap_pin_begin( &pin, regions[0].addr, 8192 ) == -1 && !swap_pins );
    assert( backings[0].heap_addr && regions[0].swap_unmapped == 2 );
    fail_perm = 0;
    assert( horizon_swap_fault( (uintptr_t)regions[0].addr, (0x24u << 26) | 0x0c ) );
    assert( permissions[0] == 3 && !regions[0].swap_unmapped && guest[0][0] == 10 );
    assert( !horizon_swap_lock( regions[0].addr, 8192 ) );
    assert( !horizon_swap_lock( (char *)regions[0].addr + 2048, 2048 ) );
    assert( !swap_locks->next && !horizon_swap_reclaim( 8192 ) );
    assert( !horizon_swap_unlock( (char *)regions[0].addr + 2048, 2048 ) );
    assert( swap_locks->size == 2048 && swap_locks->next->size == 4096 );
    assert( !horizon_swap_unlock( regions[0].addr, 8192 ) && !swap_locks );
    assert( !horizon_swap_exclude( regions[0].addr, 8192 ) );
    assert( !horizon_swap_reclaim( 8192 ) );
    /* Resident stack/report pages must not wait for the pager's mapping lock. */
    pthread_mutex_lock( &mapping_mutex );
    assert( !horizon_swap_reclaim( 8192 ) && swap_profile.native_busy );
    assert( !horizon_swap_pin_begin( &pin, regions[0].addr, 8192 ) && !pin.size );
    horizon_swap_pin_end( &pin );
    assert( horizon_swap_may_contain( regions[1].addr, 1 ) );
    pthread_mutex_unlock( &mapping_mutex );
    backings[0].swap_excluded = regions[0].swap_excluded = 0;
    horizon_swap_track( regions[0].addr, regions[0].size );
    fail_store = 1;
    assert( !horizon_swap_reclaim( 8192 ) && backings[0].heap_addr && !backings[0].swap.detached );
    assert( permissions[0] == 3 && guest[0][0] == 10 );
    fail_store = 0;
    assert( !pthread_create( &threads[0], NULL, worker, (void *)0 ) );
    assert( !pthread_create( &threads[1], NULL, worker, (void *)1 ) );
    assert( !pthread_create( &threads[2], NULL, reclaim_worker, NULL ) );
    for (i = 0; i < 3; i++) pthread_join( threads[i], NULL );
    for (i = 0; i < 4; i++)
    {
        size_t j;
        assert( !horizon_swap_pin_begin( &pin, regions[i].addr, regions[i].size ) );
        for (j = 0; j < 8192; j++) assert( guest[i][j] == (i < 2 ? 2999 % 251 : i + 10) );
        horizon_swap_pin_end( &pin );
        rb_remove( &mappings, &regions[i].entry );
        horizon_pages_free( &backing_pages, backings[i].heap_addr, 8192 );
    }
    assert( !allocations && stores && loads && !swap_stored_bytes && !swap_pins );
    assert( swap_profile.tracked_mappings >= 5 && swap_profile.tracked_bytes >= 5 * 8192 );
    assert( swap_profile.out_count == stores && swap_profile.refaults >= 3 );
    assert( swap_profile.reclaim_calls && swap_profile.scanned && swap_profile.pinned );
    assert( swap_profile.native_busy && swap_profile.restore_count >= loads );
    for (i = 0; i < 64; i++) assert( !used[i] );
    horizon_swap_configure( NULL );
    puts( "game swap policy: live data, nested pins, lock splits, fragmented backings, export exclusion, rollback and concurrent reclaim passed" );
}
