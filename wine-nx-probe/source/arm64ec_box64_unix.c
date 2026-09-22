/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "amd64_box64_engine.h"
#include "../../dlls/winebox64ec/unixlib.h"
#include "wine/unixlib.h"
#include "horizon_private.h"

struct amd64_process
{
    PEB *peb;
    ULONG_PTR address_limit;
};

struct amd64_thread
{
    TEB *teb;
    CHPE_V2_CPU_AREA_INFO *area;
    AMD64_CONTEXT context;
    struct wine_nx_amd64_state state;
    EXCEPTION_RECORD exception;
    BOOL have_context, have_state, resume_timeout;
};

static struct amd64_process process;
static pthread_key_t thread_key;
static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static int thread_key_error;

extern void wine_nx_box64_invalidate( uintptr_t address, size_t size, int destroy ) __attribute__((weak));

static void create_thread_key(void)
{
    thread_key_error = pthread_key_create( &thread_key, free );
}

static struct amd64_thread *current_thread( ULONGLONG token )
{
    struct amd64_thread *thread;

    pthread_once( &thread_once, create_thread_key );
    if (thread_key_error) return NULL;
    thread = pthread_getspecific( thread_key );
    return token && token == (ULONG_PTR)thread ? thread : NULL;
}

static NTSTATUS read_guest( void *opaque, ULONG_PTR address, void *buffer, SIZE_T size )
{
    SIZE_T read = 0;
    NTSTATUS status;

    if (address >= process.address_limit || size > process.address_limit - address)
        return STATUS_ACCESS_VIOLATION;
    status = NtReadVirtualMemory( NtCurrentProcess(), (void *)address, buffer, size, &read );
    return status ? status : read == size ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

static BOOL is_native( void *opaque, ULONG_PTR address )
{
    ULONG_PTR page = address >> 12;
    const ULONGLONG *bitmap;

    if (address >= process.address_limit || !process.peb || !process.peb->EcCodeBitMap)
        return FALSE;
    bitmap = process.peb->EcCodeBitMap;
    return !!(__atomic_load_n( &bitmap[page / 64], __ATOMIC_ACQUIRE ) &
              ((ULONGLONG)1 << (page % 64)));
}

static NTSTATUS query_abi( void *args )
{
    struct winebox64ec_query_params *p = args;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p))
        return STATUS_REVISION_MISMATCH;
    p->capabilities = WINEBOX64EC_CAP_SSE2;
    p->context_size = sizeof(AMD64_CONTEXT);
    return STATUS_SUCCESS;
}

static NTSTATUS init_process( void *args )
{
    struct winebox64ec_process_params *p = args;
    void *start, *end;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p) || p->flags ||
        !p->dispatch_ret)
        return STATUS_INVALID_PARAMETER;
    p->process = 0;
    if (p->peb != (ULONG_PTR)NtCurrentTeb()->Peb) return STATUS_INVALID_PARAMETER;
    horizon_get_address_space_limits( &start, &end );
    if ((ULONG_PTR)end < 0x8000000000ull) return STATUS_NOT_SUPPORTED;
    process.peb = (PEB *)(ULONG_PTR)p->peb;
    process.address_limit = min( (ULONG_PTR)end, (ULONG_PTR)0x8000000000ull );
    if (!process.peb->EcCodeBitMap) return STATUS_INVALID_PARAMETER;
    if (p->dispatch_ret >= process.address_limit || !is_native( NULL, p->dispatch_ret ))
        return STATUS_INVALID_ADDRESS;
    wine_nx_arm64ec_dispatch_ret = (void *)(ULONG_PTR)p->dispatch_ret;
    p->process = (ULONG_PTR)&process;
    return STATUS_SUCCESS;
}

