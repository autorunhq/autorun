#include <errno.h>
#include <stdbool.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include "../source/native_heap.h"
#ifdef WINE_NX_SHADER_ALLOC_CACHE
#include "../source/shader_alloc_cache.h"
#endif

/* Keep allocator calls observable without disabling optimization in the runtime. */
extern void *test_malloc(size_t) __asm__("malloc");
extern void *test_calloc(size_t, size_t) __asm__("calloc");
extern void *test_realloc(void *, size_t) __asm__("realloc");
extern void test_free(void *) __asm__("free");
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free

#define MiB ((size_t)1048576)
static unsigned char area[256 * 1048576] __attribute__((aligned(2097152)));
char *fake_heap_start = (char *)area, *fake_heap_end = (char *)area + sizeof(area);
static char *heap_break = (char *)area;
static struct _reent reent;
static unsigned int locks;
static unsigned long lock_calls, outer_lock_calls;
static unsigned char tls_area[4096] __attribute__((aligned(16)));
void *__aarch64_read_tp(void) { return tls_area; }
extern bool __wrap_os_get_available_system_memory(uint64_t *);

static long linux_call(long number, long a, long b, long c)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

#define check(condition) do { if (!(condition)) { \
    static const char message[] = "failed: " #condition "\n"; \
    linux_call(64, 2, (long)message, sizeof(message) - 1); \
    linux_call(93, 1, 0, 0); __builtin_unreachable(); } } while (0)

static void check_budget(void)
{
    struct mallinfo info = mallinfo();
    uint64_t available;
    check(__wrap_os_get_available_system_memory(&available));
    check(available == sizeof(area) - info.uordblks);
}

struct _reent *__getreent(void) { return &reent; }
void __malloc_lock(struct _reent *r) { (void)r; if (!locks) outer_lock_calls++; locks++; lock_calls++; }
void __malloc_unlock(struct _reent *r) { (void)r; check(locks); locks--; }
void *_sbrk_r(struct _reent *r, ptrdiff_t increment)
{
    char *previous = heap_break;
    check(locks);
    if (increment > 0 && (size_t)increment > (size_t)(fake_heap_end - heap_break))
    { r->_errno = ENOMEM; return (void *)-1; }
    if (increment < 0) check((size_t)-increment <= (size_t)(heap_break - fake_heap_start));
    heap_break += increment;
    return previous;
}

static void mixed_lifetimes(int isolated)
{
    extern void *__real__memalign_r(struct _reent *, size_t, size_t);
    void *small[192], *backing[192], *large;
    malloc_trim(0);
    for (unsigned int i = 0; i < 192; i++)
    {
        small[i] = malloc(128 * 1024);
        backing[i] = isolated ? memalign(65536, MiB) : __real__memalign_r(&reent, 65536, MiB);
        check(small[i] && backing[i]);
    }
    for (unsigned int i = 0; i < 192; i++) free(backing[i]);
    large = isolated ? memalign(2 * MiB, 128 * MiB) : __real__memalign_r(&reent, 2 * MiB, 128 * MiB);
    if (isolated) check(large != NULL);
    else check(large == NULL);
    free(large);
    for (unsigned int i = 0; i < 192; i++) free(small[i]);
}

#ifdef WINE_NX_SHADER_ALLOC_CACHE
static void shader_cache_drain(void)
{
    static const unsigned int sizes[] = {1, 31, 32, 33, 127, 128};
    void *items[128];
    struct mallinfo before = mallinfo();
    for (unsigned int scope = 0; scope < 32; scope++)
    {
        unsigned int count = sizes[scope % (sizeof(sizes) / sizeof(sizes[0]))];
        wine_nx_shader_alloc_begin();
        for (unsigned int pass = 0; pass < 2; pass++)
        {
            for (unsigned int i = 0; i < count; i++)
            {
                items[i] = malloc(32 + i * 16);
                check(items[i]);
                memset(items[i], i, 32 + i * 16);
            }
            for (unsigned int i = 0; i < count; i++) free(items[i]);
            check(mallinfo().uordblks > before.uordblks);
            check(!locks);
            unsigned long outer_before = outer_lock_calls;
            errno = EIO;
            if (!pass)
            {
                check(wine_nx_shader_alloc_trim());
                check(wine_nx_shader_alloc_active && !wine_nx_shader_alloc_trim());
            }
            else
            {
                wine_nx_shader_alloc_end();
                check(!wine_nx_shader_alloc_active);
            }
            check(errno == EIO);
            check(!locks);
            check(outer_lock_calls - outer_before == (count + 31) / 32);
            check(mallinfo().uordblks == before.uordblks);
            check_budget();
        }
    }
}

