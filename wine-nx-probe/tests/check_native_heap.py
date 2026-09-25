from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'source/native_heap.c').read_text()
source = source.replace('#include <malloc.h>', '').replace('#include <sys/reent.h>', '')
fixture = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "heap_snapshot.h"
struct _reent { int _errno; };
static _Thread_local struct _reent reent;
#define _REENT (&reent)
struct mallinfo { size_t arena, fordblks, keepcost, ordblks, uordblks; };
static unsigned char area[128 * 1048576] __attribute__((aligned(2097152)));
char *fake_heap_start = (char *)area, *fake_heap_end = (char *)area + sizeof(area);
static uintptr_t heap_break = (uintptr_t)area;
static size_t small_free;
static unsigned long mallinfo_calls;
static mtx_t mutex;
static _Thread_local unsigned int locks;
struct horizon_heap_chunk *__malloc_av_[258];
static void __malloc_lock(struct _reent *r)
{ (void)r; assert(mtx_lock(&mutex) == thrd_success); locks++; }
static void __malloc_unlock(struct _reent *r)
{ (void)r; assert(locks); locks--; assert(mtx_unlock(&mutex) == thrd_success); }
void *__real__sbrk_r(struct _reent *r, ptrdiff_t increment)
{
    uintptr_t previous = heap_break;
    (void)r;
    assert(locks);
    if (increment > 0 && (size_t)increment > (uintptr_t)fake_heap_end - heap_break) return (void *)-1;
    assert(increment >= 0 || (size_t)-increment <= heap_break - (uintptr_t)fake_heap_start);
    heap_break += increment;
    return (void *)previous;
}
void *__real__memalign_r(struct _reent *r, size_t alignment, size_t size)
{ void *result = NULL; (void)r; if (posix_memalign(&result, alignment, size)) return NULL; return result; }
void *__real__realloc_r(struct _reent *r, void *pointer, size_t size)
{ (void)r; return realloc(pointer, size); }
void __real__free_r(struct _reent *r, void *pointer) { (void)r; free(pointer); }
size_t __real__malloc_usable_size_r(struct _reent *r, void *pointer)
{ (void)r; (void)pointer; assert(0); return 0; }
struct mallinfo __real__mallinfo_r(struct _reent *r)
{
    (void)r;
    assert(locks);
    mallinfo_calls++;
    return (struct mallinfo){.arena = heap_break - (uintptr_t)fake_heap_start, .fordblks = small_free};
}
/* IMPLEMENTATION */
static void check_available(void)
{
    struct wine_nx_native_heap_stats stats;
    __malloc_lock(_REENT);
    unsigned long calls = mallinfo_calls;
    size_t available = wine_nx_native_heap_free_lower_bound();
    assert(calls == mallinfo_calls);
    wine_nx_native_heap_stats(&stats);
    assert(stats.complete && stats.used <= stats.reserved && locks == 1);
    assert(available == stats.free + stats.gap);
    __malloc_unlock(_REENT);
}

static void available_bounds(void)
{
    void *p[3];
    size_t initial = sizeof(area);
    assert(wine_nx_native_heap_free_lower_bound() == initial);
    fake_heap_end = (char *)((uintptr_t)fake_heap_start + (UINT64_C(8) << 30));
    assert(wine_nx_native_heap_free_lower_bound() == (UINT64_C(8) << 30));
    fake_heap_end = (char *)area + sizeof(area);
    for (unsigned int i = 0; i < 3; i++)
    {
        p[i] = __wrap__memalign_r(_REENT, 65536, 1048576);
        assert(p[i]);
    }
    assert(wine_nx_native_heap_free_lower_bound() == initial - 3 * 1048576);
    __wrap__free_r(_REENT, p[1]);
    assert(wine_nx_native_heap_free_lower_bound() == initial - 2 * 1048576);
    assert(__wrap__sbrk_r(_REENT, 65536) == fake_heap_start);
    small_free = 32768;
    unsigned long calls = mallinfo_calls;
    size_t available = wine_nx_native_heap_free_lower_bound();
    assert(calls == mallinfo_calls && available == initial - 2 * 1048576 - 65536);
    size_t lower;
    assert(wine_nx_native_heap_free_estimate(&lower) == available && lower == available);
    assert(wine_nx_native_heap_free_exact() == available + small_free);
    calls = mallinfo_calls;
    assert(wine_nx_native_heap_free_estimate(&lower) == available + small_free);
    wine_nx_native_heap_note_small_allocation(8192);
    assert(wine_nx_native_heap_free_estimate(&lower) == available + small_free - 8192 - 64);
    wine_nx_native_heap_note_small_allocation(small_free);
    assert(wine_nx_native_heap_free_estimate(&lower) == available && calls == mallinfo_calls);
    struct mallinfo info = __wrap__mallinfo_r(_REENT);
    assert(available + small_free == initial - info.arena + info.fordblks);
    check_available();
    small_free = 0;
    assert(__wrap__sbrk_r(_REENT, -65536) == fake_heap_start + 65536);
    __wrap__free_r(_REENT, p[0]);
    __wrap__free_r(_REENT, p[2]);
    assert(wine_nx_native_heap_free_lower_bound() == initial);
}

