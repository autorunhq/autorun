/*
 * ARM64 signal handling routines
 *
 * Copyright 2010-2013 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#ifdef __aarch64__

#include "config.h"

#ifdef __SWITCH__

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "horizon_wow64.h"
#include "horizon_private.h"

void set_process_instrumentation_callback( void *callback )
{
    (void)callback;
}

NTSTATUS signal_set_full_context( CONTEXT *context )
{
    extern void horizon_continue_context( const CONTEXT *context );
    CONTEXT resume;

    if (!context || (context->ContextFlags & CONTEXT_ARM64_FULL) != CONTEXT_ARM64_FULL)
        return STATUS_INVALID_PARAMETER;
    if (is_arm64ec())
    {
        void *start, *end;

        horizon_get_address_space_limits( &start, &end );
        if (context->Pc < (ULONG_PTR)start || context->Pc >= (ULONG_PTR)end ||
            context->Pc >= 0x8000000000ULL)
            return STATUS_INVALID_ADDRESS;
        if (!peb->EcCodeBitMap) return STATUS_INVALID_PARAMETER;
        if (!is_ec_code( context->Pc ))
        {
            CONTEXT *guest;
            SIZE_T written;
            NTSTATUS status;

            if (context->Sp < sizeof(*guest) + 16) return STATUS_INVALID_ADDRESS;
            guest = (CONTEXT *)((context->Sp - sizeof(*guest)) & ~(ULONG_PTR)15);
            status = NtWriteVirtualMemory( NtCurrentProcess(), guest, context, sizeof(*guest), &written );
            if (status) return status;
            if (written != sizeof(*guest)) return STATUS_PARTIAL_COPY;
            resume = *context;
            resume.Sp = (ULONG_PTR)guest;
            resume.Pc = (ULONG_PTR)pKiUserEmulationDispatcher;
            context = &resume;
        }
    }
    horizon_trace( "[CONTINUE] flags=%08x pc=%llx sp=%llx x18=%llx\n",
                   context->ContextFlags, (unsigned long long)context->Pc,
                   (unsigned long long)context->Sp, (unsigned long long)context->X18 );
    horizon_continue_context( context );
    return STATUS_UNSUCCESSFUL; /* The restore routine does not return. */
}

void *get_native_context( CONTEXT *context )
{
    return context;
}

void *get_wow_context( CONTEXT *context )
{
    (void)context;
    return NULL;
}

extern int horizon_capture_context( CONTEXT *context );

NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    BOOL self;
    NTSTATUS status = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );

    if (status || !self) return status;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    BOOL self;
    NTSTATUS status;

    status = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
    if (status || !self) return status;
    return horizon_capture_context( context ) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS check_current_thread_context_access( HANDLE handle, ACCESS_MASK access )
{
    OBJECT_BASIC_INFORMATION info;
    NTSTATUS status;

    if (handle == GetCurrentThread()) return STATUS_SUCCESS;
    status = NtCompareObjects( handle, GetCurrentThread() );
    if (status == STATUS_NOT_SAME_OBJECT) return STATUS_NOT_IMPLEMENTED;
    if (status) return status;
    status = NtQueryObject( handle, ObjectBasicInformation, &info, sizeof(info), NULL );
    if (status) return status;
    return (info.GrantedAccess & access) == access ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}

NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];
    NTSTATUS status;

    if (size != sizeof(I386_CONTEXT)) return STATUS_INFO_LENGTH_MISMATCH;
    if ((status = check_current_thread_context_access( handle, THREAD_SET_CONTEXT ))) return status;
    if (!cpu) return STATUS_INVALID_PARAMETER;
    return horizon_transfer_i386_context( cpu,
                                          get_cpu_area( IMAGE_FILE_MACHINE_I386 ),
                                          (I386_CONTEXT *)ctx, TRUE );
}

NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];
    NTSTATUS status;

    if (size != sizeof(I386_CONTEXT)) return STATUS_INFO_LENGTH_MISMATCH;
    if ((status = check_current_thread_context_access( handle, THREAD_GET_CONTEXT ))) return status;
    if (!cpu) return STATUS_INVALID_PARAMETER;
    return horizon_transfer_i386_context( cpu,
                                          get_cpu_area( IMAGE_FILE_MACHINE_I386 ), ctx, FALSE );
}

/* Windows leaves the wait through KiUserApcDispatcher, which calls the routine
 * on the thread's own stack and continues the context it was given. Here the
 * routine is called where the wait ends, with a context of this place, and the
 * wait's status is what the caller gets -- STATUS_USER_APC, which is the
 * WAIT_IO_COMPLETION a program waiting alertably looks for.
 *
 * The context matters for a 32-bit program: wow64 hands it to
 * Wow64ApcRoutine, which runs the program's own routine in a simulation of its
 * own and leaves it by continuing that context, which comes back here. The
 * program's registers go back as they were, and the status reaches its Eax on
 * the way out of the call that waited, as wow64cpu puts it there
 * (wow64_box64_bridge.c). A thread's first wait is the one ending its loader
 * initialization, where Eax holds the thread's entry point. */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2,
                                   ULONG_PTR arg3, PNTAPCFUNC func, NTSTATUS status )
{
    void (WINAPI *dispatch)( ULONG_PTR, ULONG_PTR, ULONG_PTR, CONTEXT * ) = (void *)func;
    CONTEXT here;

    if (flags) FIXME( "flags %#x are not supported.\n", flags );
    if (!horizon_capture_context( &here )) dispatch( arg1, arg2, arg3, &here );
    /* A context the caller wants resumed instead of returning to it. */
    if (context) return signal_set_full_context( context );
    return status;
}

void call_raise_user_exception_dispatcher(void)
{
}

NTSTATUS call_user_exception_dispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct
    {
        CONTEXT context;
        CONTEXT_EX context_ex;
        EXCEPTION_RECORD rec;
        ULONG64 align, sp, pc, redzone[2];
    } frame = {0};
    CONTEXT resume;
    ULONG_PTR stack;
    SIZE_T written;
    NTSTATUS status;

    C_ASSERT( offsetof(typeof(frame), rec) == 0x3b0 );
    C_ASSERT( sizeof(frame) == 0x470 );
    if (!rec || !context || !pKiUserExceptionDispatcher ||
        (context->ContextFlags & CONTEXT_ARM64_FULL) != CONTEXT_ARM64_FULL)
        return STATUS_INVALID_PARAMETER;
    if (context->Sp < sizeof(frame) + 16) return STATUS_INVALID_ADDRESS;
    stack = (context->Sp & ~(ULONG_PTR)15) - sizeof(frame);
    frame.context = *context;
    frame.rec = *rec;
    frame.sp = context->Sp;
    frame.pc = context->Pc;
    frame.context_ex.Legacy.Length = sizeof(CONTEXT);
    frame.context_ex.Legacy.Offset = -(LONG)sizeof(CONTEXT);
    frame.context_ex.XState.Offset = offsetof(typeof(frame), redzone) - sizeof(CONTEXT);
    frame.context_ex.All.Length = offsetof(typeof(frame), redzone);
    frame.context_ex.All.Offset = -(LONG)sizeof(CONTEXT);
    status = NtWriteVirtualMemory( NtCurrentProcess(), (void *)stack, &frame, sizeof(frame), &written );
    if (status) return status;
    if (written != sizeof(frame)) return STATUS_PARTIAL_COPY;
    resume = *context;
    resume.Sp = stack;
    resume.Pc = (ULONG_PTR)pKiUserExceptionDispatcher;
    resume.X18 = (ULONG_PTR)NtCurrentTeb();
    return signal_set_full_context( &resume );
}

/*
 * User-mode callbacks on Horizon.
 *
 * win32u (running as a "kernel" syscall) calls back into the PE app's window
 * procedures, etc., via KeUserModeCallback.  On a real OS this switches stacks
 * and CPU mode; on the Switch there is no privilege separation and everything
 * runs on a single stack, so we implement it as a direct call into the PE
 * KernelCallbackTable entry, with NtCallbackReturn unwinding back via longjmp.
 *
 * The callback table entry is a PE function; PE code reads its TEB from x18, so
 * the asm trampoline swaps x18 to the TEB for the call (unix code resolves the
 * TEB via pthread_getspecific, so its x18 is irrelevant and restored after).
 * Window-proc callbacks invoke NtCallbackReturn themselves (never returning to
 * the trampoline); simpler callbacks just return their status normally.
 */

struct switch_callback_frame
{
    struct switch_callback_frame *prev;
    struct switch_callback_frame *child;
    jmp_buf                       jmp;
    void                         *args;
    void                         *ret_ptr;
    ULONG                         ret_len;
    NTSTATUS                      status;
};

static __thread struct switch_callback_frame *switch_cb_top;
static pthread_key_t switch_cb_key;
static pthread_once_t switch_cb_once = PTHREAD_ONCE_INIT;
static int switch_cb_key_error;

static void switch_callback_destroy( void *ptr )
{
    struct switch_callback_frame *frame = ptr;
    while (frame)
    {
        struct switch_callback_frame *next = frame->child;
        free( frame->args );
        free( frame->ret_ptr );
        free( frame );
        frame = next;
    }
}

static void switch_callback_init(void)
{
    switch_cb_key_error = pthread_key_create( &switch_cb_key, switch_callback_destroy );
}
static __thread TEB *wine_nx_active_pe_teb;

void wine_nx_set_active_pe_teb( TEB *teb )
{
    extern void horizon_bind_native_stack( TEB *teb );
    if (!is_arm64ec()) horizon_bind_native_stack( teb );
    wine_nx_active_pe_teb = teb;
}

/* call a PE callback: x0=func x1=args x2=len x3=teb; returns the callback's x0.
 * May not return at all if the callback longjmps out via NtCallbackReturn. */
extern ULONG_PTR wine_nx_call_pe_callback( void *func, const void *args, ULONG len, void *teb );
__asm__(
    ".global wine_nx_call_pe_callback\n"
    ".type wine_nx_call_pe_callback, %function\n"
    "wine_nx_call_pe_callback:\n"
    "    hint 34\n"                  /* bti c */
    "    stp x29, x30, [sp, #-32]!\n"
    "    mov x29, sp\n"
    "    str x18, [sp, #16]\n"       /* save unix x18 */
    "    mov x18, x3\n"              /* x18 = PE TEB for the callback */
    "    mov x16, x0\n"              /* func */
    "    mov x0, x1\n"               /* args */
    "    mov w1, w2\n"               /* len */
    "    blr x16\n"
    "    ldr x18, [x29, #16]\n"      /* restore unix x18 */
    "    ldp x29, x30, [sp], #32\n"
    "    ret\n"
    ".size wine_nx_call_pe_callback, . - wine_nx_call_pe_callback\n"
);

NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    TEB *teb = NtCurrentTeb();
    PEB *peb = teb ? teb->Peb : NULL;
    KERNEL_CALLBACK_PROC *table;
    struct switch_callback_frame *frame;

    if (ret_ptr) *ret_ptr = NULL;
    if (ret_len) *ret_len = 0;
    if (!peb || !peb->KernelCallbackTable) return STATUS_NOT_IMPLEMENTED;
    wine_nx_set_active_pe_teb( teb );
    table = peb->KernelCallbackTable;
    if (!table[id]) return STATUS_NOT_IMPLEMENTED;

    pthread_once( &switch_cb_once, switch_callback_init );
    if (switch_cb_key_error) return STATUS_NO_MEMORY;
    frame = switch_cb_top ? switch_cb_top->child : pthread_getspecific( switch_cb_key );
    if (!frame)
    {
        if (!(frame = calloc( 1, sizeof(*frame) ))) return STATUS_NO_MEMORY;
        if (switch_cb_top) switch_cb_top->child = frame;
        else if (pthread_setspecific( switch_cb_key, frame ))
        {
            free( frame );
            return STATUS_NO_MEMORY;
        }
    }
    /* WoW64 rewrites callback arguments in place. Never expose the caller's
     * packed message buffer to that conversion. */
    if (!(frame->args = malloc( len ? len : 1 ))) return STATUS_NO_MEMORY;
    if (len) memcpy( frame->args, args, len );
    frame->prev = switch_cb_top;
    frame->ret_len = 0;
    frame->status = STATUS_SUCCESS;
    switch_cb_top = frame;

    if (!setjmp( frame->jmp ))
    {
        /* callback returned normally (no NtCallbackReturn): status is its retval */
        frame->status = (NTSTATUS)wine_nx_call_pe_callback( (void *)table[id], frame->args, len, teb );
    }
    /* else: NtCallbackReturn longjmp'd here with frame.{ret_ptr,ret_len,status} set */

    switch_cb_top = frame->prev;
    free( frame->args );
    frame->args = NULL;
    /* Results survive caller-buffer frees and stack unwinding. Each nesting
     * depth owns its result until the next callback at that depth; the thread
     * key destructor reclaims every slot on thread exit. */
    if (ret_ptr) *ret_ptr = frame->ret_len ? frame->ret_ptr : NULL;
    if (ret_len) *ret_len = frame->ret_len;
    return frame->status;
}

NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    struct switch_callback_frame *frame = switch_cb_top;

    if (!frame) return STATUS_NO_CALLBACK_ACTIVE;
    if (ret_len)
    {
        void *copy = ret_ptr ? realloc( frame->ret_ptr, ret_len ) : NULL;
        if (!copy)
        {
            frame->ret_len = 0;
            frame->status = ret_ptr ? STATUS_NO_MEMORY : STATUS_INVALID_PARAMETER;
            longjmp( frame->jmp, 1 );
        }
        frame->ret_ptr = copy;
        memcpy( copy, ret_ptr, ret_len );
    }
    frame->ret_len = ret_len;
    frame->status  = status;
    longjmp( frame->jmp, 1 );
}

NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    (void)handle;
    (void)info;
    (void)len;
    return STATUS_NOT_IMPLEMENTED;
}

void signal_init_threading(void)
{
}

NTSTATUS signal_alloc_thread( TEB *teb )
{
    (void)teb;
    return STATUS_SUCCESS;
}

void signal_free_thread( TEB *teb )
{
    (void)teb;
}

void signal_init_process(void)
{
}

/* Installed by the opt-in runtime after native WoW64 initialization. */
void (*wine_nx_wow64_thread_start)( PRTL_THREAD_START_ROUTINE, void *, BOOL, TEB * );

void DECLSPEC_NORETURN wine_nx_start_arm64ec_thread( PRTL_THREAD_START_ROUTINE entry, void *arg,
                                                  BOOL suspend, TEB *teb )
{
    extern void horizon_continue_context( const CONTEXT *context );
    CONTEXT initial = {0}, *saved;

    wine_nx_set_active_pe_teb( teb );
    initial.ContextFlags = CONTEXT_ARM64_FULL;
    initial.X0 = (ULONG_PTR)entry;
    initial.X1 = (ULONG_PTR)arg;
    initial.X18 = (ULONG_PTR)teb;
    initial.Sp = (ULONG_PTR)teb->Tib.StackBase & ~(ULONG_PTR)15;
    initial.Pc = (ULONG_PTR)pRtlUserThreadStart;
    if (suspend)
    {
        initial.ContextFlags |= CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
        wait_suspend( &initial );
        initial.ContextFlags = CONTEXT_ARM64_FULL;
    }
    saved = (CONTEXT *)(initial.Sp & ~(ULONG_PTR)15) - 1;
    *saved = initial;
    initial.Sp = (ULONG_PTR)saved;
    initial.X0 = (ULONG_PTR)saved;
    initial.Pc = (ULONG_PTR)pLdrInitializeThunk;
    horizon_continue_context( &initial );
    for (;;) sleep( 3600 );
}

void DECLSPEC_NORETURN signal_start_thread( PRTL_THREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    if (is_arm64ec()) wine_nx_start_arm64ec_thread( entry, arg, suspend, teb );
    if (get_wow_teb( teb ))
    {
        if (wine_nx_wow64_thread_start) wine_nx_wow64_thread_start( entry, arg, suspend, teb );
        /* Never branch to guest bytes using the ARM64 instruction set. */
        horizon_trace( "[THREAD] WoW64 startup failed or unavailable; parking" );
        for (;;) sleep( 3600 );
    }
    (void)suspend;
    wine_nx_set_active_pe_teb( teb );
    /* PE code expects x18 = TEB for the whole time it runs (same dance as
     * call_pe_entry_point in the runtime for the main thread).  Without it
     * the first TEB access in the thread procedure faults on whatever the
     * unix pthread runtime left in x18. */
    if (entry)
    {
        ULONG_PTR ret;
        __asm__ volatile(
            "mov x16, %[entry]\n\t"
            "mov x17, %[teb]\n\t"
            "mov x0, %[arg]\n\t"
            "mov x20, x18\n\t"
            "mov x18, x17\n\t"
            "blr x16\n\t"
            "mov x18, x20\n\t"
            "mov %[ret], x0\n\t"
            : [ret] "=r"(ret)
            : [entry] "r"(entry), [arg] "r"(arg), [teb] "r"(teb)
            : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
              "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
              "x16", "x17", "x20", "x30", "memory", "cc" );
        /* This path calls the procedure directly, without LdrInitializeThunk,
         * so there is no DLL detach to run. Exit so waiters on the handle see
         * the thread finish with its return value as the exit code. */
        NtTerminateThread( GetCurrentThread(), (LONG)ret );
        horizon_trace( "[THREAD] native thread exit failed; parking" );
    }
    for (;;) sleep( 3600 );
}

/*
 * Minimal ARM64 syscall dispatcher for the Switch build.
 *
 * When PE code (e.g. kernelbase.dll) makes an NT syscall, the PE ntdll stub
 * puts the syscall number in w8, the arguments in x0-x7, and branches to
 * __wine_syscall_dispatcher via the patched pointer.
 *
 * This dispatcher:
 *   1. Extracts the syscall number (w8 & 0xfff) and table index (w8 >> 12)
 *   2. Looks up KeServiceDescriptorTable[table].ServiceTable[syscall_num]
 *   3. If the syscall has >64 bytes of arguments, copies the extra stack
 *      arguments from the caller's frame
 *   4. Calls the handler (x0-x7 are unchanged)
 *   5. Returns the NTSTATUS result in x0
 *
 * We don't do a full context save because the Switch build doesn't use
 * signal-based exception delivery.  Callee-saved registers are preserved
 * by the normal ABI.
 */

extern SYSTEM_SERVICE_TABLE KeServiceDescriptorTable[];

/* C dispatcher called from the assembly trampoline below */
extern void wine_nx_runtime_trace( const char *msg );
extern int wine_nx_runtime_verbose __attribute__((weak));

/* Reacquire the Windows platform register after returning from Unix code.
 * Nested callbacks may unwind past a dispatcher frame, so restoring a saved
 * x18 is not sufficient; Unix TLS remains the authoritative TEB source. */
void *wine_nx_current_teb(void)
{
    if (!wine_nx_active_pe_teb) wine_nx_active_pe_teb = NtCurrentTeb();
    return wine_nx_active_pe_teb;
}

/* System calls from PE code, reported by the runtime's [PROGRESS] line. */
unsigned int wine_nx_syscalls;
/* Calls per system call id (table << 12 | function), for the runtime's [PROGRESS] line. */
unsigned int wine_nx_syscall_counts[0x2000];
void *wine_nx_arm64ec_dispatch_ret;

NTSTATUS wine_nx_do_syscall( ULONG_PTR *stack_args,
                                    ULONG_PTR x0, ULONG_PTR x1,
                                    ULONG_PTR x2, ULONG_PTR x3,
                                    ULONG_PTR x4, ULONG_PTR x5,
                                    ULONG_PTR x6, ULONG_PTR x7,
                                    unsigned int syscall_id )
{
    unsigned int table_idx = (syscall_id >> 12) & 3;
    unsigned int func_idx  = syscall_id & 0xfff;
#ifdef __SWITCH__
    /* + and - ask for the launcher back. A thread cannot be ended from outside
     * on Horizon, so each ends itself here, where every one of them passes. */
    {
        extern volatile int wine_nx_quit_requested __attribute__((weak));
        extern void wine_nx_quit_point( void ) __attribute__((weak));

        if (&wine_nx_quit_requested && wine_nx_quit_requested && &wine_nx_quit_point) wine_nx_quit_point();
    }
#endif
    SYSTEM_SERVICE_TABLE *table = &KeServiceDescriptorTable[table_idx];
    ULONG_PTR *handler;
    unsigned int arg_bytes;
    NTSTATUS result;
    BOOL trace_syscall = &wine_nx_runtime_verbose && wine_nx_runtime_verbose &&
                         !(table_idx == 1 && func_idx == 1051); /* NtUserGetMessage idle polling */

    if (func_idx >= table->ServiceLimit)
        return STATUS_INVALID_SYSTEM_SERVICE;

    handler = (ULONG_PTR *)table->ServiceTable[func_idx];
    if (!handler)
        return STATUS_INVALID_SYSTEM_SERVICE;
    /* winebox64 reads each x86 syscall's stack arguments through this. */
    if ((void *)handler == (void *)NtReadVirtualMemory) trace_syscall = FALSE;
    else
    {
        __atomic_add_fetch( &wine_nx_syscalls, 1, __ATOMIC_RELAXED );
        __atomic_add_fetch( &wine_nx_syscall_counts[syscall_id & 0x1fff], 1, __ATOMIC_RELAXED );
    }

    arg_bytes = table->ArgumentTable ? table->ArgumentTable[func_idx] : 0;

    if (trace_syscall)
    {
        char buf[128];
        snprintf( buf, sizeof(buf), "[SYSCALL] id=0x%04x t=%u f=%u args=%u handler=%p tid=%04x",
                  syscall_id, table_idx, func_idx, arg_bytes, handler,
                  (unsigned)HandleToULong( NtCurrentTeb()->ClientId.UniqueThread ) );
        wine_nx_runtime_trace( buf );
    }

    /*
     * ARM64 calling convention: first 8 args in x0-x7 (64 bytes).
     * If arg_bytes > 64, the extra arguments were on the PE caller's stack
     * and the assembly trampoline saved the caller's SP so we can copy them.
     *
     * We use a function pointer cast to call with up to 16 arguments (128 bytes).
     * This covers all NT syscalls in practice (max observed is ~112 bytes).
     */
    typedef NTSTATUS (*syscall_func_8)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                                       ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR);
    typedef NTSTATUS (*syscall_func_16)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                                        ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                                        ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                                        ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR);

    if (arg_bytes <= 64)
    {
        result = ((syscall_func_8)handler)(x0,x1,x2,x3,x4,x5,x6,x7);
    }
    else
    {
        ULONG_PTR a8=0,a9=0,a10=0,a11=0,a12=0,a13=0,a14=0,a15=0;
        unsigned int extra = (arg_bytes - 64) / 8;
        if (stack_args)
        {
            if (extra > 0) a8  = stack_args[0];
            if (extra > 1) a9  = stack_args[1];
            if (extra > 2) a10 = stack_args[2];
            if (extra > 3) a11 = stack_args[3];
            if (extra > 4) a12 = stack_args[4];
            if (extra > 5) a13 = stack_args[5];
            if (extra > 6) a14 = stack_args[6];
            if (extra > 7) a15 = stack_args[7];
        }
        result = ((syscall_func_16)handler)(x0,x1,x2,x3,x4,x5,x6,x7,
                                           a8,a9,a10,a11,a12,a13,a14,a15);
    }

    if (trace_syscall)
    {
        char buf[128];
        snprintf( buf, sizeof(buf), "[SYSCALL] id=0x%04x done status=0x%08x tid=%04x",
                  syscall_id, (unsigned)result,
                  (unsigned)HandleToULong( NtCurrentTeb()->ClientId.UniqueThread ) );
        wine_nx_runtime_trace( buf );
    }

    return result;
}