static void shader_cache(void)
{
    void *items[128] = {0};
    unsigned int seed = 94783;
    struct mallinfo before = mallinfo();
    unsigned long calls = lock_calls;
    for (unsigned int i = 0; i < 10000; i++)
    {
        void *p = memalign(64, 512);
        check(p && !((uintptr_t)p & 63));
        memset(p, 0x19, 512);
        free(p);
    }
    unsigned long uncached = lock_calls - calls;
    calls = lock_calls;
    wine_nx_shader_alloc_begin();
    for (unsigned int i = 0; i < 10000; i++)
    {
        void *p = memalign(64, 512);
        check(p && !((uintptr_t)p & 63));
        memset(p, 0x19, 512);
        free(p);
    }
    wine_nx_shader_alloc_end();
    check(lock_calls - calls < uncached / 100);
    check(mallinfo().uordblks == before.uordblks);
    check_budget();

    for (unsigned int iteration = 0; iteration < 30000; iteration++)
    {
        if (!(iteration % 500)) wine_nx_shader_alloc_begin();
        seed = seed * 1664525 + 1013904223;
        unsigned int slot = (seed >> 16) % 128;
        size_t size = 1 + (seed >> 4) % 100000;
        if (items[slot])
        {
            check(((unsigned char *)items[slot])[0] == (unsigned char)slot);
            if (seed & 2)
            {
                void *p = realloc(items[slot], size);
                check(p && ((unsigned char *)p)[0] == (unsigned char)slot);
                items[slot] = p;
            }
            else { free(items[slot]); items[slot] = NULL; }
        }
        else if (seed & 1)
        {
            size_t align = (size_t)16 << ((seed >> 9) % 7);
            items[slot] = memalign(align, size);
            check(items[slot] && !((uintptr_t)items[slot] & (align - 1)));
            ((unsigned char *)items[slot])[0] = slot;
        }
        else
        {
            items[slot] = calloc(size, 1);
            check(items[slot]);
            for (size_t i = 0; i < size; i++) check(!((unsigned char *)items[slot])[i]);
            ((unsigned char *)items[slot])[0] = slot;
        }
        if (iteration % 500 == 499) wine_nx_shader_alloc_end();
        if (!(iteration % 100)) check_budget();
    }
    for (unsigned int i = 0; i < 128; i++) free(items[i]);
    check(mallinfo().uordblks == before.uordblks);
    check_budget();
    wine_nx_shader_alloc_begin();
    void *p = malloc(1024);
    check(p);
    free(p);
    check(wine_nx_shader_alloc_trim());
    check(!wine_nx_shader_alloc_trim());
    check_budget();
    wine_nx_shader_alloc_end();
    check(mallinfo().uordblks == before.uordblks);
    void *burst[256];
    wine_nx_shader_alloc_begin();
    for (unsigned int i = 0; i < 256; i++)
    {
        burst[i] = malloc(32768);
        check(burst[i]);
    }
    for (unsigned int i = 0; i < 256; i++) free(burst[i]);
    for (unsigned int i = 0; i < 256; i++)
    {
        burst[i] = malloc(128);
        check(burst[i]);
    }
    for (unsigned int i = 0; i < 256; i++) free(burst[i]);
    calls = lock_calls;
    for (unsigned int iteration = 0; iteration < 20; iteration++)
    {
        for (unsigned int i = 0; i < 256; i++)
        {
            burst[i] = malloc(128);
            check(burst[i]);
            memset(burst[i], i, 128);
        }
        for (unsigned int i = 0; i < 256; i++)
        {
            for (unsigned int j = 0; j < 128; j++) check(((unsigned char *)burst[i])[j] == (unsigned char)i);
            free(burst[i]);
        }
    }
    check(lock_calls == calls);
    check_budget();
    wine_nx_shader_alloc_end();
    check(mallinfo().uordblks == before.uordblks);
    check_budget();
    static const char message[] = "newlib shader cache: reuse, alignment, lifetime, realloc and exact accounting passed\n";
    linux_call(64, 1, (long)message, sizeof(message) - 1);
}
#endif

