/*
 * AMD64 emulation glue for ARM64EC
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "wine/unixlib.h"

#include "../winebox64/cpuid.h"
#include "unixlib.h"

NTSYSAPI void *WINAPI RtlPcToFileHeader( void *, void ** );

static ULONGLONG process_opaque;
void *x64_return_instr;

C_ASSERT( offsetof(TEB, Peb) == 0x60 );
C_ASSERT( offsetof(TEB, ChpeV2CpuAreaInfo) == 0x1788 );
C_ASSERT( offsetof(PEB, EcCodeBitMap) == 0x368 );
C_ASSERT( offsetof(CHPE_V2_CPU_AREA_INFO, InSimulation) == 0x00 );
C_ASSERT( offsetof(CHPE_V2_CPU_AREA_INFO, EmulatorStackBase) == 0x08 );
C_ASSERT( offsetof(CHPE_V2_CPU_AREA_INFO, ContextAmd64) == 0x18 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, ContextFlags) == 0x030 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_MxCsr_copy) == 0x034 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_SegCs) == 0x038 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_EFlags) == 0x044 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X8) == 0x078 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X0) == 0x080 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X1) == 0x088 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X27) == 0x090 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Sp) == 0x098 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Fp) == 0x0a0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X25) == 0x0a8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X26) == 0x0b0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X2) == 0x0b8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X3) == 0x0c0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X4) == 0x0c8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X5) == 0x0d0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X19) == 0x0d8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X20) == 0x0e0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X21) == 0x0e8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, X22) == 0x0f0 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, Pc) == 0x0f8 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_ControlWord) == 0x100 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_MxCsr) == 0x118 );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, AMD64_MxCsr_Mask) == 0x11c );
C_ASSERT( offsetof(ARM64EC_NT_CONTEXT, V) == 0x1a0 );
C_ASSERT( sizeof(ARM64EC_NT_CONTEXT) == 0x4d0 );

static ULONG fpcsr_to_mxcsr( ULONG fpcr, ULONG fpsr )
{
    ULONG ret = 0;

    if (fpsr & 0x0001) ret |= 0x0001;
    if (fpsr & 0x0080) ret |= 0x0002;
    if (fpsr & 0x0002) ret |= 0x0004;
    if (fpsr & 0x0004) ret |= 0x0008;
    if (fpsr & 0x0008) ret |= 0x0010;
    if (fpsr & 0x0010) ret |= 0x0020;
    if (fpcr & 0x00080000) ret |= 0x0040;
    if (!(fpcr & 0x00000100)) ret |= 0x0080;
    if (!(fpcr & 0x00008000)) ret |= 0x0100;
    if (!(fpcr & 0x00000200)) ret |= 0x0200;
    if (!(fpcr & 0x00000400)) ret |= 0x0400;
    if (!(fpcr & 0x00000800)) ret |= 0x0800;
    if (!(fpcr & 0x00001000)) ret |= 0x1000;
    if (fpcr & 0x00800000) ret |= 0x2000;
    if (fpcr & 0x00400000) ret |= 0x4000;
    if (fpcr & 0x01000000) ret |= 0x8000;
    return ret;
}

static ULONGLONG mxcsr_to_fpcsr( ULONG mxcsr )
{
    ULONG fpcr = 0, fpsr = 0;

    if (mxcsr & 0x0001) fpsr |= 0x0001;
    if (mxcsr & 0x0002) fpsr |= 0x0080;
    if (mxcsr & 0x0004) fpsr |= 0x0002;
    if (mxcsr & 0x0008) fpsr |= 0x0004;
    if (mxcsr & 0x0010) fpsr |= 0x0008;
    if (mxcsr & 0x0020) fpsr |= 0x0010;
    if (mxcsr & 0x0040) fpcr |= 0x00080000;
    if (!(mxcsr & 0x0080)) fpcr |= 0x00000100;
    if (!(mxcsr & 0x0100)) fpcr |= 0x00008000;
    if (!(mxcsr & 0x0200)) fpcr |= 0x00000200;
    if (!(mxcsr & 0x0400)) fpcr |= 0x00000400;
    if (!(mxcsr & 0x0800)) fpcr |= 0x00000800;
    if (!(mxcsr & 0x1000)) fpcr |= 0x00001000;
    if (mxcsr & 0x2000) fpcr |= 0x00800000;
    if (mxcsr & 0x4000) fpcr |= 0x00400000;
    if (mxcsr & 0x8000) fpcr |= 0x01000000;
    return fpcr | ((ULONGLONG)fpsr << 32);
}

static void DECLSPEC_NORETURN fail( NTSTATUS status )
{
    NtTerminateProcess( NtCurrentProcess(), status );
    for (;;) RtlRaiseStatus( status );
}

static CHPE_V2_CPU_AREA_INFO *get_cpu_area(void)
{
    CHPE_V2_CPU_AREA_INFO *area = NtCurrentTeb()->ChpeV2CpuAreaInfo;

    if (!area || !area->ContextAmd64 || !area->EmulatorStackBase ||
        area->EmulatorStackBase <= area->EmulatorStackLimit)
        fail( STATUS_INVALID_PARAMETER );
    return area;
}

static void notify_unix( enum winebox64ec_notification notification, const void *address,
                         SIZE_T length, ULONGLONG argument0, ULONGLONG argument1,
                         BOOL is_post, NTSTATUS result )
{
    struct winebox64ec_notify_params params = {0};

    if (!process_opaque) return;
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.notification = notification;
    params.is_post = is_post;
    params.address = (ULONG_PTR)address;
    params.length = length;
    params.argument0 = argument0;
    params.argument1 = argument1;
    params.status = result;
    WINE_UNIX_CALL( winebox64ec_notify, &params );
}

static void __attribute__((used)) winebox64ec_run_context_returning( CHPE_V2_CPU_AREA_INFO *area,
                                                                     ULONG entry_kind )
{
    struct winebox64ec_run_params params = {0};
    EXCEPTION_RECORD rec = {0};
    ARM64EC_NT_CONTEXT *context = area->ContextAmd64;
    ULONGLONG fpcsr, fpcr, fpsr;
    NTSTATUS status;

    if ((context->ContextFlags & CONTEXT_AMD64_XSTATE) == CONTEXT_AMD64_XSTATE)
        fail( STATUS_NOT_SUPPORTED );
    if (entry_kind == WINEBOX64EC_ENTRY_LIVE)
    {
        __asm__ volatile( "mrs %0, fpcr; mrs %1, fpsr" : "=r" (fpcr), "=r" (fpsr) );
        context->AMD64_MxCsr = context->AMD64_MxCsr_copy = fpcsr_to_mxcsr( fpcr, fpsr );
    }
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.entry_kind = entry_kind;
    params.thread = (ULONG_PTR)area->EmulatorData[0];
    params.context = (ULONG_PTR)context;

    for (;;)
    {
        params.exit_kind = WINEBOX64EC_EXIT_NONE;
        params.target = params.exception_record = 0;
        status = WINE_UNIX_CALL( winebox64ec_run, &params );
        if (status == STATUS_TIMEOUT) continue;
        if (status) fail( status );
        if (params.exit_kind == WINEBOX64EC_EXIT_EC_TARGET)
        {
            if (params.target) context->Pc = params.target;
            if (!context->Pc) fail( STATUS_INVALID_ADDRESS );
            break;
        }
        if (params.exit_kind == WINEBOX64EC_EXIT_SYSCALL)
        {
            rec.ExceptionCode = STATUS_EMULATION_SYSCALL;
            rec.ExceptionAddress = (void *)(ULONG_PTR)context->Pc;
            status = NtRaiseException( &rec, &context->AMD64_Context, TRUE );
        }
        else if (params.exit_kind == WINEBOX64EC_EXIT_EXCEPTION && params.exception_record)
            status = NtRaiseException( (EXCEPTION_RECORD *)(ULONG_PTR)params.exception_record,
                                       &context->AMD64_Context, TRUE );
        else status = STATUS_INVALID_PARAMETER;
        fail( status ? status : STATUS_UNSUCCESSFUL );
    }
    area->InSimulation = FALSE;
    fpcsr = mxcsr_to_fpcsr( context->AMD64_MxCsr );
    fpcr = fpcsr;
    fpsr = fpcsr >> 32;
    __asm__ volatile( "msr fpcr, %0; msr fpsr, %1" :: "r" (fpcr), "r" (fpsr) );
}

NTSTATUS WINAPI ProcessInit(void)
{
    struct winebox64ec_query_params query = {0};
    struct winebox64ec_process_params process = {0};
    HMODULE module = NULL;
    void *ret_page = NULL;
    SIZE_T size = 0x1000;
    ULONG old_protect;
    NTSTATUS status;

    if (process_opaque) return STATUS_SUCCESS;
    if ((status = __wine_init_unix_call())) return status;
    query.version = WINEBOX64EC_ABI_VERSION;
    query.size = sizeof(query);
    if ((status = WINE_UNIX_CALL( winebox64ec_query, &query ))) return status;
    if (query.version != WINEBOX64EC_ABI_VERSION || query.size != sizeof(query) ||
        query.context_size < sizeof(ARM64EC_NT_CONTEXT) ||
        !(query.capabilities & WINEBOX64EC_CAP_SSE2))
        return STATUS_REVISION_MISMATCH;

    status = NtAllocateVirtualMemory( NtCurrentProcess(), &ret_page, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if (status) return status;
    *(BYTE *)ret_page = 0xc3; /* x86-64 RET for a deliberately misaligned guest stack */
    status = NtProtectVirtualMemory( NtCurrentProcess(), &ret_page, &size,
                                     PAGE_EXECUTE_READ, &old_protect );
    if (status)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &ret_page, &size, MEM_RELEASE );
        return status;
    }

    process.version = WINEBOX64EC_ABI_VERSION;
    process.size = sizeof(process);
    process.peb = (ULONG_PTR)NtCurrentTeb()->Peb;
    RtlPcToFileHeader( ProcessInit, (void **)&module );
    if (module) process.dispatch_ret = (ULONG_PTR)RtlFindExportedRoutineByName( module, "RetToEntryThunk" );
    status = WINE_UNIX_CALL( winebox64ec_process_init, &process );
    if (status || !process.process)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &ret_page, &size, MEM_RELEASE );
        return status ? status : STATUS_INVALID_PARAMETER;
    }
    x64_return_instr = ret_page;
    process_opaque = process.process;
    return STATUS_SUCCESS;
}

