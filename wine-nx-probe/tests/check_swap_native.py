from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'source/swap_native.c').read_text()
source = source[source.index('extern void *__real__malloc_r'):]
horizon = (root.parent / 'dlls/ntdll/unix/horizon.c').read_text()
reclaim = horizon[horizon.index('size_t horizon_swap_reclaim('):]
reclaim = reclaim[:reclaim.index('\n}') + 2]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "native_heap.h"
#include "horizon_swap.h"
#define MiB ((size_t)1048576)
struct _reent { int _errno; };
static struct _reent default_reent;
#define _REENT (&default_reent)
struct mallinfo { size_t arena, fordblks, keepcost, ordblks, uordblks; };
static char heap_area[512 * 1048576], result;
char *fake_heap_start = heap_area, *fake_heap_end = heap_area + sizeof(heap_area);
static struct mallinfo heap;
static unsigned int attempts, fail_attempts, reclaims;
static unsigned int heap_lock;
static unsigned long long clock_tick;
static size_t reclaimed;
static size_t idle_pages;
static size_t cache_available;
static unsigned int cache_calls;
static size_t coalesce;
static int active = 1, locked, nested;
static int mapping_mutex, backing_pages, held;
static struct { unsigned long long native_calls, native_busy, native_trim_bytes; } swap_profile;
#define min(a,b) ((a) < (b) ? (a) : (b))
static void __malloc_lock(struct _reent *r)
{ (void)r; heap_lock++; }
static void __malloc_unlock(struct _reent *r)
{ (void)r; assert(heap_lock); heap_lock--; }
static struct mallinfo _mallinfo_r(struct _reent *r)
{ (void)r; assert(heap_lock); return heap; }
void wine_nx_native_heap_stats(struct wine_nx_native_heap_stats *s)
{
    assert(heap_lock);
    *s = (struct wine_nx_native_heap_stats){.small_largest = heap.keepcost, .complete = 1};
}
static unsigned long long armGetSystemTick(void)
{ return ++clock_tick; }
void wine_nx_native_heap_note_small_allocation(size_t size) { (void)size; }
static unsigned long long armTicksToNs(unsigned long long tick) { return tick * 1000000000 / 19200000; }
int horizon_swap_enabled(void) { return active; }
static int pthread_mutex_trylock(int *mutex)
{ (void)mutex; assert(!held); if (locked) return 1; held = 1; return 0; }
static void pthread_mutex_unlock(int *mutex) { (void)mutex; assert(held); held = 0; }
static size_t horizon_pages_trim(int *pool)
{ size_t size = idle_pages; (void)pool; idle_pages = 0; return size; }
size_t wine_nx_sd_cache_reclaim(size_t size)
{
    size_t freed = cache_available < size ? cache_available : size;
    cache_available -= freed;
    cache_calls++;
    return freed;
}
static size_t swap_reclaim_locked(size_t size)
{
    assert(held && size && size <= 2 * MiB);
    reclaims++;
    reclaimed += size;
    heap.keepcost += coalesce;
    return size;
}
/* RECLAIM_IMPLEMENTATION */
void *__wrap__malloc_r(struct _reent *, size_t);
void wine_nx_runtime_trace(const char *line) { (void)line; assert(!heap_lock); }
/* IMPLEMENTATION */
static void *allocate(void)
{
    assert(!held);
    attempts++;
    return attempts <= fail_attempts ? NULL : &result;
}
void *__real__malloc_r(struct _reent *r, size_t size)
{ (void)r; (void)size; return allocate(); }
void *__real__calloc_r(struct _reent *r, size_t count, size_t size)
{ (void)r; if (size && count > SIZE_MAX / size) return NULL; return allocate(); }
void *wine_nx_native_realloc(struct _reent *r, void *old, size_t size)
{ (void)r; assert(old == &result); return size ? allocate() : NULL; }
void *wine_nx_native_memalign(struct _reent *r, size_t align, size_t size)
{
    if (!align || (align & (align - 1))) return NULL;
    if (nested) return __wrap__malloc_r(r, size);
    return allocate();
}
static void reset(unsigned int failures)
{
    assert(!allocator_depth && !allocator_deferred && !heap_lock);
    heap = (struct mallinfo){512 * MiB, 300 * MiB, 4 * MiB, 200, 212 * MiB};
    clock_tick = 0;
    native_deferred = native_budget_zero = native_no_progress = native_freed_bytes = native_cache_dropped_bytes = 0;
    native_fragmented = native_reclaim_ticks = native_max_reclaim_ticks = 0;
    attempts = reclaims = 0;
    reclaimed = idle_pages = coalesce = 0;
    cache_available = cache_calls = 0;
    memset(&swap_profile, 0, sizeof(swap_profile));
    fail_attempts = failures;
    locked = nested = 0;
    active = 1;
}
int main(void)
{
    struct _reent r = {ENOMEM};
    struct horizon_swap_reclaim_budget budget;
    reset(0);
    assert(__wrap__malloc_r(&r, 1) == &result && !reclaims);
    reset(1);
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result);
    assert(reclaims == 1 && reclaimed == 2 * MiB);
    assert(swap_profile.native_calls == 1 && !swap_profile.native_busy);
    assert(native_freed_bytes == 2 * MiB);
    reset(1);
    idle_pages = 16 * MiB;
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result && !reclaims && !idle_pages);
    assert(swap_profile.native_trim_bytes == 16 * MiB);
    reset(1);
    cache_available = 2 * MiB;
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result && !reclaims);
    assert(cache_calls == 1 && native_cache_dropped_bytes == 2 * MiB);
    reset(2);
    cache_available = 2 * MiB;
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result && reclaims == 1);
    assert(cache_calls == 1 && native_cache_dropped_bytes == 2 * MiB);
    reset(100);
    assert(!__wrap__memalign_r(&r, 65536, 256 * MiB));
    assert(reclaimed == 2 * MiB && reclaims == 1 && attempts == 2);
    assert(native_fragmented == 1);
    reset(3);
    coalesce = 16 * MiB;
    assert(__wrap__memalign_r(&r, 65536, 256 * MiB) == &result);
    assert(reclaimed == 6 * MiB && reclaims == 3 && !native_fragmented);
    reset(3);
    heap.fordblks = 0;
    assert(__wrap__calloc_r(&r, 2, 4 * MiB) == &result);
    assert(reclaimed == 6 * MiB);
    reset(1);
    assert(__wrap__realloc_r(&r, &result, 4 * MiB) == &result && reclaims == 1);
    reset(100);
    assert(!__wrap__realloc_r(&r, &result, 0) && !reclaims);
    assert(!__wrap__calloc_r(&r, SIZE_MAX, 2) && !reclaims);
    assert(!__wrap__memalign_r(&r, 3, MiB) && !reclaims);
    reset(100);
    assert(!__wrap__malloc_r(&r, 1024 * MiB) && !reclaims);
    assert(native_budget_zero == 1);
    reset(100);
    active = 0;
    cache_available = MiB;
    assert(!__wrap__malloc_r(&r, MiB) && !reclaims);
    assert(!cache_calls && cache_available == MiB);
    assert(r._errno == ENOMEM);
    reset(100);
    locked = 1;
    assert(!__wrap__malloc_r(&r, MiB) && attempts == 1 && !reclaims);
    assert(swap_profile.native_calls == 1 && swap_profile.native_busy == 1);
    assert(native_no_progress == 1);
    reset(1);
    nested = 1;
    assert(__wrap__memalign_r(&r, 4096, 4 * MiB) == &result && reclaims == 1);
    reset(2);
    cache_available = MiB;
    horizon_swap_native_begin();
    horizon_swap_native_begin();
    assert(!__wrap__memalign_r(&r, 4096, MiB) && !reclaims);
    assert(!cache_calls && cache_available == MiB);
    horizon_swap_native_end();
    assert(!__wrap__malloc_r(&r, MiB) && !reclaims);
    assert(native_deferred == 2);
    horizon_swap_native_end();
    cache_available = 0;
    budget = (struct horizon_swap_reclaim_budget){.remaining = SIZE_MAX};
    assert(horizon_swap_native_reclaim(MiB, &budget) && reclaims == 1);
    assert(__wrap__memalign_r(&r, 4096, MiB) == &result);
    assert(!allocator_depth && !allocator_deferred);
    reset(100);
    active = 0;
    nested = 1;
    assert(!__wrap__memalign_r(&r, 65536, MiB));
    assert(!reclaims && !allocator_depth && r._errno == ENOMEM);
    horizon_swap_native_profile();
    puts("native allocation: recovery, cache reclaim, fragmentation, deferral, recursion and overflow passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-native-swap-') as tmp:
    unit, binary = Path(tmp) / 'native.c', Path(tmp) / 'native'
    unit.write_text(fixture.replace('/* IMPLEMENTATION */', source).replace('/* RECLAIM_IMPLEMENTATION */', reclaim))
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root / 'source'),
                    '-I', str(root.parent / 'dlls/ntdll/unix'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
