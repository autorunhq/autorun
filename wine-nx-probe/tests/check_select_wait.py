#!/usr/bin/env python3
"""Exercise the production wait loop and wait-any/all selection on the host."""
from pathlib import Path
import subprocess
import tempfile
from horizon_sync_fixture import legacy_sync_support
source = (Path(__file__).resolve().parents[2] / 'dlls/ntdll/unix/horizon.c').read_text()
def extract(start, end):
    return source[source.index(start):source.index(end, source.index(start))]
fixture = r'''
#define HORIZON_STANDALONE_SYNTAX
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#define HORIZON_STATUS_SUCCESS 0
#define HORIZON_STATUS_TIMEOUT 0x102
#define HORIZON_STATUS_INVALID_PARAMETER 0xc000000du
#define HORIZON_STATUS_ABANDONED_WAIT_0 0x80u
#define HORIZON_SERVER_WAIT_SLICE 200000LL
#define HORIZON_SERVER_POLL_INTERVAL 10000LL
#define HORIZON_STATUS_USER_APC 0xc0
#define HORIZON_STATUS_KERNEL_APC 0x100
#define HORIZON_SELECT_ALERTABLE 1
#define HORIZON_APC_CALL_SIZE 64
#define HORIZON_APC_RESULT_SIZE 64
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define min(a, b) ((a) < (b) ? (a) : (b))
typedef struct { long long QuadPart; } LARGE_INTEGER;
struct horizon_server_object { struct { unsigned tid; } thread; };
struct horizon_server_connection { int reply_fd; unsigned tid; struct horizon_server_object *thread; void *direct_reply; };
struct horizon_select_request { struct { unsigned reply_size; } header; int flags; long long timeout;
                                unsigned size; unsigned prev_apc; };
struct horizon_select_reply { struct { unsigned error, reply_size; } header; unsigned apc_handle; unsigned signaled; };
struct horizon_select_wait_op { int op; unsigned handles[4]; };

static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned states[4], consumed[4], abandoned[4];
static unsigned horizon_server_wait_object_locked(unsigned h, int consume) {
    if (states[h]) return states[h];
    if (consume) {
        consumed[h]++; states[h] = HORIZON_STATUS_TIMEOUT;
        if (abandoned[h]) { abandoned[h] = 0; return HORIZON_STATUS_ABANDONED_WAIT_0; }
    }
    return 0;
}
static void assert_locked(void) { assert(pthread_mutex_trylock(&horizon_server_objects_mutex) == EBUSY); }
struct horizon_user_apc { unsigned size; unsigned char call[64]; };
static struct horizon_user_apc *horizon_server_take_user_apc_locked(struct horizon_server_object *t) {
    (void)t; return NULL; }
static void horizon_report_user_apc(const char *w, unsigned t, unsigned s, unsigned st) {
    (void)w; (void)t; (void)s; (void)st; }
static void horizon_server_quit_check_locked(void) {}
/* A socket operation ready for the thread comes back as a system APC, and its
 * result with the next select (check_horizon_async_sockets.py runs both). */
static unsigned async_ready_on, async_given, results_taken, signal_and_wait;
static void horizon_server_async_result_locked(const struct horizon_select_request *r,
 const unsigned char *d, unsigned n) { (void)d; (void)n; assert_locked(); results_taken += !!r->prev_apc; }
static int horizon_server_select_signals(const struct horizon_select_request *r,
 const unsigned char *d, unsigned n) { (void)r; (void)d; (void)n; return signal_and_wait; }
static unsigned horizon_server_async_apc_locked(struct horizon_server_connection *c, unsigned char *call);
/* Timers are check_waitable_timer.py's business; the loop only looks at them. */
static unsigned timer_passes;
static void horizon_server_update_timers_locked(void) { assert_locked(); timer_passes++; }
static long long ticks, first_timeout, last_timeout;
static unsigned calls, ready_after, signals, sleeps, reply_status;
static unsigned horizon_server_signal_object_locked(unsigned handle) { (void)handle; signals++; return 0; }
static int queue_wait;
static unsigned horizon_server_select_status(const struct horizon_select_request *r,
 const unsigned char *d, unsigned n) {
    (void)r; (void)d; (void)n; assert_locked();
    return ++calls >= ready_after ? 0 : HORIZON_STATUS_TIMEOUT;
}
static int horizon_server_select_polls_locked(const struct horizon_select_request *r,
 const unsigned char *d, unsigned n) {
    (void)r; (void)d; (void)n; assert_locked(); return queue_wait;
}
/* The sleeper is woken by an object change a millisecond later at most. */
static void horizon_server_sleep_locked(long long timeout) {
    assert_locked(); assert(timeout > 0);
    if (timeout > HORIZON_SERVER_WAIT_SLICE) timeout = HORIZON_SERVER_WAIT_SLICE;
    if (!sleeps++) first_timeout = timeout;
    last_timeout = timeout;
    ticks += timeout < 10000 ? timeout : 10000;
}
static void NtQueryPerformanceCounter(LARGE_INTEGER *now, void *freq) { (void)freq; now->QuadPart = ticks; }
static void NtQuerySystemTime(LARGE_INTEGER *now) { now->QuadPart = ticks; }
static unsigned horizon_server_async_apc_locked(struct horizon_server_connection *c, unsigned char *call) {
    (void)c; assert_locked();
    if (!async_ready_on || calls + 1 < async_ready_on) return 0;
    async_ready_on = 0; memset(call, 0, HORIZON_APC_CALL_SIZE); call[0] = 2; return ++async_given;
}
static int horizon_server_sync_reply(struct horizon_server_connection *connection, const void *data, unsigned size, const void *extra, unsigned n) {
    (void)connection; (void)size; (void)extra; (void)n;
    assert(pthread_mutex_trylock(&horizon_server_objects_mutex) == 0); /* Replies go out unlocked. */
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    reply_status = ((const struct horizon_select_reply *)data)->header.error; return 0;
}
'''
fixture += legacy_sync_support(source)
tests = r'''
static void run(long long timeout, unsigned ready, unsigned expected, unsigned attempts) {
    struct horizon_select_request r = { .header = { 64 }, .timeout = timeout, .size = 8 };
    struct horizon_server_object t = { { 4 } };
    struct horizon_server_connection c = { .reply_fd = 1, .tid = 4, .thread = &t };
    ticks = 100000; calls = signals = sleeps = 0; ready_after = ready;
    timer_passes = 0;
    horizon_server_handle_select(&c, (const unsigned char *)&r, NULL, 0);
    assert(reply_status == expected && calls == attempts && !signals && sleeps == attempts - 1);
    assert(timer_passes == attempts); /* every attempt sees the timers that came due */
}
int main(void) {
    run(0, 2, 0x102, 1); /* Zero timeout does not block. */
    run(-120000, 100, 0x102, 3); /* Server monotonic deadline. */
    assert(first_timeout == 20000 && last_timeout == 10000); /* Sleeps end at the deadline. */
    run(120000, 100, 0x102, 3); /* NT absolute wall deadline. */
    assert(first_timeout == 20000 && last_timeout == 10000);
    run(-200000, 3, 0, 3); /* Signal arrives before deadline. */
    run(0x7fffffffffffffffLL, 5, 0, 5); /* Infinite waits remain pending, */
    assert(first_timeout == HORIZON_SERVER_WAIT_SLICE); /* sleeping until an object changes. */
    queue_wait = 1;
    run(0x7fffffffffffffffLL, 3, 0, 3); /* Message queue waits recheck every millisecond. */
    assert(first_timeout == HORIZON_SERVER_POLL_INTERVAL && last_timeout == HORIZON_SERVER_POLL_INTERVAL);
    queue_wait = 0;
    struct horizon_select_wait_op op = { 0, { 0, 1 } };
    unsigned size = offsetof(struct horizon_select_wait_op, handles[2]);
    states[0] = 0x102; states[1] = 0;
    assert(horizon_server_select_wait(&op, size, 0) == 1 && consumed[1] == 1);
    states[0] = 0; states[1] = 0x102;
    assert(horizon_server_select_wait(&op, size, 1) == 0x102 && consumed[0] == 0);
    states[1] = 0;
    assert(horizon_server_select_wait(&op, size, 1) == 0 && consumed[0] == 1 && consumed[1] == 2);
    /* An abandoned mutex keeps its index for wait-any and taints wait-all. */
    states[0] = 0x102; states[1] = 0; abandoned[1] = 1;
    assert(horizon_server_select_wait(&op, size, 0) == 0x81 && consumed[1] == 3);
    states[0] = states[1] = 0; abandoned[0] = 1;
    assert(horizon_server_select_wait(&op, size, 1) == 0x80 && consumed[0] == 2 && consumed[1] == 4);
    /* A socket operation that becomes ready interrupts an infinite wait, */
    async_ready_on = 3;
    ticks = 100000; calls = signals = sleeps = timer_passes = 0; ready_after = 100;
    {
        struct horizon_select_request r = { .header = { 64 }, .timeout = 0x7fffffffffffffffLL, .size = 8 };
        struct horizon_server_object t = { { 4 } };
        struct horizon_server_connection c = { .reply_fd = 1, .tid = 4, .thread = &t };
        horizon_server_handle_select(&c, (const unsigned char *)&r, NULL, 0);
        assert(reply_status == HORIZON_STATUS_KERNEL_APC && async_given == 1 && calls == 2);
        /* and its result comes back with the next select, taken before anything else. */
        r.prev_apc = 1; ready_after = 1; calls = 0;
        horizon_server_handle_select(&c, (const unsigned char *)&r, NULL, 0);
        assert(results_taken == 1 && reply_status == 0);
        /* A signal-and-wait signals before an APC can interrupt it. */
        signal_and_wait = 1; async_ready_on = 1; calls = signals = 0; ready_after = 100; r.prev_apc = 0;
        struct horizon_select_signal_and_wait_op sw = { .wait = 0, .signal = 1 };
        r.size = sizeof(sw);
        horizon_server_handle_select(&c, (const unsigned char *)&r, (const void *)&sw, sizeof(sw));
        assert(reply_status == HORIZON_STATUS_KERNEL_APC && signals == 1 && !calls && async_given == 2);
    }
    puts("Horizon waits: deadlines, infinite pending, queue polling, signal once, wait-any index, atomic wait-all, abandoned, system APCs passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-wait-test-') as tmp:
    c = Path(tmp) / 'test.c'; exe = Path(tmp) / 'test'
    c.write_text(fixture + extract('static unsigned int horizon_server_select_wait(', 'static unsigned int horizon_server_select_status(') + extract('static int horizon_server_handle_select(', 'int horizon_server_sync_call(') + tests)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
