/*
 * ntdll Horizon host support
 *
 * Copyright 2026 Diogo Silva
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __SWITCH__

#ifdef HORIZON_STANDALONE_SYNTAX
#include "horizon_syntax_shim.h"
#else

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/rbtree.h"
#include "unix_private.h"
#include "horizon_mman.h"
#include "horizon_private.h"

#include <switch.h>

#endif /* HORIZON_STANDALONE_SYNTAX */

#include "horizon_clipboard.h"
#include "horizon_file_access.h"
#include "horizon_message_queue.h"
#include "horizon_msg_queue.h"
#include "horizon_win_timers.h"
#include "horizon_threads.h"
#include "horizon_registry.h"
#include "horizon_read_redirect.h"
#include "horizon_object_dirs.h"
#include "horizon_keyboard.h"
#include "horizon_mouse.h"
#include "horizon_free_range.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/iosupport.h>

/* PE entry points and callbacks currently execute on the libnx host stack.
 * Keep the native TIB truthful; the WoW64 guest stack and allocation ownership
 * remain in their original fields. */
void horizon_bind_native_stack( TEB *teb )
{
    Thread *thread = threadGetSelf();
    ULONG_PTR current = (ULONG_PTR)&thread;
    ULONG_PTR low, high;

    if (!teb || !thread) return;
    low = (ULONG_PTR)thread->stack_mirror;
    high = low + thread->stack_sz;
    if (!low || high <= low || current < low || current >= high) return;
    if ((ULONG_PTR)teb->Tib.StackLimit == low && (ULONG_PTR)teb->Tib.StackBase == high) return;
    horizon_trace( "[NXSTACK] native TEB=%p stack=%p-%p -> %p-%p current=%p\n",
                   teb, teb->Tib.StackLimit, teb->Tib.StackBase,
                   (void *)low, (void *)high, (void *)current );
    teb->Tib.StackLimit = (void *)low;
    teb->Tib.StackBase = (void *)high;
}

WINE_DEFAULT_DEBUG_CHANNEL(horizon);

#ifndef SERVER_PROTOCOL_VERSION
#define SERVER_PROTOCOL_VERSION 931
#endif

#if defined(__aarch64__) && !defined(HORIZON_NO_LIBNX_EXCEPTION_HANDLER)
typedef char horizon_exception_dump_cpu_gprs_offset[
    offsetof(ThreadExceptionDump, cpu_gprs) == 16 ? 1 : -1];
typedef char horizon_exception_dump_fp_offset[
    offsetof(ThreadExceptionDump, fp) == 248 ? 1 : -1];
typedef char horizon_exception_dump_pc_offset[
    offsetof(ThreadExceptionDump, pc) == 272 ? 1 : -1];
typedef char horizon_exception_dump_fpu_gprs_offset[
    offsetof(ThreadExceptionDump, fpu_gprs) == 288 ? 1 : -1];
typedef char horizon_exception_dump_pstate_offset[
    offsetof(ThreadExceptionDump, pstate) == 800 ? 1 : -1];

static void horizon_restore_exception_context( ThreadExceptionDump *ctx ) __attribute__((noreturn));

static void horizon_restore_exception_context( ThreadExceptionDump *ctx )
{
    __asm__ __volatile__(
        "mov x21, %0\n"
        "ldp q0,  q1,  [x21, #288]\n"
        "ldp q2,  q3,  [x21, #320]\n"
        "ldp q4,  q5,  [x21, #352]\n"
        "ldp q6,  q7,  [x21, #384]\n"
        "ldp q8,  q9,  [x21, #416]\n"
        "ldp q10, q11, [x21, #448]\n"
        "ldp q12, q13, [x21, #480]\n"
        "ldp q14, q15, [x21, #512]\n"
        "ldp q16, q17, [x21, #544]\n"
        "ldp q18, q19, [x21, #576]\n"
        "ldp q20, q21, [x21, #608]\n"
        "ldp q22, q23, [x21, #640]\n"
        "ldp q24, q25, [x21, #672]\n"
        "ldp q26, q27, [x21, #704]\n"
        "ldp q28, q29, [x21, #736]\n"
        "ldp q30, q31, [x21, #768]\n"
        "ldr w16, [x21, #800]\n"
        "msr nzcv, x16\n"
        "ldr x16, [x21, #264]\n"
        "ldr x17, [x21, #272]\n"
        "str x17, [x16, #-16]!\n"
        "mov x17, x16\n"
        "ldr x30, [x21, #256]\n"
        "ldr x29, [x21, #248]\n"
        "ldp x0,  x1,  [x21, #16]\n"
        "ldp x2,  x3,  [x21, #32]\n"
        "ldp x4,  x5,  [x21, #48]\n"
        "ldp x6,  x7,  [x21, #64]\n"
        "ldp x8,  x9,  [x21, #80]\n"
        "ldp x10, x11, [x21, #96]\n"
        "ldp x12, x13, [x21, #112]\n"
        "ldp x14, x15, [x21, #128]\n"
        "ldr x16, [x21, #144]\n"
        "ldp x18, x19, [x21, #160]\n"
        "ldr x20, [x21, #176]\n"
        "ldp x22, x23, [x21, #192]\n"
        "ldp x24, x25, [x21, #208]\n"
        "ldp x26, x27, [x21, #224]\n"
        "ldr x28, [x21, #240]\n"
        "mov sp, x17\n"
        "ldr x21, [x21, #184]\n"
        "ldr x17, [sp], #16\n"
        "br x17\n"
        :
        : "r"(ctx)
        : "memory");

    __builtin_unreachable();
}

/* The same restore giving up x9 instead of x17, for a fault in Box64's
 * translated code. Box64 keeps the guest's ESI and EDI in x16 and x17
 * (vendor/box64/src/dynarec/arm64/arm64_mapping.h) and never uses x8 or x9, so
 * resuming there through x17 left EDI holding the fault address - build 62's
 * freeze. tico-dolphin resumes its JIT through x17 and keeps x17 out of the
 * JIT's register pool for this reason. */
static void horizon_restore_exception_context_x9( ThreadExceptionDump *ctx ) __attribute__((noreturn));

static void horizon_restore_exception_context_x9( ThreadExceptionDump *ctx )
{
    __asm__ __volatile__(
        "mov x21, %0\n"
        "ldp q0,  q1,  [x21, #288]\n"
        "ldp q2,  q3,  [x21, #320]\n"
        "ldp q4,  q5,  [x21, #352]\n"
        "ldp q6,  q7,  [x21, #384]\n"
        "ldp q8,  q9,  [x21, #416]\n"
        "ldp q10, q11, [x21, #448]\n"
        "ldp q12, q13, [x21, #480]\n"
        "ldp q14, q15, [x21, #512]\n"
        "ldp q16, q17, [x21, #544]\n"
        "ldp q18, q19, [x21, #576]\n"
        "ldp q20, q21, [x21, #608]\n"
        "ldp q22, q23, [x21, #640]\n"
        "ldp q24, q25, [x21, #672]\n"
        "ldp q26, q27, [x21, #704]\n"
        "ldp q28, q29, [x21, #736]\n"
        "ldp q30, q31, [x21, #768]\n"
        "ldr w16, [x21, #800]\n"
        "msr nzcv, x16\n"
        "ldr x16, [x21, #264]\n"
        "ldr x9, [x21, #272]\n"
        "str x9, [x16, #-16]!\n"
        "mov x9, x16\n"
        "ldr x30, [x21, #256]\n"
        "ldr x29, [x21, #248]\n"
        "ldp x0,  x1,  [x21, #16]\n"
        "ldp x2,  x3,  [x21, #32]\n"
        "ldp x4,  x5,  [x21, #48]\n"
        "ldp x6,  x7,  [x21, #64]\n"
        "ldr x8,  [x21, #80]\n"
        "ldp x10, x11, [x21, #96]\n"
        "ldp x12, x13, [x21, #112]\n"
        "ldp x14, x15, [x21, #128]\n"
        "ldp x16, x17, [x21, #144]\n"
        "ldp x18, x19, [x21, #160]\n"
        "ldr x20, [x21, #176]\n"
        "ldp x22, x23, [x21, #192]\n"
        "ldp x24, x25, [x21, #208]\n"
        "ldp x26, x27, [x21, #224]\n"
        "ldr x28, [x21, #240]\n"
        "mov sp, x9\n"
        "ldr x21, [x21, #184]\n"
        "ldr x9, [sp], #16\n"
        "br x9\n"
        :
        : "r"(ctx)
        : "memory");

    __builtin_unreachable();
}

/* Dynarec builds: whether pc lies in Box64's translated code. */
extern int wine_nx_box64_is_translated_pc( ULONG_PTR pc ) __attribute__((weak));

/* Resume a handled fault: through x9 in translated code, through x17 elsewhere,
 * where the ARM64 ABI lets veneers clobber it. */
static void horizon_resume_exception( ThreadExceptionDump *ctx ) __attribute__((noreturn));

static void horizon_resume_exception( ThreadExceptionDump *ctx )
{
    if (wine_nx_box64_is_translated_pc && wine_nx_box64_is_translated_pc( ctx->pc.x ))
        horizon_restore_exception_context_x9( ctx );
    horizon_restore_exception_context( ctx );
}

/* The other half of the restore below: a context of this thread as it is here,
 * which continuing returns to. Windows takes one on the way into an APC so the
 * routine can say where to carry on, and wow64's NtContinue hands it back when
 * the program's 32-bit routine has run. Everything the restore puts back has to
 * be here, the callee-saved q registers included, or it would come back as
 * zeroes. Returns 0 when taken and 1 when continued, as setjmp does. */
extern int horizon_capture_context( CONTEXT *context );

C_ASSERT( offsetof(CONTEXT, ContextFlags) == 0x00 );
C_ASSERT( offsetof(CONTEXT, Cpsr) == 0x04 );
C_ASSERT( offsetof(CONTEXT, X0) == 0x08 );
C_ASSERT( offsetof(CONTEXT, Fp) == 0xf0 );
C_ASSERT( offsetof(CONTEXT, Lr) == 0xf8 );
C_ASSERT( offsetof(CONTEXT, Sp) == 0x100 );
C_ASSERT( offsetof(CONTEXT, Pc) == 0x108 );
C_ASSERT( offsetof(CONTEXT, V) == 0x110 );
C_ASSERT( offsetof(CONTEXT, Fpcr) == 0x310 );
C_ASSERT( offsetof(CONTEXT, Fpsr) == 0x314 );
C_ASSERT( CONTEXT_ARM64_FULL == 0x400007 );

__ASM_GLOBAL_FUNC( horizon_capture_context,
                   "stp x0,  x1,  [x0, #0x08]\n\t"
                   "stp x2,  x3,  [x0, #0x18]\n\t"
                   "stp x4,  x5,  [x0, #0x28]\n\t"
                   "stp x6,  x7,  [x0, #0x38]\n\t"
                   "stp x8,  x9,  [x0, #0x48]\n\t"
                   "stp x10, x11, [x0, #0x58]\n\t"
                   "stp x12, x13, [x0, #0x68]\n\t"
                   "stp x14, x15, [x0, #0x78]\n\t"
                   "stp x16, x17, [x0, #0x88]\n\t"
                   "stp x18, x19, [x0, #0x98]\n\t"
                   "stp x20, x21, [x0, #0xa8]\n\t"
                   "stp x22, x23, [x0, #0xb8]\n\t"
                   "stp x24, x25, [x0, #0xc8]\n\t"
                   "stp x26, x27, [x0, #0xd8]\n\t"
                   "stp x28, x29, [x0, #0xe8]\n\t"
                   "str x30,      [x0, #0xf8]\n\t"
                   "mov x1, sp\n\t"
                   "str x1,       [x0, #0x100]\n\t"
                   "adr x1, 1f\n\t"
                   "str x1,       [x0, #0x108]\n\t"
                   "stp q0,  q1,  [x0, #0x110]\n\t"
                   "stp q2,  q3,  [x0, #0x130]\n\t"
                   "stp q4,  q5,  [x0, #0x150]\n\t"
                   "stp q6,  q7,  [x0, #0x170]\n\t"
                   "stp q8,  q9,  [x0, #0x190]\n\t"
                   "stp q10, q11, [x0, #0x1b0]\n\t"
                   "stp q12, q13, [x0, #0x1d0]\n\t"
                   "stp q14, q15, [x0, #0x1f0]\n\t"
                   "stp q16, q17, [x0, #0x210]\n\t"
                   "stp q18, q19, [x0, #0x230]\n\t"
                   "stp q20, q21, [x0, #0x250]\n\t"
                   "stp q22, q23, [x0, #0x270]\n\t"
                   "stp q24, q25, [x0, #0x290]\n\t"
                   "stp q26, q27, [x0, #0x2b0]\n\t"
                   "stp q28, q29, [x0, #0x2d0]\n\t"
                   "stp q30, q31, [x0, #0x2f0]\n\t"
                   "mrs x1, nzcv\n\t"
                   "str w1,       [x0, #0x04]\n\t"
                   "mrs x1, fpcr\n\t"
                   "str w1,       [x0, #0x310]\n\t"
                   "mrs x1, fpsr\n\t"
                   "str w1,       [x0, #0x314]\n\t"
                   "mov w1, #1\n\t"
                   "str x1,       [x0, #0x08]\n\t"   /* x0 is 1 on the way back */
                   "mov w1, #7\n\t"
                   "movk w1, #0x40, lsl #16\n\t"
                   "str w1,       [x0]\n\t"           /* CONTEXT_ARM64_FULL */
                   "mov w0, #0\n\t"
                   "ret\n"
                   "1:\tmov w0, #1\n\t"
                   "ret" )

/* Cooperative NtContinue/RtlRestoreContext return. Like the exception restore
 * above, x17 is the final branch scratch register. This is not a replacement
 * for reconstructing a guest context from an interrupted dynarec block. */
void horizon_continue_context( const CONTEXT *context )
{
    ThreadExceptionDump dump = {0};
    unsigned int i;

    for (i = 0; i < 29; ++i) dump.cpu_gprs[i].x = context->X[i];
    /* Windows treats x18 separately from CONTEXT_FULL. Keep the active TEB
     * unless the caller explicitly supplies CONTEXT_ARM64_X18. */
    if (!(context->ContextFlags & (CONTEXT_ARM64_X18 & ~CONTEXT_ARM64)))
        dump.cpu_gprs[18].x = (uintptr_t)NtCurrentTeb();
    dump.fp.x = context->Fp;
    dump.lr.x = context->Lr;
    dump.sp.x = context->Sp;
    dump.pc.x = context->Pc;
    dump.pstate = context->Cpsr;
    memcpy( dump.fpu_gprs, context->V, sizeof(context->V) );
    __asm__ volatile( "msr fpcr, %0\nmsr fpsr, %1"
                      : : "r"((uint64_t)context->Fpcr), "r"((uint64_t)context->Fpsr) : "memory" );
    horizon_restore_exception_context( &dump );
}
#endif

/* __nx_exception_stack / __nx_exception_stack_size are provided by each
 * executable's top-level source (runtime.c, the smoke main.c files, etc.).
 * We deliberately do NOT define them here to avoid multiple-definition link
 * errors. */

static ULONG_PTR cached_affinity_mask;
static LONG next_core_index;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

#define HORIZON_PIPE_BUFFER_SIZE 0x10000

struct horizon_pipe
{
    pthread_mutex_t mutex;
    pthread_cond_t can_read;
    pthread_cond_t can_write;
    unsigned int refs;
    int read_open;
    int write_open;
    size_t head;
    size_t tail;
    size_t used;
    unsigned int read_waiters;  /* threads waiting in can_read, so only they are signaled */
    unsigned int write_waiters; /* threads waiting in can_write */
    unsigned int client_cores;  /* request pipes: the cores their client thread is pinned to */
    unsigned char buffer[HORIZON_PIPE_BUFFER_SIZE];
};

struct horizon_pipe_file
{
    struct horizon_pipe *pipe;
    int write_end;
};

struct horizon_fd_message
{
    int fd;
    unsigned int handle;
    struct horizon_fd_message *next;
};

struct horizon_fd_queue
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct horizon_fd_message *head;
    struct horizon_fd_message *tail;
};

#define HORIZON_SERVER_FIXED_MESSAGE_SIZE 64
#define HORIZON_REQ_NEW_THREAD 2
#define HORIZON_REQ_INIT_PROCESS_DONE 4
#define HORIZON_REQ_INIT_FIRST_THREAD 5
#define HORIZON_REQ_INIT_THREAD 6
#define HORIZON_REQ_TERMINATE_THREAD 8
#define HORIZON_REQ_GET_THREAD_INFO 14
#define HORIZON_REQ_GET_THREAD_TIMES 15
#define HORIZON_REQ_SET_THREAD_INFO 16
#define HORIZON_REQ_SUSPEND_THREAD 17
#define HORIZON_REQ_RESUME_THREAD 18
#define HORIZON_REQ_QUEUE_APC 19
#define HORIZON_REQ_CLOSE_HANDLE 21
#define HORIZON_REQ_SET_HANDLE_INFO 22
#define HORIZON_REQ_DUP_HANDLE 23
#define HORIZON_REQ_ALLOCATE_RESERVE_OBJECT 24
#define HORIZON_REQ_COMPARE_OBJECTS 25
#define HORIZON_REQ_SET_OBJECT_PERMANENCE 26
#define HORIZON_REQ_OPEN_PROCESS 27
#define HORIZON_REQ_OPEN_THREAD 28
#define HORIZON_REQ_SELECT 29
#define HORIZON_REQ_CREATE_EVENT 30
#define HORIZON_REQ_EVENT_OP 31
#define HORIZON_REQ_QUERY_EVENT 32
#define HORIZON_REQ_OPEN_EVENT 33
#define HORIZON_REQ_CREATE_KEYED_EVENT 34
#define HORIZON_REQ_OPEN_KEYED_EVENT 35
#define HORIZON_REQ_CREATE_MUTEX 36
#define HORIZON_REQ_RELEASE_MUTEX 37
#define HORIZON_REQ_OPEN_MUTEX 38
#define HORIZON_REQ_QUERY_MUTEX 39
#define HORIZON_REQ_CREATE_SEMAPHORE 40
#define HORIZON_REQ_RELEASE_SEMAPHORE 41
#define HORIZON_REQ_QUERY_SEMAPHORE 42
#define HORIZON_REQ_OPEN_SEMAPHORE 43
#define HORIZON_REQ_CREATE_FILE 44
#define HORIZON_REQ_OPEN_FILE_OBJECT 45
#define HORIZON_REQ_GET_HANDLE_UNIX_NAME 47
#define HORIZON_REQ_GET_HANDLE_FD 48
#define HORIZON_REQ_RECV_SOCKET 55
#define HORIZON_REQ_SEND_SOCKET 56
#define HORIZON_REQ_SOCKET_GET_EVENTS 57
#define HORIZON_REQ_QUERY_DIRECTORY_FILE 244
#define HORIZON_REQ_SET_FD_DISP_INFO 275
#define HORIZON_REQ_SET_FD_NAME_INFO 276
#define HORIZON_REQ_SET_FD_EOF_INFO 277
#define HORIZON_REQ_SET_ASYNC_DIRECT_RESULT 137
#define HORIZON_REQ_CANCEL_ASYNC 135
#define HORIZON_REQ_GET_ASYNC_RESULT 136
#define HORIZON_REQ_IOCTL 140
#define HORIZON_REQ_CREATE_MAPPING 63
#define HORIZON_REQ_OPEN_MAPPING 64
#define HORIZON_REQ_GET_MAPPING_INFO 65
#define HORIZON_REQ_GET_IMAGE_MAP_ADDRESS 66
#define HORIZON_REQ_MAP_VIEW 67
#define HORIZON_REQ_MAP_IMAGE_VIEW 68
#define HORIZON_REQ_UNMAP_VIEW 71
#define HORIZON_REQ_GET_MAPPING_COMMITTED_RANGE 72
#define HORIZON_REQ_ADD_MAPPING_COMMITTED_RANGE 73
#define HORIZON_REQ_GET_TOKEN_SID 230
#define HORIZON_REQ_ALLOCATE_LOCALLY_UNIQUE_ID 252
#define HORIZON_REQ_OPEN_DIRECTORY 242
#define HORIZON_REQ_GET_DIRECTORY_ENTRIES 243
#define HORIZON_REQ_UPDATE_RAWINPUT_DEVICES 285
#define HORIZON_REQ_CREATE_KEY 86
#define HORIZON_REQ_OPEN_KEY 87
#define HORIZON_REQ_DELETE_KEY 88
#define HORIZON_REQ_ENUM_KEY 91
#define HORIZON_REQ_SET_KEY_VALUE 92
#define HORIZON_REQ_GET_KEY_VALUE 93
#define HORIZON_REQ_ENUM_KEY_VALUE 94
#define HORIZON_REQ_DELETE_KEY_VALUE 95
#define HORIZON_REQ_SET_REGISTRY_NOTIFICATION 99
#define HORIZON_REQ_RENAME_KEY 100
#define HORIZON_REQ_CREATE_TIMER 101
#define HORIZON_REQ_OPEN_TIMER 102
#define HORIZON_REQ_SET_TIMER 103
#define HORIZON_REQ_CANCEL_TIMER 104
#define HORIZON_REQ_GET_TIMER_INFO 105
#define HORIZON_REQ_ADD_ATOM 108
#define HORIZON_REQ_FIND_ATOM 110
#define HORIZON_REQ_ADD_USER_ATOM 112
#define HORIZON_REQ_GET_USER_ATOM_NAME 113
#define HORIZON_REQ_GET_MSG_QUEUE_HANDLE 114
#define HORIZON_REQ_GET_MSG_QUEUE 115
#define HORIZON_REQ_SET_QUEUE_MASK 117
#define HORIZON_REQ_GET_QUEUE_STATUS 118
#define HORIZON_REQ_SEND_MESSAGE 120
#define HORIZON_REQ_POST_QUIT_MESSAGE 121
#define HORIZON_REQ_SEND_HARDWARE_MESSAGE 122
#define HORIZON_REQ_GET_MESSAGE 124
#define HORIZON_REQ_REPLY_MESSAGE 125
#define HORIZON_REQ_ACCEPT_HARDWARE_MESSAGE 126
#define HORIZON_REQ_GET_MESSAGE_REPLY 127
#define HORIZON_REQ_SET_WIN_TIMER 128
#define HORIZON_REQ_KILL_WIN_TIMER 129
#define HORIZON_REQ_CREATE_WINDOW 144
#define HORIZON_REQ_DESTROY_WINDOW 145
#define HORIZON_REQ_GET_DESKTOP_WINDOW 146
#define HORIZON_REQ_SET_WINDOW_OWNER 147
#define HORIZON_REQ_GET_WINDOW_INFO 148
#define HORIZON_REQ_INIT_WINDOW_INFO 149
#define HORIZON_REQ_SET_WINDOW_INFO 150
#define HORIZON_REQ_GET_WINDOW_LIST 153
#define HORIZON_REQ_GET_WINDOW_CHILDREN_FROM_POINT 155
#define HORIZON_REQ_GET_WINDOW_TREE 156
#define HORIZON_REQ_SET_WINDOW_POS 157
#define HORIZON_REQ_GET_WINDOW_RECTANGLES 158
#define HORIZON_REQ_GET_VISIBLE_REGION 162
#define HORIZON_REQ_GET_UPDATE_REGION 165
#define HORIZON_REQ_UPDATE_WINDOW_ZORDER 166
#define HORIZON_REQ_REDRAW_WINDOW 167
#define HORIZON_REQ_SET_WINDOW_PROPERTY 168
#define HORIZON_REQ_REMOVE_WINDOW_PROPERTY 169
#define HORIZON_REQ_GET_WINDOW_PROPERTY 170
#define HORIZON_REQ_GET_WINDOW_PROPERTIES 171
#define HORIZON_REQ_CREATE_WINSTATION 172
#define HORIZON_REQ_OPEN_WINSTATION 173
#define HORIZON_REQ_CLOSE_WINSTATION 174
#define HORIZON_REQ_SET_WINSTATION_MONITORS 175
#define HORIZON_REQ_GET_PROCESS_WINSTATION 176
#define HORIZON_REQ_SET_PROCESS_WINSTATION 177
#define HORIZON_REQ_ENUM_WINSTATION 178
#define HORIZON_REQ_CREATE_DESKTOP 179
#define HORIZON_REQ_OPEN_DESKTOP 180
#define HORIZON_REQ_OPEN_INPUT_DESKTOP 181
#define HORIZON_REQ_SET_INPUT_DESKTOP 182
#define HORIZON_REQ_CLOSE_DESKTOP 183
#define HORIZON_REQ_GET_THREAD_DESKTOP 184
#define HORIZON_REQ_SET_THREAD_DESKTOP 185
#define HORIZON_REQ_SET_USER_OBJECT_INFO 186
#define HORIZON_REQ_GET_THREAD_INPUT 190
#define HORIZON_REQ_GET_KEY_STATE 192
#define HORIZON_REQ_SET_KEY_STATE 193
#define HORIZON_REQ_SET_FOREGROUND_WINDOW 194
#define HORIZON_REQ_SET_FOCUS_WINDOW 195
#define HORIZON_REQ_SET_ACTIVE_WINDOW 196
#define HORIZON_REQ_SET_CAPTURE_WINDOW 197
#define HORIZON_REQ_SET_CARET_WINDOW 198
#define HORIZON_REQ_SET_CARET_INFO 199
#define HORIZON_REQ_CREATE_CLASS 205
#define HORIZON_REQ_OPEN_CLIPBOARD 209
#define HORIZON_REQ_CLOSE_CLIPBOARD 210
#define HORIZON_REQ_EMPTY_CLIPBOARD 211
#define HORIZON_REQ_SET_CLIPBOARD_DATA 212
#define HORIZON_REQ_GET_CLIPBOARD_DATA 213
#define HORIZON_REQ_GET_CLIPBOARD_FORMATS 214
#define HORIZON_REQ_ENUM_CLIPBOARD_FORMATS 215
#define HORIZON_REQ_RELEASE_CLIPBOARD 216
#define HORIZON_REQ_GET_CLIPBOARD_INFO 217
#define HORIZON_REQ_SET_CLIPBOARD_VIEWER 218
#define HORIZON_REQ_ADD_CLIPBOARD_LISTENER 219
#define HORIZON_REQ_REMOVE_CLIPBOARD_LISTENER 220
#define HORIZON_REQ_DESTROY_CLASS 206
#define HORIZON_REQ_ALLOC_USER_HANDLE 280
#define HORIZON_REQ_FREE_USER_HANDLE 281
#define HORIZON_REQ_SET_CURSOR 282
#define HORIZON_STATUS_SUCCESS 0
#define HORIZON_STATUS_OBJECT_NAME_EXISTS 0x40000000u
#define HORIZON_STATUS_KERNEL_APC 0x00000100u
#define HORIZON_STATUS_ALERTED 0x00000101u
#define HORIZON_STATUS_USER_APC 0x000000c0u
#define HORIZON_STATUS_TIMEOUT 0x00000102u
#define HORIZON_STATUS_PENDING 0x00000103u
#define HORIZON_STATUS_UNSUCCESSFUL 0xc0000001u
#define HORIZON_STATUS_BUFFER_OVERFLOW 0x80000005u
#define HORIZON_STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define HORIZON_STATUS_DEVICE_NOT_READY 0xc00000a3u
#define HORIZON_STATUS_IO_TIMEOUT 0xc00000b5u
#define HORIZON_STATUS_NOT_SUPPORTED 0xc00000bbu
#define HORIZON_STATUS_DEVICE_BUSY 0x80000011u
#define HORIZON_STATUS_NETWORK_BUSY 0xc00000bfu
#define HORIZON_STATUS_INVALID_CONNECTION 0xc0000140u
#define HORIZON_STATUS_CONNECTION_RESET 0xc000020du
#define HORIZON_STATUS_CONNECTION_ABORTED 0xc0000241u
#define HORIZON_STATUS_CONNECTION_REFUSED 0xc0000236u
#define HORIZON_STATUS_CONNECTION_ACTIVE 0xc000023bu
#define HORIZON_STATUS_NETWORK_UNREACHABLE 0xc000023cu
#define HORIZON_STATUS_HOST_UNREACHABLE 0xc000023du
#define HORIZON_STATUS_ADDRESS_ALREADY_ASSOCIATED 0xc0000238u
#define HORIZON_STATUS_INVALID_ADDRESS_COMPONENT 0xc0000207u
#define HORIZON_STATUS_SHARING_VIOLATION 0xc0000043u
#define HORIZON_STATUS_PIPE_DISCONNECTED 0xc00000b0u
#define HORIZON_STATUS_NOT_IMPLEMENTED 0xc0000002u
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_STATUS_NOT_MAPPED_VIEW 0xc0000019u
#define HORIZON_STATUS_INVALID_PARAMETER 0xc000000du
#define HORIZON_STATUS_INVALID_IMAGE_FORMAT 0xc000007bu
#define HORIZON_STATUS_NO_SUCH_FILE 0xc000000fu
#define HORIZON_STATUS_ACCESS_DENIED 0xc0000022u
#define HORIZON_STATUS_NO_MEMORY 0xc0000017u
#define HORIZON_STATUS_OBJECT_TYPE_MISMATCH 0xc0000024u
#define HORIZON_STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034u
#define HORIZON_STATUS_OBJECT_NAME_COLLISION 0xc0000035u
#define HORIZON_STATUS_FILE_IS_A_DIRECTORY 0xc00000bau
#define HORIZON_STATUS_DIRECTORY_NOT_EMPTY 0xc0000101u
#define HORIZON_STATUS_OBJECT_PATH_SYNTAX_BAD 0xc000003bu
#define HORIZON_STATUS_OBJECT_PATH_NOT_FOUND 0xc000003au
#define HORIZON_STATUS_MUTANT_NOT_OWNED 0xc0000046u
#define HORIZON_STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xc0000047u
#define HORIZON_STATUS_BAD_DEVICE_TYPE 0xc00000cbu
#define HORIZON_STATUS_TOO_MANY_OPENED_FILES 0xc000011fu
#define HORIZON_STATUS_INFO_LENGTH_MISMATCH 0xc0000004u
#define HORIZON_STATUS_NO_MORE_FILES 0x80000006u
#define HORIZON_STATUS_NOT_SAME_OBJECT 0xc00001acu
#define HORIZON_STATUS_INVALID_CID 0xc000000bu
#define HORIZON_STATUS_ABANDONED_WAIT_0 0x00000080u
#define HORIZON_CURRENT_THREAD_HANDLE 0xfffffffeu
#define HORIZON_CURRENT_PROCESS_HANDLE 0xffffffffu
#define HORIZON_THREAD_CREATE_SUSPENDED 0x00000001u
#define HORIZON_SET_THREAD_INFO_PRIORITY 0x01u
#define HORIZON_SET_THREAD_INFO_BASE_PRIORITY 0x02u
#define HORIZON_SET_THREAD_INFO_AFFINITY 0x04u
#define HORIZON_SET_THREAD_INFO_ENTRYPOINT 0x10u
#define HORIZON_IMAGE_FILE_MACHINE_ARM64 0xaa64
#define HORIZON_IMAGE_FILE_MACHINE_AMD64 0x8664
#define HORIZON_IMAGE_FILE_MACHINE_I386 0x014c
static unsigned short horizon_process_machine = HORIZON_IMAGE_FILE_MACHINE_ARM64;
unsigned int horizon_set_process_machine( unsigned short machine )
{
    if (machine != HORIZON_IMAGE_FILE_MACHINE_ARM64 &&
        machine != HORIZON_IMAGE_FILE_MACHINE_AMD64 &&
        machine != HORIZON_IMAGE_FILE_MACHINE_I386)
        return 0xc000007b; /* STATUS_INVALID_IMAGE_FORMAT */
    horizon_process_machine = machine;
    return 0;
}

#define HORIZON_IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b
#define HORIZON_IMAGE_FILE_RELOCS_STRIPPED 0x0001
#define HORIZON_IMAGE_FILE_DLL 0x2000
#define HORIZON_IMAGE_SCN_MEM_EXECUTE 0x20000000
#define HORIZON_IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE 0x0040
#define HORIZON_IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define HORIZON_IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG 10
#define HORIZON_IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14
#define HORIZON_IMAGE_FLAGS_IMAGE_DYNAMICALLY_RELOCATED 0x04
#define HORIZON_IMAGE_FLAGS_IMAGE_MAPPED_FLAT 0x08
#define HORIZON_SEC_IMAGE 0x01000000u
#define HORIZON_FILE_DIRECTORY_FILE 0x00000001u
#define HORIZON_FILE_NON_DIRECTORY_FILE 0x00000040u
#define HORIZON_APC_RESULT_SIZE 40
#define HORIZON_FD_TYPE_FILE 1
#define HORIZON_FD_TYPE_DIR 2
#define HORIZON_FD_TYPE_SOCKET 3
#define HORIZON_FIRST_USER_HANDLE 0x0020
#define HORIZON_LAST_USER_HANDLE 0xffef
#define HORIZON_MAX_USER_HANDLES ((HORIZON_LAST_USER_HANDLE - HORIZON_FIRST_USER_HANDLE + 1) >> 1)
#define HORIZON_MAX_ATOM_LEN 255
#define HORIZON_SESSION_MAPPING_SIZE 0x200000
#define HORIZON_DESKTOP_ATOM 32769
#define HORIZON_NTUSER_OBJ_WINDOW 0x01
#define HORIZON_NTUSER_DPI_PER_MONITOR_AWARE 0x12
#define HORIZON_SET_USER_OBJECT_SET_FLAGS 1
#define HORIZON_WSF_VISIBLE 1
#define HORIZON_GWL_EXSTYLE (-20)
#define HORIZON_GWL_STYLE (-16)
#define HORIZON_GWLP_ID (-12)
#define HORIZON_GWLP_HINSTANCE (-6)
#define HORIZON_GWLP_WNDPROC (-4)
#define HORIZON_GWLP_USERDATA (-21)
#define HORIZON_WS_VISIBLE 0x10000000u
#define HORIZON_WS_DISABLED 0x08000000u
#define HORIZON_DCX_WINDOW 0x00000001u
#define HORIZON_SWP_NOREDRAW 0x0008
#define HORIZON_SWP_SHOWWINDOW 0x0040
#define HORIZON_SWP_HIDEWINDOW 0x0080
#define HORIZON_SWP_NOZORDER 0x0004
#define HORIZON_WS_EX_TOPMOST 0x00000008u
#define HORIZON_WS_EX_TRANSPARENT 0x00000020u
#define HORIZON_WS_EX_LAYERED 0x00080000u
#define HORIZON_SET_WINPOS_PAINT_SURFACE 0x01
#define HORIZON_COORDS_CLIENT 0
#define HORIZON_COORDS_WINDOW 1
#define HORIZON_COORDS_PARENT 2
#define HORIZON_COORDS_SCREEN 3
#define HORIZON_RDW_INVALIDATE 0x0001
#define HORIZON_RDW_INTERNALPAINT 0x0002
#define HORIZON_RDW_ERASE 0x0004
#define HORIZON_RDW_VALIDATE 0x0008
#define HORIZON_RDW_NOINTERNALPAINT 0x0010
#define HORIZON_RDW_NOCHILDREN 0x0040
#define HORIZON_RDW_ALLCHILDREN 0x0080
#define HORIZON_RDW_FRAME 0x0400
#define HORIZON_UPDATE_NONCLIENT 0x001
#define HORIZON_UPDATE_ERASE 0x002
#define HORIZON_UPDATE_PAINT 0x004
#define HORIZON_UPDATE_INTERNALPAINT 0x008
#define HORIZON_UPDATE_ALLCHILDREN 0x010
#define HORIZON_UPDATE_NOCHILDREN 0x020
#define HORIZON_UPDATE_NOREGION 0x040
#define HORIZON_WS_MINIMIZE 0x20000000u
#define HORIZON_WS_CLIPCHILDREN 0x02000000u
#define HORIZON_WM_PAINT 0x000f
#define HORIZON_WM_MOUSEMOVE 0x0200
#define HORIZON_WM_LBUTTONDOWN 0x0201
#define HORIZON_WM_LBUTTONUP 0x0202
#define HORIZON_WM_LBUTTONDBLCLK 0x0203
#define HORIZON_WM_NCLBUTTONDBLCLK 0x00a3
#define HORIZON_WM_RBUTTONDOWN 0x0204
#define HORIZON_WM_RBUTTONUP 0x0205
#define HORIZON_WM_NCMOUSEFIRST 0x00a0
#define HORIZON_WM_MOUSEFIRST 0x0200
#define HORIZON_MK_LBUTTON 0x0001
#define HORIZON_MK_RBUTTON 0x0002
#define HORIZON_VK_LBUTTON 0x01
#define HORIZON_VK_RBUTTON 0x02
#define HORIZON_MSG_POSTED 6
#define HORIZON_MSG_HARDWARE 7
#define HORIZON_PM_REMOVE 0x0001
#define HORIZON_QS_POSTMESSAGE 0x0008
#define HORIZON_QS_TIMER 0x0010
#define HORIZON_QS_ALLINPUT 0x04ff
#define HORIZON_INPUT_MOUSE 0
#define HORIZON_IMDT_MOUSE 0x02
#define HORIZON_IMO_HARDWARE 0x01
#define HORIZON_INPUT_KEYBOARD 1
#define HORIZON_IMDT_KEYBOARD 0x01
#define HORIZON_WM_INPUT 0x00ff
#define HORIZON_RIM_INPUT 0
/* The MOUSEEVENTF_* a hardware message carries are in horizon_mouse.h. */
#define HORIZON_SET_CARET_POS 0x01
#define HORIZON_SET_CARET_HIDE 0x02
#define HORIZON_SET_CARET_STATE 0x04
#define HORIZON_CARET_STATE_OFF 0
#define HORIZON_CARET_STATE_ON 1
#define HORIZON_CARET_STATE_TOGGLE 2
#define HORIZON_CARET_STATE_ON_IF_MOVED 3
#define HORIZON_CAPTURE_MENU 0x01
#define HORIZON_CAPTURE_MOVESIZE 0x02
#define HORIZON_SET_CURSOR_HANDLE 0x01
#define HORIZON_SET_CURSOR_COUNT 0x02
#define HORIZON_SET_CURSOR_POS 0x04
#define HORIZON_SET_CURSOR_CLIP 0x08
#define HORIZON_SET_CURSOR_NOCLIP 0x10

#ifndef FILE_READ_DATA
#define FILE_READ_DATA 0x0001
#endif
#ifndef FILE_WRITE_DATA
#define FILE_WRITE_DATA 0x0002
#endif
#ifndef FILE_APPEND_DATA
#define FILE_APPEND_DATA 0x0004
#endif
#ifndef FILE_SUPERSEDE
#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#endif

enum horizon_select_opcode
{
    HORIZON_SELECT_NONE,
    HORIZON_SELECT_WAIT,
    HORIZON_SELECT_WAIT_ALL,
    HORIZON_SELECT_SIGNAL_AND_WAIT,
    HORIZON_SELECT_KEYED_EVENT_WAIT,
    HORIZON_SELECT_KEYED_EVENT_RELEASE
};

enum horizon_event_op
{
    HORIZON_PULSE_EVENT,
    HORIZON_SET_EVENT,
    HORIZON_RESET_EVENT
};

enum horizon_server_object_type
{
    HORIZON_SERVER_OBJECT_RESERVE = 1,
    HORIZON_SERVER_OBJECT_REG_KEY,
    HORIZON_SERVER_OBJECT_EVENT,
    HORIZON_SERVER_OBJECT_KEYED_EVENT,
    HORIZON_SERVER_OBJECT_MUTEX,
    HORIZON_SERVER_OBJECT_SEMAPHORE,
    HORIZON_SERVER_OBJECT_TIMER,
    HORIZON_SERVER_OBJECT_FILE,
    HORIZON_SERVER_OBJECT_MAPPING,
    HORIZON_SERVER_OBJECT_PROCESS,
    HORIZON_SERVER_OBJECT_THREAD,
    HORIZON_SERVER_OBJECT_SOCK,
    HORIZON_SERVER_OBJECT_WINSTATION,
    HORIZON_SERVER_OBJECT_DESKTOP,
    HORIZON_SERVER_OBJECT_MSG_QUEUE,
    HORIZON_SERVER_OBJECT_DIRECTORY,
    HORIZON_SERVER_OBJECT_COMPLETION,
    HORIZON_SERVER_OBJECT_COMPLETION_WAIT
};

struct horizon_server_request_header
{
    int req;
    unsigned int request_size;
    unsigned int reply_size;
};

struct horizon_server_reply_header
{
    unsigned int error;
    unsigned int reply_size;
};

#include "horizon_registry_wire.h"
#include "horizon_completion.h"

struct horizon_init_first_thread_request
{
    struct horizon_server_request_header header;
    int unix_pid;
    int unix_tid;
    int debug_level;
    int reply_fd;
    int wait_fd;
};

struct horizon_init_first_thread_reply
{
    struct horizon_server_reply_header header;
    unsigned int pid;
    unsigned int tid;
    long long server_start;
    unsigned int session_id;
    unsigned int inproc_device;
    unsigned int info_size;
    char pad[4];
};

struct horizon_init_process_done_reply
{
    struct horizon_server_reply_header header;
    int suspend;
    char pad[4];
};

struct horizon_init_thread_request
{
    struct horizon_server_request_header header;
    int unix_tid;
    int reply_fd;
    int wait_fd;
    unsigned long long teb;
    unsigned long long entry;
};

struct horizon_init_thread_reply
{
    struct horizon_server_reply_header header;
    int suspend;
    char pad[4];
};

struct horizon_init_process_done_request
{
    struct horizon_server_request_header header;
    char pad[4];
    unsigned long long teb;
    unsigned long long peb;
    unsigned long long ldt_copy;
};

struct horizon_terminate_thread_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int exit_code;
    char pad[4];
};

struct horizon_terminate_thread_reply
{
    struct horizon_server_reply_header header;
    int self;
    char pad[4];
};

struct horizon_get_thread_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int access;
    char pad[4];
};

struct horizon_get_thread_info_reply
{
    struct horizon_server_reply_header header;
    unsigned int pid;
    unsigned int tid;
    unsigned long long teb;
    unsigned long long entry_point;
    unsigned long long affinity;
    int exit_code;
    int priority;
    int base_priority;
    int suspend_count;
    unsigned int flags;
    unsigned int desc_len;
};

struct horizon_get_thread_times_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_thread_times_reply
{
    struct horizon_server_reply_header header;
    long long creation_time;
    long long exit_time;
    int unix_pid;
    int unix_tid;
};

struct horizon_set_thread_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int priority;
    int base_priority;
    unsigned long long affinity;
    unsigned long long entry_point;
    unsigned int token;
    int disable_boost;
    unsigned int mask;
    char pad[4];
};

struct horizon_open_thread_request
{
    struct horizon_server_request_header header;
    unsigned int tid;
    unsigned int access;
    unsigned int attributes;
};

struct horizon_suspend_thread_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int waited_handle;
    char pad[4];
};

struct horizon_suspend_thread_reply
{
    struct horizon_server_reply_header header;
    int count;
    unsigned int wait_handle;
};

struct horizon_close_handle_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_set_handle_info_reply
{
    struct horizon_server_reply_header header;
    int old_flags;
    char pad[4];
};

struct horizon_dup_handle_request
{
    struct horizon_server_request_header header;
    unsigned int src_process;
    unsigned int src_handle;
    unsigned int dst_process;
    unsigned int access;
    unsigned int attributes;
    unsigned int options;
    char pad[4];
};

struct horizon_dup_handle_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_allocate_reserve_object_request
{
    struct horizon_server_request_header header;
    int type;
};

struct horizon_allocate_reserve_object_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_compare_objects_request
{
    struct horizon_server_request_header header;
    unsigned int first;
    unsigned int second;
    char pad[4];
};

struct horizon_open_process_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_get_directory_entries_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int index;
    unsigned int max_count;
};

struct horizon_get_directory_entries_reply
{
    struct horizon_server_reply_header header;
    unsigned int total_len;
    unsigned int count;
    /* VARARG(entries,directory_entries) */
};

struct horizon_select_request
{
    struct horizon_server_request_header header;
    int flags;
    unsigned long long cookie;
    long long timeout;
    unsigned int size;
    unsigned int prev_apc;
};

struct horizon_select_reply
{
    struct horizon_server_reply_header header;
    unsigned int apc_handle;
    int signaled;
};

/* The wait is alertable: a user APC waiting for this thread runs instead. */
#define HORIZON_SELECT_ALERTABLE 1

struct horizon_queue_apc_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int reserve_handle;
    /* followed by the call, an apc_call the client built */
};

struct horizon_queue_apc_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    int self;
};

struct horizon_object_attributes
{
    unsigned int rootdir;
    unsigned int attributes;
    unsigned int sd_len;
    unsigned int name_len;
};

struct horizon_object_name
{
    unsigned int rootdir;
    const unsigned char *name;
    unsigned int name_len;
};

struct horizon_create_event_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    int manual_reset;
    int initial_state;
};

struct horizon_create_event_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_event_op_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int op;
    char pad[4];
};

struct horizon_event_op_reply
{
    struct horizon_server_reply_header header;
    int state;
    char pad[4];
};

struct horizon_query_event_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_query_event_reply
{
    struct horizon_server_reply_header header;
    int manual_reset;
    int state;
};

struct horizon_open_named_object_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int attributes;
    unsigned int rootdir;
};

struct horizon_create_file_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int sharing;
    int create;
    unsigned int options;
    unsigned int attrs;
};

struct horizon_create_file_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_set_fd_disp_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int flags;
    char pad[4];
};

struct horizon_set_fd_name_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int rootdir;
    unsigned int namelen;
    int link;
    unsigned int flags;
    /* VARARG(name,unicode_str,namelen); VARARG(filename,string); */
};

struct horizon_set_fd_eof_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long eof;
};

struct horizon_get_handle_unix_name_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_handle_unix_name_reply
{
    struct horizon_server_reply_header header;
    unsigned int name_len;
    char pad[4];
};

struct horizon_get_handle_fd_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_handle_fd_reply
{
    struct horizon_server_reply_header header;
    int type;
    int cacheable;
    unsigned int access;
    unsigned int options;
};

struct horizon_directory_file_entry
{
    unsigned int name_len;
};

struct horizon_query_directory_file_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int restart_scan;
    char pad[4];
};

struct horizon_query_directory_file_reply
{
    struct horizon_server_reply_header header;
    unsigned int total_len;
    char pad[4];
};

struct horizon_new_thread_request
{
    struct horizon_server_request_header header;
    unsigned int process;
    unsigned int access;
    unsigned int flags;
    int request_fd;
    char pad[4];
};

struct horizon_new_thread_reply
{
    struct horizon_server_reply_header header;
    unsigned int tid;
    unsigned int handle;
};

struct horizon_resume_thread_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_resume_thread_reply
{
    struct horizon_server_reply_header header;
    int count;
    char pad[4];
};

struct horizon_open_file_object_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int attributes;
    unsigned int rootdir;
    unsigned int sharing;
    unsigned int options;
    /* UTF-16LE filename follows as request data */
};

struct horizon_open_file_object_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_async_data
{
    unsigned int handle;
    unsigned int event;
    unsigned long long iosb;
    unsigned long long user;
    unsigned long long apc;
    unsigned long long apc_context;
};

struct horizon_recv_socket_request
{
    struct horizon_server_request_header header;
    int oob;
    struct horizon_async_data async;
    int force_async;
    char pad[4];
};

struct horizon_socket_io_reply
{
    struct horizon_server_reply_header header;
    unsigned int wait;
    unsigned int options;
    int nonblocking;
    char pad[4];
};

struct horizon_send_socket_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    struct horizon_async_data async;
};

struct horizon_set_async_direct_result_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long information;
    unsigned int status;
    int mark_pending;
};

struct horizon_set_async_direct_result_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_ioctl_request
{
    struct horizon_server_request_header header;
    unsigned int code;
    struct horizon_async_data async;
    /* in_data follows as request data */
};

#define HORIZON_ASYNC_DATA_DEFINED 1
#include "horizon_async.h"
#include "horizon_sockaddr.h"

struct horizon_get_async_result_request
{
    struct horizon_server_request_header header;
    char pad[4];
    unsigned long long user_arg;
};

struct horizon_cancel_async_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long iosb;
    int only_thread;
};

/* The wire layouts these are read from are server_protocol.h's. */
typedef char horizon_recv_socket_async_offset[
    offsetof( struct horizon_recv_socket_request, async ) == 16 ? 1 : -1];
typedef char horizon_send_socket_async_offset[
    offsetof( struct horizon_send_socket_request, async ) == 16 ? 1 : -1];
typedef char horizon_ioctl_async_offset[offsetof( struct horizon_ioctl_request, async ) == 16 ? 1 : -1];
typedef char horizon_get_async_result_user_offset[
    offsetof( struct horizon_get_async_result_request, user_arg ) == 16 ? 1 : -1];
typedef char horizon_cancel_async_iosb_offset[
    offsetof( struct horizon_cancel_async_request, iosb ) == 16 ? 1 : -1];
typedef char horizon_set_async_direct_result_status_offset[
    offsetof( struct horizon_set_async_direct_result_request, status ) == 24 ? 1 : -1];

/* The operations on sockets that wait, and the system APCs that run them. */
static struct horizon_async_list horizon_asyncs;
static unsigned int horizon_async_apc_ids;

#define HORIZON_APC_USER      1
#define HORIZON_APC_ASYNC_IO  2
#define HORIZON_APC_CALL_SIZE 64  /* union apc_call */
#define HORIZON_STATUS_NOT_FOUND 0xc0000225u
#define HORIZON_NT_ERROR(status) (((status) >> 30) == 3)
/* How long, in performance-counter ticks, a ready operation waits for the
 * thread that started it before any waiting thread may run it. */
#define HORIZON_ASYNC_STALE 200000ULL  /* 20 ms */

struct horizon_server_connection;
static struct horizon_async *horizon_server_async_create_locked( struct horizon_server_connection *connection,
                                                                 const struct horizon_async_data *data,
                                                                 int direction, int kind );
static void horizon_server_async_free_locked( struct horizon_async *async );
static void horizon_async_finish_locked( struct horizon_async *async, unsigned int status,
                                         unsigned long long total );
static unsigned long long horizon_async_now(void);
static void horizon_report_async( const char *what, const struct horizon_async *async, unsigned int status );
static void horizon_sock_poller_start(void);
/* The client's cached copy of a socket's fd, which accepting into the socket
 * replaces (server.c). */
extern void horizon_client_forget_fd( unsigned int handle );

struct horizon_ioctl_reply
{
    struct horizon_server_reply_header header;
    unsigned int wait;
    unsigned int options;
    /* out_data follows as reply data */
};

struct horizon_socket_get_events_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int event;
    char pad[4];
};

struct horizon_socket_get_events_reply
{
    struct horizon_server_reply_header header;
    unsigned int flags;
    char pad[4];
    /* status[13] follows as reply data */
};

struct horizon_create_mapping_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int flags;
    unsigned int file_access;
    unsigned long long size;
    unsigned int file_handle;
    char pad[4];
};

struct horizon_create_mapping_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_get_mapping_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int access;
    char pad[4];
};

struct horizon_get_mapping_info_reply
{
    struct horizon_server_reply_header header;
    unsigned long long size;
    unsigned int flags;
    unsigned int shared_file;
    unsigned int name_len;
    unsigned int total;
};

struct horizon_get_image_map_address_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_image_map_address_reply
{
    struct horizon_server_reply_header header;
    unsigned long long addr;
};

struct horizon_map_view_request
{
    struct horizon_server_request_header header;
    unsigned int mapping;
    unsigned int access;
    char pad[4];
    unsigned long long base;
    unsigned long long size;
    long long start;
};

struct horizon_map_image_view_request
{
    struct horizon_server_request_header header;
    unsigned int mapping;
    unsigned long long base;
    unsigned long long size;
    unsigned long long offset;
    unsigned int entry;
    unsigned short machine;
    char pad[2];
};

struct horizon_unmap_view_request
{
    struct horizon_server_request_header header;
    char pad[4];
    unsigned long long base;
};

struct horizon_get_mapping_committed_range_request
{
    struct horizon_server_request_header header;
    char pad[4];
    unsigned long long base;
    long long offset;
};

struct horizon_get_mapping_committed_range_reply
{
    struct horizon_server_reply_header header;
    unsigned long long size;
    int committed;
    char pad[4];
};

struct horizon_add_mapping_committed_range_request
{
    struct horizon_server_request_header header;
    char pad[4];
    unsigned long long base;
    long long offset;
    unsigned long long size;
};

/* Wine's server_protocol.h layouts. */
_Static_assert( offsetof(struct horizon_get_mapping_committed_range_request, offset) == 24,
                "get_mapping_committed_range_request layout" );
_Static_assert( offsetof(struct horizon_get_mapping_committed_range_reply, committed) == 16,
                "get_mapping_committed_range_reply layout" );
_Static_assert( offsetof(struct horizon_add_mapping_committed_range_request, size) == 32,
                "add_mapping_committed_range_request layout" );

struct horizon_pe_image_info
{
    unsigned long long base;
    unsigned long long map_addr;
    unsigned long long stack_size;
    unsigned long long stack_commit;
    unsigned int entry_point;
    unsigned int map_size;
    unsigned int alignment;
    unsigned int zerobits;
    unsigned int subsystem;
    unsigned short subsystem_minor;
    unsigned short subsystem_major;
    unsigned short osversion_major;
    unsigned short osversion_minor;
    unsigned short image_charact;
    unsigned short dll_charact;
    unsigned short machine;
    unsigned char contains_code : 1;
    unsigned char wine_builtin : 1;
    unsigned char wine_fakedll : 1;
    unsigned char is_hybrid : 1;
    unsigned char padding : 4;
    unsigned char image_flags;
    unsigned int loader_flags;
    unsigned int header_size;
    unsigned int header_map_size;
    unsigned int file_size;
    unsigned int checksum;
    unsigned int dbg_offset;
    unsigned int dbg_size;
};

struct horizon_create_mutex_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    int owned;
    char pad[4];
};

struct horizon_create_mutex_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_release_mutex_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_release_mutex_reply
{
    struct horizon_server_reply_header header;
    unsigned int prev_count;
    char pad[4];
};

struct horizon_query_mutex_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_query_mutex_reply
{
    struct horizon_server_reply_header header;
    unsigned int count;
    int owned;
    int abandoned;
    char pad[4];
};

struct horizon_create_semaphore_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int initial;
    unsigned int max;
};

struct horizon_create_semaphore_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_release_semaphore_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int count;
    char pad[4];
};

struct horizon_release_semaphore_reply
{
    struct horizon_server_reply_header header;
    unsigned int prev_count;
    char pad[4];
};

struct horizon_query_semaphore_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_query_semaphore_reply
{
    struct horizon_server_reply_header header;
    unsigned int current;
    unsigned int max;
};

struct horizon_create_timer_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    int manual;
    char pad[4];
};

struct horizon_create_timer_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_set_timer_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    long long expire;
    unsigned long long callback;
    unsigned long long arg;
    int period;
    char pad[4];
};

/* Wine's luid_t after the reply header: AllocateLocallyUniqueId's answer. */
struct horizon_allocate_locally_unique_id_reply
{
    struct horizon_server_reply_header header;
    unsigned int low_part;
    int high_part;
};

struct horizon_set_timer_reply
{
    struct horizon_server_reply_header header;
    int signaled;
    char pad[4];
};

struct horizon_cancel_timer_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_cancel_timer_reply
{
    struct horizon_server_reply_header header;
    int signaled;
    char pad[4];
};

struct horizon_get_timer_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_timer_info_reply
{
    struct horizon_server_reply_header header;
    long long when;
    int signaled;
    char pad[4];
};

struct horizon_atom_reply
{
    struct horizon_server_reply_header header;
    unsigned int atom;
    char pad[4];
};

struct horizon_create_window_request
{
    struct horizon_server_request_header header;
    unsigned int parent;
    unsigned int owner;
    unsigned int atom;
    unsigned long long class_instance;
    unsigned long long instance;
    unsigned int dpi_context;
    unsigned int style;
    unsigned int ex_style;
    char pad[4];
};

struct horizon_create_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    unsigned int parent;
    unsigned int owner;
    int extra;
    unsigned long long class_ptr;
};

struct horizon_destroy_window_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_desktop_window_request
{
    struct horizon_server_request_header header;
    int force;
};

struct horizon_get_desktop_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int top_window;
    unsigned int msg_window;
};

struct horizon_rectangle
{
    int left;
    int top;
    int right;
    int bottom;
};

struct horizon_set_window_owner_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int owner;
    char pad[4];
};

struct horizon_set_window_owner_reply
{
    struct horizon_server_reply_header header;
    unsigned int full_owner;
    unsigned int prev_owner;
};

struct horizon_get_window_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int offset;
    unsigned int size;
};

struct horizon_get_window_info_reply
{
    struct horizon_server_reply_header header;
    unsigned int last_active;
    int is_unicode;
    unsigned long long info;
};

struct horizon_init_window_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int style;
    unsigned int ex_style;
    short is_unicode;
    char pad[6];
};

struct horizon_set_window_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int offset;
    unsigned int size;
    unsigned long long new_info;
};

struct horizon_set_window_info_reply
{
    struct horizon_server_reply_header header;
    unsigned long long old_info;
};

struct horizon_get_window_tree_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_window_list_request
{
    struct horizon_server_request_header header;
    unsigned int desktop;
    unsigned int handle;
    unsigned int tid;
    int children;
    char pad[4];
};

struct horizon_get_window_list_reply
{
    struct horizon_server_reply_header header;
    int count;
    char pad[4];
};

struct horizon_get_window_children_from_point_request
{
    struct horizon_server_request_header header;
    unsigned int parent;
    int x;
    int y;
    int dpi;
    char pad[4];
};

struct horizon_get_window_children_from_point_reply
{
    struct horizon_server_reply_header header;
    int count;
    char pad[4];
};

struct horizon_get_window_tree_reply
{
    struct horizon_server_reply_header header;
    unsigned int parent;
    unsigned int owner;
    unsigned int next_sibling;
    unsigned int prev_sibling;
    unsigned int first_sibling;
    unsigned int last_sibling;
    unsigned int first_child;
    unsigned int last_child;
};

struct horizon_set_window_pos_request
{
    struct horizon_server_request_header header;
    unsigned short swp_flags;
    unsigned short paint_flags;
    unsigned int monitor_dpi;
    unsigned int handle;
    unsigned int previous;
    struct horizon_rectangle window;
    struct horizon_rectangle client;
    char pad[4];
};

struct horizon_set_window_pos_reply
{
    struct horizon_server_reply_header header;
    unsigned int new_style;
    unsigned int new_ex_style;
    unsigned int surface_win;
    char pad[4];
};

struct horizon_get_window_rectangles_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int relative;
    int dpi;
};

struct horizon_get_window_rectangles_reply
{
    struct horizon_server_reply_header header;
    struct horizon_rectangle window;
    struct horizon_rectangle client;
};

struct horizon_get_visible_region_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    unsigned int flags;
    char pad[4];
};

struct horizon_get_visible_region_reply
{
    struct horizon_server_reply_header header;
    unsigned int top_win;
    struct horizon_rectangle top_rect;
    struct horizon_rectangle win_rect;
    unsigned int paint_flags;
    unsigned int total_size;
    char pad[4];
};

struct horizon_get_update_region_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    unsigned int from_child;
    unsigned int flags;
};

struct horizon_get_update_region_reply
{
    struct horizon_server_reply_header header;
    unsigned int child;
    unsigned int flags;
    unsigned int total_size;
    char pad[4];
};

struct horizon_send_message_request
{
    struct horizon_server_request_header header;
    unsigned int id;
    int type;
    int flags;
    unsigned int win;
    unsigned int msg;
    unsigned long long wparam;
    unsigned long long lparam;
    long long timeout;
};

struct horizon_post_quit_message_request
{
    struct horizon_server_request_header header;
    int exit_code;
};

struct horizon_set_win_timer_request
{
    struct horizon_server_request_header header;
    unsigned int win;
    unsigned int msg;
    unsigned int rate;
    unsigned long long id;
    unsigned long long lparam;
};

struct horizon_set_win_timer_reply
{
    struct horizon_server_reply_header header;
    unsigned long long id;
};

struct horizon_kill_win_timer_request
{
    struct horizon_server_request_header header;
    unsigned int win;
    unsigned long long id;
    unsigned int msg;
    char pad[4];
};

struct horizon_get_msg_queue_handle_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    unsigned int idle_event;
};

struct horizon_get_msg_queue_reply
{
    struct horizon_server_reply_header header;
    unsigned long long locator_id;      /* struct obj_locator */
    unsigned long long locator_offset;
};

struct horizon_set_queue_mask_request
{
    struct horizon_server_request_header header;
    unsigned int wake_mask;
    unsigned int changed_mask;
    int poll_events;
};

struct horizon_get_queue_status_request
{
    struct horizon_server_request_header header;
    unsigned int clear_bits;
};

struct horizon_queue_bits_reply
{
    struct horizon_server_reply_header header;
    unsigned int wake_bits;
    unsigned int changed_bits;
};

struct horizon_reply_message_request
{
    struct horizon_server_request_header header;
    int remove;
    unsigned long long result;
};

struct horizon_get_message_reply_request
{
    struct horizon_server_request_header header;
    int cancel;
};

struct horizon_get_message_reply_reply
{
    struct horizon_server_reply_header header;
    unsigned long long result;
};

struct horizon_get_user_atom_name_request
{
    struct horizon_server_request_header header;
    unsigned int atom;
};

struct horizon_get_user_atom_name_reply
{
    struct horizon_server_reply_header header;
    unsigned int total;
    char pad[4];
};

/* open_clipboard, release_clipboard and the listener requests: one window. */
struct horizon_clipboard_window_request
{
    struct horizon_server_request_header header;
    unsigned int window;
};

/* One or two values: open_clipboard (owner), close and release_clipboard
 * (viewer, owner), set_clipboard_data (seqno), get_clipboard_formats (count),
 * enum_clipboard_formats (format), set_clipboard_viewer (old viewer, owner). */
struct horizon_clipboard_pair_reply
{
    struct horizon_server_reply_header header;
    unsigned int first;
    unsigned int second;
};

struct horizon_set_clipboard_data_request
{
    struct horizon_server_request_header header;
    unsigned int format;
    unsigned int lcid;
    char pad[4];
};

struct horizon_get_clipboard_data_request
{
    struct horizon_server_request_header header;
    unsigned int format;
    int render;
    int cached;
    unsigned int seqno;
    char pad[4];
};

struct horizon_get_clipboard_data_reply
{
    struct horizon_server_reply_header header;
    unsigned int from;
    unsigned int owner;
    unsigned int seqno;
    unsigned int total;
};

/* get_clipboard_formats (format) and enum_clipboard_formats (previous). */
struct horizon_clipboard_format_request
{
    struct horizon_server_request_header header;
    unsigned int format;
};

struct horizon_get_clipboard_info_reply
{
    struct horizon_server_reply_header header;
    unsigned int window;
    unsigned int owner;
    unsigned int viewer;
    unsigned int seqno;
};

struct horizon_set_clipboard_viewer_request
{
    struct horizon_server_request_header header;
    unsigned int viewer;
    unsigned int previous;
    char pad[4];
};

struct horizon_get_message_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    unsigned int get_win;
    unsigned int get_first;
    unsigned int get_last;
    unsigned int hw_id;
    unsigned int wake_mask;
    unsigned int changed_mask;
    unsigned int internal;
    char pad[4];
};

struct horizon_get_message_reply
{
    struct horizon_server_reply_header header;
    unsigned int win;
    unsigned int msg;
    unsigned long long wparam;
    unsigned long long lparam;
    int type;
    int x;
    int y;
    unsigned int time;
    unsigned int total;
    char pad[4];
};

struct horizon_hw_mouse_input
{
    int type;
    int x;
    int y;
    unsigned int data;
    unsigned int flags;
    unsigned int time;
    unsigned long long info;
};

/* union hw_input's kbd */
struct horizon_hw_keyboard_input
{
    int type;
    unsigned short vkey;
    unsigned short scan;
    unsigned int flags;
    unsigned int time;
    unsigned long long info;
};

union horizon_hw_input
{
    int type;
    struct horizon_hw_mouse_input mouse;
    struct horizon_hw_keyboard_input kbd;
    unsigned char raw[40];
};

struct horizon_send_hardware_message_request
{
    struct horizon_server_request_header header;
    unsigned int win;
    union horizon_hw_input input;
    unsigned int flags;
    char pad[4];
};

struct horizon_send_hardware_message_reply
{
    struct horizon_server_reply_header header;
    int wait;
    int prev_x;
    int prev_y;
    int new_x;
    int new_y;
    char pad[4];
};

struct horizon_accept_hardware_message_request
{
    struct horizon_server_request_header header;
    unsigned int hw_id;
};

struct horizon_get_key_state_request
{
    struct horizon_server_request_header header;
    int async;
    int key;
    char pad[4];
};

struct horizon_get_key_state_reply
{
    struct horizon_server_reply_header header;
    unsigned char state;
    char pad[7];
};

struct horizon_hardware_msg_data
{
    unsigned long long info;
    unsigned int size;
    int pad;
    unsigned int hw_id;
    unsigned int flags;
    struct
    {
        unsigned int device;
        unsigned int origin;
    } source;
    struct
    {
        int type;
        unsigned int device;
        unsigned int wparam;
        unsigned int usage;
    } rawinput;
};

struct horizon_obj_locator
{
    unsigned long long id;
    unsigned long long offset;
};

struct horizon_get_thread_input_request
{
    struct horizon_server_request_header header;
    unsigned int tid;
};

struct horizon_get_thread_input_reply
{
    struct horizon_server_reply_header header;
    struct horizon_obj_locator locator;
};

struct horizon_input_window_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_set_foreground_window_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int internal;
    char pad[4];
};

struct horizon_set_foreground_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int previous;
    int send_msg_old;
    int send_msg_new;
    char pad[4];
};

struct horizon_input_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int previous;
    char pad[4];
};

struct horizon_set_capture_window_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int flags;
    char pad[4];
};

struct horizon_set_capture_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int previous;
    unsigned int full_handle;
};

struct horizon_set_caret_window_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int width;
    int height;
};

struct horizon_set_caret_window_reply
{
    struct horizon_server_reply_header header;
    unsigned int previous;
    struct horizon_rectangle old_rect;
    int old_hide;
    int old_state;
    char pad[4];
};

struct horizon_set_caret_info_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    unsigned int handle;
    int x;
    int y;
    int hide;
    int state;
    char pad[4];
};

struct horizon_set_caret_info_reply
{
    struct horizon_server_reply_header header;
    unsigned int full_handle;
    struct horizon_rectangle old_rect;
    int old_hide;
    int old_state;
    char pad[4];
};

struct horizon_set_cursor_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    unsigned int handle;
    int show_count;
    int x;
    int y;
    struct horizon_rectangle clip;
};

struct horizon_set_cursor_reply
{
    struct horizon_server_reply_header header;
    unsigned int prev_handle;
    int prev_count;
    int prev_x;
    int prev_y;
    int new_x;
    int new_y;
    struct horizon_rectangle new_clip;
    unsigned int last_change;
    char pad[4];
};

struct horizon_alloc_user_handle_request
{
    struct horizon_server_request_header header;
    unsigned short type;
    char pad[2];
};

struct horizon_alloc_user_handle_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_free_user_handle_request
{
    struct horizon_server_request_header header;
    unsigned short type;
    char pad0[2];
    unsigned int handle;
    char pad1[4];
};

struct horizon_update_window_zorder_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    struct horizon_rectangle rect;
};

struct horizon_redraw_window_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    unsigned int flags;
    char pad[4];
};

struct horizon_set_window_property_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    unsigned long long data;
    unsigned short atom;
    char pad[6];
};

struct horizon_window_property_request
{
    struct horizon_server_request_header header;
    unsigned int window;
    unsigned short atom;
    char pad[6];
};

struct horizon_window_property_reply
{
    struct horizon_server_reply_header header;
    unsigned long long data;
};

struct horizon_get_window_properties_request
{
    struct horizon_server_request_header header;
    unsigned int window;
};

struct horizon_get_window_properties_reply
{
    struct horizon_server_reply_header header;
    int total;
    char pad[4];
};

struct horizon_property_data
{
    unsigned short atom;
    char pad[2];
    int string;
    unsigned long long data;
};

struct horizon_create_winstation_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    unsigned int access;
    unsigned int attributes;
    unsigned int rootdir;
    char pad[4];
};

struct horizon_winstation_handle_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char pad[4];
};

struct horizon_set_winstation_monitors_request
{
    struct horizon_server_request_header header;
    int increment;
};

struct horizon_set_winstation_monitors_reply
{
    struct horizon_server_reply_header header;
    unsigned long long serial;
};

struct horizon_set_process_winstation_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_enum_winstation_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_enum_winstation_reply
{
    struct horizon_server_reply_header header;
    unsigned int count;
    unsigned int total;
};

struct horizon_create_desktop_request
{
    struct horizon_server_request_header header;
    unsigned int flags;
    unsigned int access;
    unsigned int attributes;
};

struct horizon_open_desktop_request
{
    struct horizon_server_request_header header;
    unsigned int winsta;
    unsigned int flags;
    unsigned int access;
    unsigned int attributes;
    char pad[4];
};

struct horizon_get_thread_desktop_request
{
    struct horizon_server_request_header header;
    unsigned int tid;
};

struct horizon_get_thread_desktop_reply
{
    struct horizon_server_reply_header header;
    struct horizon_obj_locator locator;
    unsigned int handle;
    char pad[4];
};

struct horizon_set_thread_desktop_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_set_thread_desktop_reply
{
    struct horizon_server_reply_header header;
    struct horizon_obj_locator locator;
};

struct horizon_set_user_object_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int flags;
    unsigned int obj_flags;
    long long close_timeout;
};

struct horizon_set_user_object_info_reply
{
    struct horizon_server_reply_header header;
    int is_desktop;
    unsigned int old_obj_flags;
};

struct horizon_create_class_request
{
    struct horizon_server_request_header header;
    int local;
    unsigned int atom;
    unsigned int style;
    unsigned long long instance;
    unsigned long long client_ptr;
    short cls_extra;
    short win_extra;
    unsigned int name_offset;
};

struct horizon_create_class_reply
{
    struct horizon_server_reply_header header;
    struct horizon_obj_locator locator;
    unsigned int atom;
    char pad[4];
};

struct horizon_destroy_class_request
{
    struct horizon_server_request_header header;
    unsigned int atom;
    unsigned long long instance;
};

struct horizon_select_wait_op
{
    int op;
    unsigned int handles[1];
};

struct horizon_select_signal_and_wait_op
{
    int op;
    unsigned int wait;
    unsigned int signal;
};

struct horizon_user_entry
{
    unsigned long long offset;
    unsigned int tid;
    unsigned int pid;
    unsigned long long id;
    union
    {
        struct
        {
            unsigned short type;
            unsigned short generation;
        };
        long long uniq;
    };
};

struct horizon_shared_cursor
{
    int x;
    int y;
    unsigned int last_change;
    struct horizon_rectangle clip;
};

struct horizon_desktop_shm
{
    unsigned int flags;
    struct horizon_shared_cursor cursor;
    unsigned char keystate[256];
    unsigned long long monitor_serial;
    unsigned long long keystate_serial;
};

struct horizon_input_shm
{
    int foreground;
    unsigned int active;
    unsigned int focus;
    unsigned int capture;
    unsigned int menu_owner;
    unsigned int move_size;
    unsigned int caret;
    struct horizon_rectangle caret_rect;
    unsigned int cursor;
    int cursor_count;
    unsigned char keystate[256];
    int keystate_lock;
    unsigned long long keystate_serial;
};

struct horizon_class_shm
{
    unsigned int atom;
    unsigned int style;
    unsigned int cls_extra;
    unsigned int win_extra;
    unsigned long long instance;
    unsigned int name_offset;
    unsigned int name_len;
    unsigned short name[HORIZON_MAX_ATOM_LEN];
    unsigned short pad;
    char extra[];
};

struct horizon_window_shm
{
    struct horizon_obj_locator class;
    unsigned int dpi_context;
};

/* queue_shm_t. Its access time stays 0, so win32u always asks get_message
 * instead of trusting the bits to skip it; hooks are not supported. */
struct horizon_queue_shm
{
    long long access_time;
    unsigned int wake_mask;
    unsigned int wake_bits;
    unsigned int changed_mask;
    unsigned int changed_bits;
    unsigned int internal_bits;
    int hooks_count[16];
};

union horizon_object_shm
{
    struct horizon_desktop_shm desktop;
    struct horizon_queue_shm queue;
    struct horizon_input_shm input;
    struct horizon_class_shm class;
    struct horizon_window_shm window;
};

struct horizon_shared_object
{
    long long seq;
    unsigned long long id;
    union horizon_object_shm shm;
};

struct horizon_session_shm
{
    struct horizon_user_entry user_entries[HORIZON_MAX_USER_HANDLES];
};

struct horizon_atom_entry
{
    unsigned int atom;
    unsigned int name_len;
    unsigned char *name;
    struct horizon_atom_entry *next;
};

struct horizon_user_class
{
    int local;
    unsigned int atom;
    unsigned int base_atom;
    unsigned int style;
    unsigned long long instance;
    unsigned long long client_ptr;
    int cls_extra;
    int win_extra;
    unsigned int name_len;
    unsigned char *name;
    struct horizon_obj_locator locator;
    struct horizon_user_class *next;
};

struct horizon_window_property
{
    unsigned short atom;
    unsigned int string;
    unsigned long long data;
    struct horizon_window_property *next;
};

struct horizon_input_message
{
    unsigned int id;
    unsigned int tid;
    unsigned int win;
    unsigned int msg;
    unsigned long long wparam;
    unsigned long long lparam;
    int x;
    int y;
    unsigned int time;
    unsigned long long info;
    unsigned int device;             /* HORIZON_IMDT_*; 0 is the mouse */
    unsigned int data_flags;         /* hardware_msg_data.flags */
    int raw_keyboard;                /* WM_INPUT, carrying raw */
    struct horizon_raw_keyboard raw;
    int raw_mouse;                   /* WM_INPUT, carrying raw_m */
    struct horizon_raw_mouse raw_m;
    struct horizon_input_message *next;
};

struct horizon_user_window
{
    unsigned int handle;
    unsigned int parent;
    unsigned int owner;
    unsigned int pid;
    unsigned int tid;
    unsigned int atom;
    unsigned int style;
    unsigned int ex_style;
    unsigned int is_unicode;
    unsigned int id;
    unsigned int monitor_dpi;
    unsigned long long instance;
    unsigned long long user_data;
    unsigned int last_active;
    unsigned int desktop_handle;
    struct horizon_rectangle window_rect;
    struct horizon_rectangle client_rect;
    struct horizon_rectangle visible_rect;
    struct horizon_rectangle surface_rect;
    struct horizon_rectangle update_rect;
    unsigned int paint_flags;
    unsigned int has_update_rect;
    unsigned int has_internal_paint;
    unsigned int needs_erase;
    unsigned int needs_nonclient;
    struct horizon_user_class *class;
    struct horizon_window_property *properties;
    struct horizon_obj_locator locator;
    struct horizon_user_window *next;
};

struct horizon_session_view
{
    unsigned long long base;
    unsigned long long offset;
    unsigned long long size;
    struct horizon_session_view *next;
};

struct horizon_server_object;

struct horizon_server_connection
{
    int request_fd;
    int reply_fd;
    int wait_fd;
    unsigned int pid;
    unsigned int tid;
    /* Thread object of the client; the connection holds one reference until
     * the client's request pipe closes, which is when the thread terminates. */
    struct horizon_server_object *thread;
    /* The client's request pipe, and the cores this thread last followed it to. */
    struct horizon_pipe *request_pipe;
    unsigned int core_mask;
    /* The completion wait object remove_completion gave this client (one
     * reference) and the handle the client selects on; server/completion.c
     * keeps both in the thread. */
    struct horizon_server_object *completion_wait;
    unsigned int completion_wait_handle;
};

/* An apc_call is a few dozen bytes; anything larger is not one. */
#define HORIZON_USER_APC_MAX 512

/* A user APC a thread has been given and has not yet waited alertably for:
 * ReadFileEx's completion routine, QueueUserAPC's function. The call is the
 * client's own apc_call, kept as it arrived and handed back untouched, so the
 * server needs to know nothing of its shape. */
struct horizon_user_apc
{
    struct horizon_user_apc *next;
    unsigned int size;
    unsigned char call[1];
};

struct horizon_server_object
{
    unsigned int id;
    int type;
    unsigned int refs;
    int manual_reset;
    int signaled;
    unsigned int count;
    unsigned int max;
    struct horizon_mutex_state mutex;
    struct horizon_thread_state thread;
    struct horizon_server_object *thread_next;
    struct horizon_user_apc *apc_first, *apc_last;
    unsigned int rootdir;
    unsigned int name_len;
    unsigned char *name;
    long long timer_when;
    unsigned int timer_period;
    int file_fd;
    char *file_name;
    unsigned int file_access;
    unsigned int file_options;
    int file_delete;                /* FileDispositionInformation asked for deletion at last close */
    unsigned int file_sharing;      /* FILE_SHARE_* it was opened with, when file_shared */
    int file_shared;                /* opened by create_file, whose sharing mode is known */
    int file_is_dir;
    unsigned int dir_enum_index;
    int dir_queried;                /* a directory query has run on this handle */
    char *dir_mask;
    int sock_nonblocking;
    int sock_bound;                 /* bind() succeeded; Windows fails a second bind */
    int sock_family;                /* what the program asked for: WS AF_INET, or AF_INET6 on IPv4 */
    int sock_type, sock_protocol;
    int sock_v6only;                /* IPV6_V6ONLY: an IPv6 socket does not reach ::ffff:a.b.c.d */
    unsigned int sock_event_handle; /* event signaled by the poller (WSAEventSelect) */
    int sock_event_mask;            /* AFD_POLL_* bits the app asked for */
    int sock_pending_events;        /* accumulated AFD_POLL_* bits not yet fetched */
    int sock_connect_status;        /* NTSTATUS for AFD_POLL_CONNECT_ERR */
    int sock_err_ticks;             /* consecutive poller ticks with SO_ERROR set */
    unsigned int mapping_flags;
    unsigned int mapping_access;
    unsigned int mapping_file_access;
    unsigned long long mapping_size;
    int mapping_has_image;
    int mapping_is_session;
    struct horizon_pe_image_info mapping_image;
    unsigned int user_flags;
    unsigned int desktop_winstation;
    unsigned int desktop_top_window;
    unsigned int desktop_msg_window;
    struct horizon_obj_locator desktop_locator;
    struct horizon_reg_key *reg_key;
    int std_stream;                 /* 1 stdout, 2 stderr: writes are echoed to the log */
    unsigned int queue_tid;         /* message queue object: its thread, 0 once the thread ended */
    int directory;                  /* directory object: enum horizon_object_dir */
    struct horizon_completion_queue completion; /* completion port: queued messages */
    int completion_closed;          /* completion port: its last handle was closed */
    struct horizon_server_object *wait_port; /* completion wait: the port it waits on, referenced */
    struct horizon_completion_msg wait_msg;  /* completion wait: the message the wait took */
    int wait_has_msg;
    struct horizon_server_object *file_completion; /* file: the port its I/O results go to, referenced */
    unsigned long long file_completion_key;
    unsigned int file_completion_flags;  /* FILE_SKIP_* */
    struct horizon_memfile *mapping_memfile; /* mapping: a section with no file, kept by file_fd */
    struct horizon_server_object *mapping_shared; /* image mapping: file object backing its writable
                                                   * shared sections, as wineserver's shared_map */
};

struct horizon_server_handle_entry
{
    unsigned int handle;
    struct horizon_server_object *object;
    struct horizon_server_handle_entry *next;
    struct horizon_server_handle_entry **pprev;     /* what points at it in horizon_server_handles */
    struct horizon_server_handle_entry *hash_next;  /* in its horizon_server_handle_hash bucket */
};

static struct horizon_reg horizon_registry;

static LONG horizon_server_next_handle = 0x100;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Pending selects and thread start gates sleep here, releasing the object lock;
 * each change that can signal an object wakes them. */
static pthread_cond_t horizon_server_objects_cond = PTHREAD_COND_INITIALIZER;
static unsigned int horizon_server_sleepers;  /* guarded by horizon_server_objects_mutex */

/* Longest sleep between rechecks, a safety net for changes nothing wakes (100ns). */
#define HORIZON_SERVER_WAIT_SLICE    200000LL
/* Message queues become signaled without a wakeup (window timers, input): waits
 * on them recheck this often (100ns). */
#define HORIZON_SERVER_POLL_INTERVAL 10000LL

/* Called with horizon_server_objects_mutex held after an object may have become
 * signaled or a suspended thread may start. */
static void horizon_server_signal_changed_locked(void)
{
    if (horizon_server_sleepers) pthread_cond_broadcast( &horizon_server_objects_cond );
}

/* Sleeps with horizon_server_objects_mutex held until an object changes or
 * timeout (100ns, at most HORIZON_SERVER_WAIT_SLICE) passes. */
static void horizon_server_sleep_locked( long long timeout )
{
    if (timeout > HORIZON_SERVER_WAIT_SLICE) timeout = HORIZON_SERVER_WAIT_SLICE;
    horizon_server_sleepers++;
    /* libnx's relative wait: newlib's pthread_cond_timedwait reads the realtime
     * clock, which fails until the time service has set the boot time. */
    condvarWaitTimeout( &horizon_server_objects_cond.cond, &horizon_server_objects_mutex.normal,
                        (u64)timeout * 100 );
    horizon_server_sleepers--;
}
/* A server thread waiting for a client's objects holds the object lock while it
 * sleeps in slices. Asked to stop, it has to let the lock go first: parked or
 * ended holding it, no other thread could reach its own stopping place. */
static void horizon_server_quit_check_locked( void )
{
    extern volatile int wine_nx_quit_requested __attribute__((weak));
    extern void wine_nx_quit_point( void ) __attribute__((weak));

    if (!&wine_nx_quit_requested || !wine_nx_quit_requested || !&wine_nx_quit_point) return;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    wine_nx_quit_point();
    pthread_mutex_lock( &horizon_server_objects_mutex );
}

static struct horizon_server_handle_entry *horizon_server_handles;
/* Whether any waitable timer is running, so waits skip the walk otherwise. */
static int horizon_server_timers_armed;
/* The same entries by handle value, which only grows. Requests look their
 * handles up, and walking every open handle made each lookup slower as the
 * number of handles grew. */
#define HORIZON_SERVER_HANDLE_HASH_SIZE 4096
static struct horizon_server_handle_entry *horizon_server_handle_hash[HORIZON_SERVER_HANDLE_HASH_SIZE];
/* Every thread object, running or terminated, while referenced (open_thread). */
static struct horizon_server_object *horizon_server_threads;
static unsigned int horizon_server_running_threads;
/* Each client connection is served by its own pthread. */
static __thread struct horizon_server_connection *horizon_server_current;
static struct horizon_zombie_list horizon_server_zombies = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };
struct horizon_lifecycle_counters horizon_lifecycle;
static unsigned int horizon_process_winstation;
static unsigned int horizon_thread_desktop;
static unsigned int horizon_input_desktop;
static int horizon_session_fd = -1;
static unsigned char *horizon_session_data;
static unsigned long long horizon_session_used = sizeof(struct horizon_session_shm);
static unsigned long long horizon_session_next_id;
static unsigned int horizon_user_handle_count;
static unsigned int horizon_next_atom = 0xc000;
static struct horizon_atom_entry *horizon_atoms;
static struct horizon_user_class *horizon_classes;
static struct horizon_user_window *horizon_windows;
static struct horizon_session_view *horizon_session_views;
static struct horizon_obj_locator horizon_input_locator;
static struct horizon_input_message *horizon_input_messages;
static struct horizon_input_message **horizon_input_messages_tail = &horizon_input_messages;
/* update_rawinput_devices: the registrations of the one process that runs. */
static struct horizon_rawinput_device *horizon_rawinput_devices;
static unsigned int horizon_rawinput_device_count;
static int horizon_alt_pressed;  /* wineserver's desktop->alt_pressed */
static unsigned int horizon_next_input_message_id = 1;
static unsigned int horizon_mouse_buttons;
/* The last place a touch pointed at, which the next one is a movement from. */
#define HORIZON_NOWHERE 0x7fffffff
static int horizon_pointed_at_x = HORIZON_NOWHERE, horizon_pointed_at_y = HORIZON_NOWHERE;
static struct horizon_message_queue horizon_posted_messages = { NULL, &horizon_posted_messages.head };
static struct horizon_win_timers horizon_timers;
static struct horizon_msgqs horizon_msg_queues;
static struct horizon_clipboard horizon_clipboard;
static void horizon_server_clipboard_notify_locked(void);

static int horizon_pipe_open_r( struct _reent *r, void *fdptr, const char *path, int flags, int mode );
static int horizon_pipe_close_r( struct _reent *r, void *fdptr );
static ssize_t horizon_pipe_write_r( struct _reent *r, void *fdptr, const char *ptr, size_t len );
static ssize_t horizon_pipe_read_r( struct _reent *r, void *fdptr, char *ptr, size_t len );
static int horizon_pipe_fstat_r( struct _reent *r, void *fdptr, struct stat *st );

static const devoptab_t horizon_pipe_devoptab =
{
    .name         = "winepipe",
    .structSize   = sizeof(struct horizon_pipe_file *),
    .open_r       = horizon_pipe_open_r,
    .close_r      = horizon_pipe_close_r,
    .write_r      = horizon_pipe_write_r,
    .read_r       = horizon_pipe_read_r,
    .seek_r       = NULL,
    .fstat_r      = horizon_pipe_fstat_r,
    .stat_r       = NULL,
    .link_r       = NULL,
    .unlink_r     = NULL,
    .chdir_r      = NULL,
    .rename_r     = NULL,
    .mkdir_r      = NULL,
    .dirStateSize = 0,
    .diropen_r    = NULL,
    .dirreset_r   = NULL,
    .dirnext_r    = NULL,
    .dirclose_r   = NULL,
    .statvfs_r    = NULL,
    .ftruncate_r  = NULL,
    .fsync_r      = NULL,
    .deviceData   = NULL,
    .chmod_r      = NULL,
    .fchmod_r     = NULL,
    .rmdir_r      = NULL,
    .lstat_r      = NULL,
    .utimes_r     = NULL,
    .fpathconf_r  = NULL,
    .pathconf_r   = NULL,
    .symlink_r    = NULL,
    .readlink_r   = NULL,
};

static pthread_mutex_t horizon_pipe_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static int horizon_pipe_device = -1;
static struct horizon_fd_queue horizon_server_to_client_fds =
{
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    NULL,
    NULL
};
static struct horizon_fd_queue horizon_client_to_server_fds =
{
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    NULL,
    NULL
};

struct horizon_backing
{
    void *heap_addr;
    void *code_addr;
    VirtmemReservation *code_reservation;
    size_t size;
    unsigned int refs;
    int fd;
    off_t file_offset;
    BOOL write_back;
};

/* What a mapping of a section with no file is (horizon_memfile.h). */
enum horizon_section_state
{
    SECTION_NONE,     /* not one: a reservation, or backed memory */
    SECTION_ALIASED,  /* a range of a view, its pages mapped from the section's anchors */
    SECTION_HOLE,     /* a range of a view, unmapped while PROT_NONE */
    SECTION_ANCHOR,   /* section pages code-mapped for views to map from */
};

struct horizon_mapping
{
    void *addr;
    size_t size;
    size_t source_offset;
    int prot;
    struct horizon_backing *backing;
    VirtmemReservation *reservation;
    struct horizon_memfile *section;  /* SECTION_ALIASED and SECTION_HOLE */
    void *anchor_source;              /* SECTION_ANCHOR: the pages it was mapped from */
    size_t section_offset;
    unsigned char section_state;
    struct rb_entry entry;
};

#include "horizon_pool.h"

/* A section's pages are code-mapped as anchors, so they are alias sources too
 * and come from arenas of their own. Memfiles allocate outside mapping_mutex,
 * and a page pool is not itself thread safe. */
static pthread_mutex_t section_pages_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_page_pool section_pages;

static void *section_pages_alloc( size_t size )
{
    void *ptr;

    pthread_mutex_lock( &section_pages_mutex );
    ptr = horizon_pages_alloc_any( &section_pages, size );
    pthread_mutex_unlock( &section_pages_mutex );
    return ptr;
}

static void section_pages_free( void *ptr, size_t size )
{
    int pooled;

    pthread_mutex_lock( &section_pages_mutex );
    pooled = horizon_pages_free( &section_pages, ptr, size );
    pthread_mutex_unlock( &section_pages_mutex );
    if (!pooled) free( ptr );
}

#define HORIZON_MEMFILE_ALLOC_PAGES(size) section_pages_alloc( size )
#define HORIZON_MEMFILE_FREE_PAGES(ptr, size) section_pages_free( ptr, size )
#include "horizon_memfile.h"

/* These freelists are protected by mapping_mutex, like the mapping tree. */
static struct horizon_backing backing_slots[4096];
static struct horizon_mapping mapping_slots[8192];
static struct horizon_object_pool backing_pool = { backing_slots, NULL, sizeof(backing_slots[0]), 4096, 0 };
static struct horizon_object_pool mapping_pool = { mapping_slots, NULL, sizeof(mapping_slots[0]), 8192, 0 };
static struct horizon_page_pool backing_pages;
/* Views of sections with no file (horizon_mmap_section), under mapping_mutex. */
static unsigned long long section_view_maps, section_anchor_bytes;
static unsigned int section_anchors, section_failures;

/*
 * The pages this process has given the kernel as the source of an alias.
 *
 * svcMapProcessCodeMemory and svcMapProcessMemory reprotect their source to
 * Perm_None and mark it MemAttr_IsBorrowed, so a write to one of those pages
 * faults as an unmapped page -- an ESR translation fault, not a protection
 * fault -- for as long as the alias lives. Afterwards svcQueryMemory reports
 * ordinary read-write heap again, and a map that fails after reprotecting the
 * source restores it without the attribute ever being recorded, so the kernel
 * alone cannot tell a fault handler that it hit an alias source. A bit a page
 * can, and the last unmaps say whether one had just gone away.
 */
#define HORIZON_ALIAS_PAGES (1u << 20)  /* 4 GiB of heap, at 4 KB a page */
#define HORIZON_ALIAS_RECENT 64
static unsigned char alias_source_bits[HORIZON_ALIAS_PAGES / 8];
static const char *alias_source_base;
static unsigned int alias_source_seq;
static struct
{
    const void *source;
    const void *addr;
    size_t size;
    unsigned int seq;
} alias_source_recent[HORIZON_ALIAS_RECENT];

static void alias_source_init(void)
{
    extern char *fake_heap_start;

    alias_source_base = fake_heap_start;
}

/* The page's bit index, or ~0 for an address outside the window: an anchor
 * aliased onto a second address is not itself heap, and is not tracked. */
static size_t alias_source_page( const void *addr )
{
    size_t page;

    if (!alias_source_base || (const char *)addr < alias_source_base) return ~(size_t)0;
    page = ((uintptr_t)addr - (uintptr_t)alias_source_base) / 0x1000;
    return page < HORIZON_ALIAS_PAGES ? page : ~(size_t)0;
}

/* Records a range as the source of an alias, or as one no longer aliased.
 * Called after the kernel agrees, from any thread. */
static void note_alias_source( const void *source, const void *addr, size_t size, BOOL mapped )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    size_t page, i, pages = size / 0x1000;

    pthread_once( &once, alias_source_init );
    if ((page = alias_source_page( source )) == ~(size_t)0) return;
    if (page + pages > HORIZON_ALIAS_PAGES) return;

    for (i = 0; i < pages; i++)
    {
        unsigned char mask = 1u << ((page + i) & 7);
        unsigned char *byte = &alias_source_bits[(page + i) >> 3];

        if (mapped) __atomic_or_fetch( byte, mask, __ATOMIC_RELAXED );
        else __atomic_and_fetch( byte, (unsigned char)~mask, __ATOMIC_RELAXED );
    }
    if (!mapped)
    {
        unsigned int slot = __atomic_fetch_add( &alias_source_seq, 1, __ATOMIC_RELAXED ), i = slot % HORIZON_ALIAS_RECENT;

        /* A fault handler reads these while another thread writes them, so the
         * source, which is what it matches on, is published last. */
        __atomic_store_n( &alias_source_recent[i].source, NULL, __ATOMIC_RELAXED );
        alias_source_recent[i].addr = addr;
        alias_source_recent[i].size = size;
        alias_source_recent[i].seq = slot;
        __atomic_store_n( &alias_source_recent[i].source, source, __ATOMIC_RELEASE );
    }
}

/* Whether any page of the range is the source of an alias right now. */
static BOOL alias_source_range_live( const void *addr, size_t size )
{
    size_t page = alias_source_page( addr ), i, pages = size / 0x1000;

    if (page == ~(size_t)0 || page + pages > HORIZON_ALIAS_PAGES) return FALSE;
    for (i = 0; i < pages; i++)
        if (alias_source_bits[(page + i) >> 3] & (1u << ((page + i) & 7))) return TRUE;
    return FALSE;
}

/* Appends what the faulting address was to a trace line. */
static void alias_source_report( const void *addr, char *buffer, size_t size )
{
    size_t page = alias_source_page( addr ), len = strlen( buffer );
    unsigned int seq, i;

    if (page == ~(size_t)0)
    {
        snprintf( buffer + len, size - len, " alias=untracked" );
        return;
    }
    if (alias_source_bits[page >> 3] & (1u << (page & 7)))
    {
        snprintf( buffer + len, size - len, " alias=live" );
        return;
    }
    seq = __atomic_load_n( &alias_source_seq, __ATOMIC_RELAXED );
    for (i = 0; i < HORIZON_ALIAS_RECENT; i++)
    {
        const void *source = __atomic_load_n( &alias_source_recent[i].source, __ATOMIC_ACQUIRE );

        if (!source) continue;
        if ((uintptr_t)addr - (uintptr_t)source >= alias_source_recent[i].size) continue;
        /* Unmaps since this one: the alias the fault most likely raced went
         * away last, at age zero. */
        snprintf( buffer + len, size - len, " alias=unmapped src=%p dst=%p size=%#lx age=%u",
                  source, alias_source_recent[i].addr,
                  (unsigned long)alias_source_recent[i].size,
                  seq - 1 - alias_source_recent[i].seq );
        return;
    }
    snprintf( buffer + len, size - len, " alias=none" );
}

void horizon_memory_pool_stats( char *buffer, size_t size )
{
    pthread_mutex_lock( &mapping_mutex );
    snprintf( buffer, size, "[MEMPOOL] arena_mb=%llu pooled_allocs=%llu block_allocs=%llu shared_allocs=%llu"
              " backing_slots=%zu mapping_slots=%zu section_views=%llu anchors=%u anchor_mb=%llu section_failures=%u",
              (backing_pages.misses + section_pages.misses) * 2,
              backing_pages.hits + section_pages.hits,
              backing_pages.blocks + section_pages.blocks,
              backing_pages.shared + section_pages.shared,
              backing_pool.used, mapping_pool.used, section_view_maps, section_anchors,
              section_anchor_bytes >> 20, section_failures );
    pthread_mutex_unlock( &mapping_mutex );
}

static int compare_mapping( const void *addr, const struct rb_entry *entry )
{
    struct horizon_mapping *mapping = RB_ENTRY_VALUE( entry, struct horizon_mapping, entry );

    if (addr < mapping->addr) return -1;
    if (addr > mapping->addr) return 1;
    return 0;
}

/* The mappings by address. They never overlap, so their ends are in the order
 * of their starts. */
static struct rb_tree mappings = { compare_mapping, NULL };

static int lowest_set_core( ULONG_PTR mask )
{
    int i;

    for (i = 0; i < (int)(sizeof(mask) * 8); i++)
        if (mask & ((ULONG_PTR)1 << i)) return i;

    return 0;
}

static ULONG_PTR nth_core_mask( ULONG_PTR mask, LONG index )
{
    LONG seen = 0;
    int i;

    for (i = 0; i < (int)(sizeof(mask) * 8); i++)
    {
        ULONG_PTR bit = (ULONG_PTR)1 << i;

        if (!(mask & bit)) continue;
        if (seen++ == index) return bit;
    }

    return (ULONG_PTR)1 << lowest_set_core( mask );
}

static unsigned int count_mask_bits( ULONG_PTR mask )
{
    unsigned int count = 0;

    while (mask)
    {
        count += mask & 1;
        mask >>= 1;
    }

    return count;
}

ULONG_PTR horizon_get_system_affinity_mask(void)
{
    u64 mask = 0;
    Result rc;

    if (cached_affinity_mask) return cached_affinity_mask;

    rc = svcGetInfo( &mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0 );
    if (R_FAILED(rc) || !mask)
    {
        WARN( "svcGetInfo(InfoType_CoreMask) failed %#x, falling back to applet cores.\n", rc );
        mask = 0x7;
    }

    cached_affinity_mask = (ULONG_PTR)mask;
    TRACE( "Horizon system affinity mask %#lx.\n", (unsigned long)cached_affinity_mask );
    return cached_affinity_mask;
}

unsigned int horizon_get_processor_count(void)
{
    unsigned int count = count_mask_bits( horizon_get_system_affinity_mask() );

    return count ? count : 1;
}

/* There is no sysinfo or /proc/meminfo on Horizon. The memory this process may
 * use is the meaningful "physical memory" for Windows programs, which size
 * caches, dictionaries and thread counts from GlobalMemoryStatusEx. */
void horizon_get_memory_info( unsigned long long *total, unsigned long long *used )
{
    extern char *fake_heap_start, *fake_heap_end;
    unsigned long long heap_size, heap_free;
    struct mallinfo heap;
    u64 value;

    *total = R_SUCCEEDED( svcGetInfo( &value, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0 ) ) ? value : 0;
    *used = R_SUCCEEDED( svcGetInfo( &value, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0 ) ) ? value : 0;
    if (*used > *total) *used = *total;

    /* InfoType_UsedMemorySize counts the whole heap, which libnx claims from
     * Horizon at startup, so it reports a few megabytes free however little the
     * process has allocated. What a program can still get is what malloc holds
     * unused plus what it has not taken from the heap, the same figure the
     * runtime prints as heap_free_mb. Without this, GlobalMemoryStatusEx
     * reported 4 MB of AvailPageFile and Fallout New Vegas, which wants 512 MB,
     * quit with "Not enough memory to run application." */
    heap = mallinfo();
    heap_size = fake_heap_end > fake_heap_start ? (unsigned long long)(fake_heap_end - fake_heap_start) : 0;
    heap_free = (unsigned long long)heap.fordblks +
                (heap_size > (unsigned long long)heap.arena ? heap_size - (unsigned long long)heap.arena : 0);
    /* mallinfo's fields are narrower than the heap on a 32-bit address space,
     * so bound the result by the heap and by what Horizon reports. */
    if (heap_free > heap_size) heap_free = heap_size;
    if (heap_free > *total) heap_free = *total;
    if (*total - heap_free < *used) *used = *total - heap_free;
}

/* The pipe behind a descriptor from horizon_pipe, or NULL. */
static struct horizon_pipe *horizon_pipe_from_fd( int fd )
{
    struct horizon_pipe_file *file;
    __handle *handle;

    if (fd < 0 || horizon_pipe_device == -1 || !(handle = __get_handle( fd ))) return NULL;
    if (handle->device != (unsigned int)horizon_pipe_device || !handle->fileStruct) return NULL;
    file = *(struct horizon_pipe_file **)handle->fileStruct;
    return file ? file->pipe : NULL;
}

void horizon_pin_current_thread( ULONG_PTR requested_mask )
{
    ULONG_PTR system_mask = horizon_get_system_affinity_mask();
    ULONG_PTR mask = requested_mask & system_mask;
    struct horizon_pipe *pipe;
    LONG index;
    int preferred;
    Result rc;

    if (!mask)
    {
        unsigned int count = horizon_get_processor_count();

        index = InterlockedIncrement( &next_core_index ) - 1;
        mask = nth_core_mask( system_mask, index % count );
    }
    else if (wine_nx_thread_affinity_fixed) wine_nx_thread_affinity_fixed();

    preferred = lowest_set_core( mask );
    rc = svcSetThreadCoreMask( CUR_THREAD_HANDLE, preferred, (u32)mask );
    if (R_FAILED(rc))
    {
        WARN( "svcSetThreadCoreMask(preferred %u, mask %#lx) failed %#x.\n",
              preferred, (unsigned long)mask, rc );
        return;
    }
    TRACE( "pinned current thread to preferred %u, mask %#lx.\n", preferred, (unsigned long)mask );
    /* Its server connection thread follows it (horizon_server_follow_client). */
    if ((pipe = horizon_pipe_from_fd( ntdll_get_thread_data()->request_fd )))
        __atomic_store_n( &pipe->client_cores, (unsigned int)mask, __ATOMIC_RELAXED );
}

/* The runtime's core balancer moved a Wine thread (wine_nx_thread_balance,
 * which holds the thread registered, so its TEB is there): its connection
 * thread follows it from the next request. */
void horizon_follow_thread_cores( void *teb, unsigned int mask )
{
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&((TEB *)teb)->GdiTebBatch;
    struct horizon_pipe *pipe;

    if ((pipe = horizon_pipe_from_fd( thread_data->request_fd )))
        __atomic_store_n( &pipe->client_cores, mask, __ATOMIC_RELAXED );
}

static void horizon_set_reent_errno( struct _reent *r, int error )
{
    if (r) r->_errno = error;
    else errno = error;
}

/* The descriptors of sections with no file (horizon_memfile.h). */
static int horizon_memfile_open_r( struct _reent *r, void *fdptr, const char *path, int flags, int mode )
{
    (void)fdptr;
    (void)path;
    (void)flags;
    (void)mode;
    horizon_set_reent_errno( r, ENOSYS );
    return -1;
}

/* newlib calls this once the last descriptor duplicated from one is closed;
 * views keep the memory for as long as they need it. */
static int horizon_memfile_close_r( struct _reent *r, void *fdptr )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;

    (void)r;
    if (file) horizon_memfile_unref( file );
    *(struct horizon_memfile **)fdptr = NULL;
    return 0;
}

static ssize_t horizon_memfile_write_r( struct _reent *r, void *fdptr, const char *ptr, size_t len )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;
    ssize_t ret;

    if (!file) ret = -EBADF;
    else ret = horizon_memfile_write( file, ptr, len );
    if (ret >= 0) return ret;
    horizon_set_reent_errno( r, -ret );
    return -1;
}

static ssize_t horizon_memfile_read_r( struct _reent *r, void *fdptr, char *ptr, size_t len )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;
    ssize_t ret;

    if (!file) ret = -EBADF;
    else ret = horizon_memfile_read( file, ptr, len );
    if (ret >= 0) return ret;
    horizon_set_reent_errno( r, -ret );
    return -1;
}

static off_t horizon_memfile_seek_r( struct _reent *r, void *fdptr, off_t pos, int dir )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;
    off_t ret;

    if (!file) ret = -EBADF;
    else ret = horizon_memfile_seek( file, pos, dir );
    if (ret >= 0) return ret;
    horizon_set_reent_errno( r, -ret );
    return -1;
}

static int horizon_memfile_fstat_r( struct _reent *r, void *fdptr, struct stat *st )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;

    if (!file)
    {
        horizon_set_reent_errno( r, EBADF );
        return -1;
    }
    horizon_memfile_stat( file, st );
    return 0;
}

static int horizon_memfile_ftruncate_r( struct _reent *r, void *fdptr, off_t len )
{
    struct horizon_memfile *file = *(struct horizon_memfile **)fdptr;
    int ret;

    if (!file) ret = -EBADF;
    else ret = horizon_memfile_truncate( file, len );
    if (!ret) return 0;
    horizon_set_reent_errno( r, -ret );
    return -1;
}

static const devoptab_t horizon_memfile_devoptab =
{
    .name        = "winemem",
    .structSize  = sizeof(struct horizon_memfile *),
    .open_r      = horizon_memfile_open_r,
    .close_r     = horizon_memfile_close_r,
    .write_r     = horizon_memfile_write_r,
    .read_r      = horizon_memfile_read_r,
    .seek_r      = horizon_memfile_seek_r,
    .fstat_r     = horizon_memfile_fstat_r,
    .ftruncate_r = horizon_memfile_ftruncate_r,
};

static pthread_mutex_t horizon_memfile_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static int horizon_memfile_device = -1;

/* The kernel side of views of sections (horizon_section_anchor and below). */
static void *horizon_section_anchor( void *source, size_t size, void **token );
static int horizon_section_unanchor( void *addr, void *source, size_t size, void *token );
static int horizon_section_alias( void *dst, void *src, size_t size );
static int horizon_section_unalias( void *dst, void *src, size_t size );

static const struct horizon_memfile_ops horizon_section_ops =
{
    horizon_section_anchor,
    horizon_section_unanchor,
    horizon_section_alias,
    horizon_section_unalias,
};

/* A descriptor on size zeroed bytes, whole pages, or -1 with errno set. */
static int horizon_memfile_create( unsigned long long size, int reserve )
{
    struct horizon_memfile *file;
    int fd;

    pthread_mutex_lock( &horizon_memfile_device_mutex );
    if (horizon_memfile_device == -1)
    {
        horizon_memfile_device = FindDevice( "winemem:" );
        if (horizon_memfile_device == -1)
            horizon_memfile_device = AddDevice( &horizon_memfile_devoptab );
    }
    pthread_mutex_unlock( &horizon_memfile_device_mutex );

    if (horizon_memfile_device == -1)
    {
        errno = EMFILE;
        return -1;
    }
    if (!(file = horizon_memfile_alloc( size, reserve, &horizon_section_ops ))) return -1;
    if ((fd = __alloc_handle( horizon_memfile_device )) == -1)
    {
        horizon_memfile_unref( file );
        errno = EMFILE;
        return -1;
    }
    *(struct horizon_memfile **)__get_handle( fd )->fileStruct = file;
    return fd;
}

/* The section behind a descriptor from horizon_memfile_create, or NULL. */
static struct horizon_memfile *horizon_memfile_from_fd( int fd )
{
    __handle *handle;

    if (fd < 0 || horizon_memfile_device == -1 || !(handle = __get_handle( fd ))) return NULL;
    if (handle->device != (unsigned int)horizon_memfile_device || !handle->fileStruct) return NULL;
    return *(struct horizon_memfile **)handle->fileStruct;
}

static int horizon_pipe_open_r( struct _reent *r, void *fdptr, const char *path, int flags, int mode )
{
    (void)fdptr;
    (void)path;
    (void)flags;
    (void)mode;
    horizon_set_reent_errno( r, ENOSYS );
    return -1;
}

static void horizon_pipe_destroy( struct horizon_pipe *pipe )
{
    pthread_cond_destroy( &pipe->can_write );
    pthread_cond_destroy( &pipe->can_read );
    pthread_mutex_destroy( &pipe->mutex );
    free( pipe );
    __atomic_sub_fetch( &horizon_lifecycle.pipes, 1, __ATOMIC_RELAXED );
}

static void horizon_pipe_release( struct horizon_pipe *pipe )
{
    int destroy = 0;

    pthread_mutex_lock( &pipe->mutex );
    destroy = --pipe->refs == 0;
    pthread_mutex_unlock( &pipe->mutex );

    if (destroy) horizon_pipe_destroy( pipe );
}

static int horizon_pipe_close_r( struct _reent *r, void *fdptr )
{
    struct horizon_pipe_file *file = *(struct horizon_pipe_file **)fdptr;
    struct horizon_pipe *pipe;

    (void)r;
    if (!file) return 0;

    pipe = file->pipe;
    pthread_mutex_lock( &pipe->mutex );
    if (file->write_end) pipe->write_open = 0;
    else pipe->read_open = 0;
    pthread_cond_broadcast( &pipe->can_read );
    pthread_cond_broadcast( &pipe->can_write );
    pthread_mutex_unlock( &pipe->mutex );

    free( file );
    *(struct horizon_pipe_file **)fdptr = NULL;
    horizon_pipe_release( pipe );
    return 0;
}

/* Horizon's condition variables cannot be interrupted, so a thread asked to stop
 * would wait in one for ever. Wait in slices, and look between them, holding
 * nothing while it does: a thread that parked or ended with the lock would stop
 * the others from reaching their own quit point. */
static void horizon_cond_wait_quit( pthread_cond_t *cond, pthread_mutex_t *mutex )
{
    extern volatile int wine_nx_quit_requested __attribute__((weak));
    extern void wine_nx_quit_point( void ) __attribute__((weak));
    struct timespec deadline;

    if (!&wine_nx_quit_requested || !&wine_nx_quit_point)
    {
        pthread_cond_wait( cond, mutex );
        return;
    }
    clock_gettime( CLOCK_REALTIME, &deadline );
    deadline.tv_nsec += 250000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait( cond, mutex, &deadline );
    if (wine_nx_quit_requested)
    {
        pthread_mutex_unlock( mutex );
        wine_nx_quit_point();
        pthread_mutex_lock( mutex );
    }
}

static ssize_t horizon_pipe_write_r( struct _reent *r, void *fdptr, const char *ptr, size_t len )
{
    struct horizon_pipe_file *file = *(struct horizon_pipe_file **)fdptr;
    struct horizon_pipe *pipe;
    unsigned int wake;
    size_t total = 0;

    if (!file || !file->write_end)
    {
        horizon_set_reent_errno( r, EBADF );
        return -1;
    }
    if (!len) return 0;

    pipe = file->pipe;
    pthread_mutex_lock( &pipe->mutex );
    while (total < len)
    {
        size_t chunk, space;

        while (pipe->read_open && pipe->used == HORIZON_PIPE_BUFFER_SIZE)
        {
            /* The reader has to make room first. */
            if (pipe->read_waiters) pthread_cond_signal( &pipe->can_read );
            pipe->write_waiters++;
            horizon_cond_wait_quit( &pipe->can_write, &pipe->mutex );
            pipe->write_waiters--;
        }

        if (!pipe->read_open)
        {
            pthread_mutex_unlock( &pipe->mutex );
            if (total) return total;
            horizon_set_reent_errno( r, EPIPE );
            return -1;
        }

        space = HORIZON_PIPE_BUFFER_SIZE - pipe->used;
        chunk = min( len - total, space );
        chunk = min( chunk, HORIZON_PIPE_BUFFER_SIZE - pipe->tail );
        memcpy( pipe->buffer + pipe->tail, ptr + total, chunk );
        pipe->tail = (pipe->tail + chunk) % HORIZON_PIPE_BUFFER_SIZE;
        pipe->used += chunk;
        total += chunk;
    }
    wake = pipe->read_waiters;
    pthread_mutex_unlock( &pipe->mutex );
    /* A signal is a system call on Horizon, so only for a waiting reader, and
     * after unlocking, or the reader wakes straight into this thread's lock and
     * the unlock becomes a second system call (2% of NFSU2's main thread). */
    if (wake) pthread_cond_signal( &pipe->can_read );
    return total;
}

static ssize_t horizon_pipe_read_r( struct _reent *r, void *fdptr, char *ptr, size_t len )
{
    struct horizon_pipe_file *file = *(struct horizon_pipe_file **)fdptr;
    struct horizon_pipe *pipe;
    unsigned int wake;
    size_t total = 0;

    if (!file || file->write_end)
    {
        horizon_set_reent_errno( r, EBADF );
        return -1;
    }
    if (!len) return 0;

    pipe = file->pipe;
    pthread_mutex_lock( &pipe->mutex );
    while (total < len)
    {
        size_t chunk;

        while (pipe->write_open && !pipe->used)
        {
            /* A writer waiting for room can go on with what this took. */
            if (total && pipe->write_waiters) pthread_cond_signal( &pipe->can_write );
            pipe->read_waiters++;
            horizon_cond_wait_quit( &pipe->can_read, &pipe->mutex );
            pipe->read_waiters--;
        }

        if (!pipe->used)
        {
            pthread_mutex_unlock( &pipe->mutex );
            return total;
        }

        chunk = min( len - total, pipe->used );
        chunk = min( chunk, HORIZON_PIPE_BUFFER_SIZE - pipe->head );
        memcpy( ptr + total, pipe->buffer + pipe->head, chunk );
        pipe->head = (pipe->head + chunk) % HORIZON_PIPE_BUFFER_SIZE;
        pipe->used -= chunk;
        total += chunk;
    }
    wake = pipe->write_waiters;
    pthread_mutex_unlock( &pipe->mutex );
    if (wake) pthread_cond_signal( &pipe->can_write );
    return total;
}

static int horizon_pipe_fstat_r( struct _reent *r, void *fdptr, struct stat *st )
{
    struct horizon_pipe_file *file = *(struct horizon_pipe_file **)fdptr;

    if (!file)
    {
        horizon_set_reent_errno( r, EBADF );
        return -1;
    }
    memset( st, 0, sizeof(*st) );
    st->st_mode = S_IFIFO | 0600;
    return 0;
}

static int horizon_pipe_install_device(void)
{
    pthread_mutex_lock( &horizon_pipe_device_mutex );
    if (horizon_pipe_device == -1)
    {
        horizon_pipe_device = FindDevice( "winepipe:" );
        if (horizon_pipe_device == -1)
            horizon_pipe_device = AddDevice( &horizon_pipe_devoptab );
    }
    pthread_mutex_unlock( &horizon_pipe_device_mutex );

    if (horizon_pipe_device == -1)
    {
        errno = EMFILE;
        return -1;
    }
    return horizon_pipe_device;
}

int horizon_pipe( int fd[2] )
{
    struct horizon_pipe_file *read_file = NULL;
    struct horizon_pipe_file *write_file = NULL;
    struct horizon_pipe *pipe = NULL;
    int dev;

    fd[0] = -1;
    fd[1] = -1;

    if ((dev = horizon_pipe_install_device()) == -1) return -1;
    if (!(pipe = calloc( 1, sizeof(*pipe) )) ||
        !(read_file = calloc( 1, sizeof(*read_file) )) ||
        !(write_file = calloc( 1, sizeof(*write_file) )))
    {
        free( write_file );
        free( read_file );
        free( pipe );
        errno = ENOMEM;
        return -1;
    }

    pthread_mutex_init( &pipe->mutex, NULL );
    pthread_cond_init( &pipe->can_read, NULL );
    pthread_cond_init( &pipe->can_write, NULL );
    __atomic_add_fetch( &horizon_lifecycle.pipes, 1, __ATOMIC_RELAXED );
    pipe->refs = 2;
    pipe->read_open = 1;
    pipe->write_open = 1;

    read_file->pipe = pipe;
    read_file->write_end = 0;
    write_file->pipe = pipe;
    write_file->write_end = 1;

    if ((fd[0] = __alloc_handle( dev )) == -1)
        goto fail;
    *(struct horizon_pipe_file **)__get_handle( fd[0] )->fileStruct = read_file;
    read_file = NULL;

    if ((fd[1] = __alloc_handle( dev )) == -1)
        goto fail;
    *(struct horizon_pipe_file **)__get_handle( fd[1] )->fileStruct = write_file;
    write_file = NULL;

    return 0;

fail:
    if (fd[0] != -1) close( fd[0] );
    else if (pipe) horizon_pipe_release( pipe );
    if (fd[1] != -1) close( fd[1] );
    else if (pipe) horizon_pipe_release( pipe );
    free( write_file );
    free( read_file );
    errno = EMFILE;
    return -1;
}

static void horizon_fd_queue_push( struct horizon_fd_queue *queue, int fd, unsigned int handle )
{
    struct horizon_fd_message *message = malloc( sizeof(*message) );

    if (!message)
    {
        close( fd );
        fprintf( stderr, "wine: out of memory queueing Horizon server fd.\n" );
        exit(1);
    }

    message->fd = fd;
    message->handle = handle;
    message->next = NULL;

    pthread_mutex_lock( &queue->mutex );
    if (queue->tail) queue->tail->next = message;
    else queue->head = message;
    queue->tail = message;
    pthread_cond_signal( &queue->cond );
    pthread_mutex_unlock( &queue->mutex );
}

static void horizon_fd_queue_push_dup( struct horizon_fd_queue *queue, int fd, unsigned int handle )
{
    int passed_fd = dup( fd );

    if (passed_fd == -1)
    {
        fprintf( stderr, "wine: failed to duplicate Horizon server fd: %s\n", strerror(errno) );
        exit(1);
    }

    horizon_fd_queue_push( queue, passed_fd, handle );
}

static int horizon_fd_queue_pop( struct horizon_fd_queue *queue, unsigned int *handle )
{
    struct horizon_fd_message *message;
    int fd;

    pthread_mutex_lock( &queue->mutex );
    while (!queue->head)
        horizon_cond_wait_quit( &queue->cond, &queue->mutex );

    message = queue->head;
    queue->head = message->next;
    if (!queue->head) queue->tail = NULL;
    pthread_mutex_unlock( &queue->mutex );

    fd = message->fd;
    if (handle) *handle = message->handle;
    free( message );
    return fd;
}

void horizon_server_queue_fd( int fd, unsigned int handle )
{
    horizon_fd_queue_push_dup( &horizon_server_to_client_fds, fd, handle );
}

int horizon_server_take_client_fd( unsigned int *handle )
{
    return horizon_fd_queue_pop( &horizon_client_to_server_fds, handle );
}

static int horizon_read_exact( int fd, void *buffer, size_t size )
{
    extern volatile int wine_nx_quit_requested __attribute__((weak));
    extern void wine_nx_quit_point( void ) __attribute__((weak));
    char *ptr = buffer;

    while (size)
    {
        ssize_t ret;

        if (&wine_nx_quit_requested && wine_nx_quit_requested && &wine_nx_quit_point) wine_nx_quit_point();
        ret = read( fd, ptr, size );

        if (ret > 0)
        {
            ptr += ret;
            size -= ret;
            continue;
        }
        if (!ret) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

static int horizon_write_exact( int fd, const void *buffer, size_t size )
{
    const char *ptr = buffer;

    while (size)
    {
        ssize_t ret = write( fd, ptr, size );

        if (ret > 0)
        {
            ptr += ret;
            size -= ret;
            continue;
        }
        if (!ret) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

static int horizon_server_write_reply( int fd, const void *reply, size_t reply_size,
                                       const void *data, size_t data_size )
{
    unsigned char message[HORIZON_SERVER_FIXED_MESSAGE_SIZE] = {0};

    if (reply_size > sizeof(message))
    {
        errno = EOVERFLOW;
        return -1;
    }

    memcpy( message, reply, reply_size );
    if (horizon_write_exact( fd, message, sizeof(message) ) <= 0) return -1;
    if (data_size && horizon_write_exact( fd, data, data_size ) <= 0) return -1;
    return 0;
}

static int horizon_server_write_status( int fd, unsigned int status )
{
    struct horizon_server_reply_header reply = { status, 0 };

    return horizon_server_write_reply( fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_server_alloc_handle(void)
{
    return __sync_add_and_fetch( &horizon_server_next_handle, 4 );
}

static struct horizon_server_handle_entry **horizon_server_handle_bucket( unsigned int handle )
{
    return &horizon_server_handle_hash[(handle >> 2) & (HORIZON_SERVER_HANDLE_HASH_SIZE - 1)];
}

static struct horizon_server_handle_entry *horizon_server_find_handle_locked( unsigned int handle )
{
    struct horizon_server_handle_entry *entry;

    for (entry = *horizon_server_handle_bucket( handle ); entry; entry = entry->hash_next)
        if (entry->handle == handle) return entry;

    return NULL;
}

/* Adds an entry, its handle already set, to the list and the hash. */
static void horizon_server_link_handle_locked( struct horizon_server_handle_entry *entry )
{
    struct horizon_server_handle_entry **bucket = horizon_server_handle_bucket( entry->handle );

    if ((entry->next = horizon_server_handles)) entry->next->pprev = &entry->next;
    entry->pprev = &horizon_server_handles;
    horizon_server_handles = entry;
    entry->hash_next = *bucket;
    *bucket = entry;
}

static void horizon_server_unlink_handle_locked( struct horizon_server_handle_entry *entry )
{
    struct horizon_server_handle_entry **ptr = horizon_server_handle_bucket( entry->handle );

    if ((*entry->pprev = entry->next)) entry->next->pprev = entry->pprev;
    while (*ptr != entry) ptr = &(*ptr)->hash_next;
    *ptr = entry->hash_next;
}

static struct horizon_server_handle_entry *horizon_server_create_handle_locked( int type )
{
    struct horizon_server_object *object = calloc( 1, sizeof(*object) );
    struct horizon_server_handle_entry *entry = calloc( 1, sizeof(*entry) );

    if (!object || !entry)
    {
        free( object );
        free( entry );
        return NULL;
    }

    entry->handle = horizon_server_alloc_handle();
    entry->object = object;
    horizon_server_link_handle_locked( entry );

    object->id = entry->handle;
    object->type = type;
    object->refs = 1;
    object->file_fd = -1;
    return entry;
}

static struct horizon_server_handle_entry *horizon_server_create_handle_for_object_locked(
    struct horizon_server_object *object )
{
    struct horizon_server_handle_entry *entry = calloc( 1, sizeof(*entry) );

    if (!entry) return NULL;
    entry->handle = horizon_server_alloc_handle();
    entry->object = object;
    object->refs++;
    if (object->type == HORIZON_SERVER_OBJECT_COMPLETION) object->completion_closed = 0;
    horizon_server_link_handle_locked( entry );
    return entry;
}

/* Callers hold horizon_server_objects_mutex. */
static void horizon_server_free_object( struct horizon_server_object *object )
{
    if (!object) return;
    if (object->type == HORIZON_SERVER_OBJECT_THREAD)
    {
        struct horizon_server_object **ptr;

        for (ptr = &horizon_server_threads; *ptr; ptr = &(*ptr)->thread_next)
        {
            if (*ptr != object) continue;
            *ptr = object->thread_next;
            __atomic_sub_fetch( &horizon_lifecycle.thread_objects, 1, __ATOMIC_RELAXED );
            break;
        }
    }
    while (object->apc_first)
    {
        struct horizon_user_apc *apc = object->apc_first;

        object->apc_first = apc->next;
        free( apc );
    }
    object->apc_last = NULL;
    horizon_completion_clear( &object->completion );
    if (object->wait_port && !--object->wait_port->refs) horizon_server_free_object( object->wait_port );
    if (object->file_completion && !--object->file_completion->refs)
        horizon_server_free_object( object->file_completion );
    if (object->mapping_shared && !--object->mapping_shared->refs)
        horizon_server_free_object( object->mapping_shared );
    if (object->reg_key) horizon_reg_release( &horizon_registry, object->reg_key );
    if (object->file_fd != -1) close( object->file_fd );
    free( object->file_name );
    free( object->dir_mask );
    free( object->name );
    free( object );
}

static unsigned int horizon_server_errno_status( int error );

static int horizon_server_object_has_handles_locked( const struct horizon_server_object *object )
{
    const struct horizon_server_handle_entry *entry;

    for (entry = horizon_server_handles; entry; entry = entry->next)
        if (entry->object == object) return 1;
    return 0;
}

static unsigned int horizon_server_close_object_handle( unsigned int handle )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    if (!handle) return HORIZON_STATUS_INVALID_HANDLE;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((entry = horizon_server_find_handle_locked( handle )))
    {
        object = entry->object;
        horizon_server_unlink_handle_locked( entry );
        /* What waits on a socket that is closed ends, as wineserver ends it. */
        if (object && object->type == HORIZON_SERVER_OBJECT_SOCK && horizon_asyncs.head &&
            horizon_async_cancel( &horizon_asyncs, handle, 0, 0, horizon_async_now(), 1 ))
            horizon_server_signal_changed_locked();
        /* server/completion.c's close_handle: closing a port's last handle
         * abandons the waits on it. */
        if (object && object->type == HORIZON_SERVER_OBJECT_COMPLETION &&
            !horizon_server_object_has_handles_locked( object ))
        {
            object->completion_closed = 1;
            horizon_server_signal_changed_locked();
        }
        if (object && object->reg_key)
            horizon_reg_handle_closed( &horizon_registry, object->reg_key, handle );
        if (object && object->refs && !--object->refs)
        {
            /* DeleteFileW opens with DELETE_ON_CLOSE, then closes. Defer until
             * the last duplicate handle releases this file object. */
            if (object->type == HORIZON_SERVER_OBJECT_FILE &&
                ((object->file_options & 0x00001000u) || object->file_delete) && object->file_name)
            {
                int ret, error;
                /* Horizon's filesystem cannot delete an open file. Release
                 * this object's descriptor before removing its path. */
                if (object->file_fd != -1)
                {
                    ret = close( object->file_fd );
                    object->file_fd = -1;
                    if (ret == -1) status = horizon_server_errno_status( errno );
                }
                ret = object->file_is_dir ? rmdir( object->file_name ) : unlink( object->file_name );
                error = ret == -1 ? errno : 0;
                if (error && error != ENOENT) status = horizon_server_errno_status( error );
                horizon_trace( "[HZFILE] delete result=%d errno=%d", ret, error );
                horizon_trace( "[HZFILE] delete-on-close path=%s status=%08x",
                               object->file_name, status );
            }
            horizon_server_free_object( object );
        }
        free( entry );
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        return status;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return HORIZON_STATUS_INVALID_HANDLE;
}

static unsigned int horizon_server_duplicate_object_handle( unsigned int handle, unsigned int *new_handle )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_handle_entry *duplicate;

    *new_handle = 0;
    if (!handle) return HORIZON_STATUS_INVALID_HANDLE;

    duplicate = calloc( 1, sizeof(*duplicate) );
    if (!duplicate) return HORIZON_STATUS_NO_MEMORY;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (handle == HORIZON_CURRENT_PROCESS_HANDLE)
    {
        /* Process objects carry no state yet; a fresh one is equivalent. */
        if ((entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_PROCESS )))
            *new_handle = entry->handle;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        free( duplicate );
        return entry ? HORIZON_STATUS_SUCCESS : HORIZON_STATUS_NO_MEMORY;
    }
    if (handle == HORIZON_CURRENT_THREAD_HANDLE && horizon_server_current &&
        horizon_server_current->thread)
    {
        duplicate->object = horizon_server_current->thread;
    }
    else if (!(entry = horizon_server_find_handle_locked( handle )))
    {
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        free( duplicate );
        return HORIZON_STATUS_INVALID_HANDLE;
    }
    else duplicate->object = entry->object;

    duplicate->handle = horizon_server_alloc_handle();
    duplicate->object->refs++;
    horizon_server_link_handle_locked( duplicate );
    *new_handle = duplicate->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_parse_object_attributes( const unsigned char *data,
                                                            unsigned int data_size,
                                                            struct horizon_object_name *name )
{
    const struct horizon_object_attributes *attributes;
    unsigned int name_offset;

    memset( name, 0, sizeof(*name) );
    if (!data_size) return HORIZON_STATUS_SUCCESS;
    if (data_size < sizeof(*attributes)) return HORIZON_STATUS_INVALID_PARAMETER;

    attributes = (const void *)data;
    if (attributes->sd_len > data_size - sizeof(*attributes))
        return HORIZON_STATUS_INVALID_PARAMETER;
    name_offset = sizeof(*attributes) + attributes->sd_len;
    if (attributes->name_len > data_size - name_offset)
        return HORIZON_STATUS_INVALID_PARAMETER;

    name->rootdir = attributes->rootdir;
    name->name = data + name_offset;
    name->name_len = attributes->name_len;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_parse_open_name( const unsigned char *message,
                                                    const unsigned char *data,
                                                    unsigned int data_size,
                                                    struct horizon_object_name *name )
{
    const struct horizon_open_named_object_request *request = (const void *)message;

    memset( name, 0, sizeof(*name) );
    name->rootdir = request->rootdir;
    name->name = data;
    name->name_len = data_size;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_object_attributes_size( const unsigned char *data,
                                                           unsigned int data_size,
                                                           unsigned int *attr_size )
{
    const struct horizon_object_attributes *attributes;
    unsigned int size;

    *attr_size = 0;
    if (!data_size) return HORIZON_STATUS_SUCCESS;
    if (data_size < sizeof(*attributes)) return HORIZON_STATUS_INVALID_PARAMETER;

    attributes = (const void *)data;
    if (attributes->sd_len > data_size - sizeof(*attributes))
        return HORIZON_STATUS_INVALID_PARAMETER;
    size = sizeof(*attributes) + attributes->sd_len;
    if (attributes->name_len > data_size - size)
        return HORIZON_STATUS_INVALID_PARAMETER;
    size = (size + attributes->name_len + 3) & ~3u;
    if (size > data_size) return HORIZON_STATUS_INVALID_PARAMETER;

    *attr_size = size;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_errno_status( int error )
{
    switch (error)
    {
    case 0:
        return HORIZON_STATUS_SUCCESS;
    case EACCES:
    case EPERM:
        return HORIZON_STATUS_ACCESS_DENIED;
    case EEXIST:
        return HORIZON_STATUS_OBJECT_NAME_COLLISION;
    case ENOENT:
        return HORIZON_STATUS_NO_SUCH_FILE;
    case ENOTDIR:
        return HORIZON_STATUS_OBJECT_PATH_NOT_FOUND;
    case EMFILE:
    case ENFILE:
        return HORIZON_STATUS_TOO_MANY_OPENED_FILES;
    case ENOMEM:
        return HORIZON_STATUS_NO_MEMORY;
    default:
        return HORIZON_STATUS_INVALID_PARAMETER;
    }
}

static unsigned int horizon_server_read_exact_at( int fd, unsigned long long offset,
                                                  void *buffer, size_t size )
{
    unsigned char *ptr = buffer;

    if (lseek( fd, (off_t)offset, SEEK_SET ) == (off_t)-1)
        return horizon_server_errno_status( errno );

    while (size)
    {
        ssize_t ret = read( fd, ptr, size );

        if (ret > 0)
        {
            ptr += ret;
            size -= ret;
            continue;
        }
        if (!ret) return HORIZON_STATUS_INVALID_IMAGE_FORMAT;
        if (errno == EINTR) continue;
        return horizon_server_errno_status( errno );
    }

    return HORIZON_STATUS_SUCCESS;
}

static unsigned short horizon_get_le16( const unsigned char *ptr )
{
    return ptr[0] | ((unsigned short)ptr[1] << 8);
}

static unsigned int horizon_get_le32( const unsigned char *ptr )
{
    return ptr[0] | ((unsigned int)ptr[1] << 8) |
           ((unsigned int)ptr[2] << 16) | ((unsigned int)ptr[3] << 24);
}

static unsigned long long horizon_get_le64( const unsigned char *ptr )
{
    return (unsigned long long)horizon_get_le32( ptr ) |
           ((unsigned long long)horizon_get_le32( ptr + 4 ) << 32);
}

static int horizon_pe_data_dir( const unsigned char *opt, unsigned int opt_size, BOOL pe32,
                                unsigned int index, unsigned int *va, unsigned int *size )
{
    unsigned int count_offset = pe32 ? 92 : 108;
    unsigned int dirs_offset = pe32 ? 96 : 112;

    *va = *size = 0;
    if (opt_size < count_offset + 4 || index >= horizon_get_le32( opt + count_offset ) ||
        opt_size < dirs_offset + (index + 1) * 8)
        return 0;
    *va = horizon_get_le32( opt + dirs_offset + index * 8 );
    *size = horizon_get_le32( opt + dirs_offset + index * 8 + 4 );
    return *va && *size;
}

static size_t horizon_server_read_pe_dir( int fd, void *buffer, size_t buffer_size,
                                          unsigned int va, unsigned int size,
                                          unsigned int align_mask, const unsigned char *sections,
                                          unsigned int section_count, unsigned long long file_size )
{
    unsigned int i;

    if (!va || !size) return 0;
    for (i = 0; i < section_count; i++)
    {
        const unsigned char *section = sections + i * 40;
        unsigned int section_va = horizon_get_le32( section + 12 );
        unsigned int virtual_size = horizon_get_le32( section + 8 );
        unsigned int raw_size = horizon_get_le32( section + 16 );
        unsigned int raw_offset = horizon_get_le32( section + 20 );
        unsigned long long map_size, read_offset, read_size;

        if (va < section_va || (virtual_size && va - section_va >= virtual_size)) continue;
        map_size = virtual_size ? virtual_size : raw_size;
        map_size = (map_size + align_mask) & ~(unsigned long long)align_mask;
        if (!map_size || size >= map_size || va - section_va >= map_size - size) continue;

        read_offset = (raw_offset & ~0x1ffu) + (unsigned long long)(va - section_va);
        read_size = (unsigned long long)raw_size + (raw_offset & 0x1ffu);
        read_size = (read_size + 0x1ff) & ~(unsigned long long)0x1ff;
        if (read_size > map_size) read_size = map_size;
        if (size < read_size) read_size = size;
        if (buffer_size < read_size) read_size = buffer_size;
        if (read_offset >= file_size) return 0;
        if (read_size > file_size - read_offset) read_size = file_size - read_offset;
        if (!read_size || horizon_server_read_exact_at( fd, read_offset, buffer, read_size )) return 0;
        return read_size;
    }
    return 0;
}

static unsigned int horizon_server_read_pe_image_info( int fd, struct horizon_pe_image_info *info )
{
    static const char builtin_signature[] = "Wine builtin DLL";
    static const char fakedll_signature[] = "Wine placeholder DLL";
    unsigned char dos[64], mz_signature[32], nt[24], cfg[0xd0];
    unsigned char *headers = NULL;
    unsigned int status = HORIZON_STATUS_SUCCESS;
    unsigned int pe_offset, opt_size, section_count, headers_size;
    unsigned int size_of_image, section_alignment, size_of_headers, align_mask;
    unsigned int reloc_va = 0, reloc_size = 0, cfg_va = 0, cfg_size = 0, clr_va = 0, clr_size = 0;
    unsigned long long header_end;
    unsigned int i;
    unsigned short machine, characteristics, dll_charact;
    BOOL pe32, has_relocs;
    struct stat st;

    memset( info, 0, sizeof(*info) );

    if (fstat( fd, &st ) == -1) return horizon_server_errno_status( errno );
    if (st.st_size < (off_t)(sizeof(dos) + sizeof(nt))) return HORIZON_STATUS_INVALID_IMAGE_FORMAT;

    if ((status = horizon_server_read_exact_at( fd, 0, dos, sizeof(dos) )))
        return status;
    if (horizon_get_le16( dos ) != 0x5a4d) return HORIZON_STATUS_INVALID_IMAGE_FORMAT;
    if (st.st_size >= sizeof(dos) + sizeof(mz_signature) &&
        !horizon_server_read_exact_at( fd, sizeof(dos), mz_signature, sizeof(mz_signature) ))
    {
        info->wine_builtin = !memcmp( mz_signature, builtin_signature, sizeof(builtin_signature) );
        info->wine_fakedll = !memcmp( mz_signature, fakedll_signature, sizeof(fakedll_signature) );
    }

    pe_offset = horizon_get_le32( dos + 0x3c );
    if (pe_offset > (unsigned long long)st.st_size - sizeof(nt))
        return HORIZON_STATUS_INVALID_IMAGE_FORMAT;

    if ((status = horizon_server_read_exact_at( fd, pe_offset, nt, sizeof(nt) )))
        return status;
    if (memcmp( nt, "PE\0\0", 4 )) return HORIZON_STATUS_INVALID_IMAGE_FORMAT;

    machine = horizon_get_le16( nt + 4 );
    section_count = horizon_get_le16( nt + 6 );
    opt_size = horizon_get_le16( nt + 20 );
    characteristics = horizon_get_le16( nt + 22 );

    if (machine != HORIZON_IMAGE_FILE_MACHINE_ARM64 && machine != horizon_process_machine)
        return HORIZON_STATUS_INVALID_IMAGE_FORMAT;
    if (!section_count || section_count > 128 || opt_size < (machine == HORIZON_IMAGE_FILE_MACHINE_I386 ? 96 : 112))
        return HORIZON_STATUS_INVALID_IMAGE_FORMAT;
    if (opt_size > 4096 || section_count > (0x10000 - opt_size) / 40)
        return HORIZON_STATUS_INVALID_IMAGE_FORMAT;

    headers_size = opt_size + section_count * 40;
    if (pe_offset + sizeof(nt) > (unsigned long long)st.st_size ||
        headers_size > (unsigned long long)st.st_size - pe_offset - sizeof(nt))
        return HORIZON_STATUS_INVALID_IMAGE_FORMAT;

    if (!(headers = malloc( headers_size ))) return HORIZON_STATUS_NO_MEMORY;
    status = horizon_server_read_exact_at( fd, pe_offset + sizeof(nt), headers, headers_size );
    if (status) goto done;

    pe32 = machine == HORIZON_IMAGE_FILE_MACHINE_I386;
    if (horizon_get_le16( headers ) != (pe32 ? 0x10b : HORIZON_IMAGE_NT_OPTIONAL_HDR64_MAGIC))
    {
        status = HORIZON_STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }

    horizon_pe_data_dir( headers, opt_size, pe32, HORIZON_IMAGE_DIRECTORY_ENTRY_BASERELOC,
                         &reloc_va, &reloc_size );
    horizon_pe_data_dir( headers, opt_size, pe32, HORIZON_IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG,
                         &cfg_va, &cfg_size );
    horizon_pe_data_dir( headers, opt_size, pe32, HORIZON_IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR,
                         &clr_va, &clr_size );

    section_alignment = horizon_get_le32( headers + 32 );
    size_of_image = horizon_get_le32( headers + 56 );
    size_of_headers = horizon_get_le32( headers + 60 );
    dll_charact = horizon_get_le16( headers + 70 );

    if (!section_alignment || !size_of_image || size_of_headers > size_of_image)
    {
        status = HORIZON_STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }

    info->base = pe32 ? horizon_get_le32( headers + 28 ) : horizon_get_le64( headers + 24 );
    info->stack_size = pe32 ? horizon_get_le32( headers + 72 ) : horizon_get_le64( headers + 72 );
    info->stack_commit = pe32 ? horizon_get_le32( headers + 76 ) : horizon_get_le64( headers + 80 );
    info->entry_point = horizon_get_le32( headers + 16 );
    /* As wineserver's get_image_params (server/mapping.c) and Windows: the image
     * is mapped in whole units of its section alignment, at least a page. A
     * SizeOfImage that ends inside the last section's page, as NFS Most Wanted
     * Black Edition's speed.exe has, would otherwise leave that section looking
     * too large for the image. */
    align_mask = section_alignment - 1 > 0xfff ? section_alignment - 1 : 0xfff;
    info->map_size = (size_of_image + align_mask) & ~align_mask;
    if (info->map_size < size_of_image)
    {
        status = HORIZON_STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }
    header_end = (unsigned long long)pe_offset + sizeof(nt) + headers_size;
    if (header_end > info->map_size || header_end > 0xffffffffu)
    {
        status = HORIZON_STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }
    info->alignment = section_alignment;
    info->zerobits = 0;
    info->subsystem = horizon_get_le16( headers + 68 );
    info->subsystem_minor = horizon_get_le16( headers + 50 );
    info->subsystem_major = horizon_get_le16( headers + 48 );
    info->osversion_major = horizon_get_le16( headers + 40 );
    info->osversion_minor = horizon_get_le16( headers + 42 );
    info->image_charact = characteristics;
    info->dll_charact = dll_charact;
    info->machine = machine;
    info->contains_code = horizon_get_le32( headers + 4 ) || info->entry_point || (section_alignment & 0xfff);
    info->loader_flags = clr_va && clr_size;
    info->header_size = max( size_of_headers, (unsigned int)header_end );
    /* The headers are mapped up to the first section, as wineserver does. */
    info->header_map_size = info->map_size;
    info->file_size = st.st_size > 0xffffffffll ? 0xffffffffu : (unsigned int)st.st_size;
    info->checksum = horizon_get_le32( headers + 64 );

    if (section_alignment & 0xfff)
        info->image_flags |= HORIZON_IMAGE_FLAGS_IMAGE_MAPPED_FLAT;

    has_relocs = reloc_va && reloc_size && !(characteristics & HORIZON_IMAGE_FILE_RELOCS_STRIPPED);

    if (!(section_alignment & 0xfff) &&
        (dll_charact & HORIZON_IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) &&
        (has_relocs || info->contains_code) && !(clr_va && clr_size))
        info->image_flags |= HORIZON_IMAGE_FLAGS_IMAGE_DYNAMICALLY_RELOCATED;

    for (i = 0; i < section_count; i++)
    {
        const unsigned char *section = headers + opt_size + i * 40;

        if (horizon_get_le32( section + 12 ) < info->header_map_size)
            info->header_map_size = horizon_get_le32( section + 12 );
        if (horizon_get_le32( section + 36 ) & HORIZON_IMAGE_SCN_MEM_EXECUTE) info->contains_code = 1;
    }

    memset( cfg, 0, sizeof(cfg) );
    i = horizon_server_read_pe_dir( fd, cfg, sizeof(cfg), cfg_va, cfg_size, align_mask,
                                    headers + opt_size, section_count, st.st_size );
    if (i >= 4)
    {
        unsigned int chpe_offset = pe32 ? 0x7c : 0xc8;
        unsigned int chpe_size = pe32 ? 4 : 8;
        unsigned int declared_size = horizon_get_le32( cfg );

        if (declared_size < i) i = declared_size;
        if (i >= chpe_offset + chpe_size)
            info->is_hybrid = pe32 ? !!horizon_get_le32( cfg + chpe_offset ) :
                                     !!horizon_get_le64( cfg + chpe_offset );
    }

done:
    free( headers );
    return status;
}

static unsigned int horizon_server_utf16_name_len( const char *name )
{
    return name ? strlen( name ) * sizeof(unsigned short) : 0;
}

static void horizon_server_write_utf16_name( unsigned char *dst, const char *name )
{
    unsigned short *wide = (unsigned short *)dst;

    for (; name && *name; name++, wide++)
        *wide = (unsigned char)*name;
}

static unsigned int horizon_server_file_open_flags( const struct horizon_create_file_request *request,
                                                    int *flags )
{
    *flags = horizon_file_access_mode( request->access );

    switch (request->create)
    {
    case FILE_CREATE:
        *flags |= O_CREAT | O_EXCL;
        break;
    case FILE_OPEN:
        break;
    case FILE_OPEN_IF:
        *flags |= O_CREAT;
        break;
    case FILE_OVERWRITE:
        *flags |= O_TRUNC;
        break;
    case FILE_OVERWRITE_IF:
    case FILE_SUPERSEDE:
        *flags |= O_CREAT | O_TRUNC;
        break;
    default:
        return HORIZON_STATUS_INVALID_PARAMETER;
    }

    return HORIZON_STATUS_SUCCESS;
}

static int horizon_server_name_matches( const struct horizon_server_object *object, int type,
                                        const struct horizon_object_name *name )
{
    if (!name->name_len || !object->name_len) return 0;
    if (object->rootdir != name->rootdir) return 0;
    if (object->name_len != name->name_len) return 0;
    if (memcmp( object->name, name->name, name->name_len )) return 0;
    return !type || object->type == type;
}

/* A name relative to a BaseNamedObjects directory handle is kept as a bare name
 * with root 0, as before the server had directories: kernelbase passes its
 * handle once NtOpenDirectoryObject works, and code holding no handle still
 * finds the same object. Callers hold horizon_server_objects_mutex. */
static unsigned int horizon_server_name_root_locked( unsigned int rootdir )
{
    struct horizon_server_handle_entry *entry;

    if (rootdir && (entry = horizon_server_find_handle_locked( rootdir )) &&
        entry->object->type == HORIZON_SERVER_OBJECT_DIRECTORY &&
        entry->object->directory == HORIZON_OBJECT_DIR_NAMED_OBJECTS)
        return 0;
    return rootdir;
}

static struct horizon_server_object *horizon_server_find_named_object_any_locked(
    const struct horizon_object_name *name )
{
    struct horizon_object_name canonical = *name;
    struct horizon_server_handle_entry *entry;

    canonical.rootdir = horizon_server_name_root_locked( name->rootdir );
    for (entry = horizon_server_handles; entry; entry = entry->next)
        if (horizon_server_name_matches( entry->object, 0, &canonical )) return entry->object;

    return NULL;
}

static struct horizon_server_object *horizon_server_find_named_object_locked(
    int type, const struct horizon_object_name *name )
{
    struct horizon_object_name canonical = *name;
    struct horizon_server_handle_entry *entry;

    canonical.rootdir = horizon_server_name_root_locked( name->rootdir );
    for (entry = horizon_server_handles; entry; entry = entry->next)
        if (horizon_server_name_matches( entry->object, type, &canonical )) return entry->object;

    return NULL;
}

static unsigned int horizon_server_set_object_name( struct horizon_server_object *object,
                                                    const struct horizon_object_name *name )
{
    if (!name->name_len) return HORIZON_STATUS_SUCCESS;

    object->name = malloc( name->name_len );
    if (!object->name) return HORIZON_STATUS_NO_MEMORY;
    memcpy( object->name, name->name, name->name_len );
    object->name_len = name->name_len;
    object->rootdir = horizon_server_name_root_locked( name->rootdir );
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_create_named_object_handle_locked(
    int type, const struct horizon_object_name *name, struct horizon_server_handle_entry **entry )
{
    struct horizon_server_object *object;

    *entry = NULL;
    if ((object = horizon_server_find_named_object_any_locked( name )))
    {
        if (object->type != type) return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
        if (!(*entry = horizon_server_create_handle_for_object_locked( object )))
            return HORIZON_STATUS_NO_MEMORY;
        return HORIZON_STATUS_OBJECT_NAME_EXISTS;
    }

    if (!(*entry = horizon_server_create_handle_locked( type )))
        return HORIZON_STATUS_NO_MEMORY;
    if (horizon_server_set_object_name( (*entry)->object, name ) != HORIZON_STATUS_SUCCESS)
    {
        struct horizon_server_handle_entry *failed = *entry;

        horizon_server_unlink_handle_locked( failed );
        horizon_server_free_object( failed->object );
        free( failed );
        *entry = NULL;
        return HORIZON_STATUS_NO_MEMORY;
    }
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_open_named_object_handle( int type,
                                                            const struct horizon_object_name *name,
                                                            unsigned int *handle )
{
    struct horizon_server_object *object;
    struct horizon_server_handle_entry *entry;

    *handle = 0;
    if (!name->name_len) return HORIZON_STATUS_OBJECT_NAME_NOT_FOUND;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    object = horizon_server_find_named_object_locked( type, name );
    if (!object)
    {
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        return HORIZON_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    entry = horizon_server_create_handle_for_object_locked( object );
    if (entry) *handle = entry->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return entry ? HORIZON_STATUS_SUCCESS : HORIZON_STATUS_NO_MEMORY;
}

static unsigned int horizon_server_create_object_handle( int type, unsigned int *handle )
{
    struct horizon_server_handle_entry *entry;

    *handle = 0;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_create_handle_locked( type );
    if (entry) *handle = entry->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return entry ? HORIZON_STATUS_SUCCESS : HORIZON_STATUS_NO_MEMORY;
}

static unsigned char horizon_ascii_tolower( unsigned char ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int horizon_utf16_name_equals( const unsigned char *left, unsigned int left_len,
                                      const unsigned char *right, unsigned int right_len )
{
    unsigned int i;

    if (left_len != right_len) return 0;
    if (left_len & 1) return 0;
    for (i = 0; i < left_len; i += 2)
    {
        if (left[i + 1] || right[i + 1])
        {
            if (left[i] != right[i] || left[i + 1] != right[i + 1]) return 0;
        }
        else if (horizon_ascii_tolower( left[i] ) != horizon_ascii_tolower( right[i] ))
            return 0;
    }
    return 1;
}

static int horizon_utf16_name_equals_ascii( const unsigned char *name, unsigned int name_len,
                                            const char *ascii )
{
    unsigned int i;

    for (i = 0; ascii[i]; i++) {}
    if (name_len != i * sizeof(unsigned short)) return 0;
    for (i = 0; ascii[i]; i++)
    {
        if (name[i * 2 + 1]) return 0;
        if (horizon_ascii_tolower( name[i * 2] ) != horizon_ascii_tolower( ascii[i] )) return 0;
    }
    return 1;
}

static int horizon_utf16_name_has_ascii_suffix( const unsigned char *name, unsigned int name_len,
                                                const char *suffix )
{
    unsigned int suffix_len, offset;

    for (suffix_len = 0; suffix[suffix_len]; suffix_len++) {}
    suffix_len *= sizeof(unsigned short);
    if (name_len < suffix_len) return 0;
    offset = name_len - suffix_len;
    return horizon_utf16_name_equals_ascii( name + offset, suffix_len, suffix );
}

static unsigned int horizon_utf16_from_ascii( unsigned char *buffer, unsigned int buffer_size,
                                              const char *ascii )
{
    unsigned int i;

    for (i = 0; ascii[i] && i * 2 + 1 < buffer_size; i++)
    {
        buffer[i * 2] = ascii[i];
        buffer[i * 2 + 1] = 0;
    }
    return i * sizeof(unsigned short);
}

static struct horizon_atom_entry *horizon_server_find_atom_name_locked( const unsigned char *name,
                                                                        unsigned int name_len )
{
    struct horizon_atom_entry *entry;

    for (entry = horizon_atoms; entry; entry = entry->next)
        if (horizon_utf16_name_equals( entry->name, entry->name_len, name, name_len )) return entry;
    return NULL;
}

static unsigned int horizon_server_add_atom_locked( const unsigned char *name, unsigned int name_len,
                                                    unsigned int requested_atom, unsigned int *atom )
{
    struct horizon_atom_entry *entry;

    *atom = 0;
    if (!name_len && requested_atom)
    {
        *atom = requested_atom;
        return HORIZON_STATUS_SUCCESS;
    }
    if (!name_len || name_len > HORIZON_MAX_ATOM_LEN * sizeof(unsigned short))
        return HORIZON_STATUS_INVALID_PARAMETER;
    if ((entry = horizon_server_find_atom_name_locked( name, name_len )))
    {
        *atom = entry->atom;
        return HORIZON_STATUS_SUCCESS;
    }
    if (!(entry = calloc( 1, sizeof(*entry) ))) return HORIZON_STATUS_NO_MEMORY;
    if (!(entry->name = malloc( name_len )))
    {
        free( entry );
        return HORIZON_STATUS_NO_MEMORY;
    }
    memcpy( entry->name, name, name_len );
    entry->name_len = name_len;
    entry->atom = requested_atom ? requested_atom : horizon_next_atom++;
    entry->next = horizon_atoms;
    horizon_atoms = entry;
    *atom = entry->atom;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_find_atom_locked( const unsigned char *name, unsigned int name_len,
                                                     unsigned int *atom )
{
    struct horizon_atom_entry *entry;

    *atom = 0;
    if ((entry = horizon_server_find_atom_name_locked( name, name_len ))) *atom = entry->atom;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_ensure_session_locked(void)
{
    char path[128];
    int fd;

    if (horizon_session_fd != -1) return HORIZON_STATUS_SUCCESS;
    if (!(horizon_session_data = calloc( 1, HORIZON_SESSION_MAPPING_SIZE )))
        return HORIZON_STATUS_NO_MEMORY;

    snprintf( path, sizeof(path), "sdmc:/switch/wine/wine-nx-session-%u.shm", (unsigned int)getpid() );
    fd = open( path, O_RDWR | O_CREAT | O_TRUNC, 0600 );
    if (fd == -1)
    {
        snprintf( path, sizeof(path), "wine-nx-session-%u.shm", (unsigned int)getpid() );
        fd = open( path, O_RDWR | O_CREAT | O_TRUNC, 0600 );
    }
    if (fd == -1)
    {
        free( horizon_session_data );
        horizon_session_data = NULL;
        return horizon_server_errno_status( errno );
    }
    if (ftruncate( fd, HORIZON_SESSION_MAPPING_SIZE ) == -1)
    {
        static const char zero;

        if (lseek( fd, HORIZON_SESSION_MAPPING_SIZE - 1, SEEK_SET ) == (off_t)-1 ||
            write( fd, &zero, 1 ) != 1)
        {
            unsigned int status = horizon_server_errno_status( errno );

            close( fd );
            free( horizon_session_data );
            horizon_session_data = NULL;
            return status;
        }
    }
    horizon_session_fd = fd;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_flush_session_range_locked( unsigned long long offset,
                                                               unsigned long long size )
{
    struct horizon_session_view *view;

    if (!size) return HORIZON_STATUS_SUCCESS;
    if (offset + size > HORIZON_SESSION_MAPPING_SIZE) return HORIZON_STATUS_INVALID_PARAMETER;
    /* Horizon cannot share the mapping, so each client view gets a copy. The
     * backing file is not updated: views are filled from memory when they
     * register (horizon_server_note_session_view_locked), and writing the SD
     * card on every cursor move or queue change was slow. */
    for (view = horizon_session_views; view; view = view->next)
    {
        unsigned long long start = offset > view->offset ? offset : view->offset;
        unsigned long long end = offset + size < view->offset + view->size ? offset + size : view->offset + view->size;

        if (start >= end) continue;
        memcpy( (void *)(ULONG_PTR)(view->base + start - view->offset),
                horizon_session_data + start, end - start );
    }
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_alloc_shared_object_locked( unsigned long long shm_size,
                                                               struct horizon_obj_locator *locator )
{
    struct horizon_shared_object *object;
    unsigned long long offset, size;
    unsigned int status;

    memset( locator, 0, sizeof(*locator) );
    if ((status = horizon_server_ensure_session_locked())) return status;

    offset = (horizon_session_used + 7) & ~7ull;
    size = offsetof( struct horizon_shared_object, shm ) + shm_size;
    size = (size + 7) & ~7ull;
    if (!shm_size || offset + size > HORIZON_SESSION_MAPPING_SIZE)
        return HORIZON_STATUS_NO_MEMORY;

    object = (struct horizon_shared_object *)(horizon_session_data + offset);
    memset( object, 0, size );
    object->id = ++horizon_session_next_id;
    horizon_session_used = offset + size;
    locator->id = object->id;
    locator->offset = offset;
    return horizon_server_flush_session_range_locked( offset, size );
}

static struct horizon_shared_object *horizon_server_shared_object_locked( struct horizon_obj_locator locator )
{
    if (!locator.id || locator.offset + sizeof(struct horizon_shared_object) > HORIZON_SESSION_MAPPING_SIZE)
        return NULL;
    return (struct horizon_shared_object *)(horizon_session_data + locator.offset);
}

static unsigned int horizon_server_ensure_input_locked(void)
{
    struct horizon_shared_object *shared;
    unsigned int status;

    if (horizon_input_locator.id) return HORIZON_STATUS_SUCCESS;
    if ((status = horizon_server_alloc_shared_object_locked( sizeof(struct horizon_input_shm),
                                                             &horizon_input_locator )))
        return status;
    shared = horizon_server_shared_object_locked( horizon_input_locator );
    shared->shm.input.foreground = 1;
    shared->shm.input.cursor_count = 0;
    return horizon_server_flush_session_range_locked(
        horizon_input_locator.offset,
        offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_input_shm) );
}

static struct horizon_input_shm *horizon_server_input_shared_locked(void)
{
    struct horizon_shared_object *shared;

    if (horizon_server_ensure_input_locked()) return NULL;
    shared = horizon_server_shared_object_locked( horizon_input_locator );
    return shared ? &shared->shm.input : NULL;
}

static unsigned int horizon_server_flush_input_locked(void)
{
    if (!horizon_input_locator.id) return HORIZON_STATUS_INVALID_HANDLE;
    return horizon_server_flush_session_range_locked(
        horizon_input_locator.offset,
        offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_input_shm) );
}

/* The caret window and rectangle are shared input data. Its hide count and
 * on/off state stay in the server, as in server/queue.c. */
static int horizon_caret_hide;
static int horizon_caret_state;

/* CreateCaret and DestroyCaret: a new caret window starts at 0,0, hidden. */
static void horizon_server_set_caret_window_locked( struct horizon_input_shm *input, unsigned int win,
                                                    int width, int height )
{
    if (!win || win != input->caret) memset( &input->caret_rect, 0, sizeof(input->caret_rect) );
    input->caret = win;
    input->caret_rect.right = input->caret_rect.left + width;
    input->caret_rect.bottom = input->caret_rect.top + height;
    horizon_caret_hide = 1;
    horizon_caret_state = 0;
}

/* SetCaretPos, ShowCaret, HideCaret and the blink timer. The caller reports
 * the values from before the change; win32u draws the caret from them. */
static unsigned int horizon_server_set_caret_info_locked( struct horizon_input_shm *input, unsigned int flags,
                                                          unsigned int handle, int x, int y, int hide,
                                                          int state )
{
    struct horizon_rectangle old = input->caret_rect;

    if (handle && handle != input->caret) return HORIZON_STATUS_ACCESS_DENIED;
    if (flags & HORIZON_SET_CARET_POS)
    {
        input->caret_rect.right += x - old.left;
        input->caret_rect.bottom += y - old.top;
        input->caret_rect.left = x;
        input->caret_rect.top = y;
    }
    if (flags & HORIZON_SET_CARET_HIDE)
    {
        horizon_caret_hide += hide;
        if (horizon_caret_hide < 0) horizon_caret_hide = 0;
    }
    if (flags & HORIZON_SET_CARET_STATE)
    {
        switch (state)
        {
        case HORIZON_CARET_STATE_OFF: horizon_caret_state = 0; break;
        case HORIZON_CARET_STATE_ON: horizon_caret_state = 1; break;
        case HORIZON_CARET_STATE_TOGGLE: horizon_caret_state = !horizon_caret_state; break;
        case HORIZON_CARET_STATE_ON_IF_MOVED:
            if (x != old.left || y != old.top) horizon_caret_state = 1;
            break;
        }
    }
    return HORIZON_STATUS_SUCCESS;
}

static unsigned long long horizon_server_timer_clock(void)
{
    return armTicksToNs( armGetSystemTick() ) / 1000000ull;
}

/* Shared queue data of ended threads, for reuse. */
static struct horizon_obj_locator horizon_free_queue_shm[32];
static unsigned int horizon_free_queue_shm_count;

/* The thread's message queue, created with its shared data and waitable
 * object on first use. */
static struct horizon_msgq *horizon_server_queue_locked( unsigned int tid )
{
    struct horizon_obj_locator locator;
    struct horizon_shared_object *shared;
    struct horizon_server_object *sync;
    struct horizon_msgq *queue;
    int created;

    if (!(queue = horizon_msgq_get( &horizon_msg_queues, tid, &created )) || !created) return queue;
    if (horizon_free_queue_shm_count &&
        (shared = horizon_server_shared_object_locked( horizon_free_queue_shm[horizon_free_queue_shm_count - 1] )))
    {
        locator = horizon_free_queue_shm[--horizon_free_queue_shm_count];
        memset( &shared->shm.queue, 0, sizeof(shared->shm.queue) );
        shared->id = locator.id = ++horizon_session_next_id;
        horizon_server_flush_session_range_locked(
            locator.offset, offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_queue_shm) );
    }
    else if (horizon_server_alloc_shared_object_locked( sizeof(struct horizon_queue_shm), &locator ))
    {
        horizon_msgq_destroy( &horizon_msg_queues, queue );
        return NULL;
    }
    if (!(sync = calloc( 1, sizeof(*sync) )))
    {
        if (horizon_free_queue_shm_count < ARRAY_SIZE(horizon_free_queue_shm))
            horizon_free_queue_shm[horizon_free_queue_shm_count++] = locator;
        horizon_msgq_destroy( &horizon_msg_queues, queue );
        return NULL;
    }
    sync->type = HORIZON_SERVER_OBJECT_MSG_QUEUE;
    sync->refs = 1;
    sync->file_fd = -1;
    sync->queue_tid = tid;
    queue->sync = sync;
    queue->shm_id = locator.id;
    queue->shm_offset = locator.offset;
    return queue;
}

/* Recompute the wake bits from what is pending for the thread and publish
 * them, with the wait masks, in the shared queue data. */
static void horizon_server_refresh_queue_locked( struct horizon_msgq *queue, unsigned long long now )
{
    struct horizon_obj_locator locator = { queue->shm_id, queue->shm_offset };
    struct horizon_input_message *input;
    struct horizon_user_window *window;
    struct horizon_shared_object *shared;
    struct horizon_queue_shm *shm;
    unsigned int external = 0;

    if (horizon_message_queue_find( &horizon_posted_messages, queue->tid, 0, 0, 0, NULL, NULL ))
        external |= HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_ALLPOSTMESSAGE;
    for (input = horizon_input_messages; input; input = input->next)
        if (input->tid == queue->tid)
            external |= input->msg == HORIZON_WM_MOUSEMOVE ? HORIZON_MSGQ_QS_MOUSEMOVE : HORIZON_MSGQ_QS_MOUSEBUTTON;
    for (window = horizon_windows; window; window = window->next)
    {
        if (window->tid != queue->tid || !(window->style & HORIZON_WS_VISIBLE)) continue;
        if (!window->has_update_rect && !window->has_internal_paint) continue;
        external |= HORIZON_MSGQ_QS_PAINT;
        break;
    }
    if (horizon_win_timers_expired( &horizon_timers, queue->tid, 0, 0, 0, now, 0 ))
        external |= HORIZON_MSGQ_QS_TIMER;
    horizon_msgq_update( queue, external );

    if (!(shared = horizon_server_shared_object_locked( locator )) || shared->id != queue->shm_id) return;
    shm = &shared->shm.queue;
    if (shm->wake_bits == queue->wake_bits && shm->changed_bits == queue->changed_bits &&
        shm->wake_mask == queue->wake_mask && shm->changed_mask == queue->changed_mask)
        return;
    shm->wake_bits = queue->wake_bits;
    shm->changed_bits = queue->changed_bits;
    shm->wake_mask = queue->wake_mask;
    shm->changed_mask = queue->changed_mask;
    horizon_server_flush_session_range_locked(
        queue->shm_offset, offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_queue_shm) );
}

/* Time out sent messages, then refresh every queue: a reply or timeout wakes
 * another thread's queue, and timers and paints need no event to fall due. */
static void horizon_server_refresh_queues_locked(void)
{
    unsigned long long now = horizon_server_timer_clock();
    struct horizon_msgq *queue;

    horizon_msgq_expire( &horizon_msg_queues, now );
    for (queue = horizon_msg_queues.head; queue; queue = queue->next)
        horizon_server_refresh_queue_locked( queue, now );
}

static void horizon_server_destroy_queue_locked( unsigned int tid )
{
    struct horizon_msgq *queue = horizon_msgq_find( &horizon_msg_queues, tid );
    struct horizon_server_object *sync;

    if (!queue) return;
    sync = queue->sync;
    if (horizon_free_queue_shm_count < ARRAY_SIZE(horizon_free_queue_shm))
    {
        struct horizon_obj_locator locator = { queue->shm_id, queue->shm_offset };
        horizon_free_queue_shm[horizon_free_queue_shm_count++] = locator;
    }
    horizon_msgq_destroy( &horizon_msg_queues, queue );
    if (sync)
    {
        sync->queue_tid = 0;
        if (!--sync->refs) horizon_server_free_object( sync );
    }
    horizon_server_refresh_queues_locked();
}

static unsigned int horizon_server_note_session_view_locked( unsigned long long base,
                                                             unsigned long long offset,
                                                             unsigned long long size )
{
    struct horizon_session_view *view;

    if (!base || !size) return HORIZON_STATUS_SUCCESS;
    if (offset + size > HORIZON_SESSION_MAPPING_SIZE) return HORIZON_STATUS_INVALID_PARAMETER;
    for (view = horizon_session_views; view; view = view->next)
    {
        if (view->base != base) continue;
        view->offset = offset;
        view->size = size;
        horizon_mprotect( (void *)(ULONG_PTR)base, size, PROT_READ | PROT_WRITE );
        memcpy( (void *)(ULONG_PTR)base, horizon_session_data + offset, size );
        return HORIZON_STATUS_SUCCESS;
    }
    if (!(view = calloc( 1, sizeof(*view) ))) return HORIZON_STATUS_NO_MEMORY;
    view->base = base;
    view->offset = offset;
    view->size = size;
    view->next = horizon_session_views;
    horizon_session_views = view;
    horizon_mprotect( (void *)(ULONG_PTR)base, size, PROT_READ | PROT_WRITE );
    /* The client filled the view from the backing file, which is not kept current. */
    memcpy( (void *)(ULONG_PTR)base, horizon_session_data + offset, size );
    return HORIZON_STATUS_SUCCESS;
}

static void horizon_server_remove_session_view_locked( unsigned long long base )
{
    struct horizon_session_view **ptr;

    for (ptr = &horizon_session_views; *ptr; ptr = &(*ptr)->next)
    {
        struct horizon_session_view *view = *ptr;

        if (view->base != base) continue;
        *ptr = view->next;
        free( view );
        return;
    }
}

/* virtual_set_force_exec re-protects every view from Wine's bookkeeping, where
 * a client's session view is read-only, but this server copies session updates
 * into those views: loading a DLL without NX_COMPAT into an NX process (regsvr32
 * registering blizzard.ax) left them read-only and the next copy faulted in the
 * server thread. It holds the lock across its loop, so no copy lands until the
 * views are writable again. Called with virtual_mutex held; the server never
 * takes virtual_mutex. */
void horizon_lock_session_views( void )
{
    pthread_mutex_lock( &horizon_server_objects_mutex );
}

void horizon_unlock_session_views( void )
{
    struct horizon_session_view *view;

    for (view = horizon_session_views; view; view = view->next)
        horizon_mprotect( (void *)(ULONG_PTR)view->base, view->size, PROT_READ | PROT_WRITE );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
}

static unsigned int horizon_server_alloc_user_handle_locked( unsigned short type,
                                                             struct horizon_obj_locator locator,
                                                             unsigned int pid, unsigned int tid,
                                                             unsigned int *handle )
{
    struct horizon_user_entry *entry;
    unsigned int index;
    unsigned short generation;
    unsigned long long entry_offset;

    *handle = 0;
    if (!horizon_session_data)
    {
        unsigned int status = horizon_server_ensure_session_locked();
        if (status) return status;
    }

    for (index = 0; index < horizon_user_handle_count; index++)
    {
        entry = &((struct horizon_session_shm *)horizon_session_data)->user_entries[index];
        if (!entry->type) break;
    }
    if (index == HORIZON_MAX_USER_HANDLES) return HORIZON_STATUS_NO_MEMORY;
    if (index == horizon_user_handle_count) horizon_user_handle_count++;
    entry = &((struct horizon_session_shm *)horizon_session_data)->user_entries[index];
    generation = entry->generation + 1;
    if (!generation || generation == 0xffff) generation = 1;
    entry->offset = locator.offset;
    /* Zero identifies server-owned handles with no local client object.
     * Replacing it with process 1 makes win32u look up a nonexistent WND. */
    entry->tid = tid;
    entry->pid = pid;
    entry->id = locator.id;
    entry->uniq = ((unsigned int)generation << 16) | type;
    *handle = (index << 1) + HORIZON_FIRST_USER_HANDLE + ((unsigned int)generation << 16);
    entry_offset = offsetof( struct horizon_session_shm, user_entries ) + index * sizeof(*entry);
    return horizon_server_flush_session_range_locked( entry_offset, sizeof(*entry) );
}

static int horizon_server_handle_alloc_user_handle( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_alloc_user_handle_request *request = (const void *)message;
    struct horizon_alloc_user_handle_reply reply;
    struct horizon_obj_locator locator = {0};

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_alloc_user_handle_locked( request->type, locator,
                                                                  connection->pid, connection->tid,
                                                                  &reply.handle );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZUSER] alloc_user_handle type=%u -> %08x err=%08x\n",
                   request->type, reply.handle, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_free_user_handle( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_free_user_handle_request *request = (const void *)message;
    struct horizon_server_reply_header reply;
    struct horizon_user_entry *entry = NULL;
    unsigned int low = request->handle & 0xffff;
    unsigned int index = HORIZON_MAX_USER_HANDLES;
    unsigned long long entry_offset;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (low >= HORIZON_FIRST_USER_HANDLE && !((low - HORIZON_FIRST_USER_HANDLE) & 1))
        index = (low - HORIZON_FIRST_USER_HANDLE) >> 1;
    if (index < horizon_user_handle_count && horizon_session_data)
        entry = &((struct horizon_session_shm *)horizon_session_data)->user_entries[index];
    if (!entry || entry->type != request->type ||
        entry->generation != (request->handle >> 16))
        reply.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        entry->offset = 0;
        entry->tid = 0;
        entry->pid = 0;
        entry->id = 0;
        entry->uniq = (unsigned int)entry->generation << 16;
        entry_offset = offsetof( struct horizon_session_shm, user_entries ) + index * sizeof(*entry);
        reply.error = horizon_server_flush_session_range_locked( entry_offset, sizeof(*entry) );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZUSER] free_user_handle type=%u hwnd=%08x err=%08x\n",
                   request->type, request->handle, reply.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static struct horizon_user_window *horizon_server_find_window_locked( unsigned int handle )
{
    struct horizon_user_window *window;

    for (window = horizon_windows; window; window = window->next)
        if (window->handle == handle) return window;
    return NULL;
}

static struct horizon_server_object *horizon_server_find_handle_object_locked( unsigned int handle,
                                                                               int type )
{
    struct horizon_server_handle_entry *entry;

    if (!handle) return NULL;
    if (!(entry = horizon_server_find_handle_locked( handle ))) return NULL;
    if (type && entry->object->type != type) return NULL;
    return entry->object;
}

static unsigned int horizon_server_compare_object_handles( unsigned int first, unsigned int second )
{
    struct horizon_server_handle_entry *first_entry;
    struct horizon_server_handle_entry *second_entry;
    unsigned int status;

    if (!first || !second) return HORIZON_STATUS_INVALID_HANDLE;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    first_entry = horizon_server_find_handle_locked( first );
    second_entry = horizon_server_find_handle_locked( second );
    if (!first_entry || !second_entry) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (first_entry->object == second_entry->object) status = HORIZON_STATUS_SUCCESS;
    else status = HORIZON_STATUS_NOT_SAME_OBJECT;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return status;
}

static unsigned int horizon_server_find_typed_object_locked( unsigned int handle, int type,
                                                             struct horizon_server_object **object )
{
    struct horizon_server_handle_entry *entry;

    if (!handle) return HORIZON_STATUS_INVALID_HANDLE;
    if (!(entry = horizon_server_find_handle_locked( handle ))) return HORIZON_STATUS_INVALID_HANDLE;
    if (entry->object->type != type) return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    *object = entry->object;
    return HORIZON_STATUS_SUCCESS;
}

/* Identity of the client served by the calling server thread (mutex owner). */
static unsigned int horizon_server_current_tid(void)
{
    return horizon_server_current ? horizon_server_current->tid : 0;
}

/* Signals the timers whose time has come and moves a periodic one on. Need
 * for Speed Most Wanted paces its streaming thread with SetWaitableTimer and
 * WaitForSingleObject; signalling a timer as it was set returned every wait at
 * once, 70000 times a second on one core, and the game lost its pacing. */
static void horizon_server_update_timers_locked(void)
{
    struct horizon_server_handle_entry *entry;
    LARGE_INTEGER now;
    int armed = 0;

    if (!horizon_server_timers_armed) return;
    NtQuerySystemTime( &now );
    for (entry = horizon_server_handles; entry; entry = entry->next)
    {
        struct horizon_server_object *object = entry->object;

        if (object->type != HORIZON_SERVER_OBJECT_TIMER || !object->timer_when) continue;
        if (object->timer_when > now.QuadPart)
        {
            armed = 1;
            continue;
        }
        object->signaled = 1;
        if (!object->timer_period)
        {
            object->timer_when = 0;
            continue;
        }
        /* A period is in milliseconds, and one that was missed does not repeat. */
        do object->timer_when += (long long)object->timer_period * 10000;
        while (object->timer_when <= now.QuadPart);
        armed = 1;
    }
    horizon_server_timers_armed = armed;
}

static int horizon_server_object_is_signaled( const struct horizon_server_object *object )
{
    switch (object->type)
    {
    case HORIZON_SERVER_OBJECT_EVENT:
        return object->signaled;
    case HORIZON_SERVER_OBJECT_MUTEX:
        return horizon_mutex_signaled( &object->mutex, horizon_server_current_tid() );
    case HORIZON_SERVER_OBJECT_SEMAPHORE:
        return object->count > 0;
    case HORIZON_SERVER_OBJECT_TIMER:
        return object->signaled;
    case HORIZON_SERVER_OBJECT_THREAD:
        return object->thread.terminated;
    case HORIZON_SERVER_OBJECT_MSG_QUEUE:
    {
        struct horizon_msgq *queue = horizon_msgq_find( &horizon_msg_queues, object->queue_tid );
        return queue && horizon_msgq_signaled( queue );
    }
    case HORIZON_SERVER_OBJECT_COMPLETION:
        return object->completion.depth > 0;
    case HORIZON_SERVER_OBJECT_COMPLETION_WAIT:
        return horizon_completion_wait_signaled( object->wait_port ? &object->wait_port->completion : NULL,
                                                 object->wait_port && object->wait_port->completion_closed );
    case HORIZON_SERVER_OBJECT_PROCESS:
    case HORIZON_SERVER_OBJECT_RESERVE:
    case HORIZON_SERVER_OBJECT_KEYED_EVENT:
        return 1;
    default:
        return 0;
    }
}

/* Returns 1 when the wait acquired an abandoned mutex. */
static int horizon_server_consume_signal( struct horizon_server_object *object )
{
    switch (object->type)
    {
    case HORIZON_SERVER_OBJECT_EVENT:
        if (!object->manual_reset) object->signaled = 0;
        break;
    case HORIZON_SERVER_OBJECT_MUTEX:
        return horizon_mutex_acquire( &object->mutex, horizon_server_current_tid() );
    case HORIZON_SERVER_OBJECT_SEMAPHORE:
        if (object->count) object->count--;
        break;
    case HORIZON_SERVER_OBJECT_TIMER:
        if (!object->manual_reset) object->signaled = 0;
        break;
    case HORIZON_SERVER_OBJECT_COMPLETION_WAIT:
        return horizon_completion_wait_satisfy( object->wait_port ? &object->wait_port->completion : NULL,
                                                object->wait_port && object->wait_port->completion_closed,
                                                &object->wait_msg, &object->wait_has_msg );
    default:
        break;
    }
    return 0;
}

/* Resolve a thread handle, including the current-thread pseudo-handle. */
static struct horizon_server_object *horizon_server_get_thread_locked( unsigned int handle,
                                                                       unsigned int *status )
{
    struct horizon_server_object *object = NULL;

    if (handle == HORIZON_CURRENT_THREAD_HANDLE)
    {
        object = horizon_server_current ? horizon_server_current->thread : NULL;
        *status = object ? HORIZON_STATUS_SUCCESS : HORIZON_STATUS_INVALID_HANDLE;
        return object;
    }
    *status = horizon_server_find_typed_object_locked( handle, HORIZON_SERVER_OBJECT_THREAD, &object );
    return *status == HORIZON_STATUS_SUCCESS ? object : NULL;
}

static long long horizon_server_now(void)
{
    LARGE_INTEGER now;

    NtQuerySystemTime( &now );
    return now.QuadPart;
}

/* A new thread object with no handles; the caller adds the references. */
static struct horizon_server_object *horizon_server_alloc_thread_locked( unsigned int tid, unsigned int pid )
{
    struct horizon_server_object *object = calloc( 1, sizeof(*object) );

    if (!object) return NULL;
    object->type = HORIZON_SERVER_OBJECT_THREAD;
    object->id = tid;
    object->file_fd = -1;
    horizon_thread_init( &object->thread, tid, pid, horizon_get_system_affinity_mask(),
                         horizon_server_now() );
    object->thread_next = horizon_server_threads;
    horizon_server_threads = object;
    horizon_server_running_threads++;
    __atomic_add_fetch( &horizon_lifecycle.thread_objects, 1, __ATOMIC_RELAXED );
    return object;
}

/* Drops a client's completion wait object and the handle it was given. */
static void horizon_server_end_completion_wait_locked( struct horizon_server_connection *connection )
{
    struct horizon_server_object *wait = connection->completion_wait;
    struct horizon_server_handle_entry *entry;

    if (!wait) return;
    if ((entry = horizon_server_find_handle_locked( connection->completion_wait_handle )) && entry->object == wait)
    {
        horizon_server_unlink_handle_locked( entry );
        wait->refs--;
        free( entry );
    }
    connection->completion_wait = NULL;
    connection->completion_wait_handle = 0;
    if (!--wait->refs) horizon_server_free_object( wait );
}

/* The client's request pipe closed: it can no longer run Windows code. Mark it
 * terminated (waiters wake), abandon its mutexes and drop the connection's
 * reference. Callers hold horizon_server_objects_mutex. */
static void horizon_server_end_thread_locked( struct horizon_server_connection *connection )
{
    struct horizon_server_object *thread = connection->thread;
    struct horizon_server_handle_entry *entry;

    horizon_server_end_completion_wait_locked( connection );
    if (!thread) return;
    connection->thread = NULL;
    if (horizon_thread_mark_terminated( &thread->thread, horizon_server_now() ))
        horizon_server_running_threads--;
    horizon_message_queue_drop( &horizon_posted_messages, thread->thread.tid, 0 );
    horizon_win_timers_drop( &horizon_timers, thread->thread.tid, 0 );
    if (horizon_clip_thread_ended( &horizon_clipboard, thread->thread.tid )) horizon_server_clipboard_notify_locked();
    horizon_server_destroy_queue_locked( thread->thread.tid );
    for (entry = horizon_server_handles; entry; entry = entry->next)
        if (entry->object->type == HORIZON_SERVER_OBJECT_MUTEX)
            horizon_mutex_abandon( &entry->object->mutex, thread->thread.tid );
    horizon_server_signal_changed_locked();
    if (!--thread->refs) horizon_server_free_object( thread );
}

static unsigned int horizon_server_signal_object_locked( unsigned int handle )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object;

    if (!handle) return HORIZON_STATUS_INVALID_HANDLE;
    if (!(entry = horizon_server_find_handle_locked( handle ))) return HORIZON_STATUS_INVALID_HANDLE;

    object = entry->object;
    switch (object->type)
    {
    case HORIZON_SERVER_OBJECT_EVENT:
        if (!object->signaled)
        {
            object->signaled = 1;
            horizon_server_signal_changed_locked();
        }
        return HORIZON_STATUS_SUCCESS;
    case HORIZON_SERVER_OBJECT_MUTEX:
    {
        unsigned int previous, status = horizon_mutex_release( &object->mutex, horizon_server_current_tid(), &previous );

        if (!status && !object->mutex.count) horizon_server_signal_changed_locked();
        return status;
    }
    case HORIZON_SERVER_OBJECT_SEMAPHORE:
        if (object->count == object->max) return HORIZON_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        object->count++;
        horizon_server_signal_changed_locked();
        return HORIZON_STATUS_SUCCESS;
    default:
        return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    }
}

/* Returns SUCCESS, ABANDONED_WAIT_0 (inherited an abandoned mutex), TIMEOUT
 * (not signaled) or an error. */
static unsigned int horizon_server_wait_object_locked( unsigned int handle, int consume )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object;
    unsigned int status;

    if (!handle) return HORIZON_STATUS_INVALID_HANDLE;
    if (handle == HORIZON_CURRENT_THREAD_HANDLE)
    {
        /* The calling thread cannot terminate while it waits on itself. */
        return horizon_server_get_thread_locked( handle, &status ) ? HORIZON_STATUS_TIMEOUT : status;
    }
    if (!(entry = horizon_server_find_handle_locked( handle ))) return HORIZON_STATUS_INVALID_HANDLE;
    object = entry->object;
    if (object->type == HORIZON_SERVER_OBJECT_MSG_QUEUE) horizon_server_refresh_queues_locked();
    if (!horizon_server_object_is_signaled( object )) return HORIZON_STATUS_TIMEOUT;
    if (consume && horizon_server_consume_signal( object )) return HORIZON_STATUS_ABANDONED_WAIT_0;
    return HORIZON_STATUS_SUCCESS;
}

static int horizon_server_handle_init_first_thread( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_init_first_thread_request *request = (const void *)message;
    struct horizon_init_first_thread_reply reply;
    unsigned short machines[] = { HORIZON_IMAGE_FILE_MACHINE_ARM64, horizon_process_machine };
    unsigned int machine_size = horizon_process_machine == HORIZON_IMAGE_FILE_MACHINE_ARM64 ?
                                sizeof(machines[0]) : sizeof(machines);
    unsigned int handle;
    int reply_fd, wait_fd;

    reply_fd = horizon_server_take_client_fd( &handle );
    wait_fd = horizon_server_take_client_fd( &handle );
    connection->reply_fd = reply_fd;
    connection->wait_fd = wait_fd;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = HORIZON_STATUS_SUCCESS;
    reply.header.reply_size = machine_size;
    reply.pid = request->unix_pid > 0 ? request->unix_pid : 1;
    /* Windows thread ids come from one counter: 4 here, then 8, 12, ... in
     * new_thread. A kernel-derived id could equal a later worker's id or share
     * its NtWaitForAlertByThreadId slot ((tid >> 2) - 1) and lose wakeups. */
    reply.tid = 4;
    reply.session_id = 1;
    connection->pid = reply.pid;
    connection->tid = reply.tid;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!connection->thread &&
        (connection->thread = horizon_server_alloc_thread_locked( reply.tid, reply.pid )))
    {
        connection->thread->refs = 1;
        connection->thread->thread.started = 1;
        __atomic_add_fetch( &horizon_lifecycle.connections, 1, __ATOMIC_RELAXED );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    TRACE( "Horizon server init_first_thread pid %u tid %u reply fd %d/%d wait fd %d/%d.\n",
           reply.pid, reply.tid, reply_fd, request->reply_fd, wait_fd, request->wait_fd );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                       machines, machine_size );
}

static int horizon_server_handle_init_process_done( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_init_process_done_request *request = (const void *)message;
    struct horizon_init_process_done_reply reply;

    if (connection->reply_fd == -1)
    {
        errno = EPIPE;
        return -1;
    }

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (connection->thread) connection->thread->thread.teb = request->teb;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = HORIZON_STATUS_SUCCESS;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_init_thread( struct horizon_server_connection *connection,
                                              const unsigned char *message )
{
    const struct horizon_init_thread_request *request = (const void *)message;
    struct horizon_init_thread_reply reply;
    unsigned int handle;
    int reply_fd, wait_fd;

    reply_fd = horizon_server_take_client_fd( &handle );
    wait_fd = horizon_server_take_client_fd( &handle );

    if (connection->reply_fd != -1) close( connection->reply_fd );
    if (connection->wait_fd != -1) close( connection->wait_fd );
    connection->reply_fd = reply_fd;
    connection->wait_fd = wait_fd;
    if (!connection->pid) connection->pid = getpid();
    if (!connection->tid) connection->tid = request->unix_tid > 0 ? request->unix_tid : 1;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (connection->thread)
    {
        connection->thread->thread.teb = request->teb;
        connection->thread->thread.entry = request->entry;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    /* CREATE_SUSPENDED: hold the reply until resume_thread opens the start
     * gate, so no Windows code runs first. Wine's kernelbase creates every
     * thread suspended and resumes it after NtCreateThreadEx returns. */
    pthread_mutex_lock( &horizon_server_objects_mutex );
    while (connection->thread && !horizon_thread_may_start( &connection->thread->thread ))
    {
        horizon_server_sleep_locked( HORIZON_SERVER_WAIT_SLICE );
        horizon_server_quit_check_locked();
    }
    if (connection->thread) connection->thread->thread.started = 1;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = HORIZON_STATUS_SUCCESS;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_close_handle( struct horizon_server_connection *connection,
                                               const unsigned char *message )
{
    const struct horizon_close_handle_request *request = (const void *)message;
    unsigned int status = horizon_server_close_object_handle( request->handle );

    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_set_handle_info( struct horizon_server_connection *connection )
{
    struct horizon_set_handle_info_reply reply;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = HORIZON_STATUS_SUCCESS;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_dup_handle( struct horizon_server_connection *connection,
                                             const unsigned char *message )
{
    const struct horizon_dup_handle_request *request = (const void *)message;
    struct horizon_dup_handle_reply reply;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_duplicate_object_handle( request->src_handle, &reply.handle );

    TRACE( "Horizon server dup_handle src %08x -> %08x status %08x.\n",
           request->src_handle, reply.handle, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_allocate_reserve_object( struct horizon_server_connection *connection,
                                                          const unsigned char *message )
{
    const struct horizon_allocate_reserve_object_request *request = (const void *)message;
    struct horizon_allocate_reserve_object_reply reply;

    (void)request;
    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_create_object_handle( HORIZON_SERVER_OBJECT_RESERVE, &reply.handle );

    TRACE( "Horizon server allocate_reserve_object type %d -> %08x.\n",
           request->type, reply.handle );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_compare_objects( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_compare_objects_request *request = (const void *)message;
    unsigned int status = horizon_server_compare_object_handles( request->first, request->second );

    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_open_object( struct horizon_server_connection *connection, int type )
{
    struct horizon_open_process_reply reply;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_create_object_handle( type, &reply.handle );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_open_named_object( struct horizon_server_connection *connection,
                                                    const unsigned char *message,
                                                    const unsigned char *data, unsigned int data_size,
                                                    int type )
{
    struct horizon_open_process_reply reply;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_open_name( message, data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
        reply.header.error = horizon_server_open_named_object_handle( type, &name, &reply.handle );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* NtOpenDirectoryObject: \?? and BaseNamedObjects (horizon_object_dirs.h). */
static int horizon_server_handle_open_directory( struct horizon_server_connection *connection,
                                                 const unsigned char *message,
                                                 const unsigned char *data, unsigned int data_size )
{
    const struct horizon_open_named_object_request *request = (const void *)message;
    struct horizon_open_process_reply reply;
    struct horizon_server_handle_entry *entry;
    int dir = request->rootdir ? HORIZON_OBJECT_DIR_NONE : horizon_object_dir_from_path( data, data_size );
    char name[64];
    unsigned int i;

    memset( &reply, 0, sizeof(reply) );
    if (!dir) reply.header.error = HORIZON_STATUS_OBJECT_NAME_NOT_FOUND;
    else
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        if ((entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_DIRECTORY )))
        {
            entry->object->directory = dir;
            reply.handle = entry->handle;
        }
        else reply.header.error = HORIZON_STATUS_NO_MEMORY;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }

    for (i = 0; i < data_size / 2 && i < sizeof(name) - 1; i++)
        name[i] = data[2 * i + 1] || data[2 * i] < 0x20 || data[2 * i] > 0x7e ? '?' : data[2 * i];
    name[i] = 0;
    horizon_trace( "[HZOBJ] open_directory %s rootdir=0x%x -> handle=0x%x err=0x%x",
                   name, request->rootdir, reply.handle, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* NtQueryDirectoryObject */
static int horizon_server_handle_get_directory_entries( struct horizon_server_connection *connection,
                                                        const unsigned char *message )
{
    const struct horizon_get_directory_entries_request *request = (const void *)message;
    struct horizon_get_directory_entries_reply reply;
    struct horizon_server_handle_entry *entry;
    unsigned char data[512];
    unsigned int size = 0, max = request->header.reply_size;
    int dir = HORIZON_OBJECT_DIR_NONE;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(entry = horizon_server_find_handle_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_DIRECTORY)
        reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else
        dir = entry->object->directory;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (!reply.header.error)
        reply.header.error = horizon_object_dir_entries( dir, request->index, request->max_count,
                                                         max < sizeof(data) ? max : sizeof(data), data,
                                                         &size, &reply.count, &reply.total_len );
    reply.header.reply_size = size;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, size );
}

static int horizon_server_handle_open_mapping( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    struct horizon_open_process_reply reply;
    struct horizon_object_name name;
    struct horizon_server_object *object;
    struct horizon_server_handle_entry *entry = NULL;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_open_name( message, data, data_size, &name );
    if (reply.header.error) return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(object = horizon_server_find_named_object_locked( HORIZON_SERVER_OBJECT_MAPPING, &name )) &&
        horizon_utf16_name_has_ascii_suffix( name.name, name.name_len, "__wine_session" ))
    {
        reply.header.error = horizon_server_ensure_session_locked();
        if (!reply.header.error)
        {
            if ((entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_MAPPING )))
            {
                entry->object->file_fd = dup( horizon_session_fd );
                if (entry->object->file_fd == -1)
                    reply.header.error = horizon_server_errno_status( errno );
                else
                {
                    entry->object->mapping_size = HORIZON_SESSION_MAPPING_SIZE;
                    entry->object->mapping_access = FILE_READ_DATA | FILE_WRITE_DATA;
                    entry->object->mapping_file_access = FILE_READ_DATA | FILE_WRITE_DATA;
                    entry->object->file_access = FILE_READ_DATA | FILE_WRITE_DATA;
                    entry->object->mapping_is_session = 1;
                    reply.header.error = horizon_server_set_object_name( entry->object, &name );
                    if (!reply.header.error) reply.handle = entry->handle;
                }

                if (reply.header.error)
                {
                    horizon_server_unlink_handle_locked( entry );
                    horizon_server_free_object( entry->object );
                    free( entry );
                    entry = NULL;
                }
            }
            else reply.header.error = HORIZON_STATUS_NO_MEMORY;
        }
    }
    else if (object)
    {
        if ((entry = horizon_server_create_handle_for_object_locked( object ))) reply.handle = entry->handle;
        else reply.header.error = HORIZON_STATUS_NO_MEMORY;
    }
    else reply.header.error = HORIZON_STATUS_OBJECT_NAME_NOT_FOUND;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_atom( struct horizon_server_connection *connection,
                                       const unsigned char *data, unsigned int data_size,
                                       int add )
{
    struct horizon_atom_reply reply;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (add) reply.header.error = horizon_server_add_atom_locked( data, data_size, 0, &reply.atom );
    else reply.header.error = horizon_server_find_atom_locked( data, data_size, &reply.atom );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_server_create_winstation_locked( const struct horizon_object_name *name,
                                                             unsigned int flags, unsigned int *handle )
{
    struct horizon_server_handle_entry *entry;
    unsigned int status;

    *handle = 0;
    status = horizon_server_create_named_object_handle_locked( HORIZON_SERVER_OBJECT_WINSTATION, name, &entry );
    if (entry)
    {
        if (status == HORIZON_STATUS_SUCCESS) entry->object->user_flags = flags;
        *handle = entry->handle;
    }
    return status;
}

static int horizon_server_handle_create_winstation( struct horizon_server_connection *connection,
                                                    const unsigned char *message,
                                                    const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_winstation_request *request = (const void *)message;
    struct horizon_winstation_handle_reply reply;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    memset( &name, 0, sizeof(name) );
    name.rootdir = request->rootdir;
    name.name = data;
    name.name_len = data_size;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_create_winstation_locked( &name, request->flags, &reply.handle );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_open_winstation( struct horizon_server_connection *connection,
                                                  const unsigned char *message,
                                                  const unsigned char *data, unsigned int data_size )
{
    return horizon_server_handle_open_named_object( connection, message, data, data_size,
                                                   HORIZON_SERVER_OBJECT_WINSTATION );
}

static int horizon_server_handle_get_process_winstation( struct horizon_server_connection *connection )
{
    struct horizon_winstation_handle_reply reply;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.handle = horizon_process_winstation;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_process_winstation( struct horizon_server_connection *connection,
                                                         const unsigned char *message )
{
    const struct horizon_set_process_winstation_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_handle_object_locked( request->handle, HORIZON_SERVER_OBJECT_WINSTATION ))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else horizon_process_winstation = request->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_set_winstation_monitors( struct horizon_server_connection *connection,
                                                          const unsigned char *message )
{
    const struct horizon_set_winstation_monitors_request *request = (const void *)message;
    struct horizon_set_winstation_monitors_reply reply;
    struct horizon_server_object *winstation;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(winstation = horizon_server_find_handle_object_locked( horizon_process_winstation,
                                                                 HORIZON_SERVER_OBJECT_WINSTATION )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        if (request->increment) winstation->count++;
        reply.serial = winstation->count;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_enum_winstation( struct horizon_server_connection *connection )
{
    struct horizon_enum_winstation_reply reply;

    memset( &reply, 0, sizeof(reply) );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_server_create_desktop_locked( const struct horizon_object_name *name,
                                                          unsigned int flags, unsigned int *handle )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_shared_object *shared;
    struct horizon_obj_locator locator;
    unsigned int status;

    *handle = 0;
    if (!horizon_process_winstation) return HORIZON_STATUS_INVALID_HANDLE;
    if (!name->name_len) return HORIZON_STATUS_INVALID_HANDLE;

    status = horizon_server_create_named_object_handle_locked( HORIZON_SERVER_OBJECT_DESKTOP, name, &entry );
    if (!entry) return status;
    *handle = entry->handle;
    if (status != HORIZON_STATUS_SUCCESS) return status;

    if ((status = horizon_server_alloc_shared_object_locked( sizeof(struct horizon_desktop_shm), &locator )))
        return status;
    shared = horizon_server_shared_object_locked( locator );
    shared->shm.desktop.flags = flags;
    shared->shm.desktop.cursor.clip.right = 1280;
    shared->shm.desktop.cursor.clip.bottom = 720;
    if ((status = horizon_server_flush_session_range_locked( locator.offset, sizeof(*shared) )))
        return status;

    entry->object->desktop_locator = locator;
    entry->object->desktop_winstation = horizon_process_winstation;
    entry->object->user_flags = flags;
    if (!horizon_input_desktop) horizon_input_desktop = entry->handle;
    return HORIZON_STATUS_SUCCESS;
}

static int horizon_server_handle_create_desktop( struct horizon_server_connection *connection,
                                                 const unsigned char *message,
                                                 const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_desktop_request *request = (const void *)message;
    struct horizon_winstation_handle_reply reply;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    memset( &name, 0, sizeof(name) );
    name.rootdir = horizon_process_winstation;
    name.name = data;
    name.name_len = data_size;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_create_desktop_locked( &name, request->flags, &reply.handle );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_open_desktop( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_open_desktop_request *request = (const void *)message;
    struct horizon_winstation_handle_reply reply;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    memset( &name, 0, sizeof(name) );
    name.rootdir = request->winsta ? request->winsta : horizon_process_winstation;
    name.name = data;
    name.name_len = data_size;
    reply.header.error = horizon_server_open_named_object_handle( HORIZON_SERVER_OBJECT_DESKTOP,
                                                                  &name, &reply.handle );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_open_input_desktop( struct horizon_server_connection *connection )
{
    struct horizon_winstation_handle_reply reply;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_input_desktop) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        struct horizon_server_object *desktop;
        struct horizon_server_handle_entry *entry;

        desktop = horizon_server_find_handle_object_locked( horizon_input_desktop, HORIZON_SERVER_OBJECT_DESKTOP );
        if (!desktop) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
        else if ((entry = horizon_server_create_handle_for_object_locked( desktop ))) reply.handle = entry->handle;
        else reply.header.error = HORIZON_STATUS_NO_MEMORY;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_input_desktop( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_set_thread_desktop_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_handle_object_locked( request->handle, HORIZON_SERVER_OBJECT_DESKTOP ))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else horizon_input_desktop = request->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_get_thread_desktop( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_get_thread_desktop_request *request = (const void *)message;
    struct horizon_get_thread_desktop_reply reply;
    struct horizon_server_object *desktop;

    (void)request;
    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.handle = horizon_thread_desktop;
    if (reply.handle &&
        (desktop = horizon_server_find_handle_object_locked( reply.handle, HORIZON_SERVER_OBJECT_DESKTOP )))
        reply.locator = desktop->desktop_locator;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_thread_desktop( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_set_thread_desktop_request *request = (const void *)message;
    struct horizon_set_thread_desktop_reply reply;
    struct horizon_server_object *desktop;

    memset( &reply, 0, sizeof(reply) );
    (void)request;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(desktop = horizon_server_find_handle_object_locked( request->handle, HORIZON_SERVER_OBJECT_DESKTOP )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        horizon_thread_desktop = request->handle;
        reply.locator = desktop->desktop_locator;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_user_object_info( struct horizon_server_connection *connection,
                                                       const unsigned char *message )
{
    const struct horizon_set_user_object_info_request *request = (const void *)message;
    struct horizon_set_user_object_info_reply reply;
    struct horizon_server_handle_entry *entry;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(entry = horizon_server_find_handle_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_WINSTATION &&
             entry->object->type != HORIZON_SERVER_OBJECT_DESKTOP)
        reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else
    {
        reply.is_desktop = entry->object->type == HORIZON_SERVER_OBJECT_DESKTOP;
        reply.old_obj_flags = entry->object->user_flags;
        if (request->flags & HORIZON_SET_USER_OBJECT_SET_FLAGS)
            entry->object->user_flags = request->obj_flags;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static struct horizon_user_class *horizon_server_find_class_locked( unsigned int atom,
                                                                    unsigned long long instance )
{
    struct horizon_user_class *class;

    for (class = horizon_classes; class; class = class->next)
    {
        if (class->atom != atom) continue;
        if (!instance || !class->local || class->instance == instance) return class;
    }
    return NULL;
}

static int horizon_server_is_desktop_class( const struct horizon_user_class *class )
{
    return class && !class->local && class->base_atom == HORIZON_DESKTOP_ATOM;
}

static int horizon_server_is_message_class( const struct horizon_user_class *class )
{
    return class && !class->local && class->name &&
           horizon_utf16_name_equals_ascii( class->name, class->name_len, "Message" );
}

static unsigned int horizon_server_class_name_from_request_locked( const struct horizon_create_class_request *request,
                                                                   const unsigned char *data,
                                                                   unsigned int data_size,
                                                                   unsigned char *buffer,
                                                                   unsigned int buffer_size,
                                                                   const unsigned char **name,
                                                                   unsigned int *name_len )
{
    char atom_name[32];

    *name = data;
    *name_len = data_size;
    if (*name_len) return HORIZON_STATUS_SUCCESS;
    if (!request->atom) return HORIZON_STATUS_INVALID_PARAMETER;

    snprintf( atom_name, sizeof(atom_name), "#%u", request->atom );
    *name_len = horizon_utf16_from_ascii( buffer, buffer_size, atom_name );
    *name = buffer;
    return *name_len ? HORIZON_STATUS_SUCCESS : HORIZON_STATUS_INVALID_PARAMETER;
}

static int horizon_server_handle_create_class( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_class_request *request = (const void *)message;
    struct horizon_create_class_reply reply;
    struct horizon_user_class *class;
    struct horizon_shared_object *shared;
    struct horizon_obj_locator locator;
    const unsigned char *name;
    unsigned char atom_name[32];
    unsigned int name_len, atom, base_atom, base_offset = 0;
    unsigned long long shm_size;

    memset( &reply, 0, sizeof(reply) );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_class_name_from_request_locked( request, data, data_size,
                                                                        atom_name, sizeof(atom_name),
                                                                        &name, &name_len );
    if (!reply.header.error)
    {
        atom = request->atom;
        if (!atom)
            reply.header.error = horizon_server_add_atom_locked( name, name_len, 0, &atom );
    }
    if (!reply.header.error)
    {
        if (request->name_offset && request->name_offset < name_len / sizeof(unsigned short))
        {
            base_offset = request->name_offset;
            reply.header.error = horizon_server_add_atom_locked( name + base_offset * sizeof(unsigned short),
                                                                 name_len - base_offset * sizeof(unsigned short),
                                                                 0, &base_atom );
        }
        else reply.header.error = horizon_server_add_atom_locked( name, name_len, atom, &base_atom );
    }
    if (!reply.header.error)
    {
        struct horizon_user_class *existing = horizon_server_find_class_locked( atom, request->instance );

        /* As Wine's server: callers such as quartz's video window check for this
         * error to reuse a class they registered before; a local and a global
         * class may share a name. */
        if (existing && !existing->local == !request->local)
            reply.header.error = 0xc0010582u;  /* ERROR_CLASS_ALREADY_EXISTS */
    }
    if (!reply.header.error &&
        (request->cls_extra < 0 || request->cls_extra > 4096 ||
         request->win_extra < 0 || request->win_extra > 4096 ||
         name_len > HORIZON_MAX_ATOM_LEN * sizeof(unsigned short)))
        reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
    if (!reply.header.error)
    {
        shm_size = offsetof( struct horizon_class_shm, extra ) + request->cls_extra;
        reply.header.error = horizon_server_alloc_shared_object_locked( shm_size, &locator );
    }
    if (!reply.header.error)
    {
        if (!(class = calloc( 1, sizeof(*class) ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else if (!(class->name = malloc( name_len )))
        {
            free( class );
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
        }
        else
        {
            memcpy( class->name, name, name_len );
            class->name_len = name_len;
            class->local = request->local;
            class->atom = atom;
            class->base_atom = base_atom;
            class->style = request->style;
            class->instance = request->instance;
            class->client_ptr = request->client_ptr;
            class->cls_extra = request->cls_extra;
            class->win_extra = request->win_extra;
            class->locator = locator;
            class->next = horizon_classes;
            horizon_classes = class;

            shared = horizon_server_shared_object_locked( locator );
            shared->shm.class.atom = base_atom;
            shared->shm.class.style = request->style;
            shared->shm.class.cls_extra = request->cls_extra;
            shared->shm.class.win_extra = request->win_extra;
            shared->shm.class.instance = request->instance;
            shared->shm.class.name_offset = base_offset;
            shared->shm.class.name_len = name_len;
            memcpy( shared->shm.class.name, name, name_len );
            memset( shared->shm.class.extra, 0, request->cls_extra );
            reply.header.error = horizon_server_flush_session_range_locked(
                locator.offset, offsetof( struct horizon_shared_object, shm ) + shm_size );
            reply.locator = locator;
            reply.atom = base_atom;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    TRACE( "Horizon server create_class atom %x base %x len %u status %08x.\n",
           request->atom, reply.atom, data_size, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_destroy_class( struct horizon_server_connection *connection,
                                                const unsigned char *message,
                                                const unsigned char *data, unsigned int data_size )
{
    const struct horizon_destroy_class_request *request = (const void *)message;
    struct horizon_server_reply_header reply;
    struct horizon_user_class **ptr;
    unsigned int atom = request->atom;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!atom) horizon_server_find_atom_locked( data, data_size, &atom );
    for (ptr = &horizon_classes; *ptr; ptr = &(*ptr)->next)
    {
        struct horizon_user_class *class = *ptr;

        if (class->atom != atom) continue;
        if (request->instance && class->local && class->instance != request->instance) continue;
        *ptr = class->next;
        free( class->name );
        free( class );
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
    }
    reply.error = HORIZON_STATUS_INVALID_HANDLE;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_server_create_window_locked( unsigned int parent_handle,
                                                         unsigned int owner_handle,
                                                         unsigned int atom,
                                                         unsigned long long class_instance,
                                                         unsigned long long instance,
                                                         unsigned int dpi_context,
                                                         unsigned int style,
                                                         unsigned int ex_style,
                                                         unsigned int pid,
                                                         unsigned int tid,
                                                         struct horizon_user_window **ret )
{
    struct horizon_server_object *desktop;
    struct horizon_user_window *parent = NULL;
    struct horizon_user_window *window;
    struct horizon_user_class *class;
    struct horizon_shared_object *shared;
    struct horizon_obj_locator locator;
    unsigned int desktop_handle = horizon_thread_desktop;
    unsigned int status, handle;

    *ret = NULL;
    if (!desktop_handle) return HORIZON_STATUS_ACCESS_DENIED;
    if (!(desktop = horizon_server_find_handle_object_locked( desktop_handle, HORIZON_SERVER_OBJECT_DESKTOP )))
        return HORIZON_STATUS_INVALID_HANDLE;
    if (!(class = horizon_server_find_class_locked( atom, class_instance )))
        return HORIZON_STATUS_INVALID_HANDLE;
    if (parent_handle && !(parent = horizon_server_find_window_locked( parent_handle )))
        return HORIZON_STATUS_INVALID_HANDLE;

    if (!parent_handle)
    {
        if (horizon_server_is_desktop_class( class )) parent_handle = desktop->desktop_top_window;
        else if (horizon_server_is_message_class( class ))
            parent_handle = desktop->desktop_msg_window ? desktop->desktop_top_window : 0;
        else if (!(parent_handle = desktop->desktop_top_window))
            return HORIZON_STATUS_ACCESS_DENIED;
        if (parent_handle && !(parent = horizon_server_find_window_locked( parent_handle )))
            return HORIZON_STATUS_INVALID_HANDLE;
    }

    if ((status = horizon_server_alloc_shared_object_locked( sizeof(struct horizon_window_shm), &locator )))
        return status;
    if (!(window = calloc( 1, sizeof(*window) ))) return HORIZON_STATUS_NO_MEMORY;
    if ((status = horizon_server_alloc_user_handle_locked( HORIZON_NTUSER_OBJ_WINDOW, locator,
                                                           pid, tid, &handle )))
    {
        free( window );
        return status;
    }

    shared = horizon_server_shared_object_locked( locator );
    shared->shm.window.class = class->locator;
    shared->shm.window.dpi_context = dpi_context ? dpi_context : HORIZON_NTUSER_DPI_PER_MONITOR_AWARE;
    if ((status = horizon_server_flush_session_range_locked(
             locator.offset, offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_window_shm) )))
    {
        free( window );
        return status;
    }

    window->handle = handle;
    window->parent = parent_handle;
    window->owner = owner_handle;
    window->pid = pid;
    window->tid = tid;
    window->atom = atom;
    window->style = style;
    window->ex_style = ex_style;
    window->is_unicode = 1;
    window->monitor_dpi = 96;
    window->instance = instance;
    window->last_active = handle;
    window->desktop_handle = desktop_handle;
    window->class = class;
    window->locator = locator;
    window->next = horizon_windows;
    horizon_windows = window;

    if (!parent)
    {
        if (horizon_server_is_desktop_class( class )) desktop->desktop_top_window = handle;
        else if (horizon_server_is_message_class( class )) desktop->desktop_msg_window = handle;
    }

    *ret = window;
    return HORIZON_STATUS_SUCCESS;
}

static int horizon_server_handle_create_window( struct horizon_server_connection *connection,
                                                const unsigned char *message,
                                                const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_window_request *request = (const void *)message;
    struct horizon_create_window_reply reply;
    struct horizon_user_window *window = NULL;
    unsigned int atom = request->atom;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!atom) horizon_server_find_atom_locked( data, data_size, &atom );
    if (!atom) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.header.error = horizon_server_create_window_locked( request->parent, request->owner,
                                                                  atom, request->class_instance,
                                                                  request->instance, request->dpi_context,
                                                                  request->style, request->ex_style,
                                                                  connection->pid, connection->tid,
                                                                  &window );
        if (!reply.header.error && window)
        {
            reply.handle = window->handle;
            reply.parent = window->parent;
            reply.owner = window->owner;
            reply.extra = window->class->win_extra;
            reply.class_ptr = window->class->client_ptr;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    TRACE( "Horizon server create_window atom %x parent %08x -> %08x status %08x.\n",
           atom, request->parent, reply.handle, reply.header.error );
    horizon_trace( "[HZUSER] create_window atom=%#x parent=%08x pid=%u tid=%u -> hwnd=%08x err=%08x\n",
                   atom, request->parent, connection->pid, connection->tid,
                   reply.handle, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_destroy_window( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_destroy_window_request *request = (const void *)message;
    struct horizon_user_window **ptr;
    struct horizon_input_shm *input;
    unsigned int status = HORIZON_STATUS_INVALID_HANDLE;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (ptr = &horizon_windows; *ptr; ptr = &(*ptr)->next)
    {
        struct horizon_user_window *window = *ptr;
        struct horizon_window_property *property;

        if (request->handle && window->handle != request->handle) continue;
        *ptr = window->next;
        horizon_message_queue_drop( &horizon_posted_messages, window->tid, window->handle );
        horizon_win_timers_drop( &horizon_timers, window->tid, window->handle );
        if (horizon_clip_window_destroyed( &horizon_clipboard, window->handle )) horizon_server_clipboard_notify_locked();
        if ((input = horizon_server_input_shared_locked()) && input->caret == window->handle)
        {
            horizon_server_set_caret_window_locked( input, 0, 0, 0 );
            horizon_server_flush_input_locked();
        }
        while ((property = window->properties))
        {
            window->properties = property->next;
            free( property );
        }
        free( window );
        status = HORIZON_STATUS_SUCCESS;
        if (request->handle) break;
    }
    if (!request->handle) status = HORIZON_STATUS_SUCCESS;
    horizon_server_refresh_queues_locked();
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static void horizon_server_window_screen_rects_locked( const struct horizon_user_window *window,
                                                       struct horizon_rectangle *window_rect,
                                                       struct horizon_rectangle *client_rect );

static void horizon_server_offset_rect( struct horizon_rectangle *rect, int x, int y )
{
    rect->left += x;
    rect->right += x;
    rect->top += y;
    rect->bottom += y;
}

static int horizon_server_rect_empty( const struct horizon_rectangle *rect )
{
    return rect->left >= rect->right || rect->top >= rect->bottom;
}

static struct horizon_rectangle horizon_server_intersect_rect( struct horizon_rectangle a,
                                                               struct horizon_rectangle b )
{
    struct horizon_rectangle ret;

    ret.left = a.left > b.left ? a.left : b.left;
    ret.top = a.top > b.top ? a.top : b.top;
    ret.right = a.right < b.right ? a.right : b.right;
    ret.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (horizon_server_rect_empty( &ret ))
        ret.left = ret.top = ret.right = ret.bottom = 0;
    return ret;
}

static struct horizon_rectangle horizon_server_union_rect( struct horizon_rectangle a,
                                                           struct horizon_rectangle b )
{
    if (horizon_server_rect_empty( &a )) return b;
    if (horizon_server_rect_empty( &b )) return a;
    if (b.left < a.left) a.left = b.left;
    if (b.top < a.top) a.top = b.top;
    if (b.right > a.right) a.right = b.right;
    if (b.bottom > a.bottom) a.bottom = b.bottom;
    return a;
}

static void horizon_server_window_screen_rects_locked( const struct horizon_user_window *window,
                                                       struct horizon_rectangle *window_rect,
                                                       struct horizon_rectangle *client_rect );

static void horizon_server_mark_window_update_locked( struct horizon_user_window *window,
                                                      struct horizon_rectangle dirty,
                                                      int erase )
{
    if (horizon_server_rect_empty( &dirty )) return;

    if (window->has_update_rect)
        window->update_rect = horizon_server_union_rect( window->update_rect, dirty );
    else
        window->update_rect = dirty;
    window->has_update_rect = 1;
    if (erase) window->needs_erase = 1;
}

static void horizon_server_redraw_children_locked( struct horizon_user_window *parent,
                                                   struct horizon_rectangle dirty,
                                                   unsigned int flags )
{
    struct horizon_user_window *child;

    if (flags & HORIZON_RDW_NOCHILDREN) return;
    if (parent->style & HORIZON_WS_MINIMIZE) return;
    if ((parent->style & HORIZON_WS_CLIPCHILDREN) &&
        !(flags & HORIZON_RDW_ALLCHILDREN)) return;

    for (child = horizon_windows; child; child = child->next)
    {
        struct horizon_rectangle child_window, child_client, child_dirty;

        if (child->parent != parent->handle || !(child->style & HORIZON_WS_VISIBLE)) continue;
        horizon_server_window_screen_rects_locked( child, &child_window, &child_client );
        child_dirty = horizon_server_intersect_rect( dirty, child_window );
        if (horizon_server_rect_empty( &child_dirty )) continue;

        horizon_server_mark_window_update_locked( child, child_dirty, 1 );
        child->needs_nonclient = 1;
        horizon_server_redraw_children_locked( child, child_dirty,
                                               flags | HORIZON_RDW_FRAME | HORIZON_RDW_ERASE );
    }
}

static unsigned int horizon_server_window_update_flags_locked( const struct horizon_user_window *window,
                                                               unsigned int flags )
{
    unsigned int ret = 0;

    if ((flags & HORIZON_UPDATE_NONCLIENT) && window->has_update_rect && window->needs_nonclient)
        ret |= HORIZON_UPDATE_NONCLIENT;
    if ((flags & HORIZON_UPDATE_ERASE) && window->has_update_rect && window->needs_erase)
        ret |= HORIZON_UPDATE_ERASE;
    if ((flags & HORIZON_UPDATE_PAINT) && window->has_update_rect)
        ret |= HORIZON_UPDATE_PAINT;
    if ((flags & HORIZON_UPDATE_INTERNALPAINT) && window->has_internal_paint)
        ret |= HORIZON_UPDATE_INTERNALPAINT;
    return ret;
}

static unsigned int horizon_server_find_window_update_locked( struct horizon_user_window *window,
                                                              struct horizon_user_window *from_child,
                                                              unsigned int flags, int *past_from,
                                                              struct horizon_user_window **found )
{
    struct horizon_user_window *child;
    unsigned int ret;

    if (!*past_from)
    {
        if (window == from_child) *past_from = 1;
    }
    else if ((ret = horizon_server_window_update_flags_locked( window, flags )))
    {
        *found = window;
        return ret;
    }

    if (flags & HORIZON_UPDATE_NOCHILDREN) return 0;
    if (window->style & HORIZON_WS_MINIMIZE) return 0;
    if (!(flags & HORIZON_UPDATE_ALLCHILDREN) &&
        !(window->style & HORIZON_WS_CLIPCHILDREN)) return 0;

    for (child = horizon_windows; child; child = child->next)
    {
        if (child->parent != window->handle || !(child->style & HORIZON_WS_VISIBLE)) continue;
        if ((ret = horizon_server_find_window_update_locked( child, from_child, flags,
                                                             past_from, found )))
            return ret;
    }
    return 0;
}

static unsigned long long horizon_server_get_window_info_locked( const struct horizon_user_window *window,
                                                                 int offset, unsigned int size,
                                                                 unsigned int *status )
{
    *status = HORIZON_STATUS_SUCCESS;
    switch (offset)
    {
    case HORIZON_GWL_STYLE:
        return window->style;
    case HORIZON_GWL_EXSTYLE:
        return window->ex_style;
    case HORIZON_GWLP_ID:
        return window->id;
    case HORIZON_GWLP_HINSTANCE:
        return window->instance;
    case HORIZON_GWLP_WNDPROC:
        return window->is_unicode;
    case HORIZON_GWLP_USERDATA:
        return window->user_data;
    default:
        if (size > sizeof(unsigned long long) ||
            offset < 0 || offset > window->class->win_extra - (int)size)
            *status = HORIZON_STATUS_INVALID_PARAMETER;
        return 0;
    }
}

static void horizon_server_set_window_info_locked( struct horizon_user_window *window, int offset,
                                                   unsigned long long value, unsigned int size,
                                                   unsigned long long *old_value,
                                                   unsigned int *status )
{
    *old_value = horizon_server_get_window_info_locked( window, offset, size, status );
    if (*status) return;

    switch (offset)
    {
    case HORIZON_GWL_STYLE:
        window->style = value;
        break;
    case HORIZON_GWL_EXSTYLE:
        window->ex_style = value;
        break;
    case HORIZON_GWLP_ID:
        window->id = value;
        break;
    case HORIZON_GWLP_HINSTANCE:
        window->instance = value;
        break;
    case HORIZON_GWLP_WNDPROC:
        window->is_unicode = value;
        break;
    case HORIZON_GWLP_USERDATA:
        window->user_data = value;
        break;
    default:
        /* Extra window bytes are owned by the client WND for same-process windows. */
        break;
    }
}

static int horizon_server_handle_set_window_owner( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_set_window_owner_request *request = (const void *)message;
    struct horizon_set_window_owner_reply reply;
    struct horizon_user_window *window;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->owner && !horizon_server_find_window_locked( request->owner ))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.prev_owner = window->owner;
        reply.full_owner = window->owner = request->owner;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_window_info( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_get_window_info_request *request = (const void *)message;
    struct horizon_get_window_info_reply reply;
    struct horizon_user_window *window;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.last_active = window->last_active ? window->last_active : window->handle;
        reply.is_unicode = window->is_unicode;
        reply.info = horizon_server_get_window_info_locked( window, request->offset,
                                                            request->size, &reply.header.error );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_init_window_info( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_init_window_info_request *request = (const void *)message;
    struct horizon_user_window *window;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        window->style = request->style;
        window->ex_style = request->ex_style;
        window->is_unicode = request->is_unicode;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_set_window_info( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_set_window_info_request *request = (const void *)message;
    struct horizon_set_window_info_reply reply;
    struct horizon_user_window *window;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
        horizon_server_set_window_info_locked( window, request->offset, request->new_info,
                                               request->size, &reply.old_info,
                                               &reply.header.error );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* All windows live in one list. A window's siblings are the entries with the
 * same parent, in list order; children of a parent are interleaved with their
 * own children, so neighbors in the list are not necessarily siblings. The
 * tree and the window lists below both follow this order. */
static void horizon_server_window_tree_locked( const struct horizon_user_window *window,
                                               struct horizon_get_window_tree_reply *reply )
{
    const struct horizon_user_window *iter;
    int seen = 0;

    reply->parent = window->parent;
    reply->owner = window->owner;
    for (iter = horizon_windows; iter; iter = iter->next)
    {
        if (iter->parent == window->handle)
        {
            if (!reply->first_child) reply->first_child = iter->handle;
            reply->last_child = iter->handle;
        }
        /* As in the Wine server, a window without a parent has no siblings. */
        if (!window->parent || iter->parent != window->parent) continue;
        if (!reply->first_sibling) reply->first_sibling = iter->handle;
        reply->last_sibling = iter->handle;
        if (iter == window) seen = 1;
        else if (!seen) reply->prev_sibling = iter->handle;
        else if (!reply->next_sibling) reply->next_sibling = iter->handle;
    }
}

static void horizon_server_append_window_locked( const struct horizon_user_window *window, unsigned int tid,
                                                 unsigned int *handles, unsigned int *count,
                                                 unsigned int max_count )
{
    if (tid && window->tid != tid) return;
    if (*count < max_count) handles[*count] = window->handle;
    (*count)++;
}

/* Children of "parent" from "start" on (all when NULL), each followed by its
 * descendants when "recurse" is set, as server/window.c get_window_list(). */
static void horizon_server_window_list_locked( unsigned int parent, const struct horizon_user_window *start,
                                               unsigned int tid, int recurse, unsigned int *handles,
                                               unsigned int *count, unsigned int max_count )
{
    const struct horizon_user_window *iter;

    for (iter = horizon_windows; iter; iter = iter->next)
    {
        if (iter->parent != parent) continue;
        if (start && iter != start) continue;
        start = NULL;
        horizon_server_append_window_locked( iter, tid, handles, count, max_count );
        if (recurse) horizon_server_window_list_locked( iter->handle, NULL, tid, 1, handles, count, max_count );
    }
}

static int horizon_server_handle_get_window_tree( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_get_window_tree_request *request = (const void *)message;
    struct horizon_get_window_tree_reply reply;
    struct horizon_user_window *window;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
        horizon_server_window_tree_locked( window, &reply );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* NtUserBuildHwndList: EnumWindows, EnumChildWindows and GetDlgItem. */
static int horizon_server_handle_get_window_list( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_get_window_list_request *request = (const void *)message;
    struct horizon_get_window_list_reply reply;
    struct horizon_user_window *window = NULL, *msg_window;
    struct horizon_server_object *desktop, *thread;
    unsigned int *handles = NULL, count = 0, returned, max_count = request->header.reply_size / sizeof(*handles);
    int ret;

    memset( &reply, 0, sizeof(reply) );
    if (max_count && !(handles = malloc( max_count * sizeof(*handles) )))
        reply.header.error = HORIZON_STATUS_NO_MEMORY;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (thread = horizon_server_threads; request->tid && thread; thread = thread->thread_next)
        if (thread->thread.tid == request->tid) break;
    if (reply.header.error) ;
    else if (request->handle && !(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->tid && !thread)
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->desktop || !window)  /* top-level windows */
    {
        desktop = horizon_server_find_handle_object_locked( request->desktop ? request->desktop :
                                                            horizon_thread_desktop,
                                                            HORIZON_SERVER_OBJECT_DESKTOP );
        if (request->desktop && !desktop) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
        else if (desktop && desktop->desktop_top_window && !(request->desktop && request->children))
            horizon_server_window_list_locked( desktop->desktop_top_window, NULL, request->tid, 0,
                                               handles, &count, max_count );
    }
    else if (request->children)
        horizon_server_window_list_locked( window->handle, NULL, request->tid, 1, handles, &count, max_count );
    else if (window->parent)  /* siblings from this window on */
        horizon_server_window_list_locked( window->parent, window, request->tid, 0, handles, &count, max_count );
    else
    {
        horizon_server_append_window_locked( window, request->tid, handles, &count, max_count );
        desktop = horizon_server_find_handle_object_locked( horizon_thread_desktop,
                                                            HORIZON_SERVER_OBJECT_DESKTOP );
        if (desktop && desktop->desktop_top_window == window->handle &&
            (msg_window = horizon_server_find_window_locked( desktop->desktop_msg_window )))
            horizon_server_append_window_locked( msg_window, request->tid, handles, &count, max_count );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.count = count;
    returned = count <= max_count ? count : 0;
    if (!reply.header.error && count > max_count) reply.header.error = HORIZON_STATUS_BUFFER_TOO_SMALL;
    if (reply.header.error) returned = 0;
    reply.header.reply_size = returned * sizeof(*handles);
    horizon_trace( "[HZUSER] get_window_list desktop=%x hwnd=%08x tid=%04x children=%d count=%u max=%u err=%08x\n",
                   request->desktop, request->handle, request->tid, request->children, count, max_count,
                   reply.header.error );
    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), handles,
                                      returned * sizeof(*handles) );
    free( handles );
    return ret;
}

/* server/window.c's link_window on the flat window list, where siblings keep
 * their z-order, top first: previous is a sibling's handle, or one of these.
 * SetWindowPos used to leave every window where it was created, so a window
 * raised over a later one stayed under it for the mouse (WarCraft III's menus
 * got no hover or clicks). */
#define HORIZON_LINK_TOP       0u
#define HORIZON_LINK_BOTTOM    1u
#define HORIZON_LINK_TOPMOST   0xffffffffu
#define HORIZON_LINK_NOTOPMOST 0xfffffffeu

static void horizon_server_link_window_locked( struct horizon_user_window *window, unsigned int previous )
{
    struct horizon_user_window **ptr, **before = NULL, *after = NULL, *sibling;

    if (previous == HORIZON_LINK_NOTOPMOST)
    {
        if (!(window->ex_style & HORIZON_WS_EX_TOPMOST)) return;  /* nothing to do */
        window->ex_style &= ~HORIZON_WS_EX_TOPMOST;
        previous = HORIZON_LINK_TOP;
    }
    for (ptr = &horizon_windows; *ptr; ptr = &(*ptr)->next)
    {
        if (*ptr != window) continue;
        *ptr = window->next;
        break;
    }
    window->next = NULL;

    if (previous != HORIZON_LINK_TOP && previous != HORIZON_LINK_BOTTOM && previous != HORIZON_LINK_TOPMOST)
    {
        after = horizon_server_find_window_locked( previous );
        window->next = after->next;
        after->next = window;
        if (!(after->ex_style & HORIZON_WS_EX_TOPMOST)) window->ex_style &= ~HORIZON_WS_EX_TOPMOST;
        else
        {
            for (sibling = window->next; sibling && sibling->parent != window->parent; sibling = sibling->next) ;
            if (sibling && (sibling->ex_style & HORIZON_WS_EX_TOPMOST)) window->ex_style |= HORIZON_WS_EX_TOPMOST;
        }
        return;
    }

    if (previous == HORIZON_LINK_BOTTOM) window->ex_style &= ~HORIZON_WS_EX_TOPMOST;
    if (previous == HORIZON_LINK_TOPMOST) window->ex_style |= HORIZON_WS_EX_TOPMOST;
    for (ptr = &horizon_windows; *ptr; ptr = &(*ptr)->next)
    {
        sibling = *ptr;
        if (sibling->parent != window->parent) continue;
        after = sibling;  /* the last sibling so far */
        if (before || previous == HORIZON_LINK_BOTTOM) continue;
        /* HWND_TOP puts a window that is not topmost above the first sibling
         * that is not either, or above its own topmost owner, and so topmost. */
        if ((window->ex_style & HORIZON_WS_EX_TOPMOST) || !(sibling->ex_style & HORIZON_WS_EX_TOPMOST))
            before = ptr;
        else if (sibling->handle == window->owner)
        {
            window->ex_style |= HORIZON_WS_EX_TOPMOST;
            before = ptr;
        }
    }
    if (before)
    {
        window->next = *before;
        *before = window;
    }
    else if (after)
    {
        window->next = after->next;
        after->next = window;
    }
    else
    {
        window->next = horizon_windows;
        horizon_windows = window;
    }
}

/* SetParent and GetAncestor's list of parents. Without them SetParent failed,
 * so a window given an owner that way (a DirectShow video window put in
 * WarCraft III's movie window) stayed top-level, over its would-be parent. */
#define HORIZON_REQ_SET_PARENT 151
#define HORIZON_REQ_GET_WINDOW_PARENTS 152

struct horizon_set_parent_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int parent;
    char pad[4];
};

struct horizon_set_parent_reply
{
    struct horizon_server_reply_header header;
    unsigned int old_parent;
    unsigned int full_parent;
};

struct horizon_get_window_parents_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_get_window_parents_reply
{
    struct horizon_server_reply_header header;
    int count;
    char pad[4];
};

/* server/window.c's set_parent_window: the window goes to the top of its new
 * siblings. Moving the desktop, or a window under itself or one of its own
 * children, is refused. */
static unsigned int horizon_server_set_parent_locked( struct horizon_user_window *window,
                                                      struct horizon_user_window *parent )
{
    struct horizon_user_window *ptr;

    if (!window->parent) return HORIZON_STATUS_INVALID_PARAMETER;
    for (ptr = parent; ptr; ptr = ptr->parent ? horizon_server_find_window_locked( ptr->parent ) : NULL)
        if (ptr == window) return HORIZON_STATUS_INVALID_PARAMETER;
    window->parent = parent->handle;
    horizon_server_link_window_locked( window, HORIZON_LINK_TOP );
    return HORIZON_STATUS_SUCCESS;
}

/* The window's parents, its own first, up to the desktop: all of them are
 * counted, and the first max_count stored. */
static unsigned int horizon_server_window_parents_locked( struct horizon_user_window *window,
                                                          unsigned int *handles, unsigned int max_count )
{
    unsigned int count = 0;

    while (window->parent)
    {
        if (count < max_count) handles[count] = window->parent;
        count++;
        if (!(window = horizon_server_find_window_locked( window->parent ))) break;
    }
    return count;
}

static int horizon_server_handle_set_parent( struct horizon_server_connection *connection,
                                             const unsigned char *message )
{
    const struct horizon_set_parent_request *request = (const void *)message;
    struct horizon_set_parent_reply reply;
    struct horizon_user_window *window, *parent = NULL;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )) ||
        !(parent = horizon_server_find_window_locked( request->parent )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        unsigned int old_parent = window->parent;

        if (!(reply.header.error = horizon_server_set_parent_locked( window, parent )))
        {
            reply.old_parent = old_parent;
            reply.full_parent = parent->handle;
        }
    }
    horizon_trace( "[HZUSER] set_parent hwnd=%08x parent=%08x old=%08x err=%08x\n",
                   request->handle, request->parent, reply.old_parent, reply.header.error );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_window_parents( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_get_window_parents_request *request = (const void *)message;
    struct horizon_get_window_parents_reply reply;
    struct horizon_user_window *window;
    unsigned int handles[64], count = 0, returned;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((window = horizon_server_find_window_locked( request->handle )))
        count = horizon_server_window_parents_locked( window, handles, sizeof(handles) / sizeof(*handles) );
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    /* As server/window.c: an unknown window has none; the reply holds as many
     * as fit, and the count of all of them. */
    reply.count = count;
    returned = count < sizeof(handles) / sizeof(*handles) ? count : sizeof(handles) / sizeof(*handles);
    if (returned > request->header.reply_size / sizeof(*handles))
        returned = request->header.reply_size / sizeof(*handles);
    reply.header.reply_size = returned * sizeof(*handles);
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), handles,
                                       reply.header.reply_size );
}

static int horizon_server_handle_set_window_pos( struct horizon_server_connection *connection,
                                                 const unsigned char *message,
                                                 const unsigned char *data, unsigned int data_size )
{
    const struct horizon_set_window_pos_request *request = (const void *)message;
    const struct horizon_rectangle *extra = (const void *)data;
    struct horizon_set_window_pos_reply reply;
    struct horizon_user_window *window, *previous;
    struct horizon_rectangle old_window, old_client;
    int geometry_changed;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    /* A window placed after another must be its sibling. */
    else if (!(request->swp_flags & HORIZON_SWP_NOZORDER) && window->parent && (int)request->previous > 1 &&
             request->previous != window->handle &&
             (!(previous = horizon_server_find_window_locked( request->previous )) ||
              previous->parent != window->parent))
        reply.header.error = previous ? HORIZON_STATUS_INVALID_PARAMETER : HORIZON_STATUS_INVALID_HANDLE;
    else if (request->window.right < request->window.left ||
             request->window.bottom < request->window.top)
        reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
    else
    {
        if (!(request->swp_flags & HORIZON_SWP_NOZORDER) && window->parent && request->previous != window->handle)
            horizon_server_link_window_locked( window, request->previous );
        old_window = window->window_rect;
        old_client = window->client_rect;
        window->window_rect = request->window;
        window->client_rect = request->client;
        horizon_trace( "[HZGEOM] hwnd=%08x parent=%08x swp=%x window=%d,%d-%d,%d client=%d,%d-%d,%d\n",
                       window->handle, window->parent, request->swp_flags,
                       request->window.left, request->window.top,
                       request->window.right, request->window.bottom,
                       request->client.left, request->client.top,
                       request->client.right, request->client.bottom );
        window->visible_rect = data_size >= sizeof(*extra) ? extra[0] : request->window;
        window->surface_rect = data_size >= 2 * sizeof(*extra) ? extra[1] : window->visible_rect;
        window->monitor_dpi = request->monitor_dpi ? request->monitor_dpi : 96;
        window->paint_flags = request->paint_flags;
        if (request->swp_flags & HORIZON_SWP_SHOWWINDOW) window->style |= HORIZON_WS_VISIBLE;
        if (request->swp_flags & HORIZON_SWP_HIDEWINDOW) window->style &= ~HORIZON_WS_VISIBLE;
        reply.new_style = window->style;
        reply.new_ex_style = window->ex_style;
        geometry_changed = memcmp( &old_window, &window->window_rect, sizeof(old_window) ) ||
                           memcmp( &old_client, &window->client_rect, sizeof(old_client) );
        if (geometry_changed && (window->style & HORIZON_WS_VISIBLE) &&
            !(request->swp_flags & HORIZON_SWP_NOREDRAW))
        {
            struct horizon_rectangle win_screen, client_screen;

            horizon_server_window_screen_rects_locked( window, &win_screen, &client_screen );
            window->has_update_rect = 0;
            window->needs_erase = 0;
            window->needs_nonclient = 0;
            horizon_server_mark_window_update_locked( window, client_screen, 1 );
            horizon_trace( "[HZPAINT] setpos resize hwnd=%08x rect=%d,%d-%d,%d swp=%x\n",
                           window->handle, client_screen.left, client_screen.top,
                           client_screen.right, client_screen.bottom, request->swp_flags );
        }
        if (window->paint_flags & HORIZON_SET_WINPOS_PAINT_SURFACE)
        {
            struct horizon_rectangle win_screen, client_screen;

            horizon_server_window_screen_rects_locked( window, &win_screen, &client_screen );
            if (!horizon_server_rect_empty( &client_screen ))
            {
                horizon_server_mark_window_update_locked( window, client_screen, 1 );
                horizon_trace( "[HZPAINT] setpos dirty hwnd=%08x rect=%d,%d-%d,%d flags=%x style=%x\n",
                               window->handle, client_screen.left, client_screen.top,
                               client_screen.right, client_screen.bottom,
                               window->paint_flags, window->style );
            }
            reply.surface_win = window->handle;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static void horizon_server_window_screen_rects_locked( const struct horizon_user_window *window,
                                                       struct horizon_rectangle *window_rect,
                                                       struct horizon_rectangle *client_rect )
{
    struct horizon_user_window *parent;

    *window_rect = window->window_rect;
    *client_rect = window->client_rect;
    while (window->parent && (parent = horizon_server_find_window_locked( window->parent )))
    {
        horizon_server_offset_rect( window_rect, parent->client_rect.left, parent->client_rect.top );
        horizon_server_offset_rect( client_rect, parent->client_rect.left, parent->client_rect.top );
        window = parent;
    }
}

static int horizon_server_point_in_rect( const struct horizon_rectangle *rect, int x, int y );

static void horizon_server_collect_windows_from_point_locked( struct horizon_user_window *window,
                                                              int x, int y, unsigned int *handles,
                                                              unsigned int max_handles,
                                                              unsigned int *count )
{
    struct horizon_rectangle window_rect, client_rect;
    struct horizon_user_window *child;

    horizon_server_window_screen_rects_locked( window, &window_rect, &client_rect );
    if (!horizon_server_point_in_rect( &window_rect, x, y )) return;

    if (!(window->style & (HORIZON_WS_MINIMIZE | HORIZON_WS_DISABLED)) &&
        horizon_server_point_in_rect( &client_rect, x, y ))
    {
        for (child = horizon_windows; child; child = child->next)
        {
            if (child->parent != window->handle || !(child->style & HORIZON_WS_VISIBLE)) continue;
            horizon_server_collect_windows_from_point_locked( child, x, y, handles,
                                                              max_handles, count );
        }
    }

    if (*count < max_handles) handles[*count] = window->handle;
    (*count)++;
}

static int horizon_server_handle_get_window_children_from_point(
    struct horizon_server_connection *connection, const unsigned char *message )
{
    const struct horizon_get_window_children_from_point_request *request = (const void *)message;
    struct horizon_get_window_children_from_point_reply reply;
    struct horizon_user_window *parent;
    unsigned int *handles = NULL;
    unsigned int max_handles, returned, count = 0;
    unsigned int data_size = 0;

    memset( &reply, 0, sizeof(reply) );
    max_handles = request->header.reply_size / sizeof(*handles);
    if (max_handles && !(handles = malloc( max_handles * sizeof(*handles) )))
        reply.header.error = HORIZON_STATUS_NO_MEMORY;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!reply.header.error && !(parent = horizon_server_find_window_locked( request->parent )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (!reply.header.error)
        horizon_server_collect_windows_from_point_locked( parent, request->x, request->y,
                                                          handles, max_handles, &count );
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.count = count;
    returned = reply.count < max_handles ? reply.count : max_handles;
    data_size = returned * sizeof(*handles);
    reply.header.reply_size = data_size;
    horizon_trace( "[HZINPUT] children_from_point parent=%08x pt=%d,%d count=%d returned=%u err=%08x\n",
                   request->parent, request->x, request->y, reply.count, returned,
                   reply.header.error );
    returned = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                           handles, data_size );
    free( handles );
    return returned;
}

static unsigned int horizon_server_input_time(void)
{
    return (unsigned int)(armTicksToNs( armGetSystemTick() ) / 1000000ull);
}

static struct horizon_desktop_shm *horizon_server_desktop_shared_locked(
    struct horizon_obj_locator *locator )
{
    struct horizon_server_object *desktop;
    struct horizon_shared_object *shared;

    if (!(desktop = horizon_server_find_handle_object_locked( horizon_input_desktop,
                                                              HORIZON_SERVER_OBJECT_DESKTOP )))
        return NULL;
    *locator = desktop->desktop_locator;
    if (!(shared = horizon_server_shared_object_locked( *locator ))) return NULL;
    return &shared->shm.desktop;
}

static int horizon_server_point_in_rect( const struct horizon_rectangle *rect, int x, int y )
{
    return x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom;
}

static unsigned int horizon_server_window_depth_locked( const struct horizon_user_window *window )
{
    unsigned int depth = 0;

    while (window && window->parent)
    {
        depth++;
        window = horizon_server_find_window_locked( window->parent );
    }
    return depth;
}

static int horizon_server_window_is_descendant_locked( const struct horizon_user_window *window,
                                                       unsigned int ancestor )
{
    while (window)
    {
        if (window->handle == ancestor) return 1;
        window = window->parent ? horizon_server_find_window_locked( window->parent ) : NULL;
    }
    return 0;
}

static struct horizon_user_window *horizon_server_shallow_window_from_point_locked( int x, int y )
{
    struct horizon_user_window *window;

    for (window = horizon_windows; window; window = window->next)
    {
        struct horizon_rectangle rect, client;

        if (!(window->style & HORIZON_WS_VISIBLE)) continue;
        if ((window->ex_style & (HORIZON_WS_EX_LAYERED | HORIZON_WS_EX_TRANSPARENT)) ==
            (HORIZON_WS_EX_LAYERED | HORIZON_WS_EX_TRANSPARENT)) continue;  /* transparent */
        /* Wine's hardware queue stores the shallow top-level window.  The
         * client-side window_from_point() then performs authoritative child
         * and non-client hit testing before dispatch. */
        if (horizon_server_window_depth_locked( window ) != 1) continue;
        horizon_server_window_screen_rects_locked( window, &rect, &client );
        if (horizon_server_point_in_rect( &rect, x, y )) return window;
    }
    return NULL;
}

void wine_nx_runtime_trace( const char *msg );  /* weak, defined below */

/* The runtime log names the window under the mouse whenever that changes: a
 * program that gets no hover or clicks usually has another window there. */
static void horizon_server_log_mouse_target_locked( const struct horizon_user_window *target, int x, int y,
                                                    unsigned int capture )
{
    static unsigned int last = ~0u;
    struct horizon_rectangle rect = {0}, client;
    char name[48], line[256];
    unsigned int handle = target ? target->handle : 0, i, len = 0;

    if (handle == last) return;
    last = handle;
    if (!target)
    {
        snprintf( line, sizeof(line), "[INPUT] mouse at %d,%d over no window (capture=%08x)", x, y, capture );
        wine_nx_runtime_trace( line );
        return;
    }
    if (target->class && target->class->name)
        for (i = 0; i + 1 < target->class->name_len && len < sizeof(name) - 1; i += 2)
            name[len++] = target->class->name[i + 1] || target->class->name[i] < 0x20 ||
                          target->class->name[i] > 0x7e ? '?' : (char)target->class->name[i];
    name[len] = 0;
    horizon_server_window_screen_rects_locked( target, &rect, &client );
    snprintf( line, sizeof(line), "[INPUT] mouse at %d,%d over hwnd=%08x class=%s atom=%04x tid=%u style=%08x "
              "ex=%08x rect=%d,%d-%d,%d capture=%08x", x, y, handle, len ? name : "-", target->atom, target->tid,
              target->style, target->ex_style, rect.left, rect.top, rect.right, rect.bottom, capture );
    wine_nx_runtime_trace( line );
}

static unsigned int horizon_server_queue_mouse_locked( struct horizon_user_window *window,
                                                       unsigned int msg, unsigned long long wparam,
                                                       int x, int y, unsigned int time,
                                                       unsigned long long info )
{
    struct horizon_input_message *queued;

    if (!window) return HORIZON_STATUS_SUCCESS;
    if (!(queued = calloc( 1, sizeof(*queued) ))) return HORIZON_STATUS_NO_MEMORY;
    queued->id = horizon_next_input_message_id++;
    if (!queued->id) queued->id = horizon_next_input_message_id++;
    queued->tid = window->tid;
    queued->win = window->handle;
    queued->msg = msg;
    queued->wparam = wparam;
    queued->x = x;
    queued->y = y;
    queued->time = time;
    queued->info = info;
    *horizon_input_messages_tail = queued;
    horizon_input_messages_tail = &queued->next;
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_server_queue_key_locked( struct horizon_user_window *window, unsigned int msg,
                                                     unsigned long long wparam, unsigned long long lparam,
                                                     int x, int y, unsigned int time, unsigned long long info,
                                                     unsigned int data_flags, const struct horizon_raw_keyboard *raw )
{
    struct horizon_input_message *queued;

    if (!window) return HORIZON_STATUS_SUCCESS;
    if (!(queued = calloc( 1, sizeof(*queued) ))) return HORIZON_STATUS_NO_MEMORY;
    queued->id = horizon_next_input_message_id++;
    if (!queued->id) queued->id = horizon_next_input_message_id++;
    queued->tid = window->tid;
    queued->win = window->handle;
    queued->msg = msg;
    queued->wparam = wparam;
    queued->lparam = lparam;
    queued->x = x;
    queued->y = y;
    queued->time = time;
    queued->info = info;
    queued->device = HORIZON_IMDT_KEYBOARD;
    queued->data_flags = data_flags;
    if (raw)
    {
        queued->raw_keyboard = 1;
        queued->raw = *raw;
    }
    *horizon_input_messages_tail = queued;
    horizon_input_messages_tail = &queued->next;
    return HORIZON_STATUS_SUCCESS;
}

/* WM_INPUT for a raw mouse registration, which is how DirectInput 8 reads the
 * mouse. The cursor position rides along, as server/queue.c sends it. */
static unsigned int horizon_server_queue_raw_mouse_locked( struct horizon_user_window *window, int x, int y,
                                                           unsigned int time, unsigned long long info,
                                                           const struct horizon_raw_mouse *raw )
{
    struct horizon_input_message *queued;

    if (!window) return HORIZON_STATUS_SUCCESS;
    if (!(queued = calloc( 1, sizeof(*queued) ))) return HORIZON_STATUS_NO_MEMORY;
    queued->id = horizon_next_input_message_id++;
    if (!queued->id) queued->id = horizon_next_input_message_id++;
    queued->tid = window->tid;
    queued->win = window->handle;
    queued->msg = HORIZON_WM_INPUT;
    queued->wparam = HORIZON_RIM_INPUT;
    queued->x = x;
    queued->y = y;
    queued->time = time;
    queued->info = info;
    queued->device = HORIZON_IMDT_MOUSE;
    queued->raw_mouse = 1;
    queued->raw_m = *raw;
    *horizon_input_messages_tail = queued;
    horizon_input_messages_tail = &queued->next;
    return HORIZON_STATUS_SUCCESS;
}

/* Whether a key message is still waiting for the program to take it. The
 * thread's key state then follows those messages, and is not brought up to
 * the desktop's: server/queue.c locks it while hardware messages are being
 * processed. */
static int horizon_server_key_messages_pending_locked( void )
{
    struct horizon_input_message *queued;

    for (queued = horizon_input_messages; queued; queued = queued->next)
        if (queued->device == HORIZON_IMDT_KEYBOARD && !queued->raw_keyboard &&
            queued->msg >= HORIZON_KBD_WM_KEYDOWN && queued->msg <= HORIZON_KBD_WM_SYSKEYUP)
            return 1;
    return 0;
}

/* server/queue.c: sync_input_keystate. The thread's state takes the keys
 * that changed on the desktop since it last did, keeping what the thread set
 * for itself (SetKeyboardState) for the others. */
static unsigned char horizon_input_desktop_keystate[256];

static void horizon_server_sync_keystate_locked( struct horizon_input_shm *input,
                                                 const struct horizon_desktop_shm *desktop )
{
    unsigned int i;

    if (horizon_server_key_messages_pending_locked()) return;
    for (i = 0; i < 256; i++)
    {
        if (horizon_input_desktop_keystate[i] == desktop->keystate[i]) continue;
        input->keystate[i] = horizon_input_desktop_keystate[i] = desktop->keystate[i];
    }
    input->keystate_serial = desktop->keystate_serial;
}

static void horizon_server_remove_input_message_locked( struct horizon_input_message *message )
{
    struct horizon_input_message **ptr;

    for (ptr = &horizon_input_messages; *ptr; ptr = &(*ptr)->next)
    {
        if (*ptr != message) continue;
        *ptr = message->next;
        if (horizon_input_messages_tail == &message->next) horizon_input_messages_tail = ptr;
        free( message );
        return;
    }
}

static int horizon_server_msg_in_filter( const struct horizon_get_message_request *request, unsigned int msg )
{
    return msg >= request->get_first && msg <= request->get_last;
}

/* The client decides the final message after hit testing and double-click
 * detection, so match every form a queued message can take, as the Wine
 * server does.  Menu tracking peeks a click, sees a double-click, and then
 * removes it with only that message in the filter. */
static int horizon_server_mouse_message_matches( const struct horizon_get_message_request *request,
                                                 const struct horizon_input_message *queued )
{
    unsigned int msg = queued->msg;

    if (!request->get_first && !request->get_last) return 1;
    if (horizon_server_msg_in_filter( request, msg )) return 1;
    if (horizon_server_msg_in_filter( request, msg + HORIZON_WM_NCMOUSEFIRST - HORIZON_WM_MOUSEFIRST ))
        return 1;
    if (msg != HORIZON_WM_LBUTTONDOWN && msg != HORIZON_WM_RBUTTONDOWN) return 0;
    return horizon_server_msg_in_filter( request, msg + HORIZON_WM_LBUTTONDBLCLK - HORIZON_WM_LBUTTONDOWN ) ||
           horizon_server_msg_in_filter( request, msg + HORIZON_WM_NCLBUTTONDBLCLK - HORIZON_WM_LBUTTONDOWN );
}

/* Button transitions in the order the Wine server queues them. */
static const struct horizon_mouse_button_event
{
    unsigned int flag;
    unsigned int msg;
    unsigned int mk;
    unsigned int vk;
    int down;
} horizon_mouse_button_events[] =
{
    { HORIZON_MOUSEEVENTF_LEFTDOWN,  HORIZON_WM_LBUTTONDOWN, HORIZON_MK_LBUTTON, HORIZON_VK_LBUTTON, 1 },
    { HORIZON_MOUSEEVENTF_LEFTUP,    HORIZON_WM_LBUTTONUP,   HORIZON_MK_LBUTTON, HORIZON_VK_LBUTTON, 0 },
    { HORIZON_MOUSEEVENTF_RIGHTDOWN, HORIZON_WM_RBUTTONDOWN, HORIZON_MK_RBUTTON, HORIZON_VK_RBUTTON, 1 },
    { HORIZON_MOUSEEVENTF_RIGHTUP,   HORIZON_WM_RBUTTONUP,   HORIZON_MK_RBUTTON, HORIZON_VK_RBUTTON, 0 },
};

/* A controller key (wine_nx_send_keys, dlls/win32u/winnx_drv.c), as
 * server/queue.c queues it: WM_INPUT for a raw keyboard registration, which is
 * how DirectInput 8 reads keys, and the key message for the focus window unless
 * that registration asked for RIDEV_NOLEGACY. */
static int horizon_server_handle_send_keyboard( struct horizon_server_connection *connection,
                                                const struct horizon_send_hardware_message_request *request )
{
    const struct horizon_hw_keyboard_input *kbd = &request->input.kbd;
    struct horizon_send_hardware_message_reply reply;
    const struct horizon_rawinput_device *raw_device = NULL;
    struct horizon_user_window *focus = NULL, *raw_target = NULL;
    struct horizon_obj_locator desktop_locator;
    struct horizon_input_shm *input;
    struct horizon_desktop_shm *desktop;
    struct horizon_key_event event;
    unsigned int status = HORIZON_STATUS_SUCCESS, time, focus_handle = 0, raw_handle = 0;
    int legacy = 0;

    memset( &reply, 0, sizeof(reply) );
    memset( &event, 0, sizeof(event) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(input = horizon_server_input_shared_locked()) ||
        !(desktop = horizon_server_desktop_shared_locked( &desktop_locator )))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        time = kbd->time ? kbd->time : horizon_server_input_time();
        horizon_keyboard_event( desktop->keystate, &horizon_alt_pressed, kbd->vkey, kbd->scan, kbd->flags,
                                (unsigned int)kbd->info, &event );
        if (request->win) focus = horizon_server_find_window_locked( request->win );
        if (!focus && input->focus) focus = horizon_server_find_window_locked( input->focus );
        if (!focus && input->active) focus = horizon_server_find_window_locked( input->active );

        if ((raw_device = horizon_rawinput_find( horizon_rawinput_devices, horizon_rawinput_device_count,
                                                 HORIZON_RAWINPUT_USAGE_KEYBOARD )))
        {
            raw_target = raw_device->target ? horizon_server_find_window_locked( raw_device->target ) : focus;
            status = horizon_server_queue_key_locked( raw_target, HORIZON_WM_INPUT, HORIZON_RIM_INPUT, 0,
                                                      desktop->cursor.x, desktop->cursor.y, time, kbd->info,
                                                      kbd->flags, &event.raw );
        }
        legacy = !raw_device || !(raw_device->flags & HORIZON_RIDEV_NOLEGACY);
        if (!status && legacy)
            status = horizon_server_queue_key_locked( focus, event.message, event.vkey, event.lparam,
                                                      desktop->cursor.x, desktop->cursor.y, time, kbd->info,
                                                      event.data_flags, NULL );
        /* The desktop's state is the asynchronous one and changes now; the
         * thread's changes as the program takes each key message
         * (accept_hardware_message), as server/queue.c keeps it: Shift sent
         * down, A down and up and Shift up together must still find Shift
         * down when TranslateMessage makes a character of the A. */
        horizon_keyboard_update_state( desktop->keystate, event.message, event.vkey, 0xc0 );
        desktop->keystate_serial++;
        horizon_server_sync_keystate_locked( input, desktop );

        if (raw_target)
        {
            struct horizon_msgq *queue = horizon_server_queue_locked( raw_target->tid );

            raw_handle = raw_target->handle;
            if (queue) horizon_msgq_touch( queue, HORIZON_MSGQ_QS_RAWINPUT );
        }
        if (focus)
        {
            struct horizon_msgq *queue = horizon_server_queue_locked( focus->tid );

            focus_handle = focus->handle;
            if (queue && legacy) horizon_msgq_touch( queue, HORIZON_MSGQ_QS_KEY );
        }
        horizon_server_refresh_queues_locked();
        reply.prev_x = reply.new_x = desktop->cursor.x;
        reply.prev_y = reply.new_y = desktop->cursor.y;
        horizon_server_flush_input_locked();
        horizon_server_flush_session_range_locked(
            desktop_locator.offset,
            offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_desktop_shm) );
    }
    reply.header.error = status;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZINPUT] key vk=%02x scan=%02x flags=%x msg=%x focus=%08x raw=%08x legacy=%d err=%08x\n",
                   kbd->vkey, kbd->scan, kbd->flags, event.message, focus_handle, raw_handle, legacy, status );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_send_hardware_message( struct horizon_server_connection *connection,
                                                        const unsigned char *message )
{
    const struct horizon_send_hardware_message_request *request = (const void *)message;
    const struct horizon_hw_mouse_input *mouse = &request->input.mouse;
    struct horizon_send_hardware_message_reply reply;
    struct horizon_input_shm *input;
    struct horizon_desktop_shm *desktop;
    struct horizon_user_window *target = NULL, *raw_target = NULL;
    const struct horizon_rawinput_device *raw_device = NULL;
    struct horizon_obj_locator desktop_locator;
    struct horizon_raw_mouse raw;
    unsigned int status = HORIZON_STATUS_SUCCESS, time, flags, target_handle = 0, i;
    int x, y, dx = 0, dy = 0, legacy = 1;

    if (request->input.type == HORIZON_INPUT_KEYBOARD)
        return horizon_server_handle_send_keyboard( connection, request );

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->input.type != HORIZON_INPUT_MOUSE)
        status = HORIZON_STATUS_NOT_IMPLEMENTED;
    else if (!(input = horizon_server_input_shared_locked()) ||
             !(desktop = horizon_server_desktop_shared_locked( &desktop_locator )))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.prev_x = desktop->cursor.x;
        reply.prev_y = desktop->cursor.y;
        flags = mouse->flags;
        raw_device = horizon_rawinput_find( horizon_rawinput_devices, horizon_rawinput_device_count,
                                            HORIZON_RAWINPUT_USAGE_MOUSE );
        /* A program that asked for the mouse alone gets no mouse messages and
         * leaves the cursor where it is, as RIDEV_NOLEGACY says. */
        legacy = !raw_device || !(raw_device->flags & HORIZON_RIDEV_NOLEGACY);
        if (flags & HORIZON_MOUSEEVENTF_MOVE)
        {
            x = (flags & HORIZON_MOUSEEVENTF_ABSOLUTE) ? mouse->x : desktop->cursor.x + mouse->x;
            y = (flags & HORIZON_MOUSEEVENTF_ABSOLUTE) ? mouse->y : desktop->cursor.y + mouse->y;
            /* What the mouse did, before the screen's edges are applied: a view
             * being turned must not stop because the cursor reached a corner.
             * A touch points at a place rather than moving by an amount, and
             * the amount is from the last place it pointed at, as a finger
             * drawn across a trackpad -- not from the cursor, which a program
             * holding the mouse leaves in a corner. */
            if (!(flags & HORIZON_MOUSEEVENTF_ABSOLUTE))
            {
                dx = mouse->x;
                dy = mouse->y;
            }
            else
            {
                dx = horizon_pointed_at_x == HORIZON_NOWHERE ? 0 : mouse->x - horizon_pointed_at_x;
                dy = horizon_pointed_at_y == HORIZON_NOWHERE ? 0 : mouse->y - horizon_pointed_at_y;
            }
        }
        /* A touch that has just begun is where the next one is measured from. */
        if (flags & HORIZON_MOUSEEVENTF_ABSOLUTE)
        {
            if (flags & HORIZON_MOUSEEVENTF_LEFTDOWN) dx = dy = 0;
            horizon_pointed_at_x = mouse->x;
            horizon_pointed_at_y = mouse->y;
        }
        if (flags & HORIZON_MOUSEEVENTF_MOVE)
        {
            if (x < desktop->cursor.clip.left) x = desktop->cursor.clip.left;
            if (y < desktop->cursor.clip.top) y = desktop->cursor.clip.top;
            if (x >= desktop->cursor.clip.right) x = desktop->cursor.clip.right - 1;
            if (y >= desktop->cursor.clip.bottom) y = desktop->cursor.clip.bottom - 1;
            if (legacy)
            {
                desktop->cursor.x = x;
                desktop->cursor.y = y;
            }
            else
            {
                x = desktop->cursor.x;
                y = desktop->cursor.y;
            }
        }
        else
        {
            x = desktop->cursor.x;
            y = desktop->cursor.y;
        }
        time = mouse->time ? mouse->time : horizon_server_input_time();
        desktop->cursor.last_change = time;

        if (input->capture) target = horizon_server_find_window_locked( input->capture );
        if (!target && request->win) target = horizon_server_find_window_locked( request->win );
        if (!target) target = horizon_server_shallow_window_from_point_locked( x, y );
        if (target) target_handle = target->handle;
        horizon_server_log_mouse_target_locked( target, x, y, input->capture );

        if (raw_device)
        {
            raw_target = raw_device->target ? horizon_server_find_window_locked( raw_device->target ) : target;
            horizon_raw_mouse_event( flags, dx, dy, mouse->info, &raw );
            if ((status = horizon_server_queue_raw_mouse_locked( raw_target, x, y, time, mouse->info, &raw )))
                goto done;
        }
        if (legacy && (flags & HORIZON_MOUSEEVENTF_MOVE) &&
            (status = horizon_server_queue_mouse_locked( target, HORIZON_WM_MOUSEMOVE,
                                                         horizon_mouse_buttons, x, y, time,
                                                         mouse->info )))
            goto done;
        for (i = 0; i < sizeof(horizon_mouse_button_events) / sizeof(horizon_mouse_button_events[0]); i++)
        {
            const struct horizon_mouse_button_event *event = &horizon_mouse_button_events[i];

            if (!(flags & event->flag)) continue;
            if (event->down) horizon_mouse_buttons |= event->mk;
            else horizon_mouse_buttons &= ~event->mk;
            input->keystate[event->vk] = event->down ? 0x80 : 0;
            desktop->keystate[event->vk] = event->down ? 0x80 : 0;
            if (!legacy) continue;
            if ((status = horizon_server_queue_mouse_locked( target, event->msg,
                                                             horizon_mouse_buttons, x, y, time,
                                                             mouse->info )))
                goto done;
        }
        input->keystate_serial++;
        desktop->keystate_serial++;
done:
        if (raw_target)
        {
            struct horizon_msgq *queue = horizon_server_queue_locked( raw_target->tid );

            if (queue) horizon_msgq_touch( queue, HORIZON_MSGQ_QS_RAWINPUT );
        }
        if (target && legacy)
        {
            struct horizon_msgq *queue = horizon_server_queue_locked( target->tid );

            if (queue)
            {
                horizon_msgq_touch( queue, (flags & HORIZON_MOUSEEVENTF_MOVE) ? HORIZON_MSGQ_QS_MOUSEMOVE : 0 );
                horizon_msgq_touch( queue, (flags & ~(HORIZON_MOUSEEVENTF_MOVE | HORIZON_MOUSEEVENTF_ABSOLUTE))
                                           ? HORIZON_MSGQ_QS_MOUSEBUTTON : 0 );
            }
        }
        if (target || raw_target) horizon_server_refresh_queues_locked();
        reply.new_x = desktop->cursor.x;
        reply.new_y = desktop->cursor.y;
        horizon_server_flush_input_locked();
        horizon_server_flush_session_range_locked(
            desktop_locator.offset,
            offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_desktop_shm) );
    }
    reply.header.error = status;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZINPUT] mouse flags=%x x=%d y=%d target=%08x buttons=%x err=%08x\n",
                   mouse->flags, reply.new_x, reply.new_y, target_handle,
                   horizon_mouse_buttons, status );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* RegisterRawInputDevices sends the process's whole list each time. */
static int horizon_server_handle_update_rawinput_devices( struct horizon_server_connection *connection,
                                                          const unsigned char *data, unsigned int data_size )
{
    unsigned int count = data_size / sizeof(struct horizon_rawinput_device), status = HORIZON_STATUS_SUCCESS, i;
    struct horizon_rawinput_device *devices = NULL;

    if (count && !(devices = malloc( count * sizeof(*devices) ))) status = HORIZON_STATUS_NO_MEMORY;
    else
    {
        if (count) memcpy( devices, data, count * sizeof(*devices) );
        pthread_mutex_lock( &horizon_server_objects_mutex );
        free( horizon_rawinput_devices );
        horizon_rawinput_devices = devices;
        horizon_rawinput_device_count = count;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    horizon_trace( "[HZINPUT] update_rawinput_devices count=%u err=%08x\n", count, status );
    for (i = 0; devices && i < count; i++)
        horizon_trace( "[HZINPUT]   usage=%08x flags=%x target=%08x\n",
                       ((const struct horizon_rawinput_device *)data)[i].usage,
                       ((const struct horizon_rawinput_device *)data)[i].flags,
                       ((const struct horizon_rawinput_device *)data)[i].target );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_accept_hardware_message( struct horizon_server_connection *connection,
                                                          const unsigned char *message )
{
    const struct horizon_accept_hardware_message_request *request = (const void *)message;
    struct horizon_input_message **ptr;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (ptr = &horizon_input_messages; *ptr; ptr = &(*ptr)->next)
    {
        struct horizon_input_message *queued = *ptr;

        if (queued->id != request->hw_id) continue;
        /* The thread's key state follows the key messages it has taken
         * (server/queue.c: release_hardware_message). */
        if (queued->device == HORIZON_IMDT_KEYBOARD && !queued->raw_keyboard &&
            queued->msg >= HORIZON_KBD_WM_KEYDOWN && queued->msg <= HORIZON_KBD_WM_SYSKEYUP)
        {
            struct horizon_input_shm *input = horizon_server_input_shared_locked();
            struct horizon_obj_locator desktop_locator;
            struct horizon_desktop_shm *desktop = horizon_server_desktop_shared_locked( &desktop_locator );

            *ptr = queued->next;
            if (horizon_input_messages_tail == &queued->next) horizon_input_messages_tail = ptr;
            if (input)
            {
                horizon_keyboard_update_state( input->keystate, queued->msg, (unsigned int)queued->wparam, 0x80 );
                input->keystate_serial++;
                if (desktop) horizon_server_sync_keystate_locked( input, desktop );
                horizon_server_flush_input_locked();
            }
            free( queued );
            break;
        }
        *ptr = queued->next;
        if (horizon_input_messages_tail == &queued->next) horizon_input_messages_tail = ptr;
        free( queued );
        break;
    }
    horizon_server_refresh_queues_locked();
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_SUCCESS );
}

/* server/queue.c: get_key_state. GetAsyncKeyState reads the desktop's state
 * and clears its pressed-since bit; GetKeyState the thread's, brought up to
 * the desktop's unless key messages are still to be taken. */
static int horizon_server_handle_get_key_state( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct horizon_get_key_state_request *request = (const void *)message;
    struct horizon_get_key_state_reply reply;
    struct horizon_obj_locator desktop_locator;
    struct horizon_desktop_shm *desktop;
    struct horizon_input_shm *input;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(input = horizon_server_input_shared_locked()) ||
        !(desktop = horizon_server_desktop_shared_locked( &desktop_locator )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->async)
    {
        reply.state = desktop->keystate[request->key & 0xff];
        desktop->keystate[request->key & 0xff] &= ~0x40;
        desktop->keystate_serial++;
        horizon_server_flush_session_range_locked(
            desktop_locator.offset,
            offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_desktop_shm) );
    }
    else
    {
        horizon_server_sync_keystate_locked( input, desktop );
        reply.state = input->keystate[request->key & 0xff];
        horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_thread_input( struct horizon_server_connection *connection )
{
    struct horizon_get_thread_input_reply reply;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_ensure_input_locked();
    if (!reply.header.error) reply.locator = horizon_input_locator;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_foreground_window( struct horizon_server_connection *connection,
                                                        const unsigned char *message )
{
    const struct horizon_set_foreground_window_request *request = (const void *)message;
    struct horizon_set_foreground_window_reply reply;
    struct horizon_input_shm *input;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->handle && !horizon_server_find_window_locked( request->handle ))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (!(input = horizon_server_input_shared_locked()))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.previous = input->active;
        input->foreground = 1;
        horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_input_window( struct horizon_server_connection *connection,
                                                   const unsigned char *message, int which )
{
    const struct horizon_input_window_request *request = (const void *)message;
    struct horizon_input_window_reply reply;
    struct horizon_input_shm *input;
    unsigned int *slot;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->handle && !horizon_server_find_window_locked( request->handle ))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (!(input = horizon_server_input_shared_locked()))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        slot = which == HORIZON_REQ_SET_FOCUS_WINDOW ? &input->focus : &input->active;
        reply.previous = *slot;
        *slot = request->handle;
        horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_capture_window( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_set_capture_window_request *request = (const void *)message;
    struct horizon_set_capture_window_reply reply;
    struct horizon_input_shm *input;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->handle && !horizon_server_find_window_locked( request->handle ))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (!(input = horizon_server_input_shared_locked()))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.previous = input->capture;
        input->capture = request->handle;
        input->menu_owner = (request->flags & HORIZON_CAPTURE_MENU) ? request->handle : 0;
        input->move_size = (request->flags & HORIZON_CAPTURE_MOVESIZE) ? request->handle : 0;
        reply.full_handle = input->capture;
        horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_caret_window( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_set_caret_window_request *request = (const void *)message;
    struct horizon_set_caret_window_reply reply;
    struct horizon_input_shm *input;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->handle && !horizon_server_find_window_locked( request->handle ))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (!(input = horizon_server_input_shared_locked()))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.previous = input->caret;
        reply.old_rect = input->caret_rect;
        reply.old_hide = horizon_caret_hide;
        reply.old_state = horizon_caret_state;
        horizon_server_set_caret_window_locked( input, request->handle, request->width, request->height );
        horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZCARET] window hwnd=%08x size=%dx%d previous=%08x err=%08x\n",
                   request->handle, request->width, request->height, reply.previous, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_caret_info( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_set_caret_info_request *request = (const void *)message;
    struct horizon_set_caret_info_reply reply;
    struct horizon_input_shm *input;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(input = horizon_server_input_shared_locked()))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.full_handle = input->caret;
        reply.old_rect = input->caret_rect;
        reply.old_hide = horizon_caret_hide;
        reply.old_state = horizon_caret_state;
        reply.header.error = horizon_server_set_caret_info_locked( input, request->flags, request->handle,
                                                                   request->x, request->y, request->hide,
                                                                   request->state );
        if (!reply.header.error && (request->flags & HORIZON_SET_CARET_POS)) horizon_server_flush_input_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZCARET] info flags=%x hwnd=%08x pos=%d,%d hide=%d state=%d -> caret=%08x "
                   "hide=%d state=%d err=%08x\n", request->flags, request->handle, request->x, request->y,
                   request->hide, request->state, reply.full_handle, horizon_caret_hide, horizon_caret_state,
                   reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_cursor( struct horizon_server_connection *connection,
                                             const unsigned char *message )
{
    const struct horizon_set_cursor_request *request = (const void *)message;
    struct horizon_set_cursor_reply reply;
    struct horizon_input_shm *input;
    struct horizon_desktop_shm *desktop;
    struct horizon_obj_locator desktop_locator;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(input = horizon_server_input_shared_locked()) ||
        !(desktop = horizon_server_desktop_shared_locked( &desktop_locator )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.prev_handle = input->cursor;
        reply.prev_count = input->cursor_count;
        reply.prev_x = desktop->cursor.x;
        reply.prev_y = desktop->cursor.y;
        if (request->flags & HORIZON_SET_CURSOR_HANDLE) input->cursor = request->handle;
        if (request->flags & HORIZON_SET_CURSOR_COUNT) input->cursor_count += request->show_count;
        if (request->flags & HORIZON_SET_CURSOR_POS)
        {
            desktop->cursor.x = request->x;
            desktop->cursor.y = request->y;
            desktop->cursor.last_change = horizon_server_input_time();
        }
        if (request->flags & HORIZON_SET_CURSOR_CLIP) desktop->cursor.clip = request->clip;
        if (request->flags & HORIZON_SET_CURSOR_NOCLIP)
        {
            desktop->cursor.clip.left = desktop->cursor.clip.top = 0;
            desktop->cursor.clip.right = 1280;
            desktop->cursor.clip.bottom = 720;
        }
        reply.new_x = desktop->cursor.x;
        reply.new_y = desktop->cursor.y;
        reply.new_clip = desktop->cursor.clip;
        reply.last_change = desktop->cursor.last_change;
        horizon_server_flush_input_locked();
        horizon_server_flush_session_range_locked(
            desktop_locator.offset,
            offsetof( struct horizon_shared_object, shm ) + sizeof(struct horizon_desktop_shm) );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_window_rectangles( struct horizon_server_connection *connection,
                                                        const unsigned char *message )
{
    const struct horizon_get_window_rectangles_request *request = (const void *)message;
    struct horizon_get_window_rectangles_reply reply;
    struct horizon_user_window *window;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->handle )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.window = window->window_rect;
        reply.client = window->client_rect;
        switch (request->relative)
        {
        case HORIZON_COORDS_CLIENT:
            horizon_server_offset_rect( &reply.window, -window->client_rect.left, -window->client_rect.top );
            horizon_server_offset_rect( &reply.client, -window->client_rect.left, -window->client_rect.top );
            break;
        case HORIZON_COORDS_WINDOW:
            horizon_server_offset_rect( &reply.window, -window->window_rect.left, -window->window_rect.top );
            horizon_server_offset_rect( &reply.client, -window->window_rect.left, -window->window_rect.top );
            break;
        case HORIZON_COORDS_PARENT:
            break;
        case HORIZON_COORDS_SCREEN:
            horizon_server_window_screen_rects_locked( window, &reply.window, &reply.client );
            break;
        default:
            reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
            break;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static struct horizon_user_window *horizon_server_get_surface_window_locked( struct horizon_user_window *window )
{
    struct horizon_user_window *parent;

    while (!(window->paint_flags & HORIZON_SET_WINPOS_PAINT_SURFACE) && window->parent &&
           (parent = horizon_server_find_window_locked( window->parent )))
        window = parent;

    return window;
}

static int horizon_server_handle_get_visible_region( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_get_visible_region_request *request = (const void *)message;
    struct horizon_get_visible_region_reply reply;
    struct horizon_rectangle rect, top_client, top_surface;
    struct horizon_rectangle window_rect, client_rect;
    const void *data = NULL;
    unsigned int data_size = 0;
    struct horizon_user_window *window, *top, *top_parent;

    memset( &reply, 0, sizeof(reply) );
    memset( &rect, 0, sizeof(rect) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        top = horizon_server_get_surface_window_locked( window );
        horizon_server_window_screen_rects_locked( window, &window_rect, &client_rect );

        top_surface = top->surface_rect;
        if (top->parent && (top_parent = horizon_server_find_window_locked( top->parent )))
        {
            horizon_server_window_screen_rects_locked( top_parent, &rect, &top_client );
            horizon_server_offset_rect( &top_surface, top_client.left, top_client.top );
        }

        reply.top_win = top->handle;
        reply.top_rect = top_surface;
        reply.win_rect = (request->flags & HORIZON_DCX_WINDOW) ? window_rect : client_rect;
        reply.paint_flags = window->paint_flags;

        rect = horizon_server_intersect_rect( reply.win_rect, top_surface );
        if (!horizon_server_rect_empty( &rect ))
        {
            data = &rect;
            data_size = sizeof(rect);
            if (request->header.reply_size && data_size > request->header.reply_size)
            {
                reply.header.error = HORIZON_STATUS_BUFFER_OVERFLOW;
                data = NULL;
                data_size = 0;
            }
        }
        reply.total_size = horizon_server_rect_empty( &rect ) ? 0 : sizeof(rect);
        reply.header.reply_size = data_size;

        horizon_trace( "[HZPAINT] visible hwnd=%08x top=%08x flags=%x rect=%d,%d-%d,%d "
                       "toprect=%d,%d-%d,%d data=%u err=%08x\n",
                       request->window, reply.top_win, request->flags,
                       reply.win_rect.left, reply.win_rect.top,
                       reply.win_rect.right, reply.win_rect.bottom,
                       reply.top_rect.left, reply.top_rect.top,
                       reply.top_rect.right, reply.top_rect.bottom,
                       data_size, reply.header.error );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, data_size );
}

static int horizon_server_handle_redraw_window( struct horizon_server_connection *connection,
                                                const unsigned char *message,
                                                const unsigned char *data, unsigned int data_size )
{
    const struct horizon_redraw_window_request *request = (const void *)message;
    const struct horizon_rectangle *rects = (const void *)data;
    struct horizon_user_window *window;
    unsigned int count = data_size / sizeof(*rects);
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        if (request->flags & HORIZON_RDW_VALIDATE)
        {
            window->has_update_rect = 0;
            window->has_internal_paint = 0;
            window->needs_erase = 0;
            window->needs_nonclient = 0;
        }
        if (request->flags & HORIZON_RDW_NOINTERNALPAINT)
            window->has_internal_paint = 0;
        if (request->flags & HORIZON_RDW_INTERNALPAINT)
            window->has_internal_paint = 1;
        if (request->flags & HORIZON_RDW_ERASE)
            window->needs_erase = 1;

        if (request->flags & HORIZON_RDW_INVALIDATE)
        {
            struct horizon_rectangle win_screen, client_screen, dirty = {0};
            struct horizon_rectangle clip;
            unsigned int i;

            horizon_server_window_screen_rects_locked( window, &win_screen, &client_screen );
            clip = (request->flags & HORIZON_RDW_FRAME) ? win_screen : client_screen;
            if (!count)
                dirty = clip;
            else for (i = 0; i < count; i++)
            {
                struct horizon_rectangle rect = rects[i];

                if (horizon_server_rect_empty( &rect ))
                    rect = clip;
                else
                    horizon_server_offset_rect( &rect, win_screen.left, win_screen.top );
                rect = horizon_server_intersect_rect( rect, clip );
                dirty = horizon_server_union_rect( dirty, rect );
            }
            if (!horizon_server_rect_empty( &dirty ))
            {
                horizon_server_mark_window_update_locked( window, dirty,
                                                          !!(request->flags & HORIZON_RDW_ERASE) );
                if (request->flags & HORIZON_RDW_FRAME) window->needs_nonclient = 1;
                horizon_server_redraw_children_locked( window, dirty, request->flags );
            }
        }
        horizon_trace( "[HZPAINT] redraw hwnd=%08x flags=%x count=%u has=%u internal=%u erase=%u nc=%u rect=%d,%d-%d,%d err=%08x\n",
                       request->window, request->flags, count, window->has_update_rect,
                       window->has_internal_paint, window->needs_erase, window->needs_nonclient,
                       window->update_rect.left, window->update_rect.top,
                       window->update_rect.right, window->update_rect.bottom, status );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static struct horizon_window_property *horizon_server_find_window_property_locked(
    struct horizon_user_window *window, unsigned int atom )
{
    struct horizon_window_property *property;

    for (property = window->properties; property; property = property->next)
        if (property->atom == atom) return property;
    return NULL;
}

static unsigned int horizon_server_resolve_property_atom_locked( unsigned short request_atom,
                                                                 const unsigned char *name,
                                                                 unsigned int name_len, int add,
                                                                 unsigned int *atom )
{
    if (name_len)
    {
        if (add) return horizon_server_add_atom_locked( name, name_len, 0, atom );
        return horizon_server_find_atom_locked( name, name_len, atom );
    }
    *atom = request_atom;
    return HORIZON_STATUS_SUCCESS;
}

static int horizon_server_handle_set_window_property( struct horizon_server_connection *connection,
                                                       const unsigned char *message,
                                                       const unsigned char *data,
                                                       unsigned int data_size )
{
    const struct horizon_set_window_property_request *request = (const void *)message;
    struct horizon_window_property *property;
    struct horizon_user_window *window;
    unsigned int atom = 0, status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else if ((status = horizon_server_resolve_property_atom_locked( request->atom, data, data_size,
                                                                    1, &atom )))
        ;
    else if (!atom)
        status = HORIZON_STATUS_INVALID_PARAMETER;
    else if ((property = horizon_server_find_window_property_locked( window, atom )))
    {
        property->data = request->data;
        property->string = !!data_size;
    }
    else if (!(property = calloc( 1, sizeof(*property) )))
        status = HORIZON_STATUS_NO_MEMORY;
    else
    {
        property->atom = atom;
        property->string = !!data_size;
        property->data = request->data;
        property->next = window->properties;
        window->properties = property;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZPROP] set hwnd=%08x atom=%04x data=%016llx string=%u err=%08x\n",
                   request->window, atom, request->data, !!data_size, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_get_window_property( struct horizon_server_connection *connection,
                                                       const unsigned char *message,
                                                       const unsigned char *data,
                                                       unsigned int data_size, int remove )
{
    const struct horizon_window_property_request *request = (const void *)message;
    struct horizon_window_property **ptr, *property = NULL;
    struct horizon_window_property_reply reply;
    struct horizon_user_window *window;
    unsigned int atom = 0;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if ((reply.header.error = horizon_server_resolve_property_atom_locked(
                  request->atom, data, data_size, 0, &atom )))
        ;
    else if (atom)
    {
        for (ptr = &window->properties; *ptr; ptr = &(*ptr)->next)
        {
            if ((*ptr)->atom != atom) continue;
            property = *ptr;
            reply.data = property->data;
            if (remove)
            {
                *ptr = property->next;
                free( property );
            }
            break;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZPROP] %s hwnd=%08x atom=%04x data=%016llx err=%08x\n",
                   remove ? "remove" : "get", request->window, atom, reply.data,
                   reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_window_properties( struct horizon_server_connection *connection,
                                                         const unsigned char *message )
{
    const struct horizon_get_window_properties_request *request = (const void *)message;
    struct horizon_get_window_properties_reply reply;
    struct horizon_window_property *property;
    struct horizon_property_data *data = NULL;
    struct horizon_user_window *window;
    unsigned int count = 0, max_count, index = 0, data_size = 0;
    int status;

    memset( &reply, 0, sizeof(reply) );
    max_count = request->header.reply_size / sizeof(*data);

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        for (property = window->properties; property; property = property->next) count++;
        reply.total = count;
        if (count > max_count) count = max_count;
        if (count && !(data = calloc( count, sizeof(*data) )))
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else
        {
            for (property = window->properties; property && index < count; property = property->next)
            {
                data[index].atom = property->atom;
                data[index].string = property->string;
                data[index].data = property->data;
                index++;
            }
            data_size = index * sizeof(*data);
            reply.header.reply_size = data_size;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZPROP] list hwnd=%08x total=%d returned=%u err=%08x\n",
                   request->window, reply.total, index, reply.header.error );
    status = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, data_size );
    free( data );
    return status;
}

static int horizon_server_handle_get_update_region( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_get_update_region_request *request = (const void *)message;
    struct horizon_get_update_region_reply reply;
    struct horizon_rectangle rect;
    const void *data = NULL;
    unsigned int data_size = 0;
    struct horizon_user_window *window, *from_child = NULL, *target = NULL;
    unsigned int update_flags = 0;
    int past_from;

    memset( &reply, 0, sizeof(reply) );
    memset( &rect, 0, sizeof(rect) );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(window = horizon_server_find_window_locked( request->window )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->from_child &&
             !(from_child = horizon_server_find_window_locked( request->from_child )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        past_from = !from_child;
        update_flags = horizon_server_find_window_update_locked( window, from_child, request->flags,
                                                                 &past_from, &target );
        if (from_child && !past_from)
            reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
        else if (!target && !(request->flags & HORIZON_UPDATE_NOREGION) &&
                 !from_child && window->has_update_rect)
        {
            target = window;
            reply.child = window->handle;
            rect = window->update_rect;
            reply.total_size = sizeof(rect);
            if (!request->header.reply_size || request->header.reply_size >= sizeof(rect))
            {
                data = &rect;
                data_size = sizeof(rect);
                reply.header.reply_size = data_size;
            }
            else reply.header.error = HORIZON_STATUS_BUFFER_OVERFLOW;
        }
        else if (target)
        {
            reply.child = target->handle;
            reply.flags = update_flags;
            if (target->has_update_rect)
            {
                rect = target->update_rect;
                reply.total_size = sizeof(rect);
                if (!(request->flags & HORIZON_UPDATE_NOREGION))
                {
                    data = &rect;
                    data_size = sizeof(rect);
                }
            }
            reply.header.reply_size = data_size;
            if (!(request->flags & HORIZON_UPDATE_NOREGION))
            {
                if (update_flags & (HORIZON_UPDATE_PAINT | HORIZON_UPDATE_INTERNALPAINT))
                {
                    target->has_update_rect = 0;
                    target->has_internal_paint = 0;
                    target->needs_erase = 0;
                    target->needs_nonclient = 0;
                }
                else
                {
                    if (update_flags & HORIZON_UPDATE_ERASE) target->needs_erase = 0;
                    if (update_flags & HORIZON_UPDATE_NONCLIENT) target->needs_nonclient = 0;
                }
            }
        }
        horizon_trace( "[HZPAINT] get_update hwnd=%08x from=%08x req=%x child=%08x flags=%x size=%u has=%u internal=%u erase=%u nc=%u rect=%d,%d-%d,%d err=%08x\n",
                       request->window, request->from_child, request->flags, reply.child, reply.flags,
                       reply.total_size, target ? target->has_update_rect : window->has_update_rect,
                       target ? target->has_internal_paint : window->has_internal_paint,
                       target ? target->needs_erase : window->needs_erase,
                       target ? target->needs_nonclient : window->needs_nonclient,
                       rect.left, rect.top, rect.right, rect.bottom,
                       reply.header.error );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, data_size );
}

static void horizon_server_message_defaults_locked( int *x, int *y, unsigned int *time )
{
    struct horizon_obj_locator desktop_locator;
    struct horizon_desktop_shm *desktop;

    *time = horizon_server_input_time();
    if (!(desktop = horizon_server_desktop_shared_locked( &desktop_locator ))) return;
    *x = desktop->cursor.x;
    *y = desktop->cursor.y;
}

/* WM_CLIPBOARDUPDATE to every listener after the clipboard changed. */
static void horizon_server_clipboard_notify_locked(void)
{
    unsigned int i;

    for (i = 0; i < horizon_clipboard.listen_count; i++)
    {
        struct horizon_user_window *window = horizon_server_find_window_locked( horizon_clipboard.listeners[i] );
        struct horizon_posted_message posted;
        struct horizon_msgq *queue;

        if (!window || !window->tid || !(queue = horizon_server_queue_locked( window->tid ))) continue;
        memset( &posted, 0, sizeof(posted) );
        posted.tid = window->tid;
        posted.win = window->handle;
        posted.msg = 0x031d;  /* WM_CLIPBOARDUPDATE */
        horizon_server_message_defaults_locked( &posted.x, &posted.y, &posted.time );
        if (!horizon_message_queue_post( &horizon_posted_messages, &posted ))
            horizon_msgq_touch( queue, HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_ALLPOSTMESSAGE );
    }
    horizon_server_refresh_queues_locked();
}

/* RegisterWindowMessage and RegisterClipboardFormat. The process has one atom
 * table; user atoms share it with classes and global atoms. */
static int horizon_server_handle_add_user_atom( struct horizon_server_connection *connection,
                                                const unsigned char *data, unsigned int data_size )
{
    struct horizon_atom_reply reply;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_add_atom_locked( data, data_size, 0, &reply.atom );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_user_atom_name( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    const struct horizon_get_user_atom_name_request *request = (const void *)message;
    struct horizon_get_user_atom_name_reply reply;
    struct horizon_atom_entry *entry;
    unsigned char name[HORIZON_MAX_ATOM_LEN * sizeof(unsigned short)];
    unsigned int size = 0;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (entry = horizon_atoms; entry; entry = entry->next) if (entry->atom == request->atom) break;
    if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.total = entry->name_len;
        size = entry->name_len < request->header.reply_size ? entry->name_len : request->header.reply_size;
        if (size > sizeof(name)) size = sizeof(name);
        memcpy( name, entry->name, size );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.header.reply_size = size;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), name, size );
}

static int horizon_server_handle_clipboard( struct horizon_server_connection *connection, int req,
                                            const unsigned char *message, const unsigned char *data,
                                            unsigned int data_size )
{
    const struct horizon_clipboard_window_request *window_request = (const void *)message;
    struct horizon_clipboard_pair_reply reply;
    struct horizon_clip_data_reply clip_data;
    struct horizon_clipboard *clip = &horizon_clipboard;
    unsigned int status = HORIZON_STATUS_SUCCESS, *formats = NULL, count = 0, max, notify = 0;
    const void *reply_data = NULL;
    unsigned int reply_data_size = 0;
    unsigned char *copy = NULL;
    int ret;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    switch (req)
    {
    case HORIZON_REQ_OPEN_CLIPBOARD:
        if (window_request->window && !horizon_server_find_window_locked( window_request->window ))
            status = 0xc0010578u;  /* ERROR_INVALID_WINDOW_HANDLE */
        else status = horizon_clip_open( clip, connection->tid, window_request->window, &reply.first );
        break;
    case HORIZON_REQ_CLOSE_CLIPBOARD:
        if (clip->open_tid != connection->tid) status = HORIZON_CLIP_STATUS_NOT_OPEN;
        else notify = horizon_clip_close_locked( clip, &reply.first );
        reply.second = clip->owner;
        break;
    case HORIZON_REQ_EMPTY_CLIPBOARD:
        status = horizon_clip_empty( clip, connection->tid );
        break;
    case HORIZON_REQ_SET_CLIPBOARD_DATA:
    {
        const struct horizon_set_clipboard_data_request *request = (const void *)message;
        status = horizon_clip_set_data( clip, connection->tid, request->format, request->lcid, data, data_size,
                                        &reply.first );
        break;
    }
    case HORIZON_REQ_GET_CLIPBOARD_DATA:
    {
        const struct horizon_get_clipboard_data_request *request = (const void *)message;
        struct horizon_get_clipboard_data_reply data_reply;

        status = horizon_clip_get_data( clip, connection->tid, request->format, request->render, request->cached,
                                        request->seqno, request->header.reply_size, &clip_data );
        if (clip_data.data && (copy = malloc( clip_data.total )))
        {
            memcpy( copy, clip_data.data, clip_data.total );
            reply_data = copy;
            reply_data_size = clip_data.total;
        }
        else if (clip_data.data) status = HORIZON_STATUS_NO_MEMORY;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        memset( &data_reply, 0, sizeof(data_reply) );
        data_reply.header.error = status;
        data_reply.from = clip_data.from;
        data_reply.owner = clip_data.owner;
        data_reply.seqno = clip_data.seqno;
        data_reply.total = clip_data.total;
        data_reply.header.reply_size = reply_data_size;
        horizon_trace( "[HZCLIP] get_data tid=%04x format=%x render=%d total=%u from=%x err=%08x\n",
                       connection->tid, request->format, request->render, clip_data.total, clip_data.from, status );
        ret = horizon_server_write_reply( connection->reply_fd, &data_reply, sizeof(data_reply),
                                          reply_data, reply_data_size );
        free( copy );
        return ret;
    }
    case HORIZON_REQ_GET_CLIPBOARD_FORMATS:
    {
        const struct horizon_clipboard_format_request *request = (const void *)message;

        max = request->header.reply_size / sizeof(*formats);
        if (!request->format && max && !(formats = malloc( max * sizeof(*formats) ))) status = HORIZON_STATUS_NO_MEMORY;
        else status = horizon_clip_formats( clip, request->format, formats, max, &count );
        reply.first = count;
        if (!status && !request->format)
        {
            reply_data = formats;
            reply_data_size = count * sizeof(*formats);
        }
        break;
    }
    case HORIZON_REQ_ENUM_CLIPBOARD_FORMATS:
    {
        const struct horizon_clipboard_format_request *request = (const void *)message;
        status = horizon_clip_enum( clip, connection->tid, request->format, &reply.first );
        break;
    }
    case HORIZON_REQ_RELEASE_CLIPBOARD:
        if (!horizon_server_find_window_locked( window_request->window )) status = 0xc0010578u;
        else if (clip->owner != window_request->window) status = HORIZON_CLIP_STATUS_INVALID_OWNER;
        else
        {
            notify = horizon_clip_release_locked( clip, &reply.first );
            reply.second = clip->owner;
        }
        break;
    case HORIZON_REQ_GET_CLIPBOARD_INFO:
    {
        struct horizon_get_clipboard_info_reply info;

        memset( &info, 0, sizeof(info) );
        info.window = clip->open_win;
        info.owner = clip->owner;
        info.viewer = clip->viewer;
        info.seqno = clip->seqno;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        return horizon_server_write_reply( connection->reply_fd, &info, sizeof(info), NULL, 0 );
    }
    case HORIZON_REQ_SET_CLIPBOARD_VIEWER:
    {
        const struct horizon_set_clipboard_viewer_request *request = (const void *)message;

        if ((request->viewer && !horizon_server_find_window_locked( request->viewer )) ||
            (request->previous && !horizon_server_find_window_locked( request->previous )))
            status = 0xc0010578u;
        else status = horizon_clip_set_viewer( clip, request->viewer, request->previous, &reply.first, &reply.second );
        break;
    }
    case HORIZON_REQ_ADD_CLIPBOARD_LISTENER:
    case HORIZON_REQ_REMOVE_CLIPBOARD_LISTENER:
        if (!horizon_server_find_window_locked( window_request->window )) status = 0xc0010578u;
        else if (req == HORIZON_REQ_ADD_CLIPBOARD_LISTENER)
            status = horizon_clip_add_listener( clip, window_request->window );
        else if (!horizon_clip_remove_listener( clip, window_request->window ))
            status = HORIZON_CLIP_STATUS_INVALID_PARAMETER;
        break;
    }
    if (notify) horizon_server_clipboard_notify_locked();
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZCLIP] req=%d tid=%04x values=%x,%x size=%u err=%08x\n",
                   req, connection->tid, reply.first, reply.second, data_size, status );
    reply.header.error = status;
    reply.header.reply_size = reply_data_size;
    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), reply_data, reply_data_size );
    free( formats );
    return ret;
}

/* PostMessage and PostThreadMessage, and messages sent to another thread's
 * window (SendMessage, SendMessageTimeout, SendNotifyMessage,
 * SendMessageCallback). A thread sending to its own windows never gets here. */
static int horizon_server_handle_send_message( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_send_message_request *request = (const void *)message;
    struct horizon_posted_message posted;
    struct horizon_server_object *thread;
    struct horizon_msgq *sender, *receiver = NULL;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    memset( &posted, 0, sizeof(posted) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (thread = horizon_server_threads; thread; thread = thread->thread_next)
        if (thread->thread.tid == request->id && !thread->thread.terminated) break;
    if (!thread) status = HORIZON_STATUS_INVALID_CID;
    else if (!(receiver = horizon_server_queue_locked( request->id )) ||
             !(sender = horizon_server_queue_locked( connection->tid )))
        status = HORIZON_STATUS_NO_MEMORY;
    else if (request->type == HORIZON_MSG_POSTED)
    {
        posted.tid = request->id;
        posted.win = request->win;
        posted.msg = request->msg;
        posted.wparam = request->wparam;
        posted.lparam = request->lparam;
        horizon_server_message_defaults_locked( &posted.x, &posted.y, &posted.time );
        if (horizon_message_queue_post( &horizon_posted_messages, &posted )) status = HORIZON_STATUS_NO_MEMORY;
        else horizon_msgq_touch( receiver, HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_ALLPOSTMESSAGE );
    }
    else
    {
        struct horizon_msgq_sent fields;
        unsigned long long now = horizon_server_timer_clock();
        int has_deadline = request->timeout != 0x7fffffffffffffffLL;

        memset( &fields, 0, sizeof(fields) );
        fields.type = request->type;
        fields.win = request->win;
        fields.msg = request->msg;
        fields.wparam = request->wparam;
        fields.lparam = request->lparam;
        horizon_server_message_defaults_locked( &fields.x, &fields.y, &fields.time );
        /* Timeouts are relative (negative, 100 ns) from win32u. */
        status = horizon_msgq_send( sender, receiver, &fields, data, data_size, has_deadline,
                                    now + (request->timeout < 0 ? (unsigned long long)-request->timeout / 10000 : 0) );
    }
    if (receiver) horizon_server_refresh_queues_locked();
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZMSG] %s type=%d from=%04x to=%04x hwnd=%08x msg=%x wp=%llx lp=%llx size=%u err=%08x\n",
                   request->type == HORIZON_MSG_POSTED ? "post" : "send", request->type, connection->tid,
                   request->id, request->win, request->msg, request->wparam, request->lparam, data_size, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_post_quit_message( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_post_quit_message_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_SUCCESS;
    struct horizon_msgq *queue;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_server_queue_locked( connection->tid ))) status = HORIZON_STATUS_NO_MEMORY;
    else
    {
        horizon_msgq_post_quit( queue, request->exit_code );
        horizon_server_refresh_queue_locked( queue, horizon_server_timer_clock() );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZMSG] post_quit tid=%04x exit_code=%d err=%08x\n", connection->tid, request->exit_code, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_get_msg_queue( struct horizon_server_connection *connection )
{
    struct horizon_get_msg_queue_reply reply;
    struct horizon_msgq *queue;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_server_queue_locked( connection->tid ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
    else
    {
        reply.locator_id = queue->shm_id;
        reply.locator_offset = queue->shm_offset;
        horizon_server_refresh_queue_locked( queue, horizon_server_timer_clock() );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* The handle win32u waits on (MsgWaitForMultipleObjects, WaitMessage and
 * waiting for a SendMessage reply); it is signaled while the queue's wake
 * bits match the masks from get_message or set_queue_mask. */
static int horizon_server_handle_get_msg_queue_handle( struct horizon_server_connection *connection )
{
    struct horizon_get_msg_queue_handle_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_msgq *queue;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_server_queue_locked( connection->tid )) ||
        !(entry = horizon_server_create_handle_for_object_locked( queue->sync )))
        reply.header.error = HORIZON_STATUS_NO_MEMORY;
    else reply.handle = entry->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_queue_mask( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_set_queue_mask_request *request = (const void *)message;
    struct horizon_queue_bits_reply reply;
    struct horizon_msgq *queue;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_server_queue_locked( connection->tid ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
    else
    {
        queue->wake_mask = request->wake_mask;
        queue->changed_mask = request->changed_mask;
        horizon_server_refresh_queues_locked();
        reply.wake_bits = queue->wake_bits;
        reply.changed_bits = queue->changed_bits;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_queue_status( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_get_queue_status_request *request = (const void *)message;
    struct horizon_queue_bits_reply reply;
    struct horizon_msgq *queue;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((queue = horizon_server_queue_locked( connection->tid )))
    {
        horizon_server_refresh_queues_locked();
        reply.wake_bits = queue->wake_bits;
        reply.changed_bits = queue->changed_bits;
        queue->changed_bits &= ~request->clear_bits;
        horizon_server_refresh_queue_locked( queue, horizon_server_timer_clock() );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* The receiving thread's window procedure returned (remove) or ReplyMessage. */
static int horizon_server_handle_reply_message( struct horizon_server_connection *connection,
                                                const unsigned char *message,
                                                const unsigned char *data, unsigned int data_size )
{
    const struct horizon_reply_message_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_SUCCESS;
    struct horizon_msgq *queue;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_msgq_find( &horizon_msg_queues, connection->tid ))) status = HORIZON_STATUS_ACCESS_DENIED;
    else
    {
        horizon_msgq_reply( queue, request->result, 0, request->remove, data, data_size );
        horizon_server_refresh_queues_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZMSG] reply tid=%04x result=%llx remove=%d size=%u err=%08x\n",
                   connection->tid, request->result, request->remove, data_size, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

/* The sending thread collects its result, or gives up on it (cancel). */
static int horizon_server_handle_get_message_reply( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_get_message_reply_request *request = (const void *)message;
    struct horizon_get_message_reply_reply reply;
    struct horizon_msgq *queue;
    unsigned char *data = NULL;
    unsigned int data_size = 0;
    int ret;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(queue = horizon_msgq_find( &horizon_msg_queues, connection->tid )))
        reply.header.error = HORIZON_STATUS_ACCESS_DENIED;
    else
    {
        reply.header.error = horizon_msgq_get_reply( queue, request->cancel, &reply.result, &data, &data_size );
        horizon_server_refresh_queues_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (data_size > request->header.reply_size) data_size = request->header.reply_size;
    reply.header.reply_size = data_size;
    horizon_trace( "[HZMSG] get_reply tid=%04x result=%llx size=%u err=%08x\n",
                   connection->tid, reply.result, data_size, reply.header.error );
    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, data_size );
    free( data );
    return ret;
}

/* SetTimer and SetSystemTimer. A window timer belongs to that window's thread. */
static int horizon_server_handle_set_win_timer( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct horizon_set_win_timer_request *request = (const void *)message;
    struct horizon_set_win_timer_reply reply;
    struct horizon_user_window *window = NULL;
    unsigned int tid = connection->tid;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->win && (!(window = horizon_server_find_window_locked( request->win )) || !window->tid))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        if (window) tid = window->tid;
        switch (horizon_win_timers_set( &horizon_timers, tid, request->win, request->msg, request->id,
                                        request->rate, request->lparam, horizon_server_timer_clock(),
                                        &reply.id ))
        {
        case 0: break;
        case HORIZON_WIN_TIMERS_NO_IDS: reply.header.error = 0xc0010486u; break; /* ERROR_NO_MORE_USER_HANDLES */
        default: reply.header.error = HORIZON_STATUS_NO_MEMORY; break;
        }
        horizon_server_refresh_queues_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZMSG] set_timer tid=%04x hwnd=%08x msg=%x id=%llx rate=%u -> id=%llx err=%08x\n",
                   tid, request->win, request->msg, request->id, request->rate, reply.id, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_kill_win_timer( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_kill_win_timer_request *request = (const void *)message;
    struct horizon_user_window *window = NULL;
    unsigned int status = HORIZON_STATUS_SUCCESS, tid = connection->tid;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (request->win && (!(window = horizon_server_find_window_locked( request->win )) || !window->tid))
        status = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        if (window) tid = window->tid;
        if (horizon_win_timers_kill( &horizon_timers, tid, request->win, request->msg, request->id ))
            status = HORIZON_STATUS_INVALID_PARAMETER;
        horizon_server_refresh_queues_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    horizon_trace( "[HZMSG] kill_timer tid=%04x hwnd=%08x msg=%x id=%llx err=%08x\n",
                   tid, request->win, request->msg, request->id, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_posted_window_is_descendant( void *ctx, unsigned int child, unsigned int ancestor )
{
    (void)ctx;
    return horizon_server_window_is_descendant_locked( horizon_server_find_window_locked( child ), ancestor );
}

static int horizon_server_handle_get_message( struct horizon_server_connection *connection,
                                              const unsigned char *message )
{
    const struct horizon_get_message_request *request = (const void *)message;
    struct horizon_get_message_reply reply;
    struct horizon_hardware_msg_data hardware;
    unsigned char hardware_raw[sizeof(struct horizon_hardware_msg_data) +
                               (sizeof(struct horizon_raw_mouse) > sizeof(struct horizon_raw_keyboard)
                                ? sizeof(struct horizon_raw_mouse) : sizeof(struct horizon_raw_keyboard))];
    struct horizon_input_message *queued;
    struct horizon_posted_message **posted = NULL;
    struct horizon_win_timer *timer = NULL;
    struct horizon_msgq_sent *sent = NULL;
    struct horizon_msgq *queue = NULL;
    struct horizon_user_window *window, *parent;
    const void *reply_data = NULL;
    unsigned int reply_data_size = 0, filter = request->flags >> 16;
    int paint_requested, found = 0, ret;

    memset( &reply, 0, sizeof(reply) );
    memset( &hardware, 0, sizeof(hardware) );
    paint_requested = (!request->get_first && !request->get_last) ||
                      (request->get_first <= HORIZON_WM_PAINT &&
                       request->get_last >= HORIZON_WM_PAINT);
    if (!filter) filter = HORIZON_QS_ALLINPUT;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!request->internal) queue = horizon_server_queue_locked( connection->tid );
    /* As in server/queue.c: messages sent by other threads first, whatever the
     * filter; then posted messages, WM_QUIT once none match, hardware input,
     * paints and timers. */
    if (queue && queue->sent)
    {
        reply.type = queue->sent->type;
        reply.win = queue->sent->win;
        reply.msg = queue->sent->msg;
        reply.wparam = queue->sent->wparam;
        reply.lparam = queue->sent->lparam;
        reply.x = queue->sent->x;
        reply.y = queue->sent->y;
        reply.time = queue->sent->time;
        reply.total = queue->sent->data_size;
        if (queue->sent->data_size > request->header.reply_size)
            reply.header.error = HORIZON_STATUS_BUFFER_OVERFLOW;
        else
        {
            sent = horizon_msgq_receive( queue );
            reply.header.reply_size = sent->data_size;
            horizon_server_refresh_queues_locked();
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        horizon_trace( "[HZMSG] receive tid=%04x type=%d hwnd=%08x msg=%x size=%u err=%08x\n",
                       connection->tid, reply.type, reply.win, reply.msg, reply.total, reply.header.error );
        ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                          sent ? sent->data : NULL, sent ? sent->data_size : 0 );
        if (sent)
        {
            free( sent->data );
            free( sent );
        }
        return ret;
    }
    if (queue) horizon_msgq_begin_get( queue, filter, request->get_first, request->get_last );
    if (!request->internal && (filter & HORIZON_QS_POSTMESSAGE))
    {
        if ((posted = horizon_message_queue_find( &horizon_posted_messages, connection->tid, request->get_win,
                                                  request->get_first, request->get_last,
                                                  horizon_server_posted_window_is_descendant, NULL )))
        {
            reply.win = (*posted)->win;
            reply.msg = (*posted)->msg;
            reply.wparam = (*posted)->wparam;
            reply.lparam = (*posted)->lparam;
            reply.x = (*posted)->x;
            reply.y = (*posted)->y;
            reply.time = (*posted)->time;
            reply.type = HORIZON_MSG_POSTED;
            if (request->flags & HORIZON_PM_REMOVE)
                horizon_message_queue_remove( &horizon_posted_messages, posted );
            found = 1;
        }
        else if (queue && queue->quit_message)
        {
            reply.msg = HORIZON_MESSAGE_QUEUE_WM_QUIT;
            reply.wparam = (long long)queue->exit_code;
            reply.type = HORIZON_MSG_POSTED;
            horizon_server_message_defaults_locked( &reply.x, &reply.y, &reply.time );
            if (request->flags & HORIZON_PM_REMOVE) queue->quit_message = 0;
            found = 1;
        }
    }
    if (!found && !request->internal)
    {
        for (queued = horizon_input_messages; queued; queued = queued->next)
        {
            if (queued->tid != connection->tid) continue;
            if (request->hw_id && queued->id == request->hw_id) continue;
            if (!horizon_server_mouse_message_matches( request, queued )) continue;
            if (request->get_win)
            {
                window = horizon_server_find_window_locked( queued->win );
                if (!window || !horizon_server_window_is_descendant_locked( window, request->get_win ))
                    continue;
            }

            reply.win = queued->win;
            reply.msg = queued->msg;
            reply.wparam = queued->wparam;
            reply.lparam = queued->lparam;
            reply.type = HORIZON_MSG_HARDWARE;
            reply.x = queued->x;
            reply.y = queued->y;
            reply.time = queued->time;
            hardware.info = queued->info;
            hardware.hw_id = queued->id;
            hardware.flags = queued->data_flags;
            hardware.source.device = queued->device ? queued->device : HORIZON_IMDT_MOUSE;
            hardware.source.origin = HORIZON_IMO_HARDWARE;
            hardware.size = sizeof(hardware);
            if (queued->raw_keyboard)
            {
                /* WM_INPUT: RAWKEYBOARD follows, where NtUserGetRawInputData reads it */
                hardware.size += sizeof(queued->raw);
                hardware.rawinput.type = HORIZON_RIM_TYPEKEYBOARD;
                hardware.rawinput.device = HORIZON_WINE_KEYBOARD_HANDLE;
                hardware.rawinput.usage = HORIZON_RAWINPUT_USAGE_KEYBOARD;
                memcpy( hardware_raw + sizeof(hardware), &queued->raw, sizeof(queued->raw) );
            }
            else if (queued->raw_mouse)
            {
                /* And RAWMOUSE for one, the same way. */
                hardware.size += sizeof(queued->raw_m);
                hardware.rawinput.type = HORIZON_RIM_TYPEMOUSE;
                hardware.rawinput.device = HORIZON_WINE_MOUSE_HANDLE;
                hardware.rawinput.usage = HORIZON_RAWINPUT_USAGE_MOUSE;
                memcpy( hardware_raw + sizeof(hardware), &queued->raw_m, sizeof(queued->raw_m) );
            }
            memcpy( hardware_raw, &hardware, sizeof(hardware) );
            reply.total = hardware.size;
            reply.header.reply_size = hardware.size;
            reply_data = hardware_raw;
            reply_data_size = hardware.size;
            /* No accept_hardware_message follows raw input: PM_REMOVE takes it, as in server/queue.c. */
            if ((queued->raw_keyboard || queued->raw_mouse) && (request->flags & HORIZON_PM_REMOVE))
                horizon_server_remove_input_message_locked( queued );
            found = 1;
            break;
        }
    }
    if (!found && paint_requested && !request->internal)
    {
        for (window = horizon_windows; window; window = window->next)
        {
            if (window->tid != connection->tid || !(window->style & HORIZON_WS_VISIBLE)) continue;
            if (!window->has_update_rect && !window->has_internal_paint) continue;
            if (request->get_win)
            {
                for (parent = window; parent && parent->handle != request->get_win; )
                    parent = parent->parent ? horizon_server_find_window_locked( parent->parent ) : NULL;
                if (!parent) continue;
            }
            reply.win = window->handle;
            reply.msg = HORIZON_WM_PAINT;
            reply.type = HORIZON_MSG_POSTED;
            found = 1;
            break;
        }
    }
    if (!found && !request->internal && (filter & HORIZON_QS_TIMER) &&
        (timer = horizon_win_timers_expired( &horizon_timers, connection->tid, request->get_win,
                                             request->get_first, request->get_last,
                                             horizon_server_timer_clock(),
                                             request->flags & HORIZON_PM_REMOVE )))
    {
        reply.win = timer->win;
        reply.msg = timer->msg;
        reply.wparam = timer->id;
        reply.lparam = timer->lparam;
        reply.type = HORIZON_MSG_POSTED;
        horizon_server_message_defaults_locked( &reply.x, &reply.y, &reply.time );
        found = 1;
    }
    if (queue)
    {
        /* Nothing matched: a wait on the queue handle uses these masks. */
        if (!found)
        {
            queue->wake_mask = request->wake_mask;
            queue->changed_mask = request->changed_mask;
        }
        horizon_server_refresh_queues_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (!found) return horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_PENDING );
    if (reply.type == HORIZON_MSG_HARDWARE)
    {
        horizon_trace( "[HZINPUT] get_message id=%u hwnd=%08x msg=%x x=%d y=%d\n",
                       hardware.hw_id, reply.win, reply.msg, reply.x, reply.y );
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                           reply_data, reply_data_size );
    }
    if (posted || timer || reply.msg == HORIZON_MESSAGE_QUEUE_WM_QUIT)
        horizon_trace( "[HZMSG] get tid=%04x hwnd=%08x msg=%x wp=%llx flags=%x range=%x-%x\n",
                       connection->tid, reply.win, reply.msg, reply.wparam, request->flags,
                       request->get_first, request->get_last );
    else
        horizon_trace( "[HZPAINT] get_message WM_PAINT hwnd=%08x flags=%x range=%x-%x\n",
                       reply.win, request->flags, request->get_first, request->get_last );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_update_window_zorder( struct horizon_server_connection *connection,
                                                       const unsigned char *message )
{
    const struct horizon_update_window_zorder_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_window_locked( request->window ))
        status = HORIZON_STATUS_INVALID_HANDLE;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_get_desktop_window( struct horizon_server_connection *connection,
                                                     const unsigned char *message )
{
    struct horizon_get_desktop_window_reply reply;
    struct horizon_server_object *desktop;

    (void)message;
    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_thread_desktop ||
        !(desktop = horizon_server_find_handle_object_locked( horizon_thread_desktop,
                                                              HORIZON_SERVER_OBJECT_DESKTOP )))
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        if (!desktop->desktop_top_window)
        {
            struct horizon_user_window *window;
            /* These windows are created by the server without a client WND.
             * Do not label them as local to the requesting process: win32u
             * must resolve them as WND_DESKTOP, not a NULL client_objects slot. */
            horizon_server_create_window_locked( 0, 0, HORIZON_DESKTOP_ATOM, 0, 0,
                                                 HORIZON_NTUSER_DPI_PER_MONITOR_AWARE,
                                                 0, 0, 0, 0, &window );
        }
        if (!desktop->desktop_msg_window)
        {
            struct horizon_atom_entry *message_atom;
            struct horizon_user_window *window;

            if ((message_atom = horizon_server_find_atom_name_locked(
                     (const unsigned char *)"M\0e\0s\0s\0a\0g\0e\0",
                     7 * sizeof(unsigned short) )))
                horizon_server_create_window_locked( 0, 0, message_atom->atom, 0, 0,
                                                     HORIZON_NTUSER_DPI_PER_MONITOR_AWARE,
                                                     0, 0, 0, 0, &window );
        }
        reply.top_window = desktop->desktop_top_window;
        reply.msg_window = desktop->desktop_msg_window;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZUSER] get_desktop_window pid=%u tid=%u -> top=%08x msg=%08x err=%08x\n",
                   connection->pid, connection->tid, reply.top_window, reply.msg_window,
                   reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* A program that cannot open one of its own files usually says so in its own
 * words -- Halo reports that one of its files is missing or corrupted -- and
 * never says which, so the log names the ones it asked for and did not get,
 * with how it asked and what Horizon's open() said: The Sims 2 opens each of
 * its packages to read, then to write only to see whether it may, and
 * Horizon refused one of those as an invalid argument.
 * The loader probes for a DLL in every directory of its search path, so the
 * ones below the Wine tree are its search and not a program's own file. */
static void horizon_report_missing_file( const char *path, unsigned int status,
                                         const struct horizon_create_file_request *request, int error )
{
    static LONG reported;
    char message[448];

    if (strstr( path, "/windows/" ) || strstr( path, "/Windows/" )) return;
    if (__atomic_add_fetch( &reported, 1, __ATOMIC_RELAXED ) > 64) return;
    snprintf( message, sizeof(message), "[FS] %s could not be opened: status %08x "
              "(access %08x, sharing %x, disposition %d, options %08x, errno %d)",
              path, status, request->access, request->sharing, request->create, request->options, error );
    wine_nx_runtime_trace( message );
}

static int horizon_server_handle_create_file( struct horizon_server_connection *connection,
                                              const unsigned char *message,
                                              const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_file_request *request = (const void *)message;
    struct horizon_create_file_reply reply;
    struct horizon_server_handle_entry *entry = NULL;
    char *filename = NULL;
    char *stored_name = NULL;
    unsigned int attr_size;
    unsigned int filename_size;
    int flags;
    int is_dir = 0;
    int fd = -1;
    int open_error = 0;

    memset( &reply, 0, sizeof(reply) );
    /* DELETE_ON_CLOSE requires DELETE access, not merely a writable fd. */
    if ((request->options & 0x00001000u) &&
        !(horizon_file_map_access( request->access ) & 0x00010000u))
        reply.header.error = HORIZON_STATUS_ACCESS_DENIED;
    else reply.header.error = horizon_server_object_attributes_size( data, data_size, &attr_size );
    if (!reply.header.error)
    {
        filename_size = data_size - attr_size;
        if (!filename_size) reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
        else if (!(filename = malloc( filename_size + 1 ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else
        {
            memcpy( filename, data + attr_size, filename_size );
            filename[filename_size] = 0;
            reply.header.error = horizon_server_file_open_flags( request, &flags );
            is_dir = !!(request->options & HORIZON_FILE_DIRECTORY_FILE);
            if (is_dir && (request->options & HORIZON_FILE_NON_DIRECTORY_FILE))
                reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
        }
    }

    if (!reply.header.error && is_dir)
    {
        DIR *dir;

        /* CreateDirectoryW: create first, as wineserver's open_fd does. An
         * existing directory is fine unless the disposition was FILE_CREATE. */
        if ((flags & O_CREAT) && mkdir( filename, 0777 ) == -1 && (errno != EEXIST || (flags & O_EXCL)))
            reply.header.error = horizon_server_errno_status( errno );
        else if (!(dir = opendir( filename ))) reply.header.error = horizon_server_errno_status( errno );
        else
        {
            closedir( dir );
            if (!(stored_name = strdup( filename ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
        }
    }
    else if (!reply.header.error)
    {
        /* Windows refuses what the handles already open on the file do not
         * share, before the file system is asked. */
        unsigned int existing_access = 0, existing_sharing = 7;
        const struct horizon_server_handle_entry *other;

        pthread_mutex_lock( &horizon_server_objects_mutex );
        for (other = horizon_server_handles; other; other = other->next)
        {
            const struct horizon_server_object *object = other->object;

            if (object->type != HORIZON_SERVER_OBJECT_FILE || !object->file_shared || object->file_fd == -1 ||
                !object->file_name || !horizon_unix_path_equal( object->file_name, filename ))
                continue;
            existing_access |= object->file_access;
            existing_sharing &= object->file_sharing;
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        if (horizon_file_sharing_violation( existing_access, existing_sharing,
                                            horizon_file_map_access( request->access ), request->sharing ))
            reply.header.error = HORIZON_STATUS_SHARING_VIOLATION;
    }
    if (!reply.header.error && !is_dir)
    {
        fd = open( filename, flags, 0666 );
        if (fd == -1)
        {
            int open_errno = errno;
            DIR *dir;

            open_error = open_errno;
            /* libnx cannot open() a directory, but NT opens an existing one
             * without FILE_DIRECTORY_FILE (CreateFileW with backup semantics). */
            if (open_errno != EEXIST && (dir = opendir( filename )))
            {
                closedir( dir );
                if (!(reply.header.error = horizon_directory_open_status( request->options, flags )))
                {
                    is_dir = 1;
                    if (!(stored_name = strdup( filename ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
                }
            }
            /* The SD card refuses to open a file for writing while it is
             * open (FS result 0xe02, EIO in libnx), where Windows might share. */
            else if (open_errno == EIO && fsdevGetLastResult() == 0xe02)
                reply.header.error = HORIZON_STATUS_SHARING_VIOLATION;
            else reply.header.error = horizon_server_errno_status( open_errno );
        }
        else if (!(stored_name = strdup( filename ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
    }

    if (!reply.header.error)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        if ((entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_FILE )))
        {
            entry->object->file_fd = fd;
            entry->object->file_name = stored_name;
            entry->object->file_access = horizon_file_map_access( request->access );
            entry->object->file_options = request->options;
            entry->object->file_is_dir = is_dir;
            entry->object->file_sharing = request->sharing;
            entry->object->file_shared = !is_dir;
            reply.handle = entry->handle;
            stored_name = NULL;
            fd = -1;
        }
        else reply.header.error = HORIZON_STATUS_NO_MEMORY;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }

    if (fd != -1) close( fd );
    if (reply.header.error && filename)
        horizon_report_missing_file( filename, reply.header.error, request, open_error );
    if (is_dir)
        horizon_trace( "[HZDIR] create path=%s handle=%08x err=%08x\n",
                       filename ? filename : "<null>", reply.handle, reply.header.error );
    free( stored_name );
    free( filename );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* SetEndOfFile: extractors pre-size their output files this way. libnx maps
 * ftruncate to fsFileSetSize, which grows as well as shrinks, so wineserver's
 * write-a-byte-then-truncate growth path is not needed. */
static int horizon_server_handle_set_fd_eof_info( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_set_fd_eof_info_request *request = (const void *)message;
    struct horizon_server_handle_entry *entry;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->handle );
    if (!entry) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_FILE) status = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else if (entry->object->file_is_dir) status = HORIZON_STATUS_FILE_IS_A_DIRECTORY;
    else if (entry->object->file_fd == -1) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (request->eof > 0x7fffffffffffffffull) status = HORIZON_STATUS_INVALID_PARAMETER;
    else if (ftruncate( entry->object->file_fd, (off_t)request->eof ) == -1)
        status = horizon_server_errno_status( errno );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

/* Only an empty directory can be marked for deletion. */
static int horizon_server_dir_is_empty( const char *path )
{
    struct dirent *de;
    DIR *dir;
    int empty = 1;

    if (!(dir = opendir( path ))) return -1;
    while ((de = readdir( dir )))
    {
        if (strcmp( de->d_name, "." ) && strcmp( de->d_name, ".." ))
        {
            empty = 0;
            break;
        }
    }
    closedir( dir );
    return empty;
}

/* FileDispositionInformation(Ex), e.g. SetFileInformationByHandle(FileDispositionInfo):
 * the file is removed when the object's last handle closes, like DELETE_ON_CLOSE. */
static int horizon_server_handle_set_fd_disp_info( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_set_fd_disp_info_request *request = (const void *)message;
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object = NULL;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(entry = horizon_server_find_handle_locked( request->handle ))) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_FILE || !entry->object->file_name)
        status = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!((object = entry->object)->file_access & 0x00010000u)) status = HORIZON_STATUS_ACCESS_DENIED;
    else if ((request->flags & 0x00000001u) && object->file_is_dir)
    {
        switch (horizon_server_dir_is_empty( object->file_name ))
        {
        case -1: status = horizon_server_errno_status( errno ); break;
        case 0: status = HORIZON_STATUS_DIRECTORY_NOT_EMPTY; break;
        }
    }
    if (!status) object->file_delete = horizon_disposition_update( &object->file_options, request->flags );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

/* The SD card refuses to rename a file that is open (FS result 0xe02, EIO in libnx). */
static unsigned int horizon_server_rename_errno_status( int error )
{
    if (error == EIO && fsdevGetLastResult() == 0xe02) return HORIZON_STATUS_SHARING_VIOLATION;
    return horizon_server_errno_status( error );
}

/* FileRenameInformation: MoveFileExW, and 7-Zip replacing an archive with its
 * temporary copy. The client resolved the target's unix name. Horizon cannot
 * rename an open file, so this handle's descriptor is released for the rename
 * and reopened under the new name; another open handle makes it fail. */
static int horizon_server_handle_set_fd_name_info( struct horizon_server_connection *connection,
                                                   const unsigned char *message,
                                                   const unsigned char *data, unsigned int data_size )
{
    const struct horizon_set_fd_name_info_request *request = (const void *)message;
    enum horizon_rename_action action = HORIZON_RENAME_MOVE;
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object = NULL;
    unsigned int status = HORIZON_STATUS_SUCCESS, name_size = 0;
    char *target = NULL, *joined, *new_name;
    struct stat st;

    if (request->namelen > data_size) status = HORIZON_STATUS_INVALID_PARAMETER;
    else if (!(name_size = data_size - request->namelen)) status = HORIZON_STATUS_OBJECT_PATH_SYNTAX_BAD;
    else if (request->link) status = HORIZON_STATUS_NOT_SUPPORTED; /* FAT has no hard links */
    else if (!(target = malloc( name_size + 1 ))) status = HORIZON_STATUS_NO_MEMORY;
    else
    {
        memcpy( target, data + request->namelen, name_size );
        target[name_size] = 0;
    }

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!status)
    {
        if (!(entry = horizon_server_find_handle_locked( request->handle ))) status = HORIZON_STATUS_INVALID_HANDLE;
        else if (entry->object->type != HORIZON_SERVER_OBJECT_FILE || !entry->object->file_name)
            status = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
        else object = entry->object;
    }
    /* A name relative to a root directory handle. */
    if (!status && request->rootdir && !strchr( target, ':' ))
    {
        if (!(entry = horizon_server_find_handle_locked( request->rootdir )) ||
            entry->object->type != HORIZON_SERVER_OBJECT_FILE || !entry->object->file_is_dir ||
            !entry->object->file_name)
            status = HORIZON_STATUS_INVALID_HANDLE;
        else if (!(joined = horizon_dir_entry_path( entry->object->file_name, target )))
            status = HORIZON_STATUS_NO_MEMORY;
        else
        {
            free( target );
            target = joined;
        }
    }
    if (!status)
    {
        int exists = 0, target_is_dir = 0, target_open = 0;

        for (entry = horizon_server_handles; entry && !status; entry = entry->next)
        {
            const struct horizon_server_object *other = entry->object;

            if (other == object || other->type != HORIZON_SERVER_OBJECT_FILE ||
                !other->file_name || other->file_fd == -1)
                continue;
            if (horizon_unix_path_equal( other->file_name, object->file_name ))
                status = HORIZON_STATUS_SHARING_VIOLATION;
            else if (horizon_unix_path_equal( other->file_name, target ))
                target_open = 1;
        }
        if (!status)
        {
            if (!stat( target, &st ))
            {
                exists = 1;
                target_is_dir = S_ISDIR( st.st_mode );
            }
            else if (errno == EIO) exists = target_open = 1; /* open for writing somewhere */
            status = horizon_rename_check( exists, horizon_unix_path_equal( object->file_name, target ),
                                           target_is_dir, target_open, request->flags, &action );
        }
    }
    if (!status && action != HORIZON_RENAME_NOTHING)
    {
        off_t position = 0;
        int reopen = object->file_fd != -1;

        if (reopen)
        {
            position = lseek( object->file_fd, 0, SEEK_CUR );
            close( object->file_fd );
            object->file_fd = -1;
        }
        if (action == HORIZON_RENAME_REPLACE && unlink( target ) == -1 && errno != ENOENT)
            status = horizon_server_errno_status( errno );
        else if (rename( object->file_name, target ) == -1)
            status = horizon_server_rename_errno_status( errno );
        else if (!(new_name = strdup( target ))) status = HORIZON_STATUS_NO_MEMORY;
        else
        {
            free( object->file_name );
            object->file_name = new_name;
        }
        if (reopen)
        {
            /* The same access without creation flags, at the same position. */
            object->file_fd = open( object->file_name, horizon_file_access_mode( object->file_access ) );
            if (object->file_fd != -1 && position > 0) lseek( object->file_fd, position, SEEK_SET );
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZFILE] rename handle=%08x target=%s flags=%x action=%d status=%08x\n",
                   request->handle, target ? target : "<none>", request->flags, (int)action, status );
    free( target );
    return horizon_server_write_status( connection->reply_fd, status );
}

/* Directory listings stat each entry below the directory's unix name. */
static int horizon_server_handle_get_handle_unix_name( struct horizon_server_connection *connection,
                                                       const unsigned char *message )
{
    const struct horizon_get_handle_unix_name_request *request = (const void *)message;
    struct horizon_get_handle_unix_name_reply reply;
    struct horizon_server_handle_entry *entry;
    char *name = NULL;
    int ret;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->handle );
    if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_FILE || !entry->object->file_name)
        reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else
    {
        reply.name_len = strlen( entry->object->file_name );
        if (reply.name_len > request->header.reply_size) reply.header.error = HORIZON_STATUS_BUFFER_OVERFLOW;
        else if (!(name = strdup( entry->object->file_name ))) reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else reply.header.reply_size = reply.name_len;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), name,
                                      name ? reply.name_len : 0 );
    free( name );
    return ret;
}

static int horizon_server_handle_get_handle_fd( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct horizon_get_handle_fd_request *request = (const void *)message;
    struct horizon_get_handle_fd_reply reply;
    struct horizon_server_handle_entry *entry;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->handle );
    if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if ((entry->object->type != HORIZON_SERVER_OBJECT_FILE &&
              entry->object->type != HORIZON_SERVER_OBJECT_MAPPING &&
              entry->object->type != HORIZON_SERVER_OBJECT_SOCK) ||
             entry->object->file_fd == -1)
    {
        if (entry->object->type == HORIZON_SERVER_OBJECT_FILE && entry->object->file_is_dir)
            reply.header.error = HORIZON_STATUS_BAD_DEVICE_TYPE;
        else
            reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    }
    else if (entry->object->type == HORIZON_SERVER_OBJECT_SOCK)
    {
        reply.type = HORIZON_FD_TYPE_SOCKET;
        reply.cacheable = 1;
        reply.access = FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA;
        reply.options = entry->object->file_options;
        horizon_server_queue_fd( entry->object->file_fd, request->handle );
    }
    else
    {
        reply.type = HORIZON_FD_TYPE_FILE;
        reply.cacheable = 0;
        reply.access = entry->object->file_access;
        reply.options = entry->object->file_options;
        horizon_server_queue_fd( entry->object->file_fd, request->handle );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static char horizon_server_ascii_lower( char ch )
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 'a' - 'A' : ch;
}

static int horizon_server_wildcard_match_tail( const char *mask, const char *name )
{
    while (*mask)
    {
        if (*mask == '*' || *mask == '<')
        {
            while (*mask == '*' || *mask == '<') mask++;
            if (!*mask) return 1;
            while (*name)
            {
                if (horizon_server_wildcard_match_tail( mask, name )) return 1;
                name++;
            }
            return horizon_server_wildcard_match_tail( mask, name );
        }
        if (*mask == '?' || *mask == '>' || *mask == '"')
        {
            if (!*name) return 0;
            mask++;
            name++;
            continue;
        }
        if (horizon_server_ascii_lower( *mask ) != horizon_server_ascii_lower( *name )) return 0;
        mask++;
        name++;
    }
    return !*name;
}

static int horizon_server_wildcard_match( const char *mask, const char *name )
{
    if (!mask || !mask[0]) return 1;
    if (!strcmp( mask, "*" ) || !strcmp( mask, "*.*" )) return 1;
    return horizon_server_wildcard_match_tail( mask, name );
}

static char *horizon_server_dir_mask_from_utf16( const unsigned char *data, unsigned int data_size )
{
    unsigned int i, len = data_size / sizeof(unsigned short);
    char *mask;

    if (!data_size) return NULL;
    if (data_size & 1) return NULL;
    if (!(mask = malloc( len + 1 ))) return NULL;
    for (i = 0; i < len; i++)
    {
        unsigned int ch = data[i * 2] | (data[i * 2 + 1] << 8);
        mask[i] = ch < 0x80 ? ch : '?';
    }
    mask[len] = 0;
    return mask;
}

/* A card is formatted FAT, which has nowhere to keep a file's resource fork, so
 * macOS writes the fork of FILE to a second file named ._FILE beside it. That is
 * AppleDouble, and it is the same thing NTFS keeps in an alternate data stream,
 * which no directory listing shows. Nothing on the Windows side of the card can
 * read one, and a program that loads everything in a directory finds four
 * kilobytes of Apple metadata where a DLL should be: Halo asks for every DLL in
 * its Controls directory, loads ._CONTROLS.DLL, is told the image is invalid,
 * and reports that one of its own files is missing or corrupted. A fork is
 * listed only once the file it belongs to is gone, when it is a file of its own
 * that the owner may want to delete. */
static int horizon_dir_entry_is_resource_fork( const char *dir, const char *name )
{
    struct stat st;
    char *path;
    int fork;

    if (name[0] != '.' || name[1] != '_' || !name[2]) return 0;
    if (!(path = horizon_dir_entry_path( dir, name + 2 ))) return 0;
    fork = !stat( path, &st );
    free( path );
    return fork;
}

static void horizon_report_resource_fork( const char *dir, const char *name )
{
    static int reported;
    char message[384];

    if (reported) return;
    reported = 1;
    snprintf( message, sizeof(message), "[FS] %s/%s is a macOS resource fork, kept out of the listing", dir, name );
    wine_nx_runtime_trace( message );
}

static int horizon_server_handle_query_directory_file( struct horizon_server_connection *connection,
                                                       const unsigned char *message,
                                                       const unsigned char *data, unsigned int data_size )
{
    const struct horizon_query_directory_file_request *request = (const void *)message;
    struct horizon_query_directory_file_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object = NULL;
    struct horizon_directory_file_entry dir_entry;
    unsigned int entry_size = 0, name_len = 0;
    unsigned char *out = NULL;
    unsigned int out_size = 0;
    char trace_name[256] = "<none>";
    char trace_mask[128] = "<all>";
    unsigned int trace_index = 0;
    char *new_mask = NULL;
    DIR *dir = NULL;
    unsigned int raw_index = 0;
    int mask_changed = 0;
    int ret;

    memset( &reply, 0, sizeof(reply) );

    if (data_size)
    {
        if (data_size & 1) reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
        else if (!(new_mask = horizon_server_dir_mask_from_utf16( data, data_size )))
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else mask_changed = 1;
    }
    else if (request->restart_scan) mask_changed = 1;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!reply.header.error)
    {
        entry = horizon_server_find_handle_locked( request->handle );
        if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
        else if (entry->object->type != HORIZON_SERVER_OBJECT_FILE || !entry->object->file_is_dir)
            reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
        else if (!entry->object->file_name) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
        else object = entry->object;
    }

    if (!reply.header.error)
    {
        if (request->restart_scan) object->dir_enum_index = 0;
        if (mask_changed)
        {
            free( object->dir_mask );
            object->dir_mask = new_mask;
            new_mask = NULL;
        }

        if (!(dir = opendir( object->file_name ))) reply.header.error = horizon_server_errno_status( errno );
    }

    while (!reply.header.error)
    {
        const char *name = NULL;
        struct dirent *de = NULL;

        if (raw_index == 0) name = ".";
        else if (raw_index == 1) name = "..";
        else
        {
            while ((de = readdir( dir )))
            {
                if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
                if (horizon_dir_entry_is_resource_fork( object->file_name, de->d_name ))
                {
                    horizon_report_resource_fork( object->file_name, de->d_name );
                    continue;
                }
                name = de->d_name;
                break;
            }
            if (!name)
            {
                reply.header.error = horizon_dir_scan_end_status( !object->dir_queried );
                break;
            }
        }

        if (raw_index++ < object->dir_enum_index) continue;
        if (!horizon_server_wildcard_match( object->dir_mask, name )) continue;

        name_len = horizon_server_utf16_name_len( name );
        entry_size = (sizeof(dir_entry) + name_len + 3) & ~3;
        reply.total_len = name_len;
        if (entry_size > request->header.reply_size)
        {
            reply.header.error = HORIZON_STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        if (!(out = calloc( 1, entry_size )))
        {
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
            break;
        }

        dir_entry.name_len = name_len;
        memcpy( out, &dir_entry, sizeof(dir_entry) );
        horizon_server_write_utf16_name( out + sizeof(dir_entry), name );
        reply.header.reply_size = entry_size;
        out_size = entry_size;
        snprintf( trace_name, sizeof(trace_name), "%s", name );
        object->dir_enum_index = raw_index;
        break;
    }

    if (object)
    {
        object->dir_queried = 1;
        trace_index = object->dir_enum_index;
        if (object->dir_mask) snprintf( trace_mask, sizeof(trace_mask), "%s", object->dir_mask );
    }
    if (dir) closedir( dir );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZDIR] query handle=%08x restart=%u mask=%s name=%s idx=%u err=%08x\n",
                   request->handle, request->restart_scan, trace_mask, trace_name,
                   trace_index, reply.header.error );
    free( new_mask );

    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), out, out_size );
    free( out );
    return ret;
}

/***********************************************************************
 * Thread bridge: thread objects and their lifecycle
 *
 * NtCreateThreadEx queues the new thread's request pipe fd, then sends
 * new_thread.  We adopt the fd as a new server connection served by its own
 * pthread and return a handle to a thread object that the connection also
 * references.  The object becomes signaled when that connection sees the
 * request pipe close, which exit_thread does after the thread has finished
 * all Windows code (LdrShutdownThread, TLS callbacks, stack frames).
 * CREATE_SUSPENDED holds init_thread at a start gate until resume_thread.
 * Suspending a thread that already runs and terminating another thread need
 * an interpreter-safe stop point; both report STATUS_NOT_SUPPORTED.
 */

static void *horizon_server_thread( void *param );

/* A client thread and its connection thread take turns, so they share a core:
 * requests and replies hand over without waking another core, and connection
 * threads stop crowding the process's default core. */
static void horizon_server_follow_client( struct horizon_server_connection *connection )
{
    unsigned int mask;

    if (!connection->request_pipe) return;
    mask = __atomic_load_n( &connection->request_pipe->client_cores, __ATOMIC_RELAXED );
    if (!mask || mask == connection->core_mask) return;
    connection->core_mask = mask;
    svcSetThreadCoreMask( CUR_THREAD_HANDLE, lowest_set_core( mask ), mask );
}

static LONG horizon_server_next_tid = 4;

static int horizon_server_handle_new_thread( struct horizon_server_connection *connection,
                                             const unsigned char *message )
{
    const struct horizon_new_thread_request *request = (const void *)message;
    struct horizon_new_thread_reply reply;
    struct horizon_server_connection *thread_connection = NULL;
    struct horizon_server_handle_entry *entry = NULL;
    struct horizon_server_object *object = NULL;
    unsigned int fd_handle;
    pthread_t thread;
    int request_fd = horizon_server_take_client_fd( &fd_handle );

    /* Connection threads are joined here and when another connection ends. */
    horizon_zombie_reap( &horizon_server_zombies );
    memset( &reply, 0, sizeof(reply) );

    if (!(thread_connection = calloc( 1, sizeof(*thread_connection) )))
    {
        close( request_fd );
        reply.header.error = HORIZON_STATUS_NO_MEMORY;
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
    }
    thread_connection->request_fd = request_fd;
    thread_connection->reply_fd = -1;
    thread_connection->wait_fd = -1;
    thread_connection->pid = connection->pid;
    thread_connection->tid = __sync_add_and_fetch( &horizon_server_next_tid, 4 );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_alloc_thread_locked( thread_connection->tid, connection->pid )) &&
        (entry = horizon_server_create_handle_for_object_locked( object )))
    {
        object->refs++; /* the connection's reference, dropped when its pipe closes */
        object->thread.suspend = (request->flags & HORIZON_THREAD_CREATE_SUSPENDED) ? 1 : 0;
        thread_connection->thread = object;
        reply.handle = entry->handle;
        __atomic_add_fetch( &horizon_lifecycle.connections, 1, __ATOMIC_RELAXED );
    }
    else if (object)
    {
        horizon_server_running_threads--;
        horizon_server_free_object( object );
        object = NULL;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (!object)
    {
        close( request_fd );
        free( thread_connection );
        reply.header.error = HORIZON_STATUS_NO_MEMORY;
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
    }

    if ((errno = pthread_create( &thread, NULL, horizon_server_thread, thread_connection )))
    {
        horizon_trace( "[server] new_thread tid=%u: connection thread failed errno=%d",
                       thread_connection->tid, errno );
        pthread_mutex_lock( &horizon_server_objects_mutex );
        horizon_server_end_thread_locked( thread_connection );
        __atomic_sub_fetch( &horizon_lifecycle.connections, 1, __ATOMIC_RELAXED );
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        horizon_server_close_object_handle( reply.handle );
        close( request_fd );
        free( thread_connection );
        reply.handle = 0;
        reply.header.error = HORIZON_STATUS_NO_MEMORY;
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
    }
    /* No pthread_detach: libnx returns ENOSYS. The connection thread queues
     * itself on horizon_server_zombies when it ends. */

    reply.tid = thread_connection->tid;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_resume_thread( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct horizon_resume_thread_request *request = (const void *)message;
    struct horizon_resume_thread_reply reply;
    struct horizon_server_object *object;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
        status = horizon_thread_resume( &object->thread, &reply.count );
    horizon_server_signal_changed_locked();  /* the start gate */
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_suspend_thread( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_suspend_thread_request *request = (const void *)message;
    struct horizon_suspend_thread_reply reply;
    struct horizon_server_object *object;
    unsigned int status, tid = 0;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
    {
        status = horizon_thread_suspend( &object->thread, &reply.count );
        tid = object->thread.tid;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status == HORIZON_THREADS_STATUS_NOT_SUPPORTED)
        horizon_trace( "[server] suspend_thread tid=%u refused: thread already running", tid );
    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_terminate_thread( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_terminate_thread_request *request = (const void *)message;
    struct horizon_terminate_thread_reply reply;
    struct horizon_server_object *object;
    unsigned int status, tid = 0;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
    {
        if (object == connection->thread)
        {
            /* The client exits itself; the object signals when its pipe closes. */
            horizon_thread_set_exit_code( &object->thread, request->exit_code );
            reply.self = 1;
        }
        else if (!object->thread.terminated)
        {
            status = HORIZON_STATUS_NOT_SUPPORTED;
            tid = object->thread.tid;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status == HORIZON_STATUS_NOT_SUPPORTED)
        horizon_trace( "[server] terminate_thread tid=%u from tid=%u refused: remote termination unsupported",
                       tid, connection->tid );
    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_thread_info( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_get_thread_info_request *request = (const void *)message;
    struct horizon_get_thread_info_reply reply;
    struct horizon_server_object *object;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
    {
        const struct horizon_thread_state *thread = &object->thread;

        reply.pid = thread->pid;
        reply.tid = thread->tid;
        reply.teb = thread->teb;
        reply.entry_point = thread->entry;
        reply.affinity = thread->affinity;
        reply.exit_code = horizon_thread_exit_status( thread );
        reply.priority = thread->priority;
        reply.base_priority = thread->base_priority;
        reply.suspend_count = thread->suspend;
        reply.flags = horizon_thread_info_flags( thread, horizon_server_running_threads );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_thread_times( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_get_thread_times_request *request = (const void *)message;
    struct horizon_get_thread_times_reply reply;
    struct horizon_server_object *object;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    reply.unix_pid = reply.unix_tid = -1; /* no per-thread CPU times on Horizon */
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
    {
        reply.creation_time = object->thread.creation_time;
        reply.exit_time = object->thread.terminated ? object->thread.exit_time : 0;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_thread_info( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_set_thread_info_request *request = (const void *)message;
    struct horizon_server_object *object;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((object = horizon_server_get_thread_locked( request->handle, &status )))
    {
        struct horizon_thread_state *thread = &object->thread;

        if (request->mask & HORIZON_SET_THREAD_INFO_AFFINITY)
        {
            unsigned long long affinity = request->affinity & horizon_get_system_affinity_mask();

            if (affinity) thread->affinity = affinity;
            else status = HORIZON_STATUS_INVALID_PARAMETER;
        }
        if (!status)
        {
            if (request->mask & HORIZON_SET_THREAD_INFO_PRIORITY) thread->priority = request->priority;
            if (request->mask & HORIZON_SET_THREAD_INFO_BASE_PRIORITY)
                thread->base_priority = request->base_priority;
            if (request->mask & HORIZON_SET_THREAD_INFO_ENTRYPOINT) thread->entry = request->entry_point;
            /* Descriptions, tokens, debugger hiding and boost have no Horizon effect. */
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_open_thread( struct horizon_server_connection *connection,
                                              const unsigned char *message )
{
    const struct horizon_open_thread_request *request = (const void *)message;
    struct horizon_open_process_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *object;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (object = horizon_server_threads; object; object = object->thread_next)
        if (object->thread.tid == request->tid) break;
    if (!object) reply.header.error = HORIZON_STATUS_INVALID_CID;
    else if (!(entry = horizon_server_create_handle_for_object_locked( object )))
        reply.header.error = HORIZON_STATUS_NO_MEMORY;
    else reply.handle = entry->handle;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/***********************************************************************
 * Socket bridge: \Device\Afd handles backed by libnx BSD sockets.
 *
 * ws2_32 opens \Device\Afd (open_file_object), then drives the socket
 * with AFD ioctls.  Control ioctls (create/connect/poll/...) land here
 * via REQ_ioctl and run directly against the unix fd.  Data ioctls
 * (recvmsg/sendmsg/sockopts) run client-side in ntdll's socket.c using
 * the fd from get_handle_fd; recv_socket/send_socket only ask us for
 * permission, answered with ALERTED + nonblocking so the client does
 * the actual I/O itself and maps EAGAIN to WSAEWOULDBLOCK.
 */

/* AFD ioctl codes, mirrored from include/wine/afd.h (CTL_CODE expanded;
 * winsock headers must not be included here next to the BSD ones). */
#define HORIZON_IOCTL_AFD_BIND              0x00012003 /* BEEP/0x800/NEITHER */
#define HORIZON_IOCTL_AFD_LISTEN            0x0001200b /* BEEP/0x802/NEITHER */
#define HORIZON_IOCTL_AFD_POLL              0x00012024 /* BEEP/0x809/BUFFERED */
#define HORIZON_IOCTL_AFD_GETSOCKNAME       0x0001202f /* BEEP/0x80b/NEITHER */
#define HORIZON_IOCTL_AFD_EVENT_SELECT      0x00012087 /* BEEP/0x821/NEITHER */
#define HORIZON_IOCTL_AFD_WINE_CREATE       0x00120320 /* NETWORK/200/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_ACCEPT       0x00120324 /* NETWORK/201/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_ACCEPT_INTO  0x00120328 /* NETWORK/202/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_CONNECT      0x0012032c /* NETWORK/203/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SHUTDOWN     0x00120330 /* NETWORK/204/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_FIONBIO      0x00120344 /* NETWORK/209/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GETPEERNAME  0x00120360 /* NETWORK/216/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_ERROR 0x00120378 /* NETWORK/222/BUFFERED */
/* The options a socket is given before it is bound; RakNet, which GOG Galaxy
 * runs, sets all five. */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_BROADCAST 0x00120374 /* NETWORK/221/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_LINGER    0x00120388 /* NETWORK/226/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_RCVBUF    0x00120394 /* NETWORK/229/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_SNDBUF    0x001203ac /* NETWORK/235/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_IP_HDRINCL   0x001203dc /* NETWORK/247/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_KEEPALIVE  0x00120380 /* NETWORK/224/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_OOBINLINE  0x00120390 /* NETWORK/228/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_SO_REUSEADDR  0x001203a8 /* NETWORK/234/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_TCP_NODELAY   0x00120474 /* NETWORK/285/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_BROADCAST  0x00120370 /* NETWORK/220/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_KEEPALIVE  0x0012037c /* NETWORK/223/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_OOBINLINE  0x0012038c /* NETWORK/227/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_RCVBUF     0x00120398 /* NETWORK/230/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_REUSEADDR  0x001203a4 /* NETWORK/233/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_SO_SNDBUF     0x001203b0 /* NETWORK/236/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_TCP_NODELAY   0x00120470 /* NETWORK/284/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_INFO          0x00120368 /* NETWORK/218/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_GET_IPV6_V6ONLY   0x0012045c /* NETWORK/279/BUFFERED */
#define HORIZON_IOCTL_AFD_WINE_SET_IPV6_V6ONLY   0x00120460 /* NETWORK/280/BUFFERED */

#define HORIZON_AFD_POLL_READ        0x0001
#define HORIZON_AFD_POLL_OOB         0x0002
#define HORIZON_AFD_POLL_WRITE       0x0004
#define HORIZON_AFD_POLL_HUP         0x0008
#define HORIZON_AFD_POLL_RESET       0x0010
#define HORIZON_AFD_POLL_CLOSE       0x0020
#define HORIZON_AFD_POLL_CONNECT     0x0040
#define HORIZON_AFD_POLL_ACCEPT      0x0080
#define HORIZON_AFD_POLL_CONNECT_ERR 0x0100

#define HORIZON_WS_AF_UNSPEC 0
#define HORIZON_WS_AF_INET   2
#define HORIZON_WS_AF_INET6  23

static unsigned int horizon_sock_errno_status( int err )
{
    switch (err)
    {
        case 0:                 return HORIZON_STATUS_SUCCESS;
        case EBADF:             return HORIZON_STATUS_INVALID_HANDLE;
        case EBUSY:             return HORIZON_STATUS_DEVICE_BUSY;
        case EPERM:
        case EACCES:            return HORIZON_STATUS_ACCESS_DENIED;
        case EFAULT:            return HORIZON_STATUS_INVALID_PARAMETER;
        case EINVAL:            return HORIZON_STATUS_INVALID_PARAMETER;
        case ENFILE:
        case EMFILE:            return HORIZON_STATUS_TOO_MANY_OPENED_FILES;
        case EINPROGRESS:
        case EWOULDBLOCK:       return HORIZON_STATUS_DEVICE_NOT_READY;
        case EALREADY:          return HORIZON_STATUS_ADDRESS_ALREADY_ASSOCIATED;
        case ENOTSOCK:          return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
        case EMSGSIZE:          return HORIZON_STATUS_BUFFER_OVERFLOW;
        case EPROTONOSUPPORT:
#ifdef ESOCKTNOSUPPORT
        case ESOCKTNOSUPPORT:
#endif
#ifdef EPFNOSUPPORT
        case EPFNOSUPPORT:
#endif
        case EAFNOSUPPORT:
        case EPROTOTYPE:
        case EOPNOTSUPP:        return HORIZON_STATUS_NOT_SUPPORTED;
        case ENOPROTOOPT:       return HORIZON_STATUS_INVALID_PARAMETER;
        case EADDRINUSE:        return HORIZON_STATUS_SHARING_VIOLATION;
        case ENODEV:
        case EADDRNOTAVAIL:     return HORIZON_STATUS_INVALID_ADDRESS_COMPONENT;
        case ECONNREFUSED:      return HORIZON_STATUS_CONNECTION_REFUSED;
#ifdef ESHUTDOWN
        case ESHUTDOWN:         return HORIZON_STATUS_PIPE_DISCONNECTED;
#endif
        case ENOTCONN:          return HORIZON_STATUS_INVALID_CONNECTION;
        case ETIMEDOUT:         return HORIZON_STATUS_IO_TIMEOUT;
        case ENETUNREACH:       return HORIZON_STATUS_NETWORK_UNREACHABLE;
        case EHOSTUNREACH:      return HORIZON_STATUS_HOST_UNREACHABLE;
        case ENETDOWN:          return HORIZON_STATUS_NETWORK_BUSY;
        case EPIPE:
        case ECONNRESET:        return HORIZON_STATUS_CONNECTION_RESET;
        case ECONNABORTED:      return HORIZON_STATUS_CONNECTION_ABORTED;
        case EISCONN:           return HORIZON_STATUS_CONNECTION_ACTIVE;
        case ENOBUFS:
        case ENOMEM:            return HORIZON_STATUS_NO_MEMORY;
        default:
            horizon_trace( "[server] unmapped socket errno %d\n", err );
            return HORIZON_STATUS_UNSUCCESSFUL;
    }
}

/* GET_SO_ERROR hands the value straight to applications, so it must be
 * a WSA error (10xxx), not an NTSTATUS. */
static unsigned int horizon_sock_errno_wsa( int err )
{
    switch (err)
    {
        case 0:             return 0;
        case EINTR:         return 10004;
        case EACCES:
        case EPERM:         return 10013;
        case EINVAL:        return 10022;
        case EMFILE:
        case ENFILE:        return 10024;
        case EINPROGRESS:
        case EWOULDBLOCK:   return 10035;
        case EALREADY:      return 10037;
        case ENOTSOCK:      return 10038;
        case EMSGSIZE:      return 10040;
        case EOPNOTSUPP:    return 10045;
        case EAFNOSUPPORT:  return 10047;
        case EADDRINUSE:    return 10048;
        case EADDRNOTAVAIL: return 10049;
        case ENETDOWN:      return 10050;
        case ENETUNREACH:   return 10051;
        case ECONNABORTED:  return 10053;
        case EPIPE:
        case ECONNRESET:    return 10054;
        case ENOBUFS:       return 10055;
        case EISCONN:       return 10056;
        case ENOTCONN:      return 10057;
        case ETIMEDOUT:     return 10060;
        case ECONNREFUSED:  return 10061;
        case EHOSTUNREACH:  return 10065;
        default:
            horizon_trace( "[server] unmapped WSA errno %d\n", err );
            return 10022; /* WSAEINVAL */
    }
}

/* Windows sockaddr_in (16-bit sin_family, no sin_len) <-> BSD sockaddr_in.
 * libnx is IPv4 only; reject everything else cleanly. */
/* A Windows address as the IPv4 socket underneath takes it. An IPv6 address
 * with no IPv4 behind it (horizon_sockaddr.h) is one Horizon cannot reach:
 * unreachable for a connect, not a local address for a bind. So is IPv4
 * written the IPv6 way on a socket that asked to be IPv6 only, as a host
 * socket with IPV6_V6ONLY refuses it for wineserver. */
static unsigned int horizon_ws_sockaddr_to_unix_for( const unsigned char *ws, unsigned int len,
                                                     struct sockaddr_in *sa, int v6only, int connecting )
{
    unsigned short family;

    if (len < 16) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( &family, ws, sizeof(family) );
    memset( sa, 0, sizeof(*sa) );
#ifndef __linux__
    sa->sin_len = sizeof(*sa);
#endif
    sa->sin_family = AF_INET;
    if (family == HORIZON_WS_AF_INET6)
    {
        unsigned char port[2], v4[4];
        int found = horizon_ws_in6_to_v4( ws, len, port, v4 );

        if (found < 0) return HORIZON_STATUS_INVALID_PARAMETER;
        if (!found || (connecting && v6only && horizon_in6_is_mapped( ws + 8 )))
            return connecting ? HORIZON_STATUS_NETWORK_UNREACHABLE : HORIZON_STATUS_INVALID_ADDRESS_COMPONENT;
        memcpy( &sa->sin_port, port, 2 );
        memcpy( &sa->sin_addr, v4, 4 );
        return HORIZON_STATUS_SUCCESS;
    }
    if (family != HORIZON_WS_AF_INET) return HORIZON_STATUS_NOT_SUPPORTED;
    memcpy( &sa->sin_port, ws + 2, 2 );
    memcpy( &sa->sin_addr, ws + 4, 4 );
    return HORIZON_STATUS_SUCCESS;
}

static unsigned int horizon_ws_sockaddr_to_unix( const unsigned char *ws, unsigned int len,
                                                 struct sockaddr_in *sa )
{
    return horizon_ws_sockaddr_to_unix_for( ws, len, sa, 0, 0 );
}

/* An IPv4 address as a socket of the given family reports it: a
 * sockaddr_in, or a sockaddr_in6 for one the program opened as IPv6. */
static unsigned int horizon_ws_sockaddr_from_unix_as( const struct sockaddr_in *sa, int family,
                                                      unsigned char *ws, unsigned int len )
{
    unsigned short ws_family = HORIZON_WS_AF_INET;

    if (family == HORIZON_WS_AF_INET6)
        return horizon_ws_in6_from_v4( (const unsigned char *)&sa->sin_port,
                                       (const unsigned char *)&sa->sin_addr, ws, len );
    if (len < 16) return 0;
    memset( ws, 0, 16 );
    memcpy( ws, &ws_family, sizeof(ws_family) );
    memcpy( ws + 2, &sa->sin_port, 2 );
    memcpy( ws + 4, &sa->sin_addr, 4 );
    return 16;
}

/* Look up an Afd handle and return its fd (or -1 before WINE_CREATE). */
static unsigned int horizon_server_find_sock_locked( unsigned int handle,
                                                     struct horizon_server_object **object )
{
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked( handle );

    *object = NULL;
    if (!entry) return HORIZON_STATUS_INVALID_HANDLE;
    if (entry->object->type != HORIZON_SERVER_OBJECT_SOCK) return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    *object = entry->object;
    return HORIZON_STATUS_SUCCESS;
}

/* The family a socket was opened with, for the client's recvfrom, which
 * reports where a datagram came from the way the program's socket speaks. */
unsigned int horizon_server_sock_family( unsigned int handle )
{
    struct horizon_server_object *object;
    unsigned int family = HORIZON_WS_AF_INET;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_sock_locked( handle, &object ) && object->sock_family) family = object->sock_family;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return family;
}

/* Whether a socket operation is ready to be run by a thread: a thread that
 * sleeps then waits in the server, where it is handed over (sync.c). */
int horizon_async_any_ready(void)
{
    return __atomic_load_n( &horizon_asyncs.ready, __ATOMIC_RELAXED ) > 0;
}

static unsigned long long horizon_async_now(void)
{
    LARGE_INTEGER now;

    NtQueryPerformanceCounter( &now, NULL );
    return now.QuadPart;
}

/* Whether overlapped sockets are used and how they end: a couple dozen lines,
 * enough to see a program's first connection go through. */
static void horizon_report_async( const char *what, const struct horizon_async *async, unsigned int status )
{
    static LONG reported;
    char message[192];

    if (__atomic_add_fetch( &reported, 1, __ATOMIC_RELAXED ) > 24) return;
    snprintf( message, sizeof(message), "[ASYNC] %s: socket %04x %s by %04x, status %08x%s%s",
              what, async->sock,
              async->kind == HORIZON_ASYNC_ACCEPT_INTO ? "AcceptEx" :
              async->kind == HORIZON_ASYNC_ACCEPT ? "accept" :
              async->direction == HORIZON_ASYNC_READ ? "recv" : "send",
              async->owner_tid, status, async->port ? ", port" : "", async->data.event ? ", event" : "" );
    wine_nx_runtime_trace( message );
}

/* server/async.c's create_async: the event the program gave is reset, and the
 * socket's completion port is taken as it is now -- the result goes there
 * even if the socket is closed first. */
static struct horizon_async *horizon_server_async_create_locked( struct horizon_server_connection *connection,
                                                                 const struct horizon_async_data *data,
                                                                 int direction, int kind )
{
    struct horizon_server_handle_entry *event;
    struct horizon_server_object *sock = NULL;
    struct horizon_async *async;

    if (!(async = calloc( 1, sizeof(*async) ))) return NULL;
    async->id = horizon_async_new_id( &horizon_asyncs );
    async->owner_tid = connection->tid;
    async->sock = data->handle;
    async->direction = direction;
    async->kind = kind;
    async->state = HORIZON_ASYNC_DIRECT;
    async->status = HORIZON_STATUS_ALERTED;
    async->data = *data;
    if (!horizon_server_find_sock_locked( data->handle, &sock ) && sock->file_completion)
    {
        async->port = sock->file_completion;
        sock->file_completion->refs++;
        async->port_key = sock->file_completion_key;
        async->port_flags = sock->file_completion_flags;
    }
    if (data->event && (event = horizon_server_find_handle_locked( data->event )) &&
        event->object->type == HORIZON_SERVER_OBJECT_EVENT)
        event->object->signaled = 0;
    horizon_async_add( &horizon_asyncs, async );
    return async;
}

static void horizon_server_async_free_locked( struct horizon_async *async )
{
    struct horizon_server_object *port = async->port;

    if (port && !--port->refs) horizon_server_free_object( port );
    horizon_async_free( async );
}

/* A socket accept() made, with what the listening one has: its blocking mode
 * and its WSAEventSelect, as server/sock.c's accept_socket gives it. */
static struct horizon_server_handle_entry *horizon_server_accepted_sock_locked( struct horizon_server_object *listener,
                                                                                int fd )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *sock;

    if (!(entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_SOCK ))) return NULL;
    sock = entry->object;
    sock->file_access = listener->file_access;
    sock->file_options = listener->file_options;
    sock->file_fd = fd;
    sock->sock_bound = 1;
    sock->sock_nonblocking = listener->sock_nonblocking;
    sock->sock_event_handle = listener->sock_event_handle;
    sock->sock_event_mask = listener->sock_event_mask;
    sock->sock_family = listener->sock_family;
    sock->sock_type = listener->sock_type;
    sock->sock_protocol = listener->sock_protocol;
    sock->sock_v6only = listener->sock_v6only;
    return entry;
}

/* server/sock.c's fill_accept_output: what AcceptEx writes, which the
 * client's callback fetches with get_async_result. Returns 0 while the first
 * data it asked for has not come. */
static int horizon_sock_accept_output_locked( struct horizon_async *async, struct horizon_server_object *target )
{
    int family = target->sock_family ? target->sock_family : HORIZON_WS_AF_INET;
    unsigned int local_at, remote_at, remote_len;
    struct sockaddr_in addr;
    socklen_t addr_len;
    unsigned char *out;
    int received = 0, len;

    if (!horizon_async_accept_layout( async->out_size, async->recv_len, async->local_len,
                                      &local_at, &remote_at, &remote_len ))
    {
        horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_BUFFER_TOO_SMALL, horizon_async_now() );
        return 1;
    }
    if (!(out = calloc( 1, async->out_size )))
    {
        horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_NO_MEMORY, horizon_async_now() );
        return 1;
    }
    if (async->recv_len && (received = recv( target->file_fd, out, async->recv_len, 0 )) == -1)
    {
        unsigned int status = horizon_sock_errno_status( errno );

        free( out );
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        horizon_async_ready( &horizon_asyncs, async, status, horizon_async_now() );
        return 1;
    }
    if (async->local_len)
    {
        addr_len = sizeof(addr);
        if (getsockname( target->file_fd, (struct sockaddr *)&addr, &addr_len ) == -1 ||
            !(len = horizon_ws_sockaddr_from_unix_as( &addr, family, out + local_at + sizeof(int),
                                                      async->local_len - sizeof(int) )))
            goto too_small;
        memcpy( out + local_at, &len, sizeof(len) );
    }
    addr_len = sizeof(addr);
    if (getpeername( target->file_fd, (struct sockaddr *)&addr, &addr_len ) == -1 ||
        !(len = horizon_ws_sockaddr_from_unix_as( &addr, family, out + remote_at + sizeof(int),
                                                  remote_len - sizeof(int) )))
        goto too_small;
    memcpy( out + remote_at, &len, sizeof(len) );
    async->out = out;
    async->out_status = HORIZON_STATUS_SUCCESS;
    async->out_info = received;
    horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_ALERTED, horizon_async_now() );
    return 1;

too_small:
    free( out );
    horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_BUFFER_TOO_SMALL, horizon_async_now() );
    return 1;
}

/* An accept waiting on a listening socket whose connection may have come:
 * accept() makes a new socket, AcceptEx puts the connection into the socket it
 * was given, whose old descriptor the client may still have cached. Returns 1
 * once the async is ready for its thread. */
static int horizon_sock_accept_async_locked( struct horizon_server_object *listener, struct horizon_async *async )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *target = NULL;
    unsigned int handle;
    int fd;

    if (async->kind == HORIZON_ASYNC_ACCEPT_INTO &&
        (horizon_server_find_sock_locked( async->accept_into, &target ) || target->file_fd == -1))
    {
        horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_INVALID_HANDLE, horizon_async_now() );
        return 1;
    }
    if (!async->accepted)
    {
        if ((fd = accept( listener->file_fd, NULL, NULL )) == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            horizon_async_ready( &horizon_asyncs, async, horizon_sock_errno_status( errno ), horizon_async_now() );
            return 1;
        }
        fcntl( fd, F_SETFL, O_NONBLOCK );
        if (async->kind == HORIZON_ASYNC_ACCEPT)
        {
            if (!(async->out = malloc( sizeof(handle) )) ||
                !(entry = horizon_server_accepted_sock_locked( listener, fd )))
            {
                close( fd );
                free( async->out );
                async->out = NULL;
                horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_NO_MEMORY, horizon_async_now() );
                return 1;
            }
            handle = entry->handle;
            memcpy( async->out, &handle, sizeof(handle) );
            async->out_size = sizeof(handle);
            async->out_status = HORIZON_STATUS_SUCCESS;
            async->out_info = sizeof(handle);
            horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_ALERTED, horizon_async_now() );
            return 1;
        }
        close( target->file_fd );
        target->file_fd = fd;
        target->sock_bound = 1;
        target->sock_pending_events = 0;
        horizon_client_forget_fd( async->accept_into );
        async->accepted = 1;
    }
    return horizon_sock_accept_output_locked( async, target );
}

/* The operations waiting on one socket, oldest first in each direction: a
 * recv or send whose socket is ready goes to its thread to be done, an accept
 * is done here. Returns whether any became ready. */
static int horizon_sock_poll_asyncs_locked( unsigned int handle, struct horizon_server_object *sock )
{
    int direction, changed = 0;

    for (direction = HORIZON_ASYNC_READ; direction <= HORIZON_ASYNC_WRITE; direction++)
    {
        struct horizon_async *async = horizon_async_next_queued( &horizon_asyncs, handle, direction );
        struct horizon_server_object *target;
        struct pollfd pfd;

        if (!async) continue;
        pfd.fd = sock->file_fd;
        /* AcceptEx that asked for the first data waits for it on the socket
         * the connection went into. */
        if (async->kind == HORIZON_ASYNC_ACCEPT_INTO && async->accepted &&
            !horizon_server_find_sock_locked( async->accept_into, &target ))
            pfd.fd = target->file_fd;
        if (pfd.fd == -1) continue;
        pfd.events = direction == HORIZON_ASYNC_READ ? POLLIN : POLLOUT;
        pfd.revents = 0;
        if (poll( &pfd, 1, 0 ) <= 0 || !(pfd.revents & (pfd.events | POLLHUP | POLLERR))) continue;
        if (async->kind == HORIZON_ASYNC_IO)
            horizon_async_ready( &horizon_asyncs, async, HORIZON_STATUS_ALERTED, horizon_async_now() );
        else if (!horizon_sock_accept_async_locked( sock, async ))
            continue;
        changed = 1;
    }
    return changed;
}

/* Every socket an operation waits on, each looked at once. */
static int horizon_sock_poll_all_asyncs_locked(void)
{
    struct horizon_server_object *sock;
    struct horizon_async *async;
    unsigned int handles[64], count = 0, i;
    int changed = 0;

    for (async = horizon_asyncs.head; async && count < ARRAY_SIZE(handles); async = async->next)
    {
        if (async->state != HORIZON_ASYNC_QUEUED) continue;
        for (i = 0; i < count; i++) if (handles[i] == async->sock) break;
        if (i == count) handles[count++] = async->sock;
    }
    for (i = 0; i < count; i++)
        if (!horizon_server_find_sock_locked( handles[i], &sock ) && sock->file_fd != -1 &&
            horizon_sock_poll_asyncs_locked( handles[i], sock ))
            changed = 1;
    return changed;
}

/* Poller for WSAEventSelect: scans sockets with a registered event mask and
 * sets the associated event object when new activity shows up.  Waiters are
 * poll-based (select_wait re-checks on TIMEOUT), so flipping signaled under
 * the objects mutex is all it takes to wake them. */
static pthread_t horizon_sock_poller_thread_id;
static int horizon_sock_poller_running;

static void *horizon_sock_poller_thread( void *param )
{
    int busy = 1, since_scan = 0;

    (void)param;
    for (;;)
    {
        {
            extern volatile int wine_nx_quit_requested __attribute__((weak));
            extern void wine_nx_quit_point( void ) __attribute__((weak));

            if (&wine_nx_quit_requested && wine_nx_quit_requested && &wine_nx_quit_point) wine_nx_quit_point();
        }
        struct horizon_server_handle_entry *entry;
        int changed = 0, tick = busy ? 5000 : 50000;

        /* Overlapped operations waiting on a socket are looked at every 5 ms,
         * which is what a round trip to the program's own server costs;
         * WSAEventSelect sockets every 50 ms, as before. */
        usleep( tick );
        since_scan += tick;
        pthread_mutex_lock( &horizon_server_objects_mutex );
        if (horizon_asyncs.head) changed = horizon_sock_poll_all_asyncs_locked();
        for (entry = since_scan >= 50000 ? horizon_server_handles : NULL; entry; entry = entry->next)
        {
            struct horizon_server_object *o = entry->object;
            struct pollfd pfd;
            int bits = 0, new_bits;

            if (o->type != HORIZON_SERVER_OBJECT_SOCK || o->file_fd == -1 || !o->sock_event_mask)
                continue;

            /* Horizon's poll() does not report POLLOUT when a nonblocking
             * connect completes, so probe completion directly: getpeername
             * succeeds once the socket is connected, ENOTCONN before. */
            if ((o->sock_event_mask & HORIZON_AFD_POLL_CONNECT) &&
                !(o->sock_pending_events & (HORIZON_AFD_POLL_CONNECT | HORIZON_AFD_POLL_CONNECT_ERR)))
            {
                struct sockaddr_in peer;
                socklen_t peer_len = sizeof(peer);

                if (!getpeername( o->file_fd, (struct sockaddr *)&peer, &peer_len ))
                {
                    bits |= o->sock_event_mask & (HORIZON_AFD_POLL_CONNECT | HORIZON_AFD_POLL_WRITE);
                    o->sock_err_ticks = 0;
                    horizon_trace( "[server] poller fd=%d connect completed (getpeername)\n", o->file_fd );
                }
            }

            pfd.fd = o->file_fd;
            pfd.events = 0;
            pfd.revents = 0;
            if (o->sock_event_mask & (HORIZON_AFD_POLL_READ | HORIZON_AFD_POLL_ACCEPT)) pfd.events |= POLLIN;
            if (o->sock_event_mask & (HORIZON_AFD_POLL_WRITE | HORIZON_AFD_POLL_CONNECT)) pfd.events |= POLLOUT;
            if (o->sock_event_mask & HORIZON_AFD_POLL_OOB) pfd.events |= POLLPRI;
            if (poll( &pfd, 1, 0 ) <= 0)
            {
                int new_connect = bits & ~o->sock_pending_events;
                if (new_connect)
                {
                    o->sock_pending_events |= new_connect;
                    if (o->sock_event_handle)
                        horizon_server_signal_object_locked( o->sock_event_handle );
                }
                continue;
            }

            if (pfd.revents & POLLIN)
                bits |= o->sock_event_mask & (HORIZON_AFD_POLL_READ | HORIZON_AFD_POLL_ACCEPT);
            if (pfd.revents & POLLOUT)
                bits |= o->sock_event_mask & (HORIZON_AFD_POLL_WRITE | HORIZON_AFD_POLL_CONNECT);
            if (pfd.revents & POLLPRI)
                bits |= o->sock_event_mask & HORIZON_AFD_POLL_OOB;
            if (pfd.revents & POLLOUT) o->sock_err_ticks = 0;
            if (pfd.revents & (POLLHUP | POLLERR))
            {
                int err = 0;
                socklen_t err_len = sizeof(err);

                getsockopt( o->file_fd, SOL_SOCKET, SO_ERROR, &err, &err_len );
                if (err)
                {
                    /* Horizon's bsd service can report POLLHUP plus a stale
                     * SO_ERROR while a nonblocking connect is still in
                     * flight; require the error to persist a few ticks before
                     * declaring the connect dead so real completions win. */
                    o->sock_err_ticks++;
                    horizon_trace( "[server] poller fd=%d revents=%#x so_error=%d ticks=%d\n",
                                   o->file_fd, pfd.revents, err, o->sock_err_ticks );
                    if (o->sock_err_ticks >= 3)
                    {
                        o->sock_connect_status = (int)horizon_sock_errno_status( err );
                        bits |= o->sock_event_mask & (HORIZON_AFD_POLL_CONNECT_ERR | HORIZON_AFD_POLL_RESET);
                    }
                }
                else if ((pfd.revents & POLLHUP) && !(pfd.revents & (POLLIN | POLLOUT)) &&
                         (o->sock_event_mask & HORIZON_AFD_POLL_CONNECT))
                {
                    /* Horizon's bsd service reports POLLHUP for sockets that
                     * are merely not connected yet; with no error and no data
                     * this is just an in-flight connect, not a hangup. */
                    o->sock_err_ticks = 0;
                }
                else if (pfd.revents & POLLHUP)
                    bits |= o->sock_event_mask & (HORIZON_AFD_POLL_HUP | HORIZON_AFD_POLL_CLOSE);
            }

            new_bits = bits & ~o->sock_pending_events;
            if (new_bits)
            {
                o->sock_pending_events |= new_bits;
                horizon_trace( "[server] poller fd=%d revents=%#x -> events=%#x (pending=%#x)\n",
                               o->file_fd, pfd.revents, new_bits, o->sock_pending_events );
                if (o->sock_event_handle)
                    horizon_server_signal_object_locked( o->sock_event_handle );
            }
        }
        if (since_scan >= 50000) since_scan = 0;
        busy = horizon_async_any_queued( &horizon_asyncs );
        /* Waiting threads look for what is ready for them, or has waited too
         * long for its own thread. */
        if (changed || horizon_async_any_stale( &horizon_asyncs, horizon_async_now(), HORIZON_ASYNC_STALE ))
            horizon_server_signal_changed_locked();
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return NULL;
}

static void horizon_sock_poller_start(void)
{
    if (horizon_sock_poller_running) return;
    if (!pthread_create( &horizon_sock_poller_thread_id, NULL, horizon_sock_poller_thread, NULL ))
    {
        pthread_detach( horizon_sock_poller_thread_id );
        horizon_sock_poller_running = 1;
    }
}

static unsigned int horizon_server_get_sock_fd( unsigned int handle, int *fd, int *nonblocking )
{
    struct horizon_server_object *object;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_sock_locked( handle, &object );
    if (!status && object->file_fd == -1) status = HORIZON_STATUS_INVALID_HANDLE;
    if (!status)
    {
        *fd = object->file_fd;
        if (nonblocking) *nonblocking = object->sock_nonblocking;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return status;
}

static int horizon_server_handle_open_file_object( struct horizon_server_connection *connection,
                                                   const unsigned char *message,
                                                   const unsigned char *data, unsigned int data_size )
{
    static const unsigned short afd_name[] = {'\\','D','e','v','i','c','e','\\','A','f','d'};
    const struct horizon_open_file_object_request *request = (const void *)message;
    struct horizon_open_file_object_reply reply;
    unsigned int chars = data_size / 2;
    int is_afd = 0;

    memset( &reply, 0, sizeof(reply) );

    /* accept "\Device\Afd" and "\Device\Afd\..." in any case */
    if (chars >= 11)
    {
        unsigned int i;
        is_afd = 1;
        for (i = 0; i < 11 && is_afd; i++)
        {
            unsigned short c;
            memcpy( &c, data + 2 * i, sizeof(c) );
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (afd_name[i] >= 'A' && afd_name[i] <= 'Z' ? afd_name[i] + 'a' - 'A' : afd_name[i]))
                is_afd = 0;
        }
        if (is_afd && chars > 11)
        {
            unsigned short c;
            memcpy( &c, data + 22, sizeof(c) );
            if (c != '\\') is_afd = 0;
        }
    }

    if (is_afd)
    {
        struct horizon_server_handle_entry *entry;

        pthread_mutex_lock( &horizon_server_objects_mutex );
        if ((entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_SOCK )))
        {
            entry->object->file_access = request->access;
            entry->object->file_options = request->options;
            reply.handle = entry->handle;
        }
        else reply.header.error = HORIZON_STATUS_NO_MEMORY;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        horizon_trace( "[server] open \\Device\\Afd access=%#x options=%#x -> handle=%08x status=%08x\n",
                       request->access, request->options, reply.handle, reply.header.error );
    }
    else
    {
        reply.header.error = HORIZON_STATUS_OBJECT_NAME_NOT_FOUND;
        if (chars && chars < 120)
        {
            char ascii[128];
            unsigned int i;
            for (i = 0; i < chars; i++)
            {
                unsigned short c;
                memcpy( &c, data + 2 * i, sizeof(c) );
                ascii[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            ascii[chars] = 0;
            horizon_trace( "[server] open_file_object '%s' -> not found\n", ascii );
        }
    }

    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_sock_ioctl_create( unsigned int handle, const unsigned char *data,
                                               unsigned int data_size )
{
    struct horizon_server_object *object;
    int params[4]; /* family, type, protocol, flags (WS values) */
    unsigned int status;
    int fd, type, protocol;

    if (data_size < sizeof(params)) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( params, data, sizeof(params) );

    /* AF_INET6 is an IPv4 socket underneath: Horizon has no IPv6, and what a
     * Windows program reaches through one is IPv4 (horizon_sockaddr.h). */
    if (params[0] != HORIZON_WS_AF_INET && params[0] != HORIZON_WS_AF_INET6)
    {
        horizon_trace( "[server] WINE_CREATE family=%d unsupported\n", params[0] );
        return HORIZON_STATUS_NOT_SUPPORTED;
    }
    switch (params[1])
    {
        case 1: type = SOCK_STREAM; break;
        case 2: type = SOCK_DGRAM; break;
        default: return HORIZON_STATUS_NOT_SUPPORTED;
    }
    protocol = params[2]; /* 0 / IPPROTO_TCP / IPPROTO_UDP share values */

    if ((fd = socket( AF_INET, type, protocol )) == -1)
        return horizon_sock_errno_status( errno );
    fcntl( fd, F_SETFL, O_NONBLOCK );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_sock_locked( handle, &object );
    if (!status)
    {
        if (object->file_fd != -1) close( object->file_fd );
        object->file_fd = fd;
        object->sock_nonblocking = 0;
        object->sock_bound = 0;
        object->sock_family = params[0];
        object->sock_type = params[1];
        object->sock_protocol = params[2];
        /* Windows makes an IPv6 socket IPv6 only until told otherwise. */
        object->sock_v6only = params[0] == HORIZON_WS_AF_INET6;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (status) close( fd );
    horizon_trace( "[server] WINE_CREATE handle=%08x family=%d type=%d proto=%d -> fd=%d status=%08x\n",
                   handle, params[0], params[1], params[2], fd, status );
    return status;
}

static unsigned int horizon_sock_ioctl_connect( unsigned int handle, const unsigned char *data,
                                                unsigned int data_size )
{
    struct sockaddr_in sa;
    unsigned int status;
    int addr_len, fd, nonblocking, ret;

    if (data_size < 8 + 16) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( &addr_len, data, sizeof(addr_len) );
    if (8u + (unsigned int)addr_len > data_size) return HORIZON_STATUS_INVALID_PARAMETER;

    if ((status = horizon_server_get_sock_fd( handle, &fd, &nonblocking ))) return status;
    {
        struct horizon_server_object *object;
        int v6only = 0;

        pthread_mutex_lock( &horizon_server_objects_mutex );
        if (!horizon_server_find_sock_locked( handle, &object )) v6only = object->sock_v6only;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        if ((status = horizon_ws_sockaddr_to_unix_for( data + 8, addr_len, &sa, v6only, 1 ))) return status;
    }

    {
        const unsigned char *ip = (const unsigned char *)&sa.sin_addr;
        horizon_trace( "[server] connect target: fd=%d len=%u family=%u ip=%u.%u.%u.%u port=%u\n",
                       fd, sa.sin_len, sa.sin_family, ip[0], ip[1], ip[2], ip[3],
                       (unsigned)((((const unsigned char *)&sa.sin_port)[0] << 8) |
                                  ((const unsigned char *)&sa.sin_port)[1]) );
    }

    /* Horizon's bsd service never completes nonblocking connects in a way
     * poll/getpeername can observe; blocking connect (the only style Switch
     * homebrew uses) works.  Do the wait here — the client is blocked on
     * this request anyway and each Wine thread has its own server thread —
     * and hand back the final result; ws2_32 treats an immediate success
     * from a nonblocking connect as connected. */
    fcntl( fd, F_SETFL, 0 );
    ret = connect( fd, (struct sockaddr *)&sa, sizeof(sa) );
    status = ret ? horizon_sock_errno_status( errno ) : HORIZON_STATUS_SUCCESS;
    fcntl( fd, F_SETFL, O_NONBLOCK );

    horizon_trace( "[server] WINE_CONNECT handle=%08x fd=%d port=%u nonblocking=%d blocking-connect ret=%d errno=%d -> status=%08x\n",
                   handle, fd, (unsigned)((data[10] << 8) | data[11]), nonblocking, ret, ret ? errno : 0, status );
    return status;
}

static unsigned int horizon_sock_ioctl_poll( unsigned int handle, const unsigned char *data,
                                             unsigned int data_size, unsigned char *out,
                                             unsigned int out_max, unsigned int *out_size )
{
    long long timeout;
    unsigned int count, i, signaled = 0;
    struct pollfd pfds[64];
    struct { unsigned long long socket; int flags; int status; } entry;
    int timeout_ms, ret;

    (void)handle;
    *out_size = 0;
    if (data_size < 16) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( &timeout, data, sizeof(timeout) );
    memcpy( &count, data + 8, sizeof(count) );
    if (!count || count > 64 || data_size < 16 + count * 16) return HORIZON_STATUS_INVALID_PARAMETER;
    if (out_max < 16 + count * 16) return HORIZON_STATUS_BUFFER_TOO_SMALL;

    for (i = 0; i < count; i++)
    {
        unsigned int status;
        int fd = -1;

        memcpy( &entry, data + 16 + i * 16, sizeof(entry) );
        status = horizon_server_get_sock_fd( (unsigned int)entry.socket, &fd, NULL );
        pfds[i].fd = status ? -1 : fd;
        pfds[i].events = 0;
        pfds[i].revents = 0;
        if (entry.flags & (HORIZON_AFD_POLL_READ | HORIZON_AFD_POLL_ACCEPT)) pfds[i].events |= POLLIN;
        if (entry.flags & (HORIZON_AFD_POLL_WRITE | HORIZON_AFD_POLL_CONNECT)) pfds[i].events |= POLLOUT;
        if (entry.flags & HORIZON_AFD_POLL_OOB) pfds[i].events |= POLLPRI;
    }

    if (timeout == 0x7fffffffffffffffLL) timeout_ms = -1;
    else if (timeout <= 0)
    {
        long long ms = (-timeout) / 10000;
        timeout_ms = ms > 0x7fffffff ? 0x7fffffff : (int)ms;
    }
    else
    {
        horizon_trace( "[server] AFD_POLL absolute timeout %lld not supported, polling once\n", timeout );
        timeout_ms = 0;
    }

    while ((ret = poll( pfds, count, timeout_ms )) == -1 && errno == EINTR) {}
    if (ret == -1) return horizon_sock_errno_status( errno );

    memcpy( out, data, 16 ); /* timeout/exclusive preserved */
    for (i = 0; i < count; i++)
    {
        int flags = 0;

        memcpy( &entry, data + 16 + i * 16, sizeof(entry) );
        if (pfds[i].fd == -1) flags = HORIZON_AFD_POLL_CLOSE;
        else
        {
            if (pfds[i].revents & POLLIN) flags |= entry.flags & (HORIZON_AFD_POLL_READ | HORIZON_AFD_POLL_ACCEPT);
            if (pfds[i].revents & POLLOUT) flags |= entry.flags & (HORIZON_AFD_POLL_WRITE | HORIZON_AFD_POLL_CONNECT);
            if (pfds[i].revents & POLLPRI) flags |= HORIZON_AFD_POLL_OOB;
            if (pfds[i].revents & POLLHUP) flags |= HORIZON_AFD_POLL_HUP;
            if (pfds[i].revents & POLLNVAL) flags |= HORIZON_AFD_POLL_CLOSE;
            if (pfds[i].revents & POLLERR)
            {
                int err = 0;
                socklen_t err_len = sizeof(err);
                getsockopt( pfds[i].fd, SOL_SOCKET, SO_ERROR, &err, &err_len );
                flags |= HORIZON_AFD_POLL_CONNECT_ERR;
                entry.status = (int)horizon_sock_errno_status( err );
            }
        }
        if (!flags) continue;
        entry.flags = flags;
        if (!(pfds[i].revents & POLLERR)) entry.status = 0;
        memcpy( out + 16 + signaled * 16, &entry, sizeof(entry) );
        signaled++;
    }
    memcpy( out + 8, &signaled, sizeof(signaled) );
    *out_size = 16 + signaled * 16;
    return HORIZON_STATUS_SUCCESS;
}

/* IOCTL_AFD_BIND carries struct afd_bind_params: a field Windows itself does
 * not read, then the sockaddr. As wineserver does, the reply is the address the
 * socket ended up on — the requested address with the port the kernel chose,
 * which is what ws2_32 keeps for getsockname. */
static unsigned int horizon_sock_ioctl_bind( unsigned int handle, const unsigned char *data,
                                             unsigned int data_size, unsigned char *out,
                                             unsigned int out_max, unsigned int *out_size )
{
    struct horizon_server_object *object;
    struct sockaddr_in sa, bound;
    socklen_t bound_len = sizeof(bound);
    unsigned int status;
    int fd = -1;

    int family = HORIZON_WS_AF_INET;

    if (data_size < 4 + 16) return HORIZON_STATUS_INVALID_PARAMETER;
    if ((status = horizon_ws_sockaddr_to_unix( data + 4, data_size - 4, &sa ))) return status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_sock_locked( handle, &object );
    if (!status && object->file_fd == -1) status = HORIZON_STATUS_INVALID_HANDLE;
    if (!status && object->sock_bound) status = HORIZON_STATUS_ADDRESS_ALREADY_ASSOCIATED;
    if (!status)
    {
        fd = object->file_fd;
        if (object->sock_family) family = object->sock_family;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status) return status;

    if (bind( fd, (struct sockaddr *)&sa, sizeof(sa) ) == -1)
    {
        status = horizon_sock_errno_status( errno );
        horizon_trace( "[server] AFD_BIND handle=%08x fd=%d port=%u errno=%d -> status=%08x\n",
                       handle, fd, (unsigned)((data[6] << 8) | data[7]), errno, status );
        return status;
    }

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_sock_locked( handle, &object )) object->sock_bound = 1;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (getsockname( fd, (struct sockaddr *)&bound, &bound_len ) == -1) bound = sa;
    else bound.sin_addr = sa.sin_addr;
    /* A buffer too small for the address is not an error; Windows binds anyway. */
    *out_size = horizon_ws_sockaddr_from_unix_as( &bound, family, out, out_max );
    horizon_trace( "[server] AFD_BIND handle=%08x fd=%d port=%u -> bound port=%u\n", handle, fd,
                   (unsigned)((data[6] << 8) | data[7]),
                   (unsigned)((((const unsigned char *)&bound.sin_port)[0] << 8) |
                              ((const unsigned char *)&bound.sin_port)[1]) );
    return HORIZON_STATUS_SUCCESS;
}

/* The socket options set before a bind. Each carries its value alone, except
 * SO_LINGER, whose Windows LINGER is two 16-bit fields where BSD has two ints. */
static unsigned int horizon_sock_ioctl_setsockopt( unsigned int code, unsigned int handle,
                                                   const unsigned char *data, unsigned int data_size )
{
    int fd, level = SOL_SOCKET, option, value = 0;
    socklen_t optlen = sizeof(value);
    struct linger linger_value;
    const void *optval = &value;
    unsigned int status;

    switch (code)
    {
    case HORIZON_IOCTL_AFD_WINE_SET_SO_BROADCAST: option = SO_BROADCAST; break;
    case HORIZON_IOCTL_AFD_WINE_SET_SO_RCVBUF:    option = SO_RCVBUF; break;
    case HORIZON_IOCTL_AFD_WINE_SET_SO_SNDBUF:    option = SO_SNDBUF; break;
    case HORIZON_IOCTL_AFD_WINE_SET_IP_HDRINCL:   level = IPPROTO_IP; option = IP_HDRINCL; break;
    case HORIZON_IOCTL_AFD_WINE_SET_SO_KEEPALIVE: option = SO_KEEPALIVE; break;
    case HORIZON_IOCTL_AFD_WINE_SET_SO_OOBINLINE: option = SO_OOBINLINE; break;
    /* Asio's acceptor sets this before it binds, and gives up if it fails. */
    case HORIZON_IOCTL_AFD_WINE_SET_SO_REUSEADDR: option = SO_REUSEADDR; break;
    case HORIZON_IOCTL_AFD_WINE_SET_TCP_NODELAY:  level = IPPROTO_TCP; option = TCP_NODELAY; break;
    default:                                      option = SO_LINGER; break;
    }

    if (option == SO_LINGER)
    {
        unsigned short onoff, seconds;

        if (data_size < 4) return HORIZON_STATUS_INVALID_PARAMETER;
        memcpy( &onoff, data, sizeof(onoff) );
        memcpy( &seconds, data + 2, sizeof(seconds) );
        memset( &linger_value, 0, sizeof(linger_value) );
        linger_value.l_onoff = onoff;
        linger_value.l_linger = seconds;
        optval = &linger_value;
        optlen = sizeof(linger_value);
    }
    else
    {
        if (data_size < sizeof(value)) return HORIZON_STATUS_INVALID_PARAMETER;
        memcpy( &value, data, sizeof(value) );
    }

    if ((status = horizon_server_get_sock_fd( handle, &fd, NULL ))) return status;
    if (setsockopt( fd, level, option, optval, optlen ) == -1)
        status = horizon_sock_errno_status( errno );
    else
        status = HORIZON_STATUS_SUCCESS;
    horizon_trace( "[server] setsockopt handle=%08x fd=%d level=%d option=%d value=%d -> status=%08x\n",
                   handle, fd, level, option, value, status );
    return status;
}

/* The options getsockopt reads as an int. */
static unsigned int horizon_sock_ioctl_getsockopt( unsigned int code, unsigned int handle,
                                                   unsigned char *out, unsigned int out_max, unsigned int *out_size )
{
    int fd, level = SOL_SOCKET, option, value = 0;
    socklen_t optlen = sizeof(value);
    unsigned int status;

    switch (code)
    {
    case HORIZON_IOCTL_AFD_WINE_GET_SO_BROADCAST: option = SO_BROADCAST; break;
    case HORIZON_IOCTL_AFD_WINE_GET_SO_KEEPALIVE: option = SO_KEEPALIVE; break;
    case HORIZON_IOCTL_AFD_WINE_GET_SO_OOBINLINE: option = SO_OOBINLINE; break;
    case HORIZON_IOCTL_AFD_WINE_GET_SO_RCVBUF:    option = SO_RCVBUF; break;
    case HORIZON_IOCTL_AFD_WINE_GET_SO_REUSEADDR: option = SO_REUSEADDR; break;
    case HORIZON_IOCTL_AFD_WINE_GET_SO_SNDBUF:    option = SO_SNDBUF; break;
    default:                                      level = IPPROTO_TCP; option = TCP_NODELAY; break;
    }
    if (out_max < sizeof(value)) return HORIZON_STATUS_BUFFER_TOO_SMALL;
    if ((status = horizon_server_get_sock_fd( handle, &fd, NULL ))) return status;
    if (getsockopt( fd, level, option, &value, &optlen ) == -1) return horizon_sock_errno_status( errno );
    if (level == SOL_SOCKET && option != SO_RCVBUF && option != SO_SNDBUF) value = !!value;
    memcpy( out, &value, sizeof(value) );
    *out_size = sizeof(value);
    return HORIZON_STATUS_SUCCESS;
}

/* What the program opened the socket as, kept by the server since the socket
 * underneath is IPv4 whatever it asked for: GET_INFO (getsockopt's
 * SO_PROTOCOL_INFO, WSADuplicateSocket) and IPV6_V6ONLY, which the client
 * cannot ask an IPv4 socket about. As on Windows, V6ONLY belongs to IPv6
 * sockets and is set before the bind; wineserver's client lets it be set on
 * an unbound IPv4 socket and does nothing. */
static unsigned int horizon_sock_ioctl_family( unsigned int code, unsigned int handle,
                                               const unsigned char *in, unsigned int in_size,
                                               unsigned char *out, unsigned int out_max, unsigned int *out_size )
{
    struct horizon_server_object *object;
    unsigned int status;
    int value, info[3];

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_sock_locked( handle, &object );
    if (status) goto done;
    switch (code)
    {
    case HORIZON_IOCTL_AFD_WINE_GET_INFO:
        if (out_max < sizeof(info)) { status = HORIZON_STATUS_BUFFER_TOO_SMALL; break; }
        info[0] = object->sock_family ? object->sock_family : HORIZON_WS_AF_INET;
        info[1] = object->sock_type;
        info[2] = object->sock_protocol;
        memcpy( out, info, sizeof(info) );
        *out_size = sizeof(info);
        break;
    case HORIZON_IOCTL_AFD_WINE_GET_IPV6_V6ONLY:
        if (object->sock_family != HORIZON_WS_AF_INET6) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        if (out_max < sizeof(value)) { status = HORIZON_STATUS_BUFFER_TOO_SMALL; break; }
        value = object->sock_v6only;
        memcpy( out, &value, sizeof(value) );
        *out_size = sizeof(value);
        break;
    default:
        if (in_size < sizeof(value)) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        memcpy( &value, in, sizeof(value) );
        if (object->sock_family != HORIZON_WS_AF_INET6)
        {
            if (object->sock_bound) status = HORIZON_STATUS_INVALID_PARAMETER;
            break;
        }
        if (object->sock_bound) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        object->sock_v6only = !!value;
        break;
    }
done:
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return status;
}

/* listen(): server/sock.c refuses a socket that was never bound. */
static unsigned int horizon_sock_ioctl_listen( unsigned int handle, const unsigned char *data, unsigned int data_size )
{
    struct horizon_server_object *object;
    int params[3]; /* unknown, backlog, unknown */
    unsigned int status;
    int fd = -1;

    if (data_size < sizeof(params)) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( params, data, sizeof(params) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_sock_locked( handle, &object );
    if (!status && (object->file_fd == -1 || !object->sock_bound)) status = HORIZON_STATUS_INVALID_PARAMETER;
    if (!status) fd = object->file_fd;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (!status && listen( fd, params[1] ) == -1) status = horizon_sock_errno_status( errno );
    horizon_trace( "[server] LISTEN handle=%08x backlog=%d -> %08x\n", handle, params[1], status );
    return status;
}

/* accept(): a connection that is there is taken at once; with none, a
 * nonblocking socket says so and a blocking one waits, as an async that the
 * poller finishes and ws2_32 waits for on its event. */
static unsigned int horizon_sock_ioctl_accept( struct horizon_server_connection *connection,
                                               const struct horizon_async_data *data,
                                               unsigned char *out, unsigned int out_max, unsigned int *out_size )
{
    struct horizon_server_handle_entry *entry;
    struct horizon_server_object *listener;
    struct horizon_async *async;
    unsigned int status;
    int fd;

    if (out_max < sizeof(entry->handle)) return HORIZON_STATUS_BUFFER_TOO_SMALL;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(status = horizon_server_find_sock_locked( data->handle, &listener )))
    {
        if (listener->file_fd == -1) status = HORIZON_STATUS_INVALID_PARAMETER;
        else if ((fd = accept( listener->file_fd, NULL, NULL )) != -1)
        {
            fcntl( fd, F_SETFL, O_NONBLOCK );
            if ((entry = horizon_server_accepted_sock_locked( listener, fd )))
            {
                memcpy( out, &entry->handle, sizeof(entry->handle) );
                *out_size = sizeof(entry->handle);
            }
            else
            {
                close( fd );
                status = HORIZON_STATUS_NO_MEMORY;
            }
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK) status = horizon_sock_errno_status( errno );
        else if (listener->sock_nonblocking) status = HORIZON_STATUS_DEVICE_NOT_READY;
        else if (!(async = horizon_server_async_create_locked( connection, data, HORIZON_ASYNC_READ,
                                                               HORIZON_ASYNC_ACCEPT )))
            status = HORIZON_STATUS_NO_MEMORY;
        else
        {
            async->state = HORIZON_ASYNC_QUEUED;
            async->pending = 1;
            status = HORIZON_STATUS_PENDING;
            horizon_report_async( "pending", async, status );
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status == HORIZON_STATUS_PENDING) horizon_sock_poller_start();
    return status;
}

/* AcceptEx: always pending, as server/sock.c queues it, into a socket that
 * was made for it and never bound. out_size is the whole buffer the program
 * gave: the first data, then room for the two addresses. */
static unsigned int horizon_sock_ioctl_accept_into( struct horizon_server_connection *connection,
                                                    const struct horizon_async_data *data,
                                                    const unsigned char *in, unsigned int in_size,
                                                    unsigned int out_size )
{
    struct horizon_server_object *listener, *target;
    unsigned int params[3]; /* accept_handle, recv_len, local_len */
    unsigned int local_at, remote_at, remote_len;
    struct horizon_async *async;
    unsigned int status;

    if (in_size < sizeof(params)) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( params, in, sizeof(params) );
    if (!horizon_async_accept_layout( out_size, params[1], params[2], &local_at, &remote_at, &remote_len ))
        return HORIZON_STATUS_BUFFER_TOO_SMALL;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(status = horizon_server_find_sock_locked( data->handle, &listener )) &&
        !(status = horizon_server_find_sock_locked( params[0], &target )))
    {
        if (listener->file_fd == -1 || target->file_fd == -1 || target->sock_bound)
            status = HORIZON_STATUS_INVALID_PARAMETER;
        else if (!(async = horizon_server_async_create_locked( connection, data, HORIZON_ASYNC_READ,
                                                               HORIZON_ASYNC_ACCEPT_INTO )))
            status = HORIZON_STATUS_NO_MEMORY;
        else
        {
            async->accept_into = params[0];
            async->recv_len = params[1];
            async->local_len = params[2];
            async->out_size = out_size;
            async->state = HORIZON_ASYNC_QUEUED;
            async->pending = 1;
            status = HORIZON_STATUS_PENDING;
            horizon_report_async( "pending", async, status );
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status == HORIZON_STATUS_PENDING) horizon_sock_poller_start();
    return status;
}

/* An ioctl done at once is told as server/async.c tells any operation that
 * ends directly: the event it gave is reset when it starts and set when it
 * is done, and on a socket tied to a port a packet goes there unless the
 * socket skips that. ConnectEx that connects at once returns TRUE, and Asio
 * then waits for that packet like any other. */
static void horizon_server_ioctl_start( const struct horizon_async_data *data )
{
    struct horizon_server_handle_entry *event;

    if (!data->event) return;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((event = horizon_server_find_handle_locked( data->event )) &&
        event->object->type == HORIZON_SERVER_OBJECT_EVENT)
        event->object->signaled = 0;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
}

static void horizon_server_ioctl_done( unsigned int tid, const struct horizon_async_data *data,
                                       unsigned int status, unsigned int information )
{
    struct horizon_server_object *sock = NULL;
    struct horizon_async async;

    if (HORIZON_NT_ERROR( status ) || (!data->event && !data->apc && !data->apc_context)) return;
    memset( &async, 0, sizeof(async) );
    async.owner_tid = tid;
    async.sock = data->handle;
    async.data = *data;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!horizon_server_find_sock_locked( data->handle, &sock ) && sock->file_completion)
    {
        async.port = sock->file_completion;
        async.port_key = sock->file_completion_key;
        async.port_flags = sock->file_completion_flags;
    }
    horizon_async_finish_locked( &async, status, information );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
}

static int horizon_server_handle_ioctl( struct horizon_server_connection *connection,
                                        const unsigned char *message,
                                        const unsigned char *data, unsigned int data_size )
{
    const struct horizon_ioctl_request *request = (const void *)message;
    struct horizon_ioctl_reply reply;
    unsigned int handle = request->async.handle;
    unsigned char out[1056]; /* >= 16 + 64*16 for poll */
    unsigned int out_size = 0;
    unsigned int out_max = request->header.reply_size;
    unsigned int status;

    if (out_max > sizeof(out)) out_max = sizeof(out);
    memset( &reply, 0, sizeof(reply) );
    horizon_server_ioctl_start( &request->async );

    switch (request->code)
    {
    case HORIZON_IOCTL_AFD_WINE_CREATE:
        status = horizon_sock_ioctl_create( handle, data, data_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_CONNECT:
        status = horizon_sock_ioctl_connect( handle, data, data_size );
        break;

    case HORIZON_IOCTL_AFD_LISTEN:
        status = horizon_sock_ioctl_listen( handle, data, data_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_GET_INFO:
    case HORIZON_IOCTL_AFD_WINE_GET_IPV6_V6ONLY:
    case HORIZON_IOCTL_AFD_WINE_SET_IPV6_V6ONLY:
        status = horizon_sock_ioctl_family( request->code, handle, data, data_size, out, out_max, &out_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_ACCEPT:
        status = horizon_sock_ioctl_accept( connection, &request->async, out, out_max, &out_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_ACCEPT_INTO:
        status = horizon_sock_ioctl_accept_into( connection, &request->async, data, data_size,
                                                 request->header.reply_size );
        break;

    case HORIZON_IOCTL_AFD_BIND:
        status = horizon_sock_ioctl_bind( handle, data, data_size, out, out_max, &out_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_SET_SO_BROADCAST:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_LINGER:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_RCVBUF:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_SNDBUF:
    case HORIZON_IOCTL_AFD_WINE_SET_IP_HDRINCL:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_KEEPALIVE:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_OOBINLINE:
    case HORIZON_IOCTL_AFD_WINE_SET_SO_REUSEADDR:
    case HORIZON_IOCTL_AFD_WINE_SET_TCP_NODELAY:
        status = horizon_sock_ioctl_setsockopt( request->code, handle, data, data_size );
        break;

    case HORIZON_IOCTL_AFD_WINE_GET_SO_BROADCAST:
    case HORIZON_IOCTL_AFD_WINE_GET_SO_KEEPALIVE:
    case HORIZON_IOCTL_AFD_WINE_GET_SO_OOBINLINE:
    case HORIZON_IOCTL_AFD_WINE_GET_SO_RCVBUF:
    case HORIZON_IOCTL_AFD_WINE_GET_SO_REUSEADDR:
    case HORIZON_IOCTL_AFD_WINE_GET_SO_SNDBUF:
    case HORIZON_IOCTL_AFD_WINE_GET_TCP_NODELAY:
        status = horizon_sock_ioctl_getsockopt( request->code, handle, out, out_max, &out_size );
        break;

    case HORIZON_IOCTL_AFD_POLL:
        status = horizon_sock_ioctl_poll( handle, data, data_size, out, out_max, &out_size );
        break;

    case HORIZON_IOCTL_AFD_GETSOCKNAME:
    case HORIZON_IOCTL_AFD_WINE_GETPEERNAME:
    {
        struct sockaddr_in sa;
        socklen_t sa_len = sizeof(sa);
        int fd, ret;

        if ((status = horizon_server_get_sock_fd( handle, &fd, NULL ))) break;
        if (request->code == HORIZON_IOCTL_AFD_GETSOCKNAME)
            ret = getsockname( fd, (struct sockaddr *)&sa, &sa_len );
        else
            ret = getpeername( fd, (struct sockaddr *)&sa, &sa_len );
        if (ret == -1) { status = horizon_sock_errno_status( errno ); break; }
        if (!(out_size = horizon_ws_sockaddr_from_unix_as( &sa, horizon_server_sock_family( handle ),
                                                           out, out_max )))
            status = HORIZON_STATUS_BUFFER_TOO_SMALL;
        else
            status = HORIZON_STATUS_SUCCESS;
        break;
    }

    case HORIZON_IOCTL_AFD_WINE_GET_SO_ERROR:
    {
        int fd, err = 0;
        socklen_t err_len = sizeof(err);
        unsigned int wsa_err;

        if ((status = horizon_server_get_sock_fd( handle, &fd, NULL ))) break;
        if (getsockopt( fd, SOL_SOCKET, SO_ERROR, &err, &err_len ) == -1)
        {
            status = horizon_sock_errno_status( errno );
            break;
        }
        wsa_err = horizon_sock_errno_wsa( err );
        if (out_max < sizeof(wsa_err)) { status = HORIZON_STATUS_BUFFER_TOO_SMALL; break; }
        memcpy( out, &wsa_err, sizeof(wsa_err) );
        out_size = sizeof(wsa_err);
        status = HORIZON_STATUS_SUCCESS;
        break;
    }

    case HORIZON_IOCTL_AFD_WINE_SHUTDOWN:
    {
        int fd, how;

        if (data_size < sizeof(how)) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        memcpy( &how, data, sizeof(how) );
        if ((status = horizon_server_get_sock_fd( handle, &fd, NULL ))) break;
        if (shutdown( fd, how ) == -1) status = horizon_sock_errno_status( errno );
        else status = HORIZON_STATUS_SUCCESS;
        break;
    }

    case HORIZON_IOCTL_AFD_WINE_FIONBIO:
    {
        struct horizon_server_object *object;
        int on;

        if (data_size < sizeof(on)) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        memcpy( &on, data, sizeof(on) );
        pthread_mutex_lock( &horizon_server_objects_mutex );
        status = horizon_server_find_sock_locked( handle, &object );
        if (!status) object->sock_nonblocking = !!on;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        horizon_trace( "[server] WINE_FIONBIO handle=%08x on=%d status=%08x\n", handle, on, status );
        break;
    }

    case HORIZON_IOCTL_AFD_EVENT_SELECT:
    {
        struct horizon_server_object *object;
        unsigned long long event;
        int mask;

        if (data_size < 12) { status = HORIZON_STATUS_INVALID_PARAMETER; break; }
        memcpy( &event, data, sizeof(event) );
        memcpy( &mask, data + 8, sizeof(mask) );
        pthread_mutex_lock( &horizon_server_objects_mutex );
        status = horizon_server_find_sock_locked( handle, &object );
        if (!status)
        {
            object->sock_event_handle = (unsigned int)event;
            object->sock_event_mask = mask;
            object->sock_pending_events = 0;
            object->sock_connect_status = 0;
            object->sock_nonblocking = 1; /* WSAEventSelect implies nonblocking */
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        if (!status && mask) horizon_sock_poller_start();
        horizon_trace( "[server] EVENT_SELECT handle=%08x event=%08x mask=%#x status=%08x\n",
                       handle, (unsigned int)event, mask, status );
        break;
    }

    default:
        horizon_trace( "[server] unimplemented ioctl code=%#x handle=%08x in=%u out=%u\n",
                       request->code, handle, data_size, request->header.reply_size );
        status = HORIZON_STATUS_NOT_IMPLEMENTED;
        break;
    }

    if (status != HORIZON_STATUS_PENDING)
        horizon_server_ioctl_done( connection->tid, &request->async, status, out_size );
    reply.header.error = status;
    reply.header.reply_size = out_size;
    reply.wait = 0;
    reply.options = 0;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                       out_size ? out : NULL, out_size );
}

static int horizon_server_handle_socket_get_events( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_socket_get_events_request *request = (const void *)message;
    struct horizon_socket_get_events_reply reply;
    struct horizon_server_object *object;
    int status_array[13];
    unsigned int out_size = 0, out_max = request->header.reply_size;

    memset( &reply, 0, sizeof(reply) );
    memset( status_array, 0, sizeof(status_array) );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_find_sock_locked( request->handle, &object );
    if (!reply.header.error)
    {
        reply.flags = object->sock_pending_events;
        status_array[8] = object->sock_connect_status; /* AFD_POLL_BIT_CONNECT_ERR */
        object->sock_pending_events = 0;
        if (request->event)
        {
            struct horizon_server_handle_entry *event_entry =
                horizon_server_find_handle_locked( request->event );
            if (event_entry && event_entry->object->type == HORIZON_SERVER_OBJECT_EVENT)
                event_entry->object->signaled = 0;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    if (!reply.header.error)
    {
        out_size = sizeof(status_array);
        if (out_size > out_max) out_size = out_max;
        reply.header.reply_size = out_size;
    }
    horizon_trace( "[server] GET_EVENTS handle=%08x -> flags=%#x status=%08x\n",
                   request->handle, reply.flags, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                       out_size ? status_array : NULL, out_size );
}

static int horizon_server_handle_socket_io( struct horizon_server_connection *connection,
                                            const unsigned char *message )
{
    const struct horizon_server_request_header *header = (const void *)message;
    const struct horizon_async_data *data;
    struct horizon_socket_io_reply reply;
    struct horizon_async *async;
    int direction;

    if (header->req == HORIZON_REQ_RECV_SOCKET)
    {
        data = &((const struct horizon_recv_socket_request *)message)->async;
        direction = HORIZON_ASYNC_READ;
    }
    else
    {
        data = &((const struct horizon_send_socket_request *)message)->async;
        direction = HORIZON_ASYNC_WRITE;
    }
    memset( &reply, 0, sizeof(reply) );
    /* ALERTED tells ntdll's socket.c to do the recvmsg/sendmsg itself on the
     * cached fd, and to tell set_async_direct_result how it went. The async
     * behind the wait token is what that result is kept against: an
     * overlapped operation that would block goes pending there and is
     * finished later, when the socket is ready. nonblocking keeps a plain
     * blocking call surfacing EAGAIN as WSAEWOULDBLOCK, as before. */
    reply.header.error = HORIZON_STATUS_ALERTED;
    reply.wait = 1;
    reply.options = 0;
    reply.nonblocking = 1;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((async = horizon_server_async_create_locked( connection, data, direction, HORIZON_ASYNC_IO )))
        reply.wait = async->id;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_async_direct_result( struct horizon_server_connection *connection,
                                                          const unsigned char *message )
{
    const struct horizon_set_async_direct_result_request *request = (const void *)message;
    struct horizon_set_async_direct_result_reply reply;
    struct horizon_async *async;
    int queued = 0;

    memset( &reply, 0, sizeof(reply) );
    reply.handle = 0; /* nothing for the client to wait on: it returns what it has */
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((async = horizon_async_find_id( &horizon_asyncs, request->handle )) &&
        async->state == HORIZON_ASYNC_DIRECT)
    {
        if (request->status == HORIZON_STATUS_PENDING)
        {
            /* The program was told ERROR_IO_PENDING: it hears when the
             * socket is ready and the operation has been done. */
            async->state = HORIZON_ASYNC_QUEUED;
            async->pending = 1;
            queued = 1;
            horizon_report_async( "pending", async, request->status );
        }
        else
        {
            async->pending = request->mark_pending;
            horizon_async_remove( &horizon_asyncs, async );
            horizon_async_finish_locked( async, request->status, request->information );
            horizon_server_async_free_locked( async );
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (queued) horizon_sock_poller_start();
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* get_async_result: what an accept the server did left for the client's
 * callback, which it copies into the program's buffer. */
static int horizon_server_handle_get_async_result( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_get_async_result_request *request = (const void *)message;
    struct horizon_server_reply_header reply;
    struct horizon_async *async;
    unsigned char *out = NULL;
    unsigned int size = 0;
    int ret;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(async = horizon_async_find_user( &horizon_asyncs, request->user_arg )) || !async->out)
        reply.error = HORIZON_STATUS_INVALID_PARAMETER;
    else
    {
        reply.error = async->out_status;
        size = min( async->out ? async->out_size : 0, request->header.reply_size );
        if (size && (out = malloc( size ))) memcpy( out, async->out, size );
        else size = 0;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.reply_size = size;
    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), out, size );
    free( out );
    return ret;
}

/* cancel_async: CancelIo and CancelIoEx, which Asio calls on a socket it is
 * done with. What is cancelled is finished as cancelled, which a program that
 * was told ERROR_IO_PENDING hears about on its port like any other result. */
static int horizon_server_handle_cancel_async( struct horizon_server_connection *connection,
                                               const unsigned char *message )
{
    const struct horizon_cancel_async_request *request = (const void *)message;
    unsigned int status = HORIZON_STATUS_NOT_FOUND;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (horizon_async_cancel( &horizon_asyncs, request->handle, request->iosb,
                              request->only_thread ? connection->tid : 0, horizon_async_now(), 0 ))
    {
        status = HORIZON_STATUS_SUCCESS;
        horizon_server_signal_changed_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

#ifdef __SWITCH__
void horizon_trace( const char *fmt, ... )
{
    extern int wine_nx_runtime_verbose __attribute__((weak));
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    __builtin_va_list args;
    FILE *f;

    /* Mapping failures must survive quiet runs. Sample repeated failures so
     * an allocator retry loop cannot flood the SD card or hide the last error. */
    if (!strncmp( fmt, "[HMAP]", 6 ) && strstr( fmt, "failed" ))
    {
        /* Per call site, so one loud failure cannot sample out the first of
         * another kind: every literal format string has its own address. */
        static struct { const char *fmt; unsigned int count; } sites[24];
        static unsigned int used;
        unsigned int i, count = 0;

        for (i = 0; i < used && i < ARRAY_SIZE(sites); i++)
            if (sites[i].fmt == fmt) break;
        if (i == used && used < ARRAY_SIZE(sites))
        {
            sites[i].fmt = fmt;
            __atomic_store_n( &used, used + 1, __ATOMIC_RELAXED );
        }
        count = i < ARRAY_SIZE(sites) ? __atomic_add_fetch( &sites[i].count, 1, __ATOMIC_RELAXED ) : 1;
        if (count <= 16 || !(count & (count - 1)))
        {
            char message[384];
            __builtin_va_start( args, fmt );
            vsnprintf( message, sizeof(message), fmt, args );
            __builtin_va_end( args );
            wine_nx_runtime_trace( message );
        }
    }
    /* Each verbose line reopens the separate trace file on the SD card. */
    if (!&wine_nx_runtime_verbose || !wine_nx_runtime_verbose) return;
    pthread_mutex_lock( &lock );
    if ((f = fopen( "sdmc:/switch/wine/logs/horizon-trace.log", "a" )))
    {
        __builtin_va_start( args, fmt );
        vfprintf( f, fmt, args );
        __builtin_va_end( args );
        fputc( '\n', f );
        fclose( f );
    }
    pthread_mutex_unlock( &lock );
}

/* Address arbitration (4.0.0+) is Horizon's futex: WaitIfEqual compares and
 * sleeps atomically with respect to SignalToAddress. */
int horizon_futex_wait( const int *addr, int value, long long timeout_ns )
{
    extern volatile int wine_nx_quit_requested __attribute__((weak));
    extern void wine_nx_quit_point( void ) __attribute__((weak));
    /* Nothing interrupts svcWaitForAddress, and a thread asked to end has to
     * notice: wait without an end in slices, and look between them. */
    const long long slice = 250000000LL;
    Result rc;

    for (;;)
    {
        if (&wine_nx_quit_requested && wine_nx_quit_requested && &wine_nx_quit_point) wine_nx_quit_point();
        rc = svcWaitForAddress( (void *)addr, ArbitrationType_WaitIfEqual, value,
                                timeout_ns < 0 ? slice : timeout_ns );
        if (timeout_ns >= 0 || !(R_MODULE(rc) == Module_Kernel && R_DESCRIPTION(rc) == KernelError_TimedOut)) break;
    }
    if (R_SUCCEEDED(rc)) return 0;
    if (R_MODULE(rc) == Module_Kernel && R_DESCRIPTION(rc) == KernelError_TimedOut) errno = ETIMEDOUT;
    else if (R_MODULE(rc) == Module_Kernel && R_DESCRIPTION(rc) == KernelError_InvalidState) errno = EAGAIN;
    else errno = EINVAL;
    return -1;
}

void horizon_futex_wake( const int *addr, int count )
{
    svcSignalToAddress( (void *)addr, SignalType_Signal, 0, count );
}

/* Monotonic time since boot in 100 ns units (KUSER_SHARED_DATA InterruptTime). */
unsigned long long horizon_interrupt_time(void)
{
    return armTicksToNs( armGetSystemTick() ) / 100;
}

/* Horizon has no console: the runtime points the standard handles at files
 * and tags their objects here, so duplicated handles are echoed too. */
void horizon_mark_std_stream( HANDLE handle, int stream )
{
    struct horizon_server_handle_entry *entry;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((entry = horizon_server_find_handle_locked( HandleToULong( handle ) )) &&
        entry->object->type == HORIZON_SERVER_OBJECT_FILE)
        entry->object->std_stream = stream;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
}

/* Called by NtWriteFile after a successful write. Re-reading the files from
 * the SD card while they were open for writing captured nothing on hardware. */
void horizon_echo_std_write( HANDLE handle, const void *data, size_t size )
{
    extern void wine_nx_runtime_std_write( int stream, const char *data, size_t size ) __attribute__((weak));
    struct horizon_server_handle_entry *entry;
    int stream = 0;

    if (!&wine_nx_runtime_std_write || !size) return;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((entry = horizon_server_find_handle_locked( HandleToULong( handle ) )))
        stream = entry->object->std_stream;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (stream) wine_nx_runtime_std_write( stream, data, size );
}

/* The SD card will not open a file a second time while it is open for
 * writing, so stat() of such a file fails with EIO (seen on hardware for the
 * runtime's stdout.txt). Answer from a descriptor this process already holds.
 * On failure errno is left as the caller's stat() set it. */
int horizon_stat_open_file( const char *path, struct stat *st )
{
    struct horizon_server_handle_entry *entry;
    int saved_errno = errno, ret = -1;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    for (entry = horizon_server_handles; entry && ret; entry = entry->next)
    {
        const struct horizon_server_object *object = entry->object;

        if (object->type != HORIZON_SERVER_OBJECT_FILE || object->file_is_dir ||
            object->file_fd == -1 || !object->file_name)
            continue;
        if (horizon_unix_path_equal( object->file_name, path )) ret = fstat( object->file_fd, st );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (ret) errno = saved_errno;
    return ret;
}

/* Present only in runtimes linked with the Box64 interpreter. */
extern LONG wine_nx_box64_live_engines __attribute__((weak));

static struct horizon_lifecycle_counters horizon_lifecycle_base;
static int horizon_lifecycle_have_base;

/* Taken before the first thread is created, after process initialization. */
void horizon_lifecycle_baseline(void)
{
    if (horizon_lifecycle_have_base) return;
    horizon_lifecycle_base.connections = __atomic_load_n( &horizon_lifecycle.connections, __ATOMIC_ACQUIRE );
    horizon_lifecycle_base.thread_objects = __atomic_load_n( &horizon_lifecycle.thread_objects, __ATOMIC_ACQUIRE );
    horizon_lifecycle_base.pipes = __atomic_load_n( &horizon_lifecycle.pipes, __ATOMIC_ACQUIRE );
    horizon_lifecycle_base.tebs = __atomic_load_n( &horizon_lifecycle.tebs, __ATOMIC_ACQUIRE );
    horizon_lifecycle_base.worker_pthreads = __atomic_load_n( &horizon_lifecycle.worker_pthreads, __ATOMIC_ACQUIRE );
    horizon_lifecycle_have_base = 1;
    horizon_lifecycle_report( "baseline", 0, 0 );
}

/* One line per event; "final" also judges reclamation against the baseline.
 * One exited thread may legitimately linger: exit_thread frees the previous
 * thread's TEB and joins its pthread, and a connection thread is joined by
 * the next one to end or by the next new_thread. */
void horizon_lifecycle_report( const char *tag, unsigned int tid, int code )
{
    extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
    const struct horizon_lifecycle_counters *base = &horizon_lifecycle_base;
    struct horizon_lifecycle_counters now;
    unsigned int running, zombies;
    unsigned long long reaped;
    LONG engines = &wine_nx_box64_live_engines ?
                   __atomic_load_n( &wine_nx_box64_live_engines, __ATOMIC_ACQUIRE ) : -1;
    char buf[384];

    if (!&wine_nx_runtime_trace) return;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    running = horizon_server_running_threads;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    pthread_mutex_lock( &horizon_server_zombies.lock );
    zombies = horizon_server_zombies.pending;
    reaped = horizon_server_zombies.reaped;
    pthread_mutex_unlock( &horizon_server_zombies.lock );
    now.connections = __atomic_load_n( &horizon_lifecycle.connections, __ATOMIC_ACQUIRE );
    now.thread_objects = __atomic_load_n( &horizon_lifecycle.thread_objects, __ATOMIC_ACQUIRE );
    now.pipes = __atomic_load_n( &horizon_lifecycle.pipes, __ATOMIC_ACQUIRE );
    now.tebs = __atomic_load_n( &horizon_lifecycle.tebs, __ATOMIC_ACQUIRE );
    now.worker_pthreads = __atomic_load_n( &horizon_lifecycle.worker_pthreads, __ATOMIC_ACQUIRE );
    now.thread_exits = __atomic_load_n( &horizon_lifecycle.thread_exits, __ATOMIC_ACQUIRE );

    snprintf( buf, sizeof(buf), "[LIFECYCLE] %s tid=%u code=%08x running=%u objects=%d connections=%d "
              "zombies=%u reaped=%llu pipes=%d tebs=%d pthreads=%d exits=%d engines=%d",
              tag, tid, (unsigned int)code, running, (int)now.thread_objects, (int)now.connections,
              zombies, reaped, (int)now.pipes, (int)now.tebs, (int)now.worker_pthreads,
              (int)now.thread_exits, (int)engines );
    wine_nx_runtime_trace( buf );

    if (strcmp( tag, "final" ) || !horizon_lifecycle_have_base) return;
    snprintf( buf, sizeof(buf), "[LIFECYCLE] verdict=%s exits=%d reaped=%llu (need running=1 "
              "objects=%d connections=%d pipes=%d tebs<=%d pthreads<=%d zombies<=1 engines<=0)",
              running == 1 && now.thread_objects == base->thread_objects &&
              now.connections == base->connections && now.pipes == base->pipes &&
              now.tebs <= base->tebs + 1 && now.worker_pthreads <= base->worker_pthreads + 1 &&
              zombies <= 1 && engines <= 0 ? "PASS" : "FAIL",
              (int)now.thread_exits, reaped, (int)base->thread_objects, (int)base->connections,
              (int)base->pipes, (int)base->tebs + 1, (int)base->worker_pthreads + 1 );
    wine_nx_runtime_trace( buf );
}

BOOL horizon_get_stack_region( void **start, void **limit )
{
    u64 base, size;

    if (R_FAILED( svcGetInfo( &base, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0 ) ) ||
        R_FAILED( svcGetInfo( &size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0 ) ) ||
        !size || base + size <= base) return FALSE;
    *start = (void *)base;
    *limit = (void *)(base + size);
    return TRUE;
}

/* The next page at or after addr, below limit, that the kernel holds threads'
 * local storage in, or 0. The kernel places those pages itself, at random in
 * the code region, whenever a new thread finds no free slot in the ones it
 * has; libnx's reservations mean nothing to it. */
unsigned long long horizon_next_thread_local_page( unsigned long long addr, unsigned long long limit )
{
    MemoryInfo info;
    u32 page_info;

    while (addr < limit)
    {
        if (R_FAILED( svcQueryMemory( &info, &page_info, addr ) )) return 0;
        if (info.type == MemType_ThreadLocal) return info.addr > addr ? info.addr : addr;
        if (info.addr + info.size <= addr) return 0;  /* the last block wraps */
        addr = info.addr + info.size;
    }
    return 0;
}

void horizon_get_address_space_limits( void **start, void **limit )
{
    u64 base = 0, size = 0;
    Result rc_base, rc_size;

    rc_base = svcGetInfo( &base, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0 );
    rc_size = svcGetInfo( &size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0 );
    if (R_SUCCEEDED(rc_base) && R_SUCCEEDED(rc_size) && size && base + size > base)
    {
        *start = (void *)base;
        *limit = (void *)(base + size);
        horizon_trace( "[VA] Horizon ASLR region base=0x%llx size=0x%llx limit=0x%llx",
                       (unsigned long long)base, (unsigned long long)size,
                       (unsigned long long)(base + size) );
        return;
    }

    *start = (void *)0x08000000ULL;
    *limit = (void *)0x8000000000ULL;
    horizon_trace( "[VA] Horizon ASLR query failed base_rc=0x%x size_rc=0x%x; fallback base=%p limit=%p",
                   rc_base, rc_size, *start, *limit );
}
#endif

/* As wineserver's build_shared_mapping (server/mapping.c): the writable shared
 * sections of an image are backed by one file that every view of the image
 * shares, each section taking the next multiple of its mapped size, filled
 * from the image file. map_image_into_view maps that file over the sections;
 * without it an image with such a section - FEAR.exe's .SHARED - fails there
 * with STATUS_INVALID_IMAGE_FORMAT. The file is a memfile, the only kind
 * horizon_mmap maps shared.
 * Returns the memfile descriptor, -2 when the image has no such section, or
 * -1 with errno set. */
static int horizon_server_build_shared_mapping( int fd, unsigned int alignment )
{
    static const unsigned int sector_align = 0x1ff;
    const unsigned long long align_mask = alignment - 1 > 0xfff ? alignment - 1 : 0xfff;
    unsigned char dos[64], nt[24];
    unsigned char *headers = NULL, *sec, *buffer = NULL;
    unsigned int pe_offset, opt_size, section_count, headers_size, i;
    unsigned long long total = 0, max_file = 0, shared_pos = 0;
    struct horizon_memfile *memfile = NULL;
    int shared_fd = -1, pass;

    if (horizon_server_read_exact_at( fd, 0, dos, sizeof(dos) )) return -2;
    pe_offset = horizon_get_le32( dos + 0x3c );
    if (horizon_server_read_exact_at( fd, pe_offset, nt, sizeof(nt) )) return -2;
    section_count = horizon_get_le16( nt + 6 );
    opt_size = horizon_get_le16( nt + 20 );
    headers_size = opt_size + section_count * 40;
    /* horizon_server_read_pe_image_info has already vetted these headers */
    if (!(headers = malloc( headers_size )))
    {
        errno = ENOMEM;
        return -1;
    }
    if (horizon_server_read_exact_at( fd, pe_offset + sizeof(nt), headers, headers_size ))
    {
        free( headers );
        return -2;
    }

    /* The first pass sizes the file, the second fills it. */
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0, sec = headers + opt_size; i < section_count; i++, sec += 40)
        {
            unsigned int virtual_size = horizon_get_le32( sec + 8 );
            unsigned int raw_size = horizon_get_le32( sec + 16 );
            unsigned int raw_ptr = horizon_get_le32( sec + 20 );
            unsigned int charact = horizon_get_le32( sec + 36 );
            unsigned long long map_size, file_size, write_pos, done;
            off_t read_pos;

            if (!(charact & 0x10000000 /* IMAGE_SCN_MEM_SHARED */) ||
                !(charact & 0x80000000 /* IMAGE_SCN_MEM_WRITE */))
                continue;

            /* get_section_sizes in server/mapping.c */
            map_size = ((virtual_size ? virtual_size : raw_size) + align_mask) & ~align_mask;
            read_pos = raw_ptr & ~sector_align;
            file_size = (raw_size + (raw_ptr & sector_align) + sector_align) & ~sector_align;
            if (file_size > map_size) file_size = map_size;

            if (!pass)
            {
                total += map_size;
                if (file_size > max_file) max_file = file_size;
                continue;
            }

            write_pos = shared_pos;
            shared_pos += map_size;
            if (!raw_ptr || !file_size) continue;

            for (done = 0; done < file_size;)
            {
                ssize_t ret = pread( fd, buffer + done, file_size - done, read_pos + done );

                /* a partial sector at the end of the file is not an error */
                if (!ret && file_size - done < 0x200) break;
                if (ret <= 0) goto failed;
                done += ret;
            }
            if (horizon_memfile_pwrite( memfile, (const char *)buffer, done, write_pos ) != (ssize_t)done)
                goto failed;
        }

        if (pass) break;
        if (!total)
        {
            free( headers );
            return -2;
        }
        if ((shared_fd = horizon_memfile_create( total, 0 )) == -1) goto failed;
        if (!(memfile = horizon_memfile_from_fd( shared_fd )) || !(buffer = malloc( max_file ? max_file : 1 )))
        {
            errno = ENOMEM;
            goto failed;
        }
    }

    free( buffer );
    free( headers );
#ifdef __SWITCH__
    horizon_trace( "[HZ] shared sections: %llu bytes in a memfile", total );
#endif
    return shared_fd;

failed:
    {
        int error = errno ? errno : EIO;

        if (shared_fd != -1) close( shared_fd );
        free( buffer );
        free( headers );
        errno = error;
        return -1;
    }
}

static int horizon_server_handle_create_mapping( struct horizon_server_connection *connection,
                                                 const unsigned char *message,
                                                 const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_mapping_request *request = (const void *)message;
    struct horizon_create_mapping_reply reply;
    struct horizon_server_handle_entry *file_entry;
    struct horizon_server_handle_entry *mapping_entry = NULL;
    struct horizon_pe_image_info image_info;
    struct horizon_object_name name;
    unsigned long long mapping_size = request->size;
    unsigned int mapping_flags = request->flags;
    unsigned int file_access = request->file_access;
    char *mapping_name = NULL;
    int fd = -1, shared_fd = -1;
#ifdef __SWITCH__
    int dbg_has_image = -1;
#endif

    memset( &reply, 0, sizeof(reply) );
    memset( &image_info, 0, sizeof(image_info) );

    if ((reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name )))
    {
    }
    else if (!request->file_handle)
    {
        /* wineserver backs a section with no file with a temporary file. Views
         * here are copies read and written back through the descriptor, so
         * memory stands in for that file (horizon_memfile.h). */
        unsigned long long rounded = (mapping_size + 0xfff) & ~0xfffull;

        if (!(mapping_flags = horizon_anonymous_section_flags( request->flags, &reply.header.error )))
        {
        }
        else if (!mapping_size || rounded < mapping_size)
            reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
        else if ((fd = horizon_memfile_create( rounded, !!(mapping_flags & HORIZON_SEC_RESERVE) )) == -1)
            reply.header.error = horizon_server_errno_status( errno );
        else
        {
            mapping_size = rounded;
            file_access = FILE_READ_DATA | FILE_WRITE_DATA;
        }
    }
    else
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        file_entry = horizon_server_find_handle_locked( request->file_handle );
        if (!file_entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
        else if (file_entry->object->type != HORIZON_SERVER_OBJECT_FILE || file_entry->object->file_fd == -1)
            reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
        else if ((fd = dup( file_entry->object->file_fd )) == -1)
            reply.header.error = horizon_server_errno_status( errno );
        else if (file_entry->object->file_name && !(mapping_name = strdup( file_entry->object->file_name )))
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }

    if (!reply.header.error && (request->flags & HORIZON_SEC_IMAGE))
    {
        reply.header.error = horizon_server_read_pe_image_info( fd, &image_info );
        mapping_size = image_info.map_size;
#ifdef __SWITCH__
        horizon_trace( "[HZ] create_mapping flags=0x%x SEC_IMAGE=1 read_pe=0x%x machine=0x%x map_size=0x%x name=%s",
                       request->flags, reply.header.error, image_info.machine,
                       image_info.map_size, mapping_name ? mapping_name : "(null)" );
#endif
    }
    else if (!reply.header.error && !mapping_size)
    {
        struct stat st;

        if (fstat( fd, &st ) == -1) reply.header.error = horizon_server_errno_status( errno );
        else mapping_size = st.st_size;
    }

    if (!reply.header.error && (request->flags & HORIZON_SEC_IMAGE) &&
        (shared_fd = horizon_server_build_shared_mapping( fd, image_info.alignment )) == -1)
        reply.header.error = horizon_server_errno_status( errno );

    if (!reply.header.error)
    {
        unsigned int status = HORIZON_STATUS_SUCCESS;

        pthread_mutex_lock( &horizon_server_objects_mutex );
        if (name.name_len)
            status = horizon_server_create_named_object_handle_locked( HORIZON_SERVER_OBJECT_MAPPING, &name,
                                                                      &mapping_entry );
        else if (!(mapping_entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_MAPPING )))
            status = HORIZON_STATUS_NO_MEMORY;

        if (status == HORIZON_STATUS_SUCCESS)
        {
            mapping_entry->object->file_fd = fd;
            mapping_entry->object->file_name = mapping_name;
            mapping_entry->object->file_access = file_access;
            mapping_entry->object->file_options = 0;
            mapping_entry->object->mapping_flags = mapping_flags;
            mapping_entry->object->mapping_access = request->access;
            mapping_entry->object->mapping_file_access = file_access;
            mapping_entry->object->mapping_size = mapping_size;
            mapping_entry->object->mapping_has_image = !!(request->flags & HORIZON_SEC_IMAGE);
            mapping_entry->object->mapping_image = image_info;
            mapping_entry->object->mapping_memfile = horizon_memfile_from_fd( fd );
            if (shared_fd >= 0)
            {
                struct horizon_server_handle_entry *shared_entry =
                    horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_FILE );

                if (shared_entry)
                {
                    /* No handle of its own: the mapping holds its one reference,
                     * and get_mapping_info hands each caller a handle to it. */
                    horizon_server_unlink_handle_locked( shared_entry );
                    shared_entry->object->file_fd = shared_fd;
                    shared_entry->object->file_access = FILE_READ_DATA | FILE_WRITE_DATA;
                    mapping_entry->object->mapping_shared = shared_entry->object;
                    free( shared_entry );
                    shared_fd = -1;
                }
            }
            reply.handle = mapping_entry->handle;
#ifdef __SWITCH__
            dbg_has_image = mapping_entry->object->mapping_has_image;
#endif
            mapping_name = NULL;
            fd = -1;
        }
        /* As in wineserver, a section that already has the name is returned as it is. */
        else if (mapping_entry) reply.handle = mapping_entry->handle;
        reply.header.error = status;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }

#ifdef __SWITCH__
    horizon_trace( "[HZ] create_mapping done err=0x%x flags=0x%x file=0x%x has_image=%d handle=0x%x",
                   reply.header.error, request->flags, request->file_handle, dbg_has_image, reply.handle );
#endif

    if (fd != -1) close( fd );
    if (shared_fd >= 0) close( shared_fd );
    free( mapping_name );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_mapping_info( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_get_mapping_info_request *request = (const void *)message;
    struct horizon_get_mapping_info_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_pe_image_info image_info;
    unsigned char *data = NULL;
    unsigned int data_size = 0;
    int ret;
#ifdef __SWITCH__
    int dbg_found = 0, dbg_type = -1, dbg_has_image = -1;
    unsigned int dbg_flags = 0, dbg_machine = 0;
#endif

    memset( &reply, 0, sizeof(reply) );
    memset( &image_info, 0, sizeof(image_info) );

    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->handle );
    if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_MAPPING)
        reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else
    {
        reply.size = entry->object->mapping_size;
        reply.flags = entry->object->mapping_flags;
        reply.shared_file = 0;
        if (entry->object->mapping_shared)
        {
            struct horizon_server_handle_entry *shared =
                horizon_server_create_handle_for_object_locked( entry->object->mapping_shared );

            /* the client closes it once the view is mapped (virtual.c) */
            if (shared) reply.shared_file = shared->handle;
        }
#ifdef __SWITCH__
        dbg_found = 1;
        dbg_type = entry->object->type;
        dbg_has_image = entry->object->mapping_has_image;
        dbg_flags = entry->object->mapping_flags;
        dbg_machine = entry->object->mapping_image.machine;
#endif
        if (entry->object->mapping_has_image)
        {
            image_info = entry->object->mapping_image;
            reply.name_len = horizon_server_utf16_name_len( entry->object->file_name );
            data_size = sizeof(image_info) + reply.name_len;
            if ((data = malloc( data_size )))
            {
                memcpy( data, &image_info, sizeof(image_info) );
                horizon_server_write_utf16_name( data + sizeof(image_info), entry->object->file_name );
                reply.total = data_size;
                /* wineserver semantics: never send more reply data than the
                 * client's wine_server_set_reply() buffer. Overrunning it
                 * corrupts the client and desyncs the reply stream. */
                if (request->header.reply_size && data_size > request->header.reply_size)
                    data_size = request->header.reply_size;
                reply.header.reply_size = data_size;
            }
            else
            {
                data_size = 0;
                reply.name_len = 0;
                reply.header.error = HORIZON_STATUS_NO_MEMORY;
            }
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

#ifdef __SWITCH__
    horizon_trace( "[HZ] get_mapping_info found=%d type=%d has_image=%d flags=0x%x machine=0x%x "
                   "req_reply_max=%u sent=%u total=%u reply_size=%u name_len=%u err=0x%x "
                   "sizeof(reply)=%u sizeof(img)=%u",
                   dbg_found, dbg_type, dbg_has_image, dbg_flags, dbg_machine,
                   request->header.reply_size, data_size, reply.total,
                   reply.header.reply_size, reply.name_len,
                   reply.header.error, (unsigned)sizeof(reply), (unsigned)sizeof(image_info) );
#endif

    ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), data, data_size );
    free( data );
    return ret;
}

static int horizon_server_handle_get_image_map_address( struct horizon_server_connection *connection,
                                                        const unsigned char *message )
{
    const struct horizon_get_image_map_address_request *request = (const void *)message;
    struct horizon_get_image_map_address_reply reply;
    struct horizon_server_handle_entry *entry;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->handle );
    if (!entry) reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_MAPPING || !entry->object->mapping_has_image)
        reply.header.error = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else reply.addr = entry->object->mapping_image.map_addr;
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_map_image_view( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_map_image_view_request *request = (const void *)message;
    struct horizon_server_handle_entry *entry;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->mapping );
    if (!entry) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_MAPPING || !entry->object->mapping_has_image)
        status = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else
    {
        entry->object->mapping_image.map_addr = request->base;
        entry->object->mapping_image.base = request->base;
        entry->object->mapping_image.map_size = request->size;
        entry->object->mapping_image.entry_point = request->entry;
        entry->object->mapping_image.machine = request->machine;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_status( connection->reply_fd, status );
}

/* wineserver keeps every view of a mapping; this server keeps the views of
 * sections with no file, whose committed ranges its clients ask about. */
struct horizon_section_view
{
    struct horizon_section_view *next;
    unsigned long long base;
    unsigned long long size;
    unsigned long long start;
    struct horizon_memfile *memfile;  /* referenced */
};

static struct horizon_section_view *horizon_section_views;  /* horizon_server_objects_mutex */

static struct horizon_section_view *horizon_server_find_section_view_locked( unsigned long long base )
{
    struct horizon_section_view *view;

    for (view = horizon_section_views; view; view = view->next)
        if (view->base == base) return view;
    return NULL;
}

static int horizon_server_handle_map_view( struct horizon_server_connection *connection,
                                           const unsigned char *message )
{
    const struct horizon_map_view_request *request = (const void *)message;
    struct horizon_server_handle_entry *entry;
    struct horizon_section_view *view;
    unsigned int status = HORIZON_STATUS_SUCCESS;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    entry = horizon_server_find_handle_locked( request->mapping );
    if (!entry) status = HORIZON_STATUS_INVALID_HANDLE;
    else if (entry->object->type != HORIZON_SERVER_OBJECT_MAPPING)
        status = HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    else if (entry->object->mapping_is_session)
        status = horizon_server_note_session_view_locked( request->base, request->start, request->size );
    else if (entry->object->mapping_memfile)
    {
        const unsigned long long start = request->start, mapping_size = entry->object->mapping_size;

        /* wineserver's map_view checks; the section's size is whole pages. */
        if (request->start < 0 || start >= mapping_size || start + request->size < start ||
            start + request->size > mapping_size)
            status = HORIZON_STATUS_INVALID_PARAMETER;
        else if (!(view = calloc( 1, sizeof(*view) )))
            status = HORIZON_STATUS_NO_MEMORY;
        else
        {
            view->base = request->base;
            view->size = request->size;
            view->start = start;
            view->memfile = entry->object->mapping_memfile;
            horizon_memfile_ref( view->memfile );
            view->next = horizon_section_views;
            horizon_section_views = view;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_unmap_view( struct horizon_server_connection *connection,
                                             const unsigned char *message )
{
    const struct horizon_unmap_view_request *request = (const void *)message;
    struct horizon_section_view **ptr, *view;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    horizon_server_remove_session_view_locked( request->base );
    for (ptr = &horizon_section_views; (view = *ptr); ptr = &view->next)
    {
        if (view->base != request->base) continue;
        *ptr = view->next;
        horizon_memfile_unref( view->memfile );
        free( view );
        break;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_SUCCESS );
}

/* Which pages of a SEC_RESERVE section's view are committed: Wine sizes
 * VirtualQuery regions and checks VirtualProtect ranges with this. */
static int horizon_server_handle_get_mapping_committed_range( struct horizon_server_connection *connection,
                                                              const unsigned char *message )
{
    const struct horizon_get_mapping_committed_range_request *request = (const void *)message;
    struct horizon_get_mapping_committed_range_reply reply;
    struct horizon_section_view *view;
    unsigned long long size = 0;
    int committed = 0;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(view = horizon_server_find_section_view_locked( request->base )))
        reply.header.error = HORIZON_STATUS_NOT_MAPPED_VIEW;
    else if (!(reply.header.error = horizon_memfile_committed_range( view->memfile, view->start, view->size,
                                                                     request->offset, &size, &committed )))
    {
        reply.size = size;
        reply.committed = committed;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_add_mapping_committed_range( struct horizon_server_connection *connection,
                                                              const unsigned char *message )
{
    const struct horizon_add_mapping_committed_range_request *request = (const void *)message;
    struct horizon_section_view *view;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(view = horizon_server_find_section_view_locked( request->base )))
        status = HORIZON_STATUS_NOT_MAPPED_VIEW;
    else
        status = horizon_memfile_add_committed( view->memfile, view->start, view->size,
                                                request->offset, request->size );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

#include "horizon_registry_server.h"

static int horizon_server_handle_create_event( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_event_request *request = (const void *)message;
    struct horizon_create_event_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_EVENT, &name, &entry );
        if (entry)
        {
            if (reply.header.error == HORIZON_STATUS_SUCCESS)
            {
                entry->object->manual_reset = !!request->manual_reset;
                entry->object->signaled = !!request->initial_state;
            }
            reply.handle = entry->handle;
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_event_op( struct horizon_server_connection *connection,
                                           const unsigned char *message )
{
    const struct horizon_event_op_request *request = (const void *)message;
    struct horizon_event_op_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_EVENT, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.state = object->signaled;
        switch (request->op)
        {
        case HORIZON_PULSE_EVENT:
            object->signaled = 0;
            break;
        case HORIZON_SET_EVENT:
            /* Waiters need waking only when it becomes signaled. */
            if (!object->signaled)
            {
                object->signaled = 1;
                horizon_server_signal_changed_locked();
            }
            break;
        case HORIZON_RESET_EVENT:
            object->signaled = 0;
            break;
        default:
            status = HORIZON_STATUS_INVALID_PARAMETER;
            break;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_query_event( struct horizon_server_connection *connection,
                                              const unsigned char *message )
{
    const struct horizon_query_event_request *request = (const void *)message;
    struct horizon_query_event_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_EVENT, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.manual_reset = object->manual_reset;
        reply.state = object->signaled;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* I/O completion ports (horizon_completion.h, after server/completion.c). A
 * client thread waits on its own completion wait object: remove_completion
 * hands out that object's handle when the port's queue is empty, the client's
 * select takes the next message into it, and get_thread_completion returns
 * the message. */
static void horizon_server_post_completion_locked( struct horizon_server_object *port, unsigned long long ckey,
                                                   unsigned long long cvalue, unsigned int status,
                                                   unsigned long long information )
{
    if (horizon_completion_add( &port->completion, ckey, cvalue, status, information ))
        horizon_server_signal_changed_locked();
}

/* The client's completion wait object, created with a handle at its first wait. */
static struct horizon_server_object *horizon_server_completion_wait_locked( struct horizon_server_connection *connection )
{
    struct horizon_server_handle_entry *entry;

    if (connection->completion_wait) return connection->completion_wait;
    if (!(entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_COMPLETION_WAIT ))) return NULL;
    entry->object->refs++;  /* the connection's */
    connection->completion_wait = entry->object;
    connection->completion_wait_handle = entry->handle;
    return entry->object;
}

static void horizon_server_bind_completion_wait_locked( struct horizon_server_object *wait,
                                                        struct horizon_server_object *port )
{
    if (wait->wait_port == port) return;
    port->refs++;
    if (wait->wait_port && !--wait->wait_port->refs) horizon_server_free_object( wait->wait_port );
    wait->wait_port = port;
}

static int horizon_server_handle_create_completion( struct horizon_server_connection *connection,
                                                    const unsigned char *data, unsigned int data_size )
{
    struct horizon_create_completion_reply reply;
    struct horizon_server_handle_entry *entry = NULL;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_COMPLETION, &name, &entry );
        if (entry) reply.handle = entry->handle;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    horizon_trace( "[HZIOCP] create_completion -> handle=0x%x err=0x%x", reply.handle, reply.header.error );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_add_completion( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_add_completion_request *request = (const void *)message;
    struct horizon_server_object *port = NULL;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_COMPLETION, &port );
    if (status == HORIZON_STATUS_SUCCESS)
        horizon_server_post_completion_locked( port, request->ckey, request->cvalue, request->status,
                                               request->information );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_remove_completion( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_remove_completion_request *request = (const void *)message;
    struct horizon_remove_completion_reply reply;
    struct horizon_server_object *port = NULL, *wait;
    struct horizon_completion_msg msg;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_find_typed_object_locked( request->handle,
                                                                  HORIZON_SERVER_OBJECT_COMPLETION, &port );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        if (horizon_completion_take( &port->completion, &msg ))
        {
            reply.ckey = msg.ckey;
            reply.cvalue = msg.cvalue;
            reply.information = msg.information;
            reply.status = msg.status;
            if (connection->completion_wait)
                horizon_server_bind_completion_wait_locked( connection->completion_wait, port );
        }
        else if (!(wait = horizon_server_completion_wait_locked( connection )))
            reply.header.error = HORIZON_STATUS_NO_MEMORY;
        else
        {
            horizon_server_bind_completion_wait_locked( wait, port );
            reply.wait_handle = connection->completion_wait_handle;
            reply.header.error = HORIZON_STATUS_PENDING;
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_thread_completion( struct horizon_server_connection *connection )
{
    struct horizon_get_thread_completion_reply reply;
    struct horizon_server_object *wait;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (!(wait = connection->completion_wait) || !wait->wait_has_msg)
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else
    {
        reply.ckey = wait->wait_msg.ckey;
        reply.cvalue = wait->wait_msg.cvalue;
        reply.information = wait->wait_msg.information;
        reply.status = wait->wait_msg.status;
        wait->wait_has_msg = 0;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_query_completion( struct horizon_server_connection *connection,
                                                   const unsigned char *message )
{
    const struct horizon_query_completion_request *request = (const void *)message;
    struct horizon_query_completion_reply reply;
    struct horizon_server_object *port = NULL;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    reply.header.error = horizon_server_find_typed_object_locked( request->handle,
                                                                  HORIZON_SERVER_OBJECT_COMPLETION, &port );
    if (reply.header.error == HORIZON_STATUS_SUCCESS) reply.depth = port->completion.depth;
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* Files and sockets are what I/O completion ports are tied to. */
static unsigned int horizon_server_find_io_object_locked( unsigned int handle, struct horizon_server_object **object )
{
    unsigned int status = horizon_server_find_typed_object_locked( handle, HORIZON_SERVER_OBJECT_FILE, object );

    if (status == HORIZON_STATUS_OBJECT_TYPE_MISMATCH)
        status = horizon_server_find_typed_object_locked( handle, HORIZON_SERVER_OBJECT_SOCK, object );
    return status;
}

/* server/fd.c's set_completion_info, add_fd_completion and
 * set_fd_completion_mode, for files and sockets. */
static int horizon_server_handle_set_completion_info( struct horizon_server_connection *connection,
                                                      const unsigned char *message )
{
    const struct horizon_set_completion_info_request *request = (const void *)message;
    struct horizon_server_object *file = NULL, *port = NULL;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_io_object_locked( request->handle, &file );
    if (status == HORIZON_STATUS_SUCCESS &&
        (!horizon_completion_file_overlapped( file->file_options ) || file->file_completion))
        status = HORIZON_STATUS_INVALID_PARAMETER;
    if (status == HORIZON_STATUS_SUCCESS)
        status = horizon_server_find_typed_object_locked( request->chandle, HORIZON_SERVER_OBJECT_COMPLETION, &port );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        port->refs++;
        file->file_completion = port;
        file->file_completion_key = request->ckey;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_trace( "[HZIOCP] set_completion_info handle=0x%x port=0x%x key=0x%llx -> 0x%x",
                   request->handle, request->chandle, (unsigned long long)request->ckey, status );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_add_fd_completion( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_add_fd_completion_request *request = (const void *)message;
    struct horizon_server_object *file = NULL;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_io_object_locked( request->handle, &file );
    if (status == HORIZON_STATUS_SUCCESS && file->file_completion &&
        horizon_completion_file_posts( request->async, file->file_completion_flags ))
        horizon_server_post_completion_locked( file->file_completion, file->file_completion_key,
                                               request->cvalue, request->status, request->information );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_set_fd_completion_mode( struct horizon_server_connection *connection,
                                                         const unsigned char *message )
{
    const struct horizon_set_fd_completion_mode_request *request = (const void *)message;
    struct horizon_server_object *file = NULL;
    unsigned int status;

    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_io_object_locked( request->handle, &file );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        if (horizon_completion_file_overlapped( file->file_options ))
            file->file_completion_flags = horizon_completion_file_mode( file->file_completion_flags,
                                                                        request->flags );
        else status = HORIZON_STATUS_INVALID_PARAMETER;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    return horizon_server_write_status( connection->reply_fd, status );
}

static int horizon_server_handle_create_keyed_event( struct horizon_server_connection *connection,
                                                     const unsigned char *data, unsigned int data_size )
{
    struct horizon_open_process_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_KEYED_EVENT, &name, &entry );
        if (entry) reply.handle = entry->handle;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_create_mutex( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_mutex_request *request = (const void *)message;
    struct horizon_create_mutex_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_MUTEX, &name, &entry );
        if (entry)
        {
            if (reply.header.error == HORIZON_STATUS_SUCCESS && request->owned)
                horizon_mutex_acquire( &entry->object->mutex, connection->tid );
            reply.handle = entry->handle;
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_release_mutex( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct horizon_release_mutex_request *request = (const void *)message;
    struct horizon_release_mutex_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_MUTEX, &object );
    if (status == HORIZON_STATUS_SUCCESS)
        status = horizon_mutex_release( &object->mutex, connection->tid, &reply.prev_count );
    /* Only a release that frees the mutex lets a waiter take it. */
    if (status == HORIZON_STATUS_SUCCESS && !object->mutex.count) horizon_server_signal_changed_locked();
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_query_mutex( struct horizon_server_connection *connection,
                                              const unsigned char *message )
{
    const struct horizon_query_mutex_request *request = (const void *)message;
    struct horizon_query_mutex_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_MUTEX, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.count = object->mutex.count;
        reply.owned = object->mutex.count && object->mutex.owner == connection->tid;
        reply.abandoned = object->mutex.abandoned;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_create_semaphore( struct horizon_server_connection *connection,
                                                   const unsigned char *message,
                                                   const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_semaphore_request *request = (const void *)message;
    struct horizon_create_semaphore_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    if (!request->max || request->initial > request->max)
        reply.header.error = HORIZON_STATUS_INVALID_PARAMETER;
    else if ((reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name )))
    {
    }
    else
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_SEMAPHORE, &name, &entry );
        if (entry)
        {
            if (reply.header.error == HORIZON_STATUS_SUCCESS)
            {
                entry->object->count = request->initial;
                entry->object->max = request->max;
            }
            reply.handle = entry->handle;
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_release_semaphore( struct horizon_server_connection *connection,
                                                    const unsigned char *message )
{
    const struct horizon_release_semaphore_request *request = (const void *)message;
    struct horizon_release_semaphore_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_SEMAPHORE, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        if (!request->count) status = HORIZON_STATUS_INVALID_PARAMETER;
        else if (request->count > object->max || object->count > object->max - request->count)
            status = HORIZON_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        else
        {
            reply.prev_count = object->count;
            object->count += request->count;
            horizon_server_signal_changed_locked();
        }
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_query_semaphore( struct horizon_server_connection *connection,
                                                  const unsigned char *message )
{
    const struct horizon_query_semaphore_request *request = (const void *)message;
    struct horizon_query_semaphore_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_SEMAPHORE, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.current = object->count;
        reply.max = object->max;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_create_timer( struct horizon_server_connection *connection,
                                               const unsigned char *message,
                                               const unsigned char *data, unsigned int data_size )
{
    const struct horizon_create_timer_request *request = (const void *)message;
    struct horizon_create_timer_reply reply;
    struct horizon_server_handle_entry *entry;
    struct horizon_object_name name;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = horizon_server_parse_object_attributes( data, data_size, &name );
    if (reply.header.error == HORIZON_STATUS_SUCCESS)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        reply.header.error = horizon_server_create_named_object_handle_locked(
            HORIZON_SERVER_OBJECT_TIMER, &name, &entry );
        if (entry)
        {
            if (reply.header.error == HORIZON_STATUS_SUCCESS)
            {
                entry->object->manual_reset = !!request->manual;
                entry->object->signaled = 0;
                entry->object->timer_when = 0;
                entry->object->timer_period = 0;
            }
            reply.handle = entry->handle;
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

/* Identifiers that are unique for as long as this process runs, which is what
 * the name promises. wined3d asks for one before it will create an adapter, so
 * Direct3D does not start without this; the wineserver counts up the same way. */
static int horizon_server_handle_allocate_locally_unique_id( struct horizon_server_connection *connection )
{
    static LONG last_luid;
    struct horizon_allocate_locally_unique_id_reply reply;

    memset( &reply, 0, sizeof(reply) );
    reply.header.error = HORIZON_STATUS_SUCCESS;
    reply.low_part = (unsigned int)InterlockedIncrement( &last_luid );
    reply.high_part = 0;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_set_timer( struct horizon_server_connection *connection,
                                            const unsigned char *message )
{
    const struct horizon_set_timer_request *request = (const void *)message;
    struct horizon_set_timer_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_TIMER, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        LARGE_INTEGER now;

        /* A waitable timer is signalled when it expires, not when it is set.
         * expire is an absolute time, or a delay when it is not positive, as
         * the wineserver's set_timer takes it. */
        NtQuerySystemTime( &now );
        reply.signaled = object->signaled;
        object->timer_when = request->expire <= 0 ? now.QuadPart - request->expire
                                                  : max( request->expire, now.QuadPart );
        object->timer_period = request->period > 0 ? request->period : 0;
        object->signaled = 0;
        horizon_server_timers_armed = 1;
        horizon_server_signal_changed_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_cancel_timer( struct horizon_server_connection *connection,
                                               const unsigned char *message )
{
    const struct horizon_cancel_timer_request *request = (const void *)message;
    struct horizon_cancel_timer_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_TIMER, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.signaled = object->signaled;
        object->signaled = 0;
        object->timer_when = 0;
        object->timer_period = 0;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_handle_get_timer_info( struct horizon_server_connection *connection,
                                                 const unsigned char *message )
{
    const struct horizon_get_timer_info_request *request = (const void *)message;
    struct horizon_get_timer_info_reply reply;
    struct horizon_server_object *object = NULL;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    horizon_server_update_timers_locked();
    status = horizon_server_find_typed_object_locked( request->handle, HORIZON_SERVER_OBJECT_TIMER, &object );
    if (status == HORIZON_STATUS_SUCCESS)
    {
        reply.when = object->timer_when;
        reply.signaled = object->signaled;
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );

    reply.header.error = status;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static unsigned int horizon_server_select_wait( const struct horizon_select_wait_op *op,
                                                unsigned int size, int wait_all )
{
    unsigned int count;
    unsigned int i;
    unsigned int status = HORIZON_STATUS_TIMEOUT;

    if (size < offsetof( struct horizon_select_wait_op, handles[1] ))
        return HORIZON_STATUS_INVALID_PARAMETER;

    count = (size - offsetof( struct horizon_select_wait_op, handles )) / sizeof(op->handles[0]);
    if (!count) return HORIZON_STATUS_INVALID_PARAMETER;

    if (wait_all)
    {
        status = HORIZON_STATUS_SUCCESS;
        for (i = 0; i < count; i++)
        {
            status = horizon_server_wait_object_locked( op->handles[i], FALSE );
            if (status != HORIZON_STATUS_SUCCESS) break;
        }
        if (status == HORIZON_STATUS_SUCCESS)
            for (i = 0; i < count; i++)
                if (horizon_server_wait_object_locked( op->handles[i], TRUE ) == HORIZON_STATUS_ABANDONED_WAIT_0)
                    status = HORIZON_STATUS_ABANDONED_WAIT_0;
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            status = horizon_server_wait_object_locked( op->handles[i], TRUE );
            if (status == HORIZON_STATUS_SUCCESS || status == HORIZON_STATUS_ABANDONED_WAIT_0)
            {
                status += i;
                break;
            }
            if (status != HORIZON_STATUS_TIMEOUT) break;
        }
    }
    return status;
}

static unsigned int horizon_server_select_signal_and_wait( const struct horizon_select_signal_and_wait_op *op,
                                                           unsigned int size, int initial )
{
    unsigned int status;

    if (size < sizeof(*op)) return HORIZON_STATUS_INVALID_PARAMETER;

    status = initial ? horizon_server_signal_object_locked( op->signal ) : HORIZON_STATUS_SUCCESS;
    if (status == HORIZON_STATUS_SUCCESS)
        status = horizon_server_wait_object_locked( op->wait, TRUE );
    return status;
}

static unsigned int horizon_server_select_status( const struct horizon_select_request *request,
                                                  const unsigned char *data, unsigned int data_size, int initial )
{
    const unsigned char *select_data = NULL;
    int op;

    if (!request->size) return HORIZON_STATUS_TIMEOUT;
    if (data_size >= HORIZON_APC_RESULT_SIZE + request->size)
        select_data = data + HORIZON_APC_RESULT_SIZE;
    else if (data_size >= request->size)
        select_data = data;
    else return HORIZON_STATUS_INVALID_PARAMETER;

    if (request->size < sizeof(op)) return HORIZON_STATUS_INVALID_PARAMETER;
    memcpy( &op, select_data, sizeof(op) );

    switch (op)
    {
    case HORIZON_SELECT_WAIT:
        return horizon_server_select_wait( (const struct horizon_select_wait_op *)select_data,
                                           request->size, FALSE );
    case HORIZON_SELECT_WAIT_ALL:
        return horizon_server_select_wait( (const struct horizon_select_wait_op *)select_data,
                                           request->size, TRUE );
    case HORIZON_SELECT_SIGNAL_AND_WAIT:
        return horizon_server_select_signal_and_wait(
            (const struct horizon_select_signal_and_wait_op *)select_data, request->size, initial );
    case HORIZON_SELECT_KEYED_EVENT_WAIT:
    case HORIZON_SELECT_KEYED_EVENT_RELEASE:
        return HORIZON_STATUS_SUCCESS;
    default:
        return HORIZON_STATUS_SUCCESS;
    }
}

static int horizon_server_handle_polls_locked( unsigned int handle )
{
    struct horizon_server_handle_entry *entry;

    if (!handle || handle == HORIZON_CURRENT_THREAD_HANDLE) return 0;
    if (!(entry = horizon_server_find_handle_locked( handle ))) return 0;
    /* Neither a message queue nor a timer that is running signals the waiter
     * when it becomes ready: both are noticed by looking again. */
    return entry->object->type == HORIZON_SERVER_OBJECT_MSG_QUEUE ||
           (entry->object->type == HORIZON_SERVER_OBJECT_TIMER && entry->object->timer_when);
}

/* Whether a select waits on a message queue, which can become signaled without
 * a wakeup. Called with horizon_server_objects_mutex held. */
static int horizon_server_select_polls_locked( const struct horizon_select_request *request,
                                               const unsigned char *data, unsigned int data_size )
{
    const unsigned char *select_data;
    unsigned int count, i;
    int op;

    if (request->size < sizeof(op)) return 0;
    if (data_size >= HORIZON_APC_RESULT_SIZE + request->size)
        select_data = data + HORIZON_APC_RESULT_SIZE;
    else if (data_size >= request->size)
        select_data = data;
    else return 0;
    memcpy( &op, select_data, sizeof(op) );

    switch (op)
    {
    case HORIZON_SELECT_WAIT:
    case HORIZON_SELECT_WAIT_ALL:
    {
        const struct horizon_select_wait_op *wait = (const void *)select_data;

        if (request->size < offsetof( struct horizon_select_wait_op, handles[1] )) return 0;
        count = (request->size - offsetof( struct horizon_select_wait_op, handles )) / sizeof(wait->handles[0]);
        for (i = 0; i < count; i++)
            if (horizon_server_handle_polls_locked( wait->handles[i] )) return 1;
        return 0;
    }
    case HORIZON_SELECT_SIGNAL_AND_WAIT:
        if (request->size < sizeof(struct horizon_select_signal_and_wait_op)) return 0;
        return horizon_server_handle_polls_locked(
            ((const struct horizon_select_signal_and_wait_op *)select_data)->wait );
    default:
        return 0;
    }
}

/* Whether a program's completion routines are being called at all: a handful of
 * lines, since a program that uses them uses them for everything it reads. */
static void horizon_report_user_apc( const char *what, unsigned int tid, unsigned int size, unsigned int status )
{
    static LONG reported;
    char message[192];

    if (__atomic_add_fetch( &reported, 1, __ATOMIC_RELAXED ) > 8) return;
    snprintf( message, sizeof(message), "[APC] %s %04x, %u bytes, status %08x", what, tid, size, status );
    wine_nx_runtime_trace( message );
}

/* Windows runs a thread's user APCs when it waits alertably, oldest first, and
 * the wait ends with STATUS_USER_APC rather than performing the wait: that is
 * how ReadFileEx's completion routine is called and how SleepEx returns
 * WAIT_IO_COMPLETION. The caller holds horizon_server_objects_mutex. */
static unsigned int horizon_server_queue_user_apc_locked( struct horizon_server_object *thread,
                                                          const unsigned char *call, unsigned int size )
{
    struct horizon_user_apc *apc;

    if (!size || size > HORIZON_USER_APC_MAX) return HORIZON_STATUS_INVALID_PARAMETER;
    if (!(apc = calloc( 1, offsetof( struct horizon_user_apc, call[size] ) ))) return HORIZON_STATUS_NO_MEMORY;
    memcpy( apc->call, call, size );
    apc->size = size;
    if (thread->apc_last) thread->apc_last->next = apc;
    else thread->apc_first = apc;
    thread->apc_last = apc;
    /* The thread may already be asleep in a wait of its own. */
    horizon_server_signal_changed_locked();
    return HORIZON_STATUS_SUCCESS;
}

static struct horizon_user_apc *horizon_server_take_user_apc_locked( struct horizon_server_object *thread )
{
    struct horizon_user_apc *apc;

    if (!thread || !(apc = thread->apc_first)) return NULL;
    if (!(thread->apc_first = apc->next)) thread->apc_last = NULL;
    apc->next = NULL;
    return apc;
}

static int horizon_server_handle_queue_apc( struct horizon_server_connection *connection,
                                            const unsigned char *message,
                                            const unsigned char *data, unsigned int data_size )
{
    const struct horizon_queue_apc_request *request = (const void *)message;
    struct horizon_queue_apc_reply reply;
    struct horizon_server_object *thread;
    unsigned int status;

    memset( &reply, 0, sizeof(reply) );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((thread = horizon_server_get_thread_locked( request->handle, &status )))
    {
        /* A queue_apc with no call asks only whether the thread is this one. */
        if (data_size) status = horizon_server_queue_user_apc_locked( thread, data, data_size );
        reply.self = connection->thread == thread;
    }
    horizon_report_user_apc( "queued for thread", thread ? thread->thread.tid : 0, data_size, status );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.header.error = status;
    /* Only a system APC is answered with a handle to collect a result from. */
    reply.handle = 0;
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static int horizon_server_select_signals( const struct horizon_select_request *request,
                                          const unsigned char *data, unsigned int data_size )
{
    int op;

    if (request->size < sizeof(op)) return 0;
    if (data_size >= HORIZON_APC_RESULT_SIZE + request->size) data += HORIZON_APC_RESULT_SIZE;
    else if (data_size < request->size) return 0;
    memcpy( &op, data, sizeof(op) );
    return op == HORIZON_SELECT_SIGNAL_AND_WAIT;
}

/* server/async.c's async_set_result: how an operation on a socket that has
 * ended is told -- its completion routine as a user APC on the thread that
 * started it, or a packet on the socket's port, and its event. */
static void horizon_async_finish_locked( struct horizon_async *async, unsigned int status,
                                         unsigned long long total )
{
    struct horizon_server_object *port = async->port, *thread;
    struct horizon_server_handle_entry *event;
    unsigned int actions;

    actions = horizon_async_completion( async, HORIZON_NT_ERROR( status ), port != NULL,
                                        async->port_flags & HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS );
    if (actions & HORIZON_ASYNC_APC)
    {
        unsigned char call[HORIZON_APC_CALL_SIZE];
        unsigned int type = HORIZON_APC_USER;

        /* union apc_call's user: func, then apc_context, the status block, 0. */
        memset( call, 0, sizeof(call) );
        memcpy( call, &type, sizeof(type) );
        memcpy( call + 8, &async->data.apc, 8 );
        memcpy( call + 16, &async->data.apc_context, 8 );
        memcpy( call + 24, &async->data.iosb, 8 );
        for (thread = horizon_server_threads; thread; thread = thread->thread_next)
            if (thread->thread.tid == async->owner_tid) break;
        if (thread) horizon_server_queue_user_apc_locked( thread, call, sizeof(call) );
    }
    if (actions & HORIZON_ASYNC_POST)
        horizon_server_post_completion_locked( port, async->port_key, async->data.apc_context, status, total );
    if ((actions & HORIZON_ASYNC_EVENT) && (event = horizon_server_find_handle_locked( async->data.event )) &&
        event->object->type == HORIZON_SERVER_OBJECT_EVENT && !event->object->signaled)
    {
        event->object->signaled = 1;
        horizon_server_signal_changed_locked();
    }
    if (async->pending) horizon_report_async( "done", async, status );
}

/* What the client's callback made of an operation it ran as a system APC,
 * which comes back with its next select: not ready after all, and it waits
 * for the socket again, or ended. */
static void horizon_server_async_result_locked( const struct horizon_select_request *request,
                                                const unsigned char *data, unsigned int data_size )
{
    struct horizon_async *async;
    unsigned int status, total;

    if (!request->prev_apc || !(async = horizon_async_find_apc( &horizon_asyncs, request->prev_apc ))) return;
    async->apc_id = 0;
    status = HORIZON_STATUS_PENDING;
    total = 0;
    if (data_size >= HORIZON_APC_RESULT_SIZE + request->size)
    {
        memcpy( &status, data + 4, sizeof(status) );  /* union apc_result's async_io */
        memcpy( &total, data + 8, sizeof(total) );
    }
    if (status == HORIZON_STATUS_PENDING)
    {
        async->state = HORIZON_ASYNC_QUEUED;
        async->status = HORIZON_STATUS_ALERTED;
        return;
    }
    horizon_async_remove( &horizon_asyncs, async );
    horizon_async_finish_locked( async, status, total );
    horizon_server_async_free_locked( async );
}

/* An operation ready for this thread to run, as server/async.c's
 * async_terminate hands it over: APC_ASYNC_IO with ALERTED to do it now (or
 * fetch what an accept left), or how it ended. */
static unsigned int horizon_server_async_apc_locked( struct horizon_server_connection *connection,
                                                     unsigned char *call )
{
    struct horizon_async *async;
    unsigned int type = HORIZON_APC_ASYNC_IO, result;

    if (!horizon_asyncs.head ||
        !(async = horizon_async_ready_for( &horizon_asyncs, connection->tid, horizon_async_now(),
                                           HORIZON_ASYNC_STALE )))
        return 0;
    if (!++horizon_async_apc_ids) ++horizon_async_apc_ids;
    horizon_async_run( &horizon_asyncs, async, horizon_async_apc_ids );
    result = async->status == HORIZON_STATUS_ALERTED ? async->out_info : 0;
    memset( call, 0, HORIZON_APC_CALL_SIZE );
    memcpy( call, &type, sizeof(type) );
    memcpy( call + 4, &async->status, sizeof(async->status) );
    memcpy( call + 8, &async->data.user, 8 );
    memcpy( call + 16, &async->data.iosb, 8 );
    memcpy( call + 24, &result, sizeof(result) );
    return async->apc_id;
}

static int horizon_server_handle_select( struct horizon_server_connection *connection,
                                         const unsigned char *message,
                                         const unsigned char *data, unsigned int data_size )
{
    const struct horizon_select_request *request = (const void *)message;
    unsigned char system_call[HORIZON_APC_CALL_SIZE];
    struct horizon_user_apc *apc = NULL;
    struct horizon_select_reply reply;
    int polls, signals, ret;

    memset( &reply, 0, sizeof(reply) );
    /* Each client has its own server connection/thread. A pending wait sleeps on
     * horizon_server_objects_cond, which releases the object lock so other
     * clients can signal objects meanwhile and wake it
     * (horizon_server_signal_changed_locked). Message queues also change without
     * that wakeup (window timers), so waits on them recheck every millisecond.
     * Negative server deadlines are absolute performance-counter ticks (100ns),
     * positive deadlines use NT wall-clock time; INT64_MAX means infinite.
     * Signal-and-wait must perform its signal only on the first attempt. */
    pthread_mutex_lock( &horizon_server_objects_mutex );
    horizon_server_async_result_locked( request, data, data_size );
    polls = horizon_server_select_polls_locked( request, data, data_size );
    signals = horizon_server_select_signals( request, data, data_size );
    for (int initial = 1;; initial = 0)
    {
        LARGE_INTEGER now;
        long long timeout = HORIZON_SERVER_WAIT_SLICE;

        /* A system APC comes first, in any wait: the socket operation the
         * thread started is ready to be done. Not before a signal-and-wait
         * has signalled, which the client leaves out when it waits again. */
        if (!(initial && signals) &&
            (reply.apc_handle = horizon_server_async_apc_locked( connection, system_call )))
        {
            reply.header.error = HORIZON_STATUS_KERNEL_APC;
            break;
        }

        /* Before the wait itself, as Windows does: an APC that arrived while
         * the thread ran is due the moment it waits alertably. */
        if ((request->flags & HORIZON_SELECT_ALERTABLE) &&
            (apc = horizon_server_take_user_apc_locked( connection->thread )))
        {
            reply.header.error = HORIZON_STATUS_USER_APC;
            horizon_report_user_apc( "run by thread", connection->thread->thread.tid, apc->size, 0 );
            break;
        }
        horizon_server_update_timers_locked();
        reply.header.error = horizon_server_select_status( request, data, data_size, initial );
        if (reply.header.error != HORIZON_STATUS_TIMEOUT || !request->timeout) break;
        if (request->timeout != 0x7fffffffffffffffLL)
        {
            if (request->timeout < 0)
            {
                NtQueryPerformanceCounter( &now, NULL );
                if (now.QuadPart > -(request->timeout + 1)) break;
                timeout = -(request->timeout + 1) - now.QuadPart + 1;
            }
            else
            {
                NtQuerySystemTime( &now );
                if (now.QuadPart >= request->timeout) break;
                timeout = request->timeout - now.QuadPart;
            }
        }
        if (polls && timeout > HORIZON_SERVER_POLL_INTERVAL) timeout = HORIZON_SERVER_POLL_INTERVAL;
        horizon_server_sleep_locked( timeout );
        horizon_server_quit_check_locked();
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    reply.signaled = 1;

    TRACE( "Horizon server select size %u timeout %lld status %08x.\n",
           request->size, request->timeout, reply.header.error );
    if (reply.apc_handle)
    {
        unsigned int size = min( (unsigned int)sizeof(system_call), request->header.reply_size );

        reply.header.reply_size = size;
        return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), system_call, size );
    }
    if (apc)
    {
        unsigned int size = min( apc->size, request->header.reply_size );

        reply.header.reply_size = size;
        ret = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), apc->call, size );
        free( apc );
        return ret;
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), NULL, 0 );
}

static void *horizon_server_thread( void *param )
{
    struct horizon_server_connection *connection = param;
    unsigned char message[HORIZON_SERVER_FIXED_MESSAGE_SIZE];
    struct horizon_zombie *zombie;

    horizon_server_current = connection;
    connection->request_pipe = horizon_pipe_from_fd( connection->request_fd );
    if (wine_nx_thread_register) wine_nx_thread_register( 's', connection->tid, NULL );
    /* A client blocks until this thread replies. At the program threads'
     * priority (59) it waited for a round-robin slice behind whatever else ran
     * on the core: 0.1-0.9 ms per request in NFSU2. Above them, below the audio
     * feeder (56), it answers as soon as the client blocks. */
    svcSetThreadPriority( CUR_THREAD_HANDLE, 0x39 );
    for (;;)
    {
        {
            extern volatile int wine_nx_quit_requested __attribute__((weak));
            extern void wine_nx_quit_point( void ) __attribute__((weak));

            if (&wine_nx_quit_requested && wine_nx_quit_requested && &wine_nx_quit_point) wine_nx_quit_point();
        }
        struct horizon_server_request_header *header = (void *)message;
        unsigned char *request_data = NULL;
        int status = 0;
        int ret = horizon_read_exact( connection->request_fd, message, sizeof(message) );

        if (ret <= 0) break;
        horizon_server_follow_client( connection );
        if (header->request_size)
        {
            if (!(request_data = malloc( header->request_size )))
            {
                if (connection->reply_fd != -1)
                    status = horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_NO_MEMORY );
                if (status) goto done;
                continue;
            }
            if (horizon_read_exact( connection->request_fd, request_data, header->request_size ) <= 0)
            {
                free( request_data );
                break;
            }
        }

        switch (header->req)
        {
        case HORIZON_REQ_GET_TOKEN_SID:
            status = horizon_server_handle_registry_user( connection, message );
            break;
        case HORIZON_REQ_ALLOCATE_LOCALLY_UNIQUE_ID:
            status = horizon_server_handle_allocate_locally_unique_id( connection );
            break;
        case HORIZON_REQ_CREATE_KEY:
        case HORIZON_REQ_OPEN_KEY:
        case HORIZON_REQ_DELETE_KEY:
        case HORIZON_REQ_ENUM_KEY:
        case HORIZON_REQ_SET_KEY_VALUE:
        case HORIZON_REQ_GET_KEY_VALUE:
        case HORIZON_REQ_ENUM_KEY_VALUE:
        case HORIZON_REQ_DELETE_KEY_VALUE:
        case HORIZON_REQ_SET_REGISTRY_NOTIFICATION:
        case HORIZON_REQ_RENAME_KEY:
            status = horizon_server_handle_registry( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_INIT_FIRST_THREAD:
            status = horizon_server_handle_init_first_thread( connection, message );
            break;
        case HORIZON_REQ_INIT_PROCESS_DONE:
            status = horizon_server_handle_init_process_done( connection, message );
            break;
        case HORIZON_REQ_INIT_THREAD:
            status = horizon_server_handle_init_thread( connection, message );
            break;
        case HORIZON_REQ_NEW_THREAD:
            status = horizon_server_handle_new_thread( connection, message );
            break;
        case HORIZON_REQ_SUSPEND_THREAD:
            status = horizon_server_handle_suspend_thread( connection, message );
            break;
        case HORIZON_REQ_RESUME_THREAD:
            status = horizon_server_handle_resume_thread( connection, message );
            break;
        case HORIZON_REQ_TERMINATE_THREAD:
            status = horizon_server_handle_terminate_thread( connection, message );
            break;
        case HORIZON_REQ_GET_THREAD_INFO:
            status = horizon_server_handle_get_thread_info( connection, message );
            break;
        case HORIZON_REQ_GET_THREAD_TIMES:
            status = horizon_server_handle_get_thread_times( connection, message );
            break;
        case HORIZON_REQ_SET_THREAD_INFO:
            status = horizon_server_handle_set_thread_info( connection, message );
            break;
        case HORIZON_REQ_OPEN_FILE_OBJECT:
            status = horizon_server_handle_open_file_object( connection, message, request_data,
                                                             header->request_size );
            break;
        case HORIZON_REQ_IOCTL:
            status = horizon_server_handle_ioctl( connection, message, request_data,
                                                  header->request_size );
            break;
        case HORIZON_REQ_RECV_SOCKET:
        case HORIZON_REQ_SEND_SOCKET:
            status = horizon_server_handle_socket_io( connection, message );
            break;
        case HORIZON_REQ_SOCKET_GET_EVENTS:
            status = horizon_server_handle_socket_get_events( connection, message );
            break;
        case HORIZON_REQ_SET_ASYNC_DIRECT_RESULT:
            status = horizon_server_handle_set_async_direct_result( connection, message );
            break;
        case HORIZON_REQ_GET_ASYNC_RESULT:
            status = horizon_server_handle_get_async_result( connection, message );
            break;
        case HORIZON_REQ_CANCEL_ASYNC:
            status = horizon_server_handle_cancel_async( connection, message );
            break;
        case HORIZON_REQ_QUEUE_APC:
            status = horizon_server_handle_queue_apc( connection, message, request_data,
                                                      header->request_size );
            break;
        case HORIZON_REQ_CLOSE_HANDLE:
            status = horizon_server_handle_close_handle( connection, message );
            break;
        case HORIZON_REQ_SET_HANDLE_INFO:
            status = horizon_server_handle_set_handle_info( connection );
            break;
        case HORIZON_REQ_DUP_HANDLE:
            status = horizon_server_handle_dup_handle( connection, message );
            break;
        case HORIZON_REQ_ALLOCATE_RESERVE_OBJECT:
            status = horizon_server_handle_allocate_reserve_object( connection, message );
            break;
        case HORIZON_REQ_COMPARE_OBJECTS:
            status = horizon_server_handle_compare_objects( connection, message );
            break;
        case HORIZON_REQ_SET_OBJECT_PERMANENCE:
            status = horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_SUCCESS );
            break;
        case HORIZON_REQ_OPEN_PROCESS:
            status = horizon_server_handle_open_object( connection, HORIZON_SERVER_OBJECT_PROCESS );
            break;
        case HORIZON_REQ_OPEN_THREAD:
            status = horizon_server_handle_open_thread( connection, message );
            break;
        case HORIZON_REQ_ADD_ATOM:
            status = horizon_server_handle_atom( connection, request_data, header->request_size, 1 );
            break;
        case HORIZON_REQ_FIND_ATOM:
            status = horizon_server_handle_atom( connection, request_data, header->request_size, 0 );
            break;
        case HORIZON_REQ_SEND_MESSAGE:
            status = horizon_server_handle_send_message( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_ADD_USER_ATOM:
            status = horizon_server_handle_add_user_atom( connection, request_data, header->request_size );
            break;
        case HORIZON_REQ_GET_USER_ATOM_NAME:
            status = horizon_server_handle_get_user_atom_name( connection, message );
            break;
        case HORIZON_REQ_OPEN_CLIPBOARD:
        case HORIZON_REQ_CLOSE_CLIPBOARD:
        case HORIZON_REQ_EMPTY_CLIPBOARD:
        case HORIZON_REQ_SET_CLIPBOARD_DATA:
        case HORIZON_REQ_GET_CLIPBOARD_DATA:
        case HORIZON_REQ_GET_CLIPBOARD_FORMATS:
        case HORIZON_REQ_ENUM_CLIPBOARD_FORMATS:
        case HORIZON_REQ_RELEASE_CLIPBOARD:
        case HORIZON_REQ_GET_CLIPBOARD_INFO:
        case HORIZON_REQ_SET_CLIPBOARD_VIEWER:
        case HORIZON_REQ_ADD_CLIPBOARD_LISTENER:
        case HORIZON_REQ_REMOVE_CLIPBOARD_LISTENER:
            status = horizon_server_handle_clipboard( connection, header->req, message, request_data,
                                                      header->request_size );
            break;
        case HORIZON_REQ_REPLY_MESSAGE:
            status = horizon_server_handle_reply_message( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_GET_MESSAGE_REPLY:
            status = horizon_server_handle_get_message_reply( connection, message );
            break;
        case HORIZON_REQ_GET_MSG_QUEUE:
            status = horizon_server_handle_get_msg_queue( connection );
            break;
        case HORIZON_REQ_GET_MSG_QUEUE_HANDLE:
            status = horizon_server_handle_get_msg_queue_handle( connection );
            break;
        case HORIZON_REQ_SET_QUEUE_MASK:
            status = horizon_server_handle_set_queue_mask( connection, message );
            break;
        case HORIZON_REQ_GET_QUEUE_STATUS:
            status = horizon_server_handle_get_queue_status( connection, message );
            break;
        case HORIZON_REQ_POST_QUIT_MESSAGE:
            status = horizon_server_handle_post_quit_message( connection, message );
            break;
        case HORIZON_REQ_SET_WIN_TIMER:
            status = horizon_server_handle_set_win_timer( connection, message );
            break;
        case HORIZON_REQ_KILL_WIN_TIMER:
            status = horizon_server_handle_kill_win_timer( connection, message );
            break;
        case HORIZON_REQ_SEND_HARDWARE_MESSAGE:
            status = horizon_server_handle_send_hardware_message( connection, message );
            break;
        case HORIZON_REQ_GET_MESSAGE:
            status = horizon_server_handle_get_message( connection, message );
            break;
        case HORIZON_REQ_ACCEPT_HARDWARE_MESSAGE:
            status = horizon_server_handle_accept_hardware_message( connection, message );
            break;
        case HORIZON_REQ_ALLOC_USER_HANDLE:
            status = horizon_server_handle_alloc_user_handle( connection, message );
            break;
        case HORIZON_REQ_FREE_USER_HANDLE:
            status = horizon_server_handle_free_user_handle( connection, message );
            break;
        case HORIZON_REQ_SELECT:
            status = horizon_server_handle_select( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_CREATE_EVENT:
            status = horizon_server_handle_create_event( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_EVENT_OP:
            status = horizon_server_handle_event_op( connection, message );
            break;
        case HORIZON_REQ_QUERY_EVENT:
            status = horizon_server_handle_query_event( connection, message );
            break;
        case HORIZON_REQ_CREATE_COMPLETION:
            status = horizon_server_handle_create_completion( connection, request_data, header->request_size );
            break;
        case HORIZON_REQ_OPEN_COMPLETION:
            status = horizon_server_handle_open_named_object( connection, message, request_data,
                                                              header->request_size, HORIZON_SERVER_OBJECT_COMPLETION );
            break;
        case HORIZON_REQ_ADD_COMPLETION:
            status = horizon_server_handle_add_completion( connection, message );
            break;
        case HORIZON_REQ_REMOVE_COMPLETION:
            status = horizon_server_handle_remove_completion( connection, message );
            break;
        case HORIZON_REQ_GET_THREAD_COMPLETION:
            status = horizon_server_handle_get_thread_completion( connection );
            break;
        case HORIZON_REQ_QUERY_COMPLETION:
            status = horizon_server_handle_query_completion( connection, message );
            break;
        case HORIZON_REQ_SET_COMPLETION_INFO:
            status = horizon_server_handle_set_completion_info( connection, message );
            break;
        case HORIZON_REQ_ADD_FD_COMPLETION:
            status = horizon_server_handle_add_fd_completion( connection, message );
            break;
        case HORIZON_REQ_SET_FD_COMPLETION_MODE:
            status = horizon_server_handle_set_fd_completion_mode( connection, message );
            break;
        case HORIZON_REQ_OPEN_EVENT:
            status = horizon_server_handle_open_named_object( connection, message, request_data,
                                                              header->request_size, HORIZON_SERVER_OBJECT_EVENT );
            break;
        case HORIZON_REQ_OPEN_DIRECTORY:
            status = horizon_server_handle_open_directory( connection, message, request_data,
                                                           header->request_size );
            break;
        case HORIZON_REQ_GET_DIRECTORY_ENTRIES:
            status = horizon_server_handle_get_directory_entries( connection, message );
            break;
        case HORIZON_REQ_UPDATE_RAWINPUT_DEVICES:
            status = horizon_server_handle_update_rawinput_devices( connection, request_data,
                                                                    header->request_size );
            break;
        case HORIZON_REQ_CREATE_KEYED_EVENT:
            status = horizon_server_handle_create_keyed_event( connection, request_data, header->request_size );
            break;
        case HORIZON_REQ_OPEN_KEYED_EVENT:
            status = horizon_server_handle_open_named_object(
                connection, message, request_data, header->request_size, HORIZON_SERVER_OBJECT_KEYED_EVENT );
            break;
        case HORIZON_REQ_CREATE_MUTEX:
            status = horizon_server_handle_create_mutex( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_RELEASE_MUTEX:
            status = horizon_server_handle_release_mutex( connection, message );
            break;
        case HORIZON_REQ_OPEN_MUTEX:
            status = horizon_server_handle_open_named_object( connection, message, request_data,
                                                              header->request_size, HORIZON_SERVER_OBJECT_MUTEX );
            break;
        case HORIZON_REQ_QUERY_MUTEX:
            status = horizon_server_handle_query_mutex( connection, message );
            break;
        case HORIZON_REQ_CREATE_SEMAPHORE:
            status = horizon_server_handle_create_semaphore( connection, message, request_data,
                                                             header->request_size );
            break;
        case HORIZON_REQ_RELEASE_SEMAPHORE:
            status = horizon_server_handle_release_semaphore( connection, message );
            break;
        case HORIZON_REQ_QUERY_SEMAPHORE:
            status = horizon_server_handle_query_semaphore( connection, message );
            break;
        case HORIZON_REQ_OPEN_SEMAPHORE:
            status = horizon_server_handle_open_named_object(
                connection, message, request_data, header->request_size, HORIZON_SERVER_OBJECT_SEMAPHORE );
            break;
        case HORIZON_REQ_CREATE_FILE:
            status = horizon_server_handle_create_file( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_GET_HANDLE_UNIX_NAME:
            status = horizon_server_handle_get_handle_unix_name( connection, message );
            break;
        case HORIZON_REQ_SET_FD_EOF_INFO:
            status = horizon_server_handle_set_fd_eof_info( connection, message );
            break;
        case HORIZON_REQ_SET_FD_DISP_INFO:
            status = horizon_server_handle_set_fd_disp_info( connection, message );
            break;
        case HORIZON_REQ_SET_FD_NAME_INFO:
            status = horizon_server_handle_set_fd_name_info( connection, message, request_data,
                                                             header->request_size );
            break;
        case HORIZON_REQ_GET_HANDLE_FD:
            status = horizon_server_handle_get_handle_fd( connection, message );
            break;
        case HORIZON_REQ_QUERY_DIRECTORY_FILE:
            status = horizon_server_handle_query_directory_file( connection, message, request_data,
                                                                 header->request_size );
            break;
        case HORIZON_REQ_CREATE_MAPPING:
            status = horizon_server_handle_create_mapping( connection, message, request_data,
                                                           header->request_size );
            break;
        case HORIZON_REQ_OPEN_MAPPING:
            status = horizon_server_handle_open_mapping( connection, message, request_data,
                                                         header->request_size );
            break;
        case HORIZON_REQ_GET_MAPPING_INFO:
            status = horizon_server_handle_get_mapping_info( connection, message );
            break;
        case HORIZON_REQ_GET_IMAGE_MAP_ADDRESS:
            status = horizon_server_handle_get_image_map_address( connection, message );
            break;
        case HORIZON_REQ_MAP_VIEW:
            status = horizon_server_handle_map_view( connection, message );
            break;
        case HORIZON_REQ_MAP_IMAGE_VIEW:
            status = horizon_server_handle_map_image_view( connection, message );
            break;
        case HORIZON_REQ_UNMAP_VIEW:
            status = horizon_server_handle_unmap_view( connection, message );
            break;
        case HORIZON_REQ_GET_MAPPING_COMMITTED_RANGE:
            status = horizon_server_handle_get_mapping_committed_range( connection, message );
            break;
        case HORIZON_REQ_ADD_MAPPING_COMMITTED_RANGE:
            status = horizon_server_handle_add_mapping_committed_range( connection, message );
            break;
        case HORIZON_REQ_CREATE_TIMER:
            status = horizon_server_handle_create_timer( connection, message, request_data, header->request_size );
            break;
        case HORIZON_REQ_OPEN_TIMER:
            status = horizon_server_handle_open_named_object( connection, message, request_data,
                                                              header->request_size, HORIZON_SERVER_OBJECT_TIMER );
            break;
        case HORIZON_REQ_SET_TIMER:
            status = horizon_server_handle_set_timer( connection, message );
            break;
        case HORIZON_REQ_CANCEL_TIMER:
            status = horizon_server_handle_cancel_timer( connection, message );
            break;
        case HORIZON_REQ_GET_TIMER_INFO:
            status = horizon_server_handle_get_timer_info( connection, message );
            break;
        case HORIZON_REQ_CREATE_WINDOW:
            status = horizon_server_handle_create_window( connection, message, request_data,
                                                          header->request_size );
            break;
        case HORIZON_REQ_DESTROY_WINDOW:
            status = horizon_server_handle_destroy_window( connection, message );
            break;
        case HORIZON_REQ_GET_DESKTOP_WINDOW:
            status = horizon_server_handle_get_desktop_window( connection, message );
            break;
        case HORIZON_REQ_SET_WINDOW_OWNER:
            status = horizon_server_handle_set_window_owner( connection, message );
            break;
        case HORIZON_REQ_SET_PARENT:
            status = horizon_server_handle_set_parent( connection, message );
            break;
        case HORIZON_REQ_GET_WINDOW_PARENTS:
            status = horizon_server_handle_get_window_parents( connection, message );
            break;
        case HORIZON_REQ_GET_WINDOW_INFO:
            status = horizon_server_handle_get_window_info( connection, message );
            break;
        case HORIZON_REQ_INIT_WINDOW_INFO:
            status = horizon_server_handle_init_window_info( connection, message );
            break;
        case HORIZON_REQ_SET_WINDOW_INFO:
            status = horizon_server_handle_set_window_info( connection, message );
            break;
        case HORIZON_REQ_GET_WINDOW_CHILDREN_FROM_POINT:
            status = horizon_server_handle_get_window_children_from_point( connection, message );
            break;
        case HORIZON_REQ_GET_WINDOW_LIST:
            status = horizon_server_handle_get_window_list( connection, message );
            break;
        case HORIZON_REQ_GET_WINDOW_TREE:
            status = horizon_server_handle_get_window_tree( connection, message );
            break;
        case HORIZON_REQ_SET_WINDOW_POS:
            status = horizon_server_handle_set_window_pos( connection, message, request_data,
                                                           header->request_size );
            break;
        case HORIZON_REQ_GET_WINDOW_RECTANGLES:
            status = horizon_server_handle_get_window_rectangles( connection, message );
            break;
        case HORIZON_REQ_GET_VISIBLE_REGION:
            status = horizon_server_handle_get_visible_region( connection, message );
            break;
        case HORIZON_REQ_GET_UPDATE_REGION:
            status = horizon_server_handle_get_update_region( connection, message );
            break;
        case HORIZON_REQ_UPDATE_WINDOW_ZORDER:
            status = horizon_server_handle_update_window_zorder( connection, message );
            break;
        case HORIZON_REQ_REDRAW_WINDOW:
            status = horizon_server_handle_redraw_window( connection, message, request_data,
                                                          header->request_size );
            break;
        case HORIZON_REQ_SET_WINDOW_PROPERTY:
            status = horizon_server_handle_set_window_property( connection, message, request_data,
                                                                header->request_size );
            break;
        case HORIZON_REQ_REMOVE_WINDOW_PROPERTY:
            status = horizon_server_handle_get_window_property( connection, message, request_data,
                                                                header->request_size, 1 );
            break;
        case HORIZON_REQ_GET_WINDOW_PROPERTY:
            status = horizon_server_handle_get_window_property( connection, message, request_data,
                                                                header->request_size, 0 );
            break;
        case HORIZON_REQ_GET_WINDOW_PROPERTIES:
            status = horizon_server_handle_get_window_properties( connection, message );
            break;
        case HORIZON_REQ_CREATE_WINSTATION:
            status = horizon_server_handle_create_winstation( connection, message, request_data,
                                                              header->request_size );
            break;
        case HORIZON_REQ_OPEN_WINSTATION:
            status = horizon_server_handle_open_winstation( connection, message, request_data,
                                                            header->request_size );
            break;
        case HORIZON_REQ_CLOSE_WINSTATION:
            status = horizon_server_handle_close_handle( connection, message );
            break;
        case HORIZON_REQ_SET_WINSTATION_MONITORS:
            status = horizon_server_handle_set_winstation_monitors( connection, message );
            break;
        case HORIZON_REQ_GET_PROCESS_WINSTATION:
            status = horizon_server_handle_get_process_winstation( connection );
            break;
        case HORIZON_REQ_SET_PROCESS_WINSTATION:
            status = horizon_server_handle_set_process_winstation( connection, message );
            break;
        case HORIZON_REQ_ENUM_WINSTATION:
            status = horizon_server_handle_enum_winstation( connection );
            break;
        case HORIZON_REQ_CREATE_DESKTOP:
            status = horizon_server_handle_create_desktop( connection, message, request_data,
                                                           header->request_size );
            break;
        case HORIZON_REQ_OPEN_DESKTOP:
            status = horizon_server_handle_open_desktop( connection, message, request_data,
                                                         header->request_size );
            break;
        case HORIZON_REQ_OPEN_INPUT_DESKTOP:
            status = horizon_server_handle_open_input_desktop( connection );
            break;
        case HORIZON_REQ_SET_INPUT_DESKTOP:
            status = horizon_server_handle_set_input_desktop( connection, message );
            break;
        case HORIZON_REQ_CLOSE_DESKTOP:
            status = horizon_server_handle_close_handle( connection, message );
            break;
        case HORIZON_REQ_GET_THREAD_DESKTOP:
            status = horizon_server_handle_get_thread_desktop( connection, message );
            break;
        case HORIZON_REQ_SET_THREAD_DESKTOP:
            status = horizon_server_handle_set_thread_desktop( connection, message );
            break;
        case HORIZON_REQ_SET_USER_OBJECT_INFO:
            status = horizon_server_handle_set_user_object_info( connection, message );
            break;
        case HORIZON_REQ_GET_THREAD_INPUT:
            status = horizon_server_handle_get_thread_input( connection );
            break;
        case HORIZON_REQ_SET_FOREGROUND_WINDOW:
            status = horizon_server_handle_set_foreground_window( connection, message );
            break;
        case HORIZON_REQ_SET_FOCUS_WINDOW:
        case HORIZON_REQ_SET_ACTIVE_WINDOW:
            status = horizon_server_handle_set_input_window( connection, message, header->req );
            break;
        case HORIZON_REQ_SET_CAPTURE_WINDOW:
            status = horizon_server_handle_set_capture_window( connection, message );
            break;
        case HORIZON_REQ_SET_CARET_WINDOW:
            status = horizon_server_handle_set_caret_window( connection, message );
            break;
        case HORIZON_REQ_SET_CARET_INFO:
            status = horizon_server_handle_set_caret_info( connection, message );
            break;
        case HORIZON_REQ_CREATE_CLASS:
            status = horizon_server_handle_create_class( connection, message, request_data,
                                                         header->request_size );
            break;
        case HORIZON_REQ_DESTROY_CLASS:
            status = horizon_server_handle_destroy_class( connection, message, request_data,
                                                          header->request_size );
            break;
        case HORIZON_REQ_SET_CURSOR:
            status = horizon_server_handle_set_cursor( connection, message );
            break;
        case HORIZON_REQ_GET_KEY_STATE:
            status = horizon_server_handle_get_key_state( connection, message );
            break;
        default:
            WARN( "unimplemented Horizon server request %d size %u reply_size %u.\n",
                  header->req, header->request_size, header->reply_size );
            if (connection->reply_fd != -1)
                status = horizon_server_write_status( connection->reply_fd, HORIZON_STATUS_NOT_IMPLEMENTED );
            break;
        }

        free( request_data );
        if (status) goto done;
    }

done:
    /* The client closed its request pipe last in exit_thread: every Windows
     * frame is gone. Close our ends before signaling so a joiner observes
     * released pipes, then queue this pthread to be joined by another one. */
    if (connection->request_fd != -1) close( connection->request_fd );
    if (connection->reply_fd != -1) close( connection->reply_fd );
    if (connection->wait_fd != -1) close( connection->wait_fd );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if (connection->thread)
    {
        horizon_server_end_thread_locked( connection );
        __atomic_sub_fetch( &horizon_lifecycle.connections, 1, __ATOMIC_RELAXED );
    }
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    horizon_server_current = NULL;
    free( connection );
    if (wine_nx_thread_unregister) wine_nx_thread_unregister();
    horizon_zombie_reap( &horizon_server_zombies );
    if ((zombie = malloc( sizeof(*zombie) )))
        horizon_zombie_push( &horizon_server_zombies, zombie, pthread_self() );
    return NULL;
}

unsigned int horizon_server_protocol_version(void)
{
    return SERVER_PROTOCOL_VERSION;
}

int horizon_server_connect(void)
{
    int control_pipe[2] = {-1, -1};
    int request_pipe[2] = {-1, -1};
    struct horizon_server_connection *connection = NULL;
    pthread_t thread;

    if (horizon_pipe( control_pipe ) == -1 || horizon_pipe( request_pipe ) == -1)
    {
        int err = errno;

        if (control_pipe[0] != -1) close( control_pipe[0] );
        if (control_pipe[1] != -1) close( control_pipe[1] );
        if (request_pipe[0] != -1) close( request_pipe[0] );
        if (request_pipe[1] != -1) close( request_pipe[1] );

        errno = err;
        fprintf( stderr, "wine: failed to create Horizon server bootstrap pipes: %s\n", strerror(err) );
        exit(1);
    }

    if (!(connection = calloc( 1, sizeof(*connection) )))
    {
        close( control_pipe[0] );
        close( control_pipe[1] );
        close( request_pipe[0] );
        close( request_pipe[1] );
        errno = ENOMEM;
        fprintf( stderr, "wine: failed to allocate Horizon server connection.\n" );
        exit(1);
    }

    connection->request_fd = request_pipe[0];
    connection->reply_fd = -1;
    connection->wait_fd = -1;

    if ((errno = pthread_create( &thread, NULL, horizon_server_thread, connection )))
    {
        int err = errno;

        close( control_pipe[0] );
        close( control_pipe[1] );
        close( request_pipe[0] );
        close( request_pipe[1] );
        free( connection );
        errno = err;
        fprintf( stderr, "wine: failed to start Horizon server thread: %s\n", strerror(err) );
        exit(1);
    }
    pthread_detach( thread );

    close( control_pipe[1] );

    TRACE( "Horizon server bootstrap control fd %d request read fd %d write fd %d version %u.\n",
           control_pipe[0], request_pipe[0], request_pipe[1], horizon_server_protocol_version() );
    horizon_server_queue_fd( request_pipe[1], horizon_server_protocol_version() );
    close( request_pipe[1] );

    return control_pipe[0];
}

void horizon_server_send_fd( int fd )
{
    horizon_fd_queue_push_dup( &horizon_client_to_server_fds, fd, 0 );
}

int horizon_server_receive_fd( unsigned int *handle )
{
    return horizon_fd_queue_pop( &horizon_server_to_client_fds, handle );
}

#ifndef HORIZON_NO_LIBNX_EXCEPTION_HANDLER
/* Weak fallback definitions so smoke binaries (which don't link runtime.c
 * or virtual.c) still resolve.  The runtime/full builds provide strong
 * definitions that override these. */
__attribute__((weak)) void wine_nx_runtime_trace( const char *msg )
{
    (void)msg;
}

__attribute__((weak)) NTSTATUS virtual_handle_fault( EXCEPTION_RECORD *rec, void *stack )
{
    (void)rec;
    (void)stack;
    return STATUS_ACCESS_VIOLATION;
}

/* Optional interpreter boundary, present only in Box64-enabled runtime builds.
 * libnx invokes this handler after svcReturnFromException, on its exception
 * stack, so unwinding to an active user-mode setjmp does not strand a kernel
 * exception. Native Wine faults retain the existing handling path. */
extern BOOL wine_nx_box64_handle_fault( ULONG_PTR address, ULONG access, ULONG_PTR pc,
                                        const unsigned long long *x ) __attribute__((weak));
/* Dynarec builds: the x86 instruction and registers behind a pc in translated code. */
extern int wine_nx_box64_describe_native_pc( ULONG_PTR pc, const unsigned long long *x,
                                             char *buf, size_t size ) __attribute__((weak));
extern int wine_nx_box64_callret_trap( ULONG_PTR *pc ) __attribute__((weak));

#if defined(__aarch64__)
/* KUSER_SHARED_DATA is not always at 0x7ffe0000 on Horizon (virtual_alloc_first_teb).
 * Wine reads it through user_shared_data, but a program can read the Windows
 * address directly: serve the read from the real page and carry on. Windows maps
 * the page read-only, so a write stays an access violation. Each read costs an
 * exception, so the log shows how often it happens. */
static BOOL horizon_redirect_user_shared_data( ThreadExceptionDump *ctx )
{
    static const ULONG_PTR address = 0x7ffe0000;
    static unsigned int count;
    const unsigned char *page = (const unsigned char *)user_shared_data;
    unsigned int exception_class = ctx->esr >> 26, i;
    struct horizon_read_regs regs;
    char buf[160];

    if (!page || (ULONG_PTR)page == address) return FALSE;
    /* data aborts with a valid FAR (FnV clear) reading the page (WnR clear) */
    if ((exception_class != 0x24 && exception_class != 0x25) || (ctx->esr & ((1u << 10) | 0x40)) ||
        ctx->far.x - address >= 0x1000)
        return FALSE;
    for (i = 0; i < 29; i++) regs.x[i] = ctx->cpu_gprs[i].x;
    regs.x[29] = ctx->fp.x;
    regs.x[30] = ctx->lr.x;
    regs.sp = ctx->sp.x;
    regs.pc = ctx->pc.x;
    for (i = 0; i < 32; i++) regs.v[i] = ctx->fpu_gprs[i].v;
    if (!horizon_redirect_read( &regs, *(const u32 *)(ULONG_PTR)ctx->pc.x, address, page, 0x1000 ))
        return FALSE;
    count++;
    if (!(count & (count - 1)))
    {
        snprintf( buf, sizeof(buf), "[USD] %u reads of 0x7ffe0000 served from %p; last 0x%llx at pc=0x%llx",
                  count, page, (unsigned long long)ctx->far.x, (unsigned long long)ctx->pc.x );
        wine_nx_runtime_trace( buf );
    }
    for (i = 0; i < 29; i++) ctx->cpu_gprs[i].x = regs.x[i];
    ctx->fp.x = regs.x[29];
    ctx->lr.x = regs.x[30];
    ctx->sp.x = regs.sp;
    ctx->pc.x = regs.pc;
    for (i = 0; i < 32; i++) ctx->fpu_gprs[i].v = regs.v[i];
    return TRUE;
}

/*
 * Horizon takes user access away from whole pages while it is using them.
 *
 * KPageTableBase::SetupForIpcClient reprotects the pages a read-write IPC
 * buffer fully covers to KernelReadWrite|NotMapped for the length of the call,
 * and svcSetMemoryAttribute -- which libnx uses for every non-cacheable GPU
 * buffer, in nvMapCreate and nvMapClose -- goes through ChangePermissions with
 * a refresh, which first applies an unmapped template, in Mesosphere's words
 * "to cause all entries to page fault if accessed", forces a reschedule, then
 * maps the pages back. A thread that touches such a page in that window takes
 * a translation fault although the memory is ordinary heap, and by the time a
 * handler asks, svcQueryMemory says it is plain accessible heap again.
 *
 * Resume the instruction once the kernel agrees the page is back. An address
 * that keeps faulting is not a window closing, and still parks the thread.
 */
static BOOL horizon_transient_page_fault( ThreadExceptionDump *ctx )
{
    static u64 last_address;
    static unsigned int repeats;
    unsigned int exception_class = ctx->esr >> 26, fault_status = ctx->esr & 0x3f, i;
    u32 needed = (ctx->esr & 0x40) ? (Perm_R | Perm_W) : Perm_R;
    MemoryInfo info;
    u32 page_info;

    if (exception_class != 0x24 && exception_class != 0x25) return FALSE;
    if ((fault_status & ~3u) != 0x04) return FALSE;  /* translation fault, any level */
    if (ctx->esr & (1u << 10)) return FALSE;         /* FnV: FAR means nothing */

    if (__atomic_exchange_n( &last_address, ctx->far.x, __ATOMIC_RELAXED ) == ctx->far.x)
    {
        if (__atomic_add_fetch( &repeats, 1, __ATOMIC_RELAXED ) > 256) return FALSE;
    }
    else __atomic_store_n( &repeats, 0, __ATOMIC_RELAXED );

    for (i = 0; i < 16; i++)
    {
        if (R_FAILED( svcQueryMemory( &info, &page_info, ctx->far.x ) )) return FALSE;
        /* Only the heap: guest memory faults are Wine's to handle. */
        if (info.type != MemType_Heap) return FALSE;
        if ((info.perm & needed) == needed &&
            !(info.attr & (MemAttr_IsIpcMapped | MemAttr_IsDeviceMapped)))
            return TRUE;
        svcSleepThread( 100000ULL );  /* 0.1 ms; the calls that do this are short */
    }
    return FALSE;
}
#endif

void __libnx_exception_handler( ThreadExceptionDump *ctx )
{
    EXCEPTION_RECORD rec = { 0 };
    DWORD64 esr = ctx->esr;
    NTSTATUS status;
    char buf[512];

#if defined(__aarch64__)
    if (horizon_redirect_user_shared_data( ctx )) horizon_resume_exception( ctx );
    /* A translated RET returned natively into a block Box64 marked as possibly
     * changed, onto an undefined instruction: resume after the mark or at the
     * dynarec's epilog. Both through x9, as x17 holds the guest's EDI there.
     * The trap checks the pc is such a mark, whatever the exception class. */
    if (wine_nx_box64_callret_trap)
    {
        ULONG_PTR pc = ctx->pc.x;

        if (wine_nx_box64_callret_trap( &pc ))
        {
            ctx->pc.x = pc;
            horizon_restore_exception_context_x9( ctx );
        }
    }
    if (horizon_transient_page_fault( ctx ))
    {
        static unsigned int resumed;
        unsigned int count = __atomic_add_fetch( &resumed, 1, __ATOMIC_RELAXED );

        /* Sampled: a busy IPC or GPU path can open the window often. */
        if (count <= 8 || !(count & (count - 1)))
        {
            snprintf( buf, sizeof(buf),
                      "[EXC] transient fault at 0x%llx pc=0x%llx kind=%s; the page is back, resumed (%u)",
                      (unsigned long long)ctx->far.x, (unsigned long long)ctx->pc.x,
                      (ctx->esr & 0x40) ? "write" : "read", count );
            wine_nx_runtime_trace( buf );
        }
        horizon_resume_exception( ctx );
    }
#endif

    rec.ExceptionCode = STATUS_ACCESS_VIOLATION;
    rec.ExceptionAddress = (void *)ctx->pc.x;
    rec.NumberParameters = 2;
    if ((esr & 0xf0000000) == 0x80000000) rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
    else if (esr & 0x40) rec.ExceptionInformation[0] = EXCEPTION_WRITE_FAULT;
    else rec.ExceptionInformation[0] = EXCEPTION_READ_FAULT;
    rec.ExceptionInformation[1] = (ULONG_PTR)ctx->far.x;

    snprintf( buf, sizeof(buf),
              "[EXC] desc=0x%08x esr=0x%08x pc=0x%llx far=0x%llx sp=0x%llx lr=0x%llx kind=%s",
              ctx->error_desc, ctx->esr,
              (unsigned long long)ctx->pc.x, (unsigned long long)ctx->far.x,
              (unsigned long long)ctx->sp.x, (unsigned long long)ctx->lr.x,
              rec.ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT ? "exec" :
              rec.ExceptionInformation[0] == EXCEPTION_WRITE_FAULT   ? "write" : "read" );
    /* The fault status tells an unmapped page (translation, 0b0001xx) from one
     * the process may not touch that way (permission, 0b0011xx). */
    {
        size_t len = strlen( buf );
        snprintf( buf + len, sizeof(buf) - len, " dfsc=%#x", (unsigned)(esr & 0x3f) );
    }
    /* Put the kernel state in the first fault line: a second allocation or
     * logging operation can itself fault when native heap pages are locked. */
    {
        MemoryInfo info;
        u32 page_info;
        if (R_SUCCEEDED( svcQueryMemory( &info, &page_info, ctx->far.x ) ))
        {
            size_t len = strlen( buf );
            snprintf( buf + len, sizeof(buf) - len,
                      " region=%#llx/%#llx type=%#x attr=%#x perm=%#x ipc=%u device=%u",
                      (unsigned long long)info.addr, (unsigned long long)info.size,
                      info.type, info.attr, info.perm, info.ipc_refcount, info.device_refcount );
        }
    }
    /* Ordinary heap in that report means little on its own: the kernel unmaps
     * the source of an alias without recording anything a later query shows. */
    alias_source_report( (const void *)(ULONG_PTR)ctx->far.x, buf, sizeof(buf) );
    wine_nx_runtime_trace( buf );
    snprintf( buf, sizeof(buf),
              "[EXC] x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x18=0x%llx",
              (unsigned long long)ctx->cpu_gprs[0].x, (unsigned long long)ctx->cpu_gprs[1].x,
              (unsigned long long)ctx->cpu_gprs[2].x, (unsigned long long)ctx->cpu_gprs[3].x,
              (unsigned long long)ctx->cpu_gprs[18].x );
    wine_nx_runtime_trace( buf );

    status = virtual_handle_fault( &rec, (void *)ctx->sp.x );
#if defined(__aarch64__)
    /* A fault inside a Wine __TRY block returns to its handler, as
     * handle_syscall_fault does on Unix. virtual_check_buffer_for_write and
     * its kind probe memory on purpose and must get FALSE, not a dead thread.
     * Early init and the runtime's own threads have no TEB to ask. */
    {
        /* The program is being taken apart: memory a thread still reads is on
         * its way out. Whatever faults now is a thread that should have stopped;
         * it says so once and stops, instead of faulting here for ever. */
        extern volatile int wine_nx_quit_requested __attribute__((weak));
        static int reported;

        if (&wine_nx_quit_requested && wine_nx_quit_requested)
        {
            if (!reported++)
                wine_nx_runtime_trace( "[EXC] a thread faulted while the program was closing; stopping it" );
            svcExitThread();
        }
    }
    if (status && NtCurrentTeb() && ntdll_get_thread_data()->jmp_buf)
    {
        ctx->cpu_gprs[0].x = (ULONG_PTR)ntdll_get_thread_data()->jmp_buf;
        ctx->cpu_gprs[1].x = 1;
        ctx->pc.x = (ULONG_PTR)longjmp;
        ntdll_get_thread_data()->jmp_buf = NULL;
        wine_nx_runtime_trace( "[EXC] returning to the __TRY handler that probed it" );
        horizon_resume_exception( ctx );
    }
#endif
    if (status)
    {
        unsigned int exception_class = esr >> 26;
        unsigned long long x[31];
        unsigned int i, j;

        /* The fault is fatal from here: log every register, and the x86 side of translated code. */
        for (i = 0; i < 29; i++) x[i] = ctx->cpu_gprs[i].x;
        x[29] = ctx->fp.x;
        x[30] = ctx->lr.x;
        for (i = 4; i <= 30; i += 9)
        {
            int len = snprintf( buf, sizeof(buf), "[EXC]" );

            for (j = i; j < i + 9 && j <= 30; j++)
                len += snprintf( buf + len, sizeof(buf) - len, " x%u=0x%llx", j, x[j] );
            wine_nx_runtime_trace( buf );
        }
        if (wine_nx_box64_describe_native_pc &&
            wine_nx_box64_describe_native_pc( ctx->pc.x, x, buf, sizeof(buf) ))
            wine_nx_runtime_trace( buf );
        /* ESR.FnV invalidates FAR. Never consume instruction aborts or faults
         * outside the active interpreter's 32-bit guest address space. */
        /* A guest's data abort goes back to it as an access violation, with
         * whether it wrote (ESR ISS.WnR) and the registers it was in. */
        if ((exception_class == 0x24 || exception_class == 0x25) && !(esr & (1u << 10)) &&
            wine_nx_box64_handle_fault)
            wine_nx_box64_handle_fault( (ULONG_PTR)ctx->far.x, (esr >> 6) & 1, ctx->pc.x, x );
        snprintf( buf, sizeof(buf), "[EXC] unhandled status=0x%08x; parking thread", (unsigned)status );
        wine_nx_runtime_trace( buf );
        /* Park rather than returning: libnx's exception_returnentry would
         * svcBreak and kill the process before stdio buffers flush. */
        for (;;) svcSleepThread( 1000ULL * 1000ULL * 1000ULL );
    }

#if defined(__aarch64__)
    horizon_resume_exception( ctx );
#else
    wine_nx_runtime_trace( "[EXC] handled fault but cannot restore non-AArch64 context" );
    for (;;) svcSleepThread( 1000ULL * 1000ULL * 1000ULL );
#endif
}
#endif

static size_t page_align_size( size_t size )
{
    return (size + 0xfff) & ~(size_t)0xfff;
}

static u32 get_horizon_perm( int prot )
{
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return Perm_None;
    if (prot & PROT_EXEC) return Perm_Rx;
    if (prot & PROT_WRITE) return Perm_Rw;
    return Perm_R;
}

static int get_effective_horizon_prot( int prot )
{
    if ((prot & PROT_EXEC) && (prot & PROT_WRITE)) prot &= ~PROT_EXEC;
    return prot;
}

static void list_add_mapping( struct horizon_mapping *mapping )
{
    rb_put( &mappings, mapping->addr, &mapping->entry );
}

static void list_remove_mapping( struct horizon_mapping *mapping )
{
    rb_remove( &mappings, &mapping->entry );
}

/* The kernel's heap and alias regions. MapProcessCodeMemory refuses them, but
 * virtmemAddReservation records a reservation over them without asking, so a
 * reservation there is bookkeeping over pages that belong to someone else - in
 * the heap, libnx's malloc arena. Launched with a 32-bit address space they sit
 * below 4 GiB, among Wine's own allocations. */
struct horizon_kernel_region
{
    u64 addr;
    u64 size;
};

static struct horizon_kernel_region horizon_kernel_regions[2];
static int horizon_kernel_region_count = -1;

/* No locks and no tracing: this can run under virtmemLock. */
static void horizon_load_kernel_regions(void)
{
    static const u32 info[][2] =
    {
        { InfoType_HeapRegionAddress,  InfoType_HeapRegionSize },
        { InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
    };
    int i, count = 0;

    if (horizon_kernel_region_count >= 0) return;
    for (i = 0; i < 2; i++)
    {
        u64 addr, size;

        if (R_FAILED( svcGetInfo( &addr, info[i][0], CUR_PROCESS_HANDLE, 0 ) ) ||
            R_FAILED( svcGetInfo( &size, info[i][1], CUR_PROCESS_HANDLE, 0 ) ) || !size)
            continue;
        horizon_kernel_regions[count].addr = addr;
        horizon_kernel_regions[count].size = size;
        count++;
    }
    horizon_kernel_region_count = count;
}

static BOOL horizon_overlaps_kernel_region( void *addr, size_t size )
{
    u64 start = (u64)(uintptr_t)addr, end = start + size;
    int i;

    horizon_load_kernel_regions();
    for (i = 0; i < horizon_kernel_region_count; i++)
        if (start < horizon_kernel_regions[i].addr + horizon_kernel_regions[i].size &&
            horizon_kernel_regions[i].addr < end)
            return TRUE;
    return FALSE;
}

void horizon_log_low_address_space( void )
{
    unsigned long long type_mb[32] = {0}, gaps[6] = {0}, gap_addr[6] = {0}, addr = 0;
    char line[512];
    int len, i, j;

    while (addr < 0x100000000ull)
    {
        MemoryInfo info;
        u32 page_info;
        unsigned long long end;

        if (R_FAILED( svcQueryMemory( &info, &page_info, addr ) ) || !info.size) break;
        end = info.addr + info.size;
        if (end > 0x100000000ull) end = 0x100000000ull;
        if (end <= addr) break;
        type_mb[(info.type & 0xff) < 32 ? (info.type & 0xff) : 31] += end - addr;
        if ((info.type & 0xff) == MemType_Unmapped)
        {
            for (i = 0; i < 6 && gaps[i] >= end - addr; i++) ;
            for (j = 5; j > i; j--) { gaps[j] = gaps[j - 1]; gap_addr[j] = gap_addr[j - 1]; }
            if (i < 6) { gaps[i] = end - addr; gap_addr[i] = addr; }
        }
        addr = end;
    }
    len = snprintf( line, sizeof(line), "[VA] kernel map of the low 4 GB (MB by memory type):" );
    for (i = 0; i < 32; i++)
        if (type_mb[i] >> 20)
            len += snprintf( line + len, sizeof(line) - len, " %#x:%llu", i, type_mb[i] >> 20 );
    wine_nx_runtime_trace( line );
    len = snprintf( line, sizeof(line), "[VA] largest free ranges:" );
    for (i = 0; i < 6 && gaps[i]; i++)
        len += snprintf( line + len, sizeof(line) - len, " %llu MB at 0x%llx", gaps[i] >> 20, gap_addr[i] );
    wine_nx_runtime_trace( line );
}

/* The regions for virtual_init to keep Wine's free-area search out of. */
int horizon_get_kernel_regions( void **starts, size_t *sizes, int max )
{
    MemoryInfo meminfo;
    u32 page_info;
    u64 addr, size;
    int i;

    horizon_load_kernel_regions();
    for (i = 0; i < horizon_kernel_region_count && i < max; i++)
    {
        starts[i] = (void *)(uintptr_t)horizon_kernel_regions[i].addr;
        sizes[i] = horizon_kernel_regions[i].size;
        horizon_trace( "[VA] kernel region %d base=0x%llx size=0x%llx", i,
                       (unsigned long long)horizon_kernel_regions[i].addr,
                       (unsigned long long)horizon_kernel_regions[i].size );
    }
    /* Not excluded: outside the 39-bit layout the stack region can span the
     * small map, where fixed low image bases have to go. Reported only. */
    if (R_SUCCEEDED( svcGetInfo( &addr, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0 ) ) &&
        R_SUCCEEDED( svcGetInfo( &size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0 ) ))
        horizon_trace( "[VA] kernel stack region base=0x%llx size=0x%llx",
                       (unsigned long long)addr, (unsigned long long)size );
    /* Where Wine's first TEB block has landed on the 32-bit layout: heap (type
     * 0x5) means those pages are libnx's malloc arena. */
    if (R_SUCCEEDED( svcQueryMemory( &meminfo, &page_info, 0x7ffe0000 ) ))
        horizon_trace( "[VA] query 0x7ffe0000 base=0x%llx size=0x%llx type=0x%x perm=0x%x",
                       (unsigned long long)meminfo.addr, (unsigned long long)meminfo.size,
                       meminfo.type, meminfo.perm );
    return i;
}

/* The lowest mapping overlapping [addr, addr + size). Unmapping and protecting
 * walk a range from its start, and Wine's free-area search probes addresses
 * through here: a list of every mapping made each of those a full scan. */
static struct horizon_mapping *find_overlap_mapping( void *addr, size_t size )
{
    struct rb_entry *ptr = mappings.root;
    struct horizon_mapping *found = NULL;
    char *start = addr;
    char *end = start + size;

    while (ptr)
    {
        struct horizon_mapping *mapping = RB_ENTRY_VALUE( ptr, struct horizon_mapping, entry );

        if ((char *)mapping->addr + mapping->size <= start) ptr = ptr->right;
        else
        {
            found = mapping;
            ptr = ptr->left;
        }
    }
    return found && (char *)found->addr < end ? found : NULL;
}

static int read_fd_at( int fd, void *buffer, size_t size, off_t offset )
{
    struct horizon_memfile *section = horizon_memfile_from_fd( fd );
    char *ptr = buffer;
    int work_fd;

    /* A section with no file: one copy under its lock, at the offset, leaving
     * the position that duplicated descriptors share alone. */
    if (section)
    {
        ssize_t ret = horizon_memfile_pread( section, buffer, size, offset );

        if (ret >= 0) return 0;
        errno = -ret;
        return -1;
    }
    if ((work_fd = dup( fd )) == -1) return -1;
    if (lseek( work_fd, offset, SEEK_SET ) == (off_t)-1)
    {
        int saved_errno = errno;

        close( work_fd );
        errno = saved_errno;
        return -1;
    }

    while (size)
    {
        ssize_t ret = read( work_fd, ptr, size );

        if (ret > 0)
        {
            ptr += ret;
            size -= ret;
            continue;
        }
        if (!ret)
        {
            close( work_fd );
            return 0;
        }
        if (errno == EINTR) continue;
        {
            int saved_errno = errno;

            close( work_fd );
            errno = saved_errno;
        }
        return -1;
    }

    close( work_fd );
    return 0;
}

static void write_fd_at( int fd, const void *buffer, size_t size, off_t offset )
{
    struct horizon_memfile *section = horizon_memfile_from_fd( fd );
    const char *ptr = buffer;
    int work_fd;

    if (section)
    {
        if (horizon_memfile_pwrite( section, buffer, size, offset ) < 0)
            WARN( "failed to write back a view of a section at offset %#lx.\n", (unsigned long)offset );
        return;
    }
    work_fd = dup( fd );
    if (work_fd == -1)
    {
        WARN( "failed to duplicate Horizon file mapping fd: %s.\n", strerror(errno) );
        return;
    }
    if (lseek( work_fd, offset, SEEK_SET ) == (off_t)-1)
    {
        WARN( "failed to seek Horizon file mapping to offset %#lx: %s.\n",
              (unsigned long)offset, strerror(errno) );
        close( work_fd );
        return;
    }

    while (size)
    {
        ssize_t ret = write( work_fd, ptr, size );

        if (ret > 0)
        {
            ptr += ret;
            size -= ret;
            continue;
        }
        if (ret < 0 && errno == EINTR) continue;
        WARN( "failed to write back Horizon file mapping at offset %#lx: %s.\n",
              (unsigned long)offset, strerror(errno) );
        close( work_fd );
        return;
    }

    close( work_fd );
}

static void remove_reservation( VirtmemReservation *reservation );

static void free_backing( struct horizon_backing *backing, BOOL write_back )
{
    BOOL aliased;

    if (!backing) return;

    /* Pages the kernel still holds as the source of an alias have no mapping
     * of their own: reading them back would fault, and handing them to another
     * allocation would leave that allocation faulting on a write until the
     * alias goes. Leak them instead; the trace says an unmap was missed. */
    aliased = alias_source_range_live( backing->heap_addr, backing->size );
    if (aliased)
    {
        horizon_trace( "[HMAP] backing %p size=0x%lx freed while still an alias source",
                       backing->heap_addr, (unsigned long)backing->size );
        WARN( "freeing Horizon backing %p while the kernel still aliases it.\n", backing->heap_addr );
    }

    if (!aliased && write_back && backing->write_back && backing->fd != -1)
        write_fd_at( backing->fd, backing->heap_addr, backing->size, backing->file_offset );
    if (backing->code_reservation) remove_reservation( backing->code_reservation );
    if (backing->fd != -1) close( backing->fd );
    if (!aliased && !horizon_pages_free( &backing_pages, backing->heap_addr, backing->size ))
        free( backing->heap_addr );
    horizon_object_free( &backing_pool, backing );
}

static void release_backing( struct horizon_backing *backing )
{
    if (!backing) return;

    if (!backing->refs)
    {
        WARN( "releasing unreferenced Horizon backing %p.\n", backing );
        free_backing( backing, FALSE );
        return;
    }
    if (--backing->refs) return;
    free_backing( backing, TRUE );
}

static void destroy_backing( struct horizon_backing *backing )
{
    free_backing( backing, FALSE );
}

static struct horizon_mapping *alloc_mapping( void *addr, size_t size, struct horizon_backing *backing,
                                              size_t source_offset, VirtmemReservation *reservation,
                                              int prot )
{
    struct horizon_mapping *mapping = horizon_object_alloc( &mapping_pool );

    if (!mapping) return NULL;

    mapping->addr = addr;
    mapping->size = size;
    mapping->source_offset = source_offset;
    mapping->prot = get_effective_horizon_prot( prot );
    mapping->backing = backing;
    mapping->reservation = reservation;
    if (backing) backing->refs++;
    return mapping;
}

static VirtmemReservation *reserve_fixed_range_locked( void *addr, size_t size )
{
    VirtmemReservation *reservation = virtmemAddReservation( addr, size );

    if (!reservation) errno = ENOMEM;
    return reservation;
}

static VirtmemReservation *reserve_fixed_range( void *addr, size_t size )
{
    VirtmemReservation *reservation;

    virtmemLock();
    reservation = reserve_fixed_range_locked( addr, size );
    virtmemUnlock();
    return reservation;
}

static void remove_reservation_locked( VirtmemReservation *reservation )
{
    virtmemRemoveReservation( reservation );
}

static void remove_reservation( VirtmemReservation *reservation )
{
    virtmemLock();
    remove_reservation_locked( reservation );
    virtmemUnlock();
}

/* Wine-visible memory uses AliasCode mappings so RW pages can later become RX. */
static int check_code_memory_syscalls(void)
{
    if (!envIsSyscallHinted(0x73) || !envIsSyscallHinted(0x77) ||
        !envIsSyscallHinted(0x78) || envGetOwnProcessHandle() == INVALID_HANDLE)
    {
        errno = ENOSYS;
        return -1;
    }

    return 0;
}

/* What the kernel has at an address, for a failure that needs explaining:
 * svcMapProcessCodeMemory wants a free destination and plain read-write heap
 * as the source, and svcSetProcessMemoryPermission wants code that was never
 * written to, so the state of both says which rule was broken. */
static void describe_memory( void *addr, char *buffer, size_t size )
{
    MemoryInfo info;
    u32 page_info;

    if (R_SUCCEEDED( svcQueryMemory( &info, &page_info, (u64)(ULONG_PTR)addr ) ))
        snprintf( buffer, size, "%#llx/%#llx type=%#x perm=%#x attr=%#x",
                  (unsigned long long)info.addr, (unsigned long long)info.size,
                  info.type, info.perm, info.attr );
    else
        snprintf( buffer, size, "unknown" );
}

static int set_code_memory_perm( void *addr, void *source, size_t size, int prot, BOOL source_accessible )
{
    int effective_prot = get_effective_horizon_prot( prot );
    Result rc;

    if (check_code_memory_syscalls()) return -1;

    if ((effective_prot & PROT_EXEC) && source_accessible) armDCacheFlush( source, size );

    rc = svcSetProcessMemoryPermission( envGetOwnProcessHandle(), (u64)addr, size,
                                        get_horizon_perm( effective_prot ) );
    if (R_FAILED(rc))
    {
        char at[96], from[96];

        describe_memory( addr, at, sizeof(at) );
        describe_memory( source, from, sizeof(from) );
        horizon_trace( "[HMAP] set_perm failed addr=%p source=%p size=0x%lx prot=0x%x rc=0x%x at %s from %s",
                       addr, source, (unsigned long)size, prot, rc, at, from );
        WARN( "svcSetProcessMemoryPermission(%p, %zu, %#x) failed %#x.\n",
              addr, size, prot, rc );
        errno = EINVAL;
        return -1;
    }

    if (effective_prot & PROT_EXEC) armICacheInvalidate( addr, size );
    return 0;
}

static int map_code_memory_range( void *addr, void *source, size_t size, int prot, int map_errno )
{
    int effective_prot = get_effective_horizon_prot( prot );
    Result rc;

    if (check_code_memory_syscalls()) return -1;

    if (effective_prot & PROT_EXEC) armDCacheFlush( source, size );

    rc = svcMapProcessCodeMemory( envGetOwnProcessHandle(), (u64)addr, (u64)source, size );
    if (R_FAILED(rc))
    {
        char at[96], from[96];

        describe_memory( addr, at, sizeof(at) );
        describe_memory( source, from, sizeof(from) );
        horizon_trace( "[HMAP] map_code failed addr=%p source=%p size=0x%lx prot=0x%x rc=0x%x at %s from %s",
                       addr, source, (unsigned long)size, prot, rc, at, from );
        WARN( "svcMapProcessCodeMemory(%p, %p, %zu) failed %#x.\n", addr, source, size, rc );
        /* Say what the kernel refused rather than that the arguments were
         * wrong: a caller told EINVAL retries the same mapping for ever,
         * while EEXIST and ENOMEM both send it somewhere else. */
        if (R_MODULE(rc) == Module_Kernel && R_DESCRIPTION(rc) == KernelError_InvalidMemoryState)
            errno = EEXIST;
        else if (R_MODULE(rc) == Module_Kernel &&
                 (R_DESCRIPTION(rc) == KernelError_ResourceExhausted ||
                  R_DESCRIPTION(rc) == KernelError_OutOfMemory))
            errno = ENOMEM;
        else errno = map_errno;
        return -1;
    }
    note_alias_source( source, addr, size, TRUE );

    if (set_code_memory_perm( addr, source, size, prot, FALSE ))
    {
        svcUnmapProcessCodeMemory( envGetOwnProcessHandle(), (u64)addr, (u64)source, size );
        note_alias_source( source, addr, size, FALSE );
        return -1;
    }

    return 0;
}

/* Set while the process is being taken apart: the guest's memory is going away,
 * and both traces read the thread's TEB, which lives in it. */
static int releasing_everything;

static int unmap_code_memory_range( void *addr, void *source, size_t size )
{
    Result rc;

    if (check_code_memory_syscalls()) return -1;

    rc = svcUnmapProcessCodeMemory( envGetOwnProcessHandle(), (u64)addr, (u64)source, size );
    if (R_FAILED(rc))
    {
        char at[96], from[96];

        if (releasing_everything) return -1;
        describe_memory( addr, at, sizeof(at) );
        describe_memory( source, from, sizeof(from) );
        horizon_trace( "[HMAP] unmap_code failed addr=%p source=%p size=0x%lx rc=0x%x at %s from %s",
                       addr, source, (unsigned long)size, rc, at, from );
        WARN( "svcUnmapProcessCodeMemory(%p, %p, %zu) failed %#x.\n", addr, source, size, rc );
        errno = EINVAL;
        return -1;
    }
    note_alias_source( source, addr, size, FALSE );

    return 0;
}

static struct horizon_backing *create_backing_locked( size_t size, int prot, int fd, off_t offset, int flags )
{
    struct horizon_backing *backing;

    if (check_code_memory_syscalls()) return NULL;

    if (!(backing = horizon_object_alloc( &backing_pool )))
    {
        errno = ENOMEM;
        return NULL;
    }

    backing->fd = -1;
    backing->size = size;
    backing->heap_addr = horizon_pages_alloc_any( &backing_pages, size );
    if (!backing->heap_addr)
    {
        horizon_object_free( &backing_pool, backing );
        errno = ENOMEM;
        return NULL;
    }
    memset( backing->heap_addr, 0, size );

    if (fd != -1)
    {
        int saved_errno;

        if (read_fd_at( fd, backing->heap_addr, size, offset ))
        {
            saved_errno = errno;
            destroy_backing( backing );
            errno = saved_errno;
            return NULL;
        }

        if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
        {
            backing->fd = dup( fd );
            if (backing->fd == -1)
            {
                saved_errno = errno;
                destroy_backing( backing );
                errno = saved_errno;
                return NULL;
            }
            backing->file_offset = offset;
            backing->write_back = TRUE;
        }
    }

    return backing;
}

static int map_backing_at_locked( void *addr, size_t size, int prot, int fd, off_t offset,
                                  int flags, int map_errno )
{
    struct horizon_backing *backing;
    struct horizon_mapping *mapping;
    VirtmemReservation *reservation;

    if (!(backing = create_backing_locked( size, prot, fd, offset, flags )))
    {
        horizon_trace( "[HMAP] create_backing failed addr=%p size=0x%lx prot=0x%x errno=%d",
                       addr, (unsigned long)size, prot, errno );
        return -1;
    }

    if (!(reservation = reserve_fixed_range_locked( addr, size )))
    {
        horizon_trace( "[HMAP] reserve_target failed addr=%p size=0x%lx prot=0x%x errno=%d",
                       addr, (unsigned long)size, prot, errno );
        destroy_backing( backing );
        return -1;
    }

    backing->code_addr = addr;
    backing->code_reservation = reservation;

    if (map_code_memory_range( addr, backing->heap_addr, size, prot, map_errno ))
    {
        horizon_trace( "[HMAP] map_backing failed addr=%p source=%p size=0x%lx prot=0x%x errno=%d",
                       addr, backing->heap_addr, (unsigned long)size, prot, errno );
        backing->code_reservation = NULL;
        remove_reservation_locked( reservation );
        destroy_backing( backing );
        return -1;
    }

    if (!(mapping = alloc_mapping( addr, size, backing, 0, NULL, prot )))
    {
        /* Never recycle pages if the kernel could not remove their alias. */
        if (unmap_code_memory_range( addr, backing->heap_addr, size )) return -1;
        backing->code_reservation = NULL;
        remove_reservation_locked( reservation );
        destroy_backing( backing );
        errno = ENOMEM;
        return -1;
    }

    list_add_mapping( mapping );
    return 0;
}

static int map_backing_at( void *addr, size_t size, int prot, int fd, off_t offset, int flags,
                           int map_errno )
{
    int ret;

    virtmemLock();
    ret = map_backing_at_locked( addr, size, prot, fd, offset, flags, map_errno );
    virtmemUnlock();
    return ret;
}

/* Replaces the reservation over [start, start + size) with a mapping. A range
 * asked for with PROT_NONE is left unmapped, a hole, unless map_range says to
 * map it inaccessible: memory that is mapped can be committed later with a
 * permission change, which costs no further kernel mapping. */
static int replace_reservation_mapping( struct horizon_mapping *mapping, char *start, size_t size,
                                       int prot, BOOL map_range )
{
    char *mapping_start = mapping->addr;
    char *mapping_end = mapping_start + mapping->size;
    char *end = start + size;
    struct horizon_mapping *left = NULL, *right = NULL;
    int saved_errno;

    /* Prepare both replacements while the old reservation still excludes
     * native allocations. Dropping it first briefly exposed the entire
     * early guest arena to libnx, and allocation failures lost its metadata. */
    if (mapping_start < start)
    {
        VirtmemReservation *reservation = reserve_fixed_range( mapping_start, start - mapping_start );
        if (!reservation) goto failed;
        if (!(left = alloc_mapping( mapping_start, start - mapping_start, NULL, 0, reservation, PROT_NONE )))
        {
            remove_reservation( reservation );
            errno = ENOMEM;
            goto failed;
        }
    }

    if (end < mapping_end)
    {
        VirtmemReservation *reservation = reserve_fixed_range( end, mapping_end - end );
        if (!reservation) goto failed;
        if (!(right = alloc_mapping( end, mapping_end - end, NULL, 0, reservation, PROT_NONE )))
        {
            remove_reservation( reservation );
            errno = ENOMEM;
            goto failed;
        }
    }

    list_remove_mapping( mapping );
    if (map_range && map_backing_at( start, size, prot, -1, 0, MAP_PRIVATE | MAP_ANON, EINVAL ))
    {
        /* Keep the original reservation and tree entry on a failed commit.
         * Releasing them here turns a retry into an unrecoverable hole. */
        list_add_mapping( mapping );
        goto failed;
    }
    if (left) list_add_mapping( left );
    if (right) list_add_mapping( right );
    remove_reservation( mapping->reservation );
    horizon_object_free( &mapping_pool, mapping );
    return 0;

failed:
    saved_errno = errno;
    if (right)
    {
        remove_reservation( right->reservation );
        horizon_object_free( &mapping_pool, right );
    }
    if (left)
    {
        remove_reservation( left->reservation );
        horizon_object_free( &mapping_pool, left );
    }
    errno = saved_errno;
    return -1;
}

static int change_reservation_mapping( struct horizon_mapping *mapping, char *start, size_t size, int prot )
{
    return replace_reservation_mapping( mapping, start, size, prot, prot != PROT_NONE );
}

static int split_reservation_mapping( struct horizon_mapping *mapping, char *start, size_t size )
{
    return change_reservation_mapping( mapping, start, size, PROT_NONE );
}

static int split_backing_mapping( struct horizon_mapping *mapping, char *start, size_t size )
{
    char *mapping_start = mapping->addr;
    char *mapping_end = mapping_start + mapping->size;
    char *end = start + size;
    size_t offset = start - mapping_start;
    void *source = (char *)mapping->backing->heap_addr + mapping->source_offset + offset;
    struct horizon_mapping *left = NULL;
    struct horizon_mapping *right = NULL;

    if (mapping_start < start &&
        !(left = alloc_mapping( mapping_start, start - mapping_start,
                                mapping->backing, mapping->source_offset, NULL, mapping->prot )))
        return -1;

    if (end < mapping_end &&
        !(right = alloc_mapping( end, mapping_end - end, mapping->backing,
                                 mapping->source_offset + (end - mapping_start), NULL, mapping->prot )))
    {
        if (left)
        {
            release_backing( left->backing );
            horizon_object_free( &mapping_pool, left );
        }
        return -1;
    }

    if (unmap_code_memory_range( start, source, size ))
    {
        if (left)
        {
            release_backing( left->backing );
            horizon_object_free( &mapping_pool, left );
        }
        if (right)
        {
            release_backing( right->backing );
            horizon_object_free( &mapping_pool, right );
        }
        return -1;
    }

    list_remove_mapping( mapping );

    if (left) list_add_mapping( left );
    if (right) list_add_mapping( right );

    release_backing( mapping->backing );
    horizon_object_free( &mapping_pool, mapping );
    return 0;
}

static struct horizon_mapping *split_backing_mapping_metadata( struct horizon_mapping *mapping,
                                                               char *start, size_t size )
{
    char *mapping_start = mapping->addr;
    char *mapping_end = mapping_start + mapping->size;
    char *end = start + size;
    struct horizon_mapping *left = NULL;
    struct horizon_mapping *middle = NULL;
    struct horizon_mapping *right = NULL;

    if (mapping_start == start && mapping_end == end) return mapping;

    if (mapping_start < start &&
        !(left = alloc_mapping( mapping_start, start - mapping_start, mapping->backing,
                                mapping->source_offset, NULL, mapping->prot )))
        goto failed;

    if (!(middle = alloc_mapping( start, size, mapping->backing,
                                  mapping->source_offset + (start - mapping_start),
                                  NULL, mapping->prot )))
        goto failed;

    if (end < mapping_end &&
        !(right = alloc_mapping( end, mapping_end - end, mapping->backing,
                                 mapping->source_offset + (end - mapping_start),
                                 NULL, mapping->prot )))
        goto failed;

    list_remove_mapping( mapping );
    if (left) list_add_mapping( left );
    if (right) list_add_mapping( right );
    list_add_mapping( middle );
    release_backing( mapping->backing );
    horizon_object_free( &mapping_pool, mapping );
    return middle;

failed:
    if (left)
    {
        release_backing( left->backing );
        horizon_object_free( &mapping_pool, left );
    }
    if (middle)
    {
        release_backing( middle->backing );
        horizon_object_free( &mapping_pool, middle );
    }
    if (right)
    {
        release_backing( right->backing );
        horizon_object_free( &mapping_pool, right );
    }
    errno = ENOMEM;
    return NULL;
}

/* Gives back the stacks of the threads that ended last, for the same reason and
 * at the same point in the closing as the code mappings below. libnx maps a
 * thread's stack out of the heap and gives those pages back only when the thread
 * is joined, and a thread here is joined by the next one to end: the last
 * connection thread and the last thread of Wine's own are still holding theirs.
 * Returns how many were joined. */
unsigned int horizon_release_thread_stacks(void)
{
    extern unsigned int horizon_join_last_exited_thread(void);  /* thread.c */

    return horizon_zombie_reap( &horizon_server_zombies ) + horizon_join_last_exited_thread();
}

/* Gives every code mapping back before the loader takes the process over. One
 * left behind keeps its source pages lent and its own pages marked as code, and
 * the loader reuses that memory for the next program: it then runs into pages
 * that are not what it mapped there. Called once the program's threads are gone,
 * so nothing is walking these mappings any more. */
void horizon_release_code_mappings( unsigned int *released, unsigned int *failed )
{
    struct rb_entry *entry;

    releasing_everything = 1;
    pthread_mutex_lock( &mapping_mutex );
    /* A view first: its pages come from a section's anchors, and an anchor a
     * view still uses cannot go. */
    for (entry = rb_head( mappings.root ); entry; entry = rb_next( entry ))
    {
        struct horizon_mapping *mapping = RB_ENTRY_VALUE( entry, struct horizon_mapping, entry );

        if (mapping->section_state != SECTION_ALIASED || !mapping->section) continue;
        if (horizon_memfile_alias( mapping->section, mapping->addr, mapping->section_offset, mapping->size, 0 ))
            (*failed)++;
        else (*released)++;
    }
    for (entry = rb_head( mappings.root ); entry; entry = rb_next( entry ))
    {
        struct horizon_mapping *mapping = RB_ENTRY_VALUE( entry, struct horizon_mapping, entry );
        void *source;

        if (mapping->backing && mapping->backing->heap_addr)
            source = (char *)mapping->backing->heap_addr + mapping->source_offset;
        else if (mapping->anchor_source) source = mapping->anchor_source;  /* a section's anchor */
        else continue;
        if (unmap_code_memory_range( mapping->addr, source, mapping->size )) (*failed)++;
        else (*released)++;
    }
    pthread_mutex_unlock( &mapping_mutex );
}

static int protect_code_mapping( struct horizon_mapping *mapping, int prot )
{
    int old_prot = mapping->prot;
    int new_prot = get_effective_horizon_prot( prot );
    void *source = (char *)mapping->backing->heap_addr + mapping->source_offset;

    if ((old_prot & PROT_WRITE) && (new_prot & PROT_WRITE))
    {
        mapping->prot = new_prot;
        return 0;
    }

    if ((old_prot & PROT_WRITE) && !(new_prot & PROT_WRITE))
    {
        int saved_errno;

        /* Not a permission change: writing to a range turns it into
         * AliasCodeData, which svcSetProcessMemoryPermission refuses
         * (InvalidMemoryState), so the range has to be mapped again. */
        if (unmap_code_memory_range( mapping->addr, source, mapping->size )) return -1;
        if (!map_code_memory_range( mapping->addr, source, mapping->size, prot, EINVAL ))
        {
            mapping->prot = new_prot;
            return 0;
        }

        saved_errno = errno;
        WARN( "failed to remap writable code memory %p-%p with prot %#x; restoring old permissions.\n",
              mapping->addr, (char *)mapping->addr + mapping->size, prot );
        if (map_code_memory_range( mapping->addr, source, mapping->size, old_prot, EINVAL ))
            WARN( "failed to restore writable code memory %p-%p.\n",
                  mapping->addr, (char *)mapping->addr + mapping->size );
        errno = saved_errno;
        return -1;
    }

    if (set_code_memory_perm( mapping->addr, source, mapping->size, prot, FALSE )) return -1;
    mapping->prot = new_prot;
    return 0;
}

/* Views of sections with no file (horizon_memfile.h) show the section's own
 * pages: an anchor code-maps them once, and svcMapProcessMemory maps anchored
 * pages at each view's address as well. */
static int check_section_syscalls(void)
{
    if (check_code_memory_syscalls()) return -1;
    if (!envIsSyscallHinted(0x74) || !envIsSyscallHinted(0x75))
    {
        errno = ENOSYS;
        return -1;
    }
    return 0;
}

/* The first failures of the kernel side of section views, in the runtime log. */
static void section_failure( const char *what, void *addr, void *source, size_t size, int error )
{
    char line[192];

    if (++section_failures > 16) return;
    snprintf( line, sizeof(line), "[HMAP] section %s failed: %p from %p, 0x%zx bytes, errno %d",
              what, addr, source, size, error );
    wine_nx_runtime_trace( line );
}

/* Anchors are in the mapping tree, so nothing is mapped over them, and their
 * addresses stay reserved while the pages are there. */
/* Anchors go next to each other in one region rather than wherever libnx
 * places them. Three hundred of them, scattered, fragmented the address space
 * the runtime keeps for its own mappings until a 5 MB section had nowhere to
 * go; Need for Speed Most Wanted took the view that failed and died on it.
 * The region is reserved once, so the anchors inside it need no reservation. */
extern void *horizon_native_window_start, *horizon_native_window_end;
static int horizon_query_region( void *context, unsigned long long addr, struct horizon_region *region );

#include "horizon_code_memory.h"

/* Everything a 32-bit program can address. */
#define WINE_NX_GUEST_LIMIT 0x100000000ull

#define HORIZON_ANCHOR_REGION ((size_t)32 * 1024 * 1024)
#define HORIZON_ANCHOR_REGIONS 32

static struct { char *start, *end, *cursor; } anchor_regions[HORIZON_ANCHOR_REGIONS];
static unsigned int anchor_region_count;
static char *anchor_region, *anchor_region_end;   /* the first, for the tests */

/* A free run of that size in one of the regions, starting where the last
 * anchor ended so a region full of them is not walked from the beginning
 * every time. */
static void *find_anchor_run_locked( size_t size )
{
    unsigned int i;

    for (i = 0; i < anchor_region_count; i++)
    {
        char *candidate = anchor_regions[i].cursor;
        int wrapped = 0;

        for (;;)
        {
            struct horizon_mapping *overlap;

            if (size > (size_t)(anchor_regions[i].end - candidate))
            {
                if (wrapped++) break;
                candidate = anchor_regions[i].start;
                continue;
            }
            if (!(overlap = find_overlap_mapping( candidate, size )))
            {
                anchor_regions[i].cursor = candidate + size;
                return candidate;
            }
            candidate = (char *)overlap->addr + overlap->size;
        }
    }
    return NULL;
}

/* A free run for a new region, found by walking this runtime's own mappings
 * and the kernel's map of the window it keeps for its own placements. libnx's
 * search picks at random and asks 512 times, and each ask walks every
 * reservation the process holds: with regions full, that search was 59% of
 * Most Wanted's main thread and its frame rate halved. */
/* An anchor region is this runtime's before anything is in it: anchors are
 * packed into it by find_anchor_run_locked, which looks only at the mapping
 * tree. Its free space reads as free to the kernel and to the tree alike, and
 * build 227 put a code arena there -- after which every section anchor in the
 * region failed with EEXIST, and The Sims 2 wrote through the NULL view it was
 * handed. */
static char *anchor_region_end_overlapping( const char *start, size_t size )
{
    unsigned int i;

    for (i = 0; i < anchor_region_count; i++)
        if (start < anchor_regions[i].end && anchor_regions[i].start < start + size) return anchor_regions[i].end;
    return NULL;
}

static void *find_free_run_locked( char *candidate, char *end, size_t size )
{
    struct horizon_region region;

    if (!candidate || !end) return NULL;
    while (size <= (size_t)(end - candidate))
    {
        struct horizon_mapping *overlap = find_overlap_mapping( candidate, size );
        char *region_end;

        if ((region_end = anchor_region_end_overlapping( candidate, size )))
        {
            candidate = region_end;
            continue;
        }
        if (overlap)
        {
            candidate = (char *)overlap->addr + overlap->size;
            continue;
        }
        if (horizon_range_unmapped( (unsigned long long)(uintptr_t)candidate, size,
                                    horizon_query_region, NULL ))
            return candidate;
        /* Past the block that is in the way, which is libnx's, not ours. */
        if (!horizon_query_region( NULL, (unsigned long long)(uintptr_t)candidate, &region ) ||
            region.addr + region.size <= (unsigned long long)(uintptr_t)candidate)
            return NULL;
        candidate = (char *)(uintptr_t)(region.addr + region.size);
    }
    return NULL;
}

static void *find_anchor_region_locked( size_t size )
{
    return find_free_run_locked( horizon_native_window_start, horizon_native_window_end, size );
}

static void *find_anchor_address_locked( size_t size )
{
    void *addr, *region;
    size_t region_size = max( HORIZON_ANCHOR_REGION, size );

    if ((addr = find_anchor_run_locked( size ))) return addr;
    /* Another region rather than one anchor at a time. */
    if (anchor_region_count == HORIZON_ANCHOR_REGIONS) return NULL;
    if (!(region = find_anchor_region_locked( region_size )) &&
        !(region = virtmemFindCodeMemory( region_size, 0x1000 ))) return NULL;
    if (!virtmemAddReservation( region, region_size )) return NULL;
    horizon_trace( "[HMAP] anchor region %u: 0x%lx bytes at %p",
                   anchor_region_count + 1, (unsigned long)region_size, region );
    anchor_regions[anchor_region_count].start = region;
    anchor_regions[anchor_region_count].cursor = region;
    anchor_regions[anchor_region_count].end = (char *)region + region_size;
    if (!anchor_region_count++)
    {
        anchor_region = region;
        anchor_region_end = (char *)region + region_size;
    }
    return find_anchor_run_locked( size );
}

/* Code memory for the dynarec's arenas, placed in the same window and walked
 * the same way: what libnx's random probe could not find. Both aliases are
 * mapped while the window is locked, so the second walk sees the first one and
 * nothing else can take the range in between. */
/* Where a code arena may go. A program Wine runs here is 32-bit, so every
 * address it can name is below 4 GB: on a 36- or 39-bit address space the
 * range above that is the runtime's to use and no arena need cost the program
 * anything. Only when the whole address space is 4 GB do the two share, and
 * then the window is all a code mapping may use, since everything else below
 * 4 GB is reserved for the program. */
static void *find_code_run_locked( size_t size )
{
    void *space_start, *space_limit;

    horizon_get_address_space_limits( &space_start, &space_limit );
    if ((unsigned long long)(uintptr_t)space_limit > WINE_NX_GUEST_LIMIT)
        return find_free_run_locked( (char *)(uintptr_t)WINE_NX_GUEST_LIMIT, space_limit, size );
    return find_anchor_region_locked( size );
}

int wine_nx_code_memory_map( void *source, size_t size, struct wine_nx_code_memory *out, unsigned int *rc )
{
    Handle handle = INVALID_HANDLE;
    Result res;

    memset( out, 0, sizeof(*out) );
    *rc = 0;
    if (R_FAILED( (res = svcCreateCodeMemory( &handle, source, size )) ))
    {
        *rc = res;
        return 0;
    }
    pthread_mutex_lock( &mapping_mutex );
    virtmemLock();
    if ((out->rw = find_code_run_locked( size )))
    {
        if (R_FAILED( (res = svcControlCodeMemory( handle, CodeMapOperation_MapOwner,
                                                  out->rw, size, Perm_Rw )) ))
            out->rw = NULL;
        else
            out->rw_token = virtmemAddReservation( out->rw, size );
    }
    if (out->rw && (out->rx = find_code_run_locked( size )))
    {
        if (R_FAILED( (res = svcControlCodeMemory( handle, CodeMapOperation_MapSlave,
                                                  out->rx, size, Perm_Rx )) ))
            out->rx = NULL;
        else
            out->rx_token = virtmemAddReservation( out->rx, size );
    }
    virtmemUnlock();
    pthread_mutex_unlock( &mapping_mutex );

    if (out->rw && out->rx)
    {
        out->handle = handle;
        out->size = size;
        return 1;
    }
    *rc = res;  /* 0 when the window simply had no run that large */
    if (out->rw)
        svcControlCodeMemory( handle, CodeMapOperation_UnmapOwner, out->rw, size, 0 );
    virtmemLock();
    if (out->rw_token) virtmemRemoveReservation( out->rw_token );
    if (out->rx_token) virtmemRemoveReservation( out->rx_token );
    virtmemUnlock();
    svcCloseHandle( handle );
    memset( out, 0, sizeof(*out) );
    return 0;
}

void wine_nx_code_memory_unmap( struct wine_nx_code_memory *memory )
{
    if (!memory->handle) return;
    svcControlCodeMemory( memory->handle, CodeMapOperation_UnmapSlave,
                          memory->rx, memory->size, 0 );
    svcControlCodeMemory( memory->handle, CodeMapOperation_UnmapOwner,
                          memory->rw, memory->size, 0 );
    svcCloseHandle( memory->handle );
    virtmemLock();
    if (memory->rw_token) virtmemRemoveReservation( memory->rw_token );
    if (memory->rx_token) virtmemRemoveReservation( memory->rx_token );
    virtmemUnlock();
    memset( memory, 0, sizeof(*memory) );
}

/* The largest run the window still holds: what the next arena may ask for, and
 * in the log the difference between a window that is full and a kernel that
 * has no code memory object left. The kernel keeps one block per free run, so
 * each unmapped block is a whole run; only this runtime's own mappings, which
 * it may hold before the kernel does, still have to be taken off it. */
size_t wine_nx_native_window_free(void)
{
    char *candidate = horizon_native_window_start, *end = horizon_native_window_end;
    void *space_start, *space_limit;
    size_t largest = 0;

    horizon_get_address_space_limits( &space_start, &space_limit );
    if ((unsigned long long)(uintptr_t)space_limit > WINE_NX_GUEST_LIMIT)
    {
        /* Above the program's 4 GB, where an arena takes nothing from it. */
        candidate = (char *)(uintptr_t)WINE_NX_GUEST_LIMIT;
        end = space_limit;
    }
    if (!candidate || !end) return 0;
    pthread_mutex_lock( &mapping_mutex );
    virtmemLock();
    while (candidate < end)
    {
        struct horizon_mapping *overlap;
        struct horizon_region region;
        char *run_end, *region_end;
        unsigned int i;

        /* Taken: an anchor region, one of this runtime's mappings, or a block
         * the kernel has. Otherwise free until the first of those. */
        if ((region_end = anchor_region_end_overlapping( candidate, 1 )))
        {
            candidate = region_end;
            continue;
        }
        if (!horizon_query_region( NULL, (unsigned long long)(uintptr_t)candidate, &region )) break;
        run_end = (char *)(uintptr_t)(region.addr + region.size);
        if (run_end <= candidate) break;
        if (run_end > end) run_end = end;
        if (region.type != HORIZON_MEMTYPE_UNMAPPED)
        {
            candidate = run_end;
            continue;
        }
        if ((overlap = find_overlap_mapping( candidate, (size_t)(run_end - candidate) )))
        {
            if ((char *)overlap->addr <= candidate)
            {
                candidate = (char *)overlap->addr + overlap->size;
                continue;
            }
            run_end = overlap->addr;
        }
        for (i = 0; i < anchor_region_count; i++)
            if (anchor_regions[i].start > candidate && anchor_regions[i].start < run_end)
                run_end = anchor_regions[i].start;
        if ((size_t)(run_end - candidate) > largest) largest = (size_t)(run_end - candidate);
        candidate = run_end;
    }
    virtmemUnlock();
    pthread_mutex_unlock( &mapping_mutex );
    return largest;
}

static void *horizon_section_anchor( void *source, size_t size, void **token )
{
    VirtmemReservation *reservation = NULL;
    struct horizon_mapping *mapping;
    void *addr;

    virtmemLock();
    if (!(addr = find_anchor_address_locked( size )) &&
        (addr = virtmemFindCodeMemory( size, 0x1000 )))
        reservation = reserve_fixed_range_locked( addr, size );
    virtmemUnlock();
    if (!addr)
    {
        section_failure( "anchor address", addr, source, size, ENOMEM );
        errno = ENOMEM;
        return NULL;
    }
    if (!(mapping = alloc_mapping( addr, size, NULL, 0, reservation, PROT_READ | PROT_WRITE )))
    {
        if (reservation) remove_reservation( reservation );
        section_failure( "anchor slot", addr, source, size, ENOMEM );
        errno = ENOMEM;
        return NULL;
    }
    if (map_code_memory_range( addr, source, size, PROT_READ | PROT_WRITE, ENOMEM ))
    {
        const int saved_errno = errno;

        if (reservation) remove_reservation( reservation );
        horizon_object_free( &mapping_pool, mapping );
        section_failure( "anchor", addr, source, size, saved_errno );
        errno = saved_errno;
        return NULL;
    }
    mapping->section_state = SECTION_ANCHOR;
    mapping->anchor_source = source;
    list_add_mapping( mapping );
    section_anchors++;
    section_anchor_bytes += size;
    *token = mapping;
    return addr;
}

static int horizon_section_unanchor( void *addr, void *source, size_t size, void *token )
{
    struct horizon_mapping *mapping = token;

    if (unmap_code_memory_range( addr, source, size ))
    {
        section_failure( "unanchor", addr, source, size, errno );
        return -1;
    }
    list_remove_mapping( mapping );
    if (mapping->reservation) remove_reservation( mapping->reservation );
    horizon_object_free( &mapping_pool, mapping );
    section_anchors--;
    section_anchor_bytes -= size;
    return 0;
}

static int horizon_section_alias( void *dst, void *src, size_t size )
{
    Result rc = svcMapProcessMemory( dst, envGetOwnProcessHandle(), (u64)(ULONG_PTR)src, size );

    if (R_FAILED(rc))
    {
        horizon_trace( "[HMAP] section alias failed dst=%p src=%p size=0x%lx rc=0x%x",
                       dst, src, (unsigned long)size, rc );
        WARN( "svcMapProcessMemory(%p, %p, %zu) failed %#x.\n", dst, src, size, rc );
        section_failure( "alias", dst, src, size, (int)rc );
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int horizon_section_unalias( void *dst, void *src, size_t size )
{
    Result rc = svcUnmapProcessMemory( dst, envGetOwnProcessHandle(), (u64)(ULONG_PTR)src, size );

    if (R_FAILED(rc))
    {
        horizon_trace( "[HMAP] section unalias failed dst=%p src=%p size=0x%lx rc=0x%x",
                       dst, src, (unsigned long)size, rc );
        WARN( "svcUnmapProcessMemory(%p, %p, %zu) failed %#x.\n", dst, src, size, rc );
        section_failure( "unalias", dst, src, size, (int)rc );
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* A range of a view of a section: its pages aliased from the section's
 * anchors, or a hole while it is PROT_NONE. Each range holds a reference to
 * the section, so views keep the memory after the section's handle and
 * descriptors are closed. The caller reserved the address, and counts an
 * aliased range as a user of its anchors. */
static struct horizon_mapping *alloc_section_range( void *addr, size_t size, struct horizon_memfile *section,
                                                    size_t offset, int prot, unsigned char state,
                                                    VirtmemReservation *reservation )
{
    struct horizon_mapping *mapping = alloc_mapping( addr, size, NULL, 0, reservation, prot );

    if (!mapping)
    {
        errno = ENOMEM;
        return NULL;
    }
    mapping->section = section;
    mapping->section_offset = offset;
    mapping->section_state = state;
    horizon_memfile_ref( section );
    return mapping;
}

/* Metadata only, once the range's pages are gone from its address and its
 * reservation is removed: an aliased range stops using its anchors. */
static void free_section_range( struct horizon_mapping *mapping )
{
    if (mapping->section_state == SECTION_ALIASED)
        horizon_memfile_use( mapping->section, mapping->section_offset, mapping->size, -1 );
    horizon_memfile_unref( mapping->section );
    horizon_object_free( &mapping_pool, mapping );
}

/* A piece of a range being split, reserved on its own. An aliased piece of an
 * aliased range uses the anchors its parent uses. */
static struct horizon_mapping *section_range_piece( struct horizon_mapping *parent, char *addr, size_t size,
                                                    unsigned char state, int prot, BOOL count_use )
{
    size_t offset = parent->section_offset + (addr - (char *)parent->addr);
    VirtmemReservation *reservation = reserve_fixed_range( addr, size );
    struct horizon_mapping *piece;

    if (!reservation) return NULL;
    if (!(piece = alloc_section_range( addr, size, parent->section, offset, prot, state, reservation )))
    {
        remove_reservation( reservation );
        return NULL;
    }
    if (count_use) horizon_memfile_use( piece->section, offset, size, 1 );
    return piece;
}

/* Gives [start, start + size) of a view range a new state: SECTION_ALIASED,
 * SECTION_HOLE, or SECTION_NONE to leave the address space. The kernel goes
 * first, so a refusal changes nothing; pieces count their use before the old
 * range stops counting, so no anchor a piece needs is removed in between. */
static int change_section_range( struct horizon_mapping *mapping, char *start, size_t size,
                                 unsigned char state, int prot )
{
    char *mapping_start = mapping->addr;
    char *mapping_end = mapping_start + mapping->size;
    char *end = start + size;
    struct horizon_memfile *section = mapping->section;
    const size_t offset = mapping->section_offset + (start - mapping_start);
    const BOOL aliased = mapping->section_state == SECTION_ALIASED;
    struct horizon_mapping *left = NULL, *middle = NULL, *right = NULL;
    int saved_errno;

    if (mapping->section_state == state) return 0;

    if (aliased)
    {
        if (horizon_memfile_alias( section, start, offset, size, 0 )) return -1;
    }
    else if (state == SECTION_ALIASED)
    {
        if (horizon_memfile_map( section, start, offset, size )) return -1;
    }

    list_remove_mapping( mapping );
    remove_reservation( mapping->reservation );
    if (mapping_start < start &&
        !(left = section_range_piece( mapping, mapping_start, start - mapping_start,
                                      mapping->section_state, mapping->prot, aliased )))
        goto failed;
    if (state != SECTION_NONE && !(middle = section_range_piece( mapping, start, size, state, prot, FALSE )))
        goto failed;
    if (end < mapping_end &&
        !(right = section_range_piece( mapping, end, mapping_end - end,
                                       mapping->section_state, mapping->prot, aliased )))
        goto failed;

    if (left) list_add_mapping( left );
    if (middle) list_add_mapping( middle );
    if (right) list_add_mapping( right );
    free_section_range( mapping );
    return 0;

failed:
    /* Out of mapping slots or reservations: put the range back as it was. */
    saved_errno = errno;
    if (left)
    {
        remove_reservation( left->reservation );
        free_section_range( left );
    }
    if (middle)
    {
        middle->section_state = SECTION_HOLE;  /* its use, if any, is undone below */
        remove_reservation( middle->reservation );
        free_section_range( middle );
    }
    if (aliased) horizon_memfile_alias( section, start, offset, size, 1 );
    else if (state == SECTION_ALIASED)
    {
        horizon_memfile_alias( section, start, offset, size, 0 );
        horizon_memfile_use( section, offset, size, -1 );
    }
    if (!(mapping->reservation = reserve_fixed_range( mapping_start, mapping_end - mapping_start )))
        WARN( "lost the reservation of a view of a section at %p-%p.\n", mapping_start, mapping_end );
    list_add_mapping( mapping );
    errno = saved_errno;
    return -1;
}

/* Horizon cannot change the protection of process memory mapped from other
 * pages, so an aliased range stays read-write; only PROT_NONE takes its pages
 * away, and anything else maps them back. */
static int protect_section_range( struct horizon_mapping *mapping, char *start, size_t size, int prot )
{
    static int reported;

    if (prot != PROT_NONE && prot != (PROT_READ | PROT_WRITE) && !reported)
    {
        reported = 1;
        wine_nx_runtime_trace( "[HMAP] views of sections with no file stay read-write: Horizon cannot "
                               "change the protection of memory mapped from other pages" );
    }
    return change_section_range( mapping, start, size, prot == PROT_NONE ? SECTION_HOLE : SECTION_ALIASED, prot );
}

static int unmap_range_locked( void *addr, size_t size )
{
    char *start = addr;
    char *end = start + size;
    struct horizon_mapping *mapping;

    while ((mapping = find_overlap_mapping( start, end - start )))
    {
        char *mapping_start = mapping->addr;
        char *mapping_end = mapping_start + mapping->size;
        char *unmap_start = max( start, mapping_start );
        char *unmap_end = min( end, mapping_end );
        size_t unmap_size = unmap_end - unmap_start;

        if (mapping->section_state == SECTION_ANCHOR)
        {
            errno = EBUSY;  /* pages views of a section are mapped from */
            return -1;
        }
        if (mapping->section)
        {
            if (change_section_range( mapping, unmap_start, unmap_size, SECTION_NONE, 0 )) return -1;
        }
        else if (mapping->reservation)
        {
            if (split_reservation_mapping( mapping, unmap_start, unmap_size )) return -1;
        }
        else if (split_backing_mapping( mapping, unmap_start, unmap_size )) return -1;
    }

    return 0;
}

static int protect_range_locked( void *addr, size_t size, int prot )
{
    char *start = addr;
    char *end = start + size;
    struct horizon_mapping *mapping;

    while (start < end)
    {
        char *mapping_start;
        char *mapping_end;
        char *protect_end;
        size_t protect_size;

        mapping = find_overlap_mapping( start, end - start );
        if (!mapping || mapping->addr > (void *)start)
        {
            errno = ENOMEM;
            return -1;
        }

        mapping_start = mapping->addr;
        mapping_end = mapping_start + mapping->size;
        protect_end = min( end, mapping_end );
        protect_size = protect_end - start;

        if (mapping->section_state == SECTION_ANCHOR)
        {
            errno = ENOMEM;
            return -1;
        }
        if (mapping->section)
        {
            if (protect_section_range( mapping, start, protect_size, prot )) return -1;
        }
        else if (mapping->reservation)
        {
            if (prot != PROT_NONE)
            {
                /* Commit a whole chunk of the reservation at once. Each
                 * mapped range costs kernel memory blocks, and Horizon has
                 * 20000 for every application together
                 * (ApplicationMemoryBlockSlabHeapSize); Fallout New Vegas
                 * commits its heap a page at a time, so a mapping a page ran
                 * svcMapProcessCodeMemory out of them (ResultOutOfResource),
                 * after which every commit failed and the program retried for
                 * ever on one core. The chunk is mapped inaccessible, so pages
                 * the program has not committed still fault, and committing
                 * any of them -- now or later -- is a permission change on
                 * memory that is already mapped. */
                char *chunk_start = (char *)((uintptr_t)start & ~(uintptr_t)(HORIZON_COMMIT_CHUNK - 1));
                char *chunk_end = (char *)(((uintptr_t)protect_end + HORIZON_COMMIT_CHUNK - 1) &
                                           ~(uintptr_t)(HORIZON_COMMIT_CHUNK - 1));

                if (chunk_start < mapping_start) chunk_start = mapping_start;
                if (chunk_end > mapping_end || chunk_end < protect_end) chunk_end = mapping_end;

                horizon_trace( "[HMAP] commit reservation=%p/0x%lx range=%p/0x%lx chunk=%p/0x%lx prot=0x%x",
                               mapping->addr, (unsigned long)mapping->size, start,
                               (unsigned long)protect_size, chunk_start,
                               (unsigned long)(chunk_end - chunk_start), prot );
                if (replace_reservation_mapping( mapping, chunk_start, chunk_end - chunk_start,
                                                 PROT_NONE, TRUE ))
                {
                    horizon_trace( "[HMAP] commit reservation failed range=%p/0x%lx prot=%#x errno=%d",
                                   start, (unsigned long)protect_size, prot, errno );
                    return -1;
                }
                /* The chunk is mapped now, so this takes the branch below. */
                if (protect_range_locked( start, protect_size, prot )) return -1;
            }
        }
        else
        {
            if (!(mapping = split_backing_mapping_metadata( mapping, start, protect_size ))) return -1;
            if (protect_code_mapping( mapping, prot )) return -1;
        }

        start = protect_end;
    }

    return 0;
}

/* horizon_region_query over svcQueryMemory. No locks and no tracing: it runs under virtmemLock. */
static int horizon_query_region( void *context, unsigned long long addr, struct horizon_region *region )
{
    MemoryInfo info;
    u32 page_info;

    (void)context;
    if (R_FAILED( svcQueryMemory( &info, &page_info, addr ) )) return 0;
    region->addr = info.addr;
    region->size = info.size;
    region->type = info.type;
    return 1;
}

static int add_reservation_mapping_locked( void *start, size_t size )
{
    VirtmemReservation *reservation;
    struct horizon_mapping *mapping;

    /* A free kernel block can still lie below the permitted mapping region.
     * Reject it at reserve time, before the game builds a heap there. */
    void *region_start, *region_limit;
    horizon_get_address_space_limits( &region_start, &region_limit );
    if ((uintptr_t)start < (uintptr_t)region_start ||
        (uintptr_t)start >= (uintptr_t)region_limit ||
        size > (uintptr_t)region_limit - (uintptr_t)start)
    {
        errno = EINVAL;
        return -1;
    }

    /* see horizon_kernel_regions: this would claim pages the kernel gave away */
    if (horizon_overlaps_kernel_region( start, size ))
    {
        errno = EEXIST;
        return -1;
    }
    /* And what libnx mapped outside Wine's views - thread stacks, JIT code - which
     * a 32-bit address space puts in the same low gigabyte (horizon_free_range.h).
     * virtmemLock is held, so no libnx thread can map a stack in the meantime. */
    if (!horizon_range_unmapped( (unsigned long long)(uintptr_t)start, size, horizon_query_region, NULL ))
    {
        errno = EEXIST;
        return -1;
    }
    reservation = reserve_fixed_range_locked( start, size );
    if (!reservation) return -1;
    if (!(mapping = alloc_mapping( start, size, NULL, 0, reservation, PROT_NONE )))
    {
        remove_reservation_locked( reservation );
        errno = ENOMEM;
        return -1;
    }
    list_add_mapping( mapping );
    return 0;
}

static void *horizon_mmap_fixed( void *start, size_t size, int prot, int flags, int fd, off_t offset )
{
    BOOL anonymous = fd == -1;
    VirtmemReservation *transition;
    void *requested = start;
    const char *stage = "transition reservation";
    int saved_errno;

    size = page_align_size( size );
    if (!start || (ULONG_PTR)start & 0xfff || !size)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    pthread_mutex_lock( &mapping_mutex );
    /* Keep the target unavailable to native allocators while old mappings
     * are removed and the replacement is installed. libnx permits temporary
     * overlapping reservations; the final mapping owns its own reservation. */
    if (!(transition = reserve_fixed_range( start, size ))) goto failed;
    stage = "unmap";
    if (unmap_range_locked( start, size ))
    {
        goto failed;
    }

    if (anonymous && prot == PROT_NONE)
    {
        int ret;

        stage = "reserve unmapped range";
        virtmemLock();
        ret = add_reservation_mapping_locked( start, size );
        virtmemUnlock();
        if (ret) start = MAP_FAILED;
    }
    else
    {
        stage = "map backing";
        if (map_backing_at( start, size, prot, fd, offset, flags, EINVAL )) start = MAP_FAILED;
    }

    if (start == MAP_FAILED) goto failed;
    remove_reservation( transition );
    pthread_mutex_unlock( &mapping_mutex );
    return start;

failed:
    saved_errno = errno;
    if (transition) remove_reservation( transition );
    pthread_mutex_unlock( &mapping_mutex );
    {
        static LONG failures;
        char msg[352];

        if (__atomic_add_fetch( &failures, 1, __ATOMIC_RELAXED ) <= 16)
        {
            /* What is in the way, since the address was asked for and no other
             * will do: the kernel's own word on the block that starts there, and
             * this program's mapping of it when it is one of ours. A run that
             * places a stack or an arena where a program later wants its own
             * memory fails here and nowhere else. */
            MemoryInfo info = {0};
            u32 page_info = 0;
            struct horizon_mapping *held;
            char blocker[128] = "";
            u64 at = (u64)(uintptr_t)requested, end = at + size;

            /* The first block in the range that is not free, which is the one
             * refusing it: the range often begins with free pages and runs into
             * something further along. */
            while (at < end && R_SUCCEEDED( svcQueryMemory( &info, &page_info, at ) ))
            {
                if (info.type != MemType_Unmapped)
                {
                    snprintf( blocker, sizeof(blocker), " in the way: %010llx+%llx type=%u perm=%u attr=%u",
                              (unsigned long long)info.addr, (unsigned long long)info.size,
                              (unsigned)info.type, (unsigned)info.perm, (unsigned)info.attr );
                    break;
                }
                if (!info.size || info.addr + info.size <= at) break;
                at = info.addr + info.size;
            }
            pthread_mutex_lock( &mapping_mutex );
            if ((held = find_overlap_mapping( requested, size )))
                snprintf( blocker + strlen( blocker ), sizeof(blocker) - strlen( blocker ),
                          ", ours %p+%lx", held->addr, (unsigned long)held->size );
            pthread_mutex_unlock( &mapping_mutex );
            snprintf( msg, sizeof(msg), "[HMAP] fixed replacement failed: %s addr=%p size=0x%lx prot=%#x errno=%d%s",
                      stage, requested, (unsigned long)size, prot, saved_errno, blocker );
            wine_nx_runtime_trace( msg );
        }
    }
    errno = saved_errno;
    return MAP_FAILED;
}

void *horizon_anon_mmap_fixed( void *start, size_t size, int prot, int flags )
{
    return horizon_mmap_fixed( start, size, prot, flags, -1, 0 );
}

static void *horizon_mmap_tryfixed( void *start, size_t size, int prot, int flags, int fd, off_t offset )
{
    BOOL anonymous = fd == -1;
    void *ret = start;

    size = page_align_size( size );
    if (!start || (ULONG_PTR)start & 0xfff || !size)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    pthread_mutex_lock( &mapping_mutex );
    if (find_overlap_mapping( start, size ))
    {
        errno = EEXIST;
        ret = MAP_FAILED;
    }
    else if (anonymous && prot == PROT_NONE)
    {
        virtmemLock();
        if (add_reservation_mapping_locked( start, size )) ret = MAP_FAILED;
        virtmemUnlock();
    }
    else if (map_backing_at( start, size, prot, fd, offset, flags, EEXIST ))
    {
        ret = MAP_FAILED;
    }
    pthread_mutex_unlock( &mapping_mutex );
    return ret;
}

static void *horizon_mmap_alloc( size_t size, int prot, int flags, int fd, off_t offset )
{
    BOOL anonymous = fd == -1;
    void *addr;
    int ret;

    size = page_align_size( size );
    if (!size)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    pthread_mutex_lock( &mapping_mutex );
    virtmemLock();
    addr = virtmemFindCodeMemory( size, 0x1000 );
    if (!addr)
    {
        errno = ENOMEM;
        ret = -1;
    }
    else if (anonymous && prot == PROT_NONE)
    {
        ret = add_reservation_mapping_locked( addr, size );
    }
    else
    {
        ret = map_backing_at_locked( addr, size, prot, fd, offset, flags, ENOMEM );
    }
    virtmemUnlock();
    pthread_mutex_unlock( &mapping_mutex );

    return ret ? MAP_FAILED : addr;
}

void *horizon_anon_mmap_alloc( size_t size, int prot )
{
    return horizon_mmap_alloc( size, prot, MAP_PRIVATE | MAP_ANON, -1, 0 );
}

/* A view of a section with no file: its pages are the section's. */
static void *horizon_mmap_section( void *start, size_t size, int prot, int flags,
                                   struct horizon_memfile *section, off_t offset )
{
    const unsigned char state = prot == PROT_NONE ? SECTION_HOLE : SECTION_ALIASED;
    VirtmemReservation *reservation = NULL;
    struct horizon_mapping *mapping;
    void *addr = NULL;

    size = page_align_size( size );
    if (!size || ((ULONG_PTR)start & 0xfff) || offset < 0 || (offset & 0xfff))
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    pthread_mutex_lock( &mapping_mutex );
    if ((flags & MAP_FIXED) && (!start || unmap_range_locked( start, size )))
    {
        if (!start) errno = EINVAL;
        goto failed;
    }
    if (start && !find_overlap_mapping( start, size ) && (reservation = reserve_fixed_range( start, size )))
        addr = start;
    else if (start && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
    {
        errno = (flags & MAP_FIXED) ? ENOMEM : EEXIST;
        goto failed;
    }
    else
    {
        virtmemLock();
        if ((addr = virtmemFindCodeMemory( size, 0x1000 ))) reservation = reserve_fixed_range_locked( addr, size );
        virtmemUnlock();
        if (!reservation)
        {
            errno = ENOMEM;
            goto failed;
        }
    }

    if (state == SECTION_ALIASED && horizon_memfile_map( section, addr, offset, size ))
    {
        const int saved_errno = errno;

        remove_reservation( reservation );
        errno = saved_errno;
        goto failed;
    }
    if (!(mapping = alloc_section_range( addr, size, section, offset, prot, state, reservation )))
    {
        if (state == SECTION_ALIASED)
        {
            horizon_memfile_alias( section, addr, offset, size, 0 );
            horizon_memfile_use( section, offset, size, -1 );
        }
        remove_reservation( reservation );
        errno = ENOMEM;
        goto failed;
    }
    list_add_mapping( mapping );
    section_view_maps++;
    pthread_mutex_unlock( &mapping_mutex );
    return addr;

failed:
    pthread_mutex_unlock( &mapping_mutex );
    return MAP_FAILED;
}

void *horizon_mmap( void *start, size_t size, int prot, int flags, int fd, off_t offset )
{
    struct horizon_memfile *section;

    if (flags & MAP_ANON) fd = -1;
    else if (fd == -1)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    else if ((flags & MAP_SHARED) && (section = horizon_memfile_from_fd( fd )))
    {
        static int reported;

        if (!check_section_syscalls()) return horizon_mmap_section( start, size, prot, flags, section, offset );
        if (!reported)
        {
            reported = 1;
            wine_nx_runtime_trace( "[HMAP] views of sections with no file are copies: the loader does not "
                                   "grant svcMapProcessMemory" );
        }
    }

    if (flags & MAP_FIXED) return horizon_mmap_fixed( start, size, prot, flags, fd, offset );
    if (start)
    {
        void *ret = horizon_mmap_tryfixed( start, size, prot, flags, fd, offset );

        /* MAP_FIXED_NOREPLACE wants that address or nothing. Wine's free-area
         * search probes with it, and a mapping made elsewhere instead only cost
         * an allocation and an unmap before its next probe. */
        if (ret != MAP_FAILED || errno != EEXIST || (flags & MAP_FIXED_NOREPLACE)) return ret;
    }
    return horizon_mmap_alloc( size, prot, flags, fd, offset );
}

int horizon_munmap( void *start, size_t size )
{
    int ret;

    size = page_align_size( size );
    if (!start || (ULONG_PTR)start & 0xfff || !size)
    {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock( &mapping_mutex );
    ret = unmap_range_locked( start, size );
    pthread_mutex_unlock( &mapping_mutex );
    return ret;
}

int horizon_mprotect( void *start, size_t size, int prot )
{
    int ret;

    size = page_align_size( size );
    if (!start || (ULONG_PTR)start & 0xfff || !size)
    {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock( &mapping_mutex );
    ret = protect_range_locked( start, size, prot );
    pthread_mutex_unlock( &mapping_mutex );
    return ret;
}

int horizon_madvise( void *start, size_t size, int advice )
{
    TRACE( "madvise(%p, %zu, %#x) ignored.\n", start, size, advice );
    return 0;
}

#endif /* __SWITCH__ */
