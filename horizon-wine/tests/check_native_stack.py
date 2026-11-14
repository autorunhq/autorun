#!/usr/bin/env python3
"""Exercise native stack binding with the production Horizon helper."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
start = source.index('void horizon_bind_native_stack(')
end = source.index('\n}', start) + 2
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t ULONG_PTR;
typedef struct { struct { void *StackBase, *StackLimit; } Tib;
    void *DeallocationStack, *CpuReserved; unsigned guest_stack[2]; } TEB;
typedef struct { void *stack_mirror; size_t stack_sz; } Thread;
static Thread current_thread;
static int have_thread = 1, traces;
static Thread *threadGetSelf(void) { return have_thread ? &current_thread : NULL; }
#define horizon_trace(...) (++traces)
'''
tests = r'''
int main(void) {
    TEB teb = {{(void *)0x1001ffd20ULL, (void *)0x100102000ULL},
               (void *)0x100100000ULL, (void *)0x1001ffd20ULL, {0x300000, 0x200000}};
    TEB original = teb;
    uintptr_t local = (uintptr_t)&teb;
    current_thread.stack_mirror = (void *)(local - 65536);
    current_thread.stack_sz = 131072;
    horizon_bind_native_stack(&teb);
    assert(teb.Tib.StackLimit == current_thread.stack_mirror);
    assert((uintptr_t)teb.Tib.StackBase == local + 65536);
    assert(teb.DeallocationStack == original.DeallocationStack);
    assert(teb.CpuReserved == original.CpuReserved);
    assert(!memcmp(teb.guest_stack, original.guest_stack, sizeof(teb.guest_stack)));
    assert(traces == 1);
    horizon_bind_native_stack(&teb);
    assert(traces == 1);
    TEB bound = teb;
    current_thread.stack_mirror = (void *)0x2000;
    current_thread.stack_sz = 0x1000;
    horizon_bind_native_stack(&teb); /* Do not describe an unrelated stack. */
    assert(!memcmp(&teb, &bound, sizeof(teb)));
    current_thread.stack_mirror = (void *)(UINTPTR_MAX - 16);
    current_thread.stack_sz = 32;
    horizon_bind_native_stack(&teb); /* Reject overflow. */
    assert(!memcmp(&teb, &bound, sizeof(teb)));
    have_thread = 0;
    horizon_bind_native_stack(&teb);
    horizon_bind_native_stack(NULL);
    assert(!memcmp(&teb, &bound, sizeof(teb)));
    puts("PASS: native bounds, repeated entry, allocation/CPU/guest preservation, invalid ranges");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-native-stack-') as temp:
    src, exe = Path(temp) / 'test.c', Path(temp) / 'test'
    src.write_text(fixture + source[start:end] + tests)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    # The helper asks whether its own frame is inside the thread's stack. The
    # address sanitizer may keep locals on a stack of its own, away from it.
    subprocess.run([str(exe)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_stack_use_after_return=0'})