/*
 * Assembly trampoline.
 *
 * On entry (from PE ntdll syscall stub via blr x16):
 *   x0-x7 = syscall arguments
 *   w8    = syscall number
 *   x9    = return address from the PE stub (LR saved by stub, x9 = old LR)
 *   x29   = frame pointer
 *   x30   = return address (back to PE stub)
 *   sp    = PE caller stack (with extra args if any)
 *
 * We save the caller's SP (for stack args) and call wine_nx_do_syscall
 * with: (stack_args, x0, x1, x2, x3, x4, x5, x6, x7, syscall_id)
 *
 * wine_nx_do_syscall takes 10 args = 80 bytes.  First 8 go in x0-x7,
 * the last 2 (x7_orig, syscall_id) go on the stack.
 */
__asm__(
    ".global __wine_syscall_dispatcher\n"
    ".type __wine_syscall_dispatcher, %function\n"
    "__wine_syscall_dispatcher:\n"
    "    /* save callee-saved regs + LR + x18 (PE TEB register) */\n"
    "    stp x29, x30, [sp, #-256]!\n"
    /* x30 at entry = address of `ret` in PE syscall stub (set by `blr x16`).
     * Stub saved the real PE caller's LR into x9 before the blr.
     * Overwrite the saved x30 slot with x9 so our final `ret` jumps directly
     * to the PE caller, dodging the stub's `ret` (which would otherwise loop
     * on itself because x30 still points at it). */
    "    str x9, [sp, #8]\n"
    "    mov x29, sp\n"
    "    stp x19, x20, [sp, #16]\n"
    "    stp x21, x22, [sp, #32]\n"
    "    str x18, [sp, #48]\n"       /* preserve PE's TEB register */
    "    stp q8, q9, [sp, #96]\n"
    "    stp q10, q11, [sp, #128]\n"
    "    stp q12, q13, [sp, #160]\n"
    "    stp q14, q15, [sp, #192]\n"
    "    stp q6, q7, [sp, #224]\n"

    "    /* save original args and syscall number */\n"
    "    mov w19, w8\n"          /* syscall_id */
    "    mov x20, x0\n"         /* save original x0 */
    "    mov x21, x7\n"         /* save original x7 */
    "    add x22, x29, #256\n"  /* caller's SP (where stack args live) */

    "    /* set up call to wine_nx_do_syscall(stack_args, x0..x7, syscall_id) */\n"
    "    /* arg 0 (x0): stack_args = caller SP */\n"
    "    mov x9, x0\n"          /* temporarily save x0 */
    "    mov x0, x22\n"         /* stack_args */
    "    /* args 1-7 shift: x1=orig_x0, x2=orig_x1, etc. */\n"
    "    mov x10, x1\n"         /* save x1 */
    "    mov x1, x9\n"          /* orig x0 */
    "    mov x9, x2\n"          /* save x2 */
    "    mov x2, x10\n"         /* orig x1 */
    "    mov x10, x3\n"         /* save x3 */
    "    mov x3, x9\n"          /* orig x2 */
    "    mov x9, x4\n"          /* save x4 */
    "    mov x4, x10\n"         /* orig x3 */
    "    mov x10, x5\n"         /* save x5 */
    "    mov x5, x9\n"          /* orig x4 */
    "    mov x9, x6\n"          /* save x6 */
    "    mov x6, x10\n"         /* orig x5 */
    "    mov x7, x9\n"          /* orig x6 */
    "    /* remaining 2 args go on stack: orig x7 and syscall_id */\n"
    "    stp x21, x19, [sp, #-16]!\n"  /* [sp] = orig x7, [sp+8] = syscall_id */

    "    bl wine_nx_do_syscall\n"

    "    add sp, sp, #16\n"     /* pop stack args */
    "    /* x0 = NTSTATUS result; force x18 back to the current Wine TEB. */\n"
    "    mov x19, x0\n"
    "    bl wine_nx_current_teb\n"
    "    mov x18, x0\n"
    "    mov x0, x19\n"
    "    ldp x19, x20, [x29, #16]\n"
    "    ldp x21, x22, [x29, #32]\n"
    "    ldp q8, q9, [x29, #96]\n"
    "    ldp q10, q11, [x29, #128]\n"
    "    ldp q12, q13, [x29, #160]\n"
    "    ldp q14, q15, [x29, #192]\n"
    "    ldp q6, q7, [x29, #224]\n"
    "    ldp x29, x30, [sp], #256\n"
    "    lsr x16, x30, #39\n"
    "    cbnz x16, 1f\n"
    "    ldr x16, [x18, #0x60]\n"
    "    ldr x16, [x16, #0x368]\n"
    "    cbz x16, 2f\n"
    "    lsr x17, x30, #18\n"
    "    ldr x16, [x16, x17, lsl #3]\n"
    "    lsr x17, x30, #12\n"
    "    lsr x16, x16, x17\n"
    "    tbnz x16, #0, 2f\n"
    "1:\n"
    "    adrp x16, wine_nx_arm64ec_dispatch_ret\n"
    "    ldr x16, [x16, :lo12:wine_nx_arm64ec_dispatch_ret]\n"
    "    cbz x16, 2f\n"
    "    br x16\n"
    "2:  ret\n"
    ".size __wine_syscall_dispatcher, . - __wine_syscall_dispatcher\n"
);

void __wine_syscall_dispatcher_return(void)
{
    /* Not needed on Switch — we return normally from the dispatcher */
}

/*
 * __wine_unix_call_dispatcher — handles __wine_unix_call from PE code.
 *
 * PE ntdll's __wine_unix_call stub passes:
 *   x0 = unixlib handle (function table)
 *   x1 = function index
 *   x2 = argument pointer
 * and branches to *__wine_unix_call_dispatcher.
 *
 * The unix call mechanism dispatches to: table[index](args)
 */
typedef NTSTATUS (*unixlib_entry_t)( void *args );

NTSTATUS __wine_unix_call_dispatcher_impl( unixlib_handle_t handle, unsigned int index, void *args )
{
    const unixlib_entry_t *table = (const unixlib_entry_t *)(ULONG_PTR)handle;
    if (!table) return STATUS_INVALID_PARAMETER;
    return table[index]( args );
}

__asm__(
    ".global __wine_unix_call_dispatcher\n"
    ".global wine_nx_pe_unix_call_dispatcher\n"
    ".type __wine_unix_call_dispatcher, %function\n"
    ".type wine_nx_pe_unix_call_dispatcher, %function\n"
    "__wine_unix_call_dispatcher:\n"
    "wine_nx_pe_unix_call_dispatcher:\n"
    "    /* x0=handle, x1=index, x2=args — save x18 (PE TEB) then forward */\n"
    "    stp x29, x30, [sp, #-208]!\n"
    "    mov x29, sp\n"
    "    str x18, [sp, #16]\n"
    "    str x19, [sp, #24]\n"
    "    stp q8, q9, [sp, #48]\n"
    "    stp q10, q11, [sp, #80]\n"
    "    stp q12, q13, [sp, #112]\n"
    "    stp q14, q15, [sp, #144]\n"
    "    stp q6, q7, [sp, #176]\n"
    "    bl __wine_unix_call_dispatcher_impl\n"
    "    mov x19, x0\n"
    "    bl wine_nx_current_teb\n"
    "    mov x18, x0\n"
    "    mov x0, x19\n"
    "    ldr x19, [sp, #24]\n"
    "    ldp q8, q9, [sp, #48]\n"
    "    ldp q10, q11, [sp, #80]\n"
    "    ldp q12, q13, [sp, #112]\n"
    "    ldp q14, q15, [sp, #144]\n"
    "    ldp q6, q7, [sp, #176]\n"
    "    ldp x29, x30, [sp], #208\n"
    "    ret\n"
    ".size __wine_unix_call_dispatcher, . - __wine_unix_call_dispatcher\n"
    ".size wine_nx_pe_unix_call_dispatcher, . - wine_nx_pe_unix_call_dispatcher\n"
);


#else /* __SWITCH__ */

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else
# ifdef HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif
#ifdef HAVE_SYS_SIGNAL_H
# include <sys/signal.h>
#endif
#ifdef HAVE_SYS_UCONTEXT_H
# include <sys/ucontext.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "unix_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

#define NTDLL_DWARF_H_NO_UNWINDER
#include "dwarf.h"

struct arm64_thread_data
{
    BOOL suspend_pending;
};

C_ASSERT( sizeof(struct arm64_thread_data) <= sizeof(((struct ntdll_thread_data *)0)->cpu_data) );

static inline struct arm64_thread_data *arm64_thread_data(void)
{
    return (struct arm64_thread_data *)ntdll_get_thread_data()->cpu_data;
}

/***********************************************************************
 * signal context platform-specific definitions
 */
#ifdef linux

/* All Registers access - only for local access */
# define REG_sig(reg_name, context) ((context)->uc_mcontext.reg_name)
# define REGn_sig(reg_num, context) ((context)->uc_mcontext.regs[reg_num])

/* Special Registers access  */
# define SP_sig(context)            REG_sig(sp, context)    /* Stack pointer */
# define PC_sig(context)            REG_sig(pc, context)    /* Program counter */
# define PSTATE_sig(context)        REG_sig(pstate, context) /* Current State Register */
# define FP_sig(context)            REGn_sig(29, context)    /* Frame pointer */
# define LR_sig(context)            REGn_sig(30, context)    /* Link Register */

