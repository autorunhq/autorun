#ifndef __SWITCH__
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "fex_jit.h"

#define FEX_JIT_MAX_SIZE (64u * 1024 * 1024)
#define FEX_ARENA_FIRST_SIZE (16u * 1024 * 1024)
#define FEX_ARENA_MAX_SIZE (128u * 1024 * 1024)
#define FEX_ARENA_LARGE_ALIGNMENT (2u * 1024 * 1024)

static struct fex_jit_arena
{
    struct fex_jit_arena *next;
    void *rx, *rw;
    size_t size;
    unsigned int users, ready;
#ifdef __SWITCH__
    void *backing;
    void *rw_reservation, *rx_reservation;
    size_t reservation_size;
    Handle handle;
    unsigned int owner_mapped, slave_mapped;
#endif
} *arenas;
static struct fex_jit_mapping
{
    struct fex_jit_mapping *next;
    struct fex_jit_arena *arena;
    void *rx, *rw;
    size_t size, offset;
} *mappings;
static size_t next_arena_size = FEX_ARENA_FIRST_SIZE;
static pthread_mutex_t mapping_lock = PTHREAD_MUTEX_INITIALIZER;
static int close_arena( struct fex_jit_arena *arena );

#ifdef __SWITCH__
extern void *horizon_reserve_native_code( size_t size, void **token );
extern void horizon_release_native_code( void *token );
extern void horizon_get_address_space_limits( void **start, void **limit );

static Result create_code_arena( struct fex_jit_arena *arena, size_t size, size_t alignment, const char **stage )
{
    void *rw, *rx;
    Handle handle;
    Result rc = MAKERESULT( Module_Kernel, KernelError_OutOfMemory );

    *stage = "syscalls";
    if (!envIsSyscallHinted(0x4b) || !envIsSyscallHinted(0x4c))
        return MAKERESULT( Module_Libnx, LibnxError_JitUnavailable );
    arena->handle = INVALID_HANDLE;
    arena->size = size;
    if (size & (alignment - 1)) alignment = 4096;
    arena->reservation_size = (size + alignment + 4096 + 65535) & ~(size_t)65535;
    *stage = "reserve-rw";
    rw = horizon_reserve_native_code( arena->reservation_size, &arena->rw_reservation );
    if (!rw) goto failed;
    arena->rw = (void *)(((uintptr_t)rw + 4096 + alignment - 1) & ~(uintptr_t)(alignment - 1));
    *stage = "reserve-rx";
    rx = horizon_reserve_native_code( arena->reservation_size, &arena->rx_reservation );
    if (!rx) goto failed;
    arena->rx = (void *)(((uintptr_t)rx + 4096 + alignment - 1) & ~(uintptr_t)(alignment - 1));
    *stage = "backing";
    arena->backing = aligned_alloc( alignment, size );
    if (!arena->backing)
    {
        rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory );
        goto failed;
    }
    *stage = "create";
    rc = svcCreateCodeMemory( &handle, arena->backing, size );
    if (R_SUCCEEDED(rc))
    {
        arena->handle = handle;
        *stage = "map-rw";
        rc = svcControlCodeMemory( arena->handle, CodeMapOperation_MapOwner, arena->rw, size, Perm_Rw );
        if (R_SUCCEEDED(rc))
        {
            arena->owner_mapped = 1;
            *stage = "map-rx";
            rc = svcControlCodeMemory( arena->handle, CodeMapOperation_MapSlave, arena->rx, size, Perm_Rx );
            if (R_SUCCEEDED(rc)) arena->slave_mapped = 1;
        }
    }
failed:
    if (R_FAILED(rc)) close_arena( arena );
    return rc;
}

static int can_shrink_arena( Result result, const char *stage )
{
    if (result == MAKERESULT( Module_Kernel, KernelError_OutOfMemory ) ||
        result == MAKERESULT( Module_Libnx, LibnxError_OutOfMemory )) return 1;
    return result == MAKERESULT( Module_Kernel, KernelError_ResourceExhausted ) &&
           (!strcmp( stage, "map-rw" ) || !strcmp( stage, "map-rx" ));
}
#endif

static struct fex_jit_arena *create_arena( size_t size )
{
    struct fex_jit_arena *arena = calloc( 1, sizeof(*arena) );
    size_t capacity = size > next_arena_size ? size : next_arena_size;
#ifdef __SWITCH__
    const char *stage = "metadata";
    unsigned int result = 0;
#endif

