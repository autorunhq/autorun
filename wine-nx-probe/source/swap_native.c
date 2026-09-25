#include <switch.h>
#include "horizon_swap.h"
#include "swap_ipc.h"
#include <sys/reent.h>
#include <malloc.h>
#include <stdio.h>
#include "native_heap.h"

__thread unsigned int wine_nx_swap_native_io;

struct ipc_pins
{
    struct horizon_swap_pin pins[74];
    unsigned int count;
};

static Result pin_error(void) { return MAKERESULT( Module_Kernel, KernelError_OutOfMemory ); }

static int pin_buffer( void *context, const void *addr, size_t size )
{
    struct ipc_pins *pins = context;
    if (!size) return 0;
    if (pins->count == sizeof(pins->pins) / sizeof(pins->pins[0])) return -1;
    if (horizon_swap_pin_begin( &pins->pins[pins->count], addr, size )) return -1;
    pins->count++;
    return 0;
}

static void unpin_ipc( struct ipc_pins *pins )
{
    while (pins->count) horizon_swap_pin_end( &pins->pins[--pins->count] );
}

static int pin_ipc( struct ipc_pins *pins, void *message, size_t size )
{
    pins->count = 0;
    if (wine_nx_swap_native_io || !horizon_swap_enabled()) return 0;
    if (pin_buffer( pins, message, size ) || swap_ipc_visit( message, size, pin_buffer, pins ))
    { unpin_ipc( pins ); return -1; }
    return 0;
}

extern Result __real_svcSendSyncRequest( Handle );
Result __wrap_svcSendSyncRequest( Handle session )
{
    struct ipc_pins pins;
    Result rc;
    if (pin_ipc( &pins, armGetTls(), 0x100 )) return pin_error();
    rc = __real_svcSendSyncRequest( session );
    unpin_ipc( &pins );
    return rc;
}

extern Result __real_svcSendSyncRequestWithUserBuffer( void *, u64, Handle );
Result __wrap_svcSendSyncRequestWithUserBuffer( void *buffer, u64 size, Handle session )
{
    struct ipc_pins pins;
    Result rc;
    if (pin_ipc( &pins, buffer, size )) return pin_error();
    rc = __real_svcSendSyncRequestWithUserBuffer( buffer, size, session );
    unpin_ipc( &pins );
    return rc;
}

extern Result __real_svcReplyAndReceive( s32 *, const Handle *, s32, Handle, u64 );
Result __wrap_svcReplyAndReceive( s32 *index, const Handle *handles, s32 count, Handle reply, u64 timeout )
{
    struct ipc_pins pins;
    Result rc;
    if (pin_ipc( &pins, armGetTls(), 0x100 )) return pin_error();
    rc = __real_svcReplyAndReceive( index, handles, count, reply, timeout );
    unpin_ipc( &pins );
    return rc;
}

extern Result __real_svcReplyAndReceiveWithUserBuffer( s32 *, void *, u64, const Handle *, s32, Handle, u64 );
Result __wrap_svcReplyAndReceiveWithUserBuffer( s32 *index, void *buffer, u64 size, const Handle *handles,
                                              s32 count, Handle reply, u64 timeout )
{
    struct ipc_pins pins;
    Result rc;
    if (pin_ipc( &pins, buffer, size )) return pin_error();
    rc = __real_svcReplyAndReceiveWithUserBuffer( index, buffer, size, handles, count, reply, timeout );
    unpin_ipc( &pins );
    return rc;
}

/* Exported/async memory can outlive its original handle; keep its allocation nonpageable. */
static int exclude_buffer( void *context, const void *addr, size_t size )
{
    (void)context;
    return horizon_swap_may_contain( addr, size ) ? horizon_swap_exclude( addr, size ) : 0;
}

