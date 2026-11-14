#ifndef WINE_NX_RUNTIME_SYMBOLS_H
#define WINE_NX_RUNTIME_SYMBOLS_H

#include <stddef.h>
#include <stdint.h>

/* Opens the runtime's symbol file (tools/runtime_symbols.py) for the code
 * loaded at base; the number of functions it names, or 0. */
int wine_nx_runtime_symbols_open( const char *path, uintptr_t base );
/* "function+0xoffset" for an address in the runtime's code and nonzero, or
 * "runtime+0xoffset" and 0 when no name is known, or "" when the address is
 * not the runtime's. */
int wine_nx_runtime_symbol( uintptr_t address, char *out, size_t size );

#endif
