/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * Kernel code memory placed by this runtime instead of by libnx: the dynarec's
 * arenas (wine-nx-probe/source/wow64_box64_dynarec.c) mapped into the window
 * horizon.c keeps for the runtime's own mappings.
 *
 * jitCreate asks libnx for the two addresses, and libnx probes the address
 * space at random. A program linked for 0x400000 has to run in a 32-bit
 * address space, where that window is the only place code memory may go, and
 * once it held a couple of arenas the probe stopped finding room: The Sims 2
 * was refused 128, 64, 32 and 16 MB one after another, took 8, and ran out of
 * translated code at 133 MB while the window was still half empty.
 */
#ifndef WINE_NX_HORIZON_CODE_MEMORY_H
#define WINE_NX_HORIZON_CODE_MEMORY_H

#include <stddef.h>

struct wine_nx_code_memory
{
    unsigned int handle;   /* the kernel code memory object, 0 when unmapped */
    void *rw;              /* the alias the dynarec emits through */
    void *rx;              /* the alias translated code runs from */
    void *rw_token;        /* what keeps libnx from placing anything there */
    void *rx_token;
    size_t size;
};

/* Creates code memory over source, which must be page aligned heap memory the
 * caller owns, and maps both aliases. Returns 0 with the result that refused
 * it in *rc (0 when the window had no run that large), leaving source as it
 * was. */
extern int wine_nx_code_memory_map( void *source, size_t size, struct wine_nx_code_memory *out,
                                    unsigned int *rc ) __attribute__((weak));

/* Unmaps both aliases and closes the object. The source memory is the
 * caller's to free afterwards. */
extern void wine_nx_code_memory_unmap( struct wine_nx_code_memory *memory ) __attribute__((weak));

/* Megabytes of the window that nothing has taken, for the log. */
extern size_t wine_nx_native_window_free( void ) __attribute__((weak));

#endif