extern Result __real_svcSendAsyncRequestWithUserBuffer( Handle *, void *, u64, Handle );
Result __wrap_svcSendAsyncRequestWithUserBuffer( Handle *out, void *buffer, u64 size, Handle session )
{
    struct ipc_pins pins;
    Result rc;
    if (pin_ipc( &pins, buffer, size )) return pin_error();
    if (horizon_swap_enabled() &&
        (exclude_buffer( NULL, buffer, size ) || swap_ipc_visit( buffer, size, exclude_buffer, NULL )))
        rc = pin_error();
    else rc = __real_svcSendAsyncRequestWithUserBuffer( out, buffer, size, session );
    unpin_ipc( &pins );
    return rc;
}

extern Result __real_svcCreateTransferMemory( Handle *, void *, size_t, u32 );
Result __wrap_svcCreateTransferMemory( Handle *out, void *addr, size_t size, u32 permission )
{
    if (exclude_buffer( NULL, addr, size )) return pin_error();
    return __real_svcCreateTransferMemory( out, addr, size, permission );
}

extern Result __real_nvioctlNvmap_Alloc( u32, u32, u32, u32, u32, u8, void * );
Result __wrap_nvioctlNvmap_Alloc( u32 fd, u32 handle, u32 heapmask, u32 flags, u32 align, u8 kind, void *addr )
{
    if (horizon_swap_may_contain( addr, 1 ))
    {
        u32 size;
        Result rc = nvioctlNvmap_Param( fd, handle, NvMapParam_Size, &size );
        if (R_FAILED(rc)) return rc;
        if (horizon_swap_exclude( addr, size )) return pin_error();
    }
    return __real_nvioctlNvmap_Alloc( fd, handle, heapmask, flags, align, kind, addr );
}

extern Result __real_nvMapCreate( NvMap *, void *, u32, u32, NvKind, bool );
Result __wrap_nvMapCreate( NvMap *map, void *addr, u32 size, u32 align, NvKind kind, bool cacheable )
{
    if (exclude_buffer( NULL, addr, size )) return pin_error();
    return __real_nvMapCreate( map, addr, size, align, kind, cacheable );
}

extern Result __real_svcSetMemoryAttribute( void *, u64, u32, u32 );
Result __wrap_svcSetMemoryAttribute( void *addr, u64 size, u32 mask, u32 value )
{
    if ((mask & value) && exclude_buffer( NULL, addr, size )) return pin_error();
    return __real_svcSetMemoryAttribute( addr, size, mask, value );
}

extern Result __real_svcMapMemory( void *, void *, u64 );
Result __wrap_svcMapMemory( void *dst, void *src, u64 size )
{
    if (exclude_buffer( NULL, src, size )) return pin_error();
    return __real_svcMapMemory( dst, src, size );
}

static int exclude_process_buffer( Handle process, u64 addr, u64 size )
{
    if (process != CUR_PROCESS_HANDLE && process != envGetOwnProcessHandle()) return 0;
    return exclude_buffer( NULL, (void *)(uintptr_t)addr, size );
}

extern Result __real_svcMapDeviceAddressSpaceByForce( Handle, Handle, u64, u64, u64, u32 );
Result __wrap_svcMapDeviceAddressSpaceByForce( Handle handle, Handle process, u64 addr, u64 size, u64 dst, u32 option )
{
    if (exclude_process_buffer( process, addr, size )) return pin_error();
    return __real_svcMapDeviceAddressSpaceByForce( handle, process, addr, size, dst, option );
}

extern Result __real_svcMapDeviceAddressSpaceAligned( Handle, Handle, u64, u64, u64, u32 );
Result __wrap_svcMapDeviceAddressSpaceAligned( Handle handle, Handle process, u64 addr, u64 size, u64 dst, u32 option )
{
    if (exclude_process_buffer( process, addr, size )) return pin_error();
    return __real_svcMapDeviceAddressSpaceAligned( handle, process, addr, size, dst, option );
}

extern Result __real_svcMapDeviceAddressSpace( u64 *, Handle, Handle, u64, u64, u64, u32 );
Result __wrap_svcMapDeviceAddressSpace( u64 *mapped, Handle handle, Handle process, u64 addr, u64 size, u64 dst, u32 perm )
{
    if (exclude_process_buffer( process, addr, size )) return pin_error();
    return __real_svcMapDeviceAddressSpace( mapped, handle, process, addr, size, dst, perm );
}