static struct _aarch64_ctx *get_extended_sigcontext( const ucontext_t *sigcontext, unsigned int magic )
{
    struct _aarch64_ctx *ctx = (struct _aarch64_ctx *)sigcontext->uc_mcontext.__reserved;
    BOOL extra = FALSE;
    while ((extra || (char *)ctx < (char *)(&sigcontext->uc_mcontext + 1)) && ctx->magic && ctx->size)
    {
        if (ctx->magic == magic) return ctx;

        if (ctx->magic == EXTRA_MAGIC)
        {
            ctx = (struct _aarch64_ctx *)((struct extra_context *)ctx)->datap;
            extra = TRUE;
        }
        else
        {
            ctx = (struct _aarch64_ctx *)((char *)ctx + ctx->size);
        }
    }
    return NULL;
}

static struct fpsimd_context *get_fpsimd_context( const ucontext_t *sigcontext )
{
    return (struct fpsimd_context *)get_extended_sigcontext( sigcontext, FPSIMD_MAGIC );
}

static struct sve_context *get_sve_context( const ucontext_t *sigcontext )
{
    return (struct sve_context *)get_extended_sigcontext( sigcontext, SVE_MAGIC );
}

static DWORD64 get_fault_esr( ucontext_t *sigcontext )
{
    struct esr_context *esr = (struct esr_context *)get_extended_sigcontext( sigcontext, ESR_MAGIC );
    if (esr) return esr->esr;
    return 0;
}

#elif defined(__APPLE__)

/* All Registers access - only for local access */
# define REG_sig(reg_name, context) ((context)->uc_mcontext->__ss.__ ## reg_name)
# define REGn_sig(reg_num, context) ((context)->uc_mcontext->__ss.__x[reg_num])

/* Special Registers access  */
# define SP_sig(context)            REG_sig(sp, context)    /* Stack pointer */
# define PC_sig(context)            REG_sig(pc, context)    /* Program counter */
# define PSTATE_sig(context)        REG_sig(cpsr, context)  /* Current State Register */
# define FP_sig(context)            REG_sig(fp, context)    /* Frame pointer */
# define LR_sig(context)            REG_sig(lr, context)    /* Link Register */

static DWORD64 get_fault_esr( ucontext_t *sigcontext )
{
    return sigcontext->uc_mcontext->__es.__esr;
}

#endif /* linux */

/* stack layout when calling KiUserExceptionDispatcher */
struct exc_stack_layout
{
    CONTEXT              context;        /* 000 */
    CONTEXT_EX           context_ex;     /* 390 */
    EXCEPTION_RECORD     rec;            /* 3b0 */
    ULONG64              align;          /* 448 */
    ULONG64              sp;             /* 450 */
    ULONG64              pc;             /* 458 */
    ULONG64              redzone[2];     /* 460 */
};
C_ASSERT( offsetof(struct exc_stack_layout, rec) == 0x3b0 );
C_ASSERT( sizeof(struct exc_stack_layout) == 0x470 );

/* stack layout when calling KiUserApcDispatcher */
struct apc_stack_layout
{
    void                *func;           /* 000 APC to call*/
    ULONG64              args[3];        /* 008 function arguments */
    ULONG64              alertable;      /* 020 */
    ULONG64              align;          /* 028 */
    CONTEXT              context;        /* 030 */
    ULONG64              redzone[2];     /* 3c0 */
};
C_ASSERT( offsetof(struct apc_stack_layout, context) == 0x30 );
C_ASSERT( sizeof(struct apc_stack_layout) == 0x3d0 );

/* stack layout when calling KiUserCallbackDispatcher */
struct callback_stack_layout
{
    void                *args;           /* 000 arguments */
    ULONG                len;            /* 008 arguments len */
    ULONG                id;             /* 00c function id */
    ULONG64              unknown;        /* 010 */
    ULONG64              lr;             /* 018 */
    ULONG64              sp;             /* 020 sp+pc (machine frame) */
    ULONG64              pc;             /* 028 */
    BYTE                 args_data[0];   /* 030 copied argument data*/
};
C_ASSERT( offsetof(struct callback_stack_layout, sp) == 0x20 );
C_ASSERT( sizeof(struct callback_stack_layout) == 0x30 );

struct syscall_frame
{
    ULONG64               x[29];          /* 000 */
    ULONG64               fp;             /* 0e8 */
    ULONG64               lr;             /* 0f0 */
    ULONG64               sp;             /* 0f8 */
    ULONG64               pc;             /* 100 */
    ULONG                 cpsr;           /* 108 */
    ULONG                 restore_flags;  /* 10c */
    struct syscall_frame *prev_frame;     /* 110 */
    void                 *syscall_cfa;    /* 118 */
    ULONG                 syscall_id;     /* 120 */
    ULONG                 align;          /* 124 */
    ULONG                 fpcr;           /* 128 */
    ULONG                 fpsr;           /* 12c */
    NEON128               v[32];          /* 130 */
};

C_ASSERT( sizeof( struct syscall_frame ) == 0x330 );

static BOOL is_arm64ec_suspend_doorbell_valid(void)
{
    return is_arm64ec() && NtCurrentTeb()->ChpeV2CpuAreaInfo && NtCurrentTeb()->ChpeV2CpuAreaInfo->SuspendDoorbell;
}

/***********************************************************************
 *           context_init_empty_xstate
 *
 * Initializes a context's CONTEXT_EX structure to point to an empty xstate buffer
 */
static inline void context_init_empty_xstate( CONTEXT *context, void *xstate_buffer )
{
    CONTEXT_EX *xctx;

    xctx = (CONTEXT_EX *)(context + 1);
    xctx->Legacy.Length = sizeof(CONTEXT);
    xctx->Legacy.Offset = -(LONG)sizeof(CONTEXT);
    xctx->XState.Length = 0;
    xctx->XState.Offset = (BYTE *)xstate_buffer - (BYTE *)xctx;
    xctx->All.Length = sizeof(CONTEXT) + xctx->XState.Offset + xctx->XState.Length;
    xctx->All.Offset = -(LONG)sizeof(CONTEXT);
}

void set_process_instrumentation_callback( void *callback )
{
    if (callback) FIXME( "Not supported.\n" );
}

/***********************************************************************
 *           syscall_frame_fixup_for_fastpath
 *
 * Fixes up the given syscall frame such that the syscall dispatcher
 * can return via the fast path if CONTEXT_INTEGER is set in
 * restore_flags.
 *
 * Clobbers the frame's X16 and X17 register values.
 */
static void syscall_frame_fixup_for_fastpath( struct syscall_frame *frame )
{
    frame->x[16] = frame->pc;
    frame->x[17] = frame->sp;
}

/***********************************************************************
 *           save_fpu
 *
 * Set the FPU context from a sigcontext.
 */
static void save_fpu( CONTEXT *context, const ucontext_t *sigcontext )
{
#ifdef linux
    struct fpsimd_context *fp = get_fpsimd_context( sigcontext );

    if (!fp) return;
    context->ContextFlags |= CONTEXT_FLOATING_POINT;
    context->Fpcr = fp->fpcr;
    context->Fpsr = fp->fpsr;
    memcpy( context->V, fp->vregs, sizeof(context->V) );
#elif defined(__APPLE__)
    context->ContextFlags |= CONTEXT_FLOATING_POINT;
    context->Fpcr = sigcontext->uc_mcontext->__ns.__fpcr;
    context->Fpsr = sigcontext->uc_mcontext->__ns.__fpsr;
    memcpy( context->V, sigcontext->uc_mcontext->__ns.__v, sizeof(context->V) );
#endif
}


/***********************************************************************
 *           restore_fpu
 *
 * Restore the FPU context to a sigcontext.
 */
static void restore_fpu( const CONTEXT *context, ucontext_t *sigcontext )
{
#ifdef linux
    struct fpsimd_context *fp = get_fpsimd_context( sigcontext );

    if (!fp) return;
    fp->fpcr = context->Fpcr;
    fp->fpsr = context->Fpsr;
    memcpy( fp->vregs, context->V, sizeof(fp->vregs) );
#elif defined(__APPLE__)
    sigcontext->uc_mcontext->__ns.__fpcr = context->Fpcr;
    sigcontext->uc_mcontext->__ns.__fpsr = context->Fpsr;
    memcpy( sigcontext->uc_mcontext->__ns.__v, context->V, sizeof(context->V) );
#endif
}


/***********************************************************************
 *           save_context
 *
 * Set the register values from a sigcontext.
 */
static void save_context( CONTEXT *context, const ucontext_t *sigcontext )
{
    DWORD i;

    context->ContextFlags = CONTEXT_FULL;
    context->Fp   = FP_sig(sigcontext);     /* Frame pointer */
    context->Lr   = LR_sig(sigcontext);     /* Link register */
    context->Sp   = SP_sig(sigcontext);     /* Stack pointer */
    context->Pc   = PC_sig(sigcontext);     /* Program Counter */
    context->Cpsr = PSTATE_sig(sigcontext); /* Current State Register */
    for (i = 0; i <= 28; i++) context->X[i] = REGn_sig( i, sigcontext );
    save_fpu( context, sigcontext );
}


/***********************************************************************
 *           restore_context
 *
 * Build a sigcontext from the register values.
 */
static void restore_context( const CONTEXT *context, ucontext_t *sigcontext )
{
    DWORD i;

    FP_sig(sigcontext)     = context->Fp;   /* Frame pointer */
    LR_sig(sigcontext)     = context->Lr;   /* Link register */
    SP_sig(sigcontext)     = context->Sp;   /* Stack pointer */
    PC_sig(sigcontext)     = context->Pc;   /* Program Counter */
    PSTATE_sig(sigcontext) = context->Cpsr; /* Current State Register */
    for (i = 0; i <= 28; i++) REGn_sig( i, sigcontext ) = context->X[i];
    restore_fpu( context, sigcontext );
}


/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (is_arm64ec_suspend_doorbell_valid() && arm64_thread_data()->suspend_pending)
    {
        CONTEXT suspend_context;
        *NtCurrentTeb()->ChpeV2CpuAreaInfo->SuspendDoorbell = 0;
        arm64_thread_data()->suspend_pending = FALSE;
        suspend_context.ContextFlags = CONTEXT_FULL | CONTEXT_EXCEPTION_REPORTING; /* TODO: check */
        NtGetContextThread( GetCurrentThread(), &suspend_context );
        wait_suspend( &suspend_context );
        NtSetContextThread( GetCurrentThread(), &suspend_context );
    }

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER) /* TODO: also check with susp */
        frame->restore_flags |= CONTEXT_INTEGER;

    if (is_arm64ec() && !is_ec_code( frame->pc ))
    {
        CONTEXT *user_context = (CONTEXT *)((frame->sp - sizeof(CONTEXT)) & ~15);

        user_context->ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), user_context );
        frame->sp = (ULONG_PTR)user_context;
        frame->pc = (ULONG_PTR)pKiUserEmulationDispatcher;
    }
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return context;
}


