#!/usr/bin/env python3
"""Exercise direct synchronization dispatch with the production handlers."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
source = (root / "dlls/ntdll/unix/horizon.c").read_text()


def definition(name, struct=False):
    pattern = r"^struct " + name + r"\s*\{" if struct else r"^(?:static )?(?:inline )?(?:unsigned int|long long|int|void|struct horizon_server_object \*)\s*" + name + r"\([^;{]*\)\s*\{"
    match = re.search(pattern, source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + (";\n" if struct else "\n")


constants = "\n".join(re.findall(r"^#define HORIZON_(?:STATUS_|REQ_|SELECT_|SERVER_OBJECT_|SERVER_FIXED_MESSAGE_SIZE|APC_|PULSE_EVENT|SET_EVENT|RESET_EVENT|CURRENT_THREAD_HANDLE).*", source, re.M))
constants += "\n" + "\n".join(re.findall(r"enum horizon_(?:select_opcode|event_op|server_object_type)\s*\{[^}]+\};", source))
types = "".join(definition("horizon_" + name, True) for name in (
    "server_request_header", "server_reply_header", "select_request", "select_reply",
    "select_wait_op", "select_signal_and_wait_op", "event_op_request", "event_op_reply",
    "query_event_request", "query_event_reply", "release_mutex_request", "release_mutex_reply",
    "query_mutex_request", "query_mutex_reply", "release_semaphore_request", "release_semaphore_reply",
    "query_semaphore_request", "query_semaphore_reply"))
fixture = r'''
#define HORIZON_STANDALONE_SYNTAX
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "horizon_threads.h"
#include "horizon_msg_queue.h"
#include "horizon_win_timers.h"
#include "horizon_completion.h"
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define HORIZON_SERVER_WAIT_SLICE 200000LL
#define HORIZON_SERVER_POLL_INTERVAL 10000LL
typedef struct { long long QuadPart; } LARGE_INTEGER;
struct horizon_user_apc { unsigned size; unsigned char call[64]; };
struct horizon_server_object {
    int type, manual_reset, signaled;
    unsigned refs, count, max;
    struct horizon_mutex_state mutex;
    struct horizon_thread_state thread;
    struct horizon_server_object *thread_next;
    struct horizon_sync_link *waiters;
    union { struct horizon_user_apc *apc; struct horizon_user_apc *apc_first; };
    long long timer_when;
    unsigned timer_period;
    struct horizon_server_object *timer_next;
    int system_apc;
    unsigned queue_tid;
    struct horizon_server_object *wait_port;
    struct horizon_completion_queue completion;
    struct horizon_completion_msg wait_msg;
    int completion_closed, wait_has_msg;
};
struct horizon_server_handle_entry { struct horizon_server_object *object; };
struct horizon_server_connection {
    int reply_fd;
    unsigned tid, pid;
    struct horizon_server_object *thread;
    void *direct_reply, *direct_data;
    size_t direct_size;
};
static struct horizon_server_object objects[8], threads[2];
static struct horizon_server_handle_entry handles[8];
static struct horizon_msgqs horizon_msg_queues;
static struct horizon_win_timers horizon_timers;
static struct horizon_server_object *horizon_server_threads = threads;
static __thread struct horizon_server_connection *horizon_server_current;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
#define horizon_server_objects_cond changed
static unsigned horizon_server_sleepers;
static unsigned wake_signals;
static int count_cond_signal(pthread_cond_t *cond) {
    wake_signals++;
    return pthread_cond_signal(cond);
}
#define pthread_cond_signal count_cond_signal
static int horizon_fast_sync_enabled = 1, pipe_replies;
typedef uint64_t u64;
volatile int wine_nx_quit_requested;
static long long clock_ticks(void) {
    struct timespec ts; assert(!clock_gettime(CLOCK_REALTIME, &ts));
    return (long long)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
}
static void NtQueryPerformanceCounter(LARGE_INTEGER *n, void *f) { (void)f; n->QuadPart = clock_ticks(); }
static void NtQuerySystemTime(LARGE_INTEGER *n) { n->QuadPart = clock_ticks(); }
static unsigned long long horizon_server_timer_clock(void) { return clock_ticks() / 10000; }
static unsigned condvarWaitTimeout(pthread_cond_t *cond, pthread_mutex_t *mutex, u64 timeout) {
    long long deadline = clock_ticks() + timeout / 100;
    struct timespec ts = { deadline / 10000000, (deadline % 10000000) * 100 };
    int ret = pthread_cond_timedwait(cond, mutex, &ts);
    assert(!ret || ret == ETIMEDOUT);
    return ret ? 0xea01 : 0;
}
static void horizon_server_signal_changed_locked(void);
static void horizon_sync_notify_object_locked(struct horizon_server_object *o, int satisfy);
static void horizon_server_free_object(struct horizon_server_object *o) { (void)o; assert(0); }
static int horizon_server_write_reply(int fd, const void *r, size_t s, const void *d, size_t ds) {
    (void)fd; (void)r; (void)s; (void)d; (void)ds; pipe_replies++; return 0;
}
static unsigned horizon_server_find_typed_object_locked(unsigned h, int type, struct horizon_server_object **out) {
    if (!h || h >= 8 || !handles[h].object) return HORIZON_STATUS_INVALID_HANDLE;
    if (objects[h].type != type) return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    *out = objects + h; return 0;
}
static struct horizon_server_object *horizon_server_find_handle_object_locked(unsigned h, int type) {
    (void)type;
    if (h == HORIZON_CURRENT_THREAD_HANDLE) return horizon_server_current->thread;
    return h && h < 8 ? objects + h : NULL;
}
static unsigned horizon_server_wait_object_locked(unsigned h, int consume) {
    struct horizon_server_object *o;
    unsigned tid = horizon_server_current->tid;
    if (!h || h >= 8) return HORIZON_STATUS_INVALID_HANDLE;
    o = objects + h;
    if (o->type == HORIZON_SERVER_OBJECT_MUTEX) {
        if (!horizon_mutex_signaled(&o->mutex, tid)) return HORIZON_STATUS_TIMEOUT;
        return consume && horizon_mutex_acquire(&o->mutex, tid) ? HORIZON_STATUS_ABANDONED_WAIT_0 : 0;
    }
    if (o->type == HORIZON_SERVER_OBJECT_SEMAPHORE) {
        if (!o->count) return HORIZON_STATUS_TIMEOUT;
        if (consume) o->count--;
        return 0;
    }
    if (o->type == HORIZON_SERVER_OBJECT_THREAD) return o->thread.terminated ? 0 : HORIZON_STATUS_TIMEOUT;
    if (o->type == HORIZON_SERVER_OBJECT_MSG_QUEUE) {
        struct horizon_msgq *q = horizon_msgq_find(&horizon_msg_queues, o->queue_tid);
        unsigned long long now = horizon_server_timer_clock();
        horizon_msgq_expire(&horizon_msg_queues, now);
        if (!q) return HORIZON_STATUS_TIMEOUT;
        horizon_msgq_update(q, horizon_win_timers_expired(&horizon_timers, q->tid, 0, 0, 0, now, 0) ? HORIZON_MSGQ_QS_TIMER : 0);
        return horizon_msgq_signaled(q) ? 0 : HORIZON_STATUS_TIMEOUT;
    }
    if (o->type == HORIZON_SERVER_OBJECT_COMPLETION_WAIT) {
        if (!horizon_completion_wait_signaled(&o->wait_port->completion, o->wait_port->completion_closed))
            return HORIZON_STATUS_TIMEOUT;
        return consume && horizon_completion_wait_satisfy(&o->wait_port->completion, o->wait_port->completion_closed,
            &o->wait_msg, &o->wait_has_msg) ? HORIZON_STATUS_ABANDONED_WAIT_0 : 0;
    }
    if (!o->signaled) return HORIZON_STATUS_TIMEOUT;
    if (consume && !o->manual_reset) o->signaled = 0;
    return 0;
}
static unsigned horizon_server_signal_object_locked(unsigned h) {
    if (!h || h >= 8) return HORIZON_STATUS_INVALID_HANDLE;
    objects[h].signaled = 1; horizon_sync_notify_object_locked(objects + h, 1); return 0;
}
static void horizon_server_quit_check_locked(void);
static void horizon_report_user_apc(const char *w, unsigned t, unsigned s, unsigned st) {
    (void)w; (void)t; (void)s; (void)st;
}
static struct horizon_user_apc *horizon_server_take_user_apc_locked(struct horizon_server_object *t) {
    struct horizon_user_apc *a = t->apc; t->apc = NULL; return a;
}
static void horizon_server_async_result_locked(const struct horizon_select_request *r, const unsigned char *d, unsigned s) {
    (void)r; (void)d; (void)s;
}
static unsigned horizon_server_async_apc_locked(struct horizon_server_connection *c, unsigned char *call) {
    if (!c->thread->system_apc) return 0;
    c->thread->system_apc = 0; memset(call, 0x5a, HORIZON_APC_CALL_SIZE); return 7;
}
static int horizon_server_handle_polls_locked(unsigned h) { return h == 6; }
static struct { void *head; } horizon_asyncs = { (void *)1 };
#define HORIZON_ASYNC_STALE 1
static long long horizon_async_now(void) { return clock_ticks(); }
static void *horizon_async_ready_for(const void *list, unsigned tid, long long now, long long stale) {
    (void)list; (void)now; (void)stale;
    for (unsigned i = 0; i < 2; i++) if (threads[i].thread.tid == tid && threads[i].system_apc) return threads + i;
    return NULL;
}
'''
fixture += ''.join(definition(name, True) for name in (
    'horizon_sync_link', 'horizon_sync_waiter'))
fixture += r'''
static struct horizon_sync_waiter *horizon_sync_waiters;
static pthread_key_t horizon_sync_thread_key;
static pthread_once_t horizon_sync_thread_once = PTHREAD_ONCE_INIT;
static int horizon_sync_thread_key_valid;
static __thread struct horizon_server_object *horizon_sync_thread;
static struct horizon_server_object *horizon_server_timers;
static unsigned quit_checks;
static void horizon_server_quit_check_locked(void) {
    if (!wine_nx_quit_requested) return;
    for (struct horizon_sync_waiter *w = horizon_sync_waiters; w; w = w->next)
        assert(w->connection != horizon_server_current);
    quit_checks++;
    if (wine_nx_quit_requested == 2) {
        pthread_mutex_unlock(&horizon_server_objects_mutex);
        pthread_exit(NULL);
    }
    wine_nx_quit_requested = 0;
}
'''
fixture += ''.join(definition(name) for name in (
    'horizon_server_sleep_locked',
    'horizon_server_unlink_timer_locked', 'horizon_server_update_timers_locked',
    'horizon_sync_notify_legacy_locked', 'horizon_sync_notify_locked', 'horizon_sync_notify_async_locked',
    'horizon_server_sync_lock',
    'horizon_server_signal_changed_locked', 'horizon_sync_release_thread',
    'horizon_sync_init_thread_key', 'horizon_sync_get_thread_locked'))
fixture = fixture.replace('&horizon_server_objects_cond.cond', '&horizon_server_objects_cond').replace(
    '&horizon_server_objects_mutex.normal', '&horizon_server_objects_mutex')
functions = "".join(definition("horizon_server_" + name) for name in (
    "sync_reply", "handle_event_op", "handle_query_event", "handle_release_mutex", "handle_query_mutex",
    "handle_release_semaphore", "handle_query_semaphore", "select_wait",
    "select_status", "select_polls_locked", "select_signals"))
functions += ''.join(definition('horizon_sync_' + name) for name in (
    'unqueue_locked', 'queue_locked', 'notify_object_locked', 'queue_timeout_locked', 'sleep_locked'))
functions += definition('horizon_server_handle_select') + definition('horizon_server_sync_call')
functions = functions.replace('&waiter->cond.cond', '&waiter->cond').replace('&horizon_server_objects_mutex.normal', '&horizon_server_objects_mutex')
tests = r'''
static unsigned call(unsigned tid, const void *req, const void *data, unsigned size, void *reply, void *extra) {
    assert(horizon_server_sync_call(tid, req, data, size, reply, extra));
    assert(!horizon_server_current);
    return ((struct horizon_server_reply_header *)reply)->error;
}
static unsigned event(unsigned tid, unsigned h, unsigned op) {
    struct horizon_event_op_request r = { .header.req = HORIZON_REQ_EVENT_OP, .handle = h, .op = op };
    unsigned char reply[64]; return call(tid, &r, NULL, 0, reply, NULL);
}
static unsigned wait_for(unsigned tid, unsigned h, long long timeout, int alertable) {
    struct horizon_select_request r = { .header = { .req = HORIZON_REQ_SELECT, .reply_size = 64 },
        .flags = alertable ? HORIZON_SELECT_ALERTABLE : 0, .timeout = timeout, .size = 8 };
    struct { unsigned char apc[HORIZON_APC_RESULT_SIZE]; struct horizon_select_wait_op op; } data = {0};
    unsigned char reply[64], extra[64];
    data.op.op = HORIZON_SELECT_WAIT; data.op.handles[0] = h;
    r.header.request_size = HORIZON_APC_RESULT_SIZE + 8;
    unsigned status = call(tid, &r, &data, HORIZON_APC_RESULT_SIZE + 8, reply, extra);
    if (status == HORIZON_STATUS_KERNEL_APC) assert(extra[0] == 0x5a);
    if (status == HORIZON_STATUS_USER_APC) assert(extra[0] == 0x3c);
    return status;
}
static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 3000; i++) {
        assert(!wait_for(8, 1, -(clock_ticks() + 10000000), 0));
        assert(!event(8, 2, HORIZON_SET_EVENT));
    }
    return NULL;
}
struct wait_job { unsigned handle, second, status; int all, alertable; long long timeout; };
static void *queued_wait(void *arg) {
    struct wait_job *job = arg;
    if (!job->second) job->status = wait_for(8, job->handle, job->timeout, job->alertable);
    else {
        struct horizon_select_request r = { .header = { .req = HORIZON_REQ_SELECT, .request_size = HORIZON_APC_RESULT_SIZE + 12 },
            .size = 12, .timeout = job->timeout };
        struct { char apc[HORIZON_APC_RESULT_SIZE]; unsigned op, handles[2]; } d = {0};
        unsigned char reply[64];
        d.op = job->all ? HORIZON_SELECT_WAIT_ALL : HORIZON_SELECT_WAIT;
        d.handles[0] = job->handle; d.handles[1] = job->second;
        job->status = call(8, &r, &d, sizeof(d), reply, NULL);
    }
    return NULL;
}
static void await_queue(unsigned count) {
    long long deadline = clock_ticks() + 10000000;
    for (;;) {
        pthread_mutex_lock(&horizon_server_objects_mutex);
        unsigned n = 0;
        for (struct horizon_sync_waiter *w = horizon_sync_waiters; w; w = w->next) n++;
        if (n == count) return;
        pthread_mutex_unlock(&horizon_server_objects_mutex);
        assert(clock_ticks() < deadline);
        usleep(100);
    }
}
static void set_locked(unsigned h) {
    objects[h].signaled = 1;
    horizon_sync_notify_object_locked(objects + h, 1);
}
static void queued_tests(void) {
    pthread_t a, b;
    struct wait_job first = { .handle = 1, .timeout = INT64_MAX }, second = first;
    objects[1].signaled = objects[2].signaled = 0;
    objects[1].manual_reset = 1;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    unsigned long long wakes = wake_signals;
    set_locked(2);
    assert(wake_signals == wakes && !horizon_sync_waiters->completed);
    set_locked(1);
    assert(horizon_sync_waiters->completed);
    objects[1].signaled = 0;
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !first.status);

    objects[1].manual_reset = 0;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1); pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_create(&b, NULL, queued_wait, &second));
    await_queue(2);
    struct horizon_sync_link *head = objects[1].waiters;
    set_locked(1);
    assert(head->waiter->completed && !head->next->waiter->completed && !objects[1].signaled);
    set_locked(1);
    assert(head->next->waiter->completed && !objects[1].signaled);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL) && !first.status && !second.status);

    objects[2].signaled = 0;
    first.second = 2; first.all = 1;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    set_locked(1);
    assert(!horizon_sync_waiters->completed && objects[1].signaled);
    set_locked(2);
    assert(horizon_sync_waiters->completed && !objects[1].signaled && !objects[2].signaled);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !first.status);

    first.second = 0; first.handle = second.handle = 4;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    assert(!pthread_create(&b, NULL, queued_wait, &second));
    await_queue(2);
    objects[4].count = 1; horizon_sync_notify_object_locked(objects + 4, 1);
    head = objects[4].waiters;
    assert(head->waiter->completed && !head->next->waiter->completed && !objects[4].count);
    objects[4].count = 1; horizon_sync_notify_object_locked(objects + 4, 1);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL) && !first.status && !second.status);

    first.handle = 3;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    assert(horizon_mutex_abandon(&objects[3].mutex, 4));
    horizon_sync_notify_object_locked(objects + 3, 1);
    assert(objects[3].mutex.owner == 8 && objects[3].mutex.count == 1);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && first.status == HORIZON_STATUS_ABANDONED_WAIT_0);

    first.handle = 1; first.alertable = 1;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    threads[1].apc = calloc(1, sizeof(*threads[1].apc));
    threads[1].apc->size = 64; threads[1].apc->call[0] = 0x3c;
    set_locked(1);
    assert(!horizon_sync_waiters->completed && objects[1].signaled);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && first.status == HORIZON_STATUS_USER_APC && objects[1].signaled);
    objects[1].signaled = 0;

    first.alertable = 0;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    horizon_sync_notify_async_locked();
    assert(!horizon_sync_waiters->notified);
    threads[1].system_apc = 1;
    horizon_sync_notify_async_locked();
    assert(horizon_sync_waiters->notified && !horizon_sync_waiters->completed);
    set_locked(1);
    assert(!horizon_sync_waiters->completed && objects[1].signaled);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && first.status == HORIZON_STATUS_KERNEL_APC && objects[1].signaled);
    objects[1].signaled = 0;

    first.alertable = 0; first.timeout = -(clock_ticks() + 200000);
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    assert(!pthread_join(a, NULL) && first.status == HORIZON_STATUS_TIMEOUT);
    assert(!horizon_sync_waiters && !objects[1].waiters && objects[1].refs == 1);

    first.handle = 5; first.timeout = INT64_MAX;
    objects[5].type = HORIZON_SERVER_OBJECT_TIMER;
    objects[5].timer_when = clock_ticks() + 10000000;
    horizon_server_timers = &objects[5];
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    long long start = clock_ticks();
    objects[5].timer_when = start + 200000;
    horizon_sync_notify_object_locked(objects + 5, 0);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !first.status);
    assert(clock_ticks() - start >= 200000 && clock_ticks() - start < 10000000);
    assert(!horizon_server_timers && !objects[5].signaled && objects[5].refs == 1);
    horizon_server_unlink_timer_locked(objects + 5);

    first.handle = 7;
    objects[7].type = HORIZON_SERVER_OBJECT_THREAD;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    objects[7].thread.terminated = 1;
    horizon_sync_notify_object_locked(objects + 7, 1);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !first.status && objects[7].refs == 1);

    first.handle = 1;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    wine_nx_quit_requested = 1;
    horizon_server_signal_changed_locked();
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    for (;;) {
        await_queue(1);
        if (quit_checks) break;
        pthread_mutex_unlock(&horizon_server_objects_mutex);
        usleep(100);
    }
    assert(!wine_nx_quit_requested && objects[1].refs == 2);
    set_locked(1);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !first.status && objects[1].refs == 1);

    first.status = 0xdead;
    assert(!pthread_create(&a, NULL, queued_wait, &first));
    await_queue(1);
    wine_nx_quit_requested = 2;
    horizon_server_signal_changed_locked();
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && first.status == 0xdead && !horizon_sync_waiters);
    assert(objects[1].refs == 1 && threads[1].refs == 1);
    wine_nx_quit_requested = 0;
    pthread_mutex_lock(&horizon_server_objects_mutex);
    horizon_server_signal_changed_locked();
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    puts("Queued waits: unrelated signals, set/reset, FIFO auto-reset, wait-all, semaphore counts, mutex ownership, APC precedence, timer rearming and timeout cleanup passed");
}
static void message_io_tests(void) {
    pthread_t a, b;
    struct wait_job job = { .handle = 6, .timeout = INT64_MAX }, other = job;
    int created;
    struct horizon_msgq *q = horizon_msgq_get(&horizon_msg_queues, 8, &created);
    assert(q && created);
    objects[6].type = HORIZON_SERVER_OBJECT_MSG_QUEUE;
    objects[6].queue_tid = 8;
    q->wake_mask = HORIZON_MSGQ_QS_POSTMESSAGE;
    assert(!pthread_create(&a, NULL, queued_wait, &job));
    await_queue(1);
    unsigned long long wakes = wake_signals;
    set_locked(2);
    assert(wake_signals == wakes);
    horizon_msgq_post_quit(q, 0);
    horizon_sync_notify_object_locked(objects + 6, 0);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !job.status);
    q->quit_message = 0;

    q->wake_mask = HORIZON_MSGQ_QS_TIMER;
    unsigned long long id;
    assert(!pthread_create(&a, NULL, queued_wait, &job));
    await_queue(1);
    unsigned long long start = horizon_server_timer_clock();
    assert(!horizon_win_timers_set(&horizon_timers, 8, 0, 0x113, 0, 20, 0, start, &id));
    horizon_sync_notify_object_locked(objects + 6, 0);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !job.status);
    assert(horizon_server_timer_clock() >= start + 20);
    horizon_win_timers_drop(&horizon_timers, 8, 0);

    struct horizon_msgq_result result = { .has_deadline = 1, .deadline = horizon_server_timer_clock() + 25 };
    q->wake_mask = HORIZON_MSGQ_QS_SMRESULT;
    q->send_results = &result;
    assert(horizon_sync_queue_timeout_locked(8, 2500000) <= 250000);
    result.replied = 1;
    assert(horizon_sync_queue_timeout_locked(8, 2500000) == 2500000);
    q->send_results = NULL;
    horizon_msgq_destroy(&horizon_msg_queues, q);

    objects[5].type = HORIZON_SERVER_OBJECT_COMPLETION;
    objects[6].type = objects[7].type = HORIZON_SERVER_OBJECT_COMPLETION_WAIT;
    objects[6].wait_port = objects[7].wait_port = objects + 5;
    other.handle = 7;
    assert(!pthread_create(&a, NULL, queued_wait, &job));
    await_queue(1); pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_create(&b, NULL, queued_wait, &other));
    await_queue(2);
    assert(objects[5].refs == 3 && objects[6].refs == 2 && objects[7].refs == 2);
    assert(horizon_completion_add(&objects[5].completion, 11, 22, 0, 33));
    horizon_sync_notify_object_locked(objects + 5, 1);
    assert(objects[6].wait_has_msg && !objects[7].wait_has_msg && objects[5].completion.depth == 0);
    objects[5].completion_closed = 1;
    horizon_sync_notify_object_locked(objects + 5, 1);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(!job.status && other.status == HORIZON_STATUS_ABANDONED_WAIT_0);
    assert(objects[6].wait_msg.ckey == 11 && objects[6].wait_msg.information == 33);
    for (unsigned i = 5; i < 8; i++) {
        assert(objects[i].refs == 1 && !objects[i].waiters);
        memset(&objects[i], 0, sizeof(objects[i])); objects[i].refs = 1;
    }
    objects[2].signaled = 0;
    puts("Message/I/O waits: unrelated signals, queue wakeups, timer rearm, send deadlines, completion consumption and port closure passed");
}

int main(void) {
    union { uint64_t align; unsigned char bytes[64]; } reply;
    threads[0].thread.tid = 4; threads[1].thread.tid = 8;
    for (unsigned i = 0; i < 8; i++) objects[i].refs = 1;
    for (unsigned i = 0; i < 8; i++) handles[i].object = &objects[i];
    threads[0].refs = threads[1].refs = 1; threads[0].thread_next = &threads[1];
    objects[1].type = objects[2].type = HORIZON_SERVER_OBJECT_EVENT;
    objects[3].type = HORIZON_SERVER_OBJECT_MUTEX;
    objects[4].type = HORIZON_SERVER_OBJECT_SEMAPHORE; objects[4].max = 2;
    struct horizon_query_event_request query = { .header.req = HORIZON_REQ_QUERY_EVENT, .handle = 1 };
    horizon_fast_sync_enabled = 0;
    assert(!horizon_server_sync_call(4, &query, NULL, 0, &reply, NULL));
    horizon_fast_sync_enabled = 1;
    assert(!horizon_server_sync_call(999, &query, NULL, 0, &reply, NULL));
    query.header.req = HORIZON_REQ_CREATE_EVENT;
    assert(!horizon_server_sync_call(4, &query, NULL, 0, &reply, NULL));
    query.header.req = HORIZON_REQ_QUERY_EVENT;
    threads[0].thread.terminated = 1;
    assert(!horizon_server_sync_call(4, &query, NULL, 0, &reply, NULL));
    assert(threads[0].refs == 1);
    threads[0].thread.terminated = 0;
    assert(!event(4, 1, HORIZON_SET_EVENT));
    assert(!call(4, &query, NULL, 0, &reply, NULL));
    assert(((struct horizon_query_event_reply *)&reply)->state == 1);
    assert(event(4, 1, -1) == HORIZON_STATUS_INVALID_PARAMETER);
    memcpy(&reply, &query, sizeof(query));
    assert(!call(4, &reply, NULL, 0, &reply, NULL));
    assert(((struct horizon_query_event_reply *)&reply)->state == 1);
    assert(!wait_for(4, 1, 0, 0));
    assert(wait_for(4, 1, 0, 0) == HORIZON_STATUS_TIMEOUT);
    assert(event(4, 7, HORIZON_SET_EVENT) == HORIZON_STATUS_OBJECT_TYPE_MISMATCH);
    assert(event(4, 9, HORIZON_SET_EVENT) == HORIZON_STATUS_INVALID_HANDLE);
    objects[1].manual_reset = 1;
    assert(!event(4, 1, HORIZON_SET_EVENT));
    assert(!wait_for(4, 1, 0, 0) && !wait_for(8, 1, 0, 0));
    assert(!event(4, 1, HORIZON_RESET_EVENT)); objects[1].manual_reset = 0;
    assert(!wait_for(4, 3, 0, 0) && !wait_for(4, 3, 0, 0));
    assert(wait_for(8, 3, 0, 0) == HORIZON_STATUS_TIMEOUT);
    struct horizon_release_mutex_request mutex = { .header.req = HORIZON_REQ_RELEASE_MUTEX, .handle = 3 };
    assert(call(8, &mutex, NULL, 0, &reply, NULL) == HORIZON_STATUS_MUTANT_NOT_OWNED);
    assert(!call(4, &mutex, NULL, 0, &reply, NULL));
    assert(!call(4, &mutex, NULL, 0, &reply, NULL));
    assert(!wait_for(8, 3, 0, 0));
    horizon_mutex_abandon(&objects[3].mutex, 8);
    assert(wait_for(4, 3, 0, 0) == HORIZON_STATUS_ABANDONED_WAIT_0);
    struct horizon_release_semaphore_request sem = { .header.req = HORIZON_REQ_RELEASE_SEMAPHORE, .handle = 4, .count = 2 };
    assert(!call(4, &sem, NULL, 0, &reply, NULL));
    assert(call(4, &sem, NULL, 0, &reply, NULL) == HORIZON_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    assert(!wait_for(4, 4, 0, 0) && !wait_for(8, 4, 0, 0));
    assert(wait_for(4, 4, 0, 0) == HORIZON_STATUS_TIMEOUT);
    threads[0].system_apc = 1;
    assert(wait_for(4, 1, 0, 0) == HORIZON_STATUS_KERNEL_APC);
    threads[0].apc = calloc(1, sizeof(*threads[0].apc));
    threads[0].apc->size = 64; threads[0].apc->call[0] = 0x3c;
    assert(wait_for(4, 1, 0, 0) == HORIZON_STATUS_TIMEOUT);
    assert(wait_for(4, 1, 0, 1) == HORIZON_STATUS_USER_APC);
    long long start = clock_ticks();
    assert(wait_for(4, 1, -(start + 200000), 0) == HORIZON_STATUS_TIMEOUT);
    assert(clock_ticks() - start >= 200000);
    struct horizon_select_request select = { .header = { .req = HORIZON_REQ_SELECT, .reply_size = 64 },
        .timeout = 0, .size = 12 };
    struct { unsigned char apc[HORIZON_APC_RESULT_SIZE]; unsigned op, handles[2]; } multi = {0};
    unsigned char extra[64];
    select.header.request_size = HORIZON_APC_RESULT_SIZE + select.size;
    multi.op = HORIZON_SELECT_WAIT_ALL; multi.handles[0] = 1; multi.handles[1] = 2;
    assert(!event(4, 1, HORIZON_SET_EVENT));
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == HORIZON_STATUS_TIMEOUT);
    assert(objects[1].signaled);
    assert(!event(4, 2, HORIZON_SET_EVENT));
    assert(!call(4, &select, &multi, select.header.request_size, &reply, extra));
    assert(!objects[1].signaled && !objects[2].signaled);
    multi.op = HORIZON_SELECT_WAIT;
    assert(!event(4, 2, HORIZON_SET_EVENT));
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == 1);
    multi.op = HORIZON_SELECT_SIGNAL_AND_WAIT; multi.handles[0] = 2; multi.handles[1] = 1;
    threads[0].system_apc = 1;
    select.timeout = -(clock_ticks() + 10000000);
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == HORIZON_STATUS_KERNEL_APC);
    assert(objects[1].signaled);
    assert(!event(4, 1, HORIZON_RESET_EVENT));
    select.flags = HORIZON_SELECT_ALERTABLE;
    threads[0].apc = calloc(1, sizeof(*threads[0].apc));
    threads[0].apc->size = 64;
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == HORIZON_STATUS_USER_APC);
    assert(objects[1].signaled);
    assert(!event(4, 1, HORIZON_RESET_EVENT));
    assert(!event(4, 2, HORIZON_SET_EVENT));
    threads[0].apc = calloc(1, sizeof(*threads[0].apc));
    threads[0].apc->size = 64;
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == HORIZON_STATUS_USER_APC);
    assert(objects[1].signaled && objects[2].signaled);
    assert(!event(4, 1, HORIZON_RESET_EVENT));
    select.flags = 0;
    assert(!event(4, 2, HORIZON_SET_EVENT));
    select.size = 8; select.header.request_size = HORIZON_APC_RESULT_SIZE + select.size;
    assert(!horizon_server_select_signals(&select, (const void *)&multi, select.header.request_size));
    multi.handles[0] = 6;
    assert(horizon_server_select_polls_locked(&select, (const void *)&multi, select.header.request_size));
    multi.handles[0] = 2;
    threads[0].system_apc = 1;
    assert(call(4, &select, &multi, select.header.request_size, &reply, extra) == HORIZON_STATUS_KERNEL_APC);
    assert(objects[2].signaled);
    assert(!call(4, &select, &multi, select.header.request_size, &reply, extra));
    assert(!objects[1].signaled && !objects[2].signaled);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker, NULL));
    for (int i = 0; i < 3000; i++) {
        assert(!event(4, 1, HORIZON_SET_EVENT));
        assert(!wait_for(4, 2, -(clock_ticks() + 10000000), 0));
    }
    assert(!pthread_join(thread, NULL));
    message_io_tests();
    queued_tests();
    assert(threads[0].refs == 2 && threads[1].refs == 1 && !pipe_replies);
    assert(!horizon_sync_waiters);
    for (unsigned i = 0; i < 8; i++) assert(objects[i].refs == 1 && !objects[i].waiters);
    struct horizon_server_connection standard = { .tid = 4, .thread = threads };
    horizon_server_current = &standard;
    assert(horizon_server_sync_call(4, &query, NULL, 0, &reply, NULL));
    assert(horizon_server_current == &standard);
    assert(!horizon_server_handle_query_event(&standard, (const unsigned char *)&query));
    assert(pipe_replies == 1);
    horizon_server_current = NULL;
    assert(!pthread_setspecific(horizon_sync_thread_key, NULL));
    horizon_sync_release_thread(horizon_sync_thread);
    assert(threads[0].refs == 1 && threads[1].refs == 1);
    puts("Horizon direct sync: dispatch, events, mutexes, semaphores, APCs, deadlines and 6000 cross-thread waits passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wine-nx-fast-sync-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(constants + "\n" + types + fixture + functions + tests)
    for sanitizer in ("address,undefined", "thread"):
        subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=" + sanitizer,
                        "-I" + str(root / "dlls/ntdll/unix"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)

source = (root / "dlls/ntdll/unix/server.c").read_text()
client = r'''
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct thread_data { union { unsigned tid; struct { unsigned UniqueThread; } ClientId; }; };
static struct thread_data thread_data = { .tid = 42 };
#define NtCurrentTeb() get_thread_data()
#define HandleToULong(value) (value)
enum { REQ_select, REQ_event_op, REQ_query_event, REQ_release_mutex, REQ_query_mutex,
       REQ_release_semaphore, REQ_query_semaphore, REQ_other };
union apc_result { uint64_t words[8]; };
struct request_header { unsigned req, request_size; };
struct __server_request_info {
    union {
        union {
            struct request_header request_header;
            struct { struct request_header header; unsigned size; } select_request;
        } req;
        unsigned reply;
    } u;
    struct { void *ptr; unsigned size; } data[4];
    unsigned data_count;
    void *reply_data;
};
static int reads, calls, quitting, quit_calls;
static int horizon_fast_sync_enabled = 1;
volatile int wine_nx_quit_requested;
static jmp_buf quit_jump;
void wine_nx_quit_point(void) __attribute__((weak));
void wine_nx_quit_point(void) { quit_calls++; if (quitting) longjmp(quit_jump, 1); }
static struct thread_data *get_thread_data(void) {
    assert(!quitting); reads++; return &thread_data;
}
static int horizon_server_sync_call(unsigned tid, const void *r, const void *d,
                                    unsigned size, void *reply, void *extra)
{
    const struct request_header *header = r;
    assert(tid == 42 && size == header->request_size);
    (void)d; (void)extra; *(unsigned *)reply = 0; calls++;
    return 1;
}
'''
client_tests = r'''
int main(void)
{
    struct __server_request_info req = { .u.req.request_header.req = REQ_event_op };
    assert(horizon_sync_call(&req) && reads == 1 && calls == 1);
    req.u.req.request_header.req = REQ_other;
    assert(!horizon_sync_call(&req) && reads == 1);
    req.u.req.request_header.req = REQ_select;
    assert(!horizon_sync_call(&req) && reads == 1);
    req.u.req.request_header.req = REQ_event_op;
    req.u.req.request_header.request_size = 1025;
    assert(!horizon_sync_call(&req) && reads == 1);
    req.u.req.request_header.request_size = 1;
    assert(!horizon_sync_call(&req) && reads == 1);
    req.u.req.request_header.request_size = 0;
    wine_nx_quit_requested = 1;
    quitting = 1;
    if (!setjmp(quit_jump)) { horizon_sync_call(&req); assert(0); }
    assert(quit_calls == 1 && reads == 1 && calls == 1);
    horizon_fast_sync_enabled = 0;
    assert(!horizon_sync_call(&req) && quit_calls == 1 && reads == 1);
    horizon_fast_sync_enabled = 1;
    quitting = 0;
    assert(horizon_sync_call(&req) && quit_calls == 2 && reads == 2);
    puts("Horizon direct sync client: teardown before TEB access, cancelled quit, mode and payload bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wine-nx-sync-client-") as tmp:
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(client + definition("horizon_sync_call") + client_tests)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
