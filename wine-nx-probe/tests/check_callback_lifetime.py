#!/usr/bin/env python3
"""Exercise the actual Horizon callback bridge with mutating/nested callbacks."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/signal_arm64.c').read_text()
baseline = '--baseline' in sys.argv
if baseline:
    source = subprocess.check_output(['git', '-C', str(root), 'show',
                                      'HEAD:dlls/ntdll/unix/signal_arm64.c'], text=True)
frames = source[source.index('struct switch_callback_frame\n'):source.index('/* call a PE callback:')]
calls = source[source.index('NTSTATUS KeUserModeCallback('):source.index('NTSTATUS get_thread_ldt_entry(')]
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WINAPI
typedef unsigned int ULONG, NTSTATUS;
typedef uintptr_t ULONG_PTR;
typedef NTSTATUS (*KERNEL_CALLBACK_PROC)(void *, ULONG);
typedef struct { KERNEL_CALLBACK_PROC *KernelCallbackTable; } PEB;
typedef struct { PEB *Peb; } TEB;
#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define STATUS_NO_MEMORY 0xc0000017u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_NO_CALLBACK_ACTIVE 0xc0000258u
static KERNEL_CALLBACK_PROC table[4];
static PEB peb = { table };
static TEB teb = { &peb };
static TEB *NtCurrentTeb(void) { return &teb; }
static int is_arm64ec(void) { return 0; }
/* Stack binding is exercised separately by check_native_stack.py. */
void horizon_bind_native_stack(TEB *t) { assert(t == &teb); }
static ULONG_PTR wine_nx_call_pe_callback(void *func, const void *args, ULONG len, void *t) {
    assert(t == &teb); return ((KERNEL_CALLBACK_PROC)func)((void *)args, len);
}
'''
tests = r'''
static NTSTATUS mutate(void *args, ULONG len) {
    assert(len == 64);
    memset(args, 0x65, len);
    return NtCallbackReturn(args, len, 0);
}
static NTSTATUS stack_result(void *args, ULONG len) {
    unsigned char result[256];
    (void)args; (void)len;
    memset(result, 0x7b, sizeof(result));
    return NtCallbackReturn(result, sizeof(result), 0);
}
static NTSTATUS nested(void *args, ULONG len) {
    void *result; ULONG size;
    assert(!KeUserModeCallback(1, NULL, 0, &result, &size));
    assert(size == 256 && ((unsigned char *)result)[255] == 0x7b);
    return NtCallbackReturn(args, len, 0);
}
static NTSTATUS normal(void *args, ULONG len) { (void)args; (void)len; return 42; }
static void churn_stack(void) {
    volatile unsigned char scratch[8192];
    for (unsigned i = 0; i < sizeof(scratch); ++i) scratch[i] = 0xa5;
}
static void *test_thread(void *unused) {
    (void)unused;
    for (int round = 0; round < 50; ++round) {
        unsigned char *original = malloc(64);
        void *result; ULONG size;
        memset(original, 0x12, 64);
        assert(!KeUserModeCallback(0, original, 64, &result, &size));
        assert(result != original && size == 64);
        for (int i = 0; i < 64; ++i) assert(original[i] == 0x12);
        free(original); churn_stack();
        for (int i = 0; i < 64; ++i) assert(((unsigned char *)result)[i] == 0x65);
        assert(!KeUserModeCallback(1, NULL, 0, &result, &size));
        churn_stack();
        assert(size == 256);
        for (int i = 0; i < 256; ++i) assert(((unsigned char *)result)[i] == 0x7b);
        unsigned value = 0x12345678;
        assert(!KeUserModeCallback(2, &value, sizeof(value), &result, &size));
        assert(size == sizeof(value) && *(unsigned *)result == value);
        assert(KeUserModeCallback(3, NULL, 0, &result, &size) == 42);
        assert(!result && !size);
    }
    assert(NtCallbackReturn(NULL, 0, 0) == STATUS_NO_CALLBACK_ACTIVE);
    return NULL; /* pthread key destructor frees retained callback buffers */
}
int main(void) {
    pthread_t threads[2];
    table[0] = mutate; table[1] = stack_result; table[2] = nested; table[3] = normal;
    for (int i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, test_thread, NULL));
    for (int i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    puts("Callback lifetime: input isolation, freed caller buffer, stack results, nesting, reuse and two threads passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-callback-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(fixture + frames + calls + tests)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-pthread', str(c), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=baseline, text=True)
    if baseline:
        assert result.returncode != 0, 'Old callback bridge unexpectedly passed'
        assert 'result != original' in result.stderr or 'heap-use-after-free' in result.stderr, result.stderr
        print('Baseline regression reproduced: callback result aliases the caller buffer')
    else:
        result.check_returncode()
