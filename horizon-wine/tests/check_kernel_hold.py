#!/usr/bin/env python3
"""Host unit test for kernel hold/release/wait_change and FEX TLS register inspection (0119, 0122-0124)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tp_h = (root / 'horizon-wine/source/thread_profile.h').read_text(encoding='utf-8')
tp_c = (root / 'horizon-wine/source/thread_profile.c').read_text(encoding='utf-8')


def block(text, start_sig, end_sig):
    s = text.index(start_sig)
    e = text.index(end_sig, s)
    return text[s:e]


defs = block(tp_c, '#define NX_PROF_MAX_THREADS', '#define NX_PROF_TARGETS') + '#define NX_PROF_TARGETS 4\n'
hold_code = block(tp_c, 'static int stop_state(', 'int wine_nx_stop_state(')
fex_code = block(tp_c, 'static int fex_x86_registers(', 'static unsigned int walk_x86(')

test_c = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../source/thread_profile.h"

''' + defs + r'''

typedef unsigned Handle;
typedef struct { uint64_t addr, size; } MemoryInfo;
enum { SignalType_Signal = 0 };
struct nx_prof_thread
{
    Handle handle;
    unsigned int tid;
    char kind;
    int core;
    int fixed;
    uint64_t teb;
    uint64_t last_ticks, balance_ticks;
    volatile int stop[2];
    const void * volatile frame;
    const void * volatile changed_contexts;
};

static struct nx_prof_thread registry[NX_PROF_MAX_THREADS];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t futex_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t futex_cond = PTHREAD_COND_INITIALIZER;
static __thread struct nx_prof_thread *self_thread;
static unsigned signal_count, futex_waiters;

static void svcSignalToAddress( void *addr, int type, int op, int count )
{
    (void)addr; (void)type; (void)op; (void)count;
    pthread_mutex_lock( &futex_mutex );
    __atomic_add_fetch( &signal_count, 1, __ATOMIC_RELAXED );
    pthread_cond_broadcast( &futex_cond );
    pthread_mutex_unlock( &futex_mutex );
}

static int horizon_futex_wait( const int *addr, int value, long long timeout_ns )
{
    (void)timeout_ns;
    pthread_mutex_lock( &futex_mutex );
    if (__atomic_load_n( addr, __ATOMIC_SEQ_CST ) == value)
    {
        __atomic_add_fetch( &futex_waiters, 1, __ATOMIC_SEQ_CST );
        pthread_cond_wait( &futex_cond, &futex_mutex );
    }
    pthread_mutex_unlock( &futex_mutex );
    return 0;
}

''' + hold_code + r'''

static uint64_t readable_lo, readable_hi;
static uint64_t bad_lo, bad_hi;
static uint32_t *race_control_ptr, race_control_new_val;

static int readable_size( uint64_t address, uint64_t size, MemoryInfo *info )
{
    if (address >= bad_lo && address < bad_hi) return 0;
    if (address >= readable_lo && address + size <= readable_hi)
    {
        info->addr = readable_lo;
        info->size = readable_hi - readable_lo;
        return 1;
    }
    return 0;
}

static uint64_t read_word( uint64_t address )
{
    return *(const uint64_t *)(uintptr_t)address;
}

static uint32_t read_word32( uint64_t address )
{
    uint32_t val = *(const uint32_t *)(uintptr_t)address;
    if (race_control_ptr && address == (uint64_t)(uintptr_t)race_control_ptr)
    {
        /* Simulate the guest thread mutating FEX control between the initial check and final re-check. */
        *race_control_ptr = race_control_new_val;
        race_control_ptr = NULL;
    }
    return val;
}

''' + fex_code + r'''

static const char fake_frame1[] = "frame1";
static const char fake_frame2[] = "frame2";
static const char fake_changed[] = "changed";

struct client_args
{
    struct nx_prof_thread *slot;
    int entered;
    const void *observed_enter_changed;
    const void *observed_leave_changed;
};

static void *client_kernel_worker( void *arg )
{
    struct client_args *c = arg;
    self_thread = c->slot;
    c->observed_enter_changed = wine_nx_kernel_enter( fake_frame1 );
    __atomic_store_n( &c->entered, 1, __ATOMIC_RELEASE );
    while (!(__atomic_load_n( &c->slot->stop[0], __ATOMIC_ACQUIRE ) & WINE_NX_STOP_HELD))
        sched_yield();
    c->observed_leave_changed = wine_nx_kernel_leave();
    return NULL;
}

static void *fn_enter_release( void *p )
{
    struct nx_prof_thread *t = p;
    const void *f = NULL;
    int ch = 0;

    while (!__atomic_load_n( &futex_waiters, __ATOMIC_ACQUIRE )) sched_yield();
    assert( wine_nx_thread_hold( (const void *)(uintptr_t)t->teb, t->tid, &f, &ch ) );
    assert( f == fake_frame2 && !ch );
    __atomic_fetch_and( &t->stop[0], ~WINE_NX_STOP_SUSPEND, __ATOMIC_SEQ_CST );
    wine_nx_thread_release( (const void *)(uintptr_t)t->teb, t->tid, fake_changed );
    return NULL;
}

static void *fn_signal_waiter( void *p )
{
    struct nx_prof_thread *t = p;

    while (!__atomic_load_n( &futex_waiters, __ATOMIC_ACQUIRE )) sched_yield();
    wine_nx_thread_signal( (const void *)(uintptr_t)t->teb, t->tid );
    return NULL;
}

int main( void )
{
    static char teb_mem[0x2000];
    static char cpu_mem[0x400];
    static char fex_thread_mem[0x200];
    uint64_t teb = (uint64_t)(uintptr_t)teb_mem;
    uint64_t cpu = (uint64_t)(uintptr_t)cpu_mem;
    uint64_t fex_thread = (uint64_t)(uintptr_t)fex_thread_mem;
    uint64_t fex_frame = fex_thread + 0x80;
    uint32_t eip = 0, esp = 0, ebp = 0;

    /* 1. Kernel enter/leave/hold/release/wait_change state machine. */
    memset( registry, 0, sizeof(registry) );
    registry[0].handle = 11;
    registry[0].tid = 100;
    registry[0].teb = teb;
    self_thread = &registry[0];

    /* Enter with NULL clears LEAVING only. */
    registry[0].stop[0] = WINE_NX_STOP_LEAVING;
    assert( wine_nx_kernel_enter( NULL ) == NULL );
    assert( !(registry[0].stop[0] & WINE_NX_STOP_LEAVING) );

    /* Enter while already suspended holds immediately until release with changed contexts. */
    registry[0].stop[0] = WINE_NX_STOP_SUSPEND;
    futex_waiters = 0;
    {
        pthread_t releaser;
        assert( !pthread_create( &releaser, NULL, fn_enter_release, &registry[0] ) );
        assert( wine_nx_kernel_enter( fake_frame2 ) == fake_changed );
        assert( !pthread_join( releaser, NULL ) );
        assert( (registry[0].stop[0] & WINE_NX_STOP_KERNEL) && !(registry[0].stop[0] & WINE_NX_STOP_HELD) );
        assert( wine_nx_kernel_leave() == NULL );
        assert( !(registry[0].stop[0] & WINE_NX_STOP_KERNEL) && (registry[0].stop[0] & WINE_NX_STOP_LEAVING) );
    }

    /* Two-thread race: server observes kernel mode, holds & waits until client enters wait_while_held, then releases. */
    registry[0].stop[0] = WINE_NX_STOP_SUSPEND;
    registry[0].frame = NULL;
    registry[0].changed_contexts = NULL;
    futex_waiters = 0;
    {
        pthread_t client;
        struct client_args c = { .slot = &registry[0] };
        const void *frame = NULL;
        int ch = 0, st;

        /* Clear SUSPEND first so client enters kernel cleanly, then server suspends + holds. */
        registry[0].stop[0] = 0;
        assert( !pthread_create( &client, NULL, client_kernel_worker, &c ) );
        while (!__atomic_load_n( &c.entered, __ATOMIC_ACQUIRE )) sched_yield();
        assert( c.observed_enter_changed == NULL );

        __atomic_fetch_or( &registry[0].stop[0], WINE_NX_STOP_SUSPEND, __ATOMIC_SEQ_CST );
        st = wine_nx_thread_state( (const void *)(uintptr_t)teb, 100, &frame );
        assert( (st & WINE_NX_STOP_KERNEL) && frame == fake_frame1 );
        assert( wine_nx_thread_hold( (const void *)(uintptr_t)teb, 100, &frame, &ch ) );
        assert( frame == fake_frame1 && !ch );
        while (!__atomic_load_n( &futex_waiters, __ATOMIC_ACQUIRE )) sched_yield();
        __atomic_fetch_and( &registry[0].stop[0], ~WINE_NX_STOP_SUSPEND, __ATOMIC_SEQ_CST );
        wine_nx_thread_release( (const void *)(uintptr_t)teb, 100, fake_changed );
        assert( !pthread_join( client, NULL ) );
        assert( c.observed_leave_changed == fake_changed );
        assert( registry[0].stop[0] & WINE_NX_STOP_LEAVING );
    }

    /* WINE_NX_STOP_END wakes wait_while_held immediately. */
    registry[0].stop[0] = WINE_NX_STOP_SUSPEND | WINE_NX_STOP_KERNEL | WINE_NX_STOP_HELD;
    __atomic_fetch_or( &registry[0].stop[0], WINE_NX_STOP_END, __ATOMIC_SEQ_CST );
    wait_while_held( &registry[0] );

    /* wine_nx_suspend_requested, wine_nx_thread_wait_change and wine_nx_thread_signal. */
    registry[0].stop[0] = WINE_NX_STOP_SUSPEND;
    assert( wine_nx_suspend_requested() );
    futex_waiters = 0;
    {
        pthread_t signaler;
        assert( !pthread_create( &signaler, NULL, fn_signal_waiter, &registry[0] ) );
        wine_nx_thread_wait_change( (const void *)(uintptr_t)teb, 100, WINE_NX_STOP_SUSPEND );
        assert( !pthread_join( signaler, NULL ) );
        assert( registry[0].stop[0] & WINE_NX_STOP_HELD );
    }
    registry[0].stop[0] = 0;
    assert( !wine_nx_suspend_requested() );

    /* 2. FEX TLS register inspection + fallback to I386_CONTEXT. */
    readable_lo = (uint64_t)(uintptr_t)cpu_mem;
    if ((uint64_t)(uintptr_t)teb_mem < readable_lo) readable_lo = (uint64_t)(uintptr_t)teb_mem;
    if ((uint64_t)(uintptr_t)fex_thread_mem < readable_lo) readable_lo = (uint64_t)(uintptr_t)fex_thread_mem;
    readable_hi = (uint64_t)(uintptr_t)(cpu_mem + sizeof(cpu_mem));
    if ((uint64_t)(uintptr_t)(teb_mem + sizeof(teb_mem)) > readable_hi)
        readable_hi = (uint64_t)(uintptr_t)(teb_mem + sizeof(teb_mem));
    if ((uint64_t)(uintptr_t)(fex_thread_mem + sizeof(fex_thread_mem)) > readable_hi)
        readable_hi = (uint64_t)(uintptr_t)(fex_thread_mem + sizeof(fex_thread_mem));

    memset( teb_mem, 0, sizeof(teb_mem) );
    memset( cpu_mem, 0, sizeof(cpu_mem) );
    memset( fex_thread_mem, 0, sizeof(fex_thread_mem) );

    /* CPU area with non-i386 machine returns 0. */
    *(uint64_t *)(teb_mem + NX_PROF_TEB_CPU_AREA) = cpu;
    *(uint16_t *)(cpu_mem + NX_PROF_CPU_MACHINE) = 0x8664;
    assert( !x86_registers( teb, &eip, &esp, &ebp ) );

    /* Fallback I386_CONTEXT registers when no FEX thread TLS is set. */
    *(uint16_t *)(cpu_mem + NX_PROF_CPU_MACHINE) = NX_PROF_MACHINE_I386;
    *(uint32_t *)(cpu_mem + NX_PROF_CPU_CONTEXT + NX_PROF_I386_ESP) = 0x11223344;
    *(uint32_t *)(cpu_mem + NX_PROF_CPU_CONTEXT + NX_PROF_I386_EBP) = 0x55667788;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );

    /* Active valid FEX frame overrides EIP/ESP/EBP. */
    *(uint64_t *)(teb_mem + NX_PROF_TEB_FEX_THREAD) = fex_thread;
    *(uint32_t *)(teb_mem + NX_PROF_TEB_FEX_CONTROL) = 0;
    *(uint64_t *)(fex_thread_mem + 0) = fex_frame;
    *(uint64_t *)(fex_thread_mem + 0x80 + NX_PROF_FEX_RIP) = 0x7bc12345;
    *(uint64_t *)(fex_thread_mem + 0x80 + NX_PROF_FEX_GREGS + NX_PROF_FEX_REG_ESP * 8) = 0x0019ff00;
    *(uint64_t *)(fex_thread_mem + 0x80 + NX_PROF_FEX_GREGS + NX_PROF_FEX_REG_EBP * 8) = 0x0019ff40;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0x7bc12345 && esp == 0x0019ff00 && ebp == 0x0019ff40 );

    /* IN_JIT or AREA_DIRTY falls back to I386_CONTEXT. */
    *(uint32_t *)(teb_mem + NX_PROF_TEB_FEX_CONTROL) = NX_PROF_FEX_IN_JIT;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );

    *(uint32_t *)(teb_mem + NX_PROF_TEB_FEX_CONTROL) = NX_PROF_FEX_AREA_DIRTY;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );

    /* Out-of-span frame falls back to I386_CONTEXT. */
    *(uint32_t *)(teb_mem + NX_PROF_TEB_FEX_CONTROL) = 0;
    *(uint64_t *)(fex_thread_mem + 0) = fex_thread + NX_PROF_FEX_THREAD_SPAN;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );

    /* Unreadable FEX thread pointer triggers fallback cleanly. */
    *(uint64_t *)(fex_thread_mem + 0) = fex_frame;
    bad_lo = fex_thread;
    bad_hi = fex_thread + 16;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );
    bad_lo = bad_hi = 0;

    /* Concurrent control word modification mid-read triggers fallback. */
    *(uint64_t *)(fex_thread_mem + 0) = fex_frame;
    race_control_ptr = (uint32_t *)(teb_mem + NX_PROF_TEB_FEX_CONTROL);
    race_control_new_val = NX_PROF_FEX_IN_JIT;
    assert( x86_registers( teb, &eip, &esp, &ebp ) == 1 );
    assert( eip == 0 && esp == 0x11223344 && ebp == 0x55667788 );

    puts( "kernel hold/release and FEX TLS register inspection passed" );
    return 0;
}
'''

with tempfile.TemporaryDirectory( prefix='wine-nx-kernel-hold-' ) as directory:
    path = Path( directory )
    src = path / 'test.c'
    exe = path / 'test'
    src.write_text( test_c, encoding='utf-8' )
    cc = os.environ.get( 'WINE_NX_HOST_CC', '/usr/bin/clang' )
    subprocess.run( [cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                     '-I' + str( root / 'horizon-wine/source' ),
                     str( src ), '-o', str( exe )], check=True )
    subprocess.run( [str( exe )], check=True, timeout=15 )
