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
''' + definitions + '\n' + structures + r'''
struct horizon_server_object {
    struct horizon_thread_state thread;
    struct context_data *thread_contexts;
    unsigned thread_context_count;
    int thread_context_valid;
};
struct horizon_server_connection { int reply_fd; struct horizon_server_object *thread; void *direct_reply; };
struct horizon_user_apc { unsigned size; unsigned char call[HORIZON_APC_CALL_SIZE]; };
static struct horizon_server_object target;
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
    'static int horizon_server_handle_resume_thread(',
    'static int horizon_server_handle_suspend_thread(',
    'static void horizon_server_merge_context(',
    'static int horizon_server_handle_get_thread_context(',
    'static int horizon_server_handle_set_thread_context(',
    'static int horizon_server_handle_select('))
tests = r'''
static struct horizon_server_connection control = {0}, client = {1, &target};
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
static void suspend(void) {
    struct horizon_suspend_thread_request request = {.handle = 1};
    assert(!horizon_server_handle_suspend_thread(&control, (void *)&request) && !reply_status());
}
static void resume(void) {
    struct horizon_resume_thread_request request = {.handle = 1};
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
    for (unsigned count = 0; count <= 2; ++count) {
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
    puts("Horizon suspend: safe-point contexts, nested suspend/resume and unavailable contexts passed");
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

profile = (root / 'wine-nx-probe/source/thread_profile.c').read_text()
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
static struct { Handle handle; unsigned tid; char kind; } registry[] = {{1, 4, 'w'}, {2, 8, 's'}};
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
    assert(names == 4 && logs == 5);
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
