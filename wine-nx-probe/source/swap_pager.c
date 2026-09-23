#include "swap_pager.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int swap_pager_init( struct swap_pager *pager, unsigned int pages, unsigned int slots,
                     const struct swap_pager_ops *ops, void *context )
{
    unsigned int i;
    memset( pager, 0, sizeof(*pager) );
    if (!pages || !slots || slots > pages || pages > INT_MAX ||
        !ops || !ops->map || !ops->unmap || !ops->read || !ops->write || !ops->zero)
    { errno = EINVAL; return -1; }
    pager->pages = calloc( pages, sizeof(*pager->pages) );
    pager->owners = calloc( slots, sizeof(*pager->owners) );
    if (!pager->pages || !pager->owners)
    {
        free( pager->pages );
        free( pager->owners );
        memset( pager, 0, sizeof(*pager) );
        errno = ENOMEM;
        return -1;
    }
    for (i = 0; i < pages; i++) pager->pages[i].slot = -1;
    for (i = 0; i < slots; i++) pager->owners[i] = -1;
    pager->page_count = pages;
    pager->slot_count = slots;
    pager->ops = *ops;
    pager->context = context;
    return 0;
}

int swap_pager_fault( struct swap_pager *pager, unsigned int page )
{
    unsigned int i, slot = 0;
    int old, error;

    if (page >= pager->page_count) { errno = EINVAL; return -1; }
    if (pager->poisoned) { errno = EIO; return -1; }
    if (pager->pages[page].slot >= 0) return 0;
    for (i = 0; i < pager->slot_count; i++)
        if (pager->owners[i] < 0) break;
    if (i < pager->slot_count) slot = i;
    else
    {
        for (i = 0; i < pager->slot_count; i++)
        {
            slot = pager->hand;
            pager->hand = (pager->hand + 1) % pager->slot_count;
            if (!pager->pages[pager->owners[slot]].pins) break;
        }
        if (i == pager->slot_count) { errno = EBUSY; return -1; }
    }
    old = pager->owners[slot];
    if (old >= 0)
    {
        if (pager->ops.unmap( pager->context, old, slot )) return -1;
        if (pager->ops.write( pager->context, old, slot ))
        {
            error = errno;
            if (pager->ops.map( pager->context, old, slot )) pager->poisoned = 1;
            errno = error;
            return -1;
        }
        pager->pages[old].stored = 1;
        pager->pages[old].slot = -1;
        pager->owners[slot] = -1;
        pager->writes++;
    }
    if (pager->pages[page].stored)
    {
        if (pager->ops.read( pager->context, page, slot )) return -1;
        pager->reads++;
    }
    else pager->ops.zero( pager->context, slot );
    /* A failed map callback must leave the slot unmapped. */
    if (pager->ops.map( pager->context, page, slot )) return -1;
    pager->owners[slot] = page;
    pager->pages[page].slot = slot;
    pager->faults++;
    return 0;
}

int swap_pager_pin( struct swap_pager *pager, unsigned int page )
{
    if (page >= pager->page_count || pager->pages[page].pins == UINT_MAX)
    { errno = EINVAL; return -1; }
    if (swap_pager_fault( pager, page )) return -1;
    pager->pages[page].pins++;
    return 0;
}

int swap_pager_unpin( struct swap_pager *pager, unsigned int page )
{
    if (page >= pager->page_count || !pager->pages[page].pins) { errno = EINVAL; return -1; }
    pager->pages[page].pins--;
    return 0;
}

int swap_pager_close( struct swap_pager *pager )
{
    unsigned int i;
    int failed = pager->poisoned;
    for (i = 0; i < pager->page_count; i++)
        if (pager->pages[i].pins) { errno = EBUSY; return -1; }
    for (i = 0; i < pager->slot_count; i++)
    {
        int page = pager->owners[i];
        if (page < 0) continue;
        if (pager->ops.unmap( pager->context, page, i )) failed = 1;
        else { pager->pages[page].slot = -1; pager->owners[i] = -1; }
    }
    if (failed) { errno = EIO; return -1; }
    free( pager->pages );
    free( pager->owners );
    memset( pager, 0, sizeof(*pager) );
    return 0;
}
