#include "config.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "../fex/unixlib.h"
#include "fex_jit.h"

extern NTSTATUS wine_nx_set_fex_code_range( void *base, SIZE_T size, BOOL enable );
extern int wine_nx_fex_exception_attach(void);
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;

static NTSTATUS unsupported( void *args )
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS query( void *args )
{
    struct wine_nx_fex_query *p = args;
    if (!p || p->size != sizeof(*p) || p->version != WINE_NX_FEX_ABI_VERSION)
        return STATUS_REVISION_MISMATCH;
    return wine_nx_fex_exception_attach() ? STATUS_NO_MEMORY : STATUS_SUCCESS;
}

static NTSTATUS attach_thread( void *args )
{
    (void)args;
    return wine_nx_fex_exception_attach() ? STATUS_NO_MEMORY : STATUS_SUCCESS;
}

static NTSTATUS allocate( void *args )
{
    struct wine_nx_fex_memory *p = args;
    void *rx, *rw;
    NTSTATUS status;
    int error;

    if (!p || !p->size || (p->size & 4095)) return STATUS_INVALID_PARAMETER;
    p->rx = p->rw = 0;
    pthread_mutex_lock( &allocation_lock );
    error = wine_nx_fex_jit_create( p->size, &rx, &rw );
    if (error)
    {
        pthread_mutex_unlock( &allocation_lock );
        return error == EINVAL ? STATUS_INVALID_PARAMETER : STATUS_NO_MEMORY;
    }
    status = wine_nx_set_fex_code_range( rx, p->size, TRUE );
    if (status)
    {
        wine_nx_fex_jit_close( rx );
        pthread_mutex_unlock( &allocation_lock );
        return status;
    }
    p->rx = (ULONG_PTR)rx;
    p->rw = (ULONG_PTR)rw;
    pthread_mutex_unlock( &allocation_lock );
    return STATUS_SUCCESS;
}

static NTSTATUS release( void *args )
{
    struct wine_nx_fex_memory *p = args;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    void *rx;
    if (!p || !p->size || !p->rx) return STATUS_INVALID_PARAMETER;
    rx = (void *)(ULONG_PTR)p->rx;
    pthread_mutex_lock( &allocation_lock );
    if (wine_nx_fex_jit_size( rx ) != p->size) goto done;
    if ((status = wine_nx_set_fex_code_range( rx, p->size, FALSE ))) goto done;
    if (wine_nx_fex_jit_close( rx ))
    {
        wine_nx_set_fex_code_range( rx, p->size, TRUE );
        status = STATUS_UNSUCCESSFUL;
    }
done:
    pthread_mutex_unlock( &allocation_lock );
    return status;
}

static NTSTATUS flush( void *args )
{
    struct wine_nx_fex_memory *p = args;
    if (!p || wine_nx_fex_jit_flush( (void *)(ULONG_PTR)p->rx, p->size )) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS heap_allocate( void *args )
{
    struct wine_nx_fex_heap *p = args;
    void *pointer;
    if (!p || !p->size || p->size > SIZE_MAX || p->zero > 1) return STATUS_INVALID_PARAMETER;
    pointer = p->zero ? calloc( 1, p->size ) : malloc( p->size );
    p->pointer = (ULONG_PTR)pointer;
    return pointer ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

static NTSTATUS heap_reallocate( void *args )
{
    struct wine_nx_fex_heap *p = args;
    void *pointer;
    if (!p || !p->size || p->size > SIZE_MAX || p->zero) return STATUS_INVALID_PARAMETER;
    pointer = realloc( (void *)(ULONG_PTR)p->pointer, p->size );
    if (!pointer) return STATUS_NO_MEMORY;
    p->pointer = (ULONG_PTR)pointer;
    return STATUS_SUCCESS;
}

static NTSTATUS heap_free( void *args )
{
    struct wine_nx_fex_heap *p = args;
    if (!p) return STATUS_INVALID_PARAMETER;
    free( (void *)(ULONG_PTR)p->pointer );
    p->pointer = 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t wine_nx_fex_unix_funcs[] =
{
    unsupported, unsupported, unsupported, unsupported, unsupported, unsupported, unsupported,
    query, allocate, release, flush, attach_thread, heap_allocate, heap_reallocate, heap_free
};

C_ASSERT( sizeof(struct wine_nx_fex_query) == 8 );
C_ASSERT( sizeof(struct wine_nx_fex_memory) == 24 );
C_ASSERT( sizeof(struct wine_nx_fex_heap) == 24 );
