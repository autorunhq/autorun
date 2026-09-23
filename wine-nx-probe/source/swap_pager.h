#ifndef WINE_NX_SWAP_PAGER_H
#define WINE_NX_SWAP_PAGER_H

#include <stddef.h>
#include <stdint.h>

/* The caller serializes access; all metadata and callbacks must stay resident. */
struct swap_pager_ops
{
    int (*map)( void *context, unsigned int page, unsigned int slot );
    int (*unmap)( void *context, unsigned int page, unsigned int slot );
    int (*read)( void *context, unsigned int page, unsigned int slot );
    int (*write)( void *context, unsigned int page, unsigned int slot );
    void (*zero)( void *context, unsigned int slot );
};

struct swap_page { int slot; unsigned int pins; unsigned char stored; };
struct swap_pager
{
    struct swap_page *pages;
    int *owners;
    unsigned int page_count, slot_count, hand;
    uint64_t faults, reads, writes;
    struct swap_pager_ops ops;
    void *context;
    int poisoned;
};

int swap_pager_init( struct swap_pager *pager, unsigned int pages, unsigned int slots,
                     const struct swap_pager_ops *ops, void *context );
int swap_pager_fault( struct swap_pager *pager, unsigned int page );
int swap_pager_pin( struct swap_pager *pager, unsigned int page );
int swap_pager_unpin( struct swap_pager *pager, unsigned int page );
int swap_pager_close( struct swap_pager *pager );

#endif
