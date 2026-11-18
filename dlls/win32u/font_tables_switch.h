/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * A font file's tables for opentype.c, read on their own (Switch).
 *
 * Wine adds every font in C:\windows\fonts and share/wine/fonts each time it
 * starts (font.c keeps its font cache key volatile), and unix_face_create read
 * all of a file, once for each face in it, to look at a few tables. A CJK
 * collection added there (wqy-zenhei.ttc is 16 MB with three faces) went
 * through the card three times, more than a second at 42 MB/s.
 *
 * opentype.c reads the file's header, the face's table directory and its
 * head, OS/2 and name tables; hhea, CFF, EBDT, CBDT and EBSC it only looks up
 * in the directory. Those are read here into a buffer as large as the file, at
 * their offsets in it, and nothing else is: the parser sees the size and the
 * bytes it always did. A file that is not an sfnt (.fon, .pfb) has its first
 * bytes read, the parser turns it down, and FreeType reads it from its path.
 *
 * The parser does not check the name table's records and strings against the
 * table's length, so what they point at is read too, and past the end of the
 * file it is zeros.
 */
#ifndef FONT_TABLES_SWITCH_H
#define FONT_TABLES_SWITCH_H

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FONT_TABLES_ALLOC
#define FONT_TABLES_ALLOC(size) malloc( size )
#endif

/* More than head and OS/2 as opentype.c reads them (54 and 86 bytes). */
#define FONT_TABLES_SMALL 512

struct font_tables
{
    unsigned char *data;      /* free() it; at least the file's size */
    size_t size;              /* allocated */
    unsigned long long read;  /* bytes read from the file */
};

static inline unsigned int font_tables_be16( const unsigned char *p )
{
    return p[0] << 8 | p[1];
}

static inline unsigned int font_tables_be32( const unsigned char *p )
{
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
}

/* Bytes [offset, offset + length) of the file into data at the same offset,
 * as far as the file goes. */
static int font_tables_read( int fd, struct font_tables *t, size_t file_size, size_t offset, size_t length )
{
    size_t done = 0;

    if (offset >= file_size) return 1;
    if (length > file_size - offset) length = file_size - offset;
    if (!length) return 1;
    if (lseek( fd, offset, SEEK_SET ) == (off_t)-1) return 0;
    while (done < length)
    {
        ssize_t ret = read( fd, t->data + offset + done, length - done );

        if (ret <= 0) return 0;
        done += ret;
    }
    t->read += length;
    return 1;
}

/* What the parser reads past the end of the file is zeros. */
static int font_tables_extend( struct font_tables *t, size_t end )
{
    unsigned char *data;

    if (end <= t->size) return 1;
    if (!(data = realloc( t->data, end ))) return 0;
    memset( data + t->size, 0, end - t->size );
    t->data = data;
    t->size = end;
    return 1;
}

static int font_tables_read_name( int fd, struct font_tables *t, size_t file_size, size_t offset, size_t length )
{
    size_t have, end, i;
    unsigned int count, strings;

    if (!font_tables_read( fd, t, file_size, offset, length )) return 0;
    /* The parser takes the table whole in the file, with its header and a
     * record (tt_name_v0, 18 bytes), and only then reads the names. */
    if (length < 18 || offset > file_size || length > file_size - offset) return 1;
    count = font_tables_be16( t->data + offset + 2 );
    strings = font_tables_be16( t->data + offset + 4 );
    have = length;
    end = 6 + 12 * (size_t)count;
    if (end > have)
    {
        if (!font_tables_read( fd, t, file_size, offset + have, end - have ) ||
            !font_tables_extend( t, offset + end ))
            return 0;
        have = end;
    }
    for (i = 0; i < count; i++)
    {
        const unsigned char *record = t->data + offset + 6 + 12 * i;
        size_t to = strings + font_tables_be16( record + 10 ) + font_tables_be16( record + 8 );

        if (to > end) end = to;
    }
    if (end > have && (!font_tables_read( fd, t, file_size, offset + have, end - have ) ||
                       !font_tables_extend( t, offset + end )))
        return 0;
    return 1;
}

/* Reads what opentype.c needs of face index of the file fd, file_size bytes,
 * into t; 0 when it could not, and the file is better read whole. */
static int font_tables_load( int fd, size_t file_size, unsigned int index, struct font_tables *t )
{
    enum { HEAD = 1, OS_2 = 2, NAME = 4 };
    size_t sfnt, dir_end, at;
    unsigned int fourcc, found = 0;

    memset( t, 0, sizeof(*t) );
    if (file_size < 4 || !(t->data = FONT_TABLES_ALLOC( file_size ))) return 0;
    t->size = file_size;
    if (!font_tables_read( fd, t, file_size, 0, 12 )) goto failed;
    fourcc = font_tables_be32( t->data );
    if (fourcc == 0x74746366 /* ttcf */)
    {
        if (file_size < 12 || index >= font_tables_be32( t->data + 8 )) return 1;
        if ((file_size - 12) / 4 <= index) return 1;
        if (!font_tables_read( fd, t, file_size, 12 + 4 * (size_t)index, 4 )) goto failed;
        sfnt = font_tables_be32( t->data + 12 + 4 * (size_t)index );
    }
    else if (fourcc == 0x00010000 || fourcc == 0x4f54544f /* OTTO */) sfnt = 0;
    else return 1;

    if (sfnt > file_size || file_size - sfnt < 12) return 1;
    if (sfnt && !font_tables_read( fd, t, file_size, sfnt, 12 )) goto failed;
    dir_end = sfnt + 12 + 16 * (size_t)font_tables_be16( t->data + sfnt + 4 );
    if (dir_end > file_size) return 1;
    if (!font_tables_read( fd, t, file_size, sfnt + 12, dir_end - sfnt - 12 )) goto failed;

    for (at = sfnt + 12; at < dir_end; at += 16)
    {
        size_t offset = font_tables_be32( t->data + at + 8 ), length = font_tables_be32( t->data + at + 12 );
        unsigned int table;

        switch (font_tables_be32( t->data + at ))
        {
        case 0x68656164: table = HEAD; break; /* head */
        case 0x4f532f32: table = OS_2; break; /* OS/2 */
        case 0x6e616d65: table = NAME; break; /* name */
        default: continue;
        }
        /* The parser takes the first of a tag. */
        if (found & table) continue;
        found |= table;
        if (table == NAME)
        {
            if (!font_tables_read_name( fd, t, file_size, offset, length )) goto failed;
        }
        else if (!font_tables_read( fd, t, file_size, offset,
                                    length < FONT_TABLES_SMALL ? length : FONT_TABLES_SMALL ))
            goto failed;
    }
    return 1;

failed:
    free( t->data );
    t->data = NULL;
    return 0;
}

#endif /* FONT_TABLES_SWITCH_H */
