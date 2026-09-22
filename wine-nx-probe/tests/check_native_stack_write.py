#!/usr/bin/env python3
"""Check writes to libnx stacks without weakening Wine view protections."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def extract(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#define VPROT_READ 1
#define VPROT_WRITE 2
#define VPROT_WRITEWATCH 4
#define ROUND_ADDR(addr,mask) ((void *)((uintptr_t)(addr) & ~(uintptr_t)(mask)))
#define ROUND_SIZE(addr,size,mask) (((size) + ((uintptr_t)(addr) & (mask)) + (mask)) & ~(size_t)(mask))
typedef struct { void *stack_mirror; size_t stack_sz; } Thread;
static Thread native_thread, *current_thread = &native_thread;
static Thread *threadGetSelf(void) { return current_thread; }
static const size_t host_page_mask = 4095, host_page_size = 4096;
static pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t view_base;
static BYTE pages[2];
static unsigned probes, watch_enables, watch_updates;
static BYTE get_host_page_vprot(const void *addr) {
    uintptr_t offset = (uintptr_t)addr - view_base;
    probes++;
    return offset < 8192 ? pages[offset / 4096] : 0;
}
static int get_unix_prot(BYTE prot) {
    if (prot & VPROT_WRITEWATCH) return PROT_READ;
    return (prot & VPROT_READ ? PROT_READ : 0) | (prot & VPROT_WRITE ? PROT_WRITE : 0);
}
static void mprotect_range(void *addr, size_t size, BYTE set, BYTE clear) {
    assert((uintptr_t)addr >= view_base && size && !set && clear == VPROT_WRITEWATCH);
    watch_enables++;
}
static void update_write_watches(void *addr, size_t size, size_t written) {
    assert((uintptr_t)addr >= view_base && written == size);
    watch_updates++;
}
static void server_enter_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *signals) {
    (void)signals; pthread_mutex_lock(mutex);
}
static void server_leave_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *signals) {
    (void)signals; pthread_mutex_unlock(mutex);
}
'''
functions = extract('dlls/ntdll/unix/horizon.c', 'BOOL horizon_is_native_stack_range(')
functions += extract('dlls/ntdll/unix/virtual.c', 'static NTSTATUS check_write_access(')
functions += extract('dlls/ntdll/unix/virtual.c', 'NTSTATUS virtual_uninterrupted_write_memory(')
dispatcher_fixture = r'''
static CONTEXT continued;
static TEB teb;
#define NtCurrentTeb() (&teb)
static void *pKiUserExceptionDispatcher = (void *)0x100000;
static NTSTATUS signal_set_full_context(CONTEXT *context) {
    continued = *context;
    return STATUS_UNSUCCESSFUL;
}
NTSTATUS WINAPI NtWriteVirtualMemory(HANDLE process, void *addr, const void *buffer, SIZE_T size, SIZE_T *written) {
    NTSTATUS status;
    assert(process == NtCurrentProcess());
    status = virtual_uninterrupted_write_memory(addr, buffer, size);
    *written = status ? 0 : size;
    return status;
}
'''
dispatcher = extract('dlls/ntdll/unix/signal_arm64.c', 'NTSTATUS call_user_exception_dispatcher(')
tests = r'''
int main(void) {
    unsigned char *stack = aligned_alloc(4096, 8192);
    unsigned char *view = aligned_alloc(4096, 8192);
    unsigned char frame[0x470];
    EXCEPTION_RECORD exception = {0};
    CONTEXT context = {0};
    BOOL watch = FALSE;
    assert(stack && view);
    memset(stack, 0xcc, 8192);
    memset(view, 0xcc, 8192);
    memset(frame, 0x5a, sizeof(frame));
    view_base = (uintptr_t)view;
    native_thread.stack_mirror = stack;
    native_thread.stack_sz = 8192;
    assert(horizon_is_native_stack_range(stack, 8192));
    assert(horizon_is_native_stack_range(stack + 8191, 1));
    assert(!horizon_is_native_stack_range(stack + 8191, 2));
    assert(!horizon_is_native_stack_range((void *)((uintptr_t)stack - 1), 1));
    assert(!horizon_is_native_stack_range(stack + 8192, 1));
    assert(!horizon_is_native_stack_range(stack, SIZE_MAX));
    assert(!horizon_is_native_stack_range(NULL, 1));
    assert(!horizon_is_native_stack_range(view, sizeof(frame)));
    assert(!virtual_uninterrupted_write_memory(stack + 8192 - sizeof(frame), frame, sizeof(frame)));
    assert(!memcmp(stack + 8192 - sizeof(frame), frame, sizeof(frame)) && !probes);
    assert(stack[8192 - sizeof(frame) - 1] == 0xcc);
    assert(!virtual_uninterrupted_write_memory(stack + 4000, frame, sizeof(frame)) && !probes);
    assert(check_write_access(stack + 8191, 2, &watch) == STATUS_INVALID_USER_BUFFER);
    current_thread = NULL;
    assert(!horizon_is_native_stack_range(stack, 1));
    assert(virtual_uninterrupted_write_memory(stack, frame, sizeof(frame)) == STATUS_INVALID_USER_BUFFER);
    current_thread = &native_thread;
    native_thread.stack_mirror = NULL;
    assert(!horizon_is_native_stack_range(NULL, 1));
    native_thread.stack_mirror = (void *)(UINTPTR_MAX - 4095);
    assert(!horizon_is_native_stack_range(native_thread.stack_mirror, 1));
    native_thread.stack_mirror = stack;
    native_thread.stack_sz = 0;
    assert(!horizon_is_native_stack_range(stack, 1));
    native_thread.stack_sz = 8192;
    assert(virtual_uninterrupted_write_memory(view, frame, sizeof(frame)) == STATUS_INVALID_USER_BUFFER);
    assert(view[0] == 0xcc);
    pages[0] = VPROT_READ;
    assert(virtual_uninterrupted_write_memory(view, frame, sizeof(frame)) == STATUS_INVALID_USER_BUFFER);
    pages[0] |= VPROT_WRITE;
    assert(!virtual_uninterrupted_write_memory(view, frame, sizeof(frame)));
    assert(!memcmp(view, frame, sizeof(frame)));
    assert(virtual_uninterrupted_write_memory(view + 4000, frame, sizeof(frame)) == STATUS_INVALID_USER_BUFFER);
    assert(view[4000] == 0xcc);
    pages[1] = VPROT_READ | VPROT_WRITE;
    assert(!virtual_uninterrupted_write_memory(view + 4000, frame, sizeof(frame)));
    pages[0] |= VPROT_WRITEWATCH;
    assert(!virtual_uninterrupted_write_memory(view, frame, sizeof(frame)));
    assert(watch_enables == 1 && watch_updates == 1);
    assert(!virtual_uninterrupted_write_memory(NULL, NULL, 0));
    context.ContextFlags = CONTEXT_ARM64_FULL;
    context.Sp = (ULONG_PTR)(stack + 8192);
    context.Pc = 0xff2a64ac;
    context.X22 = 0x7bc4efd0;
    exception.ExceptionCode = STATUS_ACCESS_VIOLATION;
    exception.ExceptionAddress = (void *)context.Pc;
    exception.NumberParameters = 2;
    exception.ExceptionInformation[1] = 0x7ac0fd00;
    assert(call_user_exception_dispatcher(&exception, &context) == STATUS_UNSUCCESSFUL);
    assert(continued.Sp == context.Sp - 0x470 && continued.Pc == (ULONG_PTR)pKiUserExceptionDispatcher);
    assert(continued.X18 == (ULONG_PTR)&teb);
    assert(!memcmp((void *)continued.Sp, &context, sizeof(context)));
    assert(!memcmp((void *)(continued.Sp + 0x3b0), &exception, sizeof(exception)));
    free(view);
    free(stack);
    puts("Native stack writes: exception frames, bounds, overflow, view permissions and write watches passed");
}
'''
with tempfile.TemporaryDirectory(prefix='native-stack-write-') as directory:
    build = Path(directory)
    path = build / 'stack.c'
    path.write_text(fixture + functions + dispatcher_fixture + dispatcher + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-pthread',
                    '-D__SWITCH__', '-D__WINESRC__', '-D_WIN64', '-fms-extensions',
                    '-I' + str(root / 'include'), str(path), '-o', str(build / 'stack')], check=True)
    subprocess.run([str(build / 'stack')], check=True)
