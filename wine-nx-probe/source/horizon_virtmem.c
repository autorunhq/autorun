#include <stdint.h>
#include <stdlib.h>
#include <switch.h>

extern void *horizon_native_window_start, *horizon_native_window_end;
extern VirtmemReservation *__real_virtmemAddReservation( void *address, size_t size );
extern void __real_virtmemRemoveReservation( VirtmemReservation *reservation );
extern void *__real_virtmemFindStack( size_t size, size_t guard );

/* Accessed under libnx's virtual-memory lock. */
static struct reservation_range
{
    struct reservation_range *next;
    VirtmemReservation *reservation;
    uintptr_t start, end;
} *reservations;

VirtmemReservation *__wrap_virtmemAddReservation( void *address, size_t size )
{
    struct reservation_range *range;
    VirtmemReservation *reservation;

    if (size > UINTPTR_MAX - (uintptr_t)address) return NULL;
    if (!(range = malloc( sizeof(*range) ))) return NULL;
    if (!(reservation = __real_virtmemAddReservation( address, size )))
    {
        free( range );
        return NULL;
    }
    range->start = (uintptr_t)address;
    range->end = range->start + size;
    range->reservation = reservation;
    range->next = reservations;
    reservations = range;
    return reservation;
}

void __wrap_virtmemRemoveReservation( VirtmemReservation *reservation )
{
    struct reservation_range **link = &reservations, *range;

    while (*link && (*link)->reservation != reservation) link = &(*link)->next;
    if ((range = *link))
    {
        *link = range->next;
        free( range );
    }
    __real_virtmemRemoveReservation( reservation );
}

void *__wrap_virtmemFindStack( size_t size, size_t guard )
{
    uintptr_t start = (uintptr_t)horizon_native_window_start;
    uintptr_t end = (uintptr_t)horizon_native_window_end;
    size_t total;

    if (!start || end <= start || end > 0x100000000ULL) return __real_virtmemFindStack( size, guard );
    if (size > SIZE_MAX - 4095 || guard > SIZE_MAX - 4095) return NULL;
    size = (size + 4095) & ~(size_t)4095;
    guard = (guard + 4095) & ~(size_t)4095;
    if (!size || guard > (SIZE_MAX - size) / 2) return NULL;
    total = size + 2 * guard;
    while (end > start && total <= end - start)
    {
        struct reservation_range *range;
        uintptr_t base = end - total;
        MemoryInfo info;
        u32 page_info;

        for (range = reservations; range; range = range->next)
            if (range->start < end && base < range->end) break;
        if (range) { end = range->start & ~(uintptr_t)4095; continue; }
        if (R_FAILED(svcQueryMemory( &info, &page_info, end - 1 )) ||
            info.addr >= end || !info.size || info.size > UINT64_MAX - info.addr ||
            info.addr + info.size < end) return NULL;
        if (info.type == MemType_Unmapped && info.addr <= base) return (void *)(base + guard);
        end = info.addr & ~(uintptr_t)4095;
    }
    return NULL;
}
