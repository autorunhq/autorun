#!/usr/bin/env python3
"""Host unit test for 416-byte kernel frame context decode, program_frame, and self NtGet/SetContextThread (0118)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
sig = (root / 'dlls/ntdll/unix/signal_arm64.c').read_text(encoding='utf-8')

def block(text, start_sig, end_sig):
    s = text.index(start_sig)
    e = text.index(end_sig, s)
    return text[s:e]

kernel_ctx_code = block(sig, '#define KERNEL_FRAME_SIZE', 'NTSTATUS signal_set_full_context(')
self_ctx_code = block(sig, 'NTSTATUS WINAPI NtSetContextThread(', 'static NTSTATUS check_current_thread_context_access(')

test_c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef uint16_t USHORT;
typedef uint32_t DWORD, ULONG, BOOL;
typedef uint64_t ULONG64, ULONG_PTR, DWORD64;
typedef void *HANDLE;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define IMAGE_FILE_MACHINE_ARM64 0xaa64
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001u)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bbu)

#define CONTEXT_ARM64 0x00400000u
#define CONTEXT_ARM64_CONTROL        (CONTEXT_ARM64 | 0x1u)
#define CONTEXT_ARM64_INTEGER        (CONTEXT_ARM64 | 0x2u)
#define CONTEXT_ARM64_FLOATING_POINT (CONTEXT_ARM64 | 0x4u)
#define CONTEXT_ARM64_X18            (CONTEXT_ARM64 | 0x10u)
#define CONTEXT_ARM64_FULL           (CONTEXT_ARM64_CONTROL | CONTEXT_ARM64_INTEGER | CONTEXT_ARM64_FLOATING_POINT)

typedef struct __attribute__((aligned(16)))
{
    unsigned __int128 V[32];
} ARM64_FPU;

typedef struct __attribute__((aligned(16)))
{
    DWORD ContextFlags;
    DWORD Cpsr;
    union {
        ULONG64 X[31];
        struct {
            ULONG64 pad[18];
            ULONG64 X18;
            ULONG64 pad2[10];
            ULONG64 Fp;
            ULONG64 Lr;
        };
    };
    ULONG64 Sp;
    ULONG64 Pc;
    unsigned __int128 V[32];
    DWORD Fpcr;
    DWORD Fpsr;
} CONTEXT;

typedef struct
{
    ULONG InSimulation;
    ULONG InSyscallCallback;
    ULONG64 EmulatorStackBase;
    ULONG64 EmulatorStackLimit;
} CHPE_V2_CPU_AREA_INFO;

typedef struct { CHPE_V2_CPU_AREA_INFO *ChpeV2CpuAreaInfo; } TEB;
static TEB teb;
static TEB *NtCurrentTeb(void) { return &teb; }
static int horizon_suspend_pending;
static int sim_in_kernel;
static unsigned sim_enter_calls, sim_leave_calls, sim_enter_null_calls;
static const void *sim_enter_changed[4];
static unsigned sim_enter_idx;
static const void *sim_leave_changed;
const void *wine_nx_kernel_enter(const void *f)
{
    if (!f) { sim_enter_null_calls++; return NULL; }
    sim_enter_calls++;
    sim_in_kernel = 1;
    return sim_enter_idx < 4 ? sim_enter_changed[sim_enter_idx++] : NULL;
}
const void *wine_nx_kernel_leave(void)
{
    sim_leave_calls++;
    sim_in_kernel = 0;
    return sim_leave_changed;
}
int wine_nx_suspend_requested(void) { return 0; }
void horizon_contexts_from_server(CONTEXT *c, const void *s) { memset(c, 0, sizeof(*c)); c->ContextFlags = CONTEXT_ARM64_FULL; c->Pc = (uintptr_t)s; }
__thread const void *wine_nx_kernel_frame;
static unsigned full_set_calls, capture_calls;
static CONTEXT last_full_set;
static NTSTATUS set_full_ret = STATUS_UNSUCCESSFUL;
static int set_full_leave_kernel = 0;

NTSTATUS signal_set_full_context(CONTEXT *context)
{
    if (set_full_leave_kernel && wine_nx_kernel_frame)
    {
        wine_nx_kernel_leave();
        wine_nx_kernel_frame = NULL;
    }
    full_set_calls++;
    last_full_set = *context;
    return set_full_ret;
}

static NTSTATUS set_thread_context(HANDLE h, const CONTEXT *c, BOOL *self, USHORT m)
{
    (void)h; (void)c;
    assert(m == IMAGE_FILE_MACHINE_ARM64);
    *self = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS get_thread_context(HANDLE h, CONTEXT *c, BOOL *self, USHORT m)
{
    (void)h; (void)c;
    assert(m == IMAGE_FILE_MACHINE_ARM64);
    *self = TRUE;
    return STATUS_SUCCESS;
}

static int horizon_capture_context(CONTEXT *c)
{
    (void)c;
    capture_calls++;
    return 0;
}

''' + kernel_ctx_code + '\n' + self_ctx_code + r'''

int main(void)
{
    _Alignas(16) ULONG_PTR frame[KERNEL_FRAME_WORDS];
    CONTEXT ctx;
    CHPE_V2_CPU_AREA_INFO area = {0};
    unsigned int i;

    _Static_assert(sizeof(frame) == 416, "frame size");
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xaaaa0029;
    frame[1] = 0xaaaa0030;
    for (i = 0; i < 4; i++) frame[2 + i] = 0xaaaa0019 + i;
    frame[6] = 0xaaaa0018;
    for (i = 0; i < 8; i++)
    {
        frame[12 + 2 * i] = 0x100 + i;
        frame[12 + 2 * i + 1] = 0x200 + i;
    }
    for (i = 0; i < 2; i++)
    {
        frame[28 + 2 * i] = 0x300 + i;
        frame[28 + 2 * i + 1] = 0x400 + i;
    }
    for (i = 0; i < 6; i++) frame[32 + i] = 0xaaaa0023 + i;
    frame[38] = 0xdeadbeefULL;
    for (i = 0; i < 9; i++) frame[40 + i] = 0xaaaa0000 + i;
    frame[49] = 0x01020304;
    frame[50] = 0x05060708;

    wine_nx_kernel_context(frame, &ctx);
    assert(ctx.ContextFlags == (CONTEXT_ARM64_FULL | CONTEXT_ARM64_X18));
    assert(ctx.Fp == 0xaaaa0029 && ctx.Lr == 0xaaaa0030);
    for (i = 0; i < 4; i++) assert(ctx.X[19 + i] == 0xaaaa0019 + i);
    for (i = 0; i < 6; i++) assert(ctx.X[23 + i] == 0xaaaa0023 + i);
    for (i = 0; i < 9; i++) assert(ctx.X[i] == 0xaaaa0000 + i);
    assert(ctx.X18 == 0xaaaa0018 && ctx.Pc == 0xdeadbeefULL);
    assert(ctx.Fpcr == 0x01020304 && ctx.Fpsr == 0x05060708);
    assert(ctx.Sp == (ULONG_PTR)(frame + KERNEL_FRAME_WORDS));
    for (i = 0; i < 8; i++)
    {
        uint64_t pair[2];
        memcpy(pair, &ctx.V[8 + i], sizeof(pair));
        assert(pair[0] == 0x100 + i && pair[1] == 0x200 + i);
    }
    for (i = 0; i < 2; i++)
    {
        uint64_t pair[2];
        memcpy(pair, &ctx.V[6 + i], sizeof(pair));
        assert(pair[0] == 0x300 + i && pair[1] == 0x400 + i);
    }

    /* program_frame checks */
    assert(program_frame(frame));
    horizon_suspend_pending = 1;
    assert(!program_frame(frame));
    horizon_suspend_pending = 0;

    teb.ChpeV2CpuAreaInfo = &area;
    area.EmulatorStackLimit = (ULONG_PTR)frame;
    area.EmulatorStackBase = (ULONG_PTR)(frame + KERNEL_FRAME_WORDS + 16);
    assert(!program_frame(frame));
    area.EmulatorStackBase = (ULONG_PTR)frame;
    assert(program_frame(frame));
    area.InSimulation = 1;
    assert(!program_frame(frame));
    area.InSimulation = 0;
    area.InSyscallCallback = 1;
    assert(!program_frame(frame));
    area.InSyscallCallback = 0;
    teb.ChpeV2CpuAreaInfo = NULL;

    /* Self NtGetContextThread / NtSetContextThread with and without wine_nx_kernel_frame. */
    wine_nx_kernel_frame = NULL;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_ARM64_CONTROL;
    assert(NtGetContextThread(NULL, &ctx) == STATUS_SUCCESS && capture_calls == 1);
    assert(NtSetContextThread(NULL, &ctx) == STATUS_NOT_SUPPORTED);

    wine_nx_kernel_frame = frame;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_ARM64_INTEGER;
    assert(NtGetContextThread(NULL, &ctx) == STATUS_SUCCESS && capture_calls == 1);
    assert(ctx.ContextFlags == CONTEXT_ARM64_INTEGER && ctx.X[0] == 0xaaaa0000 && ctx.Pc == 0xdeadbeefULL);

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_ARM64_CONTROL;
    ctx.Pc = 0x77778888ULL;
    ctx.Sp = 0x99990000ULL;
    ctx.Fp = 0x1111ULL;
    ctx.Lr = 0x2222ULL;
    ctx.Cpsr = 0x60000000u;
    assert(NtSetContextThread(NULL, &ctx) == STATUS_UNSUCCESSFUL && full_set_calls == 1);
    assert(last_full_set.Pc == 0x77778888ULL && last_full_set.Sp == 0x99990000ULL);
    assert(last_full_set.X[0] == 0xaaaa0000 && last_full_set.X18 == 0xaaaa0018);

    /* return_to_kernel with SetThreadContext change must leave kernel (clearing WINE_NX_STOP_KERNEL)
     * and if signal_set_full_context returns an error, re-enter the kernel frame cleanly. */
    set_full_leave_kernel = 1;
    sim_enter_calls = sim_leave_calls = sim_enter_null_calls = sim_enter_idx = 0;
    sim_in_kernel = 0;
    sim_leave_changed = NULL;
    sim_enter_changed[0] = (const void *)0x12340000ULL;
    sim_enter_changed[1] = NULL;
    {
        const void *outer = enter_kernel(frame);
        assert(outer == frame);
        assert(sim_enter_calls == 2 && sim_leave_calls == 1);
        assert(sim_in_kernel == 1 && wine_nx_kernel_frame == frame);
        {
            const void *cb_outer = enter_program();
            assert(cb_outer == frame && sim_leave_calls == 2 && sim_in_kernel == 0 && !wine_nx_kernel_frame);
            return_to_kernel(cb_outer);
            assert(sim_enter_calls == 3 && sim_in_kernel == 1 && wine_nx_kernel_frame == frame);
        }
        leave_kernel(outer);
        assert(sim_leave_calls == 3 && sim_in_kernel == 1 && wine_nx_kernel_frame == frame);
        wine_nx_kernel_leave();
        wine_nx_kernel_frame = NULL;
    }

    puts("kernel_context: 416-byte layout, program_frame, self NtGet/SetContextThread passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-kernel-ctx-') as directory:
    path = Path(directory)
    src = path / 'test.c'
    exe = path / 'test'
    src.write_text(test_c, encoding='utf-8')
    cc = os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang')
    subprocess.run([cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