static void initialize_thread_context( ARM64EC_NT_CONTEXT *context )
{
    if (context->ContextFlags) return;
    memset( context, 0, sizeof(*context) );
    context->ContextFlags = CONTEXT_AMD64_FULL | CONTEXT_AMD64_SEGMENTS;
    context->AMD64_SegCs = 0x33;
    context->AMD64_SegDs = context->AMD64_SegEs = context->AMD64_SegGs =
        context->AMD64_SegSs = 0x2b;
    context->AMD64_SegFs = 0x53;
    context->AMD64_EFlags = 0x202;
    context->AMD64_MxCsr = context->AMD64_MxCsr_copy = 0x1f80;
    context->AMD64_MxCsr_Mask = 0xffff;
    context->AMD64_ControlWord = 0x27f;
}

NTSTATUS WINAPI ThreadInit(void)
{
    struct winebox64ec_thread_params params = {0};
    CHPE_V2_CPU_AREA_INFO *area = get_cpu_area();
    NTSTATUS status;

    if (area->EmulatorData[0]) return STATUS_SUCCESS;
    initialize_thread_context( area->ContextAmd64 );
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.cpu_area = (ULONG_PTR)area;
    params.teb = (ULONG_PTR)NtCurrentTeb();
    params.suspend_doorbell = (ULONG_PTR)area->SuspendDoorbell;
    if ((status = WINE_UNIX_CALL( winebox64ec_thread_init, &params ))) return status;
    if (!params.thread) return STATUS_INVALID_PARAMETER;
    area->EmulatorData[0] = (void *)(ULONG_PTR)params.thread;
    return STATUS_SUCCESS;
}

