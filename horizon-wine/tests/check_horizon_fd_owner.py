#!/usr/bin/env python3
"""Exercise FD queue owner matching (horizon-fd-owner) and alias-region probe
guarding (0003) in dlls/ntdll/unix/horizon.c."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def extract_function(source, name):
    match = re.search(rf'^(?:static\s+)?[A-Za-z_][^\n;]*\b{re.escape(name)}\([^;]*?\)\n\{{', source, re.M)
    if match is None:
        raise RuntimeError(f'function not found: {name}')
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


fd_msg_struct = re.search(r'^struct horizon_fd_message\n\{.*?^\};', horizon, re.M | re.S).group(0)
fd_queue_struct = re.search(r'^struct horizon_fd_queue\n\{.*?^\};', horizon, re.M | re.S).group(0)
alias_globals = re.search(
    r'^static pthread_once_t horizon_alias_once.*?^static void horizon_alias_init\( void \)\n\{.*?^\}',
    horizon, re.M | re.S,
).group(0)
funcs = '\n\n'.join([
    extract_function(horizon, 'horizon_fd_queue_push'),
    extract_function(horizon, 'horizon_fd_queue_push_dup'),
    extract_function(horizon, 'horizon_fd_queue_report').replace(
        'extern void wine_nx_runtime_trace( const char *msg );', ''),
    extract_function(horizon, 'horizon_fd_queue_pop'),
    alias_globals,
    extract_function(horizon, 'horizon_in_alias_region'),
    extract_function(horizon, 'horizon_report_alias_probe'),
])

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t Result;
typedef uintptr_t ULONG_PTR;
typedef int BOOL;

#define CUR_PROCESS_HANDLE 0xffff8001u
#define InfoType_AliasRegionAddress 2u
#define InfoType_AliasRegionSize 3u
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)

struct horizon_pipe { int id; };

''' + fd_msg_struct + '\n\n' + fd_queue_struct + r'''

static unsigned int trace_count;
static char last_trace[256];

static void wine_nx_runtime_trace( const char *msg )
{
    snprintf( last_trace, sizeof(last_trace), "%s", msg );
    trace_count++;
}

static void horizon_cond_wait_quit( pthread_cond_t *cond, pthread_mutex_t *mutex )
{
    pthread_cond_wait( cond, mutex );
}

static Result svcGetInfo( u64 *out, unsigned int type, unsigned int handle, u64 sub )
{
    (void)handle; (void)sub;
    if (type == InfoType_AliasRegionAddress) { *out = 0x200000000ull; return 0; }
    if (type == InfoType_AliasRegionSize) { *out = 0x100000000ull; return 0; }
    return 1;
}

''' + funcs + r'''

int main( void )
{
    struct horizon_fd_queue q = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL };
    struct horizon_pipe owner_a = { 1 }, owner_b = { 2 }, owner_c = { 3 }, owner_none = { 4 };
    unsigned int h = 0;
    int fds[3];

    assert( !pipe( &fds[0] ) );
    fds[2] = dup( fds[0] );
    assert( fds[2] >= 0 );

    /* Push for owner A (head), owner B (middle), owner C (tail). */
    horizon_fd_queue_push( &q, fds[0], 0x11, &owner_a );
    horizon_fd_queue_push( &q, fds[1], 0x22, &owner_b );
    horizon_fd_queue_push( &q, fds[2], 0x33, &owner_c );

    /* Pop middle owner B first, preserving head=A and tail=C. */
    assert( horizon_fd_queue_pop( &q, &owner_b, &h, "client" ) == fds[1] && h == 0x22 );
    assert( q.head && q.head->owner == &owner_a && q.tail && q.tail->owner == &owner_c );
    /* A thread whose fd was not the oldest says how many it passed. */
    assert( trace_count == 1 );
    assert( !strcmp( last_trace, "[CARAFE] client fd: own fd taken past 1 of other threads" ) );

    /* Pop tail owner C next, updating q.tail back to A. */
    assert( horizon_fd_queue_pop( &q, &owner_c, &h, "server" ) == fds[2] && h == 0x33 );
    assert( q.head && q.head->owner == &owner_a && q.tail == q.head );
    assert( trace_count == 2 );

    /* Unknown owner falls back to head (owner A) and empties queue cleanly. */
    assert( horizon_fd_queue_pop( &q, &owner_none, &h, "server" ) == fds[0] && h == 0x11 );
    assert( !q.head && !q.tail );
    assert( trace_count == 3 );
    assert( !strcmp( last_trace, "[CARAFE] server fd: none from this thread, took the oldest of 1" ) );
    /* Its own fd at the head is the usual case and says nothing; and the
     * reports stop after the first 16. */
    horizon_fd_queue_push( &q, dup( fds[0] ), 0x44, &owner_a );
    close( horizon_fd_queue_pop( &q, &owner_a, &h, "client" ) );
    assert( trace_count == 3 && h == 0x44 );
    for (int i = 0; i < 20; i++)
    {
        horizon_fd_queue_push( &q, dup( fds[0] ), 0x55, &owner_a );
        horizon_fd_queue_push( &q, dup( fds[0] ), 0x66, &owner_b );
        close( horizon_fd_queue_pop( &q, &owner_b, &h, "client" ) );
        close( horizon_fd_queue_pop( &q, &owner_a, &h, "client" ) );
    }
    assert( trace_count == 16 && !q.head && !q.tail );
    trace_count = 0;

    close( fds[0] );
    close( fds[1] );
    close( fds[2] );

    /* Verify alias region boundary checks and bounded trace reporting (0003). */
    assert( !horizon_in_alias_region( (void *)0x1fffff000ull, 0x1000 ) );
    assert( strstr( last_trace, "[HMAP] alias region" ) != NULL );
    assert( horizon_in_alias_region( (void *)0x1fffff000ull, 0x2000 ) );
    assert( horizon_in_alias_region( (void *)0x250000000ull, 0x1000 ) );
    assert( horizon_in_alias_region( (void *)0x1fffff000ull, SIZE_MAX ) );
    assert( !horizon_in_alias_region( (void *)0x300000000ull, 0x1000 ) );

    trace_count = 0;
    for (unsigned int i = 0; i < 12; i++) horizon_report_alias_probe( (void *)0x250000000ull, 0x1000 );
    assert( trace_count == 8 );

    puts( "FD queue owner matching and alias region probe reporting passed" );
    return 0;
}
'''

cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'fd_owner_test.c'
    exe = Path(tmp) / 'fd_owner_test'
    src.write_text(fixture)
    subprocess.run([
        cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
        str(src), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
