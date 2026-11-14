#!/usr/bin/env python3
"""Exercise thread-creation waiting for a kernel thread slot on the host."""
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


wait_defs = '\n'.join(re.findall(r'^#define HORIZON_THREAD_WAIT_\w+ .*', horizon, re.M))
extracted = '\n\n'.join([
    extract_function(horizon, 'horizon_zombie_reap_ended'),
    extract_function(horizon, 'horizon_reap_ended_threads'),
    extract_function(horizon, 'horizon_threads_at_limit'),
    extract_function(horizon, 'horizon_thread_may_wait'),
    extract_function(horizon, 'horizon_tls_give_place'),
    extract_function(horizon, 'horizon_create_thread_once'),
    extract_function(horizon, '__wrap_svcCreateThread'),
])

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef u32 Handle;
typedef u32 Result;
typedef void (*ThreadFunc)(void *);

#define INVALID_HANDLE 0u
#define Module_Kernel 1u
#define KernelError_OutOfResource 103u
#define KernelError_OutOfMemory 104u
#define MAKERESULT(m, d) ((((u32)(m)) & 0x1ffu) | ((((u32)(d)) & 0x1fffu) << 9))
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
#define InfoType_ResourceLimit 11u
#define LimitableResource_Threads 2u

struct horizon_zombie { pthread_t thread; struct horizon_zombie *next; };
struct horizon_zombie_list { pthread_mutex_t lock; struct horizon_zombie *head; unsigned int pending, reaped; };

static struct horizon_zombie_list horizon_server_zombies = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };
static struct { unsigned int tebs; } horizon_lifecycle;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile int wine_nx_quit_requested;
static unsigned int mock_tls_place_avail;
static unsigned int mock_server_join_avail;
static unsigned int mock_tls_give_place_calls;
static unsigned int mock_join_ended_calls;
static int mock_rlimit_used;
static int mock_rlimit_limit = 96;

/* The counters behind the [BUDGET] line: what each wait and refusal reports. */
static unsigned int mock_waited_calls, mock_waited_placed, mock_waiter_reports, mock_refused;
static unsigned long long mock_waited_ns;
#define CARAFE_THREAD_WAITED( ns, placed ) do { mock_waited_calls++; mock_waited_ns = (ns); mock_waited_placed += !!(placed); } while (0)
#define CARAFE_THREAD_WAITER_REPORT() (mock_waiter_reports++)
#define CARAFE_THREAD_REFUSED() (mock_refused++)
static int mock_real_fail_oores;
static int mock_tls_conflicts;
static u64 mock_now_ns;
static u64 mock_step_ns = 1000000000ull;
static unsigned int mock_sleep_calls;
static unsigned int mock_free_after_sleeps;
static char mock_last_trace[256];
static unsigned int mock_trace_count;
static int mock_zombie_ended;

static int horizon_pthread_ended( pthread_t thread )
{
    (void)thread;
    return mock_zombie_ended;
}

unsigned int horizon_join_ended_thread( void )
{
    mock_join_ended_calls++;
    if (mock_server_join_avail)
    {
        mock_server_join_avail--;
        if (mock_rlimit_used > 0) mock_rlimit_used--;
        return 1;
    }
    return 0;
}

unsigned int wine_nx_tls_give_place( void )
{
    mock_tls_give_place_calls++;
    if (mock_tls_place_avail)
    {
        mock_tls_place_avail--;
        if (mock_rlimit_used > 0) mock_rlimit_used--;
        return 1;
    }
    return 0;
}

static void wine_nx_runtime_trace( const char *msg )
{
    snprintf( mock_last_trace, sizeof(mock_last_trace), "%s", msg );
    mock_trace_count++;
}

static Result svcGetInfo( u64 *out, u32 type, Handle h, u64 sub )
{
    (void)type; (void)h; (void)sub;
    *out = 1;
    return 0;
}

static Result svcGetResourceLimitCurrentValue( s64 *out, u64 res, u32 which )
{
    (void)res; (void)which;
    *out = mock_rlimit_used;
    return 0;
}

static Result svcGetResourceLimitLimitValue( s64 *out, u64 res, u32 which )
{
    (void)res; (void)which;
    *out = mock_rlimit_limit;
    return 0;
}

static Result svcCloseHandle( Handle h )
{
    if (h >= 0x10 && mock_rlimit_used > 0) mock_rlimit_used--;
    return 0;
}
static u64 armGetSystemTick( void ) { return mock_now_ns; }
static u64 armTicksToNs( u64 ticks ) { return ticks; }

static void svcSleepThread( s64 ns )
{
    (void)ns;
    mock_sleep_calls++;
    mock_now_ns += mock_step_ns;
    /* Another thread ended while this one slept: its place is free. */
    if (mock_free_after_sleeps && mock_sleep_calls == mock_free_after_sleeps) mock_rlimit_used--;
}

static Handle next_handle = 0x10;

