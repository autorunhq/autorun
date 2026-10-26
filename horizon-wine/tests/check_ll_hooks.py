#!/usr/bin/env python3
"""Window hooks: the chain DirectInput reads its mouse and keyboard through.

SetWindowsHookEx(WH_MOUSE_LL/WH_KEYBOARD_LL) had nothing answering it in the
Horizon server, so dinput.dll's input thread got no hook handle at all and a
DirectInput game saw no input (Harbinger). The server keeps the hooks now
(dlls/ntdll/unix/horizon.c, logic in horizon_hooks.h), and win32u runs the
chain for every input event before it is queued
(server_send_hardware_message in dlls/win32u/message.c).
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()
hooks_header = (root / 'dlls/ntdll/unix/horizon_hooks.h').read_text()
message = (root / 'dlls/win32u/message.c').read_text()


def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# The requests are answered, under the numbers include/wine/server_protocol.h's
# enum request gives them.
for name, number in (('set_hook', 199), ('remove_hook', 200), ('start_hook_chain', 201),
                     ('finish_hook_chain', 202), ('get_hook_info', 203)):
    assert f'#define HORIZON_REQ_{name.upper()} {number}' in horizon
    assert f'case HORIZON_REQ_{name.upper()}:' in horizon
    assert f'horizon_server_handle_{name}(' in horizon

# is_hooked() reads hooks_count: every queue learns of every hook set or gone.
counts = function(horizon, 'static void horizon_server_update_hook_counts_locked')
assert 'horizon_hook_count_for' in counts and 'hooks_count[i]' in counts
assert 'horizon_server_update_hook_counts_locked();' in function(horizon, 'static int horizon_server_handle_set_hook')
assert 'horizon_server_update_hook_counts_locked();' in function(horizon, 'static int horizon_server_handle_remove_hook')
queue = function(horizon, 'static struct horizon_msgq *horizon_server_queue_locked')
assert 'horizon_hook_count_for' in queue  # a new queue reports the hooks already set
assert 'horizon_server_remove_hooks_for_thread_locked( tid )' in horizon  # thread gone, hooks gone

# remove_hook clears the proc of a hook a chain walks through, and frees it
# only once finish_hook_chain lets go (server/hook.c).
remove = function(horizon, 'static int horizon_server_handle_remove_hook')
assert 'hook->proc = 0;' in remove and 'chain_count' in remove

# win32u runs the chain for each message one input becomes, before the server
# hears of it, and an event the chain swallows never arrives.
dispatch = function(message, 'static DWORD ll_hook_mouse_input')
assert 'call_hooks( WH_MOUSE_LL, HC_ACTION' in dispatch
keyb = function(message, 'static BOOL ll_hook_keyboard_input')
assert 'call_hooks( WH_KEYBOARD_LL, HC_ACTION' in keyb
send = function(message, 'NTSTATUS server_send_hardware_message')
assert send.index('ll_hook_mouse_input') < send.index('wine_server_call( req )')
assert 'if (!swallow)' in send and 'is_hooked( WH_MOUSE_LL )' in send and 'is_hooked( WH_KEYBOARD_LL )' in send
# The cursor the hook sees is measured from where it is, without the screen's
# edges -- dinput subtracts its own GetCursorPos() for the movement.
point = function(message, 'static POINT ll_hook_mouse_point')
assert 'desktop_shm->cursor.x + input->mouse.x' in point
assert 'MOUSEEVENTF_ABSOLUTE' in point

logic_fixture = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@HEADER@

static struct horizon_hook_entry *hooks;

static struct horizon_hook_entry *add( int id, unsigned int pid, unsigned int tid,
                                       unsigned int owner_tid, unsigned long long proc )
{
    struct horizon_hook_entry *hook = calloc( 1, sizeof(*hook) );

    hook->id = id;
    hook->pid = pid;
    hook->tid = tid;
    hook->owner_pid = 1;
    hook->owner_tid = owner_tid;
    hook->event_min = HORIZON_EVENT_MIN;
    hook->event_max = HORIZON_EVENT_MAX;
    hook->proc = proc;
    hook->handle = 0x20 + (unsigned int)proc * 2;
    hook->next = hooks;
    hooks = hook;   /* newest first, as server/hook.c's list_add_head */
    return hook;
}

int main( void )
{
    struct horizon_hook_entry *thread1, *global, *thread2, *ll, *hook;
    int index = HORIZON_WH_CBT - HORIZON_WH_MINHOOK;

    /* The chain: the thread's own hooks newest first, then the global ones.
     * The thread hooks are WH_CBT (DirectInput watches its window with one),
     * and the global one stands for any global hook of the chain. */
    thread1 = add( HORIZON_WH_CBT, 0, 10, 10, 1 );
    global  = add( HORIZON_WH_CBT, 0, 0, 20, 2 );
    thread2 = add( HORIZON_WH_CBT, 0, 10, 10, 3 );

    hook = horizon_hook_next( hooks, NULL, 1, 10, index, 0 );
    assert( hook == thread2 );
    hook = horizon_hook_next( hooks, hook, 1, 10, index, 0 );
    assert( hook == thread1 );
    hook = horizon_hook_next( hooks, hook, 1, 10, index, 0 );
    assert( hook == global );
    assert( !horizon_hook_next( hooks, hook, 1, 10, index, 0 ) );

    /* Another thread sees only the global hook, and a different id is not this
     * chain at all. */
    assert( horizon_hook_next( hooks, NULL, 1, 11, index, 0 ) == global );
    assert( !horizon_hook_next( hooks, NULL, 1, 10,
                                HORIZON_WH_KEYBOARD_LL - HORIZON_WH_MINHOOK, 0 ) );

    /* remove_hook clears the proc: the chain walks past it, and the counts
     * is_hooked() reads go down. */
    thread1->proc = 0;
    hook = horizon_hook_next( hooks, NULL, 1, 10, index, 0 );
    assert( hook == thread2 );
    hook = horizon_hook_next( hooks, hook, 1, 10, index, 0 );
    assert( hook == global );
    assert( horizon_hook_count_for( hooks, index, 10 ) == 2 );
    assert( horizon_hook_count_for( hooks, index, 11 ) == 1 );

    /* A hook for one event range only answers there. */
    global->event_min = global->event_max = 5;
    assert( !horizon_hook_next( hooks, NULL, 1, 11, index, 0 ) );
    assert( horizon_hook_next( hooks, NULL, 1, 11, index, 5 ) == global );
    global->event_min = HORIZON_EVENT_MIN;
    global->event_max = HORIZON_EVENT_MAX;

    /* Low-level hooks run on the thread that set them wherever input is sent
     * from; only there is the chain called in place. */
    ll = add( HORIZON_WH_MOUSE_LL, 0, 0, 20, 4 );
    assert( horizon_hook_runs_in_owner( ll, 21 ) );
    assert( !horizon_hook_runs_in_owner( ll, ll->owner_tid ) );
    assert( !horizon_hook_runs_in_owner( thread2, 20 ) );
    assert( horizon_hook_count_for( hooks, HORIZON_WH_MOUSE_LL - HORIZON_WH_MINHOOK, 21 ) == 1 );

    /* The filter on a thread hook (WH_CBT and friends). */
    assert( horizon_hook_runs_in( thread2, 1, 10 ) );
    assert( !horizon_hook_runs_in( thread2, 1, 11 ) );

    printf( "ll hooks: the chain's order, its filters, and the counts is_hooked() reads\n" );
    return 0;
}
"""