void WINAPI ThreadTerm( HANDLE handle, LONG exit_code )
{
    CHPE_V2_CPU_AREA_INFO *area = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct winebox64ec_term_params params = {0};

    if (!area || !area->EmulatorData[0] || !RtlIsCurrentThread( handle )) return;
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.object = (ULONG_PTR)area->EmulatorData[0];
    params.handle = (ULONG_PTR)NtCurrentThread();
    params.exit_code = exit_code;
    WINE_UNIX_CALL( winebox64ec_thread_term, &params );
    area->EmulatorData[0] = NULL;
}

void WINAPI ProcessTerm( HANDLE handle, BOOL is_post, NTSTATUS status )
{
    struct winebox64ec_term_params params = {0};

    if (!process_opaque || (handle && !RtlIsCurrentProcess( handle ))) return;
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.object = process_opaque;
    params.handle = (ULONG_PTR)NtCurrentProcess();
    params.is_post = is_post;
    params.status = status;
    WINE_UNIX_CALL( winebox64ec_process_term, &params );
    if (is_post && !status) process_opaque = 0;
}

BOOLEAN WINAPI BTCpu64IsProcessorFeaturePresent( UINT feature )
{
    return winebox64_x86_feature_present( feature );
}

void WINAPI UpdateProcessorInformation( SYSTEM_CPU_INFORMATION *info )
{
    const ULONG signature = WINEBOX64_CPUID_SIGNATURE;

    info->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    info->ProcessorLevel = 15;
    info->ProcessorRevision = (((signature >> 4) & 0xf) << 8) | (signature & 0xf);
}

