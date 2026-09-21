#!/usr/bin/env python3
"""Exercise actual reservation splitting and fixed remaps with native observers."""
from pathlib import Path
import subprocess
import tempfile
import os

root = Path(__file__).resolve().parents[2]
s = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(marker):
    start = s.index(marker)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef int BOOL;
typedef int LONG;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uintptr_t ULONG_PTR;
typedef struct { u64 addr, size; u32 type, perm, attr; } MemoryInfo;
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_FAILED ((void *)-1)
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define HORIZON_POOL_ARENA (2 * 1024 * 1024ul)
#define MemType_Unmapped 0
#define R_SUCCEEDED(result) ((result) == 0)
typedef struct { uintptr_t start, end; int used; } VirtmemReservation;
struct horizon_mapping { void *addr; size_t size; VirtmemReservation *reservation; };
static VirtmemReservation reservations[32];
static struct horizon_mapping *maps[8];
static int map_count, mapping_pool, allocations, fail_allocation, fail_reservation;
static int observe, require_target, kernel_target, violation, fail_replace;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static int svcQueryMemory(MemoryInfo *info, u32 *page_info, u64 address)
{ (void)info; (void)page_info; (void)address; return -1; }
static struct horizon_mapping *find_overlap_mapping(void *p, size_t size)
{
    uintptr_t start = (uintptr_t)p, end = start + size;
    for (int i = 0; i < map_count; i++)
    {
        uintptr_t map_start = (uintptr_t)maps[i]->addr;
        if (start < map_start + maps[i]->size && end > map_start) return maps[i];
    }
    return NULL;
}
static int covered(uintptr_t p)
{
    if (kernel_target && p == 0x12000) return 1;
    for (int i = 0; i < 32; i++)
        if (reservations[i].used && p >= reservations[i].start && p < reservations[i].end) return 1;
    return 0;
}
static void native_observer(void)
{
    if (observe && (!covered(0x10000) || !covered(0x14000) || (require_target && !covered(0x12000))))
        violation = 1;
}
static VirtmemReservation *reserve_fixed_range(void *p, size_t size)
{
    if (fail_reservation && !--fail_reservation) { errno = ENOMEM; return NULL; }
    for (int i = 0; i < 32; i++) if (!reservations[i].used)
    {
        reservations[i] = (VirtmemReservation){(uintptr_t)p, (uintptr_t)p + size, 1};
        native_observer();
        return &reservations[i];
    }
    abort();
}
static void remove_reservation(VirtmemReservation *r)
{ assert(r->used); r->used = 0; native_observer(); }
static struct horizon_mapping *alloc_mapping(void *p, size_t size, void *backing,
                                             size_t off, VirtmemReservation *r, int prot)
{
    (void)backing; (void)off; (void)prot;
    if (fail_allocation && !--fail_allocation) return NULL;
    struct horizon_mapping *m = malloc(sizeof(*m));
    assert(m); allocations++;
    *m = (struct horizon_mapping){p,size,r};
    return m;
}
static void horizon_object_free(void *pool, struct horizon_mapping *m)
{ (void)pool; allocations--; free(m); }
static void list_add_mapping(struct horizon_mapping *m) { assert(map_count < 8); maps[map_count++] = m; }
static void list_remove_mapping(struct horizon_mapping *m)
{
    for (int i=0; i<map_count; i++) if (maps[i] == m) { maps[i] = maps[--map_count]; return; }
    abort();
}
'''
fixture += 'static int map_backing_at(void *, size_t, int, int, off_t, int, int);\n'
fixture += function('static int replace_reservation_mapping(')
fixture += function('static int change_reservation_mapping(')
fixture += function('static int split_reservation_mapping(')
fixture += r'''
static int unmap_range_locked(void *p, size_t size)
{
    for (int i = 0; i < map_count; i++)
        if ((uintptr_t)p >= (uintptr_t)maps[i]->addr &&
            (uintptr_t)p + size <= (uintptr_t)maps[i]->addr + maps[i]->size)
            return split_reservation_mapping(maps[i], p, size);
    abort();
}
static size_t page_align_size(size_t n) { return (n + 4095) & ~4095ul; }
static void virtmemLock(void) { native_observer(); }
static void virtmemUnlock(void) { native_observer(); }
static int add_reservation_mapping_locked(void *p, size_t size)
{
    if (fail_replace) { errno = EEXIST; return -1; }
    VirtmemReservation *r = reserve_fixed_range(p, size);
    list_add_mapping(alloc_mapping(p,size,NULL,0,r,0));
    return 0;
}
static int map_backing_at(void *p, size_t size, int prot, int fd, off_t offset, int flags, int error)
{
    (void)prot; (void)fd; (void)offset; (void)flags; (void)error;
    native_observer();
    int ret = add_reservation_mapping_locked(p, size);
    if (!ret) kernel_target = 1;
    return ret;
}
static int map_anonymous_backings(void *p, size_t size, int prot, int flags, int error)
{ return map_backing_at(p, size, prot, -1, 0, flags, error); }
static void wine_nx_runtime_trace(const char *msg) { puts(msg); }
'''
fixture += function('static void *horizon_mmap_fixed(')
fixture += r'''
static void setup(void)
{
    VirtmemReservation *r = reserve_fixed_range((void *)0x10000, 0x5000);
    list_add_mapping(alloc_mapping((void *)0x10000,0x5000,NULL,0,r,0));
    observe = 1;
}
static void cleanup(void)
{
    observe = 0;
    while (map_count)
    {
        struct horizon_mapping *m = maps[--map_count];
        remove_reservation(m->reservation);
        horizon_object_free(&mapping_pool,m);
    }
    assert(!allocations);
    for (int i = 0; i < 32; i++) assert(!reservations[i].used);
    fail_allocation = fail_reservation = fail_replace = require_target = kernel_target = violation = 0;
}
int main(void)
{
    setup();
    assert(!split_reservation_mapping(maps[0], (char *)0x12000, 0x1000));
    assert(!violation && map_count == 2 && !covered(0x12000));
    cleanup();
    for (int which = 0; which < 2; which++) for (int nth = 1; nth <= 2; nth++)
    {
        setup();
        struct horizon_mapping *old = maps[0];
        require_target = 1;
        if (which) fail_reservation = nth; else fail_allocation = nth;
        assert(split_reservation_mapping(old,(char *)0x12000,0x1000) == -1);
        assert(!violation && map_count == 1 && maps[0] == old && allocations == 1);
        assert(covered(0x12000));
        cleanup();
    }
    /* A failed commit preserves both metadata and native exclusion; retry
     * succeeds without reserving again, including full-range commits. */
    for (int whole = 0; whole < 2; whole++)
    {
        setup(); require_target = 1; fail_replace = 1;
        struct horizon_mapping *old = maps[0];
        char *start = (char *)(uintptr_t)(whole ? 0x10000 : 0x12000);
        size_t size = whole ? 0x5000 : 0x1000;
        assert(change_reservation_mapping(old,start,size,3) == -1);
        assert(errno == EEXIST && !violation && map_count == 1 && maps[0] == old);
        assert(allocations == 1 && covered(0x12000));
        fail_replace = 0;
        assert(!change_reservation_mapping(old,start,size,3));
        assert(!violation && map_count == (whole ? 1 : 3) && covered(0x12000));
        cleanup();
    }
    for (int prot = 0; prot <= 3; prot += 3)
    {
        setup(); require_target = 1;
        assert(horizon_mmap_fixed((void *)0x12000,0x1000,prot,0,-1,0) == (void *)0x12000);
        assert(!violation && map_count == 3 && covered(0x12000));
        cleanup();
    }
    for (int which = 0; which < 2; which++)
    {
        setup(); require_target = 1;
        struct horizon_mapping *old = maps[0];
        if (which) fail_allocation = 1; else fail_reservation = 1;
        assert(horizon_mmap_fixed((void *)0x12000,0x1000,0,0,-1,0) == MAP_FAILED);
        assert(errno == ENOMEM && !violation && map_count == 1 && maps[0] == old);
        cleanup();
    }
    for (int prot = 0; prot <= 3; prot += 3)
    {
        setup(); fail_replace = 1;
        assert(horizon_mmap_fixed((void *)0x12000,0x1000,prot,0,-1,0) == MAP_FAILED);
        assert(errno == EEXIST && !violation && map_count == 2);
        cleanup();
    }
    puts("Horizon reservation transitions: no exposed neighbours/target, transactional failures and remap errors passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'transition.c'
    exe = Path(tmp) / 'transition'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