# The handlers themselves, extracted whole and exercised: set_hook, the chain
# walking its hooks, and remove_hook.
def block(start_marker, end_marker):
    start = horizon.index(start_marker)
    end = horizon.index(end_marker, start)
    return horizon[start:end]


def definition(name):
    match = re.search(r'^static [^\n]*\b' + name + r'\(', horizon, re.M)
    assert match, name
    brace = horizon.index('{', match.start())
    depth, pos = 1, brace + 1
    while depth:
        depth += (horizon[pos] == '{') - (horizon[pos] == '}')
        pos += 1
    return horizon[match.start():pos] + '\n'


def struct(name):
    start = horizon.index('struct ' + name + '\n{')
    return horizon[start:horizon.index('\n};', start) + 3]


constants = '\n'.join(re.findall(
    r'^#define HORIZON_(?:STATUS_|FIRST_USER_HANDLE|LAST_USER_HANDLE|MAX_USER_HANDLES|NTUSER_OBJ_HOOK)\S* .*',
    horizon, re.M))
wire = block('/* NtUserSetWindowsHookEx and friends', 'struct horizon_send_message_request')
wire_asserts = '\n'.join(re.findall(r'^C_ASSERT\( sizeof\(struct horizon_(?:set_hook|remove_hook|start_hook_chain|finish_hook_chain|get_hook_info)[^;]+;', horizon, re.M))
handlers = ''.join(definition(name) for name in (
    'horizon_server_hook_locked', 'horizon_server_free_hook_locked',
    'horizon_server_update_hook_counts_locked', 'horizon_server_remove_hooks_for_thread_locked',
    'horizon_server_fill_hook_reply_locked', 'horizon_server_handle_set_hook',
    'horizon_server_handle_remove_hook', 'horizon_server_handle_start_hook_chain',
    'horizon_server_handle_finish_hook_chain', 'horizon_server_handle_get_hook_info'))
