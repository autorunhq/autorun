#ifndef WINE_NX_FEX_UNIXLIB_H
#define WINE_NX_FEX_UNIXLIB_H

#include <stdint.h>

#define WINE_NX_FEX_ABI_VERSION 3
#define WINE_NX_FEX_QUERY 7
#define WINE_NX_FEX_ALLOC 8
#define WINE_NX_FEX_FREE 9
#define WINE_NX_FEX_FLUSH 10
#define WINE_NX_FEX_ATTACH_THREAD 11
#define WINE_NX_FEX_HEAP_ALLOC 12
#define WINE_NX_FEX_HEAP_REALLOC 13
#define WINE_NX_FEX_HEAP_FREE 14
#define WINE_NX_FEX_PROTECT_CODE 15

struct wine_nx_fex_query { uint32_t version, size; };
struct wine_nx_fex_memory { uint64_t size, rx, rw; };
struct wine_nx_fex_heap { uint64_t size, pointer, zero; };
struct wine_nx_fex_protection { uint64_t address, enable; };

#endif
