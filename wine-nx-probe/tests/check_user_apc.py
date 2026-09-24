#!/usr/bin/env python3
"""Run the Horizon server's user APC queue (horizon_server_queue_user_apc_locked
and horizon_server_take_user_apc_locked in dlls/ntdll/unix/horizon.c).

Halo reads the 2048-byte header of each of its map files with ReadFileEx and
waits for the completion routine in SleepEx(5000, TRUE), giving up when the wait
returns anything but WAIT_IO_COMPLETION; with no user APCs the wait timed out
and the game reported that one of its files was missing or corrupted. The same
queue is what QueueUserAPC and WriteFileEx need."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()

def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

# An alertable wait runs a waiting APC instead of waiting, and says so with
# STATUS_USER_APC and the call itself, which is what server_select hands back.
select = function(horizon, 'static int horizon_server_handle_select')
assert select.index('HORIZON_SELECT_ALERTABLE') < select.index('horizon_server_select_status')
assert select.index('horizon_server_take_user_apc_locked') < select.rindex('horizon_server_sleep_locked')
assert 'HORIZON_STATUS_USER_APC' in select and 'apc->call' in select
# The queue is emptied with the thread, not left behind.
assert 'object->apc_first' in function(horizon, 'static void horizon_server_free_object')

fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HORIZON_STATUS_SUCCESS           0
#define HORIZON_STATUS_NO_MEMORY         ((unsigned int)0xc0000017)
#define HORIZON_STATUS_INVALID_PARAMETER ((unsigned int)0xc000000d)

static int woken;
static void horizon_sync_notify_legacy_locked( void ) { woken++; }
#define HORIZON_SELECT_ALERTABLE 1
struct horizon_server_connection { struct horizon_server_object *thread; };
struct horizon_select_request { unsigned flags; };
struct horizon_sync_waiter {
    struct horizon_server_connection *connection;
    const struct horizon_select_request *request;
    struct horizon_sync_waiter *next;
    int notified;
};
static struct horizon_sync_waiter *horizon_sync_waiters;
static void horizon_sync_notify_locked(struct horizon_sync_waiter *w) { w->notified++; }

@STRUCTS@

struct horizon_server_object
{
    struct horizon_user_apc *apc_first, *apc_last;
};

@BODY@

int main( void )
{
    struct horizon_server_object thread = { 0 };
    struct horizon_server_object unrelated = { 0 };
    struct horizon_server_connection own = { &thread }, other = { &unrelated };
    struct horizon_select_request alertable = { HORIZON_SELECT_ALERTABLE }, plain = { 0 };
    struct horizon_sync_waiter waits[3] = {
        { &own, &alertable, &waits[1], 0 }, { &own, &plain, &waits[2], 0 }, { &other, &alertable, NULL, 0 }
    };
    horizon_sync_waiters = waits;
    struct horizon_user_apc *apc;
    unsigned char call[40];
    unsigned int i;

    /* Nothing waiting for a thread that was given nothing. */
    assert( !horizon_server_take_user_apc_locked( &thread ) );
    assert( !horizon_server_take_user_apc_locked( NULL ) );

    /* A call is kept as it arrived, and a sleeping thread is woken for it. */
    for (i = 0; i < sizeof(call); i++) call[i] = (unsigned char)(i + 1);
    assert( !horizon_server_queue_user_apc_locked( &thread, call, sizeof(call) ) );
    assert( woken == 1 );
    assert(waits[0].notified == 1 && !waits[1].notified && !waits[2].notified);
    call[0] = 0xaa;
    assert( !horizon_server_queue_user_apc_locked( &thread, call, sizeof(call) ) );

    /* Oldest first, as Windows runs them. */
    apc = horizon_server_take_user_apc_locked( &thread );
    assert( apc && apc->size == sizeof(call) && apc->call[0] == 1 && apc->call[39] == 40 );
    free( apc );
    apc = horizon_server_take_user_apc_locked( &thread );
    assert( apc && apc->call[0] == 0xaa );
    free( apc );
    assert( !horizon_server_take_user_apc_locked( &thread ) );
    /* The last one out leaves the queue able to take another. */
    assert( !thread.apc_first && !thread.apc_last );
    assert( !horizon_server_queue_user_apc_locked( &thread, call, sizeof(call) ) );
    apc = horizon_server_take_user_apc_locked( &thread );
    assert( apc );
    free( apc );

    /* Nothing that is not a call. */
    assert( horizon_server_queue_user_apc_locked( &thread, call, 0 ) == HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_server_queue_user_apc_locked( &thread, call, HORIZON_USER_APC_MAX + 1 ) ==
            HORIZON_STATUS_INVALID_PARAMETER );
    assert( !thread.apc_first );

    printf( "user APCs queued, taken oldest first, and the waiter woken\n" );
    return 0;
}
'''

structs = horizon[horizon.index('/* An apc_call is a few dozen bytes'):
                  horizon.index('struct horizon_server_object\n{')]
body = '\n\n'.join([
    function(horizon, 'static unsigned int horizon_server_queue_user_apc_locked'),
    function(horizon, 'static struct horizon_user_apc *horizon_server_take_user_apc_locked'),
])

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'apc.c'
    source.write_text(fixture.replace('@STRUCTS@', structs).replace('@BODY@', body))
    binary = Path(tmp) / 'apc'
    subprocess.run(['cc', '-o', str(binary), str(source)], check=True)
    print(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip())
