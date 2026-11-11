#!/usr/bin/env python3
"""Host unit test for RtlWaitOnAddress exit-unlink handoff (wine-pe/0003 + carafe/0126-0127)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
unix_sync = (root / 'dlls/ntdll/unix/sync.c').read_text(encoding='utf-8')
pe_sync = (root / 'dlls/ntdll/sync.c').read_text(encoding='utf-8')

start = unix_sync.index('#define CARAFE_FUTEX_EXIT_MAGIC')
end = unix_sync.index('\n#endif\n\n\n/***********************************************************************\n *             NtWaitForAlertByThreadId', start)
unix_code = unix_sync[start:end]

assert 'FUTEX_EXIT_MAGIC 0x46555458' in pe_sync
assert 'struct futex_exit ending =' in pe_sync
assert 'NtWaitForAlertByThreadId( &ending, timeout );' in pe_sync

test_c = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

static int mock_stop_state;
static const void *unreadable_ptr;
static unsigned yield_calls;

__attribute__((weak)) int wine_nx_stop_state( int *code )
{
    if (code) *code = 0;
    return mock_stop_state;
}

static BOOL virtual_check_buffer_for_read( const void *addr, SIZE_T size )
{
    (void)size;
    return addr != NULL && addr != unreadable_ptr;
}

NTSTATUS WINAPI NtYieldExecution( void )
{
    yield_calls++;
    sched_yield();
    return 0;
}

''' + unix_code + r'''

struct list64 { ULONG64 next, prev, addr; UINT tid; };
struct list32 { UINT next, prev, addr, tid; };

struct lock_contend_ctx
{
    LONG *lock;
};

static void *release_lock_later( void *arg )
{
    struct lock_contend_ctx *ctx = arg;
    usleep( 5000 );
    __atomic_store_n( ctx->lock, 0, __ATOMIC_SEQ_CST );
    return NULL;
}

int main( void )
{
    struct carafe_futex_exit bad = {0};
    struct carafe_futex_exit good = { CARAFE_FUTEX_EXIT_MAGIC, sizeof(ULONG64), 0, 0 };

    /* 1. Validation in carafe_futex_exit_of. */
    unreadable_ptr = &bad;
    assert( !carafe_futex_exit_of( NULL ) );
    assert( !carafe_futex_exit_of( unreadable_ptr ) );
    unreadable_ptr = NULL;
    assert( !carafe_futex_exit_of( &bad ) );
    bad.magic = CARAFE_FUTEX_EXIT_MAGIC;
    bad.pointer_size = 2;
    assert( !carafe_futex_exit_of( &bad ) );
    bad.pointer_size = 16;
    assert( !carafe_futex_exit_of( &bad ) );
    assert( carafe_futex_exit_of( &good ) == &good );
    good.pointer_size = sizeof(UINT);
    assert( carafe_futex_exit_of( &good ) == &good );

    /* 2. carafe_futex_wait_left clears TLS unless the thread is ending. */
    mock_stop_state = 0;
    carafe_futex_exit = &good;
    carafe_futex_wait_left();
    assert( carafe_futex_exit == NULL );

    mock_stop_state = CARAFE_ALERT_STOP_END;
    carafe_futex_exit = &good;
    carafe_futex_wait_left();
    assert( carafe_futex_exit == &good );
    carafe_futex_exit = NULL;
    mock_stop_state = 0;

    /* 3. 64-bit queue unlink via carafe_futex_exit_take_out. */
    {
        struct list64 head = {0}, other = {0}, victim = {0};
        LONG lock = 0;
        struct carafe_futex_exit desc = {
            CARAFE_FUTEX_EXIT_MAGIC, sizeof(ULONG64),
            (ULONG64)(uintptr_t)&victim, (ULONG64)(uintptr_t)&lock
        };

        head.next = (ULONG64)(uintptr_t)&victim;
        head.prev = (ULONG64)(uintptr_t)&other;
        victim.next = (ULONG64)(uintptr_t)&other;
        victim.prev = (ULONG64)(uintptr_t)&head;
        victim.addr = 0x12345678ULL;
        other.next = (ULONG64)(uintptr_t)&head;
        other.prev = (ULONG64)(uintptr_t)&victim;
        other.addr = 0x87654321ULL;

        carafe_futex_exit = carafe_futex_exit_of( &desc );
        assert( carafe_futex_exit == &desc );
        carafe_futex_exit_take_out();
        assert( !carafe_futex_exit && lock == 0 );
        assert( head.next == (ULONG64)(uintptr_t)&other );
        assert( other.prev == (ULONG64)(uintptr_t)&head );
        assert( head.prev == (ULONG64)(uintptr_t)&other && other.next == (ULONG64)(uintptr_t)&head );

        /* Second call is a no-op; cleared entry.addr leaves the queue untouched. */
        carafe_futex_exit_take_out();
        victim.addr = 0;
        victim.next = 0xdeadbeefULL;
        victim.prev = 0xdeadbeefULL;
        carafe_futex_exit = &desc;
        carafe_futex_exit_take_out();
        assert( head.next == (ULONG64)(uintptr_t)&other && other.prev == (ULONG64)(uintptr_t)&head );
    }

    /* 4. 32-bit (WoW64) queue unlink under lock contention. */
    {
        struct list32 *nodes = (uintptr_t)&good <= 0xffffffffULL ? NULL :
#ifdef MAP_32BIT
            mmap( NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0 );
#else
            MAP_FAILED;
#endif
        struct list32 stack_nodes[3];
        struct list32 *use = nodes != MAP_FAILED && nodes != NULL ? nodes :
                             ((uintptr_t)stack_nodes <= 0xffffffffULL ? stack_nodes : NULL);
        if (use)
        {
            LONG lock = -1;
            pthread_t releaser;
            struct lock_contend_ctx ctx = { &lock };
            struct carafe_futex_exit desc;

            use[0].next = (UINT)(uintptr_t)&use[1];
            use[0].prev = (UINT)(uintptr_t)&use[2];
            use[0].addr = 0;
            use[1].next = (UINT)(uintptr_t)&use[2];
            use[1].prev = (UINT)(uintptr_t)&use[0];
            use[1].addr = 0x400000;
            use[2].next = (UINT)(uintptr_t)&use[0];
            use[2].prev = (UINT)(uintptr_t)&use[1];
            use[2].addr = 0x400004;
            desc.magic = CARAFE_FUTEX_EXIT_MAGIC;
            desc.pointer_size = sizeof(UINT);
            desc.entry = (ULONG64)(uintptr_t)&use[1];
            desc.lock = (ULONG64)(uintptr_t)&lock;

            yield_calls = 0;
            carafe_futex_exit = carafe_futex_exit_of( &desc );
            assert( !pthread_create( &releaser, NULL, release_lock_later, &ctx ) );
            carafe_futex_exit_take_out();
            assert( !pthread_join( releaser, NULL ) );
            assert( lock == 0 && yield_calls > 0 );
            assert( use[0].next == (UINT)(uintptr_t)&use[2] );
            assert( use[2].prev == (UINT)(uintptr_t)&use[0] );
        }
        if (nodes != MAP_FAILED && nodes != NULL) munmap( nodes, 4096 );
    }

    puts( "carafe_futex_exit: validation, ending flag, 64-bit unlink and lock spin passed" );
    return 0;
}
'''

with tempfile.TemporaryDirectory( prefix='wine-nx-futex-exit-' ) as directory:
    path = Path( directory )
    src = path / 'test.c'
    exe = path / 'test'
    src.write_text( test_c, encoding='utf-8' )
    cc = os.environ.get( 'WINE_NX_HOST_CC', '/usr/bin/clang' )
    subprocess.run( [cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-address',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                     '-D__WINESRC__', '-D_WIN64', '-I' + str( root / 'include' ),
                     str( src ), '-o', str( exe )], check=True )
    subprocess.run( [str( exe )], check=True, timeout=15 )