/***********************************************************************
 *              get_wow_context
 */
void *get_wow_context( CONTEXT *context )
{
    return get_cpu_area( main_image_info.Machine );
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS ret = STATUS_SUCCESS;
    BOOL self = (handle == GetCurrentThread());
    DWORD flags = context->ContextFlags & ~CONTEXT_ARM64;

    if (self && (flags & CONTEXT_DEBUG_REGISTERS)) self = FALSE;

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (ret || !self) return ret;
    }

    if (flags & CONTEXT_INTEGER)
    {
        memcpy( frame->x, context->X, sizeof(context->X[0]) * 18 );
        /* skip x18 */
        memcpy( frame->x + 19, context->X + 19, sizeof(context->X[0]) * 10 );
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->fp    = context->Fp;
        frame->lr    = context->Lr;
        frame->sp    = context->Sp;
        frame->pc    = context->Pc;
        frame->cpsr  = context->Cpsr;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        frame->fpcr = context->Fpcr;
        frame->fpsr = context->Fpsr;
        memcpy( frame->v, context->V, sizeof(frame->v) );
    }
    if (flags & CONTEXT_ARM64_X18)
    {
        frame->x[18] = context->X[18];
    }
    if (flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    frame->restore_flags |= flags & ~CONTEXT_INTEGER;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtGetContextThread  (NTDLL.@)
 *              ZwGetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM64;
    THREAD_BASIC_INFORMATION info;
    NTSTATUS ret;
    BOOL self;

    NtQueryInformationThread( handle, ThreadBasicInformation, &info, sizeof(info), NULL );
    self = HandleToULong( info.ClientId.UniqueThread ) == GetCurrentThreadId();

    if (!self)
    {
        /* Avoid exposing JIT code pointers to other processes on ARM64EC */
        if (is_arm64ec()) NtSuspendThread( handle, NULL );
        ret = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (is_arm64ec()) NtResumeThread( handle, NULL );
        return ret;
    }

    if (needed_flags & CONTEXT_INTEGER)
    {
        memcpy( context->X, frame->x, sizeof(context->X[0]) * 29 );
        context->ContextFlags |= CONTEXT_INTEGER;
    }
    if (needed_flags & CONTEXT_CONTROL)
    {
        context->Fp   = frame->fp;
        context->Lr   = frame->lr;
        context->Sp   = frame->sp;
        context->Pc   = frame->pc;
        context->Cpsr = frame->cpsr;
        context->ContextFlags |= CONTEXT_CONTROL;
    }
    if (needed_flags & CONTEXT_FLOATING_POINT)
    {
        context->Fpcr = frame->fpcr;
        context->Fpsr = frame->fpsr;
        memcpy( context->V, frame->v, sizeof(context->V) );
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
    }
    if (needed_flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              set_thread_wow64_context
 */
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = set_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame;
        const I386_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_i386;

        if (flags & CONTEXT_I386_INTEGER)
        {
            wow_frame->Eax = context->Eax;
            wow_frame->Ebx = context->Ebx;
            wow_frame->Ecx = context->Ecx;
            wow_frame->Edx = context->Edx;
            wow_frame->Esi = context->Esi;
            wow_frame->Edi = context->Edi;
        }
        if (flags & CONTEXT_I386_CONTROL)
        {
            WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];

            wow_frame->Esp    = context->Esp;
            wow_frame->Ebp    = context->Ebp;
            wow_frame->Eip    = context->Eip;
            wow_frame->EFlags = context->EFlags;
            wow_frame->SegCs  = context->SegCs;
            wow_frame->SegSs  = context->SegSs;
            cpu->Flags |= WOW64_CPURESERVED_FLAG_RESET_STATE;
        }
        if (flags & CONTEXT_I386_SEGMENTS)
        {
            wow_frame->SegDs = context->SegDs;
            wow_frame->SegEs = context->SegEs;
            wow_frame->SegFs = context->SegFs;
            wow_frame->SegGs = context->SegGs;
        }
        if (flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            wow_frame->Dr0 = context->Dr0;
            wow_frame->Dr1 = context->Dr1;
            wow_frame->Dr2 = context->Dr2;
            wow_frame->Dr3 = context->Dr3;
            wow_frame->Dr6 = context->Dr6;
            wow_frame->Dr7 = context->Dr7;
        }
        if (flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( &wow_frame->ExtendedRegisters, context->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
        }
        if (flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &wow_frame->FloatSave, &context->FloatSave, sizeof(context->FloatSave) );
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame;
        const ARM_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_ARM;

        if (flags & CONTEXT_INTEGER)
        {
            wow_frame->R0  = context->R0;
            wow_frame->R1  = context->R1;
            wow_frame->R2  = context->R2;
            wow_frame->R3  = context->R3;
            wow_frame->R4  = context->R4;
            wow_frame->R5  = context->R5;
            wow_frame->R6  = context->R6;
            wow_frame->R7  = context->R7;
            wow_frame->R8  = context->R8;
            wow_frame->R9  = context->R9;
            wow_frame->R10 = context->R10;
            wow_frame->R11 = context->R11;
            wow_frame->R12 = context->R12;
        }
        if (flags & CONTEXT_CONTROL)
        {
            wow_frame->Sp = context->Sp;
            wow_frame->Lr = context->Lr;
            wow_frame->Pc = context->Pc & ~1;
            wow_frame->Cpsr = context->Cpsr;
            if (context->Cpsr & 0x20) wow_frame->Pc |= 1; /* thumb */
        }
        if (flags & CONTEXT_FLOATING_POINT)
        {
            wow_frame->Fpscr = context->Fpscr;
            memcpy( wow_frame->D, context->D, sizeof(context->D) );
        }
        break;
    }

    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              get_thread_wow64_context
 */
NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_i386;

        if (needed_flags & CONTEXT_I386_INTEGER)
        {
            context->Eax = wow_frame->Eax;
            context->Ebx = wow_frame->Ebx;
            context->Ecx = wow_frame->Ecx;
            context->Edx = wow_frame->Edx;
            context->Esi = wow_frame->Esi;
            context->Edi = wow_frame->Edi;
            context->ContextFlags |= CONTEXT_I386_INTEGER;
        }
        if (needed_flags & CONTEXT_I386_CONTROL)
        {
            context->Esp    = wow_frame->Esp;
            context->Ebp    = wow_frame->Ebp;
            context->Eip    = wow_frame->Eip;
            context->EFlags = wow_frame->EFlags;
            context->SegCs  = wow_frame->SegCs;
            context->SegSs  = wow_frame->SegSs;
            context->ContextFlags |= CONTEXT_I386_CONTROL;
        }
        if (needed_flags & CONTEXT_I386_SEGMENTS)
        {
            context->SegDs = wow_frame->SegDs;
            context->SegEs = wow_frame->SegEs;
            context->SegFs = wow_frame->SegFs;
            context->SegGs = wow_frame->SegGs;
            context->ContextFlags |= CONTEXT_I386_SEGMENTS;
        }
        if (needed_flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( context->ExtendedRegisters, &wow_frame->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
            context->ContextFlags |= CONTEXT_I386_EXTENDED_REGISTERS;
        }
        if (needed_flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &context->FloatSave, &wow_frame->FloatSave, sizeof(context->FloatSave) );
            context->ContextFlags |= CONTEXT_I386_FLOATING_POINT;
        }
        if (needed_flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            context->Dr0 = wow_frame->Dr0;
            context->Dr1 = wow_frame->Dr1;
            context->Dr2 = wow_frame->Dr2;
            context->Dr3 = wow_frame->Dr3;
            context->Dr6 = wow_frame->Dr6;
            context->Dr7 = wow_frame->Dr7;
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM;

        if (needed_flags & CONTEXT_INTEGER)
        {
            context->R0  = wow_frame->R0;
            context->R1  = wow_frame->R1;
            context->R2  = wow_frame->R2;
            context->R3  = wow_frame->R3;
            context->R4  = wow_frame->R4;
            context->R5  = wow_frame->R5;
            context->R6  = wow_frame->R6;
            context->R7  = wow_frame->R7;
            context->R8  = wow_frame->R8;
            context->R9  = wow_frame->R9;
            context->R10 = wow_frame->R10;
            context->R11 = wow_frame->R11;
            context->R12 = wow_frame->R12;
            context->ContextFlags |= CONTEXT_INTEGER;
        }
        if (needed_flags & CONTEXT_CONTROL)
        {
            context->Sp   = wow_frame->Sp;
            context->Lr   = wow_frame->Lr;
            context->Pc   = wow_frame->Pc;
            context->Cpsr = wow_frame->Cpsr;
            context->ContextFlags |= CONTEXT_CONTROL;
        }
        if (needed_flags & CONTEXT_FLOATING_POINT)
        {
            context->Fpscr = wow_frame->Fpscr;
            memcpy( context->D, wow_frame->D, sizeof(wow_frame->D) );
            context->ContextFlags |= CONTEXT_FLOATING_POINT;
        }
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           setup_raise_exception
 */
static void setup_raise_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct exc_stack_layout *stack;
    void *stack_ptr = (void *)(SP_sig(sigcontext) & ~15);
    NTSTATUS status;

    status = send_debug_event( rec, context, TRUE, TRUE );
    if (status == DBG_CONTINUE || status == DBG_EXCEPTION_HANDLED)
    {
        restore_context( context, sigcontext );
        return;
    }

    /* fix up instruction pointer in context for EXCEPTION_BREAKPOINT */
    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT) context->Pc -= 4;

    stack = virtual_setup_exception( stack_ptr, sizeof(*stack), rec );
    stack->rec = *rec;
    stack->context = *context;
    context_init_empty_xstate( &stack->context, stack->redzone );
    stack->sp = stack->context.Sp;
    stack->pc = stack->context.Pc;

    SP_sig(sigcontext) = (ULONG_PTR)stack;
    PC_sig(sigcontext) = (ULONG_PTR)pKiUserExceptionDispatcher;
    REGn_sig(18, sigcontext) = (ULONG_PTR)NtCurrentTeb();
}


/***********************************************************************
 *           setup_exception
 *
 * Modify the signal context to call the exception raise function.
 */
static void setup_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec )
{
    CONTEXT context;

    rec->ExceptionAddress = (void *)PC_sig(sigcontext);
    save_context( &context, sigcontext );
    setup_raise_exception( sigcontext, rec, &context );
}


