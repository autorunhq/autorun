/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Shortcuts and batch files in the launcher's file browser. The library holds
 * programs, one entry for each .exe, with its settings and arguments in files
 * beside it; a .lnk or a .bat/.cmd chosen in the browser is therefore read for
 * the program it starts and the arguments it gives it, and that program is what
 * gets added.
 *
 * A shortcut made on a PC names its target by a path of that PC
 * (C:\Games\Foo\bin\foo.exe). The target is looked for by the shortcut's
 * relative path, then where Wine-NX's drives put that path, then by the end of
 * the path under the shortcut's folder and the three above it.
 *
 * Of a batch file, the first line that starts a program counts: `start`, with
 * its title and switches, `call`, or the program's name itself; `cd` and
 * `pushd` before it say where it is. %~dp0 is the batch file's folder. A line
 * that needs another variable is passed over.
 */
#ifndef LAUNCHER_SHORTCUT_H
#define LAUNCHER_SHORTCUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define LAUNCHER_SHORTCUT_TEXT 512

struct launcher_shortcut
{
    char target[LAUNCHER_SHORTCUT_TEXT];    /* the program, as the file names it: a Windows path, absolute or not */
    char relative[LAUNCHER_SHORTCUT_TEXT];  /* a .lnk's path from its own folder to the target, or empty */
    char workdir[LAUNCHER_SHORTCUT_TEXT];   /* where it is started, or empty */
    char args[LAUNCHER_SHORTCUT_TEXT];      /* its arguments, or empty */
};

static inline int launcher_shortcut_has_suffix( const char *name, const char *suffix )
{
    size_t len = strlen( name ), suffix_len = strlen( suffix );

    return name[0] != '.' && len > suffix_len && !strcasecmp( name + len - suffix_len, suffix );
}

static inline int launcher_is_shortcut( const char *name )
{
    return launcher_shortcut_has_suffix( name, ".lnk" );
}

static inline int launcher_is_batch( const char *name )
{
    return launcher_shortcut_has_suffix( name, ".bat" ) || launcher_shortcut_has_suffix( name, ".cmd" );
}

/***********************************************************************
 * .lnk ([MS-SHLLINK])
 */

