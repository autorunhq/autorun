#ifndef WINE_NX_LAUNCHER_CATALOG_H
#define WINE_NX_LAUNCHER_CATALOG_H

#include <stddef.h>
#include "launcher_list.h"

#define LAUNCHER_CATALOG_VERSION 3
#define LAUNCHER_CATALOG_FILE "launcher-library-v2.ini"

struct launcher_catalog_entry
{
    unsigned int id;
    unsigned int added_order;
    unsigned int launched_order;
    int favorite;
    char path[512];
    char title[128];
    char square_art[512];
    char portrait_art[512];
    char hero_art[512];
};

struct launcher_catalog
{
    struct launcher_catalog_entry entries[LAUNCHER_MAX_ENTRIES];
    int count;
    unsigned int next_id;
    unsigned int next_order;
};

/* What a program needs of the address space it runs in. Horizon fixes that when
 * it makes the process, from the title that started it, so a forwarder decides
 * it for everything it opens and nothing can change it afterwards. A program
 * with no relocations is linked for one address and no other, which only the
 * low 4 GB of a 32-bit address space has. */
enum launcher_address_space
{
    LAUNCHER_ADDRESS_ANY,   /* runs wherever it is put */
    LAUNCHER_ADDRESS_LOW,   /* requires the verified low-address window */
    LAUNCHER_ADDRESS_UNKNOWN  /* the program could not be read */
};

/* Reads the program's own header and says which it is. */
enum launcher_address_space launcher_program_address_space( const char *path );

enum launcher_catalog_result
{
    LAUNCHER_CATALOG_OK,
    LAUNCHER_CATALOG_MISSING,
    LAUNCHER_CATALOG_INVALID,
    LAUNCHER_CATALOG_IO_ERROR
};

void launcher_catalog_init( struct launcher_catalog *catalog );
enum launcher_catalog_result launcher_catalog_load( struct launcher_catalog *catalog, const char *path );
int launcher_catalog_save( const struct launcher_catalog *catalog, const char *path );
int launcher_catalog_find( const struct launcher_catalog *catalog, const char *path );
int launcher_catalog_add( struct launcher_catalog *catalog, const char *path, const char *title );
void launcher_catalog_remove( struct launcher_catalog *catalog, int index );
int launcher_catalog_import_legacy( struct launcher_catalog *catalog, const char *path );

#endif