static void run(void)
{
    struct wine_nx_native_heap_stats stats;
    struct mallinfo before, after;
    void *aligned, *grown, *small, *large, *items[512] = {0};
    unsigned int random_state = 834976, iteration;
    volatile size_t impossible_size = SIZE_MAX;
    wine_nx_native_heap_stats(&stats);
    check(!stats.used && stats.gap == sizeof(area) && stats.complete);
    before = mallinfo();
    check_budget();
    aligned = memalign(2 * MiB, 2 * MiB);
    check(aligned && !((uintptr_t)aligned % (2 * MiB)));
    check(malloc_usable_size(aligned) == 2 * MiB);
    after = mallinfo();
    check(after.uordblks - before.uordblks == 2 * MiB && after.arena - before.arena == 2 * MiB);
    memset(aligned, 0x69, 2 * MiB);
    small = malloc(4096);
    check(small && small < aligned);
    grown = realloc(aligned, 3 * MiB);
    check(grown && malloc_usable_size(grown) == 3 * MiB);
    for (size_t i = 0; i < 2 * MiB; i++) check(((unsigned char *)grown)[i] == 0x69);
    check(!realloc(grown, sizeof(area) * 2));
    check(malloc_usable_size(grown) == 3 * MiB);
    check(realloc(grown, MiB) == grown && malloc_usable_size(grown) == MiB);
    check(realloc(grown, 2 * MiB) == grown && malloc_usable_size(grown) == 2 * MiB);
    free(small);
    free(grown);
    check_budget();
    wine_nx_native_heap_stats(&stats);
    check(!stats.reserved && !stats.used && !stats.holes && stats.complete);

    aligned = aligned_alloc(2 * MiB, 32 * MiB);
    check(aligned && malloc_usable_size(aligned) == 32 * MiB);
    check(!malloc(240 * MiB));
    large = malloc(200 * MiB);
    check(large && (uintptr_t)large + 200 * MiB <= (uintptr_t)aligned);
    memset(large, 0x2a, 200 * MiB);
    check(!memalign(2 * MiB, 32 * MiB));
    free(large);
    malloc_trim(0);
    large = memalign(2 * MiB, 64 * MiB);
    check(large && (uintptr_t)large + 64 * MiB <= (uintptr_t)aligned);
    free(aligned);
    free(large);

    aligned = memalign(2 * MiB, 2 * MiB);
    check(aligned && !linux_call(226, (long)aligned, 2 * MiB, 0));
    check(malloc_usable_size(aligned) == 2 * MiB);
    wine_nx_native_heap_stats(&stats);
    check(stats.complete);
    free(aligned);
    check(!linux_call(226, (long)aligned, 2 * MiB, 3));

    for (iteration = 0; iteration < 50000; iteration++)
    {
        unsigned int slot;
        random_state = random_state * 1664525 + 1013904223;
        slot = (random_state >> 16) % 512;
        if (items[slot] && (random_state & 7) == 0)
        {
            void *changed = realloc(items[slot], 1 + (random_state >> 10) % (3 * MiB));
            if (changed) items[slot] = changed;
        }
        else if (items[slot]) { free(items[slot]); items[slot] = NULL; }
        else if (random_state & 1)
            items[slot] = memalign((size_t)4096 << ((random_state >> 8) % 10),
                                  65536 + (random_state >> 10) % (2 * MiB));
        else items[slot] = calloc(1 + (random_state >> 10) % 8192, 1);
        check_budget();
        if (!(iteration % 500))
        {
            wine_nx_native_heap_stats(&stats);
            check(stats.complete && !locks);
            after = mallinfo();
            check(after.arena <= sizeof(area) && after.uordblks + after.fordblks == after.arena);
        }
    }
    for (unsigned int i = 0; i < 512; i++) free(items[i]);
    wine_nx_native_heap_stats(&stats);
    check(!stats.reserved && !stats.used && !stats.holes && stats.complete && !locks);
    check(mallinfo().uordblks == before.uordblks);
    mixed_lifetimes(0);
    check_budget();
    mixed_lifetimes(1);
    check_budget();
    small = malloc(1024);
    check(small);
    check(!realloc(small, 2 * sizeof(area)) && !locks);
    check_budget();
    small = realloc(small, 0);
    check_budget();
    free(small);
    check(!calloc(impossible_size, 2));
    check_budget();
#ifdef WINE_NX_SHADER_ALLOC_CACHE
    shader_cache();
    shader_cache_drain();
#endif
    static const char message[] = "newlib heap: mixed allocation, boundary, realloc, accounting and unmapped backing tests passed\n";
    linux_call(64, 1, (long)message, sizeof(message) - 1);
}

void _start(void)
{
    run();
    linux_call(93, 0, 0, 0);
    __builtin_unreachable();
}