static NTSTATUS init_thread( void *args )
{
    struct winebox64ec_thread_params *p = args;
    struct amd64_thread *thread;
    TEB *teb = NtCurrentTeb();

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p))
        return STATUS_INVALID_PARAMETER;
    p->thread = 0;
    if (!process.peb || p->teb != (ULONG_PTR)teb || !teb->ChpeV2CpuAreaInfo ||
        p->cpu_area != (ULONG_PTR)teb->ChpeV2CpuAreaInfo ||
        p->suspend_doorbell != (ULONG_PTR)teb->ChpeV2CpuAreaInfo->SuspendDoorbell)
        return STATUS_INVALID_PARAMETER;
    pthread_once( &thread_once, create_thread_key );
    if (thread_key_error) return STATUS_NO_MEMORY;
    if (!(thread = pthread_getspecific( thread_key )))
    {
        if (!(thread = calloc( 1, sizeof(*thread) ))) return STATUS_NO_MEMORY;
        if (pthread_setspecific( thread_key, thread ))
        {
            free( thread );
            return STATUS_NO_MEMORY;
        }
        thread->teb = teb;
        thread->area = teb->ChpeV2CpuAreaInfo;
    }
    p->thread = (ULONG_PTR)thread;
    return STATUS_SUCCESS;
}

static NTSTATUS run_guest( void *args )
{
    struct winebox64ec_run_params *p = args;
    struct wine_nx_amd64_host host = { read_guest, is_native, process.address_limit };
    struct amd64_thread *thread;
    AMD64_CONTEXT *context;
    NTSTATUS status;
    unsigned int i, top;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p))
        return STATUS_INVALID_PARAMETER;
    p->exit_kind = WINEBOX64EC_EXIT_NONE;
    p->target = p->exception_record = p->executed = p->detail0 = p->detail1 = 0;
    if (!(thread = current_thread( p->thread )) ||
        p->context != (ULONG_PTR)thread->area->ContextAmd64)
        return STATUS_INVALID_PARAMETER;
    context = (AMD64_CONTEXT *)(ULONG_PTR)p->context;
    if (p->entry_kind != WINEBOX64EC_ENTRY_LIVE && p->entry_kind != WINEBOX64EC_ENTRY_CONTINUE)
        return STATUS_INVALID_PARAMETER;
    if ((context->ContextFlags & CONTEXT_AMD64_XSTATE) == CONTEXT_AMD64_XSTATE)
        return STATUS_NOT_SUPPORTED;
    if (!thread->have_state ||
        (p->entry_kind == WINEBOX64EC_ENTRY_CONTINUE && !thread->resume_timeout))
    {
        top = (context->FltSave.StatusWord >> 11) & 7;
        for (i = 0; i < 8; i++)
            thread->state.mmx[(top + i) & 7] = context->FltSave.FloatRegisters[i].Low;
        thread->have_state = TRUE;
    }
    thread->resume_timeout = FALSE;
    if (p->entry_kind == WINEBOX64EC_ENTRY_LIVE && thread->have_context)
    {
        context->EFlags = (context->EFlags & 0x8c1) | (thread->context.EFlags & ~0x8c1);
        memcpy( &context->FltSave, &thread->context.FltSave,
                offsetof(XMM_SAVE_AREA32, XmmRegisters) );
        context->FltSave.MxCsr = context->MxCsr;
    }
    status = wine_nx_box64_run_amd64( context, (ULONG_PTR)thread->teb, &thread->state,
                                     &host, thread, 0, 1000000, &p->executed );
#ifdef __SWITCH__
    if (status == STATUS_TIMEOUT && thread->area->SuspendDoorbell &&
        __atomic_load_n( thread->area->SuspendDoorbell, __ATOMIC_ACQUIRE ))
    {
        horizon_wait_suspend_arm64ec();
        thread->have_state = FALSE;
    }
