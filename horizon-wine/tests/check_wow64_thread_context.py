#!/usr/bin/env python3
"""Check Horizon self-thread handles and WoW64 startup context failures."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
signal = (root / 'dlls/ntdll/unix/signal_arm64.c').read_text()
functions = signal[signal.index('static NTSTATUS check_current_thread_context_access('):signal.index('extern void wine_nx_call_pe_apc(')]
syscall = (root / 'dlls/wow64/syscall.c').read_text()
start = syscall.index('static NTSTATUS thread_init(void)')
init = syscall[start:syscall.index('\n}\n', start) + 3]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "horizon_wow64.h"
#include "struct32.h"
#define GetCurrentThread() NtCurrentThread()
#define GetCurrentProcess() NtCurrentProcess()
static TEB teb;
static TEB32 teb32;
#define NtCurrentTeb() (&teb)
#define NtCurrentTeb32() (&teb32)
static WOW64_CPURESERVED cpu = {0, IMAGE_FILE_MACHINE_I386}, remote_cpu = {0, IMAGE_FILE_MACHINE_I386};
static I386_CONTEXT saved, remote_saved;
static unsigned queries, comparisons, server_info_calls, wait_stopped_calls;
static NTSTATUS query_error, server_info_error = STATUS_NOT_IMPLEMENTED;
struct thread_data { TEB *teb; };
struct teb_data { struct thread_data *thread; };
static struct thread_data thread_data = { &teb }, remote_thread_data;
static TEB remote_teb;
static struct thread_data *get_thread_data(void) { return &thread_data; }
static NTSTATUS get_thread_context(HANDLE handle, void *context, BOOL *self, USHORT machine) {
    CONTEXT *ctx = context;
    assert((handle == (HANDLE)16 || handle == (HANDLE)20) && machine == IMAGE_FILE_MACHINE_ARM64 && ctx->ContextFlags == CONTEXT_ARM64_CONTROL);
    (void)self;
    wait_stopped_calls++;
    return STATUS_SUCCESS;
}
static void *get_cpu_area(struct thread_data *data, USHORT machine) {
    assert((data == &thread_data || data == &remote_thread_data) && machine == IMAGE_FILE_MACHINE_I386);
    return data == &remote_thread_data ? &remote_saved : &saved;
}
struct { unsigned handle, access; } get_thread_info_req, *req = &get_thread_info_req;
struct { ULONG64 teb; } get_thread_info_reply, *reply = &get_thread_info_reply;
#define SERVER_START_REQ(name) do { memset(req, 0, sizeof(*req)); memset(reply, 0, sizeof(*reply));
#define SERVER_END_REQ } while (0)
static unsigned wine_server_obj_handle(HANDLE h) { return (unsigned)(uintptr_t)h; }
static void *wine_server_get_ptr(ULONG64 ptr) { return (void *)(uintptr_t)ptr; }
static NTSTATUS wine_server_call(void *r) {
    (void)r;
    server_info_calls++;
    if ((req->handle == 16 || req->handle == 20) && !server_info_error) {
        reply->teb = (ULONG64)(uintptr_t)&remote_teb;
        return STATUS_SUCCESS;
    }
    return server_info_error;
}
NTSTATUS WINAPI NtCompareObjects(HANDLE first, HANDLE second) {
    assert(second == GetCurrentThread());
    comparisons++;
    if (first == (HANDLE)4 || first == (HANDLE)8 || first == (HANDLE)12) return STATUS_SUCCESS;
    if (first == (HANDLE)16 || first == (HANDLE)20) return STATUS_NOT_SAME_OBJECT;
    return STATUS_INVALID_HANDLE;
}
NTSTATUS WINAPI NtQueryObject(HANDLE handle, OBJECT_INFORMATION_CLASS type, void *buffer, ULONG size, ULONG *retlen) {
    OBJECT_BASIC_INFORMATION *info = buffer;
    assert(type == ObjectBasicInformation && size == sizeof(*info) && !retlen);
    queries++;
    if (query_error) return query_error;
    memset(info, 0, sizeof(*info));
    if (handle == (HANDLE)4) info->GrantedAccess = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;
    else if (handle == (HANDLE)8) info->GrantedAccess = THREAD_GET_CONTEXT;
    else if (handle == (HANDLE)12) info->GrantedAccess = THREAD_SET_CONTEXT;
    else if (handle == (HANDLE)16) info->GrantedAccess = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;
    else if (handle == (HANDLE)20) info->GrantedAccess = THREAD_GET_CONTEXT;
    else assert(0);
    return STATUS_SUCCESS;
}
'''
init_fixture = r'''
static USHORT current_machine = IMAGE_FILE_MACHINE_I386;
static WOW64INFO info, *wow64info = &info;
static SYSTEM_DLL_INIT_BLOCK init_block, *pLdrSystemDllInitBlock = &init_block;
static NTSTATUS get_error, set_error;
static unsigned thread_inits, get_calls, set_calls;
static void *pBTCpuGetBopCode(void) { return (void *)0x110000; }
static void cpu_thread_init(void) { thread_inits++; }
static void (*pBTCpuThreadInit)(void) = cpu_thread_init;
static NTSTATUS pBTCpuGetContext(HANDLE thread, HANDLE process, void *unknown, void *ctx) {
    assert(thread == GetCurrentThread() && process == GetCurrentProcess() && !unknown);
    get_calls++;
    if (get_error) return get_error;
    return get_thread_wow64_context((HANDLE)4, ctx, sizeof(I386_CONTEXT));
}
static NTSTATUS pBTCpuSetContext(HANDLE thread, HANDLE process, void *unknown, void *ctx) {
    assert(thread == GetCurrentThread() && process == GetCurrentProcess() && !unknown);
    set_calls++;
    if (set_error) return set_error;
    return set_thread_wow64_context((HANDLE)4, ctx, sizeof(I386_CONTEXT));
}
#define ERR(...) ((void)0)
'''
tests = r'''
int main(void) {
    I386_CONTEXT ctx = {0}, before;
    void *stack;
    unsigned count;
    teb.TlsSlots[WOW64_TLS_CPURESERVED] = &cpu;
    saved.ContextFlags = ctx.ContextFlags = CONTEXT_I386_FULL;
    saved.Esp = 0x1700000; saved.Eip = 0x110000; saved.Eax = 0x12345678;
    assert(!get_thread_wow64_context(GetCurrentThread(), &ctx, sizeof(ctx)));
    assert(ctx.Esp == saved.Esp && ctx.Eax == saved.Eax && !queries && !comparisons);
    ctx.Esp = 0;
    assert(!get_thread_wow64_context((HANDLE)4, &ctx, sizeof(ctx)) && ctx.Esp == saved.Esp);
    assert(queries == 1 && comparisons == 1);
    ctx.Eax++;
    assert(!set_thread_wow64_context((HANDLE)4, &ctx, sizeof(ctx)) && saved.Eax == ctx.Eax);
    assert(!get_thread_wow64_context((HANDLE)8, &ctx, sizeof(ctx)));
    before = saved;
    assert(set_thread_wow64_context((HANDLE)8, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!memcmp(&before, &saved, sizeof(saved)));
    assert(!set_thread_wow64_context((HANDLE)12, &ctx, sizeof(ctx)));
    before = ctx;
    assert(get_thread_wow64_context((HANDLE)12, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!memcmp(&before, &ctx, sizeof(ctx)));
    count = queries;
    assert(get_thread_wow64_context((HANDLE)16, &ctx, sizeof(ctx)) == STATUS_NOT_IMPLEMENTED);
    assert(set_thread_wow64_context((HANDLE)16, &ctx, sizeof(ctx)) == STATUS_NOT_IMPLEMENTED);
    assert(server_info_calls == 2 && wait_stopped_calls == 0);
    server_info_error = STATUS_SUCCESS;
    remote_thread_data.teb = &remote_teb;
    ((struct teb_data *)&remote_teb.GdiTebBatch)->thread = &remote_thread_data;
    remote_teb.TlsSlots[WOW64_TLS_CPURESERVED] = &remote_cpu;
    remote_saved.ContextFlags = CONTEXT_I386_FULL;
    remote_saved.Eax = 0xfeedface;
    assert(!get_thread_wow64_context((HANDLE)16, &ctx, sizeof(ctx)) && ctx.Eax == 0xfeedface);
    assert(wait_stopped_calls == 1);
    ctx.Eax = 0xcafebabe;
    assert(!set_thread_wow64_context((HANDLE)16, &ctx, sizeof(ctx)) && remote_saved.Eax == 0xcafebabe);
    assert(wait_stopped_calls == 2);
    /* Another thread's handle is held to its access as this thread's is,
     * before the server is asked and before the thread is stopped. */
    count = server_info_calls;
    before = remote_saved;
    assert(set_thread_wow64_context((HANDLE)20, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!memcmp(&before, &remote_saved, sizeof(remote_saved)));
    assert(server_info_calls == count && wait_stopped_calls == 2);
    assert(!get_thread_wow64_context((HANDLE)20, &ctx, sizeof(ctx)) && ctx.Eax == 0xcafebabe);
    assert(wait_stopped_calls == 3);
    count = queries;
    server_info_error = STATUS_NOT_IMPLEMENTED;
    assert(get_thread_wow64_context(NULL, &ctx, sizeof(ctx)) == STATUS_INVALID_HANDLE);
    assert(set_thread_wow64_context(NULL, &ctx, sizeof(ctx)) == STATUS_INVALID_HANDLE);
    assert(queries == count);
    query_error = STATUS_UNSUCCESSFUL;
    assert(get_thread_wow64_context((HANDLE)4, &ctx, sizeof(ctx)) == query_error);
    query_error = 0;
    assert(get_thread_wow64_context((HANDLE)4, &ctx, 1) == STATUS_INFO_LENGTH_MISMATCH);
    assert(set_thread_wow64_context((HANDLE)4, &ctx, 1) == STATUS_INFO_LENGTH_MISMATCH);
    assert(get_thread_wow64_context((HANDLE)4, NULL, sizeof(ctx)) == STATUS_INVALID_PARAMETER);
    teb.TlsSlots[WOW64_TLS_CPURESERVED] = NULL;
    assert(get_thread_wow64_context((HANDLE)4, &ctx, sizeof(ctx)) == STATUS_INVALID_PARAMETER);
    teb.TlsSlots[WOW64_TLS_CPURESERVED] = &cpu;

    get_error = STATUS_NOT_IMPLEMENTED;
    assert(thread_init() == get_error && get_calls == 1 && !set_calls);
    get_error = 0;
    saved.Esp = 0;
    assert(thread_init() == STATUS_BAD_INITIAL_STACK && !set_calls);
    saved.Esp = sizeof(I386_CONTEXT);
    assert(thread_init() == STATUS_BAD_INITIAL_STACK && !set_calls);
    stack = mmap((void *)0x17000000, 65536, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
    assert(stack == (void *)0x17000000);
    saved.Esp = PtrToUlong((char *)stack + 65536);
    before = saved;
    init_block.pLdrInitializeThunk = 0x210000;
    assert(!thread_init() && set_calls == 1);
    assert(saved.Eip == init_block.pLdrInitializeThunk);
    assert(saved.Esp == before.Esp - sizeof(I386_CONTEXT) - 5 * sizeof(ULONG));
    assert(*(ULONG *)ULongToPtr(saved.Esp) == 0xdeadbabe);
    assert(!memcmp((I386_CONTEXT *)ULongToPtr(before.Esp) - 1, &before, sizeof(before)));
    assert(cpu.Flags & WOW64_CPURESERVED_FLAG_RESET_STATE);
    assert(teb32.WOW32Reserved == 0x110000 && teb.TlsSlots[WOW64_TLS_WOW64INFO] == wow64info);
    saved = before;
    set_error = STATUS_ACCESS_DENIED;
    assert(thread_init() == set_error && set_calls == 2);
    assert(!memcmp(&before, &saved, sizeof(saved)));
    munmap(stack, 65536);
    current_machine = IMAGE_FILE_MACHINE_ARMNT;
    get_error = STATUS_NOT_IMPLEMENTED;
    assert(thread_init() == get_error);
    current_machine = 0;
    assert(thread_init() == STATUS_INVALID_IMAGE_FORMAT);
    puts("WoW64 context: self aliases, access rights, rejected remote handles and checked loader frame passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wow64-context-') as directory:
    build = Path(directory)
    file = build / 'context.c'
    file.write_text(fixture + functions + init_fixture + init + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-D__WINESRC__', '-D_WIN64',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    '-I' + str(root / 'dlls/wow64'),
                    str(file), '-o', str(build / 'context')], check=True)
    subprocess.run([str(build / 'context')], check=True)
