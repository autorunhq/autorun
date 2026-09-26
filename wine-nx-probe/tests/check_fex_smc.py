#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
vm = (root / 'dlls/ntdll/unix/virtual.c').read_text()
host = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#define __SWITCH__ 1
#define WINE_NX_SWAP_POC 1
#define TRUE 1
#define FALSE 0
#define VPROT_READ 1
#define VPROT_WRITE 2
#define VPROT_EXEC 4
#define VPROT_WRITECOPY 8
#define VPROT_GUARD 16
#define VPROT_COMMITTED 32
#define VPROT_WRITEWATCH 64
#define VPROT_FEX_SMC 128
#define VPROT_SYSTEM 512
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER 1
#define STATUS_NOT_SUPPORTED 2
#define SECTION_NONE 0
#define Perm_Rw 3
#define R_SUCCEEDED(rc) (!(rc))
#define R_FAILED(rc) ((rc) != 0)
typedef int BOOL, Result, NTSTATUS;
typedef uint8_t BYTE;
typedef uintptr_t ULONG_PTR;
typedef uint64_t u64;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
static const uintptr_t host_page_mask = 4095, host_page_size = 4096;
static struct backing { int write_back; struct { int detached; } swap; int swap_excluded; } backing;
static struct horizon_mapping {
    void *addr;
    size_t size;
    int prot;
    struct backing *backing;
    void *reservation;
    int section_state;
} mapping;
static struct file_view { unsigned int protect; } view;
static BYTE page_prot;
static int arm64ec, native, pinned, failure, splits, sets, process_sets, physical, code_static;
static struct horizon_mapping *find_overlap_mapping(void *base, size_t size)
{ assert(base == (void *)0x400000 && size == 4096); return &mapping; }
static int swap_pinned_locked(void *base, size_t size) { (void)base; (void)size; return pinned; }
static struct horizon_mapping *split_backing_mapping_metadata(struct horizon_mapping *m, void *b, size_t s)
{ (void)b; (void)s; splits++; return m; }
static int get_horizon_perm(int prot) { return prot; }
static int svcSetMemoryPermission(void *base, size_t size, int perm)
{
    assert(base == mapping.addr && size == 4096 && !(perm & PROT_EXEC));
    sets++;
    if (failure || code_static) return 1;
    physical = perm;
    return 0;
}
static int envGetOwnProcessHandle(void) { return 1; }
static int svcSetProcessMemoryPermission(int handle, u64 base, size_t size, int perm)
{
    assert(handle == 1 && base == (uintptr_t)mapping.addr && size == 4096 && perm == Perm_Rw);
    process_sets++;
    if (failure) return 1;
    code_static = 0;
    physical = perm;
    return 0;
}
static struct file_view *find_view(void *base, size_t size) { (void)base; (void)size; return &view; }
static BYTE get_host_page_vprot(void *base) { (void)base; return page_prot; }
static void set_page_vprot(void *base, size_t size, BYTE prot) { (void)base; (void)size; page_prot = prot; }
static int is_arm64ec(void) { return arm64ec; }
static int is_emulated_code(ULONG_PTR base) { (void)base; return !native; }
static void server_enter_uninterrupted_section(pthread_mutex_t *m, sigset_t *s)
{ (void)s; assert(!pthread_mutex_lock(m)); }
static void server_leave_uninterrupted_section(pthread_mutex_t *m, sigset_t *s)
{ (void)s; assert(!pthread_mutex_unlock(m)); }
'''

tests = r'''
static void reset(void)
{
    backing = (struct backing){0};
    mapping = (struct horizon_mapping){.addr = (void *)0x400000, .size = 4096,
        .prot = PROT_READ | PROT_WRITE, .backing = &backing};
    page_prot = VPROT_READ | VPROT_WRITE | VPROT_EXEC | VPROT_COMMITTED;
    arm64ec = 1; native = pinned = failure = splits = sets = process_sets = code_static = 0;
    physical = PROT_READ | PROT_WRITE;
    view.protect = 0;
}
int main(void)
{
    void *base = (void *)0x400000;
    reset();
    assert(wine_nx_fex_protect_code((void *)1, 1) == STATUS_INVALID_PARAMETER);
    assert(!wine_nx_fex_protect_code(base, 1));
    assert(physical == PROT_READ && (page_prot & 15) == 7 && (page_prot & VPROT_FEX_SMC));
    assert(get_unix_prot(page_prot) == PROT_READ && sets == 1 && backing.swap_excluded);
    assert(!wine_nx_fex_protect_code(base, 1) && sets == 1);
    assert(!wine_nx_fex_protect_code(base, 0));
    assert(physical == (PROT_READ | PROT_WRITE) && (page_prot & 15) == 7 && !(page_prot & VPROT_FEX_SMC));
    assert(!wine_nx_fex_protect_code(base, 0) && sets == 2);
    reset();
    code_static = 1; mapping.prot = physical = PROT_READ | PROT_EXEC;
    page_prot |= VPROT_WRITEWATCH;
    assert(!wine_nx_fex_protect_code(base, 1) && !sets);
    assert(!wine_nx_fex_protect_code(base, 0) && sets == 1 && process_sets == 1);
    assert(!(page_prot & (VPROT_FEX_SMC | VPROT_WRITEWATCH)));
    assert(!wine_nx_fex_protect_code(base, 1) && physical == PROT_READ && process_sets == 1);
    reset();
    failure = 1;
    BYTE original = page_prot;
    assert(wine_nx_fex_protect_code(base, 1) == STATUS_NOT_SUPPORTED);
    assert(page_prot == original && physical == (PROT_READ | PROT_WRITE));
    for (unsigned i = 0; i < 9; ++i) {
        reset();
        switch (i) {
        case 0: native = 1; break;
        case 1: page_prot |= VPROT_GUARD; break;
        case 2: view.protect = VPROT_WRITEWATCH; break;
        case 3: page_prot &= ~VPROT_WRITE; break;
        case 4: mapping.section_state = 1; break;
        case 5: backing.write_back = 1; break;
        case 6: pinned = 1; break;
        case 7: backing.swap.detached = 1; break;
        case 8: mapping.reservation = base; break;
        }
        assert(wine_nx_fex_protect_code(base, 1) == STATUS_NOT_SUPPORTED);
        assert(!sets && !(page_prot & VPROT_FEX_SMC));
    }
    reset();
    arm64ec = 0;
    assert(!wine_nx_fex_protect_code(base, 1));
    failure = 1;
    assert(wine_nx_fex_protect_code(base, 0) == STATUS_NOT_SUPPORTED);
    assert(page_prot & VPROT_FEX_SMC);
    puts("FEX SMC: host permissions, guest flags, private/shared/native exclusions and failed transitions passed");
}
'''

with tempfile.TemporaryDirectory(prefix='fex-smc-') as directory:
    path = Path(directory)
    (path / 'smc.c').write_text(fixture + function(vm, 'static int get_unix_prot(') +
                              function(host, 'int horizon_protect_fex_page(') +
                              function(vm, 'NTSTATUS wine_nx_fex_protect_code(') + tests)
    cc = os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang')
    subprocess.run([cc, '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined',
                    str(path / 'smc.c'), '-o', str(path / 'smc')], check=True)
    subprocess.run([str(path / 'smc')], check=True)