void WINAPI BTCpu64FlushInstructionCache( void *address, SIZE_T size )
{
    notify_unix( WINEBOX64EC_NOTIFY_FLUSH, address, size, 0, 0, TRUE, STATUS_SUCCESS );
}

void WINAPI FlushInstructionCacheHeavy( void *address, SIZE_T size )
{
    notify_unix( WINEBOX64EC_NOTIFY_FLUSH_HEAVY, address, size, 0, 0, TRUE, STATUS_SUCCESS );
}

void WINAPI BTCpu64NotifyMemoryDirty( void *address, SIZE_T size )
{
    notify_unix( WINEBOX64EC_NOTIFY_DIRTY, address, size, 0, 0, TRUE, STATUS_SUCCESS );
}

void WINAPI BTCpu64NotifyReadFile( HANDLE handle, void *address, SIZE_T size,
                                   BOOL is_post, NTSTATUS status )
{
    notify_unix( WINEBOX64EC_NOTIFY_READ_FILE, address, size, (ULONG_PTR)handle, 0,
                 is_post, status );
}

NTSTATUS WINAPI NotifyMapViewOfSection( void *unknown1, void *address, void *unknown2,
                                        SIZE_T size, ULONG alloc_type, ULONG protect )
{
    struct winebox64ec_notify_params params = {0};

    if (!process_opaque) return STATUS_SUCCESS;
    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.notification = WINEBOX64EC_NOTIFY_MAP;
    params.is_post = TRUE;
    params.address = (ULONG_PTR)address;
    params.length = size;
    params.argument0 = alloc_type;
    params.argument1 = protect;
    return WINE_UNIX_CALL( winebox64ec_notify, &params );
}

void WINAPI NotifyMemoryAlloc( void *address, SIZE_T size, ULONG type, ULONG protect,
                               BOOL is_post, NTSTATUS status )
{
    notify_unix( WINEBOX64EC_NOTIFY_ALLOC, address, size, type, protect, is_post, status );
}

void WINAPI NotifyMemoryFree( void *address, SIZE_T size, ULONG type, BOOL is_post, NTSTATUS status )
{
    notify_unix( WINEBOX64EC_NOTIFY_FREE, address, size, type, 0, is_post, status );
}

void WINAPI NotifyMemoryProtect( void *address, SIZE_T size, ULONG protect,
                                 BOOL is_post, NTSTATUS status )
{
    notify_unix( WINEBOX64EC_NOTIFY_PROTECT, address, size, protect, 0, is_post, status );
}

void WINAPI NotifyUnmapViewOfSection( void *address, BOOL is_post, NTSTATUS status )
{
    notify_unix( WINEBOX64EC_NOTIFY_UNMAP, address, 0, 0, 0, is_post, status );
}

void WINAPI ResetToConsistentState( EXCEPTION_RECORD *record, CONTEXT *context,
                                    ARM64_NT_CONTEXT *native_context )
{
    CHPE_V2_CPU_AREA_INFO *area = get_cpu_area();
    struct winebox64ec_reset_params params = {0};
    NTSTATUS status;

    params.version = WINEBOX64EC_ABI_VERSION;
    params.size = sizeof(params);
    params.thread = (ULONG_PTR)area->EmulatorData[0];
    params.exception_record = (ULONG_PTR)record;
    params.guest_context = (ULONG_PTR)context;
    params.native_context = (ULONG_PTR)native_context;
    status = WINE_UNIX_CALL( winebox64ec_reset, &params );
    if (status) return;
    if (params.handled)
    {
        status = NtContinue( context, FALSE );
        fail( status ? status : STATUS_UNSUCCESSFUL );
    }
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH) LdrDisableThreadCalloutsForDll( instance );
    return TRUE;
}

