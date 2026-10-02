#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <string.h>
#include <sys/reent.h>
#include "backing_heap.h"
#include "heap_snapshot.h"
#include "native_heap.h"
#ifdef WINE_NX_SHADER_ALLOC_CACHE
#include "shader_alloc_cache.h"
#endif

extern char *fake_heap_start, *fake_heap_end;
extern void *__real__sbrk_r( struct _reent *, ptrdiff_t );
extern void *__real__malloc_r( struct _reent *, size_t );
extern void *__real__calloc_r( struct _reent *, size_t, size_t );
extern void *__real__memalign_r( struct _reent *, size_t, size_t );
extern void *__real__realloc_r( struct _reent *, void *, size_t );
extern void __real__free_r( struct _reent *, void * );
extern struct mallinfo __real__mallinfo_r( struct _reent * );
extern size_t __real__malloc_usable_size_r( struct _reent *, void * );

/* Aligned storage grows down; newlib grows up. Both use newlib's recursive lock. */
static struct backing_block blocks[131072];
static struct backing_heap heap;
static uintptr_t backing_start = UINTPTR_MAX, backing_end;
enum { SMALL_FREE_CACHE_LIMIT = 16 * 1048576 };
static size_t cached_small_free;
static unsigned long long small_allocated, cached_allocated;
static size_t small_used;
static __thread unsigned int allocation_depth;

static size_t small_allocation_size( struct _reent *reent, void *pointer )
{
    /* Newlib's usable size excludes one word of the allocated chunk. */
    return pointer ? __real__malloc_usable_size_r( reent, pointer ) + sizeof(size_t) : 0;
}

void *wine_nx_native_malloc( struct _reent *reent, size_t size )
{
    void *result;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
    if (!allocation_depth && wine_nx_shader_alloc_active &&
        (result = wine_nx_shader_alloc_take( size, 16 ))) return result;
retry:
#endif
    allocation_depth++;
    result = __real__malloc_r( reent, size );
    allocation_depth--;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
    if (!result && !allocation_depth && wine_nx_shader_alloc_trim()) goto retry;
#endif
    if (!allocation_depth && result)
        __atomic_fetch_add( &small_used, small_allocation_size( reent, result ), __ATOMIC_RELAXED );
    return result;
}

void *wine_nx_native_calloc( struct _reent *reent, size_t count, size_t size )
{
    void *result;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
    if (!allocation_depth && wine_nx_shader_alloc_active && size && count <= SIZE_MAX / size &&
        (result = wine_nx_shader_alloc_take( count * size, 16 )))
    {
        memset( result, 0, count * size );
        return result;
    }
retry:
#endif
    allocation_depth++;
    result = __real__calloc_r( reent, count, size );
    allocation_depth--;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
    if (!result && size && count <= SIZE_MAX / size && !allocation_depth && wine_nx_shader_alloc_trim()) goto retry;
#endif
    if (!allocation_depth && result)
        __atomic_fetch_add( &small_used, small_allocation_size( reent, result ), __ATOMIC_RELAXED );
    return result;
}

static size_t free_lower_bound_locked(void)
{
    uintptr_t floor = (uintptr_t)__real__sbrk_r( _REENT, 0 );
    uintptr_t limit = heap.blocks ? heap.base + (size_t)heap.bottom * BACKING_UNIT : (uintptr_t)fake_heap_end;
    size_t available = (size_t)(heap.count - heap.bottom) * BACKING_UNIT - heap.used;
    if (limit > floor) available += limit - floor;
    return available;
}

static void cache_small_free_locked( size_t available )
{
    cached_small_free = available < SMALL_FREE_CACHE_LIMIT ? available : SMALL_FREE_CACHE_LIMIT;
    cached_allocated = __atomic_load_n( &small_allocated, __ATOMIC_RELAXED );
}

static void init_heap(void)
{
    uintptr_t start = ((uintptr_t)fake_heap_start + BACKING_UNIT - 1) & ~(uintptr_t)(BACKING_UNIT - 1);
    uintptr_t end = (uintptr_t)fake_heap_end & ~(uintptr_t)(BACKING_UNIT - 1);
    size_t count;
    if (heap.blocks || end <= start) return;
    count = (end - start) / BACKING_UNIT;
    if (count > sizeof(blocks) / sizeof(blocks[0])) count = sizeof(blocks) / sizeof(blocks[0]);
    backing_init( &heap, blocks, end - count * BACKING_UNIT, count );
    backing_end = end;
    __atomic_store_n( &backing_start, end, __ATOMIC_RELEASE );
}

static int owns_pointer( const void *pointer )
{
    return (uintptr_t)pointer >= __atomic_load_n( &backing_start, __ATOMIC_ACQUIRE ) &&
           (uintptr_t)pointer < backing_end;
}