static inline uint32_t launcher_lnk_u32( const unsigned char *p )
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void launcher_shortcut_put_utf8( char *out, size_t size, size_t *used, unsigned int ch )
{
    if (ch < 0x80)
    {
        if (*used + 1 < size) out[(*used)++] = (char)ch;
    }
    else if (ch < 0x800)
    {
        if (*used + 2 < size)
        {
            out[(*used)++] = (char)(0xc0 | (ch >> 6));
            out[(*used)++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    else if (*used + 3 < size)
    {
        out[(*used)++] = (char)(0xe0 | (ch >> 12));
        out[(*used)++] = (char)(0x80 | ((ch >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (ch & 0x3f));
    }
}

/* One StringData item at *offset, into out; skipped when out is NULL. */
static inline int launcher_lnk_string( const unsigned char *data, size_t len, size_t *offset, int unicode,
                                       char *out, size_t size )
{
    size_t count, bytes, i, used = 0;

    if (*offset + 2 > len) return 0;
    count = data[*offset] | (data[*offset + 1] << 8);
    bytes = unicode ? count * 2 : count;
    *offset += 2;
    if (*offset + bytes > len) return 0;
    if (out && size)
    {
        for (i = 0; i < count; i++)
        {
            unsigned int ch = unicode ? data[*offset + 2 * i] | (data[*offset + 2 * i + 1] << 8) : data[*offset + i];

            if (!ch) break;
            if (ch >= 0xd800 && ch < 0xe000) ch = '?';  /* outside the BMP: not in a path the card can hold anyway */
            launcher_shortcut_put_utf8( out, size, &used, ch );
        }
        out[used] = 0;
    }
    *offset += bytes;
    return 1;
}

static inline void launcher_lnk_ansi( const unsigned char *data, size_t len, size_t offset, char *out, size_t size,
                                      size_t *used )
{
    while (offset < len && data[offset])
        launcher_shortcut_put_utf8( out, size, used, data[offset++] );
    out[*used] = 0;
}

static inline void launcher_lnk_wide( const unsigned char *data, size_t len, size_t offset, char *out, size_t size,
                                      size_t *used )
{
    while (offset + 1 < len && (data[offset] || data[offset + 1]))
    {
        unsigned int ch = data[offset] | (data[offset + 1] << 8);

        if (ch >= 0xd800 && ch < 0xe000) ch = '?';
        launcher_shortcut_put_utf8( out, size, used, ch );
        offset += 2;
    }
    out[*used] = 0;
}

/* Returns 0 for a file that is not a shortcut or names no file. */
static inline int launcher_lnk_parse( const void *file, size_t len, struct launcher_shortcut *out )
{
    static const unsigned char clsid[16] = { 0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
    const unsigned char *data = file;
    uint32_t flags;
    size_t offset = 0x4c;
    int unicode;

    memset( out, 0, sizeof(*out) );
    if (len < 0x4c || launcher_lnk_u32( data ) != 0x4c || memcmp( data + 4, clsid, sizeof(clsid) )) return 0;
    flags = launcher_lnk_u32( data + 0x14 );
    unicode = !!(flags & 0x80);

    if (flags & 0x1)  /* HasLinkTargetIDList */
    {
        if (offset + 2 > len) return 0;
        offset += 2 + (data[offset] | (data[offset + 1] << 8));
        if (offset > len) return 0;
    }
    if (flags & 0x2)  /* HasLinkInfo */
    {
        uint32_t info_size, header_size, info_flags, base, suffix, base_wide = 0, suffix_wide = 0;
        const unsigned char *info = data + offset;
        size_t used = 0;

        if (offset + 0x1c > len) return 0;
        info_size = launcher_lnk_u32( info );
        header_size = launcher_lnk_u32( info + 4 );
        info_flags = launcher_lnk_u32( info + 8 );
        base = launcher_lnk_u32( info + 0x10 );
        suffix = launcher_lnk_u32( info + 0x18 );
        if (info_size < 0x1c || info_size > len - offset) return 0;
        if (header_size >= 0x24 && info_size >= 0x24)
        {
            base_wide = launcher_lnk_u32( info + 0x1c );
            suffix_wide = launcher_lnk_u32( info + 0x20 );
        }
        if (info_flags & 0x1)  /* VolumeIDAndLocalBasePath */
        {
            if (base_wide && base_wide < info_size)
                launcher_lnk_wide( info, info_size, base_wide, out->target, sizeof(out->target), &used );
            else if (base && base < info_size)
                launcher_lnk_ansi( info, info_size, base, out->target, sizeof(out->target), &used );
            if (suffix_wide && suffix_wide < info_size)
                launcher_lnk_wide( info, info_size, suffix_wide, out->target, sizeof(out->target), &used );
            else if (suffix && suffix < info_size)
                launcher_lnk_ansi( info, info_size, suffix, out->target, sizeof(out->target), &used );
        }
        offset += info_size;
    }
    if ((flags & 0x4) && !launcher_lnk_string( data, len, &offset, unicode, NULL, 0 )) return out->target[0] != 0;
    if ((flags & 0x8) &&
        !launcher_lnk_string( data, len, &offset, unicode, out->relative, sizeof(out->relative) ))
        return out->target[0] != 0;
    if ((flags & 0x10) &&
        !launcher_lnk_string( data, len, &offset, unicode, out->workdir, sizeof(out->workdir) ))
        return out->target[0] || out->relative[0];
    if (flags & 0x20) launcher_lnk_string( data, len, &offset, unicode, out->args, sizeof(out->args) );
    return out->target[0] || out->relative[0];
}

/***********************************************************************
 * .bat, .cmd
 */

static inline const char *launcher_bat_skip_space( const char *p )
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* The word at *p, quoted or not, without its quotes; *p moves past it. */
static inline int launcher_bat_word( const char **p, char *out, size_t size, int *quoted )
{
    const char *s = launcher_bat_skip_space( *p );
    size_t used = 0;
    int in_quotes = 0;

    if (quoted) *quoted = *s == '"';
    if (!*s) return 0;
    for (; *s && (in_quotes || (*s != ' ' && *s != '\t')); s++)
    {
        if (*s == '"') in_quotes = !in_quotes;
        else if (used + 1 < size) out[used++] = *s;
    }
    out[used] = 0;
    *p = s;
    return 1;
}

static inline int launcher_bat_is( const char *word, const char *command )
{
    return !strcasecmp( word, command );
}

/* %~dp0 and %cd% are the batch file's folder, which relative paths start from
 * already. Returns 0 when another variable is left. */
static inline int launcher_bat_expand( char *text )
{
    static const char *const own[] = { "%~dp0", "%cd%\\", "%cd%/", "%cd%", "%*" };
    size_t i;

    for (i = 0; i < sizeof(own) / sizeof(own[0]); i++)
    {
        size_t len = strlen( own[i] );
        char *at = text;

        while (*at)
        {
            if (!strncasecmp( at, own[i], len )) memmove( at, at + len, strlen( at + len ) + 1 );
            else at++;
        }
    }
    return !strchr( text, '%' );
}

static inline void launcher_bat_trim( char *text )
{
    size_t len = strlen( text );
    const char *start = launcher_bat_skip_space( text );

    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n'))
        text[--len] = 0;
    if (start != text) memmove( text, start, strlen( start ) + 1 );
}

/* text is the file, NUL-terminated. Returns 0 when no line starts a program. */
static inline int launcher_bat_parse( const char *text, struct launcher_shortcut *out )
{
    static const char *const passed_over[] =
    {
        "rem", "echo", "echo.", "set", "setlocal", "endlocal", "title", "pause", "cls", "color", "mode", "chcp",
        "timeout", "if", "goto", "exit", "popd", "for", "del", "copy", "xcopy", "move", "mkdir", "md", "rmdir", "rd",
        "reg", "regedit", "ping", "sleep", "taskkill", "attrib", "ren", "rename", "type", "ver", "path", "prompt",
        "shift", "verify", "break",
    };
    char line[1024], word[LAUNCHER_SHORTCUT_TEXT];
    const char *next = text;

    memset( out, 0, sizeof(*out) );
    if ((unsigned char)next[0] == 0xef && (unsigned char)next[1] == 0xbb && (unsigned char)next[2] == 0xbf) next += 3;
    while (*next)
    {
        const char *end = next + strcspn( next, "\r\n" ), *p;
        size_t len = (size_t)(end - next), i;
        int quoted, known = 0;

        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy( line, next, len );
        line[len] = 0;
        next = end + strspn( end, "\r\n" );

        p = launcher_bat_skip_space( line );
        while (*p == '@') p = launcher_bat_skip_space( p + 1 );
        if (!*p || *p == ':') continue;  /* labels and :: comments */
        if (!launcher_bat_word( &p, word, sizeof(word), &quoted )) continue;

        if (!quoted)
        {
            for (i = 0; i < sizeof(passed_over) / sizeof(passed_over[0]); i++)
                if (launcher_bat_is( word, passed_over[i] )) known = 1;
            if (known) continue;
            if (launcher_bat_is( word, "cd" ) || launcher_bat_is( word, "chdir" ) || launcher_bat_is( word, "pushd" ) ||
                !strncasecmp( word, "cd\\", 3 ) || !strncasecmp( word, "cd.", 3 ))
            {
                char dir[LAUNCHER_SHORTCUT_TEXT];

                if (!strncasecmp( word, "cd\\", 3 ) || !strncasecmp( word, "cd.", 3 ))
                    snprintf( dir, sizeof(dir), "%s%s", word + 2, p );
                else
                {
                    p = launcher_bat_skip_space( p );
                    if (!strncasecmp( p, "/d", 2 ) && (p[2] == ' ' || p[2] == '\t')) p += 2;
                    snprintf( dir, sizeof(dir), "%s", launcher_bat_skip_space( p ) );
                }
                launcher_bat_trim( dir );
                if (dir[0] == '"')
                {
                    const char *q = dir;
                    char unquoted[LAUNCHER_SHORTCUT_TEXT];

                    launcher_bat_word( &q, unquoted, sizeof(unquoted), NULL );
                    snprintf( dir, sizeof(dir), "%s", unquoted );
                }
                if (launcher_bat_expand( dir )) snprintf( out->workdir, sizeof(out->workdir), "%s", dir );
                continue;
            }
            if (launcher_bat_is( word, "call" ))
            {
                if (!launcher_bat_word( &p, word, sizeof(word), &quoted )) continue;
            }
            else if (launcher_bat_is( word, "start" ))
            {
                char workdir[LAUNCHER_SHORTCUT_TEXT] = "";
                int first = 1, found = 0;

                while (launcher_bat_word( &p, word, sizeof(word), &quoted ))
                {
                    if (first && quoted)  /* start's window title */
                    {
                        first = 0;
                        continue;
                    }
                    first = 0;
                    if (!quoted && word[0] == '/')
                    {
                        if (launcher_bat_is( word, "/d" )) launcher_bat_word( &p, workdir, sizeof(workdir), NULL );
                        else if (!strncasecmp( word, "/d", 2 ) && word[2])
                            snprintf( workdir, sizeof(workdir), "%s", word + 2 );
                        continue;
                    }
                    found = 1;
                    break;
                }
                if (!found) continue;
                if (workdir[0] && launcher_bat_expand( workdir ))
                    snprintf( out->workdir, sizeof(out->workdir), "%s", workdir );
            }
        }

        if (!launcher_bat_expand( word ) || !word[0]) continue;
        if (launcher_shortcut_has_suffix( word, ".bat" ) || launcher_shortcut_has_suffix( word, ".cmd" )) continue;
        if (!launcher_shortcut_has_suffix( word, ".exe" ))
        {
            const char *name = word + strlen( word );

            while (name > word && name[-1] != '\\' && name[-1] != '/') name--;
            if (strchr( name, '.' ) || strlen( word ) + 5 > sizeof(word)) continue;  /* a document, a .com, a .msi */
            strcat( word, ".exe" );
        }
        snprintf( out->target, sizeof(out->target), "%s", word );
        snprintf( out->args, sizeof(out->args), "%s", p );
        launcher_bat_trim( out->args );
        if (!launcher_bat_expand( out->args )) out->args[0] = 0;
        launcher_bat_trim( out->args );
        return 1;
    }
    return 0;
}

/***********************************************************************
 * Where the target is on the card
 */

typedef int (*launcher_shortcut_exists)( const char *path );

/* base, a folder on the card, with a relative path in either kind of slashes
 * joined on; . and .. are followed, never above the device's root. */
static inline int launcher_shortcut_join( const char *base, const char *relative, char *out, size_t size )
{
    const char *root_end = strchr( base, ':' );
    size_t root, used;

    root = root_end ? (size_t)(root_end - base) + 1 : 0;
    used = strlen( base );
    if (used + 1 >= size) return 0;
    memcpy( out, base, used + 1 );
    while (used > root + 1 && out[used - 1] == '/') out[--used] = 0;

    while (*relative)
    {
        size_t len = strcspn( relative, "\\/" );

        if (len == 2 && relative[0] == '.' && relative[1] == '.')
        {
            while (used > root && out[used - 1] != '/') used--;
            if (used > root + 1) used--;
            out[used] = 0;
        }
        else if (len && !(len == 1 && relative[0] == '.'))
        {
            int slash = !(used && out[used - 1] == '/');

            if (used + slash + len + 1 > size) return 0;
            if (slash) out[used++] = '/';
            memcpy( out + used, relative, len );
            used += len;
            out[used] = 0;
        }
        relative += len;
        while (*relative == '\\' || *relative == '/') relative++;
    }
    return 1;
}

static inline int launcher_shortcut_try( const char *path, launcher_shortcut_exists exists, char *out, size_t size )
{
    if (!launcher_shortcut_has_suffix( path, ".exe" ) || !exists( path ) || strlen( path ) >= size) return 0;
    strcpy( out, path );
    return 1;
}

/* The card's path of a Windows path, absolute or relative to folder. */
static inline int launcher_shortcut_locate( const char *folder, const char *windows, launcher_shortcut_exists exists,
                                            char *out, size_t size )
{
    char path[512], above[512];
    const char *rest, *parts[32];
    int count = 0, k, up;

    if (!windows[0]) return 0;
    if (!(((windows[0] | 0x20) >= 'a' && (windows[0] | 0x20) <= 'z') && windows[1] == ':'))
    {
        if (windows[0] == '\\' || windows[0] == '/') return 0;  /* a network path, or a drive's root unnamed */
        return launcher_shortcut_join( folder, windows, path, sizeof(path) ) &&
               launcher_shortcut_try( path, exists, out, size );
    }

    /* Where Wine-NX's drives put it (launcher_list.h: C: drive_c, Z: the card, D: to H: USB). */
    {
        char letter = (char)(windows[0] & ~0x20), root[40] = "";

        if (letter == 'C') snprintf( root, sizeof(root), "sdmc:/switch/wine/drive_c" );
        else if (letter == 'Z') snprintf( root, sizeof(root), "sdmc:/" );
        else if (letter >= 'D' && letter <= 'H') snprintf( root, sizeof(root), "ums%d:/", letter - 'D' );
        if (root[0] && launcher_shortcut_join( root, windows + 2, path, sizeof(path) ) &&
            launcher_shortcut_try( path, exists, out, size ))
            return 1;
    }

    /* The end of the path, longest first, under the folder and the three above it. */
    for (rest = windows + 2; *rest && count < 32; )
    {
        while (*rest == '\\' || *rest == '/') rest++;
        if (!*rest) break;
        parts[count++] = rest;
        rest += strcspn( rest, "\\/" );
    }
    for (k = 0; k < count; k++)
    {
        snprintf( above, sizeof(above), "%s", folder );
        for (up = 0; up < 4; up++)
        {
            if (launcher_shortcut_join( above, parts[k], path, sizeof(path) ) &&
                launcher_shortcut_try( path, exists, out, size ))
                return 1;
            if (!launcher_shortcut_join( above, "..", path, sizeof(path) ) || !strcmp( path, above )) break;
            snprintf( above, sizeof(above), "%s", path );
        }
    }
    return 0;
}

/* The program a parsed shortcut or batch file starts. folder is the file's own. */
static inline int launcher_shortcut_resolve( const char *folder, const struct launcher_shortcut *shortcut,
                                             launcher_shortcut_exists exists, char *out, size_t size )
{
    char joined[1100];

    if (shortcut->relative[0] && launcher_shortcut_locate( folder, shortcut->relative, exists, out, size )) return 1;
    if (!shortcut->target[0]) return 0;
    /* A batch file's program after its cd; a shortcut's target is whole already. */
    if (shortcut->workdir[0] && shortcut->target[1] != ':' && shortcut->target[0] != '\\' && shortcut->target[0] != '/')
    {
        snprintf( joined, sizeof(joined), "%s\\%s", shortcut->workdir, shortcut->target );
        if (launcher_shortcut_locate( folder, joined, exists, out, size )) return 1;
    }
    return launcher_shortcut_locate( folder, shortcut->target, exists, out, size );
}

#endif /* LAUNCHER_SHORTCUT_H */
