/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * A game's text file as the launcher's editor (launcher_text_edit.c) holds it:
 * its lines in UTF-8, and what it takes to write them back as the file had
 * them -- its encoding and byte order mark, its line ends, and whether it ended
 * with one. A line nobody changed goes back byte for byte. Pure, so
 * tests/check_text_edit.py runs it on a computer.
 *
 * Games keep their settings in the encodings Windows programs write: UTF-8,
 * with a byte order mark or without, UTF-16 with one (Unreal's .ini files, a
 * .inf), and the ANSI code page, Windows-1252 on a western Windows, for a file
 * that is not UTF-8. Bytes Windows-1252 leaves undefined are kept as the
 * control characters of the same number, so that every such file reads and
 * writes back as it was. A file with a NUL in it and no UTF-16 mark is not
 * text.
 */
#ifndef WINE_NX_LAUNCHER_TEXT_H
#define WINE_NX_LAUNCHER_TEXT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum launcher_text_encoding
{
    LAUNCHER_TEXT_UTF8,
    LAUNCHER_TEXT_UTF8_BOM,
    LAUNCHER_TEXT_UTF16LE,
    LAUNCHER_TEXT_UTF16BE,
    LAUNCHER_TEXT_ANSI,       /* Windows-1252 */
};

enum launcher_text_result
{
    LAUNCHER_TEXT_OK,
    LAUNCHER_TEXT_BINARY,     /* not a text file */
    LAUNCHER_TEXT_TOO_LONG,   /* more lines than max_lines */
    LAUNCHER_TEXT_NO_MEMORY,
};

struct launcher_text
{
    enum launcher_text_encoding encoding;
    const char *newline;      /* "\r\n", "\n" or "\r": the file's first */
    int final_newline;        /* whether the last line had one */
    int count, capacity;
    char **lines;             /* UTF-8, without their ends */
};