static int worker(void *argument)
{
    unsigned int seed = (uintptr_t)argument + 1;
    unsigned char *live[32] = {0};
    size_t sizes[32] = {0};
    for (unsigned int iteration = 0; iteration < 40000; iteration++)
    {
        seed = seed * 1664525 + 1013904223;
        unsigned int slot = (seed >> 16) % 32;
        if (live[slot])
        {
            assert(live[slot][0] == (unsigned char)slot && live[slot][sizes[slot] - 1] == (unsigned char)slot);
            if (!(seed % 3))
            {
                size_t size = 1 + (seed >> 8) % (2 * 1048576);
                unsigned char *result = __wrap__realloc_r(_REENT, live[slot], size);
                if (result)
                {
                    assert(result[0] == (unsigned char)slot);
                    if (size >= sizes[slot]) assert(result[sizes[slot] - 1] == (unsigned char)slot);
                    live[slot] = result;
                    sizes[slot] = size;
                    result[size - 1] = slot;
                }
            }
            else { __wrap__free_r(_REENT, live[slot]); live[slot] = NULL; }
        }
        else
        {
            size_t size = 65536 + (seed >> 8) % (2 * 1048576);
            size_t alignment = (size_t)4096 << ((seed >> 6) % 10);
            live[slot] = __wrap__memalign_r(_REENT, alignment, size);
            if (live[slot])
            {
                assert(!((uintptr_t)live[slot] % alignment));
                assert(__wrap__malloc_usable_size_r(_REENT, live[slot]) >= size);
                sizes[slot] = size;
                live[slot][0] = live[slot][size - 1] = slot;
            }
        }
        if (!(iteration % 100)) check_available();
    }
    for (unsigned int i = 0; i < 32; i++) __wrap__free_r(_REENT, live[i]);
    return 0;
}
static int small_heap_worker(void *argument)
{
    (void)argument;
    for (unsigned int iteration = 0; iteration < 40000; iteration++)
    {
        __malloc_lock(_REENT);
        void *p = __wrap__sbrk_r(_REENT, 65536);
        if (p != (void *)-1)
        {
            memset(p, 0xab, 65536);
            assert(__wrap__sbrk_r(_REENT, -65536) == (char *)p + 65536);
        }
        __malloc_unlock(_REENT);
    }
    return 0;
}
int main(void)
{
    thrd_t workers[5];
    assert(mtx_init(&mutex, mtx_recursive) == thrd_success);
    for (unsigned int i = 0; i < 128; i++)
        __malloc_av_[2 * i + 2] = __malloc_av_[2 * i + 3] =
            (struct horizon_heap_chunk *)((char *)&__malloc_av_[2 * i + 2] - 2 * sizeof(size_t));
    available_bounds();
    for (uintptr_t i = 0; i < 4; i++) assert(thrd_create(&workers[i], worker, (void *)i) == thrd_success);
    assert(thrd_create(&workers[4], small_heap_worker, NULL) == thrd_success);
    for (unsigned int i = 0; i < 5; i++) assert(thrd_join(workers[i], NULL) == thrd_success);
    assert(!heap.used && !heap.holes && heap.bottom == heap.count);
    assert(heap_break == (uintptr_t)fake_heap_start);
    void *p = __wrap__memalign_r(_REENT, 4096, 4096);
    assert(p && !owns_pointer(p));
    __wrap__free_r(_REENT, p);
    p = __wrap__realloc_r(_REENT, NULL, 1024);
    assert(p && !owns_pointer(p));
    __wrap__free_r(_REENT, p);
    mtx_destroy(&mutex);
    puts("native heap: 200000 concurrent allocation/resize/free/break operations passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-native-heap-') as tmp:
    unit, binary = Path(tmp) / 'heap.c', Path(tmp) / 'heap'
    unit.write_text(fixture.replace('/* IMPLEMENTATION */', source))
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root / 'source'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
