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


definitions = '\n'.join(re.findall(r'^#define HORIZON_(?:STATUS_|CURRENT_|REQ_|SERVER_HANDLE_HASH_SIZE)\w* .*', source, re.M))
structures = '\n'.join(block(source, 'struct ' + name + '\n') + ';' for name in (
    'horizon_server_request_header', 'horizon_server_reply_header',
    'horizon_server_handle_entry', 'horizon_dup_handle_request', 'horizon_dup_handle_reply',
    'horizon_compare_objects_request', 'horizon_get_object_info_request',
    'horizon_get_object_info_reply', 'horizon_open_thread_request', 'horizon_open_process_reply'))
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
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
struct horizon_server_connection { int reply_fd; struct horizon_server_object *thread; };
static struct horizon_server_connection connection, *horizon_server_current = &connection;
static struct horizon_server_object *horizon_server_threads;
static struct horizon_server_handle_entry *horizon_server_handles;
static struct horizon_server_handle_entry *horizon_server_handle_hash[HORIZON_SERVER_HANDLE_HASH_SIZE];
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned next_handle = 0x100;
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
    'static unsigned int horizon_server_duplicate_object_handle(',
    'static struct horizon_server_object *horizon_server_find_handle_object_locked(',
    'static unsigned int horizon_server_compare_object_handles(',
    'static int horizon_server_handle_dup_handle(',
    'static int horizon_server_handle_compare_objects(',
    'static int horizon_server_handle_get_object_info(',
    'static int horizon_server_handle_open_thread('))
dispatch = r'''
unsigned int CDECL wine_server_call(void *ptr) {
    struct __server_request_info *req = ptr;
    const unsigned char *message = (const void *)&req->u.req;
    struct horizon_server_connection *connection = horizon_server_current;
    int status;
    switch (req->u.req.request_header.req) {
'''
for name in ('DUP_HANDLE', 'COMPARE_OBJECTS', 'GET_OBJECT_INFO', 'OPEN_THREAD'):
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
signal = (root / 'dlls/ntdll/unix/signal_arm64.c').read_text()
contexts = signal[signal.index('static NTSTATUS check_current_thread_context_access('):signal.index('/* Windows leaves the wait')]
context_fixture = r'''
static TEB teb;
#define NtCurrentTeb() (&teb)
static WOW64_CPURESERVED cpu = {0, IMAGE_FILE_MACHINE_I386};
static I386_CONTEXT saved;
static void *get_cpu_area(USHORT machine) { assert(machine == IMAGE_FILE_MACHINE_I386); return &saved; }
'''
tests = r'''
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
    entry->thread_access = THREAD_ALL_ACCESS;
    assert(NtCompareObjects(wine_server_ptr_handle(entry->handle), alias) == STATUS_NOT_SAME_OBJECT);
    assert(get_thread_wow64_context(wine_server_ptr_handle(entry->handle), &ctx, sizeof(ctx)) == STATUS_NOT_IMPLEMENTED);
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
    puts("Horizon thread handles: object queries, duplication, identity and FEX context access passed");
}
'''
with tempfile.TemporaryDirectory(prefix='horizon-handles-') as directory:
    build = Path(directory)
    path = build / 'handles.c'
    path.write_text(fixture + helpers + dispatch + query + ntcalls + context_fixture + contexts + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-pthread', '-D__WINESRC__', '-D_WIN64',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    str(path), '-o', str(build / 'handles')], check=True)
    subprocess.run([str(build / 'handles')], check=True)
