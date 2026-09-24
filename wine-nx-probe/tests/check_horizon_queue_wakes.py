#!/usr/bin/env python3
"""Exercise queue publication and targeted wake edges from horizon.c."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(name):
    match = re.search(r'^static void ' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#include "horizon_msg_queue.h"
#include "horizon_message_queue.h"
#include "horizon_win_timers.h"
#define HORIZON_WM_MOUSEMOVE 0x200
#define HORIZON_WS_VISIBLE 0x10000000
struct horizon_obj_locator { unsigned long long id, offset; };
struct horizon_queue_shm { unsigned wake_bits, changed_bits, wake_mask, changed_mask; };
struct horizon_shared_object { unsigned long long id; struct { struct horizon_queue_shm queue; } shm; };
struct horizon_server_object { _Atomic int signaled; unsigned wakes; };
struct horizon_input_message { unsigned tid, msg; struct horizon_input_message *next; };
struct horizon_user_window { unsigned tid, style; int has_update_rect, has_internal_paint; struct horizon_user_window *next; };
static struct horizon_input_message *horizon_input_messages;
static struct horizon_user_window *horizon_windows;
static struct horizon_message_queue horizon_posted_messages = { NULL, &horizon_posted_messages.head };
static struct horizon_win_timers horizon_timers;
static struct horizon_msgqs horizon_msg_queues;
static struct horizon_shared_object shared[2];
static unsigned flushes;
static unsigned long long clock_ms;
static unsigned long long horizon_server_timer_clock(void) { return clock_ms; }
static struct horizon_shared_object *horizon_server_shared_object_locked(struct horizon_obj_locator loc) {
    assert(loc.offset < 2); return &shared[loc.offset];
}
static void horizon_server_flush_session_range_locked(unsigned long long offset, size_t size) {
    assert(offset < 2 && size == sizeof(struct horizon_shared_object)); flushes++;
}
static void horizon_sync_notify_object_locked(struct horizon_server_object *object, int satisfy) {
    assert(!satisfy); object->wakes++;
}
'''
tests = r'''
int main(void) {
    struct horizon_server_object sync[2] = {0};
    int created;
    struct horizon_msgq *q = horizon_msgq_get(&horizon_msg_queues, 8, &created);
    struct horizon_msgq *other = horizon_msgq_get(&horizon_msg_queues, 12, &created);
    q->sync = sync; other->sync = sync + 1;
    q->shm_id = shared[0].id = 1; other->shm_id = shared[1].id = 2; other->shm_offset = 1;
    q->wake_mask = other->wake_mask = HORIZON_MSGQ_QS_POSTMESSAGE;
    horizon_msgq_post_quit(q, 0);
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].wakes == 1 && !sync[1].wakes && sync[0].signaled);
    unsigned previous = flushes;
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].wakes == 1 && flushes == previous);
    q->quit_message = 0;
    horizon_server_refresh_wait_queue_locked(q);
    assert(!sync[0].signaled && !sync[1].wakes);
    horizon_msgq_post_quit(q, 0);
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].wakes == 2);
    q->quit_message = 0; q->wake_mask = 0; q->changed_mask = HORIZON_MSGQ_QS_PAINT;
    struct horizon_user_window win = {8, HORIZON_WS_VISIBLE, 1, 0, NULL};
    horizon_windows = &win;
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].signaled && !sync[1].wakes);
    q->changed_bits = 0;
    horizon_server_refresh_wait_queue_locked(q);
    assert(!sync[0].signaled);
    horizon_msgq_touch(q, HORIZON_MSGQ_QS_PAINT);
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].signaled && sync[0].wakes == 3);
    horizon_windows = NULL;
    q->changed_mask = 0; q->wake_mask = HORIZON_MSGQ_QS_TIMER;
    clock_ms = 100;
    unsigned long long id;
    assert(!horizon_win_timers_set(&horizon_timers, 8, 0, 0x113, 0, 25, 0, clock_ms, &id));
    horizon_server_refresh_wait_queue_locked(q);
    assert(!sync[0].signaled);
    clock_ms += 25;
    horizon_server_refresh_wait_queue_locked(q);
    assert(sync[0].signaled && sync[0].wakes == 4 && !sync[1].wakes);
    horizon_win_timers_drop(&horizon_timers, 8, 0);
    horizon_server_refresh_wait_queue_locked(q);
    assert(!sync[0].signaled);
    horizon_msgq_destroy(&horizon_msg_queues, q);
    horizon_msgq_destroy(&horizon_msg_queues, other);
    puts("Queue publication: targeted wake edges, changed-mask rearm, paint, timers and unchanged-state suppression passed");
}
'''
body = '\n'.join(function(name) for name in (
    'horizon_server_refresh_queue_locked', 'horizon_server_refresh_queues_locked',
    'horizon_server_refresh_wait_queue_locked'))
with tempfile.TemporaryDirectory(prefix='horizon-queue-wakes-') as tmp:
    code, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    code.write_text(fixture + body + tests)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'dlls/ntdll/unix'), str(code), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
