/*
 * Names an address in the runtime's own code, for its log: a fault, a stall
 * report. The names come from the symbol file the packager writes beside the
 * NRO (tools/runtime_symbols.py), read by binary search straight from the card
 * with a descriptor opened at start: a fault handler cannot count on the heap,
 * and a lookup that finds the file busy gives the bare offset instead.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "runtime_symbols.h"

#define SYMBOLS_MAGIC "ARSYM1\0\0"

static int symbols_fd = -1;
static uint32_t symbols_count, symbols_names;
static uintptr_t symbols_base;
static pthread_mutex_t symbols_mutex = PTHREAD_MUTEX_INITIALIZER;

static int read_at( int fd, off_t offset, void *buffer, size_t size )
{
    return lseek( fd, offset, SEEK_SET ) == offset && read( fd, buffer, size ) == (ssize_t)size;
}

int wine_nx_runtime_symbols_open( const char *path, uintptr_t base )
{
    unsigned char header[16];
    int fd;

    if (symbols_fd >= 0 || (fd = open( path, O_RDONLY )) < 0) return 0;
    if (!read_at( fd, 0, header, sizeof(header) ) || memcmp( header, SYMBOLS_MAGIC, 8 ))
    {
        close( fd );
        return 0;
    }
    memcpy( &symbols_count, header + 8, 4 );
    memcpy( &symbols_names, header + 12, 4 );
    symbols_base = base;
    symbols_fd = fd;
    return symbols_count;
}

/* The function at or below offset, as its record. */
static int find( uint32_t offset, uint32_t record[3] )
{
    uint32_t low = 0, high = symbols_count;

    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2, entry[3];

        if (!read_at( symbols_fd, 16 + (off_t)middle * 12, entry, sizeof(entry) )) return 0;
        if (entry[0] <= offset) low = middle + 1;
        else high = middle;
    }
    if (!low) return 0;
    return read_at( symbols_fd, 16 + (off_t)(low - 1) * 12, record, 12 );
}

int wine_nx_runtime_symbol( uintptr_t address, char *out, size_t size )
{
    uint32_t record[3], offset;
    char name[128];
    ssize_t got;
    int found = 0;

    if (!size) return 0;
    out[0] = 0;
    if (!symbols_base || address < symbols_base || address - symbols_base >= 0xffffffffu) return 0;
    offset = (uint32_t)(address - symbols_base);
    if (symbols_fd >= 0 && !pthread_mutex_trylock( &symbols_mutex ))
    {
        /* A function's size bounds it; one without a size is believed for
         * 64 KB, beyond which the address is more likely padding or a stub. */
        if (find( offset, record ) && (record[1] ? offset - record[0] < record[1] : offset - record[0] < 0x10000) &&
            record[2] < symbols_names &&
            lseek( symbols_fd, 16 + (off_t)symbols_count * 12 + record[2], SEEK_SET ) >= 0 &&
            (got = read( symbols_fd, name, sizeof(name) - 1 )) > 0)
        {
            name[got] = 0;
            snprintf( out, size, "%s+%#x", name, offset - record[0] );
            found = 1;
        }
        pthread_mutex_unlock( &symbols_mutex );
    }
    if (!found) snprintf( out, size, "runtime+%#x", offset );
    return found;
}
