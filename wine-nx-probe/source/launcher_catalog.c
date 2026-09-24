#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "launcher_catalog.h"

static void trim( char *text )
{
    char *start = text, *end;
    while (*start == ' ' || *start == '\t') start++;
    if (start != text) memmove( text, start, strlen( start ) + 1 );
    end = text + strlen( text );
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = 0;
}

static int copy_value( char *out, size_t size, const char *value )
{
    size_t i = 0;
    while (*value)
    {
        unsigned char c = *value++;
        if (c == '%')
        {
            unsigned int v;
            if (sscanf( value, "%2x", &v ) != 1) return 0;
            c = v;
            value += 2;
        }
        if (!c || c == '\r' || c == '\n' || i + 1 >= size) return 0;
        out[i++] = c;
    }
    out[i] = 0;
    return 1;
}

static int write_value( FILE *file, const char *key, const char *value )
{
    const unsigned char *p = (const unsigned char *)value;
    if (fprintf( file, "%s=", key ) < 0) return 0;
    while (*p)
    {
        if (*p == '%' || *p == '\r' || *p == '\n' || *p == '=' || *p == '[' || *p == ']')
        {
            if (fprintf( file, "%%%02X", *p++ ) < 0) return 0;
        }
        else if (fputc( *p++, file ) == EOF) return 0;
    }
    return fputc( '\n', file ) != EOF;
}

void launcher_catalog_init( struct launcher_catalog *catalog )
{
    memset( catalog, 0, sizeof(*catalog) );
    catalog->next_id = catalog->next_order = 1;
}

/* Little-endian fields of a PE header, read by hand: the launcher has no Windows
 * headers, and only four of them are wanted. */
static unsigned int pe_u16( const unsigned char *p ) { return p[0] | (p[1] << 8); }
static unsigned long long pe_u32( const unsigned char *p )
{
    return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) |
           ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24);
}

enum launcher_address_space launcher_program_address_space( const char *path )
{
    /* Enough for the DOS stub's one useful field and the whole PE header. */
    unsigned char head[0x400];
    const unsigned char *pe, *optional, *directories;
    unsigned long long lfanew, image_base;
    unsigned int magic, characteristics, dll_characteristics, directory_count, relocations;
    size_t read;
    FILE *file = fopen( path, "rb" );

    if (!file) return LAUNCHER_ADDRESS_UNKNOWN;
    read = fread( head, 1, sizeof(head), file );
    fclose( file );
    if (read < 0x40 || head[0] != 'M' || head[1] != 'Z') return LAUNCHER_ADDRESS_UNKNOWN;
    lfanew = pe_u32( head + 0x3c );
    /* The COFF header is 20 bytes and the smallest optional header 96. */
    if (lfanew + 4 + 20 + 96 > read) return LAUNCHER_ADDRESS_UNKNOWN;
    pe = head + lfanew;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] || pe[3]) return LAUNCHER_ADDRESS_UNKNOWN;
    characteristics = pe_u16( pe + 4 + 18 );
    optional = pe + 4 + 20;
    magic = pe_u16( optional );
    if (magic == 0x10b)        /* PE32 */
    {
        image_base = pe_u32( optional + 28 );
        directory_count = (unsigned int)pe_u32( optional + 92 );
        directories = optional + 96;
    }
    else if (magic == 0x20b)   /* PE32+ */
    {
        image_base = pe_u32( optional + 24 ) | (pe_u32( optional + 28 ) << 32);
        directory_count = (unsigned int)pe_u32( optional + 108 );
        directories = optional + 112;
    }
    else return LAUNCHER_ADDRESS_UNKNOWN;
    dll_characteristics = pe_u16( optional + 70 );
    /* Directory 5 is the base relocations. */
    if (directories + 6 * 8 > head + read) return LAUNCHER_ADDRESS_UNKNOWN;
    relocations = directory_count > 5 ? (unsigned int)pe_u32( directories + 5 * 8 + 4 ) : 0;

    /* It can be moved if it says where to, or if it allows being moved at all.
     * IMAGE_FILE_RELOCS_STRIPPED is 0x0001, IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE
     * 0x0040. An image above 4 GB is out of a 32-bit address space's reach and
     * cannot want one. */
    if (relocations && !(characteristics & 0x0001)) return LAUNCHER_ADDRESS_ANY;
    if (dll_characteristics & 0x0040) return LAUNCHER_ADDRESS_ANY;
    if (image_base >= 0x100000000ull) return LAUNCHER_ADDRESS_ANY;
    return LAUNCHER_ADDRESS_LOW;
}

