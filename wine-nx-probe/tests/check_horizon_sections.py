#!/usr/bin/env python3
"""Run horizon.c's code for views of sections with no file against a host
stand-in for the Horizon kernel (horizon_memfile_host.h) that really shares
pages between addresses: a view mapped over a reservation the way Wine maps
one, another placed anywhere and overlapping it, the descriptor closed before
either is unmapped, a range taken away with PROT_NONE and given back, anchors
nothing may unmap or map over, kernel refusals that must change nothing, and
views unmapped in pieces. After each step, the mapping tree must not overlap
and every entry must hold exactly its own reservation."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'dlls/ntdll/unix/horizon.c').read_text()

def function(name):
    # The definition, not a prototype: the parameters end in ")" before "{".
    m = re.search('^' + re.escape(name) + r'[^;{]*?\)\s*\n\{', s, re.M | re.S)
    assert m, name
    i = m.end()
    depth = 1
    while depth:
        depth += (s[i] == '{') - (s[i] == '}')
        i += 1
    return s[m.start():i] + '\n'

def block(pattern):
    return re.search(pattern, s, re.M | re.S).group(0) + '\n'

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "wine/rbtree.h"
#include "horizon_pool.h"
#include "horizon_free_range.h"
#include "horizon_memfile_host.h"
#include "horizon_memfile.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
typedef int BOOL;
#define TRUE 1
#define FALSE 0
typedef unsigned long ULONG_PTR;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u32 Result;
typedef u32 Handle;
#define R_FAILED(rc) ((rc) != 0)
#define WARN(...) ((void)0)
#define horizon_trace(...) ((void)0)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static int traces;
static void wine_nx_runtime_trace( const char *msg ) { (void)msg; traces++; }
static void describe_memory( void *addr, char *buffer, size_t size )
{ snprintf( buffer, size, "%p", addr ); }
static int check_code_memory_syscalls(void) { return 0; }
static int envIsSyscallHinted( unsigned int number ) { (void)number; return 1; }
static Handle envGetOwnProcessHandle(void) { return 1; }
static int get_effective_horizon_prot( int prot ) { return prot; }

/* libnx's reservations: they must never overlap. */
typedef struct VirtmemReservation { void *addr; size_t size; struct VirtmemReservation *next; } VirtmemReservation;
static VirtmemReservation *reservations;
static int reservation_count;
static char *code_pool;
static size_t code_pool_used;
/* The window the runtime places its own mappings in, and the kernel's map of
 * it: with no window set, anchor regions come from libnx as they do on an
 * address space the runtime made no reservations in. */
void *horizon_native_window_start, *horizon_native_window_end;
static void horizon_get_address_space_limits(void **start, void **end)
{ *start = (void *)0x200000; *end = (void *)0x100000000ULL; }
static struct { unsigned long long addr, size; } native_blocks[64];
static unsigned int native_block_count;
static int horizon_query_region( void *context, unsigned long long addr, struct horizon_region *region )
{
    unsigned long long next = ~0ull;
    unsigned int i;

    (void)context;
    for (i = 0; i < native_block_count; i++)
    {
        if (addr >= native_blocks[i].addr && addr - native_blocks[i].addr < native_blocks[i].size)
        {
            region->addr = native_blocks[i].addr;
            region->size = native_blocks[i].size;
            region->type = HORIZON_MEMTYPE_UNMAPPED + 1;
            return 1;
        }
        if (native_blocks[i].addr > addr && native_blocks[i].addr < next) next = native_blocks[i].addr;
    }
    region->addr = addr;
    region->size = (next == ~0ull ? addr + (1ull << 32) : next) - addr;
    region->type = HORIZON_MEMTYPE_UNMAPPED;
    return 1;
}
static void virtmemLock(void) {}
static void virtmemUnlock(void) {}
static VirtmemReservation *virtmemAddReservation( void *addr, size_t size )
{
    VirtmemReservation *reservation;

    for (reservation = reservations; reservation; reservation = reservation->next)
        assert( !host_ranges_overlap( reservation->addr, reservation->size, addr, size ) );
    reservation = calloc( 1, sizeof(*reservation) );
    reservation->addr = addr;
    reservation->size = size;
    reservation->next = reservations;
    reservations = reservation;
    reservation_count++;
    return reservation;
}
static void virtmemRemoveReservation( VirtmemReservation *reservation )
{
    VirtmemReservation **ptr;

    for (ptr = &reservations; *ptr != reservation; ptr = &(*ptr)->next) assert( *ptr );
    *ptr = reservation->next;
    free( reservation );
    reservation_count--;
}
static size_t code_pool_size;
static void *virtmemFindCodeMemory( size_t size, size_t align )
{
    void *addr = code_pool + code_pool_used;

    (void)align;
    code_pool_used += (size + HOST_PAGE - 1) / HOST_PAGE * HOST_PAGE + HOST_PAGE;
    assert( code_pool_used <= code_pool_size );
    return addr;
}

/* The kernel calls, through the stand-in. */
static int map_code_memory_range( void *addr, void *source, size_t size, int prot, int map_errno )
{
    (void)prot;
    if (host_anchor( addr, source, size )) return 0;
    errno = map_errno;
    return -1;
}
static int unmap_code_memory_range( void *addr, void *source, size_t size )
{
    return host_unanchor( addr, source, size );
}
static Result svcMapProcessMemory( void *dst, Handle process, u64 src, u64 size )
{
    assert( process == 1 );
    return host_alias( dst, (void *)(uintptr_t)src, size ) ? 1 : 0;
}
static Result svcUnmapProcessMemory( void *dst, Handle process, u64 src, u64 size )
{
    assert( process == 1 );
    return host_unalias( dst, (void *)(uintptr_t)src, size ) ? 1 : 0;
}
'''
# The anchor region's size, and the commit granularity, from the real source:
# the host stand-in gets PROT_* and MAP_* from <sys/mman.h> instead.
# The commit granularity horizon.c reserves with, and the anchor region's size
# and count, from the real source: the host stand-in gets PROT_* and MAP_* from
# <sys/mman.h> instead.
fixture += '\n'.join(re.findall(r'^#define HORIZON_COMMIT_\w+ .*$',
                                (root / 'dlls/ntdll/unix/horizon_mman.h').read_text(), re.M)) + '\n'
fixture += re.search(r'^#define HORIZON_ANCHOR_REGION .*$', s, re.M)[0] + '\n'
fixture += re.search(r'^#define HORIZON_ANCHOR_REGIONS .*$', s, re.M)[0] + '\n'
fixture += block(r'^static struct \{ char \*start, \*end, \*cursor; \} anchor_regions\[[^\]]*\];')
fixture += block(r'^static unsigned int anchor_region_count;')
fixture += block(r'^static char \*anchor_region, \*anchor_region_end;')
fixture += block(r'^enum horizon_section_state\n\{.*?^\};')
fixture += block(r'^struct horizon_native_code\n\{.*?^\};')
fixture += '''
static int allow_native_view, native_views;
static void *virtual_alloc_horizon_native(size_t size, void **token);
static void virtual_free_horizon_native(void *token);
'''
for name in ['horizon_backing', 'horizon_mapping']:
    fixture += block(r'^struct ' + name + r'\n\{.*?^\};')
a = s.index('static struct horizon_backing backing_slots[')
b = s.index('void horizon_memory_pool_stats(', a)
fixture += s[a:b]
fixture += function('static int compare_mapping(')
fixture += block(r'^static struct rb_tree mappings = .*?;')
fixture += r'''
/* Backed memory and its protection are other tests' business. */
static int split_backing_mapping( struct horizon_mapping *m, char *start, size_t size )
{ (void)m; (void)start; (void)size; abort(); }
static struct horizon_mapping *split_backing_mapping_metadata( struct horizon_mapping *m, char *start, size_t size )
{ (void)m; (void)start; (void)size; abort(); }
static int protect_code_mapping( struct horizon_mapping *m, int prot ) { (void)m; (void)prot; abort(); }
static int protect_reservation_mapping( struct horizon_mapping *m, char *start, size_t size, int prot )
{ (void)m; (void)start; (void)size; (void)prot; abort(); }
static int map_backing_at( void *addr, size_t size, int prot, int fd, off_t offset, int flags, int map_errno )
{ (void)addr; (void)size; (void)prot; (void)fd; (void)offset; (void)flags; (void)map_errno; abort(); }
'''
for name in ['static void list_add_mapping(', 'static void list_remove_mapping(',
             'static struct horizon_mapping *find_overlap_mapping(', 'static struct horizon_mapping *alloc_mapping(',
             'static VirtmemReservation *reserve_fixed_range_locked(', 'static VirtmemReservation *reserve_fixed_range(',
             'static void remove_reservation_locked(', 'static void remove_reservation(',
             'static void *find_anchor_run_locked(', 'static void *find_anchor_region_locked(', 'static void *find_anchor_address_locked(', 'static int replace_reservation_mapping(', 'static int change_reservation_mapping(', 'static int split_reservation_mapping(', 'static size_t page_align_size(',
             'void *horizon_reserve_native_code(', 'void horizon_release_native_code(',
             'static void section_failure(', 'static void *horizon_section_anchor(', 'static int horizon_section_unanchor(',
             'static int horizon_section_alias(', 'static int horizon_section_unalias(']:
    fixture += function(name)
fixture += block(r'^static const struct horizon_memfile_ops horizon_section_ops =\n\{.*?^\};')
for name in ['static struct horizon_mapping *alloc_section_range(', 'static void free_section_range(',
             'static struct horizon_mapping *section_range_piece(', 'static int change_section_range(',
             'static int protect_section_range(', 'static int unmap_range_locked(',
             'static int protect_range_locked(', 'static void *horizon_mmap_section(']:
    fixture += function(name)
fixture += r'''
#define P HOST_PAGE
#define RW (PROT_READ | PROT_WRITE)

static void check_tree(void)
{
    struct rb_entry *entry;
    struct horizon_mapping *prev = NULL;
    int count = 0;

    for (entry = rb_head( mappings.root ); entry; entry = rb_next( entry ))
    {
        struct horizon_mapping *mapping = RB_ENTRY_VALUE( entry, struct horizon_mapping, entry );

        if (mapping->reservation)
            assert( mapping->reservation->addr == mapping->addr &&
                    mapping->reservation->size == mapping->size );
        else
        {
            /* An anchor packed into a region is covered by the one
             * reservation that region itself holds. */
            unsigned int i;
            int inside = 0;

            assert( mapping->section_state == SECTION_ANCHOR );
            for (i = 0; i < anchor_region_count; i++)
                inside |= (char *)mapping->addr >= anchor_regions[i].start &&
                          (char *)mapping->addr + mapping->size <= anchor_regions[i].end;
            assert( inside );
            count--;
        }
        assert( !mapping->section == (mapping->section_state == SECTION_NONE || mapping->section_state == SECTION_ANCHOR || mapping->section_state == SECTION_NATIVE) );
        if (prev) assert( (char *)prev->addr + prev->size <= (char *)mapping->addr );
        prev = mapping;
        count++;
    }
    assert( count + (int)anchor_region_count == reservation_count );
}

static struct horizon_mapping *entry_at( void *addr )
{
    return find_overlap_mapping( addr, 1 );
}

static struct horizon_mapping *any_anchor(void)
{
    struct rb_entry *entry;

    for (entry = rb_head( mappings.root ); entry; entry = rb_next( entry ))
    {
        struct horizon_mapping *mapping = RB_ENTRY_VALUE( entry, struct horizon_mapping, entry );
        if (mapping->section_state == SECTION_ANCHOR) return mapping;
    }
    return NULL;
}

/* What Wine does before mapping a view over its address with MAP_FIXED. */
static void reserve_view( void *addr, size_t size )
{
    struct horizon_mapping *mapping = alloc_mapping( addr, size, NULL, 0, reserve_fixed_range( addr, size ), PROT_NONE );

    list_add_mapping( mapping );
}

static void *virtual_alloc_horizon_native(size_t size, void **token)
{
    void *addr = (void *)0x90000000;
    assert(!pthread_mutex_trylock(&mapping_mutex));
    pthread_mutex_unlock(&mapping_mutex);
    *token = NULL;
    if (!allow_native_view) return NULL;
    assert(!native_views && !find_overlap_mapping(addr, size));
    reserve_view(addr, size);
    *token = entry_at(addr);
    ++native_views;
    return addr;
}

static void virtual_free_horizon_native(void *token)
{
    struct horizon_mapping *mapping = token;
    assert(!pthread_mutex_trylock(&mapping_mutex));
    assert(native_views == 1 && mapping->section_state == SECTION_NONE);
    assert(!unmap_range_locked(mapping->addr, mapping->size));
    --native_views;
    pthread_mutex_unlock(&mapping_mutex);
}

int main(void)
{
    struct horizon_memfile *file, *reserved;
    unsigned char *views, *a, *b, *c, buffer[8];
    struct horizon_mapping *anchor;
    int before_reservations, before_anchors;

    {
        void *token, *other, *region;
        horizon_native_window_start = (void *)0x20000000;
        horizon_native_window_end = (void *)0x40000000;
        native_blocks[0].addr = 0x20000000;
        native_blocks[0].size = 0x100000;
        native_block_count = 1;
        region = horizon_reserve_native_code( 0x10000000, &token );
        assert( region == (void *)0x20100000 && token );
        assert( entry_at(region)->section_state == SECTION_NATIVE );
        assert( unmap_range_locked(region, P) == -1 && errno == EBUSY );
        assert( protect_range_locked(region, P, RW) == -1 );
        assert( !horizon_reserve_native_code(0x10000000, &other) && !other );
        allow_native_view = 1;
        assert( horizon_reserve_native_code(0x10000000, &other) == (void *)0x90000000 );
        assert( native_views == 1 && entry_at((void *)0x90000000)->section_state == SECTION_NATIVE );
        assert( unmap_range_locked((void *)0x90000000, P) == -1 && errno == EBUSY );
        assert( protect_range_locked((void *)0x90000000, P, RW) == -1 );
        check_tree();
        horizon_release_native_code(other);
        assert(!native_views && !entry_at((void *)0x90000000));
        allow_native_view = 0;
        check_tree();
        horizon_release_native_code(token);
        assert( !mappings.root && !reservation_count );
        {
            void *tokens[8], *addresses[8];
            const size_t sizes[] = { 16, 16, 32, 32, 16, 16, 16, 16 };
            unsigned int i;
            horizon_native_window_end = (void *)0x30000000;
            native_block_count = 4;
            for (i = 0; i < 4; ++i)
            {
                native_blocks[i].addr = 0x20000000 + i * 0x4000000;
                native_blocks[i].size = P;
            }
            assert( !horizon_reserve_native_code(0x4000000, &other) && !other );
            for (i = 0; i < 8; ++i)
            {
                addresses[i] = horizon_reserve_native_code(sizes[i] * 0x100000 + 2 * P, &tokens[i]);
                assert( addresses[i] && tokens[i] );
                check_tree();
            }
            horizon_release_native_code(tokens[2]);
            assert( horizon_reserve_native_code(sizes[2] * 0x100000 + 2 * P, &tokens[2]) == addresses[2] );
            for (i = 0; i < 8; ++i) horizon_release_native_code(tokens[i]);
            assert( !mappings.root && !reservation_count );
        }
        {
            void *tokens[9];
            const size_t sizes[] = { 0x2000000, 0x6000, 0x6000,
                0x1002000, 0x1002000, 0x2002000, 0x2002000, 0x4002000, 0x4002000 };
            unsigned int i;
            horizon_native_window_end = (void *)0x40000000;
            native_blocks[0].addr = 0x26b20000;
            native_blocks[0].size = P;
            native_blocks[1].addr = 0x3381f000;
            native_blocks[1].size = P;
            native_blocks[2].addr = 0x344f1000;
            native_blocks[2].size = 0x100000;
            native_blocks[3].addr = 0x296c3000;
            native_blocks[3].size = 0xc000;
            native_block_count = 44;
            for (i = 4; i < native_block_count; ++i)
            {
                native_blocks[i].addr = 0x3fefc000 - (i - 4) * 0x104000;
                native_blocks[i].size = 0x100000;
            }
            for (i = 0; i < 9; ++i)
            {
                assert( horizon_reserve_native_code(sizes[i], &tokens[i]) && tokens[i] );
                check_tree();
            }
            for (i = 0; i < 9; ++i) horizon_release_native_code(tokens[i]);
            assert( !mappings.root && !reservation_count );
        }
        native_block_count = 0;
        horizon_native_window_start = horizon_native_window_end = NULL;
    }

    code_pool_size = 3 * HORIZON_ANCHOR_REGION + 256 * P;  /* room for the extra regions */
    code_pool = host_reserve( code_pool_size );
    views = host_reserve( 64 * P );
    assert( (file = horizon_memfile_alloc( 8 * P, 0, &horizon_section_ops )) );

    /* a: pages 1-4 over Wine's reservation; b: pages 3-6 wherever. */
    reserve_view( views, 4 * P );
    a = horizon_mmap_section( views, 4 * P, RW, MAP_FIXED | MAP_SHARED, file, 1 * P );
    assert( a == views && entry_at( a )->section_state == SECTION_ALIASED );
    check_tree();
    assert( (b = horizon_mmap_section( NULL, 4 * P, RW, MAP_SHARED, file, 3 * P )) != MAP_FAILED );
    check_tree();
    /* The anchors are packed into the region end to end, not spread through
     * the address space the runtime keeps for its own mappings. */
    {
        struct rb_entry *e;
        char *next = anchor_region;
        int anchors = 0;

        for (e = rb_head( mappings.root ); e; e = rb_next( e ))
        {
            struct horizon_mapping *m = RB_ENTRY_VALUE( e, struct horizon_mapping, entry );

            if (m->section_state != SECTION_ANCHOR) continue;
            assert( (char *)m->addr == next );
            next = (char *)m->addr + m->size;
            anchors++;
        }
        assert( anchors && next <= anchor_region_end );
    }
    {
        char *cursor = anchor_regions[0].cursor;
        native_blocks[0].addr = (uintptr_t)(cursor + P);
        native_blocks[0].size = P;
        native_block_count = 1;
        assert( find_anchor_run_locked(4 * P) == cursor + 2 * P );
        native_blocks[0].addr = (uintptr_t)anchor_regions[0].start;
        native_blocks[0].size = HORIZON_ANCHOR_REGION;
        assert( !find_anchor_run_locked(P) );
        native_block_count = 0;
    }
    /* A region with no room left takes another one: falling back to libnx for
     * each anchor walks every reservation the process holds. */
    {
        int before = reservation_count;
        void *filler = find_anchor_address_locked( HORIZON_ANCHOR_REGION );
        void *next_region;

        assert( filler && anchor_region_count == 2 );  /* it did not fit in the first */
        assert( reservation_count == before + 1 );     /* one reservation, not one an anchor */
        list_add_mapping( alloc_mapping( filler, HORIZON_ANCHOR_REGION, NULL, 0, NULL, RW ) );
        /* With that region full the packing carries on in the one before it. */
        next_region = find_anchor_address_locked( P );
        assert( next_region && anchor_region_count == 2 );
        assert( (char *)next_region >= anchor_region && (char *)next_region < anchor_region_end );
        list_remove_mapping( entry_at( filler ) );  /* both regions stay reserved */
    }
    /* A region is found by walking the runtime's own map and the kernel's,
     * not by asking libnx for random addresses. */
    {
        char *window = host_reserve( 4 * HORIZON_ANCHOR_REGION );

        horizon_native_window_start = window;
        horizon_native_window_end = window + 4 * HORIZON_ANCHOR_REGION;
        native_blocks[0].addr = (unsigned long long)(uintptr_t)(window + HORIZON_ANCHOR_REGION);
        native_blocks[0].size = HORIZON_ANCHOR_REGION;
        native_block_count = 1;
        assert( find_anchor_region_locked( HORIZON_ANCHOR_REGION ) == window );
        /* Past one of ours and one of libnx's. */
        list_add_mapping( alloc_mapping( window, HORIZON_ANCHOR_REGION, NULL, 0, NULL, RW ) );
        assert( find_anchor_region_locked( HORIZON_ANCHOR_REGION ) == window + 2 * HORIZON_ANCHOR_REGION );
        /* Nothing that large left in the window. */
        assert( !find_anchor_region_locked( 3 * HORIZON_ANCHOR_REGION ) );
        list_remove_mapping( entry_at( window ) );
        {
            char *saved_start = anchor_regions[0].start, *saved_end = anchor_regions[0].end;
            anchor_regions[0].start = window;
            anchor_regions[0].end = window + HORIZON_ANCHOR_REGION;
            assert( find_anchor_region_locked(HORIZON_ANCHOR_REGION) == window + 2 * HORIZON_ANCHOR_REGION );
            anchor_regions[0].start = saved_start;
            anchor_regions[0].end = saved_end;
        }
        list_add_mapping( alloc_mapping(window, 5 * HORIZON_ANCHOR_REGION, NULL, 0, NULL, RW) );
        assert( !find_anchor_region_locked(P) );
        list_remove_mapping( entry_at(window) );
        native_block_count = 0;
        horizon_native_window_start = horizon_native_window_end = NULL;
    }
    b[0] = 0x11;
    assert( a[2 * P] == 0x11 );
    a[3 * P + 1] = 0x22;
    assert( b[P + 1] == 0x22 );
    b[2 * P] = 0x66;  /* page 5, only b's */
    errno = 0;
    assert( horizon_mmap_section( a, P, RW, MAP_FIXED_NOREPLACE | MAP_SHARED, file, 0 ) == MAP_FAILED && errno == EEXIST );

    /* The descriptor goes before the views. */
    horizon_memfile_unref( file );
    b[1] = 0x12;
    assert( a[2 * P + 1] == 0x12 );

    /* a's page 2 goes PROT_NONE; written meanwhile, it comes back with the data. */
    assert( !protect_range_locked( a + P, P, PROT_NONE ) );
    assert( entry_at( a + P )->section_state == SECTION_HOLE && entry_at( a )->section_state == SECTION_ALIASED &&
            entry_at( a + 2 * P )->section_state == SECTION_ALIASED );
    check_tree();
    assert( horizon_memfile_pwrite( file, "hole", 4, 2 * P ) == 4 );
    assert( !protect_range_locked( a + P, P, RW ) );
    assert( entry_at( a + P )->section_state == SECTION_ALIASED && !memcmp( a + P, "hole", 4 ) );
    check_tree();
    a[P + 4] = 0x44;
    assert( horizon_memfile_pread( file, (char *)buffer, 1, 2 * P + 4 ) == 1 && buffer[0] == 0x44 );

    /* Read-only asks leave the pages read-write, said once. */
    assert( !protect_range_locked( a, 4 * P, PROT_READ ) && !protect_range_locked( a, 4 * P, PROT_READ ) );
    assert( traces == 1 );
    a[0] = 1;

    /* Anchors: nothing unmaps, reprotects or maps over them. */
    assert( (anchor = any_anchor()) );
    errno = 0;
    assert( unmap_range_locked( anchor->addr, P ) == -1 && errno == EBUSY );
    assert( protect_range_locked( anchor->addr, P, PROT_NONE ) == -1 );
    errno = 0;
    assert( horizon_mmap_section( anchor->addr, P, RW, MAP_FIXED | MAP_SHARED, file, 0 ) == MAP_FAILED &&
            errno == EBUSY );
    check_tree();

    /* The kernel refuses to take a range away: it stays as it was. */
    host_fail_unalias = 1;
    assert( unmap_range_locked( a + 2 * P, P ) == -1 );
    assert( protect_range_locked( a + 3 * P, P, PROT_NONE ) == -1 );
    host_fail_unalias = 0;
    assert( entry_at( a + 2 * P )->section_state == SECTION_ALIASED && a[2 * P] == 0x11 );
    check_tree();

    /* The kernel refuses a new view's first piece: nothing is left behind. */
    before_reservations = reservation_count;
    before_anchors = host_live_anchors();
    host_fail_alias_countdown = 1;
    errno = 0;
    assert( horizon_mmap_section( NULL, 2 * P, RW, MAP_SHARED, file, 6 * P ) == MAP_FAILED && errno == ENOMEM );
    host_fail_alias_countdown = 0;
    assert( reservation_count == before_reservations && host_live_anchors() == before_anchors );
    check_tree();

    /* b loses its second page, then everything in one call across its pieces;
     * page 5's anchor goes with it and its data stays. */
    assert( !unmap_range_locked( b + P, P ) );
    assert( !entry_at( b + P ) && entry_at( b )->section_state == SECTION_ALIASED );
    check_tree();
    assert( !unmap_range_locked( b, 4 * P ) );
    check_tree();
    assert( host_live_anchors() == 1 );
    assert( horizon_memfile_pread( file, (char *)buffer, 1, 5 * P ) == 1 && buffer[0] == 0x66 );

    /* a goes across its three entries: the last reference frees the memory. */
    assert( !unmap_range_locked( a, 4 * P ) );
    assert( !mappings.root && reservation_count == (int)anchor_region_count );
    assert( !host_live_anchors() && !host_alias_count );
    assert( host_pages_live == 0 );

    /* A view mapped PROT_NONE has no pages until it is made accessible. */
    assert( (reserved = horizon_memfile_alloc( 2 * P, 1, &horizon_section_ops )) );
    assert( (c = horizon_mmap_section( NULL, 2 * P, PROT_NONE, MAP_SHARED, reserved, 0 )) != MAP_FAILED );
    assert( entry_at( c )->section_state == SECTION_HOLE && !host_live_anchors() );
    assert( !protect_range_locked( c, 2 * P, RW ) && host_live_anchors() == 1 );
    c[P] = 0x77;
    assert( horizon_memfile_pread( reserved, (char *)buffer, 1, P ) == 1 && buffer[0] == 0x77 );
    check_tree();
    assert( !unmap_range_locked( c, 2 * P ) );
    horizon_memfile_unref( reserved );
    assert( !mappings.root && reservation_count == (int)anchor_region_count );
    assert( !host_live_anchors() && host_pages_live == 0 );

    puts( "Section views in horizon.c: Wine's fixed maps and placed maps, overlapping writes, close before unmap, "
          "PROT_NONE and back, anchors kept from others, kernel refusals and unmapping in pieces passed" );
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'sections.c'
    c.write_text(fixture)
    exe = Path(tmp) / 'sections'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    '-I' + str(root / 'wine-nx-probe/tests'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
