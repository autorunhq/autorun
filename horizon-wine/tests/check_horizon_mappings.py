#!/usr/bin/env python3
"""Run the Horizon mapping index (compare_mapping, list_add_mapping,
list_remove_mapping and find_overlap_mapping in dlls/ntdll/unix/horizon.c)
against a brute-force search, and horizon_mmap's choice of path for each flag.

WarCraft III's loading spent a third of its main thread scanning an unsorted
list of every mapping, and Wine's free-area search probed each taken address
with MAP_FIXED_NOREPLACE, which horizon_mmap turned into an allocation elsewhere
that Wine only unmapped again."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()
mman = (root / 'dlls/ntdll/unix/horizon_mman.h').read_text()

def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

mapping_struct = re.search(r'^struct horizon_mapping\n\{.*?^\};', horizon, re.M | re.S).group(0)
tree = re.search(r'^static struct rb_tree mappings = .*?;', horizon, re.M).group(0)
flags = '\n'.join(re.findall(r'^#define MAP_(?:FIXED|FIXED_NOREPLACE|ANON|FAILED|PRIVATE|SHARED)\b.*$', mman, re.M))

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "wine/rbtree.h"
''' + flags + r'''
typedef struct VirtmemReservation VirtmemReservation;
struct horizon_backing;
'''

stubs = r'''
static const char *path_taken;
static void *tryfixed_result;
static int tryfixed_errno;
static unsigned long long alias_start = 0x50000000ull, alias_end = 0x60000000ull;
static int alias_probes;
/* Views of sections with no file have their own test (check_horizon_sections.py). */
struct horizon_memfile;
static struct horizon_memfile *horizon_memfile_from_fd( int fd ) { (void)fd; return NULL; }
static int check_section_syscalls(void) { return -1; }
static void wine_nx_runtime_trace( const char *msg ) { (void)msg; }
static int horizon_in_alias_region( const void *start, size_t size )
{
    unsigned long long begin = (unsigned long long)(size_t)start, end = begin + size;
    return alias_start < alias_end && begin < alias_end && end > alias_start;
}
static void horizon_report_alias_probe( const void *start, size_t size )
{
    (void)start; (void)size;
    alias_probes++;
}
static void *horizon_mmap_section( void *start, size_t size, int prot, int flags,
                                   struct horizon_memfile *section, off_t offset )
{ (void)start; (void)size; (void)prot; (void)flags; (void)section; (void)offset; abort(); }
static void *horizon_mmap_fixed( void *start, size_t size, int prot, int flags, int fd, off_t offset )
{
    path_taken = "fixed";
    return start;
}
static void *horizon_mmap_tryfixed( void *start, size_t size, int prot, int flags, int fd, off_t offset )
{
    path_taken = "tryfixed";
    errno = tryfixed_errno;
    return tryfixed_result;
}
static void *horizon_mmap_alloc( size_t size, int prot, int flags, int fd, off_t offset )
{
    path_taken = "alloc";
    return (void *)0x9000000;
}
'''

tests = r'''
#define PAGE 0x1000
#define SLOTS 4096
static struct horizon_mapping pool[SLOTS];
static int used[SLOTS];

/* The lowest mapping overlapping the range, by scanning every one. */
static struct horizon_mapping *brute_lowest( char *start, char *end )
{
    struct horizon_mapping *best = NULL;
    int i;

    for (i = 0; i < SLOTS; i++)
    {
        char *s, *e;
        if (!used[i]) continue;
        s = pool[i].addr;
        e = s + pool[i].size;
        if (start < e && s < end && (!best || s < (char *)best->addr)) best = &pool[i];
    }
    return best;
}

static unsigned int rng = 12345;
static unsigned int next_random(void) { return rng = rng * 1103515245 + 12345, rng >> 8; }

int main(void)
{
    int i, round, checks = 0;

    /* Slot i may map any part of [i * 16 pages, (i + 1) * 16 pages): never overlapping. */
    for (round = 0; round < 200000; round++)
    {
        int slot = next_random() % SLOTS;

        if (used[slot] && next_random() % 3 == 0)
        {
            list_remove_mapping( &pool[slot] );
            used[slot] = 0;
        }
        else if (!used[slot])
        {
            unsigned int first = next_random() % 16, pages = 1 + next_random() % (16 - first);

            pool[slot].addr = (void *)(0x10000000ul + (unsigned long)slot * 16 * PAGE + first * PAGE);
            pool[slot].size = pages * PAGE;
            list_add_mapping( &pool[slot] );
            used[slot] = 1;
        }
        if (round % 7 == 0)
        {
            char *start = (char *)0x10000000ul + (next_random() % (SLOTS * 16)) * PAGE;
            size_t size = (1 + next_random() % 64) * PAGE;

            assert( find_overlap_mapping( start, size ) == brute_lowest( start, start + size ) );
            checks++;
        }
    }
    printf( "find_overlap_mapping matched the full scan %d times\n", checks );

    /* mprotect's case: a range whose start lies in one mapping while a later
     * mapping in it was added first. The list returned the later one, and
     * protect_range_locked then failed with ENOMEM. */
    for (i = 0; i < SLOTS; i++) if (used[i]) { list_remove_mapping( &pool[i] ); used[i] = 0; }
    assert( !mappings.root );
    pool[1].addr = (void *)0x20004000ul; pool[1].size = 4 * PAGE; list_add_mapping( &pool[1] );
    pool[0].addr = (void *)0x20000000ul; pool[0].size = 4 * PAGE; list_add_mapping( &pool[0] );
    assert( find_overlap_mapping( (void *)0x20002000ul, 8 * PAGE ) == &pool[0] );
    assert( find_overlap_mapping( (void *)0x20004000ul, PAGE ) == &pool[1] );
    assert( !find_overlap_mapping( (void *)0x20008000ul, PAGE ) );
    assert( !find_overlap_mapping( (void *)0x1ffff000ul, PAGE ) );

    /* horizon_mmap: a taken address fails for MAP_FIXED_NOREPLACE, falls back to
     * any address for a plain hint, and MAP_FIXED replaces. */
    tryfixed_result = MAP_FAILED;
    tryfixed_errno = EEXIST;
    assert( horizon_mmap( (void *)0x30000000, PAGE, 0, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0 ) == MAP_FAILED );
    assert( errno == EEXIST && !strcmp( path_taken, "tryfixed" ) );
    assert( horizon_mmap( (void *)0x30000000, PAGE, 0, MAP_PRIVATE | MAP_ANON, -1, 0 ) == (void *)0x9000000 );
    assert( !strcmp( path_taken, "alloc" ) );
    assert( horizon_mmap( (void *)0x30000000, PAGE, 0, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0 ) == (void *)0x30000000 );
    assert( !strcmp( path_taken, "fixed" ) );
    tryfixed_result = (void *)0x30000000;
    tryfixed_errno = 0;
    assert( horizon_mmap( (void *)0x30000000, PAGE, 0, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0 ) == (void *)0x30000000 );
    /* Other failures are reported whatever the flags. */
    tryfixed_result = MAP_FAILED;
    tryfixed_errno = ENOMEM;
    assert( horizon_mmap( (void *)0x30000000, PAGE, 0, MAP_PRIVATE | MAP_ANON, -1, 0 ) == MAP_FAILED && errno == ENOMEM );

    /* Hints inside the alias region refuse MAP_FIXED_NOREPLACE with ENOMEM and
     * fall back to alloc for plain hints without probing tryfixed. */
    path_taken = "none";
    errno = 0;
    assert( horizon_mmap( (void *)0x50000000, PAGE, 0, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0 ) == MAP_FAILED );
    assert( errno == ENOMEM && !strcmp( path_taken, "none" ) && alias_probes == 1 );
    assert( horizon_mmap( (void *)0x50000000, PAGE, 0, MAP_PRIVATE | MAP_ANON, -1, 0 ) == (void *)0x9000000 );
    assert( !strcmp( path_taken, "alloc" ) && alias_probes == 2 );

    printf( "PASS: Horizon mapping index and MAP_FIXED_NOREPLACE\n" );
    return 0;
}
'''

source = (fixture + mapping_struct + '\n' + function(horizon, 'static int compare_mapping(') + '\n' + tree + '\n' +
          function(horizon, 'static void list_add_mapping(') + '\n' +
          function(horizon, 'static void list_remove_mapping(') + '\n' +
          function(horizon, 'static struct horizon_mapping *find_overlap_mapping(') + '\n' +
          '#include <string.h>\n' + stubs + function(horizon, 'void *horizon_mmap(') + '\n' + tests)
with tempfile.TemporaryDirectory() as tmp:
    c_file = Path(tmp) / 'horizon_mappings.c'
    exe = Path(tmp) / 'horizon_mappings'
    c_file.write_text(source)
    subprocess.run(['cc', '-std=gnu99', '-O1', '-Wall', '-Wno-unused-parameter', '-Wno-unused-function', '-Werror',
                    '-I', str(root / 'include'), '-o', str(exe), str(c_file)], check=True)
    subprocess.run([str(exe)], check=True)
