#ifndef WINE_NX_SWAP_FILE_H
#define WINE_NX_SWAP_FILE_H

#include "swap_store.h"

#define SWAP_FILE_UNIT 4096

/* Serialized by the owning memory manager; no allocation during I/O. */
struct swap_file
{
    struct swap_store store;
    unsigned char *used;
    unsigned int units, hint;
    uint64_t used_units, scan_units, scan_ticks, no_run;
    uint64_t writes, write_bytes, write_ticks, max_write_ticks, write_errors;
    uint64_t reads, read_bytes, read_ticks, max_read_ticks, read_errors;
};

int swap_file_open( struct swap_file *file, const char *directory, unsigned int megabytes );
void swap_file_close( struct swap_file *file );
int swap_file_save( void *context, const void *data, size_t size, unsigned int *token );
int swap_file_load( void *context, unsigned int token, void *data, size_t size );
void swap_file_discard( void *context, unsigned int token, size_t size );

#endif