extern Result __real_svcWaitForAddress( void *, u32, s64, s64 );
Result __wrap_svcWaitForAddress( void *addr, u32 type, s64 value, s64 timeout )
{
    struct horizon_swap_pin pin;
    Result rc;
    if (horizon_swap_pin_begin( &pin, addr,
         type == ArbitrationType_WaitIfEqual64 ? sizeof(u64) : sizeof(u32) )) return pin_error();
    rc = __real_svcWaitForAddress( addr, type, value, timeout );
    horizon_swap_pin_end( &pin );
    return rc;
}

extern Result __real_svcSignalToAddress( void *, u32, s32, s32 );
Result __wrap_svcSignalToAddress( void *addr, u32 type, s32 value, s32 count )
{
    struct horizon_swap_pin pin;
    Result rc;
    if (horizon_swap_pin_begin( &pin, addr, sizeof(u32) )) return pin_error();
    rc = __real_svcSignalToAddress( addr, type, value, count );
    horizon_swap_pin_end( &pin );
    return rc;
}

extern Result __real_svcArbitrateLock( u32, u32 *, u32 );
Result __wrap_svcArbitrateLock( u32 owner, u32 *addr, u32 self )
{
    struct horizon_swap_pin pin;
    Result rc;
    if (horizon_swap_pin_begin( &pin, addr, sizeof(*addr) )) return pin_error();
    rc = __real_svcArbitrateLock( owner, addr, self );
    horizon_swap_pin_end( &pin );
    return rc;
}

extern Result __real_svcArbitrateUnlock( u32 * );
Result __wrap_svcArbitrateUnlock( u32 *addr )
{
    struct horizon_swap_pin pin;
    Result rc;
    if (horizon_swap_pin_begin( &pin, addr, sizeof(*addr) )) return pin_error();
    rc = __real_svcArbitrateUnlock( addr );
    horizon_swap_pin_end( &pin );
    return rc;
}

extern Result __real_svcWaitProcessWideKeyAtomic( u32 *, u32 *, u32, u64 );
Result __wrap_svcWaitProcessWideKeyAtomic( u32 *key, u32 *tag, u32 self, u64 timeout )
{
    struct horizon_swap_pin pins[2];
    Result rc;
    if (horizon_swap_pin_begin( &pins[0], key, sizeof(*key) )) return pin_error();
    if (horizon_swap_pin_begin( &pins[1], tag, sizeof(*tag) )) rc = pin_error();
    else
    {
        rc = __real_svcWaitProcessWideKeyAtomic( key, tag, self, timeout );
        horizon_swap_pin_end( &pins[1] );
    }
    horizon_swap_pin_end( &pins[0] );
    return rc;
}

extern void __real_svcSignalProcessWideKey( u32 *, s32 );
void __wrap_svcSignalProcessWideKey( u32 *key, s32 count )
{
    struct horizon_swap_pin pin;
    if (horizon_swap_pin_begin( &pin, key, sizeof(*key) )) return;
    __real_svcSignalProcessWideKey( key, count );
    horizon_swap_pin_end( &pin );
}

extern void *__real__malloc_r( struct _reent *, size_t );
extern void *__real__calloc_r( struct _reent *, size_t, size_t );
extern size_t wine_nx_sd_cache_reclaim( size_t );
static __thread unsigned int allocator_depth;
static __thread unsigned int allocator_deferred;
static unsigned long long native_deferred, native_budget_zero, native_no_progress, native_freed_bytes;
static unsigned long long native_cache_dropped_bytes;
static unsigned long long native_fragmented, native_reclaim_ticks, native_max_reclaim_ticks;
void horizon_swap_native_begin(void) { allocator_deferred++; }
void horizon_swap_native_end(void) { allocator_deferred--; }