static const uint16_t launcher_text_cp1252[32] =
{
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

/* The code point at *pos of a UTF-8 string of size bytes, moving *pos past it;
 * -1 for bytes that are not UTF-8 (overlong, a surrogate, past U+10FFFF, cut). */
static inline int32_t launcher_text_utf8_next( const unsigned char *text, size_t size, size_t *pos )
{
    size_t i = *pos, need;
    uint32_t c = text[i], min;

    if (c < 0x80) { *pos = i + 1; return c; }
    if ((c & 0xe0) == 0xc0) { need = 1; c &= 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { need = 2; c &= 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { need = 3; c &= 0x07; min = 0x10000; }
    else return -1;
    if (size - i <= need) return -1;
    for (size_t k = 1; k <= need; k++)
    {
        if ((text[i + k] & 0xc0) != 0x80) return -1;
        c = (c << 6) | (text[i + k] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    *pos = i + need + 1;
    return (int32_t)c;
}

static inline size_t launcher_text_utf8_put( char *out, uint32_t c )
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000)
    {
        out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f)); out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

/* The characters of a UTF-8 line as the console's keyboard counts them: UTF-16
 * code units, two for a character past U+FFFF, one for a byte that is not
 * UTF-8. */
static inline size_t launcher_text_utf16_length( const char *line )
{
    const unsigned char *text = (const unsigned char *)line;
    size_t size = strlen( line ), pos = 0, units = 0;

    while (pos < size)
    {
        int32_t c = launcher_text_utf8_next( text, size, &pos );

        if (c < 0) pos++;
        units += c >= 0x10000 ? 2 : 1;
    }
    return units;
}

static inline void launcher_text_free( struct launcher_text *text )
{
    for (int i = 0; i < text->count; i++) free( text->lines[i] );
    free( text->lines );
    memset( text, 0, sizeof(*text) );
}

static inline int launcher_text_insert( struct launcher_text *text, int index, const char *line )
{
    char *copy;

    if (index < 0 || index > text->count) return 0;
    if (text->count == text->capacity)
    {
        int capacity = text->capacity ? text->capacity * 2 : 64;
        char **lines = realloc( text->lines, capacity * sizeof(*lines) );

        if (!lines) return 0;
        text->lines = lines;
        text->capacity = capacity;
    }
    if (!(copy = strdup( line ))) return 0;
    memmove( text->lines + index + 1, text->lines + index, (text->count - index) * sizeof(*text->lines) );
    text->lines[index] = copy;
    text->count++;
    return 1;
}

static inline int launcher_text_set( struct launcher_text *text, int index, const char *line )
{
    char *copy;

    if (index < 0 || index >= text->count || !(copy = strdup( line ))) return 0;
    free( text->lines[index] );
    text->lines[index] = copy;
    return 1;
}

static inline void launcher_text_delete( struct launcher_text *text, int index )
{
    if (index < 0 || index >= text->count) return;
    free( text->lines[index] );
    memmove( text->lines + index, text->lines + index + 1, (text->count - index - 1) * sizeof(*text->lines) );
    text->count--;
}

/* The file's bytes as UTF-8, NUL-terminated, in a buffer of the caller's to free. */
static inline enum launcher_text_result launcher_text_decode( const unsigned char *data, size_t size,
                                                             enum launcher_text_encoding *encoding,
                                                             char **out, size_t *out_size )
{
    size_t pos = 0, len = 0;
    char *utf8;

    if (size >= 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff)))
    {
        int big = data[0] == 0xfe;

        *encoding = big ? LAUNCHER_TEXT_UTF16BE : LAUNCHER_TEXT_UTF16LE;
        if (size % 2) return LAUNCHER_TEXT_BINARY;
        if (!(utf8 = malloc( (size - 2) / 2 * 3 + 1 ))) return LAUNCHER_TEXT_NO_MEMORY;
        for (pos = 2; pos < size; pos += 2)
        {
            uint32_t c = big ? (data[pos] << 8 | data[pos + 1]) : (data[pos + 1] << 8 | data[pos]);

            if (!c) { free( utf8 ); return LAUNCHER_TEXT_BINARY; }
            if (c >= 0xd800 && c <= 0xdbff && pos + 3 < size)
            {
                uint32_t low = big ? (data[pos + 2] << 8 | data[pos + 3]) : (data[pos + 3] << 8 | data[pos + 2]);

                if (low >= 0xdc00 && low <= 0xdfff)
                {
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    pos += 2;
                }
                else c = 0xfffd;
            }
            else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
            len += launcher_text_utf8_put( utf8 + len, c );
        }
        utf8[len] = 0;
        *out = utf8;
        *out_size = len;
        return LAUNCHER_TEXT_OK;
    }
    if (memchr( data, 0, size )) return LAUNCHER_TEXT_BINARY;
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
    {
        *encoding = LAUNCHER_TEXT_UTF8_BOM;
        data += 3;
        size -= 3;
    }
    else *encoding = LAUNCHER_TEXT_UTF8;
    while (pos < size && launcher_text_utf8_next( data, size, &pos ) >= 0) {}
    if (pos < size)
    {
        /* Not UTF-8: the ANSI code page. A file with a UTF-8 mark and bytes
         * that are not is read the same way, mark and all. */
        if (*encoding == LAUNCHER_TEXT_UTF8_BOM) { data -= 3; size += 3; }
        *encoding = LAUNCHER_TEXT_ANSI;
        if (!(utf8 = malloc( size * 3 + 1 ))) return LAUNCHER_TEXT_NO_MEMORY;
        for (pos = 0; pos < size; pos++)
            len += launcher_text_utf8_put( utf8 + len, data[pos] >= 0x80 && data[pos] < 0xa0
                                                       ? launcher_text_cp1252[data[pos] - 0x80] : data[pos] );
    }
    else
    {
        if (!(utf8 = malloc( size + 1 ))) return LAUNCHER_TEXT_NO_MEMORY;
        memcpy( utf8, data, size );
        len = size;
    }
    utf8[len] = 0;
    *out = utf8;
    *out_size = len;
    return LAUNCHER_TEXT_OK;
}

/* A file's bytes into lines. text is left empty on failure. */
static inline enum launcher_text_result launcher_text_parse( struct launcher_text *text, const unsigned char *data,
                                                            size_t size, int max_lines )
{
    enum launcher_text_result result;
    size_t utf8_size, start = 0, i;
    char *utf8;

    memset( text, 0, sizeof(*text) );
    if ((result = launcher_text_decode( data, size, &text->encoding, &utf8, &utf8_size ))) return result;
    text->newline = "\r\n";
    for (i = 0; i < utf8_size; i++)
        if (utf8[i] == '\r' || utf8[i] == '\n')
        {
            text->newline = utf8[i] == '\n' ? "\n" : i + 1 < utf8_size && utf8[i + 1] == '\n' ? "\r\n" : "\r";
            break;
        }
    /* Any of the three ends a line; the file's first is what they are written
     * back as. An empty file is one empty line without an end. */
    for (i = 0; i <= utf8_size; i++)
    {
        int end = i == utf8_size, crlf;

        if (!end && utf8[i] != '\r' && utf8[i] != '\n') continue;
        if (end && start == utf8_size && text->count)
        {
            text->final_newline = 1;
            break;
        }
        if (text->count >= max_lines) { result = LAUNCHER_TEXT_TOO_LONG; break; }
        crlf = !end && utf8[i] == '\r' && i + 1 < utf8_size && utf8[i + 1] == '\n';
        utf8[i] = 0;
        if (!launcher_text_insert( text, text->count, utf8 + start )) { result = LAUNCHER_TEXT_NO_MEMORY; break; }
        i += crlf;
        start = i + 1;
    }
    free( utf8 );
    if (result) launcher_text_free( text );
    return result;
}

/* The lines as the file's bytes, in a buffer of the caller's to free; NULL
 * when there is no memory. A character the ANSI code page has no byte for
 * becomes '?'. */
static inline unsigned char *launcher_text_build( const struct launcher_text *text, size_t *size )
{
    size_t total = 3, len = 0, newline = strlen( text->newline );
    unsigned char *out;

    for (int i = 0; i < text->count; i++) total += strlen( text->lines[i] ) + newline;
    /* UTF-16 takes at most two bytes for each byte of UTF-8, and a byte that is
     * not UTF-8 three, as U+FFFD. */
    if (!(out = malloc( total * 3 ))) return NULL;
    if (text->encoding == LAUNCHER_TEXT_UTF8_BOM) { memcpy( out, "\xef\xbb\xbf", 3 ); len = 3; }
    if (text->encoding == LAUNCHER_TEXT_UTF16LE) { memcpy( out, "\xff\xfe", 2 ); len = 2; }
    if (text->encoding == LAUNCHER_TEXT_UTF16BE) { memcpy( out, "\xfe\xff", 2 ); len = 2; }
    for (int i = 0; i < text->count; i++)
    {
        const unsigned char *line = (const unsigned char *)text->lines[i];
        size_t line_size = strlen( text->lines[i] ), pos = 0;
        int last = i == text->count - 1;

        for (int part = 0; part < 2; part++)
        {
            if (part)
            {
                if (last && !text->final_newline) break;
                line = (const unsigned char *)text->newline;
                line_size = newline;
                pos = 0;
            }
            while (pos < line_size)
            {
                int32_t c = launcher_text_utf8_next( line, line_size, &pos );

                if (c < 0) { c = 0xfffd; pos++; }
                switch (text->encoding)
                {
                case LAUNCHER_TEXT_UTF8:
                case LAUNCHER_TEXT_UTF8_BOM:
                    len += launcher_text_utf8_put( (char *)out + len, c );
                    break;
                case LAUNCHER_TEXT_ANSI:
                {
                    int byte = c < 0x80 || (c >= 0xa0 && c < 0x100) ? c : '?';

                    for (int k = 0; byte == '?' && k < 32; k++)
                        if (launcher_text_cp1252[k] == c) byte = 0x80 + k;
                    out[len++] = (unsigned char)byte;
                    break;
                }
                case LAUNCHER_TEXT_UTF16LE:
                case LAUNCHER_TEXT_UTF16BE:
                {
                    uint16_t units[2];
                    int n = 1;

                    if (c >= 0x10000)
                    {
                        units[0] = 0xd800 + ((c - 0x10000) >> 10);
                        units[1] = 0xdc00 + ((c - 0x10000) & 0x3ff);
                        n = 2;
                    }
                    else units[0] = (uint16_t)c;
                    for (int k = 0; k < n; k++)
                    {
                        out[len++] = text->encoding == LAUNCHER_TEXT_UTF16LE ? units[k] & 0xff : units[k] >> 8;
                        out[len++] = text->encoding == LAUNCHER_TEXT_UTF16LE ? units[k] >> 8 : units[k] & 0xff;
                    }
                    break;
                }
                }
            }
        }
    }
    *size = len;
    return out;
}

/* Whether a line is an .ini setting, "key = value": where its key starts and
 * how long it is, and where its value starts (after the '=' and the blanks
 * after it). A section, a comment (';', '#', "//") and a line without a key
 * before its '=' are not settings. */
static inline int launcher_text_setting( const char *line, size_t *key_start, size_t *key_len, size_t *value_start )
{
    const char *equals, *start = line, *end;

    while (*start == ' ' || *start == '\t') start++;
    if (*start == ';' || *start == '#' || *start == '[' || (start[0] == '/' && start[1] == '/')) return 0;
    if (!(equals = strchr( start, '=' ))) return 0;
    end = equals;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end == start) return 0;
    *key_start = start - line;
    *key_len = end - start;
    end = equals + 1;
    while (*end == ' ' || *end == '\t') end++;
    *value_start = end - line;
    return 1;
}

#endif /* WINE_NX_LAUNCHER_TEXT_H */
