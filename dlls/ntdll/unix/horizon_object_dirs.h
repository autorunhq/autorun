/*
 * The object-manager directories the Horizon server provides.
 *
 * wineserver keeps a whole object namespace. Programs reach two parts of it
 * directly: \?? (also \DosDevices), whose entries GetLogicalDrives and
 * QueryDosDevice list, and BaseNamedObjects, which kernelbase opens before it
 * creates a named object. The server answers for those two.
 */

#ifndef __WINE_HORIZON_OBJECT_DIRS_H
#define __WINE_HORIZON_OBJECT_DIRS_H

#include <string.h>

enum horizon_object_dir
{
    HORIZON_OBJECT_DIR_NONE,
    HORIZON_OBJECT_DIR_DOS_DEVICES,   /* \??, \DosDevices, \GLOBAL?? */
    HORIZON_OBJECT_DIR_NAMED_OBJECTS  /* \BaseNamedObjects, \Sessions\<n>\BaseNamedObjects */
};

#define HORIZON_OBJECT_DIR_STATUS_SUCCESS         0x00000000u
#define HORIZON_OBJECT_DIR_STATUS_MORE_ENTRIES    0x00000105u
#define HORIZON_OBJECT_DIR_STATUS_NO_MORE_ENTRIES 0x8000001au

/* Compares UTF-16LE name bytes with an ASCII path, ignoring ASCII case. A '#'
 * in the path stands for one or more decimal digits. */
static inline int horizon_object_dir_path_is( const unsigned char *name, unsigned int len, const char *path )
{
    unsigned int pos = 0;

    if (len % 2) return 0;
    for (; *path; path++)
    {
        unsigned int c, want = (unsigned char)*path;

        if (want == '#')
        {
            unsigned int start = pos;

            while (pos < len && !name[pos + 1] && name[pos] >= '0' && name[pos] <= '9') pos += 2;
            if (pos == start) return 0;
            continue;
        }
        if (pos >= len) return 0;
        c = name[pos] | name[pos + 1] << 8;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (want >= 'a' && want <= 'z') want -= 'a' - 'A';
        if (c != want) return 0;
        pos += 2;
    }
    return pos == len;
}

static inline int horizon_object_dir_from_path( const unsigned char *name, unsigned int len )
{
    if (horizon_object_dir_path_is( name, len, "\\??" ) ||
        horizon_object_dir_path_is( name, len, "\\DosDevices" ) ||
        horizon_object_dir_path_is( name, len, "\\GLOBAL??" ))
        return HORIZON_OBJECT_DIR_DOS_DEVICES;
    if (horizon_object_dir_path_is( name, len, "\\BaseNamedObjects" ) ||
        horizon_object_dir_path_is( name, len, "\\Sessions\\#\\BaseNamedObjects" ))
        return HORIZON_OBJECT_DIR_NAMED_OBJECTS;
    return HORIZON_OBJECT_DIR_NONE;
}

struct horizon_object_dir_entry
{
    const char *name;
    const char *type;
};

static const struct horizon_object_dir_entry horizon_object_dir_dos_devices[] =
{
    { "C:", "SymbolicLink" },
    { "D:", "SymbolicLink" },
    { "E:", "SymbolicLink" },
    { "F:", "SymbolicLink" },
    { "G:", "SymbolicLink" },
    { "H:", "SymbolicLink" },
    { "Z:", "SymbolicLink" },
};

/* Writes get_directory_entries records - struct directory_entry (name_len,
 * type_len), then the UTF-16 name and type, padded to 4 bytes - starting at
 * index, at most max_count of them in at most reply_max bytes, with
 * wineserver's statuses: SUCCESS when some fit, MORE_ENTRIES when the next one
 * does not, NO_MORE_ENTRIES when there is none to send. total_len counts the
 * name and type bytes of every entry looked at, sent or not. */
static inline unsigned int horizon_object_dir_entries( int dir, unsigned int drive_mask,
                                                       unsigned int index, unsigned int max_count,
                                                       unsigned int reply_max, unsigned char *out,
                                                       unsigned int *out_size, unsigned int *count,
                                                       unsigned int *total_len )
{
    const struct horizon_object_dir_entry *entries = NULL;
    unsigned int entry_count = 0, status = HORIZON_OBJECT_DIR_STATUS_NO_MORE_ENTRIES, size = 0, i, k, ordinal = 0;

    if (dir == HORIZON_OBJECT_DIR_DOS_DEVICES)
    {
        entries = horizon_object_dir_dos_devices;
        entry_count = sizeof(horizon_object_dir_dos_devices) / sizeof(horizon_object_dir_dos_devices[0]);
    }
    *count = *total_len = 0;
    for (i = 0; i < entry_count && *count < max_count; i++)
    {
        const struct horizon_object_dir_entry *entry = &entries[i];
        unsigned int name_len = 2 * (unsigned int)strlen( entry->name );
        unsigned int type_len = 2 * (unsigned int)strlen( entry->type );
        unsigned int entry_size = (8 + name_len + type_len + 3) & ~3u;

        if (!(drive_mask & (1u << (entry->name[0] - 'A'))) || ordinal++ < index) continue;
        *total_len += name_len + type_len;
        if (size + entry_size > reply_max)
        {
            status = HORIZON_OBJECT_DIR_STATUS_MORE_ENTRIES;
            break;
        }
        memset( out + size, 0, entry_size );
        memcpy( out + size, &name_len, 4 );
        memcpy( out + size + 4, &type_len, 4 );
        for (k = 0; entry->name[k]; k++) out[size + 8 + 2 * k] = (unsigned char)entry->name[k];
        for (k = 0; entry->type[k]; k++) out[size + 8 + name_len + 2 * k] = (unsigned char)entry->type[k];
        size += entry_size;
        (*count)++;
        status = HORIZON_OBJECT_DIR_STATUS_SUCCESS;
    }
    *out_size = size;
    return status;
}

#endif /* __WINE_HORIZON_OBJECT_DIRS_H */