int horizon_swap_native_reclaim( size_t size, struct horizon_swap_reclaim_budget *budget )
{
    size_t freed;
    unsigned long long tick, elapsed, previous;
    if (!size || !horizon_swap_enabled()) return 0;
    if (allocator_deferred)
    {
        __atomic_add_fetch( &native_deferred, 1, __ATOMIC_RELAXED );
        return 0;
    }
    if (budget->remaining == SIZE_MAX || budget->fragmented)
    {
        extern char *fake_heap_start, *fake_heap_end;
        struct mallinfo heap;
        struct wine_nx_native_heap_stats backing;
        size_t heap_size = fake_heap_end - fake_heap_start;
        size_t available, largest;
        __malloc_lock( _REENT );
        heap = _mallinfo_r( _REENT );
        wine_nx_native_heap_stats( &backing );
        __malloc_unlock( _REENT );
        available = heap.fordblks + (heap_size > heap.arena ? heap_size - heap.arena : 0);
        largest = backing.small_largest > backing.largest ? backing.small_largest : backing.largest;
        if (budget->remaining != SIZE_MAX && backing.complete && largest <= budget->largest)
        {
            __atomic_add_fetch( &native_fragmented, 1, __ATOMIC_RELAXED );
            budget->remaining = 0;
            return 0;
        }
        if (budget->remaining == SIZE_MAX)
        {
            budget->remaining = size > heap_size ? 0 : size > available ? size - available :
                                size < 16 * 1048576u ? size : 16 * 1048576u;
            budget->fragmented = backing.complete && size <= available && size > largest;
        }
        budget->largest = largest;
    }
    if (!budget->remaining)
    {
        __atomic_add_fetch( &native_budget_zero, 1, __ATOMIC_RELAXED );
        return 0;
    }
    if (!budget->cache_tried)
    {
        budget->cache_tried = 1;
        freed = wine_nx_sd_cache_reclaim( size );
        if (freed)
        {
            __atomic_add_fetch( &native_cache_dropped_bytes, freed, __ATOMIC_RELAXED );
            return 1;
        }
    }
    tick = armGetSystemTick();
    freed = horizon_swap_reclaim( budget->remaining );
    elapsed = armGetSystemTick() - tick;
    __atomic_add_fetch( &native_reclaim_ticks, elapsed, __ATOMIC_RELAXED );
    previous = __atomic_load_n( &native_max_reclaim_ticks, __ATOMIC_RELAXED );
    while (previous < elapsed && !__atomic_compare_exchange_n( &native_max_reclaim_ticks, &previous,
                elapsed, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
    if (freed) __atomic_add_fetch( &native_freed_bytes, freed, __ATOMIC_RELAXED );
    else __atomic_add_fetch( &native_no_progress, 1, __ATOMIC_RELAXED );
    budget->remaining -= freed < budget->remaining ? freed : budget->remaining;
    if (!freed) budget->remaining = 0;
    return freed != 0;
}

void horizon_swap_native_profile(void)
{
    extern void wine_nx_runtime_trace( const char * );
    extern char *fake_heap_start, *fake_heap_end;
    struct wine_nx_native_heap_stats backing;
    struct mallinfo heap;
    size_t heap_size = fake_heap_end - fake_heap_start, available;
    char line[256];
    __malloc_lock( _REENT );
    heap = _mallinfo_r( _REENT );
    wine_nx_native_heap_stats( &backing );
    __malloc_unlock( _REENT );
    available = heap.fordblks + (heap_size > heap.arena ? heap_size - heap.arena : 0);
    snprintf( line, sizeof(line), "[HEAP] free_mb=%llu aligned_free_mb=%llu aligned_largest_mb=%llu "
              "small_free_mb=%llu small_largest_mb=%llu gap_mb=%llu holes=%llu/%llu scan=%s",
              (unsigned long long)available >> 20, (unsigned long long)backing.free >> 20,
              (unsigned long long)backing.largest >> 20, (unsigned long long)backing.small_free >> 20,
              (unsigned long long)backing.small_largest >> 20, (unsigned long long)backing.gap >> 20,
              (unsigned long long)backing.holes, (unsigned long long)backing.small_holes,
              backing.complete ? "ok" : "incomplete" );
    wine_nx_runtime_trace( line );
    if (!horizon_swap_enabled()) return;
    snprintf( line, sizeof(line), "[SWAP-NATIVE] deferred=%llu budget_zero=%llu fragmented=%llu no_progress=%llu "
              "cache_drop_mb=%llu reclaimed_mb=%llu reclaim_ms=%llu max_reclaim_ms=%llu",
              __atomic_load_n( &native_deferred, __ATOMIC_RELAXED ),
              __atomic_load_n( &native_budget_zero, __ATOMIC_RELAXED ),
              __atomic_load_n( &native_fragmented, __ATOMIC_RELAXED ),
              __atomic_load_n( &native_no_progress, __ATOMIC_RELAXED ),
              __atomic_load_n( &native_cache_dropped_bytes, __ATOMIC_RELAXED ) >> 20,
              __atomic_load_n( &native_freed_bytes, __ATOMIC_RELAXED ) >> 20,
              (unsigned long long)(armTicksToNs( __atomic_load_n( &native_reclaim_ticks, __ATOMIC_RELAXED ) ) / 1000000),
              (unsigned long long)(armTicksToNs( __atomic_load_n( &native_max_reclaim_ticks, __ATOMIC_RELAXED ) ) / 1000000) );
    wine_nx_runtime_trace( line );
}

void *__wrap__malloc_r( struct _reent *reent, size_t size )
{
    void *ptr;
    struct horizon_swap_reclaim_budget budget = { .remaining = SIZE_MAX };
    allocator_depth++;
    ptr = __real__malloc_r( reent, size );
    while (!ptr && allocator_depth == 1 && horizon_swap_native_reclaim( size, &budget ))
        ptr = __real__malloc_r( reent, size );
    if (ptr && horizon_swap_enabled()) wine_nx_native_heap_note_small_allocation( size );
    allocator_depth--;
    return ptr;
}

void *__wrap__calloc_r( struct _reent *reent, size_t count, size_t size )
{
    void *ptr;
    struct horizon_swap_reclaim_budget budget = { .remaining = SIZE_MAX };
    allocator_depth++;
    ptr = __real__calloc_r( reent, count, size );
    while (!ptr && size && allocator_depth == 1 && count <= SIZE_MAX / size &&
           horizon_swap_native_reclaim( count * size, &budget ))
        ptr = __real__calloc_r( reent, count, size );
    if (ptr && size && count <= SIZE_MAX / size && horizon_swap_enabled())
        wine_nx_native_heap_note_small_allocation( count * size );
    allocator_depth--;
    return ptr;
}

void *__wrap__realloc_r( struct _reent *reent, void *old, size_t size )
{
    void *ptr;
    struct horizon_swap_reclaim_budget budget = { .remaining = SIZE_MAX };
    allocator_depth++;
    ptr = wine_nx_native_realloc( reent, old, size );
    while (!ptr && allocator_depth == 1 && horizon_swap_native_reclaim( size, &budget ))
        ptr = wine_nx_native_realloc( reent, old, size );
    if (ptr && horizon_swap_enabled()) wine_nx_native_heap_note_small_allocation( size );
    allocator_depth--;
    return ptr;
}

void *__wrap__memalign_r( struct _reent *reent, size_t align, size_t size )
{
    void *ptr;
    struct horizon_swap_reclaim_budget budget = { .remaining = SIZE_MAX };
    allocator_depth++;
    ptr = wine_nx_native_memalign( reent, align, size );
    while (!ptr && allocator_depth == 1 && align && !(align & (align - 1)) &&
           horizon_swap_native_reclaim( size, &budget ))
        ptr = wine_nx_native_memalign( reent, align, size );
    if (ptr && (align < 4096 || size < 65536) && horizon_swap_enabled())
        wine_nx_native_heap_note_small_allocation( size <= SIZE_MAX - align ? size + align : SIZE_MAX );
    allocator_depth--;
    return ptr;
}