/***********************************************************************
 *           call_user_apc_dispatcher
 */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                   PNTAPCFUNC func, NTSTATUS status )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = context ? context->Sp : frame->sp;
    struct apc_stack_layout *stack;

    if (flags) FIXME( "flags %#x are not supported.\n", flags );

    sp &= ~15;
    stack = (struct apc_stack_layout *)sp - 1;
    if (context)
    {
        memmove( &stack->context, context, sizeof(stack->context) );
        NtSetContextThread( GetCurrentThread(), &stack->context );
    }
    else
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.X0 = status;
    }
    stack->func      = func;
    stack->args[0]   = arg1;
    stack->args[1]   = arg2;
    stack->args[2]   = arg3;
    stack->alertable = TRUE;

    frame->sp = (ULONG64)stack;
    frame->pc = (ULONG64)pKiUserApcDispatcher;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher(void)
{
    get_syscall_frame()->pc = (UINT64)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    struct exc_stack_layout *stack;
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (status) return status;
    stack = (struct exc_stack_layout *)(context->Sp & ~15) - 1;
    memmove( &stack->context, context, sizeof(*context) );
    memmove( &stack->rec, rec, sizeof(*rec) );
    context_init_empty_xstate( &stack->context, stack->redzone );
    stack->sp = stack->context.Sp;
    stack->pc = stack->context.Pc;

    frame->pc = (ULONG64)pKiUserExceptionDispatcher;
    frame->sp = (ULONG64)stack;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 *           call_user_mode_callback
 */
extern NTSTATUS call_user_mode_callback( ULONG64 user_sp, void **ret_ptr, ULONG *ret_len,
                                         void *func, TEB *teb );
__ASM_GLOBAL_FUNC( call_user_mode_callback,
                   "stp x29, x30, [sp,#-0xd0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xd0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xd0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xc8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "stp d8,  d9,  [x29, #0x60]\n\t"
                   "stp d10, d11, [x29, #0x70]\n\t"
                   "stp d12, d13, [x29, #0x80]\n\t"
                   "stp d14, d15, [x29, #0x90]\n\t"
                   "stp x1, x2, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "mov x18, x4\n\t"              /* teb */
                   "mrs x1, fpcr\n\t"
                   "mrs x2, fpsr\n\t"
                   "bfi x1, x2, #0, #32\n\t"
                   "ldr x2, [x18]\n\t"            /* teb->Tib.ExceptionList */
                   "stp x1, x2, [x29, #0xb0]\n\t"

                   "ldr x7, [x18, #0x378]\n\t"    /* thread_data->syscall_frame */
                   "sub x1, sp, #0x330\n\t"       /* sizeof(struct syscall_frame) */
                   "str x1, [x18, #0x378]\n\t"    /* thread_data->syscall_frame */
                   "add x8, x29, #0xd0\n\t"
                   "stp x7, x8, [x1, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   "ldr w11, [x18, #0x380]\n\t"   /* thread_data->syscall_trace */
                   "cbnz x11, 1f\n\t"
                   /* switch to user stack */
                   "mov sp, x0\n\t"               /* user_sp */
                   "br x3\n"
                   "1:\tmov x19, x18\n\t"         /* teb */
                   "mov x20, x0\n\t"              /* user_sp */
                   "mov x21, x3\n\t"              /* func */
                   "mov sp, x1\n\t"
                   "ldr x1, [x20]\n\t"            /* args */
                   "ldp w2, w0, [x20, #8]\n\t"    /* len, id */
                   "str x0, [x29, #0xc0]\n\t"     /* id */
                   "bl " __ASM_NAME("trace_usercall") "\n\t"
                   "mov x18, x19\n\t"             /* teb */
                   "mov sp, x20\n\t"              /* user_sp */
                   "br x21" )


/***********************************************************************
 *           user_mode_callback_return
 */
extern void DECLSPEC_NORETURN user_mode_callback_return( void *ret_ptr, ULONG ret_len,
                                                         NTSTATUS status, TEB *teb );
__ASM_GLOBAL_FUNC( user_mode_callback_return,
                   "ldr x4, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "ldp x5, x29, [x4,#0x110]\n\t" /* prev_frame,syscall_cfa */
                   "str x5, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "sub x29, x29, #0xd0\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   __ASM_CFI(".cfi_rel_offset 29,0x00\n\t")
                   __ASM_CFI(".cfi_rel_offset 30,0x08\n\t")
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "ldp x5, x6, [x29, #0xb0]\n\t"
                   "str x6, [x3]\n\t"             /* teb->Tib.ExceptionList */
                   "msr fpcr, x5\n\t"
                   "lsr x5, x5, #32\n\t"
                   "msr fpsr, x5\n\t"
                   "ldp x5, x6, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "str x0, [x5]\n\t"             /* ret_ptr */
                   "str w1, [x6]\n\t"             /* ret_len */
                   "ldr w11, [x3, #0x380]\n\t"    /* thread_data->syscall_trace */
                   "cbz x11, 1f\n\t"
                   "ldr w3, [x29, #0xc0]\n\t"     /* id */
                   "mov x19, x2\n\t"
                   "bl " __ASM_NAME("trace_userret") "\n\t"
                   "mov x2, x19\n"                /* status */
                   "1:\tldp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_same_value 19\n\t")
                   __ASM_CFI(".cfi_same_value 20\n\t")
                   "ldp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_same_value 21\n\t")
                   __ASM_CFI(".cfi_same_value 22\n\t")
                   "ldp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_same_value 23\n\t")
                   __ASM_CFI(".cfi_same_value 24\n\t")
                   "ldp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_same_value 25\n\t")
                   __ASM_CFI(".cfi_same_value 26\n\t")
                   "ldp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_same_value 27\n\t")
                   __ASM_CFI(".cfi_same_value 28\n\t")
                   "ldp d8,  d9,  [x29, #0x60]\n\t"
                   "ldp d10, d11, [x29, #0x70]\n\t"
                   "ldp d12, d13, [x29, #0x80]\n\t"
                   "ldp d14, d15, [x29, #0x90]\n\t"
                   "mov x0, x2\n\t"               /* status */
                   "mov sp, x29\n\t"
                   "ldp x29, x30, [sp], #0xd0\n\t"
                   "ret" )


/***********************************************************************
 *           user_mode_abort_thread
 */
extern void DECLSPEC_NORETURN user_mode_abort_thread( NTSTATUS status, struct syscall_frame *frame );
__ASM_GLOBAL_FUNC( user_mode_abort_thread,
                   "ldr x1, [x1, #0x118]\n\t"    /* frame->syscall_cfa */
                   "sub x29, x1, #0xc0\n\t"
                   /* switch to kernel stack */
                   "mov sp, x29\n\t"
                   __ASM_CFI(".cfi_def_cfa 29,0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19,-0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20,-0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21,-0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22,-0x98\n\t")
                   __ASM_CFI(".cfi_offset 23,-0x90\n\t")
                   __ASM_CFI(".cfi_offset 24,-0x88\n\t")
                   __ASM_CFI(".cfi_offset 25,-0x80\n\t")
                   __ASM_CFI(".cfi_offset 26,-0x78\n\t")
                   __ASM_CFI(".cfi_offset 27,-0x70\n\t")
                   __ASM_CFI(".cfi_offset 28,-0x68\n\t")
                   "bl " __ASM_NAME("abort_thread") )


/***********************************************************************
 *           KeUserModeCallback
 */
NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = (frame->sp - offsetof( struct callback_stack_layout, args_data[len] ) - 16) & ~15;
    struct callback_stack_layout *stack = (struct callback_stack_layout *)sp;

#ifndef __SWITCH__
    /* on the Switch secondary threads run on heap-allocated pthread stacks,
     * not kernel_stack, so this comparison would be meaningless there */
    if ((char *)ntdll_get_thread_data()->kernel_stack + min_kernel_stack > (char *)&frame)
        return STATUS_STACK_OVERFLOW;
#endif

    stack->args = stack->args_data;
    stack->len  = len;
    stack->id   = id;
    stack->lr   = frame->lr;
    stack->sp   = frame->sp;
    stack->pc   = frame->pc;
    memcpy( stack->args_data, args, len );
    return call_user_mode_callback( sp, ret_ptr, ret_len, pKiUserCallbackDispatcher, NtCurrentTeb() );
}


/***********************************************************************
 *           NtCallbackReturn  (NTDLL.@)
 */
NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    if (!get_syscall_frame()->prev_frame) return STATUS_NO_CALLBACK_ACTIVE;
    user_mode_callback_return( ret_ptr, ret_len, status, NtCurrentTeb() );
}


/***********************************************************************
 *           handle_syscall_fault
 *
 * Handle a page fault happening during a system call.
 */
static BOOL handle_syscall_fault( ucontext_t *context, EXCEPTION_RECORD *rec )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD i;

    if (!is_inside_syscall( SP_sig(context) )) return FALSE;

    TRACE( "code=%x flags=%x addr=%p pc=%p tid=%04x\n",
           rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress,
           (void *)PC_sig(context), GetCurrentThreadId() );
    for (i = 0; i < rec->NumberParameters; i++)
        TRACE( " info[%d]=%016lx\n", i, rec->ExceptionInformation[i] );

    TRACE("  x0=%016lx  x1=%016lx  x2=%016lx  x3=%016lx\n",
          (DWORD64)REGn_sig(0, context), (DWORD64)REGn_sig(1, context),
          (DWORD64)REGn_sig(2, context), (DWORD64)REGn_sig(3, context) );
    TRACE("  x4=%016lx  x5=%016lx  x6=%016lx  x7=%016lx\n",
          (DWORD64)REGn_sig(4, context), (DWORD64)REGn_sig(5, context),
          (DWORD64)REGn_sig(6, context), (DWORD64)REGn_sig(7, context) );
    TRACE("  x8=%016lx  x9=%016lx x10=%016lx x11=%016lx\n",
          (DWORD64)REGn_sig(8, context), (DWORD64)REGn_sig(9, context),
          (DWORD64)REGn_sig(10, context), (DWORD64)REGn_sig(11, context) );
    TRACE(" x12=%016lx x13=%016lx x14=%016lx x15=%016lx\n",
          (DWORD64)REGn_sig(12, context), (DWORD64)REGn_sig(13, context),
          (DWORD64)REGn_sig(14, context), (DWORD64)REGn_sig(15, context) );
    TRACE(" x16=%016lx x17=%016lx x18=%016lx x19=%016lx\n",
          (DWORD64)REGn_sig(16, context), (DWORD64)REGn_sig(17, context),
          (DWORD64)REGn_sig(18, context), (DWORD64)REGn_sig(19, context) );
    TRACE(" x20=%016lx x21=%016lx x22=%016lx x23=%016lx\n",
          (DWORD64)REGn_sig(20, context), (DWORD64)REGn_sig(21, context),
          (DWORD64)REGn_sig(22, context), (DWORD64)REGn_sig(23, context) );
    TRACE(" x24=%016lx x25=%016lx x26=%016lx x27=%016lx\n",
          (DWORD64)REGn_sig(24, context), (DWORD64)REGn_sig(25, context),
          (DWORD64)REGn_sig(26, context), (DWORD64)REGn_sig(27, context) );
    TRACE(" x28=%016lx  fp=%016lx  lr=%016lx  sp=%016lx\n",
          (DWORD64)REGn_sig(28, context), (DWORD64)FP_sig(context),
          (DWORD64)LR_sig(context), (DWORD64)SP_sig(context) );

    if (rec->ExceptionCode == STATUS_ACCESS_VIOLATION
            && is_inside_syscall_stack_guard( (char *)rec->ExceptionInformation[1] ))
        ERR_(seh)( "Syscall stack overrun.\n ");

    if (ntdll_get_thread_data()->jmp_buf)
    {
        TRACE( "returning to handler\n" );
        REGn_sig(0, context) = (ULONG_PTR)ntdll_get_thread_data()->jmp_buf;
        REGn_sig(1, context) = 1;
        PC_sig(context)      = (ULONG_PTR)longjmp;
        ntdll_get_thread_data()->jmp_buf = NULL;
    }
    else
    {
        TRACE( "returning to user mode ip=%p ret=%08x\n", (void *)frame->pc, rec->ExceptionCode );
        REGn_sig(0, context)  = rec->ExceptionCode;
        REGn_sig(18, context) = (ULONG_PTR)NtCurrentTeb();
        SP_sig(context)       = (ULONG_PTR)frame;
        PC_sig(context)       = (ULONG_PTR)__wine_syscall_dispatcher_return;
    }
    return TRUE;
}


