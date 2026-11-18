#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import tempfile
from horizon_sync_fixture import legacy_sync_support

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def block(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


definitions = '\n'.join(re.findall(r'^#define HORIZON_(?:STATUS_|IMAGE_FILE_MACHINE_|APC_|SELECT_|SERVER_WAIT_SLICE|SERVER_POLL_INTERVAL)\w* .*', source, re.M))
structures = '\n'.join(block(source, 'struct horizon_' + name + '\n') + ';' for name in (
    'server_request_header', 'server_reply_header', 'select_request', 'select_reply',
    'resume_thread_request', 'resume_thread_reply', 'suspend_thread_request', 'suspend_thread_reply',
    'terminate_thread_request', 'terminate_thread_reply',
    'get_thread_context_request', 'get_thread_context_reply',
    'set_thread_context_request', 'set_thread_context_reply'))
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/server_protocol.h"
#include "horizon_threads.h"
#define TRACE(...) ((void)0)
static void horizon_trace(const char *fmt, ...) { (void)fmt; }
#define HORIZON_STOP_SUSPEND 1
#define HORIZON_STOP_END     2
#define HORIZON_STOP_KERNEL  4
#define HORIZON_STOP_HELD    8
#define HORIZON_STOP_LEAVING 16
#define R_FAILED(rc) ((rc) != 0)
typedef unsigned Handle;
typedef int Result;
enum { ThreadActivity_Paused, ThreadActivity_Runnable };
typedef struct {
    struct { uint64_t x; } pc;
    uint64_t sp, lr, fp, psr;
    struct { uint64_t x; } cpu_gprs[29];
    unsigned char fpu_gprs[32][16];
    uint32_t fpcr, fpsr;
} ThreadContext;
''' + definitions + '\n' + structures + r'''
struct horizon_server_object {
    unsigned refs;
    struct horizon_thread_state thread;
    struct context_data *thread_contexts;
    struct context_data *frozen_context;
    Handle frozen;
    struct context_data *held_contexts;
    unsigned int held_count;
    int held, held_changed;
    unsigned thread_context_count;
    int thread_context_valid;
    int ending;
};
struct horizon_server_connection { int reply_fd; unsigned tid; struct horizon_server_object *thread; void *direct_reply; };
struct horizon_user_apc { unsigned size; unsigned char call[HORIZON_APC_CALL_SIZE]; };
static struct horizon_server_object target = {.refs = 1};
/* A handler that waits for the thread holds it meanwhile; the handle's own
 * reference outlives every request here, so nothing may free the thread. */
static void horizon_server_free_object(struct horizon_server_object *object) { (void)object; assert(0); }
/* Carafe's event ring (stage 2) and the shared server threads (stage 6): the
 * handlers note what they did and tell the pool before they sleep. Here they
 * run on plain threads; a handler that says it blocks is counted. */
enum { CARAFE_DIAG_SUSPEND, CARAFE_DIAG_RESUME, CARAFE_DIAG_TERMINATE, CARAFE_DIAG_SUSPENDED_WAIT,
       CARAFE_DIAG_SUSPENDED_DONE };
static unsigned diag_events[5];
#define CARAFE_DIAG_EVENT( kind, actor, target, value ) \
    do { (void)(actor); (void)(target); (void)(value); \
         __atomic_add_fetch( &diag_events[kind], 1, __ATOMIC_RELAXED ); } while (0)
static __thread int horizon_server_pool_worker, horizon_server_pool_blocked;
static unsigned pool_blocking_calls;
static void horizon_server_pool_blocking_locked(void) {
    horizon_server_pool_blocked = 1;
    __atomic_add_fetch( &pool_blocking_calls, 1, __ATOMIC_RELAXED );
}
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct { char reply[64]; _Alignas(8) char data[4096]; unsigned size; } replies[2];
static void horizon_server_signal_changed_locked(void) { pthread_cond_broadcast(&changed); }
static void horizon_server_sleep_locked(long long timeout) {
    (void)timeout; assert(!pthread_cond_wait(&changed, &horizon_server_objects_mutex));
}
static void horizon_server_quit_check_locked(void) {}
static struct horizon_server_object *horizon_server_get_thread_locked(unsigned handle, unsigned *status) {
    *status = handle == 1 ? 0 : STATUS_INVALID_HANDLE;
    return handle == 1 ? &target : NULL;
}
static Handle kernel_thread_handle, next_kernel_thread_handle;
static int kernel_paused, kernel_in_runtime, kernel_ctx_fail, stop_mask, stop_exit_code, pause_granted = 1;
static uint64_t kernel_pc = 0x55550000ULL;
static const void *kernel_frame, *released_changed;
static int kernel_can_hold, kernel_state_bits;
static unsigned alert_calls, signal_calls, wait_change_calls;
static uint64_t kernel_frame_pc = 0x66660000ULL;
__attribute__((weak)) int wine_nx_thread_stop_request(const void *t, unsigned tid, int set, int clear, int code, Handle *h) {
    (void)t; (void)tid;
    if (!kernel_thread_handle) return 0;
    if (set & HORIZON_STOP_END) stop_exit_code = code;
    stop_mask = (stop_mask | set) & ~clear;
    if (h) *h = kernel_thread_handle;
    return 1;
}
__attribute__((weak)) int wine_nx_in_runtime_code(uint64_t addr) { (void)addr; return kernel_in_runtime; }
__attribute__((weak)) int wine_nx_thread_pause_granted(void) { return pause_granted; }
__attribute__((weak)) int wine_nx_thread_hold(const void *t, unsigned tid, const void **frame, int *changed) {
    (void)t; (void)tid;
    if (!kernel_can_hold || !kernel_frame) return 0;
    *frame = kernel_frame;
    *changed = 0;
    return 1;
}
__attribute__((weak)) void wine_nx_thread_release(const void *t, unsigned tid, const void *changed) {
    (void)t; (void)tid;
    released_changed = changed;
}
__attribute__((weak)) int wine_nx_thread_state(const void *t, unsigned tid, const void **frame) {
    (void)t; (void)tid;
    if (frame) *frame = kernel_frame;
    return kernel_state_bits;
}
__attribute__((weak)) void wine_nx_thread_wait_change(const void *t, unsigned tid, int observed) {
    (void)t; (void)tid; (void)observed;
    /* This wait is outside the server's lock: a shared server thread must
     * have told the pool by now, or the pool is one thread short meanwhile. */
    assert(!horizon_server_pool_worker || horizon_server_pool_blocked);
    wait_change_calls++;
    kernel_can_hold = 1;
    kernel_frame = (const void *)0xfeedULL;
}
__attribute__((weak)) void wine_nx_thread_signal(const void *t, unsigned tid) {
    (void)t; (void)tid;
    signal_calls++;
}
__attribute__((weak)) void wine_nx_kernel_context(const void *frame, CONTEXT *context) {
    ARM64_NT_CONTEXT *arm64 = (ARM64_NT_CONTEXT *)context;
    assert(frame == kernel_frame);
    memset(arm64, 0, sizeof(*arm64));
    arm64->ContextFlags = CONTEXT_ARM64_FULL | CONTEXT_ARM64_X18;
    arm64->Pc = kernel_frame_pc;
    arm64->Sp = 0x88880000ULL;
}
__attribute__((weak)) unsigned horizon_contexts_to_server(struct context_data server_contexts[2], CONTEXT *context) {
    const ARM64_NT_CONTEXT *arm64 = (const ARM64_NT_CONTEXT *)context;
    memset(server_contexts, 0, 2 * sizeof(*server_contexts));
    server_contexts[0].machine = HORIZON_IMAGE_FILE_MACHINE_ARM64;
    server_contexts[0].flags = SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_EXEC_SPACE;
    server_contexts[0].ctl.arm64_regs.pc = arm64->Pc;
    server_contexts[0].ctl.arm64_regs.sp = arm64->Sp;
    return 1;
}
NTSTATUS WINAPI NtAlertThreadByThreadId(HANDLE tid) {
    assert(tid == ULongToHandle(target.thread.tid));
    alert_calls++;
    return STATUS_SUCCESS;
}
static Result svcSetThreadActivity(Handle h, int activity) {
    if (activity == ThreadActivity_Paused) { assert(h == kernel_thread_handle && !kernel_paused); kernel_paused = 1; }
    else { assert(kernel_paused); kernel_paused = 0; }
    return 0;
}
static Result svcGetThreadContext3(ThreadContext *ctx, Handle h) {
    assert(h == kernel_thread_handle && kernel_paused);
    memset(ctx, 0, sizeof(*ctx));
    ctx->pc.x = kernel_pc;
    ctx->sp = 0x77770000ULL;
    if (next_kernel_thread_handle)
    {
        kernel_thread_handle = next_kernel_thread_handle;
        next_kernel_thread_handle = 0;
    }
    if (kernel_ctx_fail)
    {
        kernel_ctx_fail = 0;
        return 1;
    }
    return 0;
}
static int horizon_server_write_reply(int fd, const void *reply, size_t size, const void *data, size_t data_size) {
    assert(size <= sizeof(replies[fd].reply) && data_size <= sizeof(replies[fd].data));
    memcpy(replies[fd].reply, reply, size);
    if (data_size) memcpy(replies[fd].data, data, data_size);
    replies[fd].size = data_size;
    return 0;
}
static int horizon_server_sync_reply(struct horizon_server_connection *c, const void *r, size_t s, const void *d, size_t n) {
    return horizon_server_write_reply(c->reply_fd, r, s, d, n);
}
static void horizon_server_async_result_locked(const void *r, const void *d, unsigned s) { (void)r; (void)d; (void)s; }
static int horizon_server_select_polls_locked(const void *r, const void *d, unsigned s) { (void)r; (void)d; (void)s; return 0; }
static int horizon_server_select_signals(const void *r, const void *d, unsigned s) { (void)r; (void)d; (void)s; return 0; }
static unsigned horizon_server_wait_object_locked(unsigned h, int consume) { (void)h; (void)consume; assert(0); return 0; }
static unsigned horizon_server_signal_object_locked(unsigned h) { (void)h; assert(0); return 0; }
static unsigned horizon_server_async_apc_locked(void *c, void *d) { (void)c; (void)d; return 0; }
static struct horizon_user_apc *horizon_server_take_user_apc_locked(void *t) { (void)t; return NULL; }
static void horizon_report_user_apc(const char *m, unsigned tid, unsigned size, unsigned status) { (void)m; (void)tid; (void)size; (void)status; }
static void horizon_server_update_timers_locked(void) {}
static unsigned horizon_server_select_status(const void *r, const void *d, unsigned s) { (void)r; (void)d; (void)s; return STATUS_TIMEOUT; }
NTSTATUS WINAPI NtQueryPerformanceCounter(LARGE_INTEGER *n, LARGE_INTEGER *p) { (void)p; n->QuadPart = 0; return 0; }
NTSTATUS WINAPI NtQuerySystemTime(LARGE_INTEGER *n) { n->QuadPart = 0; return 0; }
'''
fixture += legacy_sync_support(source)
functions = '\n'.join(block(source, name) for name in (
    'static void horizon_server_set_suspend_doorbell_locked(',
    'static int horizon_server_has_doorbell_locked(',
    'static int horizon_server_stop_request_locked(',
    'static int horizon_server_can_pause(',
    'static void horizon_server_context_from_kernel(',
    'static int horizon_server_freeze_leaving_locked(',
    'static int horizon_server_freeze_locked(',
    'static void horizon_server_thaw_locked(',
    'static int horizon_server_hold_locked(',
    'static void horizon_server_release_locked(',
    'static void horizon_server_wait_thread_locked(',
    'static int horizon_server_handle_resume_thread(',
    'static int horizon_server_handle_suspend_thread(',
    'static void horizon_server_merge_context(',
    'static int horizon_server_handle_get_thread_context(',
    'static int horizon_server_handle_set_thread_context(',
    'static int horizon_server_handle_terminate_thread(',
    'static int horizon_server_handle_select('))
tests = r'''
static struct horizon_server_connection control = {0, 1}, client = {1, 2, &target};
static ULONG doorbell;
static CHPE_V2_CPU_AREA_INFO area;
static TEB teb;
static NTSTATUS reply_status(void) { return ((struct horizon_server_reply_header *)replies[0].reply)->error; }
static void *select_thread(void *arg) {
    unsigned count = (uintptr_t)arg;
    struct horizon_select_request request = {.header.reply_size = 4096};
    struct { char apc[HORIZON_APC_RESULT_SIZE]; struct context_data contexts[2]; } data = {0};
    data.contexts[0].machine = IMAGE_FILE_MACHINE_ARM64;
    data.contexts[0].flags = SERVER_CTX_CONTROL | SERVER_CTX_INTEGER;
    data.contexts[0].ctl.arm64_regs.pc = 0x12345678;
    data.contexts[0].ctl.arm64_regs.sp = 0x87650000;
    data.contexts[1].machine = IMAGE_FILE_MACHINE_I386;
    data.contexts[1].flags = SERVER_CTX_CONTROL;
    data.contexts[1].ctl.i386_regs.eip = 0x12340000;
    assert(!horizon_server_handle_select(&client, (void *)&request, (void *)&data,
                                        HORIZON_APC_RESULT_SIZE + count * sizeof(struct context_data)));
    return NULL;
}
static unsigned suspends, resumes;
static void suspend(void) {
    struct horizon_suspend_thread_request request = {.handle = 1};
    suspends++;
    assert(!horizon_server_handle_suspend_thread(&control, (void *)&request) && !reply_status());
}
static void resume(void) {
    struct horizon_resume_thread_request request = {.handle = 1};
    resumes++;
    assert(!horizon_server_handle_resume_thread(&control, (void *)&request) && !reply_status());
}
int main(void) {
    pthread_t thread;
    struct horizon_get_thread_context_request get = {.handle = 1, .machine = IMAGE_FILE_MACHINE_ARM64,
                                                    .flags = SERVER_CTX_CONTROL};
    struct horizon_set_thread_context_request set = {.handle = 1};
    struct context_data update = {.machine = IMAGE_FILE_MACHINE_ARM64, .flags = SERVER_CTX_CONTROL};
    struct context_data *returned;
    target.thread.started = 1;
    target.thread.teb = (uintptr_t)&teb;
    teb.ChpeV2CpuAreaInfo = &area;
    area.SuspendDoorbell = &doorbell;
    for (unsigned count = 1; count <= 2; ++count) {
        suspend(); suspend();
        assert(doorbell == 1 && !target.thread_context_valid);
        assert(!pthread_create(&thread, NULL, select_thread, (void *)(uintptr_t)count));
        pthread_mutex_lock(&horizon_server_objects_mutex);
        while (!target.thread_context_valid) pthread_cond_wait(&changed, &horizon_server_objects_mutex);
        pthread_mutex_unlock(&horizon_server_objects_mutex);
        assert(!horizon_server_handle_get_thread_context(&control, (void *)&get));
        assert(reply_status() == (count ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED));
        if (count) {
            returned = (void *)replies[0].data;
            assert(returned->ctl.arm64_regs.pc == 0x12345678);
            assert(returned->flags == SERVER_CTX_CONTROL);
            update.ctl.arm64_regs.pc = 0x22222222;
            assert(!horizon_server_handle_set_thread_context(&control, (void *)&set, (void *)&update, sizeof(update)));
            assert(!reply_status());
        }
        resume();
        assert(target.thread.suspend == 1 && doorbell && target.thread_context_valid);
        resume();
        assert(!doorbell && !pthread_join(thread, NULL));
        assert(!target.thread_contexts && !target.thread_context_valid && !target.thread_context_count);
        if (count) {
            assert(replies[1].size == HORIZON_APC_CALL_SIZE + count * sizeof(struct context_data));
            returned = (void *)(replies[1].data + HORIZON_APC_CALL_SIZE);
            assert(returned->ctl.arm64_regs.pc == 0x22222222);
            if (count == 2) assert(returned[1].ctl.i386_regs.eip == 0x12340000);
        }
    }
    suspend(); resume();
    select_thread((void *)1);
    assert(!target.thread_contexts && !target.thread_context_valid);

    /* Non-doorbell thread: freeze outside runtime code, resume, then freeze while leaving kernel. */
    teb.ChpeV2CpuAreaInfo = NULL;
    kernel_thread_handle = 42;
    kernel_in_runtime = 0;
    suspend();
    assert(kernel_paused && target.frozen == 42 && target.thread_context_valid);
    assert(!horizon_server_handle_get_thread_context(&control, (void *)&get) && !reply_status());
    returned = (void *)replies[0].data;
    assert(returned->ctl.arm64_regs.pc == kernel_pc);
    assert(!horizon_server_handle_set_thread_context(&control, (void *)&set, (void *)&update, sizeof(update)));
    assert(reply_status() == STATUS_NOT_SUPPORTED);
    resume();
    assert(!kernel_paused && !target.frozen && !target.thread_context_valid);

    /* Leaving kernel inside runtime code gives the saved frame context. */
    kernel_in_runtime = 1;
    kernel_frame = (const void *)0xdeadULL;
    kernel_state_bits = HORIZON_STOP_LEAVING;
    suspend();
    assert(kernel_paused && target.frozen == 42 && target.thread_context_valid);
    assert(!horizon_server_handle_get_thread_context(&control, (void *)&get) && !reply_status());
    returned = (void *)replies[0].data;
    assert(returned->ctl.arm64_regs.pc == kernel_frame_pc);
    resume();
    assert(!kernel_paused && !target.frozen && !target.thread_context_valid);
    kernel_state_bits = 0;

    /* Thread held in kernel: GetThreadContext waits via wait_thread_locked, SetThreadContext edits, Resume releases. */
    kernel_can_hold = 0;
    kernel_frame = NULL;
    suspend();
    assert(!kernel_paused && !target.frozen && !target.held && (stop_mask & HORIZON_STOP_SUSPEND));
    /* Asked by a shared server thread (stage 6): it waits for the thread
     * outside the lock, so it tells the pool first, once. */
    horizon_server_pool_worker = 1;
    assert(!horizon_server_handle_get_thread_context(&control, (void *)&get) && !reply_status());
    assert(wait_change_calls == 1 && target.held && target.thread_context_valid);
    assert(pool_blocking_calls == 1 && horizon_server_pool_blocked);
    horizon_server_pool_worker = horizon_server_pool_blocked = 0;
    returned = (void *)replies[0].data;
    assert(returned->ctl.arm64_regs.pc == kernel_frame_pc);
    update.ctl.arm64_regs.pc = 0x33333333;
    assert(!horizon_server_handle_set_thread_context(&control, (void *)&set, (void *)&update, sizeof(update)) && !reply_status());
    assert(target.held_changed);
    released_changed = NULL;
    resume();
    assert(!target.held && !target.held_changed && released_changed == target.held_contexts);
    assert(((const struct context_data *)released_changed)->ctl.arm64_regs.pc == 0x33333333);

    /* Held thread entering select with suspend_contexts overwrites held mode cleanly without zero reply. */
    kernel_can_hold = 1;
    suspend();
    assert(target.held && target.thread_context_valid);
    update.ctl.arm64_regs.pc = 0x44444444;
    assert(!horizon_server_handle_set_thread_context(&control, (void *)&set, (void *)&update, sizeof(update)) && !reply_status());
    assert(target.held_changed);
    assert(!pthread_create(&thread, NULL, select_thread, (void *)1));
    pthread_mutex_lock(&horizon_server_objects_mutex);
    while (target.held) pthread_cond_wait(&changed, &horizon_server_objects_mutex);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    resume();
    assert(!pthread_join(thread, NULL));
    assert(replies[1].size == HORIZON_APC_CALL_SIZE + sizeof(struct context_data));
    returned = (void *)(replies[1].data + HORIZON_APC_CALL_SIZE);
    assert(returned->ctl.arm64_regs.pc == 0x44444444);

    /* Held thread entering server select signals stop word even without doorbell contexts. */
    kernel_can_hold = 1;
    suspend();
    assert(target.held && target.thread_context_valid);
    update.ctl.arm64_regs.pc = 0x55555555;
    assert(!horizon_server_handle_set_thread_context(&control, (void *)&set, (void *)&update, sizeof(update)) && !reply_status());
    assert(target.held_changed);
    target.thread_context_valid = 0;
    signal_calls = 0;
    assert(!pthread_create(&thread, NULL, select_thread, (void *)0));
    pthread_mutex_lock(&horizon_server_objects_mutex);
    while (!target.thread_context_valid) pthread_cond_wait(&changed, &horizon_server_objects_mutex);
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    assert(signal_calls == 1);

    /* Terminate releases held/frozen state without forwarding pending SetThreadContext jumps, clears suspend, sets END, and alerts tid. */
    {
        struct horizon_terminate_thread_request term = {.handle = 1, .exit_code = 77};
        released_changed = (const void *)0x1;
        assert(!horizon_server_handle_terminate_thread(&control, (void *)&term) && !reply_status());
        assert(alert_calls == 1 && !target.held && !target.held_changed && !released_changed && !kernel_paused);
        assert(target.ending && !target.thread.suspend && (stop_mask & HORIZON_STOP_END));
        assert(!(stop_mask & HORIZON_STOP_SUSPEND) && stop_exit_code == 77);
        assert(target.thread.exit_code == 77);
        assert(!pthread_join(thread, NULL));
        assert(((struct horizon_server_reply_header *)replies[1].reply)->error == HORIZON_STATUS_THREAD_IS_TERMINATING);
    }
    /* Carafe's event ring (stage 2) heard of every request, and of each
     * select that sat out a suspension and came back. */
    assert(diag_events[CARAFE_DIAG_SUSPEND] == suspends && diag_events[CARAFE_DIAG_RESUME] == resumes);
    assert(suspends == 10 && resumes == 9 && diag_events[CARAFE_DIAG_TERMINATE] == 1);
    assert(diag_events[CARAFE_DIAG_SUSPENDED_WAIT] == diag_events[CARAFE_DIAG_SUSPENDED_DONE]);
    assert(diag_events[CARAFE_DIAG_SUSPENDED_WAIT] == 4 && pool_blocking_calls == 1 && target.refs == 1);
    free(target.frozen_context);
    free(target.held_contexts);
    puts("Horizon suspend: safe-point contexts, nested suspend/resume, kernel hold/leaving and terminate passed");
}
'''

server = (root / 'dlls/ntdll/unix/server.c').read_text()
continuation = block(server, 'NTSTATUS WINAPI NtContinueEx(')
guard = continuation[continuation.index('#ifdef __SWITCH__'):continuation.index('#endif') + len('#endif')]
resume_test = r'''
#include <assert.h>
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#ifndef __aarch64__
#undef CONTEXT
#define CONTEXT ARM64_NT_CONTEXT
#endif
#define __SWITCH__
static TEB teb;
#undef NtCurrentTeb
#define NtCurrentTeb() (&teb)
static int horizon_suspend_pending, waits;
static BOOL is_arm64ec(void) { return TRUE; }
static void wait_suspend(CONTEXT *context) {
    assert(!horizon_suspend_pending);
    assert(context->Pc == 0x12345678);
    context->Pc = 0x22345678;
    waits++;
}
static void resume_context(CONTEXT *context) {
''' + guard + r'''
}
int main(void) {
    CHPE_V2_CPU_AREA_INFO area = {0};
    CONTEXT context = {.ContextFlags = CONTEXT_ARM64_FULL, .Pc = 0x12345678};
    teb.ChpeV2CpuAreaInfo = &area;
    resume_context(&context);
    assert(!waits);
    horizon_suspend_pending = 1;
    area.InSimulation = 1;
    resume_context(&context);
    assert(!waits && horizon_suspend_pending);
    area.InSimulation = 0;
    area.InSyscallCallback = 1;
    resume_context(&context);
    assert(!waits && horizon_suspend_pending);
    area.InSyscallCallback = 0;
    resume_context(&context);
    assert(waits == 1 && !horizon_suspend_pending && context.Pc == 0x22345678);
    resume_context(&context);
    assert(waits == 1);
}
'''

profile = (root / 'horizon-wine/source/thread_profile.c').read_text()
report_test = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#define NX_PROF_MAX_THREADS 2
#define NX_PROF_DEPTH 8
#define NX_PROF_LINE 1000
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
typedef unsigned Handle;
typedef int Result;
typedef struct { struct { uint64_t x; } pc; uint64_t sp, lr; } ThreadContext;
enum { ThreadActivity_Paused, ThreadActivity_Runnable };
static struct { Handle handle; unsigned tid; char kind; uint64_t teb; } registry[] = {{1, 4, 'w', 0}, {2, 8, 's', 0}};
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned names, logs, paused;
static int failure;
static Handle threadGetCurHandle(void) { return 99; }
static unsigned wine_nx_threads_other(void) { return 2; }
static Result svcSetThreadActivity(Handle handle, int activity) {
    (void)handle;
    if (activity == ThreadActivity_Paused) {
        if (failure == 1) return 1;
        assert(!paused); paused = 1;
    } else { assert(paused); paused = 0; }
    return 0;
}
static Result svcGetThreadContext3(ThreadContext *ctx, Handle handle) {
    (void)handle;
    assert(paused); *ctx = (ThreadContext){{0x12340000}, 0x20000, 0x12340000};
    return failure == 2;
}
static void svcSleepThread(unsigned long ns) { (void)ns; }
static unsigned walk_callers(const ThreadContext *ctx, uint64_t *callers) {
    assert(paused); callers[0] = ctx->lr; return 1;
}
static int x86_registers(uint64_t teb, uint32_t *eip, uint32_t *esp, uint32_t *ebp) {
    (void)teb; (void)eip; (void)esp; (void)ebp;
    assert(paused);
    return 0;
}
static unsigned walk_x86(uint64_t teb, uint32_t *callers) {
    (void)teb; (void)callers;
    assert(paused);
    return 0;
}
/* Carafe's native callers (stage 2): the stack is copied while the thread is
 * paused, and looked through for return addresses, which asks for images
 * and so may take locks, only after the thread runs again. */
#define CARAFE_SCAN_WORDS 4
static unsigned copies, picks;
static unsigned copy_stack(const ThreadContext *ctx, uint64_t *words) {
    assert(paused); words[0] = ctx->lr + 4; copies++;
    return 1;
}
static unsigned pick_calls(const uint64_t *words, unsigned count, uint64_t *callers) {
    assert(!paused && !pthread_mutex_trylock(&profile_mutex));
    pthread_mutex_unlock(&profile_mutex);
    picks++;
    if (count) { assert(count == 1 && words[0] == 0x12340004); callers[0] = words[0]; }
    return count;
}
static void name_address(uint64_t address, char *buf, size_t size) {
    assert(!paused && !pthread_mutex_trylock(&profile_mutex));
    pthread_mutex_unlock(&profile_mutex);
    snprintf(buf, size, "%lx", (unsigned long)address); names++;
}
static int appendf(char *line, int len, const char *fmt, ...) { (void)line; (void)fmt; return len; }
static void wine_nx_runtime_trace(const char *line) {
    (void)line;
    assert(!paused && !pthread_mutex_trylock(&profile_mutex));
    pthread_mutex_unlock(&profile_mutex); logs++;
}
''' + block(profile, 'void wine_nx_threads_report_stalled(') + r'''
int main(void) {
    for (failure = 0; failure < 3; ++failure) {
        wine_nx_threads_report_stalled();
        assert(!paused && !pthread_mutex_trylock(&profile_mutex));
        pthread_mutex_unlock(&profile_mutex);
    }
    /* Two threads read once each: where it is and one caller, and for the
     * Windows thread the one native caller its copied stack holds. */
    assert(names == 5 && logs == 5 && copies == 1 && picks == 2);
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-suspend-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + functions + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11', '-g', '-O1',
                    '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread', '-D__WINESRC__', '-D_WIN64',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
    for name, text in (('continue', resume_test), ('report', report_test)):
        (path / f'{name}.c').write_text(text)
        subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11', '-g', '-O1',
                        '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                        '-D__WINESRC__', '-D_WIN64', '-I' + str(root / 'include'),
                        str(path / f'{name}.c'), '-o', str(path / name)], check=True)
        subprocess.run([str(path / name)], check=True, timeout=30)
    print('FEX continuation and stall diagnostic lock boundaries passed')