#endif
    thread->context = *context;
    thread->have_context = TRUE;
    p->entry_kind = WINEBOX64EC_ENTRY_CONTINUE;
    if (status == STATUS_TIMEOUT)
    {
        thread->resume_timeout = thread->have_state;
        return status;
    }
    if (status == STATUS_EMULATION_SYSCALL)
    {
        p->exit_kind = WINEBOX64EC_EXIT_SYSCALL;
        return STATUS_SUCCESS;
    }
    if (!status)
    {
        if (!is_native( thread, context->Rip )) return STATUS_INVALID_ADDRESS;
        p->exit_kind = WINEBOX64EC_EXIT_EC_TARGET;
        p->target = context->Rip;
        return STATUS_SUCCESS;
    }
    memset( &thread->exception, 0, sizeof(thread->exception) );
    thread->exception.ExceptionCode = status;
    thread->exception.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    thread->exception.ExceptionAddress = (void *)(ULONG_PTR)context->Rip;
    p->exception_record = (ULONG_PTR)&thread->exception;
    p->exit_kind = WINEBOX64EC_EXIT_EXCEPTION;
    return STATUS_SUCCESS;
}

static NTSTATUS notify_memory( void *args )
{
    struct winebox64ec_notify_params *p = args;
    MEMORY_BASIC_INFORMATION info;
    ULONG_PTR address;
    SIZE_T length;
    int destroy = 0;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p) ||
        p->notification > WINEBOX64EC_NOTIFY_UNMAP)
        return STATUS_INVALID_PARAMETER;
    if (p->is_post && p->status) return STATUS_SUCCESS;
    address = p->address;
    length = p->length;
    if (!address && !length)
    {
        address = 0;
        length = process.address_limit;
    }
    else if (!length)
    {
        ULONG_PTR end;

        if (NtQueryVirtualMemory( NtCurrentProcess(), (void *)address, MemoryBasicInformation,
                                  &info, sizeof(info), NULL )) return STATUS_SUCCESS;
        if (!info.AllocationBase || info.State == MEM_FREE) return STATUS_SUCCESS;
        address = (ULONG_PTR)info.AllocationBase;
        end = (ULONG_PTR)info.BaseAddress + info.RegionSize;
        while (end < process.address_limit &&
               !NtQueryVirtualMemory( NtCurrentProcess(), (void *)end, MemoryBasicInformation,
                                      &info, sizeof(info), NULL ) &&
               (ULONG_PTR)info.AllocationBase == address && info.RegionSize &&
               info.RegionSize <= process.address_limit - end)
            end += info.RegionSize;
        if (end < address) return STATUS_INVALID_PARAMETER;
        length = end - address;
    }
    if (address >= process.address_limit || length > process.address_limit - address)
        return STATUS_INVALID_PARAMETER;
    if (p->notification == WINEBOX64EC_NOTIFY_FREE || p->notification == WINEBOX64EC_NOTIFY_UNMAP)
        destroy = 1;
    if (wine_nx_box64_invalidate) wine_nx_box64_invalidate( address, length, destroy );
    return STATUS_SUCCESS;
}

static NTSTATUS reset_state( void *args )
{
    struct winebox64ec_reset_params *p = args;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p) ||
        !current_thread( p->thread )) return STATUS_INVALID_PARAMETER;
    p->handled = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS term_thread( void *args )
{
    struct winebox64ec_term_params *p = args;
    struct amd64_thread *thread;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p))
        return STATUS_INVALID_PARAMETER;
    if (!(thread = current_thread( p->object ))) return STATUS_INVALID_PARAMETER;
    if ((HANDLE)(ULONG_PTR)p->handle != NtCurrentThread()) return STATUS_SUCCESS;
    pthread_setspecific( thread_key, NULL );
    free( thread );
    return STATUS_SUCCESS;
}

static NTSTATUS term_process( void *args )
{
    struct winebox64ec_term_params *p = args;

    if (!p || p->version != WINEBOX64EC_ABI_VERSION || p->size != sizeof(*p) ||
        p->object != (ULONG_PTR)&process) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

const unixlib_entry_t wine_nx_winebox64ec_unix_funcs[] =
{
    query_abi, init_process, init_thread, run_guest, notify_memory, reset_state, term_thread, term_process
};
