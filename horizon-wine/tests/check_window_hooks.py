#!/usr/bin/env python3
"""Exercise window hooks (0085) and low-level hooks (0099) in horizon.c on the host."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def extract_range(source, start_pat, end_pat):
    s = source.index(start_pat)
    e = source.index(end_pat, s)
    return source[s:e]


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


start_hook = horizon.index('#define HORIZON_NTUSER_OBJ_HOOK 0x0f')
end_hook_fn = horizon.index('static int horizon_server_handle_window_hook(')
brace = horizon.index('{', end_hook_fn)
depth, pos = 1, brace + 1
while depth:
    depth += (horizon[pos] == '{') - (horizon[pos] == '}')
    pos += 1
hook_block = horizon[start_hook:pos]

# The handler of a mouse event hands its hook the movement whether or not the
# cursor follows it: the point is made from it and the cursor, and DirectInput
# reads the point less the cursor as the motion. With 0 for a mouse that moves
# the cursor, Doom 3's DirectInput mouse saw none.
handler = extract_function(horizon, 'horizon_server_handle_send_hardware_message')
assert 'horizon_server_queue_mouse_hook_ll_locked( flags, mouse->data, dx, dy, time, mouse->info );' in handler, \
    'the low-level mouse hook must get the movement'

ll_helpers = '\n\n'.join([
    extract_function(horizon, 'horizon_server_queue_mouse_hook_ll_locked'),
    extract_function(horizon, 'horizon_server_queue_key_hook_ll_locked'),
    extract_function(horizon, 'horizon_server_hook_ll_point_locked'),
])

ll_defs = '\n'.join(re.findall(r'^#define HORIZON_WM_(?:MBUTTON|MOUSE|XBUTTON)\w* .*', horizon, re.M))

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "horizon_msg_queue.h"
#include "horizon_keyboard.h"

#define HORIZON_REQ_SET_HOOK 199
#define HORIZON_REQ_REMOVE_HOOK 200
#define HORIZON_REQ_START_HOOK_CHAIN 201
#define HORIZON_REQ_FINISH_HOOK_CHAIN 202
#define HORIZON_REQ_GET_HOOK_INFO 203

#define HORIZON_STATUS_SUCCESS 0u
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_STATUS_INVALID_PARAMETER 0xc000000du
#define HORIZON_STATUS_NO_MEMORY 0xc0000017u
#define HORIZON_STATUS_NOT_IMPLEMENTED 0xc0000002u

#define HORIZON_FIRST_USER_HANDLE 0x1000u
#define HORIZON_MAX_USER_HANDLES 64
#define HORIZON_IMDT_KEYBOARD 1u
#define HORIZON_IMDT_MOUSE 2u
#define HORIZON_IMO_HARDWARE 1u
#define HORIZON_WM_LBUTTONDOWN 0x0201u
#define HORIZON_WM_LBUTTONUP 0x0202u
#define HORIZON_WM_RBUTTONDOWN 0x0204u
#define HORIZON_WM_RBUTTONUP 0x0205u

struct horizon_server_request_header { int req; unsigned int request_size; unsigned int reply_size; };
struct horizon_server_reply_header { unsigned int error; unsigned int reply_size; };
struct horizon_obj_locator { unsigned long long id; unsigned long long offset; };
struct horizon_rectangle { int left, top, right, bottom; };
struct horizon_shared_cursor { int x, y; unsigned int last_change; struct horizon_rectangle clip; };
struct horizon_desktop_shm { unsigned int flags; struct horizon_shared_cursor cursor; unsigned char keystate[256]; unsigned long long monitor_serial, keystate_serial; };
struct horizon_queue_shm { long long access_time; unsigned int wake_mask, wake_bits, changed_mask, changed_bits, internal_bits; int hooks_count[17]; };
struct horizon_shared_object { long long seq; unsigned long long id; union { struct horizon_queue_shm queue; struct horizon_desktop_shm desktop; } shm; };
struct horizon_user_entry { unsigned long long offset; unsigned int tid, pid; unsigned long long id; union { struct { unsigned short type, generation; }; long long uniq; }; };
struct horizon_session_shm { struct horizon_user_entry user_entries[HORIZON_MAX_USER_HANDLES]; };
struct horizon_hw_keyboard_input { int type; unsigned short vkey, scan; unsigned int flags, time; unsigned long long info; };
struct horizon_hardware_msg_data {
    unsigned long long info; unsigned int size; int pad; unsigned int hw_id, flags;
    struct { unsigned int device, origin; } source;
};
struct horizon_get_message_reply {
    struct horizon_server_reply_header header;
    unsigned int win, msg; unsigned long long wparam, lparam;
    int type, x, y; unsigned int time, total; char __pad_52[4];
};
struct horizon_server_object {
    struct { unsigned int tid; int terminated; } thread;
    struct horizon_server_object *thread_next;
};
struct horizon_server_connection { int reply_fd; unsigned int pid, tid; };

static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_msgqs horizon_msg_queues;
static struct horizon_server_object *horizon_server_threads;
static struct horizon_session_shm mock_session_shm;
static void *horizon_session_data = &mock_session_shm;
static unsigned int horizon_user_handle_count = HORIZON_MAX_USER_HANDLES;
static struct horizon_shared_object mock_queue_shms[4];
static struct horizon_shared_object mock_desktop_shm;
static unsigned char last_reply[1024];
static unsigned int last_reply_size;
static unsigned char last_extra[1024];
static unsigned int last_extra_size;
static unsigned int refresh_calls;

static void horizon_trace( const char *fmt, ... ) { (void)fmt; }
static unsigned long long horizon_server_timer_clock( void ) { return 1000ull; }
static void horizon_server_flush_session_range_locked( unsigned long long off, unsigned long long size ) { (void)off; (void)size; }

static struct horizon_shared_object *horizon_server_shared_object_locked( struct horizon_obj_locator loc )
{
    if (loc.id >= 1 && loc.id <= 4) return &mock_queue_shms[loc.id - 1];
    return NULL;
}

static struct horizon_desktop_shm *horizon_server_desktop_shared_locked( struct horizon_obj_locator *loc )
{
    if (loc) { loc->id = 99; loc->offset = 0; }
    return &mock_desktop_shm.shm.desktop;
}

static struct horizon_msgq *horizon_server_queue_locked( unsigned int tid )
{
    int created = 0;
    struct horizon_msgq *q = horizon_msgq_get( &horizon_msg_queues, tid, &created );
    if (created)
    {
        unsigned int slot = (tid & 3) + 1;
        q->shm_id = slot;
        mock_queue_shms[slot - 1].id = slot;
    }
    return q;
}

static void horizon_server_refresh_queue_locked( struct horizon_msgq *queue, unsigned long long now )
{
    (void)queue; (void)now;
    refresh_calls++;
}

static unsigned int horizon_server_alloc_user_handle_locked( unsigned short type, struct horizon_obj_locator loc,
                                                             unsigned int pid, unsigned int tid, unsigned int *out )
{
    unsigned int i;
    (void)loc;
    for (i = 0; i < horizon_user_handle_count; i++)
    {
        struct horizon_user_entry *e = &mock_session_shm.user_entries[i];
        if (e->type) continue;
        e->generation++;
        if (!e->generation) e->generation = 1;
        e->type = type;
        e->pid = pid;
        e->tid = tid;
        *out = ((unsigned int)e->generation << 16) | (HORIZON_FIRST_USER_HANDLE + i * 2);
        return HORIZON_STATUS_SUCCESS;
    }
    return HORIZON_STATUS_NO_MEMORY;
}

static int horizon_server_write_reply( int fd, const void *reply, size_t size, const void *data, size_t data_size )
{
    (void)fd;
    assert( size <= sizeof(last_reply) && data_size <= sizeof(last_extra) );
    memcpy( last_reply, reply, size );
    last_reply_size = (unsigned int)size;
    if (data && data_size) memcpy( last_extra, data, data_size );
    last_extra_size = (unsigned int)data_size;
    return 0;
}

''' + hook_block + '\n\n' + ll_defs + '\n' + ll_helpers + r'''

static struct horizon_server_object t1, t2, t3;

static void test_hooks_lifecycle_and_chains( void )
{
    struct horizon_server_connection c1 = { 1, 0x100, 1 };
    struct horizon_server_connection c2 = { 1, 0x100, 2 };
    struct horizon_server_connection c3 = { 1, 0x100, 3 };
    struct horizon_set_hook_request set_req;
    struct horizon_remove_hook_request rem_req;
    struct horizon_start_hook_chain_request start_req;
    struct horizon_finish_hook_chain_request fin_req;
    struct horizon_get_hook_info_request info_req;
    const struct horizon_set_hook_reply *set_rep = (const void *)last_reply;
    const struct horizon_start_hook_chain_reply *start_rep = (const void *)last_reply;
    const struct horizon_get_hook_info_reply *info_rep = (const void *)last_reply;
    const unsigned char mod_name[] = "hookmod.dll";
    unsigned int h_thread, h_global, h_g2;

    t1.thread.tid = 1; t1.thread_next = &t2;
    t2.thread.tid = 2; t2.thread_next = &t3;
    t3.thread.tid = 3;
    horizon_server_threads = &t1;
    horizon_server_queue_locked( 1 );
    horizon_server_queue_locked( 2 );
    horizon_server_queue_locked( 3 );

    /* Reject unsupported/bad hooks cleanly. */
    memset( &set_req, 0, sizeof(set_req) );
    set_req.id = HORIZON_WH_JOURNALPLAYBACK;
    set_req.proc = 0x1111;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( set_rep->header.error == HORIZON_STATUS_NOT_IMPLEMENTED );

    set_req.id = HORIZON_WH_KEYBOARD_LL;
    set_req.tid = 1; /* LL hooks must be global (tid == 0) */
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( set_rep->header.error == HORIZON_STATUS_INVALID_PARAMETER );

    /* Install global WH_CALLWNDPROC (id=4) on c1 and per-thread WH_CALLWNDPROC on c2. */
    set_req.id = 4;
    set_req.tid = 0;
    set_req.proc = 0x2222;
    set_req.unicode = 1;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, mod_name, sizeof(mod_name) );
    assert( !set_rep->header.error && (h_global = set_rep->handle) );

    set_req.tid = 2;
    set_req.proc = 0x3333;
    horizon_server_handle_window_hook( &c2, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_thread = set_rep->handle) );

    /* Check hooks_count[] in shm: tid 1 sees global (1), tid 2 sees thread+global (2). */
    assert( mock_queue_shms[1].shm.queue.hooks_count[4 - HORIZON_WH_MINHOOK] == 1 );
    assert( mock_queue_shms[2].shm.queue.hooks_count[4 - HORIZON_WH_MINHOOK] == 2 );

    /* Chain traversal on c2: thread hook comes first, then global hook with module name. */
    memset( &start_req, 0, sizeof(start_req) );
    start_req.header.reply_size = 520;
    start_req.id = 4;
    horizon_server_handle_window_hook( &c2, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( !start_rep->header.error && start_rep->handle == h_thread && start_rep->proc == 0x3333 && start_rep->tid == 0 );

    memset( &info_req, 0, sizeof(info_req) );
    info_req.header.reply_size = 520;
    info_req.handle = h_thread;
    info_req.get_next = 1;
    horizon_server_handle_window_hook( &c2, (const void *)&info_req, HORIZON_REQ_GET_HOOK_INFO, NULL, 0 );
    assert( !info_rep->header.error && info_rep->handle == h_global && info_rep->proc == 0x2222 );
    assert( last_extra_size == sizeof(mod_name) && !memcmp( last_extra, mod_name, sizeof(mod_name) ) );

    /* Remove h_thread while chain is active: deferred free until finish_hook_chain. */
    memset( &rem_req, 0, sizeof(rem_req) );
    rem_req.handle = h_thread;
    horizon_server_handle_window_hook( &c2, (const void *)&rem_req, HORIZON_REQ_REMOVE_HOOK, NULL, 0 );
    assert( !last_reply[0] && horizon_window_hook_find_locked( h_thread ) != NULL );
    assert( mock_queue_shms[2].shm.queue.hooks_count[4 - HORIZON_WH_MINHOOK] == 1 );

    memset( &fin_req, 0, sizeof(fin_req) );
    fin_req.id = 4;
    horizon_server_handle_window_hook( &c2, (const void *)&fin_req, HORIZON_REQ_FINISH_HOOK_CHAIN, NULL, 0 );
    assert( horizon_window_hook_find_locked( h_thread ) == NULL );

    /* Cleanup global hook. */
    rem_req.handle = h_global;
    horizon_server_handle_window_hook( &c1, (const void *)&rem_req, HORIZON_REQ_REMOVE_HOOK, NULL, 0 );
    assert( horizon_window_hooks == NULL );

    /* Thread exiting mid-hook-chain releases its chain count and deferred unlinked hooks. */
    set_req.id = 4;
    set_req.tid = 0;
    set_req.proc = 0x2345;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_global = set_rep->handle) );

    memset( &start_req, 0, sizeof(start_req) );
    start_req.header.reply_size = 520;
    start_req.id = 4;
    horizon_server_handle_window_hook( &c2, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( !start_rep->header.error && horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 1 );

    rem_req.handle = h_global;
    horizon_server_handle_window_hook( &c1, (const void *)&rem_req, HORIZON_REQ_REMOVE_HOOK, NULL, 0 );
    assert( horizon_window_hook_find_locked( h_global ) != NULL );

    horizon_server_remove_thread_hooks_locked( 2 );
    assert( horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 0 );
    assert( horizon_window_hook_find_locked( h_global ) == NULL );
    assert( horizon_window_hooks == NULL );

    /* Overlapping nested + cross-thread chains, foreign finish rejection, concurrent remove, partial exit. */
    set_req.id = 4;
    set_req.tid = 0;
    set_req.proc = 0x3456;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_global = set_rep->handle) );
    set_req.proc = 0x4567;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_g2 = set_rep->handle) );

    /* c2 starts 2 nested chains, c3 starts 1 overlapping chain (global == 3). */
    horizon_server_handle_window_hook( &c2, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( !start_rep->header.error && start_rep->handle == h_g2 );
    horizon_server_handle_window_hook( &c2, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( !start_rep->header.error );
    horizon_server_handle_window_hook( &c3, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( !start_rep->header.error );
    assert( horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 3 );

    /* Remove both global hooks while chains are active: deferred free keeps both intact. */
    rem_req.handle = h_g2;
    horizon_server_handle_window_hook( &c1, (const void *)&rem_req, HORIZON_REQ_REMOVE_HOOK, NULL, 0 );
    rem_req.handle = h_global;
    horizon_server_handle_window_hook( &c1, (const void *)&rem_req, HORIZON_REQ_REMOVE_HOOK, NULL, 0 );
    assert( horizon_window_hook_find_locked( h_g2 ) != NULL );
    assert( horizon_window_hook_find_locked( h_global ) != NULL );

    /* Thread c1 with no active chain cannot prematurely finish another thread's chain. */
    fin_req.id = 4;
    horizon_server_handle_window_hook( &c1, (const void *)&fin_req, HORIZON_REQ_FINISH_HOOK_CHAIN, NULL, 0 );
    assert( ((const struct horizon_server_reply_header *)last_reply)->error == HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 3 );

    /* c2 exits mid-chain: drops only its 2 chains; c3 still keeps global == 1 and hooks alive. */
    horizon_server_remove_thread_hooks_locked( 2 );
    assert( horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 1 );
    assert( horizon_window_hook_find_locked( h_g2 ) != NULL );
    assert( horizon_window_hook_find_locked( h_global ) != NULL );

    /* Only when c3 finishes its chain does global drop to 0 and both deferred hooks free. */
    horizon_server_handle_window_hook( &c3, (const void *)&fin_req, HORIZON_REQ_FINISH_HOOK_CHAIN, NULL, 0 );
    assert( !((const struct horizon_server_reply_header *)last_reply)->error );
    assert( horizon_window_hook_chains[4 - HORIZON_WH_MINHOOK] == 0 );
    assert( horizon_window_hook_find_locked( h_g2 ) == NULL );
    assert( horizon_window_hook_find_locked( h_global ) == NULL );
    assert( horizon_window_hooks == NULL );
}

static void test_ll_hooks_queue_and_drain( void )
{
    struct horizon_server_connection c1 = { 1, 0x100, 1 };
    struct horizon_server_connection c2 = { 1, 0x100, 2 };
    struct horizon_set_hook_request set_req;
    struct horizon_start_hook_chain_request start_req;
    const struct horizon_set_hook_reply *set_rep = (const void *)last_reply;
    const struct horizon_start_hook_chain_reply *start_rep = (const void *)last_reply;
    struct horizon_msgq *q1 = horizon_server_queue_locked( 1 );
    struct horizon_hw_keyboard_input kbd = { 0 };
    struct horizon_key_event ke = { 0 };
    unsigned int h_kbd, h_mouse, i;

    memset( &set_req, 0, sizeof(set_req) );
    set_req.id = HORIZON_WH_KEYBOARD_LL;
    set_req.proc = 0x4444;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_kbd = set_rep->handle) );

    set_req.id = HORIZON_WH_MOUSE_LL;
    set_req.proc = 0x5555;
    horizon_server_handle_window_hook( &c1, (const void *)&set_req, HORIZON_REQ_SET_HOOK, NULL, 0 );
    assert( !set_rep->header.error && (h_mouse = set_rep->handle) );

    /* Cross-thread chain reports hook owner tid/pid so caller runs in owner thread. */
    memset( &start_req, 0, sizeof(start_req) );
    start_req.id = HORIZON_WH_MOUSE_LL;
    horizon_server_handle_window_hook( &c2, (const void *)&start_req, HORIZON_REQ_START_HOOK_CHAIN, NULL, 0 );
    assert( start_rep->handle == h_mouse && start_rep->tid == 1 && start_rep->pid == 0x100 );

    /* Queue mouse move + button click + keyboard LL messages, verify pending cap (64). */
    mock_desktop_shm.shm.desktop.cursor.x = 400;
    mock_desktop_shm.shm.desktop.cursor.y = 300;
    kbd.vkey = HORIZON_KBD_VK_MENU;
    kbd.scan = 0x38;
    ke.message = 0x0104; /* WM_SYSKEYDOWN */
    ke.vkey = 0xa4;      /* VK_LMENU */
    ke.data_flags = 0x20;
    horizon_server_queue_key_hook_ll_locked( &kbd, &ke, 123 );
    horizon_server_queue_mouse_hook_ll_locked( 0x0001 | 0x0002, 0, 15, -10, 124, 0xdead );
    assert( horizon_server_hook_ll_pending_locked( q1 ) == 3 );

    /* Verify point adjustment uses current cursor pos on get_message for mouse move (and click). */
    mock_desktop_shm.shm.desktop.cursor.x = 500;
    mock_desktop_shm.shm.desktop.cursor.y = 200;
    for (i = 0; i < 3; i++)
    {
        struct horizon_msgq_sent *sent = horizon_msgq_receive( q1 );
        struct horizon_get_message_reply gm;
        assert( sent && sent->type == HORIZON_MSG_HOOK_LL );
        memset( &gm, 0, sizeof(gm) );
        gm.type = sent->type;
        gm.msg = sent->msg;
        gm.x = sent->x;
        gm.y = sent->y;
        horizon_server_hook_ll_point_locked( &gm );
        if (sent->msg == HORIZON_WH_MOUSE_LL && sent->wparam == HORIZON_WM_MOUSEMOVE)
            assert( gm.x == 515 && gm.y == 190 );
        else if (sent->msg == HORIZON_WH_MOUSE_LL && sent->wparam == HORIZON_WM_LBUTTONDOWN)
            assert( gm.x == 500 && gm.y == 200 );
        else if (sent->msg == HORIZON_WH_KEYBOARD_LL)
            assert( (sent->lparam & 0xff) == 0xa4 && ((sent->lparam >> 16) & 0xff) == 0x38 );
        free( sent->data );
        free( sent );
        assert( !horizon_msgq_reply( q1, 0, 0, 1, NULL, 0 ) );
    }
    assert( horizon_server_hook_ll_pending_locked( q1 ) == 0 && !q1->recv_result );

    /* Cap at HORIZON_HOOK_LL_PENDING_MAX (64) messages if thread stops pumping. */
    for (i = 0; i < 80; i++)
        horizon_server_queue_mouse_hook_ll_locked( 0x0001, 0, 1, 1, 200 + i, 0 );
    assert( horizon_server_hook_ll_pending_locked( q1 ) == HORIZON_HOOK_LL_PENDING_MAX );

    /* Thread exit removes remaining hooks owned by tid 1. */
    horizon_server_remove_thread_hooks_locked( 1 );
    assert( !horizon_window_hook_from_locked( horizon_window_hooks, HORIZON_WH_MOUSE_LL - HORIZON_WH_MINHOOK, 0, 1 ) );
}

int main( void )
{
    test_hooks_lifecycle_and_chains();
    test_ll_hooks_queue_and_drain();
    puts( "Window hooks (0085) and low-level hooks (0099) lifecycle, chains, shm, and LL delivery passed" );
    return 0;
}
'''

cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'hooks_test.c'
    exe = Path(tmp) / 'hooks_test'
    src.write_text(fixture)
    subprocess.run([
        cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-variable',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
        '-I' + str(root / 'dlls/ntdll/unix'),
        str(src), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