void *__wrap__sbrk_r( struct _reent *reent, ptrdiff_t increment )
{
    void *result;
    uintptr_t current, limit;
    __malloc_lock( reent );
    current = (uintptr_t)__real__sbrk_r( reent, 0 );
    limit = heap.blocks ? heap.base + (size_t)heap.bottom * BACKING_UNIT : (uintptr_t)fake_heap_end;
    if (increment > 0 && (current > limit || (size_t)increment > limit - current))
    {
        reent->_errno = ENOMEM;
        result = (void *)-1;
    }
    else result = __real__sbrk_r( reent, increment );
    __malloc_unlock( reent );
    return result;
}

void *wine_nx_native_memalign( struct _reent *reent, size_t alignment, size_t size )
{
    void *result;
    uintptr_t floor;
    if (alignment < 4096 || (alignment & (alignment - 1)) || size < BACKING_UNIT)
    {
#ifdef WINE_NX_SHADER_ALLOC_CACHE
        if (!allocation_depth && wine_nx_shader_alloc_active &&
            (result = wine_nx_shader_alloc_take( size, alignment ))) return result;
retry:
#endif
        allocation_depth++;
        result = __real__memalign_r( reent, alignment, size );
        allocation_depth--;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
        if (!result && !allocation_depth && wine_nx_shader_alloc_trim()) goto retry;
#endif
        if (!allocation_depth && result)
            __atomic_fetch_add( &small_used, small_allocation_size( reent, result ), __ATOMIC_RELAXED );
        return result;
    }
    __malloc_lock( reent );
    init_heap();
    floor = (uintptr_t)__real__sbrk_r( reent, 0 );
    result = heap.blocks ? backing_alloc( &heap, floor, alignment, size ) : NULL;
    if (result)
        __atomic_store_n( &backing_start, heap.base + (size_t)heap.bottom * BACKING_UNIT, __ATOMIC_RELEASE );
    else reent->_errno = ENOMEM;
    __malloc_unlock( reent );
    return result;
}

#ifdef WINE_NX_SHADER_ALLOC_CACHE
void wine_nx_native_free_cached( void *const *pointers, unsigned int count, size_t bytes )
{
    struct _reent *current_reent = _REENT;
    __malloc_lock( current_reent );
    for (unsigned int i = 0; i < count; i++) __real__free_r( current_reent, pointers[i] );
    __atomic_fetch_sub( &small_used, bytes, __ATOMIC_RELAXED );
    __malloc_unlock( current_reent );
}
#endif

void __wrap__free_r( struct _reent *reent, void *pointer )
{
    if (!owns_pointer( pointer ))
    {
        if (!allocation_depth && pointer)
        {
            size_t size = small_allocation_size( reent, pointer );
#ifdef WINE_NX_SHADER_ALLOC_CACHE
            if (wine_nx_shader_alloc_active && wine_nx_shader_alloc_put( pointer, size - sizeof(size_t) )) return;
#endif
            __atomic_fetch_sub( &small_used, size, __ATOMIC_RELAXED );
        }
        __real__free_r( reent, pointer );
        return;
    }
    __malloc_lock( reent );
    backing_free( &heap, pointer );
    __atomic_store_n( &backing_start, heap.base + (size_t)heap.bottom * BACKING_UNIT, __ATOMIC_RELEASE );
    __malloc_unlock( reent );
}

size_t __wrap__malloc_usable_size_r( struct _reent *reent, void *pointer )
{
    size_t size;
    if (!owns_pointer( pointer )) return __real__malloc_usable_size_r( reent, pointer );
    __malloc_lock( reent );
    size = backing_size( &heap, pointer );
    __malloc_unlock( reent );
    return size;
}

void *wine_nx_native_realloc( struct _reent *reent, void *pointer, size_t size )
{
    void *result;
    size_t previous;
    if (!owns_pointer( pointer ))
    {
        previous = allocation_depth ? 0 : small_allocation_size( reent, pointer );
#ifdef WINE_NX_SHADER_ALLOC_CACHE
        if (!allocation_depth && wine_nx_shader_alloc_active && size)
        {
            size_t usable = pointer ? previous - sizeof(size_t) : 0;
            if (size <= usable && usable <= 65536 && size > usable / 2) return pointer;
            if ((result = wine_nx_shader_alloc_take( size, 16 )))
            {
                if (pointer) memcpy( result, pointer, size < usable ? size : usable );
                __wrap__free_r( reent, pointer );
                return result;
            }
        }
retry:
#endif
        allocation_depth++;
        result = __real__realloc_r( reent, pointer, size );
        allocation_depth--;
#ifdef WINE_NX_SHADER_ALLOC_CACHE
        if (!result && size && !allocation_depth && wine_nx_shader_alloc_trim()) goto retry;
#endif
        if (!allocation_depth && (result || !size))
            __atomic_fetch_add( &small_used, small_allocation_size( reent, result ) - previous, __ATOMIC_RELAXED );
        return result;
    }
    if (!size) { __wrap__free_r( reent, pointer ); return NULL; }
    __malloc_lock( reent );
    previous = backing_size( &heap, pointer );
    result = backing_resize( &heap, pointer, size ) ? pointer : NULL;
    __malloc_unlock( reent );
    if (result) return result;
    if (!(result = wine_nx_native_memalign( reent, BACKING_UNIT, size ))) return NULL;
    memcpy( result, pointer, previous );
    __wrap__free_r( reent, pointer );
    return result;
}

