#!/usr/bin/env python3
"""Run Wine's early reservation, allocation and release code against native maps.

Protect a guest range before simulated libnx mappings fragment it, then reserve,
commit and release the 60,032 KiB block requested during NFSU2 race loading, and
the 172 MiB Most Wanted reserves in one piece.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('WINE_NX_VIRTUAL_SOURCE', root / 'dlls/ntdll/unix/virtual.c')).read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wine/list.h"
/* Exercise the Horizon branch even when compiled on macOS. */
#undef __APPLE__
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef int BOOL;
#define MAP_FAILED ((void *)-1)
#define MAP_NORESERVE 0
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define ROUND_ADDR(addr,mask) ((void *)((UINT_PTR)(addr) & ~(UINT_PTR)(mask)))
#define ROUND_SIZE(addr,size,mask) (((SIZE_T)(size) + ((UINT_PTR)(addr) & (mask)) + (mask)) & ~(UINT_PTR)(mask))
static const uintptr_t limit_4g = 0x100000000ull;
static const uintptr_t host_page_mask = 0xfff, granularity_mask = 0xffff;
static void *host_addr_space_limit = (void *)0x100000000ull;
static void *address_space_start = (void *)0x10000;
static struct list reserved_areas = LIST_INIT(reserved_areas);
struct native_map { uintptr_t start, end; };
static struct native_map native[4096];
static size_t native_count, committed, fixed_calls;
static int fixed_failures;
static uintptr_t stack_lo = 0x200000, stack_hi = 0x40000000;
static int stack_query_ok = 1;
static int horizon_get_stack_region(void **start, void **end)
{
    if (!stack_query_ok) return 0;
    *start = (void *)stack_lo;
    *end = (void *)stack_hi;
    return 1;
}
/* 4 GB unless a case says otherwise: the window keeps more of the stack region
 * only where the dynarec's code memory has nowhere else to go. */
static uintptr_t space_limit = 0x100000000ull;
static void horizon_get_address_space_limits(void **start, void **limit)
{
    *start = (void *)0x8000000ull;
    *limit = (void *)space_limit;
}
static int overlaps_native(uintptr_t start, size_t size)
{
    for (size_t i = 0; i < native_count; i++)
        if (start < native[i].end && native[i].start < start + size) return 1;
    return 0;
}
static void *anon_mmap_tryfixed(void *addr, size_t size, int prot, int flags)
{
    assert(prot == PROT_NONE);
    (void)flags;
    if (overlaps_native((uintptr_t)addr, size)) return MAP_FAILED;
    assert(native_count < 4096);
    native[native_count++] = (struct native_map){(uintptr_t)addr, (uintptr_t)addr + size};
    return addr;
}
static void wine_nx_runtime_trace(const char *msg) { puts(msg); }
'''
for name in ('reserved_area', 'range_entry', 'alloc_area'):
    fixture += re.search(r'^struct ' + name + r'\n\{.*?^\};', source, re.M | re.S)[0] + '\n'
fixture += r'''
/* virtual_init excludes the kernel heap before making reservations. */
static struct range_entry free_ranges[16] = {{(void *)0x10000, (void *)0x75000000},
                                            {(void *)0xf5000000, (void *)0x100000000ull},
                                            {(void *)0x100000000ull, (void *)0x8000000000ull}};
static struct range_entry *free_ranges_end = free_ranges + 2;
static struct range_entry horizon_free_range_exclusions[2];
static unsigned int horizon_free_range_exclusion_count;
#define view_block_size sizeof(free_ranges)
'''
fixture += block('static struct range_entry *free_ranges_lower_bound(')
fixture += block('static void free_ranges_exclude(')
fixture += block('static void free_ranges_restore_exclusions(')
# The window left for native thread stacks, from the real source.
for name in ('HORIZON_NATIVE_STACKS', 'HORIZON_NATIVE_STACKS_4G'):
    fixture += re.search(r'^#define %s .*$' % name, source, re.M)[0] + '\n'
# The window the runtime hands horizon.c for its own placements.
fixture += 'void *horizon_native_window_start, *horizon_native_window_end;\n'
for marker in ('static void mmap_add_reserved_area(', 'static int mmap_is_in_reserved_area(',
               'static void reserve_area(', 'static void horizon_reserve_guest_address_space('):
    fixture += block(marker)
fixture += r'''
static void *anon_mmap_fixed(void *ptr, size_t size, int prot, int flags)
{
    (void)flags;
    assert(mmap_is_in_reserved_area(ptr, size) == 1);
    fixed_calls++;
    if (fixed_failures)
    {
        if (fixed_failures > 0) fixed_failures--;
        return MAP_FAILED;
    }
    committed = prot == PROT_NONE ? 0 : size;
    return ptr;
}
/* This fixture has no free space outside the early reservation. Exercise the
 * real reserved-area branch rather than mocking Wine's choice of address. */
static void *try_map_free_area_range(struct alloc_area *a, void *start, void *end)
{ (void)a; (void)start; (void)end; return NULL; }
static size_t unmap_area_above_user_limit(void *ptr, size_t size)
{ (void)ptr; return size; }
static int munmap(void *ptr, size_t size)
{ (void)ptr; (void)size; abort(); }
'''
fixture += block('static void *alloc_free_area_in_range(')
fixture += block('static void unmap_area(')
fixture += block('static void mmap_remove_reserved_area(')
fixture += r'''
/* The thread-local pages the kernel has, as the test places them, and one
 * view the program already has. Wine's remove_reserved_area also unmaps the
 * host reservation, which this fixture does not model. */
#include <pthread.h>
#include <signal.h>
struct file_view;
static pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
static void server_enter_uninterrupted_section( pthread_mutex_t *m, sigset_t *s ) { (void)s; pthread_mutex_lock(m); }
static void server_leave_uninterrupted_section( pthread_mutex_t *m, sigset_t *s ) { (void)s; pthread_mutex_unlock(m); }
static unsigned long long tls_pages[8];
static unsigned int tls_page_count;
static uintptr_t view_start, view_end;
static unsigned long long horizon_next_thread_local_page( unsigned long long addr, unsigned long long limit )
{
    unsigned long long best = 0;
    unsigned int i;
    for (i = 0; i < tls_page_count; i++)
        if (tls_pages[i] >= addr && tls_pages[i] < limit && (!best || tls_pages[i] < best)) best = tls_pages[i];
    return best;
}
static struct file_view *find_view_range( const void *addr, size_t size )
{
    return (uintptr_t)addr < view_end && view_start < (uintptr_t)addr + size ? (struct file_view *)1 : NULL;
}
static void remove_reserved_area( void *addr, size_t size ) { mmap_remove_reserved_area( addr, size ); }
'''
fixture += block('unsigned int horizon_drop_thread_local_pages(')
fixture += r'''
static void cleanup(void)
{
    while (!list_empty(&reserved_areas))
    {
        struct reserved_area *a = LIST_ENTRY(list_head(&reserved_areas), struct reserved_area, entry);
        list_remove(&a->entry);
        free(a);
    }
    native_count = 0;
}
int main(void)
{
    struct alloc_area a = {.size = 60032u * 1024, .align_mask = 0xffff};
    void *ptr;
    /* A released neighboring view may merge free ranges across a sparse
     * kernel exclusion. Restore both partial sides of the permanent hole. */
    horizon_free_range_exclusions[0] = (struct range_entry){(void *)0x100000, (void *)0x300000};
    horizon_free_range_exclusion_count = 1;
    free_ranges[0] = (struct range_entry){0, (void *)0x180000};
    free_ranges[1] = (struct range_entry){(void *)0x200000, (void *)0x500000};
    free_ranges_end = free_ranges + 2;
    free_ranges_restore_exclusions();
    assert(free_ranges_end == free_ranges + 2);
    assert(free_ranges[0].base == 0 && free_ranges[0].end == (void *)0x100000);
    assert(free_ranges[1].base == (void *)0x300000 && free_ranges[1].end == (void *)0x500000);
    free_ranges[0] = (struct range_entry){0, (void *)0x500000};
    free_ranges_end = free_ranges + 1;
    free_ranges_restore_exclusions();
    assert(free_ranges_end == free_ranges + 2);
    assert(free_ranges[0].end == (void *)0x100000 && free_ranges[1].base == (void *)0x300000);
    horizon_free_range_exclusion_count = 0;
    free_ranges[0] = (struct range_entry){(void *)0x10000, (void *)0x75000000};
    free_ranges[1] = (struct range_entry){(void *)0xf5000000, (void *)0x100000000ull};
    free_ranges[2] = (struct range_entry){(void *)0x100000000ull, (void *)0x8000000000ull};
    free_ranges_end = free_ranges + 2;
    /* Model an existing native executable and the kernel heap. Neither may
     * be overwritten by the early reservations. */
    native[native_count++] = (struct native_map){0x10000000, 0x11000000};
    native[native_count++] = (struct native_map){0x75000000, 0xf5000000};
    horizon_reserve_guest_address_space();
    assert(!committed); /* reservation consumes address space, not physical pages */
    assert(!mmap_is_in_reserved_area((void *)0x10000000, 0x1000));
    assert(!mmap_is_in_reserved_area((void *)0x75000000, 0x1000));
    assert(mmap_is_in_reserved_area((void *)0x11000000, a.size) == 1);
    /* Future libnx allocations cannot split the protected guest range into
     * 16 MiB gaps. Native allocation still has space outside the reservation. */
    for (uintptr_t p = 0x1000000; p < (uintptr_t)horizon_native_window_start; p += 0x1000000)
        assert(anon_mmap_tryfixed((void *)p, 0x100000, PROT_NONE, 0) == MAP_FAILED);
    /* Unlike generic native mappings, thread stack mirrors MUST lie in the
     * kernel stack region. Build 103 reserved all of it. The window left at
     * the top takes more of them, with their guard pages, than the 96 threads
     * Horizon allows; a stack a megabyte, placed two megabytes apart. */
    {
        uintptr_t window = HORIZON_NATIVE_STACKS_4G < (stack_hi - stack_lo) / 8 * 5
                           ? HORIZON_NATIVE_STACKS_4G : (stack_hi - stack_lo) / 8 * 5;
        uintptr_t stacks = 0;
        for (uintptr_t p = stack_hi - window + 0x10000; p + 0x104000 <= stack_hi; p += 0x200000)
        {
            assert(p - 0x4000 >= stack_lo);
            assert(anon_mmap_tryfixed((void *)(p - 0x4000), 0x108000, PROT_NONE, 0) != MAP_FAILED);
            stacks++;
        }
        assert(stacks > 96);
    }
    for (int top_down = 0; top_down < 2; top_down++)
    {
        a.top_down = top_down;
        a.unix_prot = PROT_NONE;
        ptr = alloc_free_area_in_range(&a, (char *)0x10000, (char *)0x40000000);
        assert(ptr && ptr != MAP_FAILED && !committed);
        assert(anon_mmap_fixed(ptr, a.size, PROT_READ | PROT_WRITE, 0) == ptr);
        assert(committed == a.size);
        unmap_area(ptr, a.size);
        assert(!committed && mmap_is_in_reserved_area(ptr, a.size) == 1);
        assert(anon_mmap_tryfixed(ptr, a.size, PROT_NONE, 0) == MAP_FAILED);
        assert(alloc_free_area_in_range(&a, ptr, (char *)ptr + a.size) == ptr);
    }
    assert(fixed_calls == 8);
    for (int top_down = 0; top_down < 2; top_down++)
    {
        a.top_down = top_down;
        fixed_failures = -1;
        assert(!alloc_free_area_in_range(&a, (char *)0x10000, (char *)0x40000000));
        fixed_failures = 1;
        ptr = alloc_free_area_in_range(&a, (char *)0x10000, (char *)0x40000000);
        assert(ptr && ptr != MAP_FAILED && !fixed_failures);
    }
    /* Most Wanted reserves 172 MiB in one piece, with its own mappings
     * filling the space below the window and the kernel heap taking 2 GB.
     * Everything else a 32-bit address space holds is the guest's, so the
     * range above the stack region is protected too: left free, libnx
     * scattered stacks and code memory through it and the request failed. */
    cleanup();
    native[native_count++] = (struct native_map){0x400000, 0x28000000};
    native[native_count++] = (struct native_map){0x78200000, 0xf8200000};
    horizon_reserve_guest_address_space();
    /* Five eighths of the stack region: both aliases of every code arena come
     * out of the window, and half of it ran the dynarec out of room. */
    assert(horizon_native_window_start == (char *)stack_hi - (stack_hi - stack_lo) / 8 * 5);
    assert(horizon_native_window_end == (void *)stack_hi);
    assert(mmap_is_in_reserved_area((void *)0x40000000, 0xafd0000) == 1);
    assert(!mmap_is_in_reserved_area((void *)(stack_hi - (stack_hi - stack_lo) / 8 * 5), 0x1000));
    {
        struct alloc_area big = {.size = 0xafd0000, .align_mask = 0xffff};
        void *p = alloc_free_area_in_range(&big, (char *)0x10000, (char *)0x100000000ull);
        assert(p && p != MAP_FAILED);
        assert((uintptr_t)p >= stack_hi && (uintptr_t)p + big.size <= 0x78200000);
    }
    /* A 39-bit address space: libnx can place code memory outside the window,
     * so the window is half the region again and the program keeps the rest.
     * Fallout New Vegas reserves its own memory low, and five eighths here
     * took 256 MB of what it addresses. */
    cleanup();
    space_limit = 0x8000000000ull;
    stack_lo = 0x8000000;
    stack_hi = 0x80000000;
    horizon_reserve_guest_address_space();
    assert(horizon_native_window_start == (char *)stack_hi - HORIZON_NATIVE_STACKS);
    assert(horizon_native_window_end == (void *)stack_hi);
    assert(mmap_is_in_reserved_area((void *)0x50000000, 0x1000) == 1);  /* still the guest's */
    cleanup();
    space_limit = 0x100000000ull;
    stack_lo = 0x200000;
    stack_hi = 0x40000000;

    cleanup();
    stack_query_ok = 0;
    horizon_reserve_guest_address_space();
    assert(!native_count && list_empty(&reserved_areas));
    stack_query_ok = 1;
    /* A smaller kernel stack region must leave its own upper half usable. */
    stack_hi = 0x10000000;
    horizon_reserve_guest_address_space();
    assert(anon_mmap_tryfixed((void *)0xc000000, 0x108000, PROT_NONE, 0) != MAP_FAILED);
    cleanup();
    /* A 36- or 39-bit launch keeps the same low range for the guest, and
     * nothing above 4 GB, which libnx has to itself. Its stack region can be
     * up there as well, and then no window is taken out of the guest's. */
    host_addr_space_limit = (void *)0x8000000000ull;
    free_ranges_end = free_ranges + 3;
    stack_lo = 0x800000000ull;
    stack_hi = 0x900000000ull;
    horizon_reserve_guest_address_space();
    assert(mmap_is_in_reserved_area((void *)0x10000000, 0x40000000) == 1);
    assert(!mmap_is_in_reserved_area((void *)0x100000000ull, 0x1000));
    assert(!mmap_is_in_reserved_area((void *)stack_lo, 0x1000));
    cleanup();
    /* The kernel's thread-local pages: one put in the middle of the guest's
     * reservation, one in the native window and one inside a view the program
     * already has. Only the first is taken out; its neighbours stay reserved,
     * and no allocation handed out afterwards covers it. The Sims 2 was given
     * 4 MB with such a page inside, could not commit it and crashed. */
    host_addr_space_limit = (void *)0x100000000ull;
    free_ranges_end = free_ranges + 2;
    stack_lo = 0x200000;
    stack_hi = 0x40000000;
    horizon_reserve_guest_address_space();
    {
        const unsigned long long inside = 0x4644000, window = (uintptr_t)horizon_native_window_start + 0x10000;
        const unsigned long long viewed = 0x9100000;
        struct alloc_area big = {.size = 0x400000, .align_mask = 0xffff};
        unsigned int found = 0, dropped, i;

        assert(mmap_is_in_reserved_area((void *)(uintptr_t)inside, 0x1000) == 1);
        tls_pages[0] = inside;
        tls_pages[1] = window;
        tls_pages[2] = viewed;
        tls_page_count = 3;
        view_start = 0x9000000;
        view_end = 0x9400000;
        dropped = horizon_drop_thread_local_pages(&found);
        assert(found == 3 && dropped == 1);
        assert(!mmap_is_in_reserved_area((void *)(uintptr_t)inside, 0x1000));
        assert(mmap_is_in_reserved_area((void *)(uintptr_t)(inside - 0x1000), 0x1000) == 1);
        assert(mmap_is_in_reserved_area((void *)(uintptr_t)(inside + 0x1000), 0x1000) == 1);
        assert(mmap_is_in_reserved_area((void *)(uintptr_t)viewed, 0x1000) == 1);
        for (i = 0; i < 64; i++)
        {
            char *p = alloc_free_area_in_range(&big, (char *)0x10000, (char *)0x18000000);
            if (!p || p == MAP_FAILED) break;
            assert((uintptr_t)p + big.size <= inside || (uintptr_t)p > inside);
        }
        assert(i > 4);
        tls_page_count = 0;
        view_start = view_end = 0;
    }
    cleanup();
    puts("Horizon guest reservation: native stacks within kernel limits, query failure, guest "
         "allocation/reuse, Most Wanted's reservation, large address spaces and thread-local pages passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'guest_reserve.c'
    exe = Path(tmp) / 'guest_reserve'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-sign-compare', '-Wno-tautological-compare',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'include'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
