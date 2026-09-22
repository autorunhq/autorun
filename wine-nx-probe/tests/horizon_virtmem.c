#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../source/horizon_virtmem.c"

struct VirtmemReservation { uintptr_t start; size_t size; };
void *horizon_native_window_start, *horizon_native_window_end;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int original_calls, reserve_calls, reserve_fail, query_fail;
static struct { uintptr_t start, end; } mapped[128];
static unsigned int mapped_count;

int wine_nx_low_window_reserve(void) { return 1; }

static void check_locked(void) { assert(pthread_mutex_trylock(&lock) != 0); }
VirtmemReservation *__real_virtmemAddReservation(void *address, size_t size)
{
    VirtmemReservation *reservation;
    check_locked();
    reserve_calls++;
    if (reserve_fail) return NULL;
    reservation = malloc(sizeof(*reservation));
    assert(reservation);
    reservation->start = (uintptr_t)address;
    reservation->size = size;
    return reservation;
}
void __real_virtmemRemoveReservation(VirtmemReservation *reservation)
{
    check_locked();
    free(reservation);
}
void *__real_virtmemFindStack(size_t size, size_t guard)
{
    check_locked();
    assert(size == 4096 && guard == 4096);
    original_calls++;
    return (void *)0x700000000ULL;
}
Result svcQueryMemory(MemoryInfo *info, u32 *page_info, uint64_t address)
{
    uintptr_t low = 0, high = 0x100000000ULL;
    check_locked();
    *page_info = 0;
    if (query_fail == 1) return 1;
    for (unsigned int i = 0; i < mapped_count; ++i)
    {
        if (mapped[i].start <= address && address < mapped[i].end)
        {
            *info = (MemoryInfo){mapped[i].start, mapped[i].end - mapped[i].start, 1};
            return 0;
        }
        if (mapped[i].end <= address && mapped[i].end > low) low = mapped[i].end;
        if (mapped[i].start > address && mapped[i].start < high) high = mapped[i].start;
    }
    *info = (MemoryInfo){low, high - low, MemType_Unmapped};
    if (query_fail == 2) info->size = 0;
    if (query_fail == 3) info->addr = address + 1;
    if (query_fail == 4) *info = (MemoryInfo){UINT64_MAX - 1, 4096, 0};
    if (query_fail == 5) *info = (MemoryInfo){0, address, 0};
    return 0;
}
static void add_stack(uintptr_t address, size_t size)
{
    for (unsigned int i = 0; i < mapped_count; ++i)
        assert(address + size <= mapped[i].start || address >= mapped[i].end);
    for (struct reservation_range *range = reservations; range; range = range->next)
        assert(address + size <= range->start || address >= range->end);
    assert(mapped_count < 128);
    mapped[mapped_count].start = address;
    mapped[mapped_count++].end = address + size;
}
static void *worker(void *arg)
{
    (void)arg;
    assert(!pthread_mutex_lock(&lock));
    uintptr_t address = (uintptr_t)__wrap_virtmemFindStack(0x100000, 0x4000);
    assert(address);
    add_stack(address, 0x100000);
    assert(!pthread_mutex_unlock(&lock));
    return NULL;
}
int main(void)
{
    VirtmemReservation *low, *high, *overlap;
    pthread_t threads[40];
    pthread_mutex_lock(&lock);
    assert(__wrap_virtmemFindStack(4096, 4096) == (void *)0x700000000ULL);
    horizon_native_window_start = (void *)0x7e0000000ULL;
    horizon_native_window_end = (void *)0x800000000ULL;
    assert(__wrap_virtmemFindStack(4096, 4096) == (void *)0x700000000ULL && original_calls == 2);
    horizon_native_window_start = (void *)0x20100000;
    horizon_native_window_end = (void *)0x40000000;
    assert(__wrap_virtmemFindStack(1, 1) == (void *)0x3fffe000);
    low = __wrap_virtmemAddReservation((void *)0x20100000, 0x8000000);
    high = __wrap_virtmemAddReservation((void *)0x3f000000, 0x1000000);
    overlap = __wrap_virtmemAddReservation((void *)0x3f800000, 0x400000);
    assert(low && high && overlap);
    assert(__wrap_virtmemFindStack(0x100000, 0x4000) == (void *)0x3eefc000);
    __wrap_virtmemRemoveReservation(high);
    assert(__wrap_virtmemFindStack(0x100000, 0x4000) == (void *)0x3fefc000);
    __wrap_virtmemRemoveReservation(overlap);
    high = __wrap_virtmemAddReservation((void *)0x3ff00001, 0xfffff);
    assert(__wrap_virtmemFindStack(0x100000, 0x4000) == (void *)0x3fdfc000);
    __wrap_virtmemRemoveReservation(high);
    reserve_fail = 1;
    assert(!__wrap_virtmemAddReservation((void *)0x38000000, 4096));
    reserve_fail = 0;
    unsigned int calls = reserve_calls;
    assert(!__wrap_virtmemAddReservation((void *)(UINTPTR_MAX - 1), 4096) && reserve_calls == calls);
    assert(!__wrap_virtmemFindStack(0, 0));
    assert(!__wrap_virtmemFindStack(SIZE_MAX, 0));
    assert(!__wrap_virtmemFindStack(4096, SIZE_MAX));
    assert(!__wrap_virtmemFindStack(4096, SIZE_MAX / 2));
    for (query_fail = 1; query_fail <= 5; ++query_fail) assert(!__wrap_virtmemFindStack(4096, 4096));
    query_fail = 0;
    add_stack(0x3ff00000, 0x100000);
    uintptr_t first = (uintptr_t)__wrap_virtmemFindStack(0x100000, 0x4000);
    assert(first == 0x3fdfc000);
    add_stack(first, 0x100000);
    assert((uintptr_t)__wrap_virtmemFindStack(0x100000, 0x4000) == first - 0x104000);
    --mapped_count;
    assert((uintptr_t)__wrap_virtmemFindStack(0x100000, 0x4000) == first);
    mapped_count = 0;
    pthread_mutex_unlock(&lock);
    for (unsigned int i = 0; i < 40; ++i) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned int i = 0; i < 40; ++i) assert(!pthread_join(threads[i], NULL));
    pthread_mutex_lock(&lock);
    assert(mapped_count == 40 && mapped[39].start > 0x3d000000);
    mapped_count = 0;
    __wrap_virtmemRemoveReservation(low);
    assert(!reservations);
    high = __wrap_virtmemAddReservation(horizon_native_window_start, 0x1ff00000);
    assert(!__wrap_virtmemFindStack(4096, 4096));
    __wrap_virtmemRemoveReservation(high);
    assert(!reservations);
    pthread_mutex_unlock(&lock);
    puts("Horizon stacks: compact placement, reservations, guards, reuse, failures and 40 concurrent allocations passed");
}
