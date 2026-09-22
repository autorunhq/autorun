#!/usr/bin/env python3
"""Exercise the ARM64EC Unix bridge with host-side engine and NT mocks."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'wine-nx-probe/source/arm64ec_box64_unix.c'

fixture = rf'''
#define __WINESRC__
#define WINE_UNIX_LIB
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "dlls/winebox64ec/unixlib.h"
#include "wine-nx-probe/source/amd64_box64_engine.h"

#define __WINE_WINE_UNIXLIB_H
typedef NTSTATUS (*unixlib_entry_t)( void *args );
#define __NTDLL_UNIX_HORIZON_PRIVATE_H
void horizon_get_address_space_limits( void **start, void **limit );
void horizon_wait_suspend_arm64ec(void);
void *wine_nx_arm64ec_dispatch_ret;

static _Thread_local TEB *active_teb;
static TEB *mock_current_teb(void) {{ return active_teb; }}
#define NtCurrentTeb mock_current_teb
#define __SWITCH__
#include "{source.as_posix()}"
#undef NtCurrentTeb

static void *horizon_start, *horizon_end;
static ULONG_PTR read_address;
static SIZE_T read_size;
static unsigned int read_calls;
static struct
{{
    ULONG_PTR start, end, allocation;
    ULONG state;
}} regions[8];
static unsigned int region_count, query_calls;
static ULONG_PTR invalidated_address;
static SIZE_T invalidated_size;
static int invalidated_destroy;
static unsigned int invalidated_calls;

static NTSTATUS engine_status;
static AMD64_CONTEXT engine_seen;
static struct wine_nx_amd64_state engine_state_seen, engine_state_return;
static BOOL engine_replace_state;
static ULONG_PTR engine_gs;
static ULONG_PTR engine_limit;
static ULONGLONG engine_executed;
static unsigned int engine_calls;
static ULONG_PTR engine_probe_read, engine_probe_native;
static NTSTATUS engine_probe_status;
static BOOL engine_probe_native_result;
static unsigned int suspend_calls;

void horizon_wait_suspend_arm64ec(void)
{{
    CHPE_V2_CPU_AREA_INFO *area = active_teb->ChpeV2CpuAreaInfo;
    AMD64_CONTEXT *context = (AMD64_CONTEXT *)area->ContextAmd64;
    unsigned int i;

    assert( *area->SuspendDoorbell );
    *area->SuspendDoorbell = 0;
    suspend_calls++;
    context->Rip = 0x100009000ull;
    context->FltSave.StatusWord = 0;
    for (i = 0; i < 8; i++) context->FltSave.FloatRegisters[i].Low = 0xa000 + i;
}}

void horizon_get_address_space_limits( void **start, void **limit )
{{
    *start = horizon_start;
    *limit = horizon_end;
}}

NTSTATUS WINAPI NtReadVirtualMemory( HANDLE process_handle, const void *address, void *buffer,
                                     SIZE_T size, SIZE_T *read )
{{
    (void)process_handle;
    read_calls++;
    read_address = (ULONG_PTR)address;
    read_size = size;
    memset( buffer, 0x5a, size );
    *read = size;
    return STATUS_SUCCESS;
}}

NTSTATUS WINAPI NtQueryVirtualMemory( HANDLE process_handle, const void *address,
                                      MEMORY_INFORMATION_CLASS class, void *buffer,
                                      SIZE_T size, SIZE_T *ret_size )
{{
    MEMORY_BASIC_INFORMATION *info = buffer;
    ULONG_PTR ptr = (ULONG_PTR)address;
    unsigned int i;

    (void)process_handle;
    assert( class == MemoryBasicInformation && size == sizeof(*info) );
    query_calls++;
    for (i = 0; i < region_count; i++)
    {{
        if (ptr < regions[i].start || ptr >= regions[i].end) continue;
        memset( info, 0, sizeof(*info) );
        info->BaseAddress = (void *)regions[i].start;
        info->AllocationBase = (void *)regions[i].allocation;
        info->RegionSize = regions[i].end - regions[i].start;
        info->State = regions[i].state;
        if (ret_size) *ret_size = sizeof(*info);
        return STATUS_SUCCESS;
    }}
    return STATUS_INVALID_ADDRESS;
}}

void wine_nx_box64_invalidate( uintptr_t address, size_t size, int destroy )
{{
    invalidated_calls++;
    invalidated_address = address;
    invalidated_size = size;
    invalidated_destroy = destroy;
}}

NTSTATUS wine_nx_box64_run_amd64( AMD64_CONTEXT *context, ULONG_PTR gs_base,
                                  struct wine_nx_amd64_state *state,
                                  const struct wine_nx_amd64_host *host, void *opaque,
                                  ULONG_PTR completion, ULONGLONG budget, ULONGLONG *executed )
{{
    unsigned char bytes[8];

    (void)completion;
    assert( budget == 1000000 );
    engine_calls++;
    engine_seen = *context;
    engine_state_seen = *state;
    if (engine_replace_state) *state = engine_state_return;
    engine_gs = gs_base;
    engine_limit = host->address_limit;
    *executed = engine_executed;
    if (engine_probe_read)
        engine_probe_status = host->read( opaque, engine_probe_read, bytes, sizeof(bytes) );
    if (engine_probe_native)
        engine_probe_native_result = host->is_native( opaque, engine_probe_native );
    return engine_status;
}}

static PEB peb;
static TEB main_teb;
static CHPE_V2_CPU_AREA_INFO main_area;
static ARM64EC_NT_CONTEXT main_arm_context;
static ULONG main_doorbell;
static ULONGLONG *native_bitmap;
static ULONG_PTR main_thread_token;

static AMD64_CONTEXT *main_context(void)
{{
    return (AMD64_CONTEXT *)&main_arm_context;
}}

static void init_teb( TEB *teb, CHPE_V2_CPU_AREA_INFO *area,
                      ARM64EC_NT_CONTEXT *context, ULONG *doorbell )
{{
    memset( teb, 0, sizeof(*teb) );
    memset( area, 0, sizeof(*area) );
    memset( context, 0, sizeof(*context) );
    teb->Peb = &peb;
    teb->ChpeV2CpuAreaInfo = area;
    area->ContextAmd64 = context;
    area->SuspendDoorbell = doorbell;
}}

static void mark_native( ULONG_PTR address, BOOL value )
{{
    ULONG_PTR page = address >> 12;
    ULONGLONG mask = (ULONGLONG)1 << (page % 64);

    if (value) native_bitmap[page / 64] |= mask;
    else native_bitmap[page / 64] &= ~mask;
}}

static void test_abi_and_process(void)
{{
    struct winebox64ec_query_params query = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(query) }};
    struct winebox64ec_process_params init = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(init),
                                                .dispatch_ret = 0x100003000ull }};

    assert( query_abi( &query ) == STATUS_SUCCESS );
    assert( query.capabilities == WINEBOX64EC_CAP_SSE2 );
    assert( query.context_size == sizeof(AMD64_CONTEXT) );
    query.version++;
    assert( query_abi( &query ) == STATUS_REVISION_MISMATCH );
    query.version = WINEBOX64EC_ABI_VERSION;
    query.size--;
    assert( query_abi( &query ) == STATUS_REVISION_MISMATCH );
    assert( query_abi( NULL ) == STATUS_REVISION_MISMATCH );

    init.peb = (ULONG_PTR)&peb;
    init.version++;
    assert( init_process( &init ) == STATUS_INVALID_PARAMETER );
    init.version = WINEBOX64EC_ABI_VERSION;
    init.size--;
    assert( init_process( &init ) == STATUS_INVALID_PARAMETER );
    init.size = sizeof(init);
    horizon_end = (void *)0x7fffffffffull;
    assert( init_process( &init ) == STATUS_NOT_SUPPORTED && !init.process );
    horizon_end = (void *)0x9000000000ull;
    init.flags = 1;
    assert( init_process( &init ) == STATUS_INVALID_PARAMETER );
    init.flags = 0;
    init.peb++;
    assert( init_process( &init ) == STATUS_INVALID_PARAMETER );
    init.peb = (ULONG_PTR)&peb;
    peb.EcCodeBitMap = NULL;
    assert( init_process( &init ) == STATUS_INVALID_PARAMETER );
    peb.EcCodeBitMap = native_bitmap;
    assert( init_process( &init ) == STATUS_INVALID_ADDRESS );
    mark_native( init.dispatch_ret, TRUE );
    assert( init_process( &init ) == STATUS_SUCCESS );
    assert( init.process && process.address_limit == 0x8000000000ull );
    assert( wine_nx_arm64ec_dispatch_ret == (void *)(uintptr_t)init.dispatch_ret );
}}

static void test_thread_and_context(void)
{{
    struct winebox64ec_thread_params init = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(init) }};
    struct winebox64ec_run_params run = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(run) }};
    AMD64_CONTEXT old, live, authoritative;
    DWORD expected_flags;
    unsigned int i;

    init.teb = (ULONG_PTR)&main_teb;
    init.cpu_area = (ULONG_PTR)&main_area;
    init.suspend_doorbell = (ULONG_PTR)&main_doorbell;
    init.size--;
    assert( init_thread( &init ) == STATUS_INVALID_PARAMETER );
    init.size = sizeof(init);
    init.cpu_area++;
    assert( init_thread( &init ) == STATUS_INVALID_PARAMETER );
    init.cpu_area = (ULONG_PTR)&main_area;
    assert( init_thread( &init ) == STATUS_SUCCESS );
    main_thread_token = init.thread;
    assert( main_thread_token );

    run.thread = main_thread_token + 1;
    run.context = (ULONG_PTR)main_context();
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    run.version++;
    assert( run_guest( &run ) == STATUS_INVALID_PARAMETER );
    run.version = WINEBOX64EC_ABI_VERSION;
    assert( run_guest( &run ) == STATUS_INVALID_PARAMETER );
    run.thread = main_thread_token;
    run.context++;
    assert( run_guest( &run ) == STATUS_INVALID_PARAMETER );
    run.context = (ULONG_PTR)main_context();
    run.entry_kind = 0;
    assert( run_guest( &run ) == STATUS_INVALID_PARAMETER );

    memset( &old, 0, sizeof(old) );
    old.ContextFlags = CONTEXT_AMD64_FULL;
    old.EFlags = 0x216;
    old.Rax = 0x1111222233334444ull;
    old.FltSave.ControlWord = 0x37f;
    old.FltSave.StatusWord = 3 << 11;
    for (i = 0; i < 8; i++) old.FltSave.FloatRegisters[i].Low = 0x1000 + i;
    old.FltSave.XmmRegisters[0].Low = 0xaaaaaaaaaaaaaaaaull;
    old.MxCsr = old.FltSave.MxCsr = 0x1f80;
    *main_context() = old;
    engine_status = STATUS_EMULATION_SYSCALL;
    engine_executed = 17;
    for (i = 0; i < 8; i++) engine_state_return.mmx[i] = 0x7000 + i;
    engine_replace_state = TRUE;
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( run.exit_kind == WINEBOX64EC_EXIT_SYSCALL && run.executed == 17 );
    assert( run.entry_kind == WINEBOX64EC_ENTRY_CONTINUE );
    assert( engine_gs == (ULONG_PTR)&main_teb && engine_gs > 0xffffffffu );
    assert( engine_limit == 0x8000000000ull );
    for (i = 0; i < 8; i++) assert( engine_state_seen.mmx[(3 + i) & 7] == 0x1000 + i );

    memset( &live, 0, sizeof(live) );
    live.ContextFlags = CONTEXT_AMD64_FULL;
    live.EFlags = 0x8c1;
    live.Rax = 0x5555666677778888ull;
    live.FltSave.ControlWord = 0x1234;
    live.FltSave.FloatRegisters[0].Low = 0x123456789abcdef0ull;
    live.FltSave.XmmRegisters[0].Low = 0xbbbbbbbbbbbbbbbbull;
    live.MxCsr = 0x9fc0;
    live.FltSave.MxCsr = 0xdead;
    *main_context() = live;
    engine_replace_state = FALSE;
    run.entry_kind = WINEBOX64EC_ENTRY_LIVE;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    expected_flags = (live.EFlags & 0x8c1) | (old.EFlags & ~0x8c1);
    assert( engine_seen.EFlags == expected_flags );
    assert( engine_seen.Rax == live.Rax );
    assert( engine_seen.FltSave.ControlWord == old.FltSave.ControlWord );
    assert( engine_seen.FltSave.FloatRegisters[0].Low == old.FltSave.FloatRegisters[0].Low );
    assert( engine_seen.FltSave.XmmRegisters[0].Low == live.FltSave.XmmRegisters[0].Low );
    assert( engine_seen.MxCsr == live.MxCsr && engine_seen.FltSave.MxCsr == live.MxCsr );
    for (i = 0; i < 8; i++) assert( engine_state_seen.mmx[i] == 0x7000 + i );

    memset( &authoritative, 0x5c, sizeof(authoritative) );
    authoritative.ContextFlags = CONTEXT_AMD64_FULL;
    authoritative.Rip = 0x100004000ull;
    authoritative.FltSave.StatusWord = 5 << 11;
    for (i = 0; i < 8; i++) authoritative.FltSave.FloatRegisters[i].Low = 0x9000 + i;
    *main_context() = authoritative;
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( !memcmp( &engine_seen, &authoritative, sizeof(authoritative) ) );
    for (i = 0; i < 8; i++) assert( engine_state_seen.mmx[(5 + i) & 7] == 0x9000 + i );

    main_context()->ContextFlags = CONTEXT_AMD64_XSTATE;
    engine_calls = 0;
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    assert( run_guest( &run ) == STATUS_NOT_SUPPORTED && !engine_calls );
}}

static void test_engine_exits_and_callbacks(void)
{{
    struct winebox64ec_run_params run = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(run) }};
    EXCEPTION_RECORD *exception;
    ULONG_PTR ec_target = 0x100006000ull;
    unsigned int i;

    run.thread = main_thread_token;
    run.context = (ULONG_PTR)main_context();
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    memset( main_context(), 0, sizeof(*main_context()) );
    main_context()->ContextFlags = CONTEXT_AMD64_FULL;
    main_context()->Rip = ec_target;
    mark_native( ec_target, TRUE );
    mark_native( 0x100007000ull, TRUE );
    engine_probe_read = 0x100005123ull;
    engine_probe_native = 0x100007000ull;
    read_calls = 0;
    engine_status = STATUS_SUCCESS;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( run.exit_kind == WINEBOX64EC_EXIT_EC_TARGET && run.target == ec_target );
    assert( engine_probe_status == STATUS_SUCCESS && read_calls == 1 );
    assert( read_address == engine_probe_read && read_size == 8 && engine_probe_native_result );

    mark_native( ec_target, FALSE );
    assert( run_guest( &run ) == STATUS_INVALID_ADDRESS );
    assert( run.exit_kind == WINEBOX64EC_EXIT_NONE );

    engine_status = STATUS_ACCESS_VIOLATION;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( run.exit_kind == WINEBOX64EC_EXIT_EXCEPTION && run.exception_record );
    exception = (EXCEPTION_RECORD *)(ULONG_PTR)run.exception_record;
    assert( exception->ExceptionCode == (DWORD)STATUS_ACCESS_VIOLATION );
    assert( exception->ExceptionFlags == EXCEPTION_NONCONTINUABLE );
    assert( exception->ExceptionAddress == (void *)ec_target );

    engine_status = STATUS_TIMEOUT;
    for (i = 0; i < 8; i++) engine_state_return.mmx[i] = 0xd000 + i;
    engine_replace_state = TRUE;
    assert( run_guest( &run ) == STATUS_TIMEOUT );
    assert( run.exit_kind == WINEBOX64EC_EXIT_NONE );
    for (i = 0; i < 8; i++) main_context()->FltSave.FloatRegisters[i].Low = 0xe000 + i;
    engine_replace_state = FALSE;
    engine_status = STATUS_EMULATION_SYSCALL;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    for (i = 0; i < 8; i++) assert( engine_state_seen.mmx[i] == 0xd000 + i );
    engine_probe_read = engine_probe_native = 0;

    assert( !suspend_calls );
    main_doorbell = 1;
    engine_status = STATUS_TIMEOUT;
    assert( run_guest( &run ) == STATUS_TIMEOUT );
    assert( suspend_calls == 1 && !main_doorbell );
    engine_status = STATUS_EMULATION_SYSCALL;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( engine_seen.Rip == 0x100009000ull );
    for (i = 0; i < 8; i++) assert( engine_state_seen.mmx[i] == 0xa000 + i );
    assert( suspend_calls == 1 );
}}

static void test_notifications(void)
{{
    struct winebox64ec_notify_params notify = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(notify) }};

    notify.notification = WINEBOX64EC_NOTIFY_DIRTY;
    notify.address = 0x123456789ull;
    notify.length = 0x23456;
    invalidated_calls = 0;
    notify.size--;
    assert( notify_memory( &notify ) == STATUS_INVALID_PARAMETER );
    notify.size = sizeof(notify);
    assert( notify_memory( &notify ) == STATUS_SUCCESS );
    assert( invalidated_calls == 1 && invalidated_address == notify.address );
    assert( invalidated_size == notify.length && !invalidated_destroy );

    region_count = 3;
    regions[0].start = 0x100004000ull;
    regions[0].end = 0x100006000ull;
    regions[0].allocation = 0x100000000ull;
    regions[0].state = MEM_COMMIT;
    regions[1].start = 0x100006000ull;
    regions[1].end = 0x100009000ull;
    regions[1].allocation = 0x100000000ull;
    regions[1].state = MEM_RESERVE;
    regions[2].start = 0x100009000ull;
    regions[2].end = 0x10000a000ull;
    regions[2].allocation = 0x100009000ull;
    regions[2].state = MEM_COMMIT;
    notify.notification = WINEBOX64EC_NOTIFY_FREE;
    notify.address = 0x100004123ull;
    notify.length = 0;
    query_calls = invalidated_calls = 0;
    assert( notify_memory( &notify ) == STATUS_SUCCESS );
    assert( query_calls == 3 && invalidated_calls == 1 );
    assert( invalidated_address == 0x100000000ull && invalidated_size == 0x9000 );
    assert( invalidated_destroy );

    notify.is_post = 1;
    notify.status = STATUS_ACCESS_DENIED;
    invalidated_calls = 0;
    assert( notify_memory( &notify ) == STATUS_SUCCESS && !invalidated_calls );
    notify.is_post = 0;
    notify.status = 0;
    notify.address = process.address_limit - 0x1000;
    notify.length = 0x2000;
    assert( notify_memory( &notify ) == STATUS_INVALID_PARAMETER );
}}

struct remote_thread
{{
    TEB teb;
    CHPE_V2_CPU_AREA_INFO area;
    ARM64EC_NT_CONTEXT context;
    ULONG doorbell;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready, proceed;
    ULONG_PTR token;
    ULONGLONG mmx0;
    NTSTATUS reset_status, term_status;
}};

static void *remote_thread_main( void *arg )
{{
    struct remote_thread *remote = arg;
    struct winebox64ec_thread_params init = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(init) }};
    struct winebox64ec_reset_params reset = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(reset) }};
    struct winebox64ec_term_params term = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(term) }};
    struct winebox64ec_run_params run = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(run) }};

    active_teb = &remote->teb;
    init.teb = (ULONG_PTR)&remote->teb;
    init.cpu_area = (ULONG_PTR)&remote->area;
    init.suspend_doorbell = (ULONG_PTR)&remote->doorbell;
    assert( init_thread( &init ) == STATUS_SUCCESS );
    remote->context.ContextFlags = CONTEXT_AMD64_FULL;
    ((AMD64_CONTEXT *)&remote->context)->FltSave.FloatRegisters[0].Low = 0xf000;
    run.thread = init.thread;
    run.context = (ULONG_PTR)&remote->context;
    run.entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    engine_status = STATUS_EMULATION_SYSCALL;
    engine_replace_state = FALSE;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    remote->mmx0 = engine_state_seen.mmx[0];
    pthread_mutex_lock( &remote->mutex );
    remote->token = init.thread;
    remote->ready = 1;
    pthread_cond_signal( &remote->cond );
    while (!remote->proceed) pthread_cond_wait( &remote->cond, &remote->mutex );
    pthread_mutex_unlock( &remote->mutex );
    reset.thread = remote->token;
    remote->reset_status = reset_state( &reset );
    term.object = remote->token;
    term.handle = (ULONG_PTR)NtCurrentThread();
    remote->term_status = term_thread( &term );
    return NULL;
}}

static void test_thread_ownership(void)
{{
    struct remote_thread remote;
    struct winebox64ec_term_params term = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(term) }};
    struct winebox64ec_reset_params reset = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(reset) }};
    pthread_t pthread;
    struct winebox64ec_run_params run = {{ .version = WINEBOX64EC_ABI_VERSION, .size = sizeof(run) }};

    memset( &remote, 0, sizeof(remote) );
    init_teb( &remote.teb, &remote.area, &remote.context, &remote.doorbell );
    pthread_mutex_init( &remote.mutex, NULL );
    pthread_cond_init( &remote.cond, NULL );
    pthread_create( &pthread, NULL, remote_thread_main, &remote );
    pthread_mutex_lock( &remote.mutex );
    while (!remote.ready) pthread_cond_wait( &remote.cond, &remote.mutex );
    pthread_mutex_unlock( &remote.mutex );
    assert( remote.mmx0 == 0xf000 );

    main_context()->ContextFlags = CONTEXT_AMD64_FULL;
    run.thread = main_thread_token;
    run.context = (ULONG_PTR)main_context();
    run.entry_kind = WINEBOX64EC_ENTRY_LIVE;
    engine_status = STATUS_EMULATION_SYSCALL;
    engine_replace_state = FALSE;
    assert( run_guest( &run ) == STATUS_SUCCESS );
    assert( engine_state_seen.mmx[0] == 0xa000 );

    term.object = remote.token;
    term.handle = (ULONG_PTR)NtCurrentThread();
    assert( term_thread( &term ) == STATUS_INVALID_PARAMETER );
    reset.thread = remote.token;
    assert( reset_state( &reset ) == STATUS_INVALID_PARAMETER );
    pthread_mutex_lock( &remote.mutex );
    remote.proceed = 1;
    pthread_cond_signal( &remote.cond );
    pthread_mutex_unlock( &remote.mutex );
    pthread_join( pthread, NULL );
    assert( remote.reset_status == STATUS_SUCCESS && remote.term_status == STATUS_SUCCESS );

    term.object = main_thread_token;
    term.handle = 0x1234;
    assert( term_thread( &term ) == STATUS_SUCCESS );
    reset.thread = main_thread_token;
    assert( reset_state( &reset ) == STATUS_SUCCESS );
    term.handle = (ULONG_PTR)NtCurrentThread();
    assert( term_thread( &term ) == STATUS_SUCCESS );
    assert( reset_state( &reset ) == STATUS_INVALID_PARAMETER );
    pthread_cond_destroy( &remote.cond );
    pthread_mutex_destroy( &remote.mutex );
}}

int main(void)
{{
    horizon_start = (void *)0x10000;
    horizon_end = (void *)0x9000000000ull;
    native_bitmap = calloc( 1, 0x1000000 );
    assert( native_bitmap );
    init_teb( &main_teb, &main_area, &main_arm_context, &main_doorbell );
    active_teb = &main_teb;
    test_abi_and_process();
    test_thread_and_context();
    test_engine_exits_and_callbacks();
    test_notifications();
    test_thread_ownership();
    free( native_bitmap );
    puts( "ARM64EC Box64 Unix bridge: ABI, ownership, contexts, suspension, exits and invalidation passed" );
    return 0;
}}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-arm64ec-unix-') as tmp:
    tmp = Path(tmp)
    c = tmp / 'test.c'
    exe = tmp / 'test'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-pthread',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    '-I' + str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
