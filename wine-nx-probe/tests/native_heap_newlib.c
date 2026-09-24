#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include "../source/native_heap.h"

#define MiB ((size_t)1048576)
static unsigned char area[256 * 1048576] __attribute__((aligned(2097152)));
char *fake_heap_start = (char *)area, *fake_heap_end = (char *)area + sizeof(area);
static char *heap_break = (char *)area;
static struct _reent reent;
static unsigned int locks;

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

struct _reent *__getreent(void) { return &reent; }
void __malloc_lock(struct _reent *r) { (void)r; locks++; }
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

static void run(void)
{
    struct wine_nx_native_heap_stats stats;
    struct mallinfo before, after;
    void *aligned, *grown, *small, *large, *items[512] = {0};
    unsigned int random_state = 834976, iteration;
    wine_nx_native_heap_stats(&stats);
    check(!stats.used && stats.gap == sizeof(area) && stats.complete);
    before = mallinfo();
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
        if (items[slot]) { free(items[slot]); items[slot] = NULL; }
        else if (random_state & 1)
            items[slot] = memalign((size_t)4096 << ((random_state >> 8) % 10),
                                  65536 + (random_state >> 10) % (2 * MiB));
        else items[slot] = malloc(1 + (random_state >> 10) % 8192);
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
    mixed_lifetimes(1);
    static const char message[] = "newlib heap: mixed allocation, boundary, realloc, accounting and unmapped backing tests passed\n";
    linux_call(64, 1, (long)message, sizeof(message) - 1);
}

void _start(void)
{
    run();
    linux_call(93, 0, 0, 0);
    __builtin_unreachable();
}
