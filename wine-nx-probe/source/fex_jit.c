#ifndef __SWITCH__
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
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

static struct fex_jit_mapping
{
    struct fex_jit_mapping *next;
    void *rx, *rw;
    size_t size;
#ifdef __SWITCH__
    void *backing;
    void *rw_reservation, *rx_reservation;
    Handle handle;
    unsigned int owner_mapped, slave_mapped;
#endif
} *mappings;
static pthread_mutex_t mapping_lock = PTHREAD_MUTEX_INITIALIZER;
extern void wine_nx_runtime_trace( const char *message ) __attribute__((weak));
static int close_mapping( struct fex_jit_mapping *mapping );

#ifdef __SWITCH__
extern void *horizon_reserve_native_code( size_t size, void **token );
extern void horizon_release_native_code( void *token );

static Result create_code_mapping( struct fex_jit_mapping *mapping, size_t size )
{
    void *rw, *rx;
    Result rc = MAKERESULT( Module_Kernel, KernelError_OutOfMemory );

    if (!envIsSyscallHinted(0x4b) || !envIsSyscallHinted(0x4c))
        return MAKERESULT( Module_Libnx, LibnxError_JitUnavailable );
    mapping->handle = INVALID_HANDLE;
    mapping->size = size;
    rw = horizon_reserve_native_code( size + 8192, &mapping->rw_reservation );
    if (!rw) goto failed;
    mapping->rw = (char *)rw + 4096;
    rx = horizon_reserve_native_code( size + 8192, &mapping->rx_reservation );
    if (!rx) goto failed;
    mapping->rx = (char *)rx + 4096;
    mapping->backing = aligned_alloc( 4096, size );
    if (!mapping->backing)
    {
        rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory );
        goto failed;
    }
    rc = svcCreateCodeMemory( &mapping->handle, mapping->backing, size );
    if (R_SUCCEEDED(rc))
    {
        rc = svcControlCodeMemory( mapping->handle, CodeMapOperation_MapOwner, mapping->rw, size, Perm_Rw );
        if (R_SUCCEEDED(rc))
        {
            mapping->owner_mapped = 1;
            rc = svcControlCodeMemory( mapping->handle, CodeMapOperation_MapSlave, mapping->rx, size, Perm_Rx );
            if (R_SUCCEEDED(rc)) mapping->slave_mapped = 1;
        }
    }
failed:
    if (R_FAILED(rc)) close_mapping( mapping );
    return rc;
}
#endif

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
    unsigned int count = 0, result = 0;
    size_t bytes = 0;
    char message[224];
    size_t reserved = 0;
    int error = ENOMEM;

    if (!rx || !rw) return EINVAL;
    *rx = *rw = NULL;
    if (!size || size > FEX_JIT_MAX_SIZE) return EINVAL;
    size = (size + 4095) & ~(size_t)4095;
    pthread_mutex_lock( &mapping_lock );
    mapping = calloc( 1, sizeof(*mapping) );
    if (!mapping) goto done;
#ifdef __SWITCH__
    result = create_code_mapping( mapping, size );
    if (R_FAILED(result)) goto done;
#else
    {
        int fd = memfd_create( "wine-nx-fex", 0 );
        void *write = MAP_FAILED, *exec = MAP_FAILED;
        if (fd < 0) goto done;
        if (!ftruncate( fd, size ))
        {
            write = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
            exec = mmap( NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0 );
        }
        close( fd );
        if (write == MAP_FAILED || exec == MAP_FAILED)
        {
            if (write != MAP_FAILED) munmap( write, size );
            if (exec != MAP_FAILED) munmap( exec, size );
            goto done;
        }
        mapping->rw = write;
        mapping->rx = exec;
    }
#endif
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
    for (mapping = mappings; mapping; mapping = mapping->next)
    {
        count++;
        bytes += mapping->size;
#ifdef __SWITCH__
        reserved += (size_t)(!!mapping->rw_reservation + !!mapping->rx_reservation) * (mapping->size + 8192);
#endif
    }
    snprintf( message, sizeof(message), "[FEX JIT] size_kb=%zu error=%d rc=%#x buffers=%u active_kb=%zu reserved_kb=%zu rx=%p rw=%p",
              size >> 10, error, result, count, bytes >> 10, reserved >> 10, *rx, *rw );
    pthread_mutex_unlock( &mapping_lock );
    if (wine_nx_runtime_trace) wine_nx_runtime_trace( message );
    return error;
}

static int close_mapping( struct fex_jit_mapping *mapping )
{
#ifdef __SWITCH__
    if (mapping->slave_mapped)
    {
        if (R_FAILED(svcControlCodeMemory( mapping->handle, CodeMapOperation_UnmapSlave,
                                         mapping->rx, mapping->size, 0 ))) return EIO;
        mapping->slave_mapped = 0;
    }
    if (mapping->owner_mapped)
    {
        if (R_FAILED(svcControlCodeMemory( mapping->handle, CodeMapOperation_UnmapOwner,
                                         mapping->rw, mapping->size, 0 ))) return EIO;
        mapping->owner_mapped = 0;
    }
    if (mapping->handle != INVALID_HANDLE)
    {
        if (R_FAILED(svcCloseHandle( mapping->handle ))) return EIO;
        mapping->handle = INVALID_HANDLE;
    }
    free( mapping->backing );
    if (mapping->rx_reservation) horizon_release_native_code( mapping->rx_reservation );
    if (mapping->rw_reservation) horizon_release_native_code( mapping->rw_reservation );
#else
    int write_error = munmap( mapping->rw, mapping->size );
    int exec_error = munmap( mapping->rx, mapping->size );
    if (write_error || exec_error) return EIO;
#endif
    memset( mapping, 0, sizeof(*mapping) );
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
    if (size && (mapping = find_mapping( rx, size )))
    {
        char *rw = (char *)mapping->rw + ((uintptr_t)rx - (uintptr_t)mapping->rx);
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
    pthread_mutex_lock( &mapping_lock );
    for (entry = &mappings; (mapping = *entry);)
    {
        next = mapping->next;
        if (close_mapping( mapping ))
        {
            retained += mapping->size;
            entry = &mapping->next;
        }
        else
        {
            *entry = next;
            free( mapping );
        }
    }
    pthread_mutex_unlock( &mapping_lock );
    return retained;
}