/* Transition bridge adapted from FEX Module.S at commit
 * 395b132f346b1a45def246d10c52245edba1ef02. Copyright FEX contributors,
 * MIT license. See LICENSE.FEX. */
__ASM_GLOBAL_FUNC( DispatchJump,
                   ".seh_endprologue\n\t"
                   "str lr, [sp, #-8]!\n\t"
                   "b winebox64ec_check_target" )

__ASM_GLOBAL_FUNC( RetToEntryThunk,
                   ".seh_endprologue\n\t"
                   "mov x9, lr\n\t"
                   "b winebox64ec_check_target" )

__ASM_GLOBAL_FUNC( winebox64ec_check_target,
                   ".seh_endprologue\n\t"
                   "lsr x16, x9, #39\n\t"
                   "cbnz x16, winebox64ec_enter_live\n\t"
                   "ldr x16, [x18, #0x60]\n\t"
                   "ldr x16, [x16, #0x368]\n\t"
                   "lsr x17, x9, #18\n\t"
                   "ldr x16, [x16, x17, lsl #3]\n\t"
                   "lsr x17, x9, #12\n\t"
                   "lsr x16, x16, x17\n\t"
                   "tbnz x16, #0, winebox64ec_bridge_ec\n\t"
                   "b winebox64ec_enter_live" )

__ASM_GLOBAL_FUNC( ExitToX64,
                   ".seh_endprologue\n\t"
                   "str lr, [sp, #-8]!\n\t"
                   "b winebox64ec_enter_live" )

__ASM_GLOBAL_FUNC( winebox64ec_enter_live,
                   ".seh_endprologue\n\t"
                   "ldr x16, [x18, #0x1788]\n\t"
                   "mov w17, #1\n\t"
                   "strb w17, [x16]\n\t"
                   "ldr x16, [x16, #0x18]\n\t"
                   "str x8,  [x16, #0x078]\n\t"
                   "str x0,  [x16, #0x080]\n\t"
                   "str x1,  [x16, #0x088]\n\t"
                   "str x27, [x16, #0x090]\n\t"
                   "mov x17, sp\n\t"
                   "str x17, [x16, #0x098]\n\t"
                   "str x29, [x16, #0x0a0]\n\t"
                   "str x25, [x16, #0x0a8]\n\t"
                   "str x26, [x16, #0x0b0]\n\t"
                   "str x2,  [x16, #0x0b8]\n\t"
                   "str x3,  [x16, #0x0c0]\n\t"
                   "str x4,  [x16, #0x0c8]\n\t"
                   "str x5,  [x16, #0x0d0]\n\t"
                   "str x19, [x16, #0x0d8]\n\t"
                   "str x20, [x16, #0x0e0]\n\t"
                   "str x21, [x16, #0x0e8]\n\t"
                   "str x22, [x16, #0x0f0]\n\t"
                   "str x9,  [x16, #0x0f8]\n\t"
                   "stp q0,  q1,  [x16, #0x1a0]\n\t"
                   "stp q2,  q3,  [x16, #0x1c0]\n\t"
                   "stp q4,  q5,  [x16, #0x1e0]\n\t"
                   "stp q6,  q7,  [x16, #0x200]\n\t"
                   "stp q8,  q9,  [x16, #0x220]\n\t"
                   "stp q10, q11, [x16, #0x240]\n\t"
                   "stp q12, q13, [x16, #0x260]\n\t"
                   "stp q14, q15, [x16, #0x280]\n\t"
                   "mrs x15, nzcv\n\t"
                   "mov w14, #2\n\t"
                   "ubfx x13, x15, #29, #1\n\t"
                   "orr w14, w14, w13\n\t"
                   "ubfx x13, x15, #30, #1\n\t"
                   "orr w14, w14, w13, lsl #6\n\t"
                   "ubfx x13, x15, #31, #1\n\t"
                   "orr w14, w14, w13, lsl #7\n\t"
                   "ubfx x13, x15, #28, #1\n\t"
                   "orr w14, w14, w13, lsl #11\n\t"
                   "str w14, [x16, #0x044]\n\t"
                   "mov w14, #0x000b\n\t"
                   "movk w14, #0x0010, lsl #16\n\t"
                   "str w14, [x16, #0x030]\n\t"
                   "ldr x0, [x18, #0x1788]\n\t"
                   "mov w1, #1\n\t"
                   "ldr x15, [x0, #0x008]\n\t"
                   "mov sp, x15\n\t"
                   "bl \"#winebox64ec_run_context_returning\"\n\t"
                   "b winebox64ec_leave_simulation" )