/**********************************************************************
 *		segv_handler
 *
 * Handler for SIGSEGV.
 */
static void segv_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    DWORD64 esr = get_fault_esr( context );

    rec.NumberParameters = 2;
    if ((esr & 0xf0000000) == 0x80000000) rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
    else if (esr & 0x40) rec.ExceptionInformation[0] = EXCEPTION_WRITE_FAULT;
    else rec.ExceptionInformation[0] = EXCEPTION_READ_FAULT;
    rec.ExceptionInformation[1] = (ULONG_PTR)siginfo->si_addr;
    if (!virtual_handle_fault( &rec, (void *)SP_sig(context) )) return;
    if (handle_syscall_fault( context, &rec )) return;
    setup_exception( context, &rec );
}


/**********************************************************************
 *		ill_handler
 *
 * Handler for SIGILL.
 */
static void ill_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_ILLEGAL_INSTRUCTION };
    ucontext_t *context = sigcontext;

    if (!(PSTATE_sig( context ) & 0x10) && /* AArch64 (not WoW) */
        !(PC_sig( context ) & 3))
    {
        ULONG instr = *(ULONG *)PC_sig( context );
        /* emulate mrs xN, CurrentEL */
        if ((instr & ~0x1f) == 0xd5384240) {
            ULONG reg = instr & 0x1f;
            /* ignore writes to xzr */
            if (reg != 31) REGn_sig(reg, context) = 0;
            PC_sig(context) += 4;
            return;
        }
    }

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		bus_handler
 *
 * Handler for SIGBUS.
 */
static void bus_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_DATATYPE_MISALIGNMENT };

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		trap_handler
 *
 * Handler for SIGTRAP.
 */
static void trap_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    CONTEXT ctx;

    rec.ExceptionAddress = (void *)PC_sig(context);
    save_context( &ctx, sigcontext );

    switch (siginfo->si_code)
    {
    case TRAP_TRACE:
        rec.ExceptionCode = EXCEPTION_SINGLE_STEP;
        break;
    case TRAP_BRKPT:
        /* debug exceptions do not update ESR on Linux, so we fetch the instruction directly. */
        if (!(PSTATE_sig( context ) & 0x10) && /* AArch64 (not WoW) */
            !(PC_sig( context ) & 3))
        {
            ULONG imm = (*(ULONG *)PC_sig( context ) >> 5) & 0xffff;
            switch (imm)
            {
            case 0xf000:
                ctx.Pc += 4;  /* skip the brk instruction */
                rec.ExceptionCode = EXCEPTION_BREAKPOINT;
                rec.NumberParameters = 1;
                break;
            case 0xf001:
                rec.ExceptionCode = STATUS_ASSERTION_FAILURE;
                break;
            case 0xf003:
                rec.ExceptionCode = STATUS_STACK_BUFFER_OVERRUN;
                rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                rec.NumberParameters = 1;
                rec.ExceptionInformation[0] = ctx.X[0];
                NtRaiseException( &rec, &ctx, FALSE );
                break;
            case 0xf004:
                rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
                break;
            default:
                rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
                break;
            }
        }
        break;
    default:
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    }

    setup_raise_exception( sigcontext, &rec, &ctx );
}

/**********************************************************************
 *		fpe_handler
 *
 * Handler for SIGFPE.
 */
static void fpe_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };

    switch (siginfo->si_code & 0xffff )
    {
#ifdef FPE_FLTSUB
    case FPE_FLTSUB:
        rec.ExceptionCode = EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        break;
#endif
#ifdef FPE_INTDIV
    case FPE_INTDIV:
        rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_INTOVF
    case FPE_INTOVF:
        rec.ExceptionCode = EXCEPTION_INT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTDIV
    case FPE_FLTDIV:
        rec.ExceptionCode = EXCEPTION_FLT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_FLTOVF
    case FPE_FLTOVF:
        rec.ExceptionCode = EXCEPTION_FLT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTUND
    case FPE_FLTUND:
        rec.ExceptionCode = EXCEPTION_FLT_UNDERFLOW;
        break;
#endif
#ifdef FPE_FLTRES
    case FPE_FLTRES:
        rec.ExceptionCode = EXCEPTION_FLT_INEXACT_RESULT;
        break;
#endif
#ifdef FPE_FLTINV
    case FPE_FLTINV:
#endif
    default:
        rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
        break;
    }
    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		int_handler
 *
 * Handler for SIGINT.
 */
static void int_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    HANDLE handle;

    if (!p__wine_ctrl_routine) return;
    if (!NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                           p__wine_ctrl_routine, 0 /* CTRL_C_EVENT */, 0, 0, 0, 0, NULL ))
        NtClose( handle );
}


/**********************************************************************
 *		abrt_handler
 *
 * Handler for SIGABRT.
 */
static void abrt_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_WINE_ASSERTION, EXCEPTION_NONCONTINUABLE };

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		quit_handler
 *
 * Handler for SIGQUIT.
 */
static void quit_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    ucontext_t *context = sigcontext;

    if (!is_inside_syscall( SP_sig(context) )) user_mode_abort_thread( 0, get_syscall_frame() );
    abort_thread(0);
}


/**********************************************************************
 *		usr1_handler
 *
 * Handler for SIGUSR1, used to signal a thread that it got suspended.
 */
static void usr1_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    ucontext_t *ucontext = sigcontext;
    CONTEXT context;

    if (is_arm64ec_suspend_doorbell_valid() &&
        (NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation || NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback))
    {
        *NtCurrentTeb()->ChpeV2CpuAreaInfo->SuspendDoorbell = 1;
        arm64_thread_data()->suspend_pending = TRUE;
        return;
    }

    if (is_inside_syscall( SP_sig(ucontext) ))
    {
        context.ContextFlags = CONTEXT_FULL | CONTEXT_EXCEPTION_REQUEST;
        NtGetContextThread( GetCurrentThread(), &context );
        wait_suspend( &context );
        NtSetContextThread( GetCurrentThread(), &context );
    }
    else
    {
        save_context( &context, ucontext );
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING;
        wait_suspend( &context );
        restore_context( &context, ucontext );
    }
}


/**********************************************************************
 *		usr2_handler
 *
 * Handler for SIGUSR2, used to set a thread context.
 */
static void usr2_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    struct syscall_frame *frame = get_syscall_frame();
    ucontext_t *context = sigcontext;
    DWORD i;

    if (!is_inside_syscall( SP_sig(context) )) return;

    FP_sig(context)     = frame->fp;
    LR_sig(context)     = frame->lr;
    SP_sig(context)     = frame->sp;
    PC_sig(context)     = frame->pc;
    PSTATE_sig(context) = frame->cpsr;
    for (i = 0; i <= 28; i++) REGn_sig( i, context ) = frame->x[i];

#ifdef linux
    {
        struct fpsimd_context *fp = get_fpsimd_context( sigcontext );
        struct sve_context *sve = get_sve_context( sigcontext );
        if (fp)
        {
            fp->fpcr = frame->fpcr;
            fp->fpsr = frame->fpsr;
            memcpy( fp->vregs, frame->v, sizeof(fp->vregs) );
        }

        if (sve)
        {
            /* setup FEX SVE state */
            ULONG64 vq = sve_vq_from_vl(sve->vl);
            *(UINT16 *)((BYTE *)sve + SVE_SIG_PREG_OFFSET(vq, 2)) = 0x155;
            *(UINT16 *)((BYTE *)sve + SVE_SIG_PREG_OFFSET(vq, 6)) = 0xffff;
        }
    }
#elif defined(__APPLE__)
    context->uc_mcontext->__ns.__fpcr = frame->fpcr;
    context->uc_mcontext->__ns.__fpsr = frame->fpsr;
    memcpy( context->uc_mcontext->__ns.__v, frame->v, sizeof(frame->v) );
#endif
}


/**********************************************************************
 *           get_thread_ldt_entry
 */
NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *             signal_init_threading
 */
void signal_init_threading(void)
{
}


/**********************************************************************
 *             signal_alloc_thread
 */
NTSTATUS signal_alloc_thread( TEB *teb )
{
    return STATUS_SUCCESS;
}


/**********************************************************************
 *             signal_free_thread
 */
void signal_free_thread( TEB *teb )
{
}


/**********************************************************************
 *		signal_init_process
 */
