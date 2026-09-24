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
#define MiB ((size_t)1048576)
struct _reent { int _errno; };
struct mallinfo { size_t arena, fordblks, keepcost, ordblks, uordblks; };
static char heap_area[512 * 1048576], result;
char *fake_heap_start = heap_area, *fake_heap_end = heap_area + sizeof(heap_area);
static struct mallinfo heap;
static unsigned int attempts, fail_attempts, reclaims;
static unsigned int heap_lock;
static size_t reclaimed;
static size_t idle_pages;
static int active = 1, locked, nested;
static int mapping_mutex, backing_pages, held;
#define min(a,b) ((a) < (b) ? (a) : (b))
static struct mallinfo mallinfo(void) { return heap; }
static int horizon_swap_enabled(void) { return active; }
static int pthread_mutex_trylock(int *mutex)
{ (void)mutex; assert(!held); if (locked) return 1; held = 1; return 0; }
static void pthread_mutex_unlock(int *mutex) { (void)mutex; assert(held); held = 0; }
static size_t horizon_pages_trim(int *pool)
{ size_t size = idle_pages; (void)pool; idle_pages = 0; return size; }
static size_t swap_reclaim_locked(size_t size)
{
    assert(held && size && size <= 2 * MiB);
    reclaims++;
    reclaimed += size;
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
    attempts = reclaims = 0;
    reclaimed = idle_pages = 0;
    fail_attempts = failures;
    locked = nested = 0;
    active = 1;
}
int main(void)
{
    struct _reent r = {ENOMEM};
    size_t budget;
    reset(0);
    assert(__wrap__malloc_r(&r, 1) == &result && !reclaims);
    reset(1);
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result);
    assert(reclaims == 1 && reclaimed == 2 * MiB);
    reset(1);
    idle_pages = 16 * MiB;
    assert(__wrap__malloc_r(&r, 4 * MiB) == &result && !reclaims && !idle_pages);
    reset(100);
    assert(!__wrap__memalign_r(&r, 65536, 256 * MiB));
    assert(reclaimed == 16 * MiB && reclaims == 8 && attempts == 9);
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
    reset(100);
    active = 0;
    assert(!__wrap__malloc_r(&r, MiB) && !reclaims);
    assert(r._errno == ENOMEM);
    reset(100);
    locked = 1;
    assert(!__wrap__malloc_r(&r, MiB) && attempts == 1 && !reclaims);
    reset(1);
    nested = 1;
    assert(__wrap__memalign_r(&r, 4096, 4 * MiB) == &result && reclaims == 1);
    reset(2);
    horizon_swap_native_begin();
    horizon_swap_native_begin();
    assert(!__wrap__memalign_r(&r, 4096, MiB) && !reclaims);
    horizon_swap_native_end();
    assert(!__wrap__malloc_r(&r, MiB) && !reclaims);
    horizon_swap_native_end();
    budget = SIZE_MAX;
    assert(horizon_swap_native_reclaim(MiB, &budget) && reclaims == 1);
    assert(__wrap__memalign_r(&r, 4096, MiB) == &result);
    assert(!allocator_depth && !allocator_deferred);
    reset(100);
    active = 0;
    nested = 1;
    assert(!__wrap__memalign_r(&r, 65536, MiB));
    assert(!reclaims && !allocator_depth && r._errno == ENOMEM);
    puts("native allocation: recovery, deferral, recursion and overflow passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-native-swap-') as tmp:
    unit, binary = Path(tmp) / 'native.c', Path(tmp) / 'native'
    unit.write_text(fixture.replace('/* IMPLEMENTATION */', source).replace('/* RECLAIM_IMPLEMENTATION */', reclaim))
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root / 'source'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
