#!/usr/bin/env python3
"""Exercise CPU-mode routing and bounds in FEX's code-map registration."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/virtual.c').read_text()
start = source.index('NTSTATUS wine_nx_set_fex_code_range(')
end = source.index('\n}\n', start) + 3
function = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
typedef int NTSTATUS, BOOL;
typedef uintptr_t ULONG_PTR, SIZE_T;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER 1
#define STATUS_INVALID_ADDRESS 2
#define STATUS_NO_MEMORY 3
#define VPROT_READ 1
#define VPROT_WRITE 2
#define VPROT_COMMITTED 4
#define ROUND_ADDR(a,b) ((void *)((uintptr_t)(a) & ~(b)))
#define ROUND_SIZE(a,b,c) (((b) + ((a) & (c)) + (c)) & ~(c))
static const SIZE_T page_mask = 4095, page_shift = 12;
static struct file_view { void *base; SIZE_T size; } view, *arm64ec_view;
static _Alignas(4096) unsigned char bitmap[4096];
static pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
static int arm64ec, locks, commits, marked, cleared, commit_ok = 1;
static int is_arm64ec(void) { return arm64ec; }
static void server_enter_uninterrupted_section(pthread_mutex_t *m, sigset_t *s)
{ (void)s; assert(!pthread_mutex_lock(m)); locks++; }
static void server_leave_uninterrupted_section(pthread_mutex_t *m, sigset_t *s)
{ (void)s; assert(locks); locks--; assert(!pthread_mutex_unlock(m)); }
static int set_vprot(struct file_view *v, void *b, SIZE_T s, int prot)
{
    assert(v == &view && b == bitmap && s == sizeof(bitmap) && prot == 7);
    commits++;
    return commit_ok;
}
static void set_arm64ec_range(void *b, SIZE_T s) { assert(b && s); marked++; }
static void clear_arm64ec_range(void *b, SIZE_T s) { assert(b && s); cleared++; }
'''
tests = r'''
int main(void)
{
    void *code = (void *)0x100000;
    assert(wine_nx_set_fex_code_range(code, 4096, 1) == STATUS_SUCCESS);
    assert(wine_nx_set_fex_code_range(code, 4096, 0) == STATUS_SUCCESS);
    assert(!commits && !marked && !cleared && !locks);
    assert(wine_nx_set_fex_code_range(code, 0, 1) == STATUS_INVALID_PARAMETER);
    assert(wine_nx_set_fex_code_range((void *)1, 4096, 1) == STATUS_INVALID_PARAMETER);
    assert(wine_nx_set_fex_code_range(code, 1, 1) == STATUS_INVALID_PARAMETER);
    assert(wine_nx_set_fex_code_range((void *)(UINTPTR_MAX - 4095), 8192, 1) == STATUS_INVALID_PARAMETER);
    arm64ec = 1;
    assert(wine_nx_set_fex_code_range(code, 4096, 1) == STATUS_INVALID_ADDRESS);
    view.base = bitmap; view.size = sizeof(bitmap); arm64ec_view = &view;
    assert(wine_nx_set_fex_code_range(code, 8192, 1) == STATUS_SUCCESS);
    assert(wine_nx_set_fex_code_range(code, 8192, 0) == STATUS_SUCCESS);
    assert(commits == 2 && marked == 1 && cleared == 1 && !locks);
    commit_ok = 0;
    assert(wine_nx_set_fex_code_range(code, 4096, 1) == STATUS_NO_MEMORY);
    assert(marked == 1 && !locks);
    assert(wine_nx_set_fex_code_range((void *)0x8000000, 4096, 1) == STATUS_INVALID_ADDRESS);
    puts("FEX memory: WoW64 without an EC bitmap, ARM64EC registration, bounds and failed commits passed");
}
'''
with tempfile.TemporaryDirectory(prefix='fex-memory-') as directory:
    path = Path(directory)
    (path / 'memory.c').write_text(fixture + function + tests)
    cc = os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang')
    subprocess.run([cc, '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined',
                    str(path / 'memory.c'), '-o', str(path / 'memory')], check=True)
    subprocess.run([str(path / 'memory')], check=True)
