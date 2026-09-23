#include <switch.h>
#include "horizon_swap.h"
#include "swap_ipc.h"
#include <sys/reent.h>
#include <malloc.h>
#include <stdio.h>

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
extern void *__real__realloc_r( struct _reent *, void *, size_t );
extern void *__real__memalign_r( struct _reent *, size_t, size_t );
static __thread unsigned int allocator_depth;
static __thread unsigned int allocator_deferred;
static unsigned long long allocation_failures, largest_failed_allocation;

void horizon_swap_native_begin(void) { allocator_deferred++; }
void horizon_swap_native_end(void) { allocator_deferred--; }

int horizon_swap_native_reclaim( size_t size, size_t *budget )
{
    size_t freed;
    if (!size || allocator_deferred || !horizon_swap_enabled()) return 0;
    if (*budget == SIZE_MAX)
    {
        extern char *fake_heap_start, *fake_heap_end;
        struct mallinfo heap = mallinfo();
        size_t heap_size = fake_heap_end - fake_heap_start;
        size_t available = heap.fordblks + (heap_size > heap.arena ? heap_size - heap.arena : 0);
        /* A fragmented heap needs a contiguous block, not unlimited page-outs. */
        *budget = size > heap_size ? 0 : size > available ? size - available :
                  size < 16 * 1048576u ? size : 16 * 1048576u;
    }
    if (!*budget) return 0;
    freed = horizon_swap_reclaim( *budget );
    *budget -= freed < *budget ? freed : *budget;
    if (!freed) *budget = 0;
    return freed != 0;
}

void horizon_swap_native_failed( size_t size )
{
    unsigned long long largest;
    if (!size || allocator_depth > 1 || allocator_deferred || !horizon_swap_enabled()) return;
    largest = __atomic_load_n( &largest_failed_allocation, __ATOMIC_RELAXED );
    while (size > largest && !__atomic_compare_exchange_n( &largest_failed_allocation, &largest,
                                                          size, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
    __atomic_add_fetch( &allocation_failures, 1, __ATOMIC_RELEASE );
}

void horizon_swap_native_report(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    extern void wine_nx_runtime_trace( const char * );
    static unsigned long long reported;
    unsigned long long failures = __atomic_load_n( &allocation_failures, __ATOMIC_ACQUIRE );
    unsigned long long previous = __atomic_load_n( &reported, __ATOMIC_RELAXED );
    struct mallinfo heap;
    size_t heap_size, uncommitted;
    char line[224];
    do
    {
        if (failures <= previous) return;
    } while (!__atomic_compare_exchange_n( &reported, &previous, failures, 0,
                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED ));
    heap = mallinfo();
    heap_size = fake_heap_end - fake_heap_start;
    uncommitted = heap_size > heap.arena ? heap_size - heap.arena : 0;
    snprintf( line, sizeof(line), "[SWAP-NATIVE] failures=%llu largest_failed_kb=%llu "
              "free_kb=%llu tail_kb=%llu holes=%llu", failures,
              __atomic_load_n( &largest_failed_allocation, __ATOMIC_RELAXED ) / 1024,
              (unsigned long long)(heap.fordblks + uncommitted) / 1024,
              (unsigned long long)(heap.keepcost + uncommitted) / 1024,
              (unsigned long long)heap.ordblks );
    wine_nx_runtime_trace( line );
}

void *__wrap__malloc_r( struct _reent *reent, size_t size )
{
    void *ptr;
    size_t budget = SIZE_MAX;
    allocator_depth++;
    ptr = __real__malloc_r( reent, size );
    while (!ptr && allocator_depth == 1 && horizon_swap_native_reclaim( size, &budget ))
        ptr = __real__malloc_r( reent, size );
    if (!ptr) horizon_swap_native_failed( size );
    allocator_depth--;
    return ptr;
}

void *__wrap__calloc_r( struct _reent *reent, size_t count, size_t size )
{
    void *ptr;
    size_t budget = SIZE_MAX;
    allocator_depth++;
    ptr = __real__calloc_r( reent, count, size );
    while (!ptr && size && allocator_depth == 1 && count <= SIZE_MAX / size &&
           horizon_swap_native_reclaim( count * size, &budget ))
        ptr = __real__calloc_r( reent, count, size );
    if (!ptr && size && count <= SIZE_MAX / size) horizon_swap_native_failed( count * size );
    allocator_depth--;
    return ptr;
}

void *__wrap__realloc_r( struct _reent *reent, void *old, size_t size )
{
    void *ptr;
    size_t budget = SIZE_MAX;
    allocator_depth++;
    ptr = __real__realloc_r( reent, old, size );
    while (!ptr && allocator_depth == 1 && horizon_swap_native_reclaim( size, &budget ))
        ptr = __real__realloc_r( reent, old, size );
    if (!ptr) horizon_swap_native_failed( size );
    allocator_depth--;
    return ptr;
}

void *__wrap__memalign_r( struct _reent *reent, size_t align, size_t size )
{
    void *ptr;
    size_t budget = SIZE_MAX;
    allocator_depth++;
    ptr = __real__memalign_r( reent, align, size );
    while (!ptr && allocator_depth == 1 && align && !(align & (align - 1)) &&
           horizon_swap_native_reclaim( size, &budget ))
        ptr = __real__memalign_r( reent, align, size );
    if (!ptr && align && !(align & (align - 1))) horizon_swap_native_failed( size );
    allocator_depth--;
    return ptr;
}