    if (!arena) return NULL;
#ifdef __SWITCH__
    {
        void *start, *limit;
        size_t alignment = FEX_ARENA_LARGE_ALIGNMENT;

        horizon_get_address_space_limits( &start, &limit );
        if ((uintptr_t)limit <= 0x100000000ULL)
        {
            if (capacity > FEX_JIT_MAX_SIZE) capacity = FEX_JIT_MAX_SIZE;
            alignment = 65536;
        }
        for (;;)
        {
            result = create_code_arena( arena, capacity, alignment, &stage );
            if (R_SUCCEEDED(result) || arena->size || !can_shrink_arena( result, stage )) break;
            next_arena_size = capacity / 2 < FEX_ARENA_FIRST_SIZE ? FEX_ARENA_FIRST_SIZE : capacity / 2;
            if (capacity == size) break;
            capacity = capacity / 2 < size ? size : capacity / 2;
            capacity = (capacity + 4095) & ~(size_t)4095;
        }
    }
    if (R_FAILED(result))
    {
        if (arena->size)
        {
            arena->next = arenas;
            arenas = arena;
        }
        else free( arena );
        return NULL;
    }
#else
    {
        int fd = memfd_create( "wine-nx-fex", 0 );
        void *write = MAP_FAILED, *exec = MAP_FAILED;

        if (fd >= 0)
        {
            if (!ftruncate( fd, capacity ))
            {
                write = mmap( NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
                exec = mmap( NULL, capacity, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0 );
            }
            close( fd );
        }
        if (write == MAP_FAILED || exec == MAP_FAILED)
        {
            if (write != MAP_FAILED) munmap( write, capacity );
            if (exec != MAP_FAILED) munmap( exec, capacity );
            free( arena );
            return NULL;
        }
        arena->rw = write;
        arena->rx = exec;
        arena->size = capacity;
    }
#endif
    arena->ready = 1;
    arena->next = arenas;
    arenas = arena;
    next_arena_size = capacity < FEX_ARENA_MAX_SIZE / 2 ? capacity * 2 : FEX_ARENA_MAX_SIZE;
    return arena;
}

static int find_arena_space( struct fex_jit_arena *arena, size_t size, size_t *offset )
{
    const struct fex_jit_mapping *mapping;
    size_t at = 0;

    if (!arena->ready) return 0;
    while (at <= arena->size && size <= arena->size - at)
    {
        for (mapping = mappings; mapping; mapping = mapping->next)
            if (mapping->arena == arena && at < mapping->offset + mapping->size &&
                mapping->offset < at + size) break;
        if (!mapping) { *offset = at; return 1; }
        at = mapping->offset + mapping->size;
    }
    return 0;
}

static struct fex_jit_mapping *find_mapping( const void *address, size_t size )
{
    uintptr_t start = (uintptr_t)address;
    struct fex_jit_mapping *mapping;