void signal_init_process(void)
{
    struct sigaction sig_act;
    struct ntdll_thread_data *thread_data = ntdll_get_thread_data();
    void *kernel_stack = (char *)thread_data->kernel_stack + kernel_stack_size;

    thread_data->syscall_frame = (struct syscall_frame *)kernel_stack - 1;

    signal_alloc_thread( NtCurrentTeb() );

    sig_act.sa_mask = server_block_set;
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;

    sig_act.sa_sigaction = int_handler;
    if (sigaction( SIGINT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = fpe_handler;
    if (sigaction( SIGFPE, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = abrt_handler;
    if (sigaction( SIGABRT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = quit_handler;
    if (sigaction( SIGQUIT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = usr1_handler;
    if (sigaction( SIGUSR1, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = usr2_handler;
    if (sigaction( SIGUSR2, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = trap_handler;
    if (sigaction( SIGTRAP, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = segv_handler;
    if (sigaction( SIGSEGV, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = ill_handler;
    if (sigaction( SIGILL, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = bus_handler;
    if (sigaction( SIGBUS, &sig_act, NULL ) == -1) goto error;
    return;

 error:
    perror("sigaction");
    exit(1);
}


/***********************************************************************
 *           syscall_dispatcher_return_slowpath
 */
void syscall_dispatcher_return_slowpath(void)
{
    raise( SIGUSR2 );
}

/***********************************************************************
 *           init_syscall_frame
 */
void init_syscall_frame( LPTHREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    struct syscall_frame *frame = thread_data->syscall_frame;
    CONTEXT *ctx, context = { CONTEXT_ALL };
    I386_CONTEXT *i386_context;
    ARM_CONTEXT *arm_context;

    ((struct arm64_thread_data *)(thread_data->cpu_data))->suspend_pending = FALSE;

    context.X0  = (DWORD64)entry;
    context.X1  = (DWORD64)arg;
    context.X18 = (DWORD64)teb;
    context.Sp  = (DWORD64)teb->Tib.StackBase;
    context.Pc  = (DWORD64)pRtlUserThreadStart;

    if ((i386_context = get_cpu_area( IMAGE_FILE_MACHINE_I386 )))
    {
        XMM_SAVE_AREA32 *fpu = (XMM_SAVE_AREA32 *)i386_context->ExtendedRegisters;
        i386_context->ContextFlags = CONTEXT_I386_ALL;
        i386_context->Eax = (ULONG_PTR)entry;
        i386_context->Ebx = (arg == peb ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        i386_context->Esp = get_wow_teb( teb )->Tib.StackBase - 16;
        i386_context->Eip = pLdrSystemDllInitBlock->pRtlUserThreadStart;
        i386_context->SegCs = 0x23;
        i386_context->SegDs = 0x2b;
        i386_context->SegEs = 0x2b;
        i386_context->SegFs = 0x53;
        i386_context->SegGs = 0x2b;
        i386_context->SegSs = 0x2b;
        i386_context->EFlags = 0x202;
        fpu->ControlWord = 0x27f;
        fpu->MxCsr = 0x1f80;
        fpux_to_fpu( &i386_context->FloatSave, fpu );
    }
    else if ((arm_context = get_cpu_area( IMAGE_FILE_MACHINE_ARMNT )))
    {
        arm_context->ContextFlags = CONTEXT_ARM_ALL;
        arm_context->R0 = (ULONG_PTR)entry;
        arm_context->R1 = (arg == peb ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        arm_context->Sp = get_wow_teb( teb )->Tib.StackBase;
        arm_context->Pc = pLdrSystemDllInitBlock->pRtlUserThreadStart;
        if (arm_context->Pc & 1) arm_context->Cpsr |= 0x20; /* thumb mode */
    }

    if (suspend)
    {
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
        wait_suspend( &context );
    }

    ctx = (CONTEXT *)((ULONG_PTR)context.Sp & ~15) - 1;
    *ctx = context;
    ctx->ContextFlags = CONTEXT_FULL;
    signal_set_full_context( ctx );

    frame->sp    = (ULONG64)ctx;
    frame->pc    = (ULONG64)pLdrInitializeThunk;
    frame->x[0]  = (ULONG64)ctx;
    frame->x[18] = (ULONG64)teb;
    syscall_frame_fixup_for_fastpath( frame );

    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
}


/***********************************************************************
 *           signal_start_thread
 */
__ASM_GLOBAL_FUNC( signal_start_thread,
                   "stp x29, x30, [sp,#-0xc0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "add x5, x29, #0xc0\n\t"     /* syscall_cfa */
                   /* set syscall frame */
                   "ldr x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "cbnz x4, 1f\n\t"
                   "sub x4, sp, #0x330\n\t"     /* sizeof(struct syscall_frame) */
                   "str x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "1:\tstr wzr, [x4, #0x10c]\n\t" /* frame->restore_flags */
                   "stp xzr, x5, [x4, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   /* switch to kernel stack */
                   "mov sp, x4\n\t"
                   "bl " __ASM_NAME("init_syscall_frame") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/***********************************************************************
 *           __wine_syscall_dispatcher
 */
__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,
                   "hint 34\n\t" /* bti c */
                   "ldr x10, [x18, #0x378]\n\t" /* thread_data->syscall_frame */
                   "stp x18, x19, [x10, #0x90]\n\t"
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "mov x19, sp\n\t"
                   "stp x9, x19, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "str w8, [x10, #0x120]\n\t"
                   "mrs x9, FPCR\n\t"
                   "str w9, [x10, #0x128]\n\t"
                   "mrs x9, FPSR\n\t"
                   "str w9, [x10, #0x12c]\n\t"
                   "stp q0,  q1,  [x10, #0x130]\n\t"
                   "stp q2,  q3,  [x10, #0x150]\n\t"
                   "stp q4,  q5,  [x10, #0x170]\n\t"
                   "stp q6,  q7,  [x10, #0x190]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "stp q16, q17, [x10, #0x230]\n\t"
                   "stp q18, q19, [x10, #0x250]\n\t"
                   "stp q20, q21, [x10, #0x270]\n\t"
                   "stp q22, q23, [x10, #0x290]\n\t"
                   "stp q24, q25, [x10, #0x2b0]\n\t"
                   "stp q26, q27, [x10, #0x2d0]\n\t"
                   "stp q28, q29, [x10, #0x2f0]\n\t"
                   "stp q30, q31, [x10, #0x310]\n\t"
                   "mov x22, x10\n\t"
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x22, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   "and x20, x8, #0xfff\n\t"    /* syscall number */
                   "ubfx x21, x8, #12, #2\n\t"  /* syscall table number */
                   "ldr x16, [x18, #0x370]\n\t" /* thread_data->syscall_table */
                   "add x21, x16, x21, lsl #5\n\t"
                   "ldr x16, [x21, #16]\n\t"    /* table->ServiceLimit */
                   "cmp x20, x16\n\t"
                   "bcs " __ASM_LOCAL_LABEL("bad_syscall") "\n\t"
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w9, [x16, x20]\n\t"
                   "subs x9, x9, #64\n\t"
                   "bls 2f\n\t"
                   "sub sp, sp, x9\n\t"
                   "tbz x9, #3, 1f\n\t"
                   "sub sp, sp, #8\n"
                   "1:\tsub x9, x9, #8\n\t"
                   "ldr x10, [x19, x9]\n\t"
                   "str x10, [sp, x9]\n\t"
                   "cbnz x9, 1b\n"
                   "2:\tldr x16, [x21]\n\t"     /* table->ServiceTable */
                   "ldr x23, [x16, x20, lsl 3]\n\t"
                   "ldr w11, [x18, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall") "\n\t"
                   "blr x23\n\t"
                   "mov sp, x22\n"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") ":\n\t"
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "tbz x16, #1, 2f\n\t"        /* CONTEXT_INTEGER */
                   "ldp x12, x13, [sp, #0x80]\n\t" /* frame->x[16..17] */
                   "ldp x14, x15, [sp, #0xf8]\n\t" /* frame->sp, frame->pc */
                   "cmp x12, x15\n\t"              /* frame->x16 == frame->pc? */
                   "ccmp x13, x14, #0, eq\n\t"     /* frame->x17 == frame->sp? */
                   "beq 1f\n\t"                    /* take slowpath if unequal */
                   "bl " __ASM_NAME("syscall_dispatcher_return_slowpath") "\n"
                   "1:\tldp x0, x1, [sp, #0x00]\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x8, x9, [sp, #0x40]\n\t"
                   "ldp x10, x11, [sp, #0x50]\n\t"
                   "ldp x12, x13, [sp, #0x60]\n\t"
                   "ldp x14, x15, [sp, #0x70]\n"
                   "2:\tldp x18, x19, [sp, #0x90]\n\t"
                   "ldp x20, x21, [sp, #0xa0]\n\t"
                   "ldp x22, x23, [sp, #0xb0]\n\t"
                   "ldp x24, x25, [sp, #0xc0]\n\t"
                   "ldp x26, x27, [sp, #0xd0]\n\t"
                   "ldp x28, x29, [sp, #0xe0]\n\t"
                   "tbz x16, #2, 1f\n\t"        /* CONTEXT_FLOATING_POINT */
                   "ldp q0,  q1,  [sp, #0x130]\n\t"
                   "ldp q2,  q3,  [sp, #0x150]\n\t"
                   "ldp q4,  q5,  [sp, #0x170]\n\t"
                   "ldp q6,  q7,  [sp, #0x190]\n\t"
                   "ldp q8,  q9,  [sp, #0x1b0]\n\t"
                   "ldp q10, q11, [sp, #0x1d0]\n\t"
                   "ldp q12, q13, [sp, #0x1f0]\n\t"
                   "ldp q14, q15, [sp, #0x210]\n\t"
                   "ldp q16, q17, [sp, #0x230]\n\t"
                   "ldp q18, q19, [sp, #0x250]\n\t"
                   "ldp q20, q21, [sp, #0x270]\n\t"
                   "ldp q22, q23, [sp, #0x290]\n\t"
                   "ldp q24, q25, [sp, #0x2b0]\n\t"
                   "ldp q26, q27, [sp, #0x2d0]\n\t"
                   "ldp q28, q29, [sp, #0x2f0]\n\t"
                   "ldp q30, q31, [sp, #0x310]\n\t"
                   "ldr w17, [sp, #0x128]\n\t"
                   "msr FPCR, x17\n\t"
                   "ldr w17, [sp, #0x12c]\n\t"
                   "msr FPSR, x17\n"
                   "1:\tldp x16, x17, [sp, #0x100]\n\t"
                   "msr NZCV, x17\n\t"
                   "ldp x30, x17, [sp, #0xf0]\n\t"
                   /* switch to user stack */
                   "mov sp, x17\n\t"
                   "ret x16\n"

                   __ASM_LOCAL_LABEL("trace_syscall") ":\n\t"
                   "stp x0, x1, [sp, #-0x40]!\n\t"
                   "stp x2, x3, [sp, #0x10]\n\t"
                   "stp x4, x5, [sp, #0x20]\n\t"
                   "stp x6, x7, [sp, #0x30]\n\t"
                   "mov x0, x8\n\t"             /* id */
                   "mov x1, sp\n\t"             /* args */
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w2, [x16, x20]\n\t"    /* len */
                   "bl " __ASM_NAME("trace_syscall") "\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x0, x1, [sp], #0x40\n\t"
                   "blr x23\n"
                   "mov sp, x22\n"

                   __ASM_LOCAL_LABEL("trace_syscall_ret") ":\n\t"
                   "mov x21, x0\n\t"            /* retval */
                   "ldr w0, [sp, #0x120]\n\t"   /* frame->syscall_id */
                   "mov x1, x21\n\t"            /* retval */
                   "bl " __ASM_NAME("trace_sysret") "\n\t"
                   "mov x0, x21\n\t"            /* retval */
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n"

                   __ASM_LOCAL_LABEL("bad_syscall") ":\n\t"
                   "mov x0, #0xc0000000\n\t"    /* STATUS_INVALID_SYSTEM_SERVICE */
                   "movk x0, #0x001c\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )

__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher_return,
                   "ldr w11, [x18, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall_ret") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/***********************************************************************
 *           __wine_unix_call_dispatcher
 */
__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher,
                   "hint 34\n\t" /* bti c */
                   "ldr x10, [x18, #0x378]\n\t" /* thread_data->syscall_frame */
                   "stp x18, x19, [x10, #0x90]\n\t"
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "mov x9, sp\n\t"
                   "stp x30, x9, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "mov x19, x10\n\t"
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x19, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   "ldr x16, [x0, x1, lsl 3]\n\t"
                   "mov x0, x2\n\t"             /* args */
                   "blr x16\n\t"
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "cbnz w16, " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n\t"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   "ldp x18, x19, [sp, #0x90]\n\t"
                   "ldp x16, x17, [sp, #0xf8]\n\t"
                   /* switch to user stack */
                   "mov sp, x16\n\t"
                   "ret x17" )

#endif /* __SWITCH__ */

#endif  /* __aarch64__ */
