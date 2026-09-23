#ifndef WINE_NX_SWAP_STORE_H
#define WINE_NX_SWAP_STORE_H

#include <stddef.h>
#include <stdint.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define SWAP_STORE_SEGMENT (UINT64_C(1024) * 1024 * 1024)
#define SWAP_STORE_FILES 8

struct swap_store
{
    int fd[SWAP_STORE_FILES];
    uint64_t size;
    const char *operation;
    uint64_t offset;
    uint32_t fs_error;
#ifdef __SWITCH__
    Service io_session;
    FsFile files[SWAP_STORE_FILES];
#endif
};

typedef int (*swap_progress)( void *context, const char *phase, uint64_t done, uint64_t total );

int swap_store_open( struct swap_store *store, const char *directory, unsigned int megabytes,
                     swap_progress progress, void *context );
int swap_store_open_existing( struct swap_store *store, const char *directory, unsigned int megabytes );
int swap_store_read( struct swap_store *store, uint64_t offset, void *data, size_t size );
int swap_store_write( struct swap_store *store, uint64_t offset, const void *data, size_t size );
void swap_store_close( struct swap_store *store );
int swap_store_remove( const char *directory );

#endif
