#!/usr/bin/env python3
"""Reject uncommittable reservations even when the kernel reports free pages."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
start = source.index('static int add_reservation_mapping_locked(')
end = source.index('\nstatic Result check_thread_local_range(', start)
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define PROT_NONE 0
typedef int VirtmemReservation;
struct horizon_mapping { int dummy; };
static int reservations, mappings, overlap, mapped;
static uintptr_t low = 0x8000000, high = 0x8000000000;
static void horizon_get_address_space_limits(void **start, void **limit)
{ *start = (void *)low; *limit = (void *)high; }
static int horizon_overlaps_kernel_region(void *start, size_t size)
{ (void)start; (void)size; return overlap; }
static int horizon_query_region;
static int horizon_range_unmapped(unsigned long long a, size_t n, int q, void *ctx)
{ (void)a; (void)n; (void)q; (void)ctx; return !mapped; }
static VirtmemReservation *reserve_fixed_range_locked(void *start, size_t size)
{ (void)start; (void)size; reservations++; return (void *)1; }
static void remove_reservation_locked(VirtmemReservation *r)
{ (void)r; reservations--; }
static struct horizon_mapping *alloc_mapping(void *a, size_t n, void *b,
                                             size_t o, void *r, int p)
{ (void)a; (void)n; (void)b; (void)o; (void)r; (void)p; return (void *)2; }
static void list_add_mapping(struct horizon_mapping *m) { (void)m; mappings++; }
'''
fixture += source[start:end]
fixture += r'''
int main(void)
{
    for (uintptr_t p = 0x1000000; p < low; p += 0x1000000)
        assert(add_reservation_mapping_locked((void *)p, 0x400000) == -1 && errno == EINVAL);
    assert(add_reservation_mapping_locked((void *)(low-4096), 8192) == -1);
    assert(add_reservation_mapping_locked((void *)high, 4096) == -1);
    assert(add_reservation_mapping_locked((void *)(high-4096), 8192) == -1);
    assert(add_reservation_mapping_locked((void *)low, SIZE_MAX) == -1);
    assert(!reservations && !mappings);
    assert(!add_reservation_mapping_locked((void *)low, 4096));
    assert(!add_reservation_mapping_locked((void *)(high-4096), 4096));
    assert(reservations == 2 && mappings == 2);
    overlap = 1;
    assert(add_reservation_mapping_locked((void *)low, 4096) == -1 && errno == EEXIST);
    overlap = 0; mapped = 1;
    assert(add_reservation_mapping_locked((void *)low, 4096) == -1 && errno == EEXIST);
    assert(reservations == 2 && mappings == 2);
    /* A different process layout must use its queried bounds. */
    low = 0x200000; high = 0x400000; mapped = 0;
    assert(!add_reservation_mapping_locked((void *)low, 4096));
    puts("Reservation bounds: low/high edges, overflow, kernel ownership and alternate layouts passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'bounds.c'
    exe = Path(tmp) / 'bounds'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
