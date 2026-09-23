#ifndef WINE_NX_SWAP_POC_H
#define WINE_NX_SWAP_POC_H

#include "swap_store.h"

int wine_nx_swap_test( const char *directory, unsigned int megabytes, swap_progress progress,
                       void *context, char *result, size_t result_size );
int wine_nx_swap_fault( uint64_t address, uint32_t esr );

#endif
