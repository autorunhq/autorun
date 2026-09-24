#ifndef HORIZON_SWAP_H
#define HORIZON_SWAP_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

struct horizon_swap_storage
{
    void *context;
    int (*save)( void *, const void *, size_t, unsigned int * );
    int (*load)( void *, unsigned int, void *, size_t );
    void (*discard)( void *, unsigned int, size_t );
    uint64_t capacity;
};

struct horizon_swap_entry { unsigned int token; unsigned char detached; };
struct horizon_swap_ops
{
    int (*unmap)( void *, void * );
    int (*map)( void *, void * ); /* Retry completes any partially restored mappings. */
    void *(*alloc)( void *, size_t );
    void (*free)( void *, void * );
};

/* The caller serializes faults, pinning and mapping changes. */
static inline int horizon_swap_out( struct horizon_swap_entry *entry, void **memory, size_t size,
                                    const struct horizon_swap_ops *ops, void *context,
                                    const struct horizon_swap_storage *storage )
{
    int error;
    if (entry->token) return 0;
    if (!entry->detached && ops->unmap( context, *memory )) return -1;
    entry->detached = 1;
    if (storage->save( storage->context, *memory, size, &entry->token ))
    {
        error = errno;
        if (!ops->map( context, *memory )) entry->detached = 0;
        errno = error;
        return -1;
    }
    ops->free( context, *memory );
    *memory = NULL;
    return 0;
}

static inline int horizon_swap_in( struct horizon_swap_entry *entry, void **memory, size_t size,
                                   const struct horizon_swap_ops *ops, void *context,
                                   const struct horizon_swap_storage *storage )
{
    void *data = *memory;
    int error;
    if (!entry->detached) return 0;
    if (!data)
    {
        if (!(data = ops->alloc( context, size ))) { errno = ENOMEM; return -1; }
        if (storage->load( storage->context, entry->token, data, size )) goto failed;
    }
    *memory = data;
    if (ops->map( context, data )) return -1;
    entry->detached = 0;
    if (entry->token) storage->discard( storage->context, entry->token, size );
    entry->token = 0;
    return 0;
failed:
    error = errno;
    if (!*memory) ops->free( context, data );
    errno = error;
    return -1;
}

void horizon_swap_configure( const struct horizon_swap_storage *storage );
int horizon_swap_enabled(void);
size_t horizon_swap_reclaim( size_t size );
int horizon_swap_native_reclaim( size_t size, size_t *budget );
void horizon_swap_native_begin(void);
void horizon_swap_native_end(void);
struct horizon_swap_pin
{
    const void *addr;
    size_t size;
    struct horizon_swap_pin *next;
};
void horizon_swap_track( void *addr, size_t size );
int horizon_swap_exclude( const void *addr, size_t size );
int horizon_swap_pin_begin( struct horizon_swap_pin *pin, const void *addr, size_t size );
void horizon_swap_pin_end( struct horizon_swap_pin *pin );
int horizon_swap_may_contain( const void *addr, size_t size );
int horizon_swap_lock( const void *addr, size_t size );
int horizon_swap_unlock( const void *addr, size_t size );
void horizon_swap_report(void);
void horizon_swap_get_memory_info( unsigned long long *total, unsigned long long *available );

#endif