types = ''.join(struct(name) + ';\n' for name in (
    'horizon_server_request_header', 'horizon_server_reply_header', 'horizon_obj_locator',
    'horizon_queue_shm', 'horizon_user_entry'))

handler_fixture = r"""
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "horizon_msg_queue.h"

@HOOKS_HEADER@
@TYPES@
@CONSTANTS@
#define C_ASSERT(x) _Static_assert(x, #x)
@WIRE@
@WIRE_ASSERTS@

struct horizon_session_shm
{
    struct horizon_user_entry user_entries[HORIZON_MAX_USER_HANDLES];
};
static struct horizon_session_shm fake_session;
static unsigned char *horizon_session_data = (unsigned char *)&fake_session;
static unsigned int horizon_user_handle_count;
static struct horizon_hook_entry *horizon_hooks;
static struct horizon_msgqs horizon_msg_queues;

struct horizon_shared_object
{
    unsigned long long id;
    struct { struct horizon_queue_shm queue; } shm;
};
static struct horizon_shared_object fake_shared;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned int horizon_server_alloc_user_handle_locked( unsigned short type,
                                                             struct horizon_obj_locator locator,
                                                             unsigned int pid, unsigned int tid,
                                                             unsigned int *handle )
{
    unsigned int index = horizon_user_handle_count++;
    struct horizon_user_entry *entry = &fake_session.user_entries[index];

    (void)locator;
    entry->type = type;
    entry->generation = 1;
    entry->pid = pid;
    entry->tid = tid;
    *handle = (index << 1) + HORIZON_FIRST_USER_HANDLE + 0x10000;
    return 0;
}
static unsigned int horizon_server_flush_session_range_locked( unsigned long long offset,
                                                               unsigned long long size )
{
    (void)offset; (void)size;
    return 0;
}
static struct horizon_shared_object *horizon_server_shared_object_locked( struct horizon_obj_locator locator )
{
    return locator.id == fake_shared.id ? &fake_shared : NULL;
}

/* What the last write_reply carried, for the test to look at. */
static unsigned char last_reply[64];
static size_t last_reply_size;
static unsigned int last_status = 0xdeadbeef;

static int horizon_server_write_reply( int fd, const void *reply, size_t reply_size,
                                       const void *data, size_t data_size )
{
    (void)fd; (void)data; (void)data_size;
    assert( reply_size <= sizeof(last_reply) );
    memcpy( last_reply, reply, reply_size );
    last_reply_size = reply_size;
    last_status = 0;
    return 0;
}
static int horizon_server_write_status( int fd, unsigned int status )
{
    (void)fd;
    last_status = status;
    return 0;
}
void horizon_trace( const char *fmt, ... ) { (void)fmt; }

struct horizon_server_connection
{
    unsigned int pid, tid;
    int reply_fd;
};

@HANDLERS@

int main( void )
{
    struct horizon_server_connection hooker = { 1, 10, -1 };
    struct horizon_server_connection caller = { 1, 20, -1 };
    struct horizon_start_hook_chain_request start;
    struct horizon_finish_hook_chain_request finish;
    struct horizon_get_hook_info_request info;
    struct horizon_remove_hook_request remove;
    struct horizon_set_hook_request set;
    unsigned char buf[64];
    unsigned int handle;

    fake_shared.id = 1;

    /* DirectInput's input thread sets the low-level mouse hook. */
    memset( &set, 0, sizeof(set) );
    set.id = HORIZON_WH_MOUSE_LL;
    set.event_min = HORIZON_EVENT_MIN;
    set.event_max = HORIZON_EVENT_MAX;
    set.proc = 0x401000;
    set.unicode = 1;
    memcpy( buf, &set, sizeof(set) );
    assert( horizon_server_handle_set_hook( &hooker, buf, NULL, 0 ) == 0 );
    assert( last_status == 0 && last_reply_size == sizeof(struct horizon_set_hook_reply) );
    handle = ((struct horizon_set_hook_reply *)last_reply)->handle;
    assert( handle );

    /* is_hooked() sees it on every thread. */
    assert( horizon_hook_count_for( horizon_hooks, HORIZON_WH_MOUSE_LL - HORIZON_WH_MINHOOK, 20 ) == 1 );

    /* The chain starts at it, set to run on the thread that set it. */
    memset( &start, 0, sizeof(start) );
    start.id = HORIZON_WH_MOUSE_LL;
    memcpy( buf, &start, sizeof(start) );
    assert( horizon_server_handle_start_hook_chain( &caller, buf ) == 0 );
    assert( last_reply_size == sizeof(struct horizon_start_hook_chain_reply) );
    assert( ((struct horizon_start_hook_chain_reply *)last_reply)->handle == handle );
    assert( ((struct horizon_start_hook_chain_reply *)last_reply)->tid == 10 );
    assert( ((struct horizon_start_hook_chain_reply *)last_reply)->proc == 0x401000 );

    /* get_hook_info answers the same hook, and get_next finds nothing behind it. */
    memset( &info, 0, sizeof(info) );
    info.handle = handle;
    memcpy( buf, &info, sizeof(info) );
    assert( horizon_server_handle_get_hook_info( &caller, buf ) == 0 );
    assert( ((struct horizon_get_hook_info_reply *)last_reply)->handle == handle );
    assert( ((struct horizon_get_hook_info_reply *)last_reply)->id == HORIZON_WH_MOUSE_LL );
    info.get_next = 1;
    memcpy( buf, &info, sizeof(info) );
    assert( horizon_server_handle_get_hook_info( &caller, buf ) == 0 );
    assert( !((struct horizon_get_hook_info_reply *)last_reply)->handle );

    /* While the chain holds it, remove_hook only clears the proc. */
    memset( &remove, 0, sizeof(remove) );
    remove.handle = handle;
    memcpy( buf, &remove, sizeof(remove) );
    assert( horizon_server_handle_remove_hook( &hooker, buf ) == 0 );
    assert( last_status == 0 );
    assert( horizon_hook_count_for( horizon_hooks, HORIZON_WH_MOUSE_LL - HORIZON_WH_MINHOOK, 20 ) == 0 );
    assert( horizon_server_hook_locked( handle ) && !horizon_server_hook_locked( handle )->proc );

    /* finish_hook_chain frees it. */
    memset( &finish, 0, sizeof(finish) );
    finish.id = HORIZON_WH_MOUSE_LL;
    memcpy( buf, &finish, sizeof(finish) );
    assert( horizon_server_handle_finish_hook_chain( &caller, buf ) == 0 );
    assert( !horizon_server_hook_locked( handle ) );

    /* A low-level hook asked for by thread is refused, as server/hook.c does. */
    set.tid = 10;
    memcpy( buf, &set, sizeof(set) );
    horizon_server_handle_set_hook( &hooker, buf, NULL, 0 );
    assert( last_reply_size == sizeof(struct horizon_set_hook_reply) );
    assert( ((struct horizon_set_hook_reply *)last_reply)->header.error == HORIZON_STATUS_INVALID_PARAMETER );
    assert( !horizon_server_hook_locked( handle ) );

    /* And a thread that goes away takes its hooks with it. */
    set.tid = 0;
    memcpy( buf, &set, sizeof(set) );
    assert( horizon_server_handle_set_hook( &hooker, buf, NULL, 0 ) == 0 );
    handle = ((struct horizon_set_hook_reply *)last_reply)->handle;
    horizon_server_remove_hooks_for_thread_locked( 10 );
    assert( !horizon_server_hook_locked( handle ) );

    printf( "ll hooks: set_hook, the chain, and remove_hook as the handlers run them\n" );
    return 0;
}
"""

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    source = tmp / 'll_hooks.c'
    source.write_text(logic_fixture.replace('@HEADER@', hooks_header))
    binary = tmp / 'll_hooks'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-o', str(binary), str(source)],
                   check=True)
    print(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip())

    source = tmp / 'll_hook_handlers.c'
    body = handler_fixture
    for marker, text_thing in (('@HOOKS_HEADER@', hooks_header), ('@TYPES@', types),
                               ('@CONSTANTS@', constants), ('@WIRE@', wire),
                               ('@WIRE_ASSERTS@', wire_asserts), ('@HANDLERS@', handlers)):
        body = body.replace(marker, text_thing)
    source.write_text(body)
    binary = tmp / 'll_hook_handlers'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-I', str(root / 'dlls/ntdll/unix'), '-o', str(binary), str(source)],
                   check=True)
    print(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip())
