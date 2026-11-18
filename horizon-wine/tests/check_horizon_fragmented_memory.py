#!/usr/bin/env python3
"""Exercise fragmented section memory, section rollback on alias failure, and
chunked anonymous backing fallback/rollback on the host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def extract_function(source, name):
    match = re.search(rf'^static [^\n;]*\b{re.escape(name)}\([^;]*?\)\n\{{', source, re.M)
    if match is None:
        raise RuntimeError(f'function not found: {name}')
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


defs = '\n'.join(re.findall(r'^#define HORIZON_ANONYMOUS_BACKING_(?:MIN|UNIT) .*', horizon, re.M))
map_anon_src = extract_function(horizon, 'map_anonymous_backings')
mmap_sec_funcs = '\n\n'.join(
    extract_function(horizon, name)
    for name in ('alloc_section_range', 'free_section_range', 'horizon_mmap_section')
)

fixture = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "horizon_memfile_host.h"
#include "horizon_memfile.h"

#define SECTION_NONE    0
#define SECTION_ANCHOR  1
#define SECTION_ALIASED 2
#define SECTION_HOLE    3
#define HORIZON_POOL_PAGE  4096
#define HORIZON_POOL_ARENA (64 * 1024 * 1024)
typedef uintptr_t ULONG_PTR;
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

typedef struct { void *addr; size_t size; int live; } VirtmemReservation;

struct horizon_mapping
{
    void *addr;
    size_t size;
    struct horizon_memfile *section;
    size_t section_offset;
    unsigned char section_state;
    VirtmemReservation *reservation;
    int prot;
    struct horizon_mapping *next;
};

static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_mapping *mapping_head;
static unsigned int section_view_maps;
static int trace_count;

static void horizon_trace( const char *fmt, ... )
{
    (void)fmt;
    trace_count++;
}

static size_t page_align_size( size_t size )
{
    size_t p = HOST_PAGE;
    return (size + p - 1) & ~(p - 1);
}

/* Host memfile operations using horizon_memfile_host.h */
static void *host_sec_anchor( void *source, size_t size, void **token )
{
    void *addr;
    if (host_fail_anchor) { errno = ENOMEM; return NULL; }
    addr = host_anchor( NULL, source, size );
    *token = addr;
    return addr;
}

static int host_sec_unanchor( void *addr, void *source, size_t size, void *token )
{
    (void)token;
    if (host_fail_unanchor) { errno = EINVAL; return -1; }
    return host_unanchor( addr, source, size );
}

static int host_sec_alias( void *dst, void *src, size_t size )
{
    return host_alias( dst, src, size );
}

static int host_sec_unalias( void *dst, void *src, size_t size )
{
    return host_unalias( dst, src, size );
}

static const struct horizon_memfile_ops test_section_ops =
{
    host_sec_anchor, host_sec_unanchor, host_sec_alias, host_sec_unalias,
};

/* --- Mock chunk allocator for map_anonymous_backings --- */
struct mock_chunk { char *addr; size_t size; };
static struct mock_chunk mock_chunks[256];
static unsigned int mock_chunk_count;
static size_t mock_fail_above;
static int mock_fail_after_count = -1;
static int mock_unmap_calls;

static int map_backing_at( void *addr, size_t size, int prot, int fd, off_t off, int flags, int map_errno )
{
    (void)prot; (void)fd; (void)off; (void)flags; (void)map_errno;
    if (mock_fail_after_count == 0) { errno = EIO; return -1; }
    if (mock_fail_above && size > mock_fail_above) { errno = ENOMEM; return -1; }
    if (mock_fail_after_count > 0) mock_fail_after_count--;
    assert( mock_chunk_count < 256 );
    mock_chunks[mock_chunk_count].addr = addr;
    mock_chunks[mock_chunk_count].size = size;
    mock_chunk_count++;
    return 0;
}

static int unmap_range_locked( void *addr, size_t size )
{
    char *start = addr;
    char *end = start + size;
    unsigned int i, out = 0;
    size_t removed = 0;

    mock_unmap_calls++;
    for (i = 0; i < mock_chunk_count; i++)
    {
        if (mock_chunks[i].addr >= start && mock_chunks[i].addr + mock_chunks[i].size <= end)
            removed += mock_chunks[i].size;
        else
            mock_chunks[out++] = mock_chunks[i];
    }
    mock_chunk_count = out;
    assert( removed == size );
    return 0;
}

/* --- Mock reservation/mapping pool for horizon_mmap_section --- */
static int active_reservations;
static int fail_alloc_mapping_after = -1;

static VirtmemReservation *reserve_fixed_range_locked( void *addr, size_t size )
{
    VirtmemReservation *r = calloc( 1, sizeof(*r) );
    assert( r );
    r->addr = addr;
    r->size = size;
    r->live = 1;
    active_reservations++;
    return r;
}

static void remove_reservation( VirtmemReservation *r )
{
    assert( r && r->live );
    r->live = 0;
    active_reservations--;
    free( r );
}

static VirtmemReservation *reserve_fixed_range( void *addr, size_t size )
{
    return reserve_fixed_range_locked( addr, size );
}

static void *find_free_address_range( size_t size )
{
    return host_reserve( size );
}

static void *virtmemFindCodeMemory( size_t size, size_t align )
{
    (void)align;
    return host_reserve( size );
}

static struct horizon_mapping *find_overlap_mapping( void *addr, size_t size )
{
    struct horizon_mapping *m;
    char *start = addr, *end = start + size;
    for (m = mapping_head; m; m = m->next)
        if ((char *)m->addr < end && (char *)m->addr + m->size > start) return m;
    return NULL;
}

static void virtmemLock( void ) {}
static void virtmemUnlock( void ) {}

static struct horizon_mapping *alloc_mapping( void *addr, size_t size, void *f, off_t off,
                                              VirtmemReservation *res, int prot )
{
    struct horizon_mapping *m;
    (void)f; (void)off;
    if (fail_alloc_mapping_after == 0) { errno = ENOMEM; return NULL; }
    if (fail_alloc_mapping_after > 0) fail_alloc_mapping_after--;
    m = calloc( 1, sizeof(*m) );
    if (!m) return NULL;
    m->addr = addr;
    m->size = size;
    m->reservation = res;
    m->prot = prot;
    return m;
}

static void free_mapping( struct horizon_mapping *m )
{
    free( m );
}

static void list_add_mapping( struct horizon_mapping *m )
{
    m->next = mapping_head;
    mapping_head = m;
}

static void list_remove_mapping( struct horizon_mapping *m )
{
    struct horizon_mapping **link;
    for (link = &mapping_head; *link; link = &(*link)->next)
        if (*link == m) { *link = m->next; m->next = NULL; return; }
    assert( 0 );
}

static unsigned int count_mappings( void )
{
    unsigned int n = 0;
    struct horizon_mapping *m;
    for (m = mapping_head; m; m = m->next) n++;
    return n;
}

''' + defs + '\n' + map_anon_src + '\n\n' + mmap_sec_funcs + r'''

static void test_anonymous_backing_chunks( void )
{
    char *base = (char *)0x100000000ULL;
    size_t total = 160 * 1024 * 1024;
    size_t mapped = 0;
    unsigned int i;

    /* 1. Large anonymous map splits downward from 512 MiB down to 64 MiB chunks. */
    mock_chunk_count = 0;
    mock_fail_above = 64 * 1024 * 1024;
    mock_fail_after_count = -1;
    assert( !map_anonymous_backings( base, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, ENOMEM ) );
    assert( mock_chunk_count == 3 );
    for (i = 0; i < mock_chunk_count; i++)
    {
        assert( mock_chunks[i].addr == base + mapped );
        assert( mock_chunks[i].size <= 64 * 1024 * 1024 );
        mapped += mock_chunks[i].size;
    }
    assert( mapped == total );

    /* 2. Mid-range failure rolls back all earlier chunks atomically. */
    mock_chunk_count = 0;
    mock_fail_above = 64 * 1024 * 1024;
    mock_fail_after_count = 2;
    mock_unmap_calls = 0;
    errno = 0;
    assert( map_anonymous_backings( base, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, ENOMEM ) == -1 );
    assert( errno == EIO && mock_unmap_calls == 1 && mock_chunk_count == 0 );

    /* 3. Fragmented below HORIZON_ANONYMOUS_BACKING_UNIT / 1 MiB splits down and maps the full range. */
    mock_chunk_count = 0;
    mock_fail_above = 1024 * 1024;
    mock_fail_after_count = -1;
    assert( !map_anonymous_backings( base, 16 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, ENOMEM ) );
    mapped = 0;
    for (i = 0; i < mock_chunk_count; i++)
    {
        assert( mock_chunks[i].addr == base + mapped );
        assert( mock_chunks[i].size <= 1024 * 1024 );
        mapped += mock_chunks[i].size;
    }
    assert( mapped == 16 * 1024 * 1024 && mock_chunk_count > 16 );

    /* 4. Smallest granularity failure (<= HORIZON_ANONYMOUS_BACKING_MIN) fails cleanly. */
    mock_chunk_count = 0;
    mock_fail_above = 2048;
    mock_fail_after_count = -1;
    errno = 0;
    assert( map_anonymous_backings( base, 16 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, ENOMEM ) == -1 );
    assert( errno == ENOMEM && mock_chunk_count == 0 );
}

static void test_fragmented_section_and_rollback( void )
{
    size_t P = HOST_PAGE;
    struct horizon_memfile *file;
    void *held[4];
    unsigned char write_buf[64], read_buf[64];
    char *v1, *v2;
    unsigned int i;

    /* Fragment the host backing heap with alternating free holes so the next
     * multi-page section spans separate physical chunks (0138). */
    for (i = 0; i < 4; i++) assert( (held[i] = host_alloc_pages( 2 * P )) );
    host_free_pages( held[0], 2 * P );
    host_free_pages( held[2], 2 * P );

    assert( (file = horizon_memfile_alloc( 4 * P, 0, &test_section_ops )) );
    for (i = 0; i < sizeof(write_buf); i++) write_buf[i] = (unsigned char)(0xa0 + i);
    assert( horizon_memfile_pwrite( file, (char *)write_buf, sizeof(write_buf), 2 * P - 16 ) == sizeof(write_buf) );
    assert( horizon_memfile_pread( file, (char *)read_buf, sizeof(read_buf), 2 * P - 16 ) == sizeof(read_buf) );
    assert( !memcmp( write_buf, read_buf, sizeof(write_buf) ) );

    /* Map view 1; check boundary data across backing chunks and refcount preservation. */
    assert( (v1 = horizon_mmap_section( NULL, 4 * P, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0 )) != MAP_FAILED );
    assert( count_mappings() == 1 && mapping_head->section_state == SECTION_ALIASED );
    assert( !memcmp( v1 + 2 * P - 16, write_buf, sizeof(write_buf) ) );

    /* Alias failure on a second view leaves no dangling mapping, reservation, or use count (0136). */
    host_fail_alias_countdown = 1;
    errno = 0;
    assert( horizon_mmap_section( NULL, 4 * P, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0 ) == MAP_FAILED );
    assert( errno == ENOMEM );
    host_fail_alias_countdown = 0;
    assert( count_mappings() == 1 && active_reservations == 1 );

    /* Second view succeeds; release both views and ensure zero leaked anchors/pages. */
    assert( (v2 = horizon_mmap_section( NULL, 4 * P, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0 )) != MAP_FAILED );
    assert( count_mappings() == 2 && active_reservations == 2 );
    v2[3 * P] = 0x5c;
    assert( v1[3 * P] == 0x5c );

    while (mapping_head)
    {
        struct horizon_mapping *m = mapping_head;
        list_remove_mapping( m );
        assert( !horizon_memfile_alias( m->section, m->addr, m->section_offset, m->size, 0 ) );
        remove_reservation( m->reservation );
        assert( !munmap( m->addr, m->size ) );
        free_section_range( m );
    }
    horizon_memfile_unref( file );
    assert( active_reservations == 0 && !host_alias_count && !host_live_anchors() );

    host_free_pages( held[1], 2 * P );
    host_free_pages( held[3], 2 * P );
    assert( host_pages_live == 0 );
}

int main( void )
{
    test_anonymous_backing_chunks();
    test_fragmented_section_and_rollback();
    puts( "Fragmented section views, alias-failure rollback, and chunked anonymous backing fallback passed" );
    return 0;
}
'''

cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'frag_test.c'
    exe = Path(tmp) / 'frag_test'
    src.write_text(fixture)
    subprocess.run([
        cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-variable',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
        '-I' + str(root / 'dlls/ntdll/unix'),
        '-I' + str(root / 'horizon-wine/tests'),
        str(src), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
