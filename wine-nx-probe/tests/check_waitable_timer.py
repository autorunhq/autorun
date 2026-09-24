#!/usr/bin/env python3
"""Run horizon.c's waitable timers against a clock the test moves by hand.

A timer is signalled when it expires, not when it is set. Need for Speed Most
Wanted paces its streaming thread with SetWaitableTimer and
WaitForSingleObject; signalling on the set returned every wait at once, 70000
times a second on one core, and the game lost its pacing and froze."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(name):
    m = re.search('^' + re.escape(name) + r'[^;{]*?\)\s*\n\{', source, re.M | re.S)
    assert m, name
    i = m.end()
    depth = 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[m.start():i] + '\n'


fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HORIZON_STATUS_SUCCESS 0
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_CURRENT_THREAD_HANDLE 0xfffffffeu
#define max(a,b) ((a) > (b) ? (a) : (b))

typedef struct { long long QuadPart; } LARGE_INTEGER;
enum { HORIZON_SERVER_OBJECT_TIMER = 1, HORIZON_SERVER_OBJECT_MSG_QUEUE, HORIZON_SERVER_OBJECT_EVENT };

struct horizon_server_object
{
    int type;
    int manual_reset;
    int signaled;
    long long timer_when;
    unsigned int timer_period;
    struct horizon_server_object *timer_next;
};
struct horizon_server_handle_entry
{
    struct horizon_server_handle_entry *next;
    unsigned int handle;
    struct horizon_server_object *object;
};
struct horizon_server_connection { int reply_fd; };
struct horizon_set_timer_request { unsigned int handle; long long expire; int period; };
struct horizon_set_timer_reply { struct { unsigned int error; } header; int signaled; };
struct horizon_cancel_timer_request { unsigned int handle; };
struct horizon_cancel_timer_reply { struct { unsigned int error; } header; int signaled; };

static long long clock_ns100 = 130000000000000000LL;   /* an ordinary NT time */
static void NtQuerySystemTime( LARGE_INTEGER *now ) { now->QuadPart = clock_ns100; }
static void horizon_sync_notify_object_locked(struct horizon_server_object *o, int satisfy) { (void)o; (void)satisfy; }

static struct horizon_server_object objects[2];
static struct horizon_server_handle_entry entries[2] = {
    { &entries[1], 1, &objects[0] }, { NULL, 2, &objects[1] } };
static struct horizon_server_handle_entry *horizon_server_handles = entries;
static struct horizon_server_object *horizon_server_timers;

static struct horizon_server_handle_entry *horizon_server_find_handle_locked( unsigned int handle )
{
    struct horizon_server_handle_entry *entry;

    for (entry = horizon_server_handles; entry; entry = entry->next)
        if (entry->handle == handle) return entry;
    return NULL;
}

static unsigned int horizon_server_find_typed_object_locked( unsigned int handle, int type,
                                                             struct horizon_server_object **out )
{
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked( handle );

    if (!entry || entry->object->type != type) return HORIZON_STATUS_INVALID_HANDLE;
    *out = entry->object;
    return HORIZON_STATUS_SUCCESS;
}

/* Both replies carry the same error and previous state. */
static struct horizon_set_timer_reply last_reply;
static int horizon_server_write_reply( int fd, const void *data, unsigned int size,
                                       const void *extra, unsigned int extra_size )
{
    (void)fd; (void)extra; (void)extra_size;
    assert( size == sizeof(last_reply) );
    memcpy( &last_reply, data, size );
    return 0;
}

static int pthread_mutex_lock( void *m ) { (void)m; return 0; }
static int pthread_mutex_unlock( void *m ) { (void)m; return 0; }
static int horizon_server_objects_mutex;
'''

for name in ['static void horizon_server_unlink_timer_locked(',
             'static void horizon_server_update_timers_locked(',
             'static int horizon_server_handle_polls_locked(',
             'static int horizon_server_handle_set_timer(',
             'static int horizon_server_handle_cancel_timer(']:
    fixture += function(name)

# The timer arms of the two switches, without the object types around them.
fixture += r'''
static int timer_is_signaled( const struct horizon_server_object *object ) { return object->signaled; }
static void timer_consume_signal( struct horizon_server_object *object )
{
    if (!object->manual_reset) object->signaled = 0;
}

static void set_timer( unsigned int handle, long long expire, int period )
{
    struct horizon_set_timer_request request = { handle, expire, period };
    struct horizon_server_connection connection = { 1 };

    horizon_server_handle_set_timer( &connection, (const unsigned char *)&request );
}

static void cancel_timer( unsigned int handle )
{
    struct horizon_cancel_timer_request request = { handle };
    struct horizon_server_connection connection = { 1 };

    horizon_server_handle_cancel_timer( &connection, (const unsigned char *)&request );
}

#define MS 10000LL

int main(void)
{
    objects[0].type = HORIZON_SERVER_OBJECT_TIMER;
    objects[1].type = HORIZON_SERVER_OBJECT_MSG_QUEUE;

    /* Setting a timer does not signal it, and a wait on it looks again. */
    set_timer( 1, -16 * MS, 0 );
    assert( !last_reply.header.error && !last_reply.signaled );
    assert( objects[0].timer_when == clock_ns100 + 16 * MS && !timer_is_signaled( &objects[0] ) );
    assert( horizon_server_handle_polls_locked( 1 ) && horizon_server_handle_polls_locked( 2 ) );
    horizon_server_update_timers_locked();
    assert( !timer_is_signaled( &objects[0] ) );

    /* It is signalled once its time has come, and taking it resets it. */
    clock_ns100 += 16 * MS;
    horizon_server_update_timers_locked();
    assert( timer_is_signaled( &objects[0] ) && !objects[0].timer_when );
    assert( !horizon_server_handle_polls_locked( 1 ) );  /* nothing left to wait for */
    timer_consume_signal( &objects[0] );
    assert( !timer_is_signaled( &objects[0] ) );
    clock_ns100 += 100 * MS;
    horizon_server_update_timers_locked();
    assert( !timer_is_signaled( &objects[0] ) );  /* it does not come back on its own */

    /* Setting it again reports the state it was in. */
    set_timer( 1, -MS, 0 );
    clock_ns100 += MS;
    horizon_server_update_timers_locked();
    assert( timer_is_signaled( &objects[0] ) );
    set_timer( 1, -MS, 0 );
    assert( last_reply.signaled == 1 && !timer_is_signaled( &objects[0] ) );

    /* A periodic timer comes back, and periods missed while nothing waited
     * do not each signal in turn. */
    set_timer( 1, -MS, 16 );
    clock_ns100 += MS;
    horizon_server_update_timers_locked();
    assert( timer_is_signaled( &objects[0] ) && objects[0].timer_when == clock_ns100 + 16 * MS );
    timer_consume_signal( &objects[0] );
    clock_ns100 += 100 * MS;
    horizon_server_update_timers_locked();
    assert( timer_is_signaled( &objects[0] ) && objects[0].timer_when > clock_ns100 );
    assert( objects[0].timer_when <= clock_ns100 + 16 * MS );

    /* A manual timer stays signalled; cancelling stops it either way. */
    objects[0].manual_reset = 1;
    timer_consume_signal( &objects[0] );
    assert( timer_is_signaled( &objects[0] ) );
    cancel_timer( 1 );
    assert( last_reply.signaled == 1 );
    assert( !timer_is_signaled( &objects[0] ) && !objects[0].timer_when && !objects[0].timer_period );
    clock_ns100 += 1000 * MS;
    horizon_server_update_timers_locked();
    assert( !timer_is_signaled( &objects[0] ) );

    /* An absolute time is taken as it is, and one in the past is due at once. */
    set_timer( 1, clock_ns100 + 5 * MS, 0 );
    assert( objects[0].timer_when == clock_ns100 + 5 * MS );
    set_timer( 1, clock_ns100 - 5 * MS, 0 );
    assert( objects[0].timer_when == clock_ns100 );
    horizon_server_update_timers_locked();
    assert( timer_is_signaled( &objects[0] ) );

    /* With no timer running the pass has nothing to walk. */
    cancel_timer( 1 );
    assert( !horizon_server_timers );
    set_timer( 2, -MS, 0 );  /* not a timer: refused, and nothing is armed */
    assert( last_reply.header.error == HORIZON_STATUS_INVALID_HANDLE );
    assert( !horizon_server_timers );

    puts( "Waitable timers: set does not signal, expiry does, reset, previous state, periods, "
          "manual reset, cancel and absolute times passed" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'waitable_timer.c'
    c.write_text(fixture)
    exe = Path(tmp) / 'waitable_timer'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