struct mallinfo __wrap__mallinfo_r( struct _reent *reent )
{
    struct mallinfo info;
    size_t reserved;
    __malloc_lock( reent );
    info = __real__mallinfo_r( reent );
    cache_small_free_locked( info.fordblks > 0 ? info.fordblks : 0 );
    reserved = (size_t)(heap.count - heap.bottom) * BACKING_UNIT;
    info.arena += reserved;
    info.uordblks += heap.used;
    info.fordblks += reserved - heap.used;
    info.ordblks += heap.holes;
    __malloc_unlock( reent );
    return info;
}

bool __wrap_os_get_available_system_memory( uint64_t *available )
{
    size_t used;
    uint64_t capacity = (uintptr_t)fake_heap_end > (uintptr_t)fake_heap_start ?
                        (uintptr_t)fake_heap_end - (uintptr_t)fake_heap_start : 0;
    __malloc_lock( _REENT );
    used = heap.used + __atomic_load_n( &small_used, __ATOMIC_RELAXED );
    __malloc_unlock( _REENT );
    *available = used < capacity ? capacity - used : 0;
    return capacity != 0;
}

size_t wine_nx_native_heap_free_lower_bound(void)
{
    size_t available;
    __malloc_lock( _REENT );
    available = free_lower_bound_locked();
    __malloc_unlock( _REENT );
    return available;
}

size_t wine_nx_native_heap_free_estimate( size_t *lower_bound )
{
    unsigned long long allocated, spent;
    size_t available;
    __malloc_lock( _REENT );
    available = free_lower_bound_locked();
    if (lower_bound) *lower_bound = available;
    allocated = __atomic_load_n( &small_allocated, __ATOMIC_RELAXED );
    spent = allocated >= cached_allocated ? allocated - cached_allocated : ULLONG_MAX;
    if (spent < cached_small_free) available += cached_small_free - spent;
    __malloc_unlock( _REENT );
    return available;
}

size_t wine_nx_native_heap_free_exact(void)
{
    struct mallinfo info;
    size_t available, small_free;
    __malloc_lock( _REENT );
    info = __real__mallinfo_r( _REENT );
    small_free = info.fordblks > 0 ? info.fordblks : 0;
    available = free_lower_bound_locked() + small_free;
    cache_small_free_locked( small_free );
    __malloc_unlock( _REENT );
    return available;
}

void wine_nx_native_heap_note_small_allocation( size_t size )
{
    size_t charge = size < SMALL_FREE_CACHE_LIMIT - 64 ? size + 64 : SMALL_FREE_CACHE_LIMIT;
    __atomic_add_fetch( &small_allocated, charge, __ATOMIC_RELAXED );
}

void wine_nx_native_heap_stats( struct wine_nx_native_heap_stats *stats )
{
    uintptr_t floor, limit;
    struct mallinfo info;
    __malloc_lock( _REENT );
    info = __real__mallinfo_r( _REENT );
    floor = (uintptr_t)__real__sbrk_r( _REENT, 0 );
    limit = heap.blocks ? heap.base + (size_t)heap.bottom * BACKING_UNIT : (uintptr_t)fake_heap_end;
    *stats = (struct wine_nx_native_heap_stats){
        .reserved = (size_t)(heap.count - heap.bottom) * BACKING_UNIT,
        .used = heap.used, .largest = backing_largest( &heap ),
        .gap = limit > floor ? limit - floor : 0, .holes = heap.holes,
        .small_free = info.fordblks, .small_largest = info.keepcost, .small_holes = info.ordblks
    };
    stats->free = stats->reserved - stats->used;
    if (stats->gap > stats->largest) stats->largest = stats->gap;
    stats->small_largest += stats->gap;
    stats->complete = horizon_heap_largest( (uintptr_t)fake_heap_start, floor, info.ordblks, &stats->small_largest );
    __malloc_unlock( _REENT );
}

#ifndef WINE_NX_SWAP_POC
void *__wrap__malloc_r( struct _reent *reent, size_t size )
{
    return wine_nx_native_malloc( reent, size );
}

void *__wrap__calloc_r( struct _reent *reent, size_t count, size_t size )
{
    return wine_nx_native_calloc( reent, count, size );
}

void *__wrap__memalign_r( struct _reent *reent, size_t alignment, size_t size )
{
    return wine_nx_native_memalign( reent, alignment, size );
}

void *__wrap__realloc_r( struct _reent *reent, void *pointer, size_t size )
{
    return wine_nx_native_realloc( reent, pointer, size );
}
#endif