int launcher_catalog_find( const struct launcher_catalog *catalog, const char *path )
{
    int i;
    for (i = 0; i < catalog->count; i++)
        if (!strcasecmp( catalog->entries[i].path, path )) return i;
    return -1;
}

int launcher_catalog_add( struct launcher_catalog *catalog, const char *path, const char *title )
{
    struct launcher_catalog_entry *entry;
    int found = launcher_catalog_find( catalog, path );
    if (found >= 0) return found;
    if (!path[0] || strpbrk( path, "\r\n" ) || strlen( path ) >= sizeof(catalog->entries[0].path) ||
        catalog->count >= LAUNCHER_MAX_ENTRIES) return -1;
    entry = &catalog->entries[catalog->count];
    memset( entry, 0, sizeof(*entry) );
    entry->id = catalog->next_id++;
    entry->added_order = catalog->next_order++;
    snprintf( entry->path, sizeof(entry->path), "%s", path );
    snprintf( entry->title, sizeof(entry->title), "%s", title ? title : "" );
    return catalog->count++;
}

void launcher_catalog_remove( struct launcher_catalog *catalog, int index )
{
    if (index < 0 || index >= catalog->count) return;
    memmove( &catalog->entries[index], &catalog->entries[index + 1],
             (catalog->count - index - 1) * sizeof(catalog->entries[0]) );
    catalog->count--;
}

enum launcher_catalog_result launcher_catalog_load( struct launcher_catalog *catalog, const char *path )
{
    char line[1200];
    struct launcher_catalog parsed;
    struct launcher_catalog_entry *entry = NULL;
    FILE *file = fopen( path, "r" );
    int version = 0, invalid = 0;

