#!/usr/bin/env python3
"""Run horizon.c's protect_range_locked against stand-ins for everything it
maps with, and check that committing part of a reservation maps a run of pages
once. Horizon shares 20000 memory blocks between all applications, and a
mapping a page ran svcMapProcessCodeMemory out of them on hardware, after which
every commit failed and Fallout New Vegas retried for ever on one core."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(name):
    m = re.search('^' + re.escape(name) + r'[^;{]*?\)\s*\n\{', s, re.M | re.S)
    assert m, name
    i = m.end()
    depth = 1
    while depth:
        depth += (s[i] == '{') - (s[i] == '}')
        i += 1
    return s[m.start():i] + '\n'


fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "horizon_mman.h"

typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define horizon_trace(...) ((void)0)
#define HORIZON_POOL_PAGE 4096
#define HORIZON_POOL_ARENA ((size_t)2 * 1048576)

enum horizon_section_state { SECTION_NONE, SECTION_ALIASED, SECTION_HOLE, SECTION_ANCHOR, SECTION_NATIVE };
struct horizon_backing { struct { int detached; } swap; int swap_excluded; };
static struct horizon_backing backing;
struct horizon_mapping
{
    void *addr;
    size_t size;
    int prot;
    struct horizon_backing *backing;
    void *section;
    unsigned char section_state;
    void *reservation;    /* set while the range is only reserved */
    int swap_managed, swap_excluded;
};

#define MAPS 64
static struct horizon_mapping maps[MAPS];
static unsigned int map_count;
static unsigned int maps_made, protects;
static char *last_map_addr;
static size_t last_map_size;
static int last_map_prot;

/* The lowest mapping that overlaps, as the real tree walk returns. */
static struct horizon_mapping *find_overlap_mapping( void *addr, size_t size )
{
    char *start = addr, *end = start + size;
    struct horizon_mapping *found = NULL;
    unsigned int i;

    for (i = 0; i < map_count; i++)
    {
        if ((char *)maps[i].addr >= end || start >= (char *)maps[i].addr + maps[i].size) continue;
        if (!found || maps[i].addr < found->addr) found = &maps[i];
    }
    return found;
}

static struct horizon_mapping *add_map( char *addr, size_t size, int prot, void *reservation )
{
    assert( map_count < MAPS );
    maps[map_count] = (struct horizon_mapping){ .addr = addr, .size = size, .prot = prot,
        .backing = reservation ? NULL : &backing, .reservation = reservation };
    return &maps[map_count++];
}

static void drop_map( struct horizon_mapping *m )
{
    *m = maps[--map_count];
}

/* The commit itself: one call for the whole chunk, mapped inaccessible. */
static int replace_reservation_mapping( struct horizon_mapping *m, char *start, size_t size,
                                        int prot, BOOL map_range )
{
    char *m_start = m->addr, *m_end = m_start + m->size;  /* void * converts */
    void *reservation = m->reservation;

    assert( prot == PROT_NONE && map_range );
    maps_made++;
    last_map_addr = start;
    last_map_size = size;
    last_map_prot = prot;
    drop_map( m );
    if (m_start < start) add_map( m_start, start - m_start, PROT_NONE, reservation );
    if (start + size < m_end) add_map( start + size, m_end - start - size, PROT_NONE, reservation );
    add_map( start, size, prot, NULL );
    return 0;
}

/* Taking part of a mapped range back is only a protection change. */
static struct horizon_mapping *split_backing_mapping_metadata( struct horizon_mapping *m,
                                                               char *start, size_t size )
{
    char *m_start = m->addr, *m_end = m_start + m->size;
    int prot = m->prot;

    if (m_start == start && m->size == size) return m;
    drop_map( m );
    if (m_start < start) add_map( m_start, start - m_start, prot, NULL );
    if (start + size < m_end) add_map( start + size, m_end - start - size, prot, NULL );
    return add_map( start, size, prot, NULL );
}

static int protect_code_mapping( struct horizon_mapping *m, int prot )
{
    protects++;
    m->prot = prot;
    return 0;
}

static int protect_section_range( struct horizon_mapping *m, char *start, size_t size, int prot )
{ (void)m; (void)start; (void)size; (void)prot; abort(); }
static int protect_reservation_mapping( struct horizon_mapping *m, char *start, size_t size, int prot )
{
    assert( start == m->addr && size == m->size );
    m->prot = prot;
    return 0;
}
#ifdef WINE_NX_SWAP_POC
static int swap_active;
static int horizon_swap_enabled(void) { return swap_active; }
static int swap_restore_locked( struct horizon_backing *b ) { (void)b; abort(); }
#endif

static size_t anonymous_max, anonymous_bytes, allocation_limit = SIZE_MAX, rollback_size;
static unsigned int anonymous_maps, fail_after;
static int map_backing_at( void *addr, size_t size, int prot, int fd, off_t offset, int flags, int map_errno )
{
    (void)addr; (void)prot; (void)fd; (void)offset; (void)flags; (void)map_errno;
    if (fail_after && anonymous_maps == fail_after) { errno = EIO; return -1; }
    if (size > allocation_limit) { errno = ENOMEM; return -1; }
    anonymous_max = max( anonymous_max, size );
    anonymous_bytes += size;
    anonymous_maps++;
    return 0;
}
static int unmap_range_locked( void *addr, size_t size )
{ (void)addr; rollback_size = size; return 0; }
'''

fixture += function('static int protect_range_locked(')
fixture += function('static int map_anonymous_backings(')

fixture += r'''
#define RW (PROT_READ | PROT_WRITE)

static int prot_at( char *addr )
{
    struct horizon_mapping *m = find_overlap_mapping( addr, 1 );
    return m ? m->prot : -1;
}

static int reserved_at( char *addr )
{
    struct horizon_mapping *m = find_overlap_mapping( addr, 1 );
    return m && m->reservation != NULL;
}

static void anonymous_chunks( int prot, int flags, size_t expected )
{
    anonymous_max = anonymous_bytes = anonymous_maps = 0;
    assert( !map_anonymous_backings( (void *)0x60000000, 16 * 1048576, prot, flags, EINVAL ) );
    assert( anonymous_max == expected && anonymous_bytes == 16 * 1048576 );
    if (allocation_limit == SIZE_MAX) assert( anonymous_maps == 16 * 1048576 / expected );
}

int main(void)
{
    char *base = (char *)0x40000000;
    char *small = (char *)0x50000000;

    /* A page commit maps the 64 KB chunk around it, inaccessible, and then
     * raises only the page asked for. */
    add_map( base, 0x200000, PROT_NONE, (void *)1 );
    maps_made = protects = 0;
    assert( !protect_range_locked( base + 0x35000, 0x1000, RW ) );
    assert( maps_made == 1 && last_map_addr == base + 0x30000 && last_map_size == HORIZON_COMMIT_CHUNK );
    assert( last_map_prot == PROT_NONE );
    assert( prot_at( base + 0x35000 ) == RW );
    assert( prot_at( base + 0x30000 ) == PROT_NONE && prot_at( base + 0x3f000 ) == PROT_NONE );
    assert( protects == 1 );
    /* Outside the chunk nothing was committed. */
    assert( reserved_at( base ) && reserved_at( base + 0x40000 ) );

    /* The next page in the same chunk maps nothing new. */
    maps_made = protects = 0;
    assert( !protect_range_locked( base + 0x36000, 0x1000, RW ) );
    assert( !maps_made && protects == 1 && prot_at( base + 0x36000 ) == RW );
    assert( prot_at( base + 0x35000 ) == RW && prot_at( base + 0x37000 ) == PROT_NONE );

    /* A commit crossing the chunk takes both chunks, still one mapping each. */
    maps_made = protects = 0;
    assert( !protect_range_locked( base + 0x3f000, 0x2000, RW ) );
    assert( maps_made == 1 && last_map_addr == base + 0x40000 && last_map_size == HORIZON_COMMIT_CHUNK );
    assert( prot_at( base + 0x3f000 ) == RW && prot_at( base + 0x40000 ) == RW );
    assert( prot_at( base + 0x41000 ) == PROT_NONE && reserved_at( base + 0x50000 ) );

    /* A reservation smaller than a chunk is mapped whole, once. */
    map_count = 0;
    add_map( small, 0x8000, PROT_NONE, (void *)1 );
    maps_made = protects = 0;
    assert( !protect_range_locked( small + 0x2000, 0x1000, RW ) );
    assert( maps_made == 1 && last_map_addr == small && last_map_size == 0x8000 );
    assert( prot_at( small + 0x2000 ) == RW && prot_at( small ) == PROT_NONE );
    maps_made = 0;
    assert( !protect_range_locked( small + 0x7000, 0x1000, RW ) );
    assert( !maps_made && prot_at( small + 0x7000 ) == RW );

    /* Asking for PROT_NONE over a reservation still commits nothing. */
    map_count = 0;
    add_map( base, 0x200000, PROT_NONE, (void *)1 );
    maps_made = 0;
    assert( !protect_range_locked( base + 0x35000, 0x1000, PROT_NONE ) );
    assert( !maps_made && reserved_at( base + 0x35000 ) );

#ifdef WINE_NX_SWAP_POC
    swap_active = 1;
    anonymous_chunks( RW, MAP_PRIVATE, HORIZON_POOL_ARENA );
    /* Executable and shared allocations cannot be paged out. */
    anonymous_chunks( RW | PROT_EXEC, MAP_PRIVATE, 16 * 1048576 );
    anonymous_chunks( PROT_READ | PROT_EXEC, MAP_PRIVATE, 16 * 1048576 );
    anonymous_chunks( RW, MAP_SHARED, 16 * 1048576 );
    for (unsigned int kind = 0; kind < 4; kind++)
    {
        int prot = kind == 2 ? RW | PROT_EXEC : RW;
        struct horizon_mapping *m;
        map_count = maps_made = 0;
        m = add_map( base, 16 * 1048576, PROT_NONE, (void *)1 );
        m->swap_managed = kind != 0;
        m->swap_excluded = kind == 1;
        assert( !protect_range_locked( base, 16 * 1048576, prot ) );
        if (kind == 3) assert( !maps_made && reserved_at( base ) && prot_at( base ) == RW );
        else assert( maps_made == 1 && last_map_size == 16 * 1048576 && prot_at( base ) == prot );
    }
    swap_active = 0;
#endif
    anonymous_chunks( RW, MAP_PRIVATE, 16 * 1048576 );
    anonymous_chunks( RW | PROT_EXEC, MAP_PRIVATE, 16 * 1048576 );
    allocation_limit = 4 * 1048576;
    anonymous_chunks( RW | PROT_EXEC, MAP_PRIVATE, allocation_limit );
    fail_after = 1;
    anonymous_maps = rollback_size = 0;
    assert( map_anonymous_backings( base, 16 * 1048576, RW | PROT_EXEC, MAP_PRIVATE, EINVAL ) == -1 );
    assert( errno == EIO && rollback_size == allocation_limit );

    puts( "Commit chunks: one mapping a chunk, uncommitted pages inaccessible, "
          "pageable limits, native allocation sizes and rollback passed" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'commit_chunks.c'
    c.write_text(fixture)
    exe = Path(tmp) / 'commit_chunks'
    for defines in ([], ['-DWINE_NX_SWAP_POC']):
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *defines,
                        '-I' + str(root / 'dlls/ntdll/unix'), str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