    for (mapping = mappings; mapping; mapping = mapping->next)
    {
        uintptr_t base = (uintptr_t)mapping->rx;
        if (base && start >= base && start - base < mapping->size &&
            size <= mapping->size - (start - base)) return mapping;
    }
    return NULL;
}

int wine_nx_fex_jit_create( size_t size, void **rx, void **rw )
{
    struct fex_jit_mapping *mapping;
    struct fex_jit_arena *arena, *candidate;
    size_t offset = 0;
    int error = ENOMEM;

    if (!rx || !rw) return EINVAL;
    *rx = *rw = NULL;
    if (!size || size > FEX_JIT_MAX_SIZE) return EINVAL;
    size = (size + 4095) & ~(size_t)4095;
    pthread_mutex_lock( &mapping_lock );
    mapping = calloc( 1, sizeof(*mapping) );
    if (!mapping) goto done;
    arena = NULL;
    for (candidate = arenas; candidate; candidate = candidate->next)
    {
        size_t candidate_offset;
        if (find_arena_space( candidate, size, &candidate_offset ) &&
            (!arena || candidate->size < arena->size))
        {
            arena = candidate;
            offset = candidate_offset;
        }
    }
    if (!arena && !(arena = create_arena( size ))) goto done;
    arena->users++;
    mapping->arena = arena;
    mapping->offset = offset;
    mapping->rx = (char *)arena->rx + offset;
    mapping->rw = (char *)arena->rw + offset;
    mapping->size = size;
    *rx = mapping->rx;
    *rw = mapping->rw;
    error = 0;
done:
    if (mapping)
    {
        if (mapping->size)
        {
            mapping->next = mappings;
            mappings = mapping;
        }
        else free( mapping );
    }
    pthread_mutex_unlock( &mapping_lock );
    return error;
}

static int close_arena( struct fex_jit_arena *arena )
{
    arena->ready = 0;
#ifdef __SWITCH__
    if (arena->slave_mapped)
    {
        if (R_FAILED(svcControlCodeMemory( arena->handle, CodeMapOperation_UnmapSlave,
                                         arena->rx, arena->size, 0 ))) return EIO;
        arena->slave_mapped = 0;
    }
    if (arena->owner_mapped)
    {
        if (R_FAILED(svcControlCodeMemory( arena->handle, CodeMapOperation_UnmapOwner,
                                         arena->rw, arena->size, 0 ))) return EIO;
        arena->owner_mapped = 0;
    }
    if (arena->handle != INVALID_HANDLE)
    {
        if (R_FAILED(svcCloseHandle( arena->handle ))) return EIO;
        arena->handle = INVALID_HANDLE;
    }
    free( arena->backing );
    if (arena->rx_reservation) horizon_release_native_code( arena->rx_reservation );
    if (arena->rw_reservation) horizon_release_native_code( arena->rw_reservation );
#else
    int write_error = munmap( arena->rw, arena->size );
    int exec_error = munmap( arena->rx, arena->size );
    if (write_error || exec_error) return EIO;
#endif
    memset( arena, 0, sizeof(*arena) );
    return 0;
}

static int close_mapping( struct fex_jit_mapping *mapping )
{
    struct fex_jit_arena *arena = mapping->arena, **entry, *next;

    if (arena->users > 1) { arena->users--; return 0; }
    for (entry = &arenas; *entry != arena; entry = &(*entry)->next) {}
    next = arena->next;
    if (close_arena( arena )) return EIO;
    *entry = next;
    free( arena );
    if (!arenas) next_arena_size = FEX_ARENA_FIRST_SIZE;
    return 0;
}

int wine_nx_fex_jit_close( void *rx )
{
    struct fex_jit_mapping **entry, *mapping, *next;
    int error = EINVAL;
    pthread_mutex_lock( &mapping_lock );
    for (entry = &mappings; (mapping = *entry); entry = &mapping->next)
    {
        if (!rx || mapping->rx != rx) continue;
        next = mapping->next;
        if (!(error = close_mapping( mapping )))
        {
            *entry = next;
            free( mapping );
        }
        break;
    }
    pthread_mutex_unlock( &mapping_lock );
    return error;
}

int wine_nx_fex_jit_flush( const void *rx, size_t size )
{
    struct fex_jit_mapping *mapping;
    int error = EINVAL;
    pthread_mutex_lock( &mapping_lock );
    if (size && (mapping = find_mapping( rx, size )) && mapping->arena->ready)
    {
        size_t offset = (uintptr_t)rx - (uintptr_t)mapping->rx;
        char *rw = (char *)mapping->rw + offset;
#ifdef __SWITCH__
        armDCacheFlush( rw, size );
        armICacheInvalidate( (void *)rx, size );
#else
        __builtin___clear_cache( rw, rw + size );
        __builtin___clear_cache( (char *)rx, (char *)rx + size );
#endif
        error = 0;
    }
    pthread_mutex_unlock( &mapping_lock );
    return error;
}

size_t wine_nx_fex_jit_size( const void *rx )
{
    struct fex_jit_mapping *mapping;
    size_t size = 0;
    pthread_mutex_lock( &mapping_lock );
    if ((mapping = find_mapping( rx, 1 )) && mapping->rx == rx) size = mapping->size;
    pthread_mutex_unlock( &mapping_lock );
    return size;
}

size_t wine_nx_fex_jit_release(void)
{
    size_t retained = 0;
    struct fex_jit_mapping **entry, *mapping, *next;
    struct fex_jit_arena **arena_entry, *arena, *arena_next;
    pthread_mutex_lock( &mapping_lock );
    for (entry = &mappings; (mapping = *entry);)
    {
        next = mapping->next;
        if (close_mapping( mapping ))
        {
            entry = &mapping->next;
        }
        else
        {
            *entry = next;
            free( mapping );
        }
    }
    for (arena_entry = &arenas; (arena = *arena_entry);)
    {
        arena_next = arena->next;
        if (arena->users || close_arena( arena ))
        {
            retained += arena->size;
            arena_entry = &arena->next;
        }
        else
        {
            *arena_entry = arena_next;
            free( arena );
        }
    }
    if (!arenas)
    {
        next_arena_size = FEX_ARENA_FIRST_SIZE;
    }
    pthread_mutex_unlock( &mapping_lock );
    return retained;
}