    if (!file) return errno == ENOENT ? LAUNCHER_CATALOG_MISSING : LAUNCHER_CATALOG_IO_ERROR;
    launcher_catalog_init( &parsed );
    while (fgets( line, sizeof(line), file ))
    {
        char *equals, *key, *value;
        unsigned int n;
        if (!strchr( line, '\n' ) && !feof( file )) { invalid = 1; break; }
        trim( line );
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[')
        {
            char tail;
            if (sscanf( line, "[game %u]%c", &n, &tail ) != 1 || !n || parsed.count >= LAUNCHER_MAX_ENTRIES)
            { invalid = 1; break; }
            entry = &parsed.entries[parsed.count++];
            memset( entry, 0, sizeof(*entry) );
            entry->id = n;
            if (n >= parsed.next_id) parsed.next_id = n + 1;
            continue;
        }
        if (!(equals = strchr( line, '=' ))) { invalid = 1; break; }
        *equals = 0; key = line; value = equals + 1; trim( key ); trim( value );
        if (!strcmp( key, "version" ) && !entry) version = atoi( value );
        else if (!strcmp( key, "next-id" ) && !entry)
        {
            char *end;
            unsigned long id;
            errno = 0;
            id = strtoul( value, &end, 10 );
            if (errno || *end || !id || id >= UINT_MAX) { invalid = 1; break; }
            parsed.next_id = id;
        }
        else if (!entry) { invalid = 1; break; }
        else if (!strcmp( key, "path" )) invalid |= !copy_value( entry->path, sizeof(entry->path), value );
        else if (!strcmp( key, "title" )) invalid |= !copy_value( entry->title, sizeof(entry->title), value );
        else if (!strcmp( key, "square-art" )) invalid |= !copy_value( entry->square_art, sizeof(entry->square_art), value );
        else if (!strcmp( key, "portrait-art" ) || !strcmp( key, "landscape-art" ))
            invalid |= !copy_value( entry->portrait_art, sizeof(entry->portrait_art), value );
        else if (!strcmp( key, "hero-art" )) invalid |= !copy_value( entry->hero_art, sizeof(entry->hero_art), value );
        else if (!strcmp( key, "favorite" )) entry->favorite = atoi( value ) == 1;
        else if (!strcmp( key, "added-order" )) entry->added_order = strtoul( value, NULL, 10 );
        else if (!strcmp( key, "launched-order" )) entry->launched_order = strtoul( value, NULL, 10 );
    }
    if (ferror( file )) invalid = 1;
    fclose( file );
    if (version != 2 && version != LAUNCHER_CATALOG_VERSION) invalid = 1;
    for (int i = 0; !invalid && i < parsed.count; i++)
    {
        if (!parsed.entries[i].path[0]) invalid = 1;
        if (!parsed.entries[i].added_order) parsed.entries[i].added_order = parsed.next_order++;
        if (parsed.entries[i].added_order >= parsed.next_order) parsed.next_order = parsed.entries[i].added_order + 1;
        if (parsed.entries[i].launched_order >= parsed.next_order) parsed.next_order = parsed.entries[i].launched_order + 1;
        for (int j = 0; j < i; j++)
            if (parsed.entries[j].id == parsed.entries[i].id ||
                !strcasecmp( parsed.entries[j].path, parsed.entries[i].path )) invalid = 1;
    }
    if (invalid) return LAUNCHER_CATALOG_INVALID;
    *catalog = parsed;
    return LAUNCHER_CATALOG_OK;
}

int launcher_catalog_save( const struct launcher_catalog *catalog, const char *path )
{
    char temp[600], backup[600];
    FILE *file;
    int i, ok = 1;
    if ((size_t)snprintf( temp, sizeof(temp), "%s.tmp", path ) >= sizeof(temp) ||
        (size_t)snprintf( backup, sizeof(backup), "%s.bak", path ) >= sizeof(backup)) return 0;
    if (!(file = fopen( temp, "w" ))) return 0;
    ok &= fprintf( file, "version=%d\nnext-id=%u\n", LAUNCHER_CATALOG_VERSION, catalog->next_id ) > 0;
    for (i = 0; ok && i < catalog->count; i++)
    {
        const struct launcher_catalog_entry *entry = &catalog->entries[i];
        ok &= fprintf( file, "\n[game %u]\n", entry->id ) > 0;
        ok &= write_value( file, "path", entry->path );
        ok &= write_value( file, "title", entry->title );
        ok &= fprintf( file, "added-order=%u\nlaunched-order=%u\nfavorite=%d\n",
                       entry->added_order, entry->launched_order, entry->favorite ) > 0;
        if (entry->square_art[0]) ok &= write_value( file, "square-art", entry->square_art );
        if (entry->portrait_art[0]) ok &= write_value( file, "portrait-art", entry->portrait_art );
        if (entry->hero_art[0]) ok &= write_value( file, "hero-art", entry->hero_art );
    }
    if (fflush( file )) ok = 0;
    if (fclose( file )) ok = 0;
    if (!ok) { remove( temp ); return 0; }
    remove( backup );
    rename( path, backup );
    if (rename( temp, path ))
    {
        rename( backup, path );
        remove( temp );
        return 0;
    }
    return 1;
}

int launcher_catalog_import_legacy( struct launcher_catalog *catalog, const char *path )
{
    char line[600];
    FILE *file = fopen( path, "r" );
    if (!file) return errno == ENOENT;
    while (fgets( line, sizeof(line), file ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        trim( line );
        if (line[0] && launcher_catalog_add( catalog, line, "" ) < 0) { fclose( file ); return 0; }
    }
    return !ferror( file ) && !fclose( file );
}