Result __real_svcCreateThread( Handle *out, ThreadFunc entry, void *arg, void *stack, s32 prio, s32 core )
{
    (void)entry; (void)arg; (void)stack; (void)prio; (void)core;
    if (mock_real_fail_oores > 0)
    {
        mock_real_fail_oores--;
        return MAKERESULT( Module_Kernel, KernelError_OutOfResource );
    }
    if (mock_rlimit_used >= mock_rlimit_limit)
        return MAKERESULT( Module_Kernel, KernelError_OutOfResource );
    mock_rlimit_used++;
    *out = next_handle++;
    return 0;
}

static Result check_thread_local_reservations( u64 *conflict )
{
    if (mock_tls_conflicts > 0) { mock_tls_conflicts--; *conflict = 0x1000; return 0; }
    *conflict = 0;
    return 0;
}

static void *dummy_worker( void *arg ) { (void)arg; return NULL; }
static void dummy_thread_entry( void *arg ) { (void)arg; }

''' + wait_defs + '\n' + extracted + r'''

static void reset_state( void )
{
    horizon_lifecycle.tebs = 2;
    wine_nx_quit_requested = 0;
    mock_tls_place_avail = 0;
    mock_server_join_avail = 0;
    mock_tls_give_place_calls = 0;
    mock_join_ended_calls = 0;
    mock_rlimit_used = 96;
    mock_rlimit_limit = 96;
    mock_real_fail_oores = 0;
    mock_tls_conflicts = 0;
    mock_now_ns = 1000000ull;
    mock_step_ns = 1000000000ull;
    mock_sleep_calls = 0;
    mock_trace_count = 0;
    mock_last_trace[0] = 0;
    mock_zombie_ended = 0;
    mock_waited_calls = mock_waited_placed = mock_waiter_reports = mock_refused = 0;
    mock_waited_ns = 0;
    mock_free_after_sleeps = 0;
}

int main( void )
{
    Handle h = 0;
    struct horizon_zombie *z;
    Result rc;

    /* 1. Reaps an ended zombie server pthread to free a kernel slot and succeeds without sleeping. */
    reset_state();
    z = calloc( 1, sizeof(*z) );
    assert( !pthread_create( &z->thread, NULL, dummy_worker, NULL ) );
    z->next = horizon_server_zombies.head;
    horizon_server_zombies.head = z;
    horizon_server_zombies.pending = 1;
    mock_zombie_ended = 1;
    mock_rlimit_used = 95;
    mock_real_fail_oores = 1; /* first attempt sees OutOfResource, second succeeds after reaping */
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( R_SUCCEEDED(rc) && h != 0 );
    assert( horizon_server_zombies.reaped == 1 && horizon_server_zombies.pending == 0 );
    assert( mock_sleep_calls == 0 );
    assert( !mock_waited_calls && !mock_refused && !mock_waiter_reports );

    /* 2. Calls wine_nx_tls_give_place() when no ended pthread is waiting, freeing a TLS slot. */
    reset_state();
    mock_tls_place_avail = 1;
    mock_tls_conflicts = 2;
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( R_SUCCEEDED(rc) && mock_tls_give_place_calls == 1 );
    assert( mock_sleep_calls == 0 );

    /* 3. Quit requested or zero active TEBs skips waiting immediately. */
    reset_state();
    wine_nx_quit_requested = 1;
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( rc == MAKERESULT( Module_Kernel, KernelError_OutOfResource ) && mock_sleep_calls == 0 );
    assert( mock_refused == 1 && !mock_waited_calls );
    reset_state();
    horizon_lifecycle.tebs = 0;
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( rc == MAKERESULT( Module_Kernel, KernelError_OutOfResource ) && mock_sleep_calls == 0 );

    /* 4. Timed wait eventually logs progress and gives up after HORIZON_THREAD_WAIT_NS. */
    reset_state();
    mock_step_ns = 6000000000ull; /* 6s per step -> hits 5s note then 20s limit */
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( rc == MAKERESULT( Module_Kernel, KernelError_OutOfResource ) );
    assert( mock_sleep_calls >= 3 && strstr( mock_last_trace, "[THREAD] create failed" ) != NULL );
    /* The wait is counted once as lost, with how long it took; the threads'
     * places are asked for once, at the first note; the refusal is counted. */
    assert( mock_waited_calls == 1 && !mock_waited_placed && mock_waited_ns >= HORIZON_THREAD_WAIT_NS );
    assert( mock_waiter_reports == 1 && mock_refused == 1 );

    /* 5. A place that frees while waiting: counted as a wait that was placed, and not as a refusal. */
    reset_state();
    mock_free_after_sleeps = 2;
    rc = __wrap_svcCreateThread( &h, dummy_thread_entry, NULL, NULL, 0, 0 );
    assert( R_SUCCEEDED(rc) && mock_sleep_calls == 2 );
    assert( mock_waited_calls == 1 && mock_waited_placed == 1 && mock_waited_ns > 0 && !mock_refused );
    assert( strstr( mock_last_trace, "[THREAD] waited" ) != NULL );

    puts( "Thread creation waits for a place: zombie reap, TLS place give-up, quit bypass, timeout, a place freed while waiting, and what each reports passed" );
    return 0;
}
'''

cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'wait_test.c'
    exe = Path(tmp) / 'wait_test'
    src.write_text(fixture)
    subprocess.run([
        cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-address',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
        str(src), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
