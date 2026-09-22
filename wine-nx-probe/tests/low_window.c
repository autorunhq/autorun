#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include "../source/low_window.c"

struct VirtmemReservation { uintptr_t base; size_t size; };
static struct VirtmemReservation reservation;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static u64 aslr_base, aslr_end, heap_base, occupied;
static unsigned int reserved_count, find_count, mapped_count, logs;
static int info_fail, reservation_fail, map_fail, permission_fail, query_fail;
static void *mapped_address, *mapped_backing;
static u32 mapped_perm;

static void locked(void) { assert(pthread_mutex_trylock(&mutex) != 0); }
void virtmemLock(void) { assert(!pthread_mutex_lock(&mutex)); }
void virtmemUnlock(void) { assert(!pthread_mutex_unlock(&mutex)); }
VirtmemReservation *__real_virtmemAddReservation(void *address, size_t size)
{
    locked();
    if (reservation_fail) return NULL;
    assert(!reserved_count++);
    reservation = (struct VirtmemReservation){ (uintptr_t)address, size };
    return &reservation;
}
static void *find_memory(size_t size, size_t guard)
{
    locked();
    assert(size == 0x1000 && guard == 0x1000);
    find_count++;
    if (aslr_base == WINE_NX_GUEST_BASE && aslr_end == WINE_NX_HOST_LIMIT)
    {
        assert(reserved_count == 1);
        assert(reservation.base == WINE_NX_GUEST_BASE);
        assert(reservation.base + reservation.size == WINE_NX_NATIVE_BASE);
    }
    return (void *)0x700000000ULL;
}
void *__real_virtmemFindAslr(size_t size, size_t guard) { return find_memory(size, guard); }
void *__real_virtmemFindCodeMemory(size_t size, size_t guard) { return find_memory(size, guard); }
Result svcGetInfo(u64 *value, InfoType type, Handle process, u64 sub)
{
    assert(process == CUR_PROCESS_HANDLE && sub == 0);
    if (info_fail) return 1;
    if (type == InfoType_AslrRegionAddress) *value = aslr_base;
    else if (type == InfoType_AslrRegionSize) *value = aslr_end - aslr_base;
    else *value = heap_base;
    return 0;
}
Result svcQueryMemory(MemoryInfo *info, u32 *page_info, u64 address)
{
    locked();
    *page_info = 0;
    if (query_fail) return 1;
    if (mapped_address && address == (uintptr_t)mapped_address)
        *info = (MemoryInfo){ address, 0x1000, 1, mapped_perm };
    else *info = (MemoryInfo){ 0, WINE_NX_HOST_LIMIT, occupied ? 1 : 0, 0 };
    return 0;
}
Result svcMapProcessCodeMemory(Handle process, u64 address, u64 backing, size_t size)
{
    locked();
    assert(process == 1 && !mapped_address && size == 0x1000);
    assert(address == 0x400000 || address == 0xffff0000);
    assert(backing >= WINE_NX_NATIVE_BASE && !(backing & 0xfff));
    mapped_count++;
    if (map_fail) return 2;
    mapped_address = mmap((void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped_address == (void *)(uintptr_t)address);
    mapped_backing = (void *)(uintptr_t)backing;
    memcpy(mapped_address, mapped_backing, size);
    return 0;
}
Result svcSetProcessMemoryPermission(Handle process, u64 address, size_t size, u32 perm)
{
    assert(process == 1 && (uintptr_t)mapped_address == address);
    if (permission_fail) return 3;
    mapped_perm = perm;
    assert(!mprotect(mapped_address, size, perm == Perm_Rx ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE));
    return 0;
}
Result svcUnmapProcessCodeMemory(Handle process, u64 address, u64 backing, size_t size)
{
    assert(process == 1 && (uintptr_t)mapped_address == address && (uintptr_t)mapped_backing == backing);
    if (mapped_perm == Perm_Rw) memcpy(mapped_backing, mapped_address, size);
    assert(!munmap(mapped_address, size));
    mapped_address = mapped_backing = NULL;
    mapped_perm = 0;
    return 0;
}
void *armGetTls(void) { return &mutex; }
void armDCacheFlush(void *address, size_t size) { __builtin___clear_cache(address, (char *)address + size); }
void armICacheInvalidate(void *address, size_t size) { __builtin___clear_cache(address, (char *)address + size); }
Handle envGetOwnProcessHandle(void) { return 1; }
static void report(const char *message)
{
    assert(!pthread_mutex_trylock(&mutex));
    pthread_mutex_unlock(&mutex);
    assert(strstr(message, "[LOWVA]"));
    logs++;
}
static void reset(u64 base, u64 end)
{
    assert(!mapped_address);
    profile_checked = low_window = reserved_count = find_count = mapped_count = logs = 0;
    info_fail = reservation_fail = map_fail = permission_fail = query_fail = 0;
    guest_reservation = NULL;
    aslr_base = base;
    aslr_end = end;
    heap_base = 0x1000000000;
    occupied = 0;
}
static void *worker(void *unused)
{
    (void)unused;
    virtmemLock();
    assert(__wrap_virtmemFindAslr(0x1000, 0x1000));
    assert(__wrap_virtmemFindCodeMemory(0x1000, 0x1000));
    virtmemUnlock();
    return NULL;
}
int main(void)
{
    pthread_t workers[16];
    const u64 profiles[][2] = { {0x200000, 0x100000000}, {0x8000000, 0x1000000000},
                               {0x8000000, WINE_NX_HOST_LIMIT} };
    for (unsigned int i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
    {
        reset(profiles[i][0], profiles[i][1]);
        worker(NULL);
        assert(!wine_nx_low_window_probe(report));
        assert(!reserved_count && !mapped_count && logs == 1);
    }
    reset(WINE_NX_GUEST_BASE, WINE_NX_HOST_LIMIT);
    for (unsigned int i = 0; i < 16; i++) assert(!pthread_create(&workers[i], NULL, worker, NULL));
    for (unsigned int i = 0; i < 16; i++) assert(!pthread_join(workers[i], NULL));
    assert(reserved_count == 1 && find_count == 32);
    reset(WINE_NX_GUEST_BASE, WINE_NX_HOST_LIMIT);
    virtmemLock();
    info_fail = 1;
    assert(!__wrap_virtmemFindAslr(0x1000, 0x1000) && !find_count);
    info_fail = 0;
    reservation_fail = 1;
    assert(!__wrap_virtmemFindCodeMemory(0x1000, 0x1000) && !find_count);
    reservation_fail = 0;
    assert(__wrap_virtmemFindAslr(0x1000, 0x1000));
    virtmemUnlock();
    heap_base = 0x40000000;
    assert(!wine_nx_low_window_probe(report) && !mapped_count);
    heap_base = 0x1000000000;
    occupied = 1;
    assert(!wine_nx_low_window_probe(report) && !mapped_count);
    occupied = 0;
    query_fail = 1;
    assert(!wine_nx_low_window_probe(report) && !mapped_count);
    query_fail = 0;
    map_fail = 1;
    assert(!wine_nx_low_window_probe(report) && mapped_count == 1);
    map_fail = 0;
    permission_fail = 1;
    assert(!wine_nx_low_window_probe(report) && !mapped_address);
    permission_fail = 0;
#ifdef __aarch64__
    assert(wine_nx_low_window_probe(report));
    assert(!mapped_address);
    puts("Low window: AArch64 execution at 0x400000 and high-edge writable mapping passed (mock SVCs)");
#endif
    puts("Low window: stock profiles, reservation, concurrency and failure cleanup passed");
}
