#!/usr/bin/env python3
"""Exercise the Horizon request handlers used by FEX context initialization."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


definitions = '\n'.join(re.findall(r'^#define HORIZON_(?:STATUS_|CURRENT_|REQ_|PROCESS_|SERVER_HANDLE_HASH_SIZE)\w* .*', source, re.M))
structures = '\n'.join(block(source, 'struct ' + name + '\n') + ';' for name in (
    'horizon_server_request_header', 'horizon_server_reply_header',
    'horizon_server_handle_entry', 'horizon_dup_handle_request', 'horizon_dup_handle_reply',
    'horizon_compare_objects_request', 'horizon_get_object_info_request',
    'horizon_get_object_info_reply', 'horizon_open_thread_request', 'horizon_open_process_reply',
    'horizon_get_next_thread_request', 'horizon_get_thread_info_request', 'horizon_get_thread_info_reply',
    'horizon_open_process_request'))
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winternl.h"
#include "wine/server.h"
#include "horizon_threads.h"
#include "horizon_wow64.h"
#undef TRACE
#define TRACE(...) ((void)0)
''' + definitions + '\n' + block(source, 'enum horizon_server_object_type\n') + ';\n' + r'''
struct horizon_server_object {
    unsigned refs, id, mapping_access;
    int type, file_fd, file_peer_fd, completion_closed;
    struct horizon_server_object *thread_next;
    struct horizon_thread_state thread;
};
''' + structures + r'''
struct horizon_server_connection { int reply_fd; unsigned pid; struct horizon_server_object *thread; };
static struct horizon_server_connection connection, *horizon_server_current = &connection;
static struct horizon_server_object *horizon_server_threads;
static unsigned horizon_server_running_threads = 1;
static struct horizon_server_handle_entry *horizon_server_handles;
static struct horizon_server_handle_entry *horizon_server_handle_hash[HORIZON_SERVER_HANDLE_HASH_SIZE];
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned next_handle = 0x100;
static ULONG_PTR horizon_get_system_affinity_mask(void) { return 7; }
static unsigned horizon_server_alloc_handle(void) { return next_handle += 4; }
static union generic_reply wire_reply;
static int horizon_server_write_reply(int fd, const void *data, size_t size, const void *extra, size_t extra_size) {
    assert(fd == 0 && size <= sizeof(wire_reply) && !extra && !extra_size);
    memset(&wire_reply, 0, sizeof(wire_reply));
    memcpy(&wire_reply, data, size);
    return 0;
}
static int horizon_server_write_status(int fd, unsigned status) {
    struct horizon_server_reply_header reply = {status, 0};
    return horizon_server_write_reply(fd, &reply, sizeof(reply), NULL, 0);
}
static pthread_mutex_t fd_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static void server_enter_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *signals) {
    (void)signals; pthread_mutex_lock(mutex);
}
static void server_leave_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *signals) {
    (void)signals; pthread_mutex_unlock(mutex);
}
static int remove_fd_from_cache(HANDLE handle) { (void)handle; assert(0); return -1; }
static void close_inproc_sync(HANDLE handle) { (void)handle; assert(0); }
static unsigned server_queue_process_apc(HANDLE handle, const union apc_call *call, union apc_result *result) {
    (void)handle; (void)call; (void)result; assert(0); return STATUS_NOT_IMPLEMENTED;
}
'''
helpers = '\n'.join(block(source, name) for name in (
    'static struct horizon_server_handle_entry **horizon_server_handle_bucket(',
    'static struct horizon_server_handle_entry *horizon_server_find_handle_locked(',
    'static void horizon_server_link_handle_locked(',
    'static void horizon_server_unlink_handle_locked(',
    'static struct horizon_server_handle_entry *horizon_server_create_handle_locked(',
    'static struct horizon_server_handle_entry *horizon_server_create_handle_for_object_locked(',
    'static unsigned int horizon_server_find_typed_object_locked(',
    'static struct horizon_server_object *horizon_server_get_thread_locked(',
    'static unsigned int horizon_process_map_access(',
    'unsigned int horizon_query_process_affinity(',
    'static unsigned int horizon_server_duplicate_object_handle(',
    'static struct horizon_server_object *horizon_server_find_handle_object_locked(',
    'static unsigned int horizon_server_compare_object_handles(',
    'static int horizon_server_handle_dup_handle(',
    'static int horizon_server_handle_compare_objects(',
    'static int horizon_server_handle_get_object_info(',
    'static int horizon_server_handle_open_thread(',
    'static int horizon_server_handle_open_process(',
    'static int horizon_server_handle_get_thread_info(',
    'static int horizon_server_handle_get_next_thread('))
dispatch = r'''
unsigned int CDECL wine_server_call(void *ptr) {
    struct __server_request_info *req = ptr;
    const unsigned char *message = (const void *)&req->u.req;
    struct horizon_server_connection *connection = horizon_server_current;
    int status;
    switch (req->u.req.request_header.req) {
'''
for name in ('DUP_HANDLE', 'COMPARE_OBJECTS', 'GET_OBJECT_INFO', 'OPEN_THREAD', 'GET_NEXT_THREAD', 'GET_THREAD_INFO'):
    dispatch += re.search(r'        case HORIZON_REQ_' + name + r':\n.*?            break;', source, re.S)[0]
dispatch += r'''
    default: assert(0); return STATUS_NOT_IMPLEMENTED;
    }
    assert(!status);
    req->u.reply = wire_reply;
    return req->u.reply.reply_header.error;
}
'''
file_source = (root / 'dlls/ntdll/unix/file.c').read_text()
query = block(file_source, 'NTSTATUS WINAPI NtQueryObject(')
query = query[:query.index('    case ObjectNameInformation:')] + r'''
    default: return STATUS_NOT_IMPLEMENTED;
    }
    return status;
}
'''
server = (root / 'dlls/ntdll/unix/server.c').read_text()
ntcalls = '\n'.join(block(server, 'NTSTATUS WINAPI ' + name + '(') for name in ('NtDuplicateObject', 'NtCompareObjects'))
ntcalls += '\n' + block((root / 'dlls/ntdll/unix/thread.c').read_text(), 'NTSTATUS WINAPI NtGetNextThread(')
process = (root / 'dlls/ntdll/unix/process.c').read_text()
affinity = process[process.index('    case ProcessAffinityMask:'):process.index('    case ProcessSessionInformation:')]
ntcalls += r'''
#define __SWITCH__
NTSTATUS WINAPI NtQueryInformationProcess(HANDLE handle, PROCESSINFOCLASS class, void *info,
                                         ULONG size, ULONG *ret_len) {
    ULONG len = 0;
    NTSTATUS ret = 0;
    switch (class) {
''' + affinity + r'''
    default: return STATUS_INVALID_INFO_CLASS;
    }
    if (ret_len) *ret_len = len;
    return ret;
}
#undef __SWITCH__
'''
signal = (root / 'dlls/ntdll/unix/signal_arm64.c').read_text()
contexts = signal[signal.index('static NTSTATUS check_current_thread_context_access('):signal.index('extern void wine_nx_call_pe_apc(')]
context_fixture = r'''
static TEB teb;
#define NtCurrentTeb() (&teb)
static WOW64_CPURESERVED cpu = {0, IMAGE_FILE_MACHINE_I386};
static I386_CONTEXT saved, other_saved;
static WOW64_CPURESERVED other_cpu = {0, IMAGE_FILE_MACHINE_I386};
struct thread_data { TEB *teb; };
/* Where ntdll keeps a thread's own data in its TEB (unix_private.h). */
struct teb_data { void *cpu_data[16]; void *syscall_table, *syscall_frame; int syscall_trace; struct thread_data *thread; };
static TEB other_teb;
static struct thread_data thread_data = {&teb}, other_thread_data = {&other_teb};
static struct thread_data *get_thread_data(void) { return &thread_data; }
static void *get_cpu_area(struct thread_data *data, USHORT machine) {
    assert(machine == IMAGE_FILE_MACHINE_I386);
    if (data == &other_thread_data) return &other_saved;
    assert(data == &thread_data); return &saved;
}
/* The wait for another thread to stop: its native context is asked for. */
static unsigned stop_waits;
static NTSTATUS stop_status;
NTSTATUS get_thread_context(HANDLE handle, void *context, BOOL *self, USHORT machine) {
    assert(handle != NtCurrentThread() && machine == IMAGE_FILE_MACHINE_ARM64);
    assert(((CONTEXT *)context)->ContextFlags == CONTEXT_ARM64_CONTROL);
    *self = FALSE;
    stop_waits++;
    return stop_status;
}
'''
tls_fixture = r'''
#define ERR(...) ((void)0)
#define GetProcessHeap() ((HANDLE)1)
static IMAGE_TLS_DIRECTORY test_tls_dir, *tls_dirs;
static UINT tls_module_count;
PVOID WINAPI RtlImageDirectoryEntryToData(HMODULE module, BOOL image, USHORT directory, ULONG *size) {
    (void)module; assert(image && directory == IMAGE_DIRECTORY_ENTRY_TLS);
    *size = sizeof(test_tls_dir); return &test_tls_dir;
}
PVOID WINAPI RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size) {
    assert(heap == GetProcessHeap()); return flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size);
}
PVOID WINAPI RtlReAllocateHeap(HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size) {
    void *result;
    assert(heap == GetProcessHeap() && flags == HEAP_ZERO_MEMORY);
    result = realloc(ptr, size);
    assert(result && size == tls_module_count * 2 * sizeof(*tls_dirs));
    memset((char *)result + size / 2, 0, size / 2);
    return result;
}
BOOLEAN WINAPI RtlFreeHeap(HANDLE heap, ULONG flags, PVOID ptr) {
    assert(heap == GetProcessHeap() && !flags); free(ptr); return TRUE;
}
NTSTATUS WINAPI NtClose(HANDLE handle) {
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked(wine_server_obj_handle(handle));
    if (!entry) return STATUS_INVALID_HANDLE;
    horizon_server_unlink_handle_locked(entry);
    entry->object->refs--;
    free(entry);
    return 0;
}
NTSTATUS WINAPI NtQueryInformationThread(HANDLE handle, THREADINFOCLASS info_class, void *data, ULONG size, ULONG *ret_len) {
    struct horizon_get_thread_info_request request = {0};
    THREAD_BASIC_INFORMATION *info = data;
    assert(info_class == ThreadBasicInformation && size == sizeof(*info) && !ret_len);
    request.handle = wine_server_obj_handle(handle);
    assert(!horizon_server_handle_get_thread_info(&connection, (const void *)&request));
    if (wire_reply.reply_header.error) return wire_reply.reply_header.error;
    memset(info, 0, size);
    info->TebBaseAddress = wine_server_get_ptr(wire_reply.get_thread_info_reply.teb);
    info->ClientId.UniqueThread = ULongToHandle(wire_reply.get_thread_info_reply.tid);
    return 0;
}
'''
tls_fixture += block((root / 'dlls/ntdll/loader.c').read_text(), 'static BOOL alloc_tls_slot(')

tests = r'''
static void test_dynamic_tls(void) {
    struct horizon_server_object threads[4] = {0};
    struct horizon_server_object *saved_threads = horizon_server_threads;
    unsigned saved_pid = connection.pid;
    TEB tebs[3] = {0};
    void **old[2];
    const unsigned char initial[] = {0x52, 0x45, 0x33, 0x21};
    ULONG index = ~0u;
    LDR_DATA_TABLE_ENTRY module = {0};
    unsigned i, j;
    connection.pid = 1;
    tls_module_count = 1;
    tls_dirs = calloc(1, sizeof(*tls_dirs));
    for (i = 0; i < 4; ++i) {
        threads[i].type = HORIZON_SERVER_OBJECT_THREAD;
        threads[i].refs = 1;
        threads[i].thread.tid = 4 * (i + 1);
        threads[i].thread.pid = 1;
        threads[i].thread.teb = i < 3 ? (uintptr_t)&tebs[i] : 0xdead;
        threads[i].thread_next = i ? &threads[i - 1] : NULL;
        if (i < 2) tebs[i].ThreadLocalStoragePointer = calloc(1, sizeof(void *));
    }
    threads[3].thread.terminated = 1;
    horizon_server_threads = &threads[3];
    test_tls_dir.StartAddressOfRawData = (uintptr_t)initial;
    test_tls_dir.EndAddressOfRawData = (uintptr_t)initial + sizeof(initial);
    test_tls_dir.SizeOfZeroFill = 16;
    test_tls_dir.AddressOfIndex = (uintptr_t)&index;
    assert(alloc_tls_slot(&module) && index == 0);
    for (i = 0; i < 2; ++i) {
        old[i] = tebs[i].ThreadLocalStoragePointer;
        assert(!memcmp(old[i][0], initial, sizeof(initial)));
        for (j = 0; j < 16; ++j) assert(!((unsigned char *)old[i][0])[sizeof(initial) + j]);
    }
    assert(old[0][0] != old[1][0] && !tebs[2].ThreadLocalStoragePointer);
    assert(alloc_tls_slot(&module) && index == 1 && tls_module_count == 2);
    for (i = 0; i < 2; ++i) {
        void **slots = tebs[i].ThreadLocalStoragePointer;
        assert(slots != old[i] && slots[0] == old[i][0]);
        assert(!memcmp(slots[1], initial, sizeof(initial)) && slots[0] != slots[1]);
        free(slots[0]); free(slots[1]); free(slots); free(old[i]);
    }
    for (i = 0; i < 4; ++i) assert(threads[i].refs == 1);
    free(tls_dirs);
    horizon_server_threads = saved_threads;
    connection.pid = saved_pid;
}

static void test_next_thread(void) {
    struct horizon_server_object threads[4] = {0};
    struct horizon_server_object process = {.type = HORIZON_SERVER_OBJECT_PROCESS};
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *saved_threads = horizon_server_threads;
    unsigned pid = connection.pid;
    HANDLE next, previous = NULL, handles[3], process_handle;
    unsigned i;
    _Static_assert(HORIZON_REQ_GET_NEXT_THREAD == REQ_get_next_thread, "request number");
    _Static_assert(sizeof(struct horizon_get_next_thread_request) == sizeof(struct get_next_thread_request), "request size");
    _Static_assert(sizeof(struct horizon_open_process_reply) == sizeof(struct get_next_thread_reply), "reply size");
    connection.pid = 1;
    for (i = 0; i < 4; i++) {
        threads[i].type = HORIZON_SERVER_OBJECT_THREAD;
        threads[i].refs = 1;
        threads[i].thread.tid = 4 * (i + 1);
        threads[i].thread.pid = 1;
        threads[i].thread_next = i ? &threads[i - 1] : NULL;
    }
    threads[3].thread.is_system = 1;
    threads[1].thread.terminated = 1;
    horizon_server_threads = &threads[3];
    for (i = 0; i < 3; i++) {
        assert(!NtGetNextThread(NtCurrentProcess(), previous, THREAD_QUERY_INFORMATION, 0, 0, &next));
        entry = horizon_server_find_handle_locked(wine_server_obj_handle(next));
        assert(entry && entry->object == &threads[i]);
        assert(entry->access == (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION));
        handles[i] = previous = next;
    }
    assert(NtGetNextThread(NtCurrentProcess(), previous, 0, 0, 0, &next) == STATUS_NO_MORE_ENTRIES && !next);
    previous = NULL;
    for (i = 3; i--;) {
        assert(!NtGetNextThread(NtCurrentProcess(), previous, 0, 0, 1, &next));
        entry = horizon_server_find_handle_locked(wine_server_obj_handle(next));
        assert(entry->object == &threads[i] && !entry->access);
        previous = handles[i];
        horizon_server_unlink_handle_locked(entry);
        entry->object->refs--;
        free(entry);
    }
    assert(NtGetNextThread(NtCurrentProcess(), previous, 0, 0, 1, &next) == STATUS_NO_MORE_ENTRIES && !next);
    assert(NtGetNextThread(NtCurrentProcess(), 0, 0, 0, 2, &next) == STATUS_INVALID_PARAMETER);
    assert(NtGetNextThread((HANDLE)0xdead, 0, 0, 0, 0, &next) == STATUS_INVALID_HANDLE);
    assert(NtGetNextThread(handles[0], 0, 0, 0, 0, &next) == STATUS_OBJECT_TYPE_MISMATCH);
    assert(NtGetNextThread(NtCurrentProcess(), (HANDLE)0xdead, 0, 0, 0, &next) == STATUS_INVALID_HANDLE);
    entry = horizon_server_create_handle_for_object_locked(&process);
    process_handle = wine_server_ptr_handle(entry->handle);
    assert(NtGetNextThread(NtCurrentProcess(), process_handle, 0, 0, 0, &next) == STATUS_OBJECT_TYPE_MISMATCH);
    connection.pid = 2;
    assert(NtGetNextThread(process_handle, 0, 0, 0, 0, &next) == STATUS_NO_MORE_ENTRIES);
    horizon_server_unlink_handle_locked(entry);
    entry->object->refs--;
    free(entry);
    for (i = 0; i < 3; i++) {
        entry = horizon_server_find_handle_locked(wine_server_obj_handle(handles[i]));
        horizon_server_unlink_handle_locked(entry);
        entry->object->refs--;
        free(entry);
        assert(threads[i].refs == 1);
    }
    connection.pid = pid;
    horizon_server_threads = saved_threads;
}

static OBJECT_BASIC_INFORMATION query_info(HANDLE handle) {
    OBJECT_BASIC_INFORMATION info;
    ULONG size = 0;
    assert(!NtQueryObject(handle, ObjectBasicInformation, &info, sizeof(info), &size));
    assert(size == sizeof(info));
    return info;
}
static HANDLE duplicate(HANDLE handle, ACCESS_MASK access, ULONG options) {
    HANDLE result = NULL;
    assert(!NtDuplicateObject(NtCurrentProcess(), handle, NtCurrentProcess(), &result, access, 0, options));
    assert(result);
    return result;
}
static void test_process_affinity(void) {
    struct horizon_open_process_request request = {0};
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object;
    const unsigned rights[] = {0, PROCESS_QUERY_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION,
                               GENERIC_READ, GENERIC_EXECUTE, MAXIMUM_ALLOWED};
    ULONG_PTR mask = 0;
    ULONG length = 0;
    unsigned saved_pid = connection.pid;
    unsigned handle;
    _Static_assert(sizeof(request) == sizeof(struct open_process_request), "process request size");
    assert(!horizon_query_process_affinity(HORIZON_CURRENT_PROCESS_HANDLE, &mask) && mask == 7);
    assert(!NtQueryInformationProcess(NtCurrentProcess(), ProcessAffinityMask, &mask, sizeof(mask), &length));
    assert(mask == 7 && length == sizeof(mask));
    assert(NtQueryInformationProcess(NtCurrentProcess(), ProcessAffinityMask, &mask, sizeof(mask) - 1,
                                     NULL) == STATUS_INFO_LENGTH_MISMATCH);
    assert(horizon_query_process_affinity(HORIZON_CURRENT_PROCESS_HANDLE, NULL) == HORIZON_STATUS_ACCESS_VIOLATION);
    assert(horizon_query_process_affinity(HORIZON_CURRENT_THREAD_HANDLE, &mask) == HORIZON_STATUS_INVALID_HANDLE);
    connection.pid = request.pid = 23;
    for (unsigned i = 0; i < sizeof(rights) / sizeof(rights[0]); ++i) {
        request.access = rights[i];
        assert(!horizon_server_handle_open_process(&connection, (const void *)&request));
        assert(!wire_reply.reply_header.error);
        handle = wire_reply.open_process_reply.handle;
        mask = 0xcccccccc;
        assert(horizon_query_process_affinity(handle, &mask) == (i ? 0u : HORIZON_STATUS_ACCESS_DENIED));
        assert(mask == (i ? 7 : 0xcccccccc));
        entry = horizon_server_find_handle_locked(handle);
        object = entry->object;
        HANDLE alias = duplicate(wine_server_ptr_handle(handle), 0, DUPLICATE_SAME_ACCESS);
        assert(horizon_query_process_affinity(wine_server_obj_handle(alias), &mask) == (i ? 0u : HORIZON_STATUS_ACCESS_DENIED));
        assert(!NtClose(alias));
        assert(!NtClose(wine_server_ptr_handle(handle)));
        assert(horizon_query_process_affinity(handle, &mask) == HORIZON_STATUS_INVALID_HANDLE);
        assert(!object->refs);
        free(object);
    }
    request.pid++;
    assert(!horizon_server_handle_open_process(&connection, (const void *)&request));
    assert(wire_reply.reply_header.error == HORIZON_STATUS_INVALID_CID && !wire_reply.open_process_reply.handle);
    HANDLE alias = duplicate(NtCurrentProcess(), PROCESS_QUERY_INFORMATION, 0);
    handle = wine_server_obj_handle(alias);
    object = horizon_server_find_handle_locked(handle)->object;
    assert(!horizon_query_process_affinity(handle, &mask) && mask == 7);
    unsigned count = 0;
    for (unsigned bit = 1; bit && bit <= mask; bit = 1u << ++count) assert(count < 32);
    assert(count == 3);
    assert(!NtClose(alias));
    free(object);
    connection.pid = saved_pid;
}
int main(void) {
    struct horizon_server_object self = {.refs = 1, .type = HORIZON_SERVER_OBJECT_THREAD};
    struct horizon_server_object other = {.refs = 1, .type = HORIZON_SERVER_OBJECT_THREAD};
    struct horizon_server_object section = {.refs = 1, .type = HORIZON_SERVER_OBJECT_MAPPING,
                                            .mapping_access = SECTION_ALL_ACCESS};
    struct horizon_server_handle_entry *entry;
    struct horizon_open_thread_request open = {0};
    I386_CONTEXT ctx = {.ContextFlags = CONTEXT_I386_FULL};
    OBJECT_BASIC_INFORMATION info;
    HANDLE alias, read, write, clone, zero;
    ULONG size;
    test_next_thread();
    test_dynamic_tls();
    test_process_affinity();
    _Static_assert(HORIZON_REQ_GET_OBJECT_INFO == REQ_get_object_info, "request number");
    _Static_assert(sizeof(struct horizon_get_object_info_request) == sizeof(struct get_object_info_request), "request size");
    _Static_assert(sizeof(struct horizon_get_object_info_reply) == sizeof(struct get_object_info_reply), "reply size");
    _Static_assert(offsetof(struct horizon_get_object_info_reply, access) == offsetof(struct get_object_info_reply, access), "access offset");
    _Static_assert(offsetof(struct horizon_get_object_info_reply, handle_count) == offsetof(struct get_object_info_reply, handle_count), "count offset");
    assert(HORIZON_THREAD_ALL_ACCESS == THREAD_ALL_ACCESS);
    assert(horizon_thread_map_access(GENERIC_READ) == (STANDARD_RIGHTS_READ | THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_GET_CONTEXT));
    assert(horizon_thread_map_access(GENERIC_WRITE) == (STANDARD_RIGHTS_WRITE | THREAD_SET_LIMITED_INFORMATION | THREAD_SET_INFORMATION | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_TERMINATE | 4));
    assert(horizon_thread_map_access(GENERIC_EXECUTE) == (STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE | THREAD_RESUME | THREAD_QUERY_LIMITED_INFORMATION));
    assert(horizon_thread_map_access(GENERIC_ALL) == THREAD_ALL_ACCESS);
    assert(horizon_thread_map_access(MAXIMUM_ALLOWED) == THREAD_ALL_ACCESS);
    assert(!horizon_thread_map_access(0));
    connection.thread = &self;
    horizon_server_threads = &self;
    self.thread.tid = 4;
    teb.TlsSlots[WOW64_TLS_CPURESERVED] = &cpu;
    saved.Esp = 0x016ffff0;
    saved.Eip = 0x011f5970;
    saved.SegCs = 0x23;
    saved.EFlags = 0x202;
    info = query_info(NtCurrentThread());
    assert(info.GrantedAccess == THREAD_ALL_ACCESS && info.PointerCount == 1 && !info.HandleCount);
    alias = duplicate(NtCurrentThread(), THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, 0);
    info = query_info(alias);
    assert(info.GrantedAccess == (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT));
    assert(info.PointerCount == 2 && info.HandleCount == 1);
    assert(!NtCompareObjects(alias, NtCurrentThread()));
    assert(!NtCompareObjects(NtCurrentThread(), alias));
    assert(!NtCompareObjects(NtCurrentThread(), NtCurrentThread()));
    assert(!set_thread_wow64_context(alias, &saved, sizeof(saved)));
    assert(!get_thread_wow64_context(alias, &ctx, sizeof(ctx)));
    assert(ctx.Esp == saved.Esp && ctx.Eip == saved.Eip && ctx.EFlags == saved.EFlags);
    ctx.Eip += 16;
    assert(!set_thread_wow64_context(alias, &ctx, sizeof(ctx)) && saved.Eip == ctx.Eip);
    read = duplicate(alias, THREAD_GET_CONTEXT, 0);
    write = duplicate(alias, THREAD_SET_CONTEXT, 0);
    clone = duplicate(read, THREAD_ALL_ACCESS, DUPLICATE_SAME_ACCESS);
    zero = duplicate(alias, 0, 0);
    assert(query_info(read).GrantedAccess == THREAD_GET_CONTEXT);
    assert(query_info(clone).GrantedAccess == THREAD_GET_CONTEXT);
    assert(!query_info(zero).GrantedAccess);
    assert(!get_thread_wow64_context(read, &ctx, sizeof(ctx)));
    assert(set_thread_wow64_context(read, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!set_thread_wow64_context(write, &ctx, sizeof(ctx)));
    assert(get_thread_wow64_context(write, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(get_thread_wow64_context(zero, &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(query_info(NtCurrentThread()).HandleCount == 5);
    open.tid = 4;
    open.access = GENERIC_READ;
    assert(!horizon_server_handle_open_thread(&connection, (const void *)&open));
    assert(!wire_reply.reply_header.error);
    assert(query_info(wine_server_ptr_handle(wire_reply.open_thread_reply.handle)).GrantedAccess == horizon_thread_map_access(GENERIC_READ));
    entry = horizon_server_create_handle_for_object_locked(&other);
    entry->access = THREAD_ALL_ACCESS;
    assert(NtCompareObjects(wine_server_ptr_handle(entry->handle), alias) == STATUS_NOT_SAME_OBJECT);
    /* Another thread: found by its TEB, and read once it has stopped. */
    assert(!stop_waits);
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_INVALID_PARAMETER);
    other.thread.tid = 8;
    other.thread.teb = (uintptr_t)&other_teb;
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_INVALID_PARAMETER);
    ((struct teb_data *)&other_teb.GdiTebBatch)->thread = &other_thread_data;
    assert(!stop_waits);
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_INVALID_PARAMETER);
    assert(stop_waits == 1);
    other_teb.TlsSlots[WOW64_TLS_CPURESERVED] = &other_cpu;
    other_saved.Esp = 0x026ffff0;
    other_saved.Eip = 0x021f5970;
    other_saved.SegCs = 0x23;
    other_saved.EFlags = 0x246;
    stop_status = STATUS_UNSUCCESSFUL;
    ctx.Eip = 0;
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_UNSUCCESSFUL);
    assert(stop_waits == 2 && !ctx.Eip);
    stop_status = 0;
    assert(!get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)));
    assert(stop_waits == 3 && ctx.Esp == other_saved.Esp && ctx.Eip == other_saved.Eip && ctx.EFlags == other_saved.EFlags);
    ctx.Eip += 32;
    assert(!set_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)));
    assert(stop_waits == 4 && other_saved.Eip == ctx.Eip && saved.Eip != ctx.Eip);
    /* The handle has to grant the access, and is refused before the thread is asked to stop. */
    entry->access = THREAD_GET_CONTEXT;
    assert(set_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)));
    entry->access = THREAD_SET_CONTEXT;
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_ACCESS_DENIED);
    assert(!set_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)));
    assert(stop_waits == 6);
    assert(NtQueryObject((HANDLE)0xdead, ObjectBasicInformation, &info, sizeof(info), &size) == STATUS_INVALID_HANDLE && !size);
    assert(NtQueryObject(alias, ObjectBasicInformation, &info, sizeof(info) - 1, &size) == STATUS_INFO_LENGTH_MISMATCH && !size);
    assert(NtCompareObjects((HANDLE)0xdead, (HANDLE)0xdead) == STATUS_INVALID_HANDLE);
    entry = horizon_server_create_handle_for_object_locked(&section);
    alias = wine_server_ptr_handle(entry->handle);
    info = query_info(alias);
    assert(info.HandleCount == 1 && info.PointerCount == 2 && info.GrantedAccess == SECTION_ALL_ACCESS);
    clone = duplicate(alias, 0, DUPLICATE_SAME_ACCESS);
    assert(!NtCompareObjects(alias, clone));
    assert(query_info(alias).HandleCount == 2);
    entry = horizon_server_find_handle_locked(wine_server_obj_handle(clone));
    horizon_server_unlink_handle_locked(entry);
    entry->object->refs--;
    free(entry);
    assert(query_info(alias).HandleCount == 1);
    connection.thread = NULL;
    assert(NtQueryObject(NtCurrentThread(), ObjectBasicInformation, &info, sizeof(info), NULL) == STATUS_INVALID_HANDLE);
    while ((entry = horizon_server_handles)) {
        horizon_server_unlink_handle_locked(entry);
        entry->object->refs--;
        free(entry);
    }
    assert(self.refs == 1 && other.refs == 1 && section.refs == 1);
    puts("Horizon thread handles: enumeration, dynamic DLL TLS, lifetime, duplication and context access passed");
}
'''
with tempfile.TemporaryDirectory(prefix='horizon-handles-') as directory:
    build = Path(directory)
    path = build / 'handles.c'
    path.write_text(fixture + helpers + dispatch + query + ntcalls + context_fixture + contexts + tls_fixture + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-pthread', '-D__WINESRC__', '-D_WIN64',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    str(path), '-o', str(build / 'handles')], check=True)
    subprocess.run([str(build / 'handles')], check=True)