__ASM_GLOBAL_FUNC( BeginSimulation,
                   ".seh_endprologue\n\t"
                   "ldr x0, [x18, #0x1788]\n\t"
                   "mov w16, #1\n\t"
                   "strb w16, [x0]\n\t"
                   "ldr x16, [x0, #0x008]\n\t"
                   "mov sp, x16\n\t"
                   "mov w1, #2\n\t"
                   "bl \"#winebox64ec_run_context_returning\"\n\t"
                   "b winebox64ec_leave_simulation" )

__ASM_GLOBAL_FUNC( winebox64ec_leave_simulation,
                   ".seh_endprologue\n\t"
                   "ldr x17, [x18, #0x1788]\n\t"
                   "strb wzr, [x17]\n\t"
                   "ldr x16, [x17, #0x18]\n\t"
                   "ldr w15, [x16, #0x044]\n\t"
                   "mov x14, #0\n\t"
                   "ubfx x13, x15, #0, #1\n\t"
                   "orr x14, x14, x13, lsl #29\n\t"
                   "ubfx x13, x15, #6, #1\n\t"
                   "orr x14, x14, x13, lsl #30\n\t"
                   "ubfx x13, x15, #7, #1\n\t"
                   "orr x14, x14, x13, lsl #31\n\t"
                   "ubfx x13, x15, #11, #1\n\t"
                   "orr x14, x14, x13, lsl #28\n\t"
                   "msr nzcv, x14\n\t"
                   "ldr x9,  [x16, #0x0f8]\n\t"
                   "ldr x8,  [x16, #0x078]\n\t"
                   "ldr x0,  [x16, #0x080]\n\t"
                   "ldr x1,  [x16, #0x088]\n\t"
                   "ldr x27, [x16, #0x090]\n\t"
                   "ldr x15, [x16, #0x098]\n\t"
                   "ldr x29, [x16, #0x0a0]\n\t"
                   "ldr x25, [x16, #0x0a8]\n\t"
                   "ldr x26, [x16, #0x0b0]\n\t"
                   "ldr x2,  [x16, #0x0b8]\n\t"
                   "ldr x3,  [x16, #0x0c0]\n\t"
                   "ldr x4,  [x16, #0x0c8]\n\t"
                   "ldr x5,  [x16, #0x0d0]\n\t"
                   "ldr x19, [x16, #0x0d8]\n\t"
                   "ldr x20, [x16, #0x0e0]\n\t"
                   "ldr x21, [x16, #0x0e8]\n\t"
                   "ldr x22, [x16, #0x0f0]\n\t"
                   "ldp q0,  q1,  [x16, #0x1a0]\n\t"
                   "ldp q2,  q3,  [x16, #0x1c0]\n\t"
                   "ldp q4,  q5,  [x16, #0x1e0]\n\t"
                   "ldp q6,  q7,  [x16, #0x200]\n\t"
                   "ldp q8,  q9,  [x16, #0x220]\n\t"
                   "ldp q10, q11, [x16, #0x240]\n\t"
                   "ldp q12, q13, [x16, #0x260]\n\t"
                   "ldp q14, q15, [x16, #0x280]\n\t"
                   "mov sp, x15\n\t"
                   "b winebox64ec_bridge_ec" )

__ASM_GLOBAL_FUNC( winebox64ec_bridge_ec,
                   ".seh_endprologue\n\t"
                   "ldr x17, [x18, #0x1788]\n\t"
                   "strb wzr, [x17]\n\t"
                   "mov x17, x9\n\t"
                   "mov w16, #0x0200\n\t"
                   "movk w16, #0xd63f, lsl #16\n\t"
                   "ldursw x15, [x17, #-4]\n\t"
                   "cmp w15, w16\n\t"
                   "b.eq 2f\n\t"
                   "and x15, x15, #-4\n\t"
                   "add x17, x17, x15\n\t"
                   "mov x4, sp\n\t"
                   "tbz x4, #3, 1f\n\t"
                   "ldr lr, [x4], #8\n\t"
                   "mov sp, x4\n"
                   "2:\tbr x17\n"
                   "1:\tadrp lr, x64_return_instr\n\t"
                   "ldr lr, [lr, #:lo12:x64_return_instr]\n\t"
                   "br x17" )
