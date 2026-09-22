#ifndef WINE_NX_FEX_JIT_H
#define WINE_NX_FEX_JIT_H

#include <stddef.h>

int wine_nx_fex_jit_create( size_t size, void **rx, void **rw );
int wine_nx_fex_jit_close( void *rx );
int wine_nx_fex_jit_flush( const void *rx, size_t size );
size_t wine_nx_fex_jit_size( const void *rx );
size_t wine_nx_fex_jit_release(void);

#endif
