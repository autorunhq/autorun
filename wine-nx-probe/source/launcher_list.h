/*
 * The program list of the runtime's launcher: which files count as Windows
 * programs, their order, which one to start on, whether args.txt belongs to
 * the chosen program, and which rows are visible.
 */
#ifndef WINE_NX_LAUNCHER_LIST_H
#define WINE_NX_LAUNCHER_LIST_H

#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static inline const char *launcher_machine_name( unsigned short machine )
{
    switch (machine)
    {
    case 0x014c: return "x86";
    case 0x8664: return "x64";
    case 0xaa64: return "ARM64";
    default: return "Unknown";
    }
}
#define LAUNCHER_MAX_ENTRIES 256
#define LAUNCHER_DRIVE_C "sdmc:/switch/wine/drive_c"
/* USB volumes ums0: to ums4: are D: to H:; programs are looked for in their Wine folder. */
#define LAUNCHER_USB_DRIVES 5
#define LAUNCHER_USB_PROGRAMS "Wine"

struct launcher_entry
{
    char path[512];          /* sdmc:/switch/wine/drive_c/... or ums0:/Wine/... */
    char dos[256];           /* C:\... */
    unsigned short machine;  /* IMAGE_FILE_MACHINE_* */
};

/* The DOS path of a file, as Wine-NX's drives map it (dlls/ntdll/unix/file.c): C: is
 * drive_c, Z: the card's root and D: to H: the USB volumes. Returns 0 for a path elsewhere
 * or too long. */
static inline int launcher_dos_path( const char *path, char *out, size_t size )
{
    size_t drive_c_len = strlen( LAUNCHER_DRIVE_C ), len;
    const char *rest;
    char letter;
    int n;

    if (!strncasecmp( path, LAUNCHER_DRIVE_C, drive_c_len ) && (!path[drive_c_len] || path[drive_c_len] == '/'))
    {
        letter = 'C';
        rest = path + drive_c_len;
    }
    else if (!strncmp( path, "sdmc:/", 6 ) || !strcmp( path, "sdmc:" ))
    {
        letter = 'Z';
        rest = path + 5;
    }
    else if (!strncmp( path, "ums", 3 ) && path[3] >= '0' && path[3] < '0' + LAUNCHER_USB_DRIVES &&
             path[4] == ':' && (!path[5] || path[5] == '/'))
    {
        letter = 'D' + path[3] - '0';
        rest = path + 5;
    }
    else return 0;
    while (*rest == '/') rest++;
    n = snprintf( out, size, "%c:\\%s", letter, rest );
    if (n <= 0 || (size_t)n >= size) return 0;
    for (len = 3; out[len]; len++) if (out[len] == '/') out[len] = '\\';
    while (len > 3 && out[len - 1] == '\\') out[--len] = 0;
    return 1;
}

static inline int launcher_is_exe( const char *name )
{
    size_t len = strlen( name );

    return name[0] != '.' && len > 4 && !strcasecmp( name + len - 4, ".exe" );
}

static inline int launcher_compare( const void *a, const void *b )
{
    const struct launcher_entry *x = a, *y = b;

    return strcasecmp( x->dos, y->dos );
}

/* args.txt holds a whole command line. It belongs to the program when its
 * first word, quoted or not, names that program. */
static inline int launcher_args_match( const char *args, const char *dos )
{
    size_t len = strlen( dos );

    while (*args == ' ' || *args == '\t') args++;
    if (*args == '"')
    {
        args++;
        return !strncasecmp( args, dos, len ) && args[len] == '"';
    }
    return !strncasecmp( args, dos, len ) && (!args[len] || args[len] == ' ' || args[len] == '\t');
}

/* A program's own file lives next to it, named after it with suffix in place of
 * .exe. Returns 0 when the path does not end in .exe or does not fit. */
static inline int launcher_sibling_path( const char *exe_path, const char *suffix, char *out, size_t size )
{
    size_t len = strlen( exe_path ), suffix_size = strlen( suffix ) + 1;

    if (len < 4 || strcasecmp( exe_path + len - 4, ".exe" ) || len - 4 + suffix_size > size) return 0;
    memcpy( out, exe_path, len - 4 );
    memcpy( out + len - 4, suffix, suffix_size );
    return 1;
}

/* A program's own arguments: openttd.exe reads openttd.args.txt. */
static inline int launcher_args_path( const char *exe_path, char *out, size_t size )
{
    return launcher_sibling_path( exe_path, ".args.txt", out, size );
}

/* A program's own controls, applied over keys.txt: SPEED2.EXE reads SPEED2.keys.txt. */
static inline int launcher_keys_path( const char *exe_path, char *out, size_t size )
{
    return launcher_sibling_path( exe_path, ".keys.txt", out, size );
}

/* The command line for a program with its own arguments; a path with spaces is quoted. */
static inline int launcher_command_line( const char *dos, const char *args, char *out, size_t size )
{
    const char *quote = strchr( dos, ' ' ) ? "\"" : "";
    int len = snprintf( out, size, "%s%s%s %s", quote, dos, quote, args );

    return len > 0 && (size_t)len < size;
}

/* The entry named by target.txt (a path or a C:\ path), or the first. */
static inline int launcher_find( const struct launcher_entry *entries, int count, const char *target )
{
    int i;

    for (i = 0; i < count; i++)
        if (!strcasecmp( entries[i].path, target ) || !strcasecmp( entries[i].dos, target )) return i;
    return 0;
}

/* Horizontal row edges turn the page, keeping the same row when it exists. */
static inline int launcher_grid_move( int selection, int count, int columns, int rows, int dx, int dy )
{
    int next, page_size, first, row;

    if (count <= 0 || columns <= 0 || rows <= 0) return 0;
    page_size = columns * rows;
    first = selection / page_size * page_size;
    row = (selection - first) / columns;
    if (dx > 0 && (selection % columns == columns - 1 || selection == count - 1))
    {
        if (first + page_size >= count) return selection;
        next = first + page_size + row * columns;
        return next < count ? next : count - 1;
    }
    if (dx < 0 && selection % columns == 0)
        return first ? first - page_size + row * columns + columns - 1 : selection;
    next = selection + dx + dy * columns;
    /* Down from the last full row lands on the last program rather than nowhere. */
    if (dy > 0 && next >= count && selection + columns - selection % columns < count) next = count - 1;
    if (next < 0 || next >= count) return selection;
    return next;
}

/* Page shortcuts preserve the slot, clamped to the last available program. */
static inline int launcher_grid_page( int selection, int count, int columns, int rows, int direction )
{
    int next, page_size, first;

    if (count <= 0 || columns <= 0 || rows <= 0) return 0;
    page_size = columns * rows;
    first = (selection / page_size + direction) * page_size;
    if (first < 0 || first >= count) return selection;
    next = first + selection % page_size;
    return next < count ? next : count - 1;
}

/* The first visible row, moved only as far as needed to show "selected". */
static inline int launcher_first_visible( int first, int selected, int count, int rows )
{
    if (selected < first) first = selected;
    if (selected >= first + rows) first = selected - rows + 1;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    return first;
}

#endif
