/*
 * What the launcher shows for a Windows program: the icon and the title in its
 * resources. Only the bytes needed are read, so a program with a large resource
 * section costs a few small reads. Every offset in the file is checked, since
 * any file named .exe gets here.
 */
#ifndef WINE_NX_LAUNCHER_PE_H
#define WINE_NX_LAUNCHER_PE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAUNCHER_PE_MAX_SECTIONS  96
#define LAUNCHER_RES_MAX_ENTRIES  4096     /* entries read from one resource directory */
#define LAUNCHER_RES_MAX_SIZE     0x100000 /* an icon or version resource larger than this is ignored */
#define LAUNCHER_ICON_MAX_SIDE    512

#define LAUNCHER_RT_ICON       3
#define LAUNCHER_RT_GROUP_ICON 14
#define LAUNCHER_RT_VERSION    16

struct launcher_pe_section
{
    uint32_t va, raw_offset, raw_size;
};

struct launcher_pe
{
    FILE *file;
    uint64_t file_size;
    struct launcher_pe_section sections[LAUNCHER_PE_MAX_SECTIONS];
    unsigned int section_count;
    uint32_t resource_rva, resource_size;
};

enum launcher_icon_kind
{
    LAUNCHER_ICON_NONE,
    LAUNCHER_ICON_RGBA,  /* width x height pixels, 4 bytes each: R, G, B, A */
    LAUNCHER_ICON_PNG,   /* a PNG file of size bytes, width and height from its header */
};

struct launcher_icon
{
    enum launcher_icon_kind kind;
    int width, height;
    unsigned char *data;
    size_t size;
};

static inline uint16_t launcher_u16( const unsigned char *p )
{
    return p[0] | (p[1] << 8);
}

static inline uint32_t launcher_u32( const unsigned char *p )
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int launcher_pe_read_at( struct launcher_pe *pe, uint64_t offset, void *buffer, size_t size )
{
    if (offset > pe->file_size || size > pe->file_size - offset) return 0;
    if (fseek( pe->file, (long)offset, SEEK_SET )) return 0;
    return fread( buffer, 1, size, pe->file ) == size;
}

/* Read size bytes at an address in the image; they must lie in one section's file data. */
static inline int launcher_pe_read_rva( struct launcher_pe *pe, uint32_t rva, void *buffer, size_t size )
{
    unsigned int i;

    for (i = 0; i < pe->section_count; i++)
    {
        const struct launcher_pe_section *section = &pe->sections[i];

        if (rva < section->va || rva - section->va >= section->raw_size) continue;
        if (size > section->raw_size - (rva - section->va)) return 0;
        return launcher_pe_read_at( pe, (uint64_t)section->raw_offset + (rva - section->va), buffer, size );
    }
    return 0;
}

static inline void launcher_pe_close( struct launcher_pe *pe )
{
    if (pe->file) fclose( pe->file );
    pe->file = NULL;
}

/* Returns 1 for a PE image, whose resources may still be missing (resource_size 0). */
static inline int launcher_pe_open( struct launcher_pe *pe, const char *path )
{
    unsigned char dos[64], nt[24 + 240], section[40];
    uint32_t nt_offset, directories_offset, directory_count;
    uint16_t optional_size, magic;
    unsigned int i, count;
    long end;

    memset( pe, 0, sizeof(*pe) );
    if (!(pe->file = fopen( path, "rb" ))) return 0;
    if (fseek( pe->file, 0, SEEK_END ) || (end = ftell( pe->file )) < 0) goto fail;
    pe->file_size = (uint64_t)end;

    if (!launcher_pe_read_at( pe, 0, dos, sizeof(dos) ) || dos[0] != 'M' || dos[1] != 'Z') goto fail;
    nt_offset = launcher_u32( dos + 0x3c );
    if (!launcher_pe_read_at( pe, nt_offset, nt, 24 + 2 ) || memcmp( nt, "PE\0\0", 4 )) goto fail;
    count = launcher_u16( nt + 6 );
    optional_size = launcher_u16( nt + 20 );
    magic = launcher_u16( nt + 24 );
    if (magic == 0x10b) directories_offset = 96;
    else if (magic == 0x20b) directories_offset = 112;
    else goto fail;
    if (optional_size > 240) optional_size = 240;
    if (optional_size < directories_offset || !launcher_pe_read_at( pe, nt_offset, nt, 24 + optional_size )) goto fail;

    directory_count = launcher_u32( nt + 24 + directories_offset - 4 );
    if (directory_count > 2 && directories_offset + 3 * 8 <= optional_size)
    {
        pe->resource_rva = launcher_u32( nt + 24 + directories_offset + 2 * 8 );
        pe->resource_size = launcher_u32( nt + 24 + directories_offset + 2 * 8 + 4 );
    }

    if (count > LAUNCHER_PE_MAX_SECTIONS) count = LAUNCHER_PE_MAX_SECTIONS;
    for (i = 0; i < count; i++)
    {
        uint64_t offset = (uint64_t)nt_offset + 24 + launcher_u16( nt + 20 ) + i * 40;

        if (!launcher_pe_read_at( pe, offset, section, sizeof(section) )) break;
        pe->sections[i].va = launcher_u32( section + 12 );
        pe->sections[i].raw_size = launcher_u32( section + 16 );
        pe->sections[i].raw_offset = launcher_u32( section + 20 );
        /* The loader maps no more than the virtual size, when there is one. */
        if (launcher_u32( section + 8 ) && launcher_u32( section + 8 ) < pe->sections[i].raw_size)
            pe->sections[i].raw_size = launcher_u32( section + 8 );
    }
    pe->section_count = i;
    return 1;

fail:
    launcher_pe_close( pe );
    return 0;
}

/* The OffsetToData of an entry in the resource directory at dir (relative to the
 * resource section): the entry with this ID, or the first entry when id < 0. */
static inline int launcher_pe_res_entry( struct launcher_pe *pe, uint32_t dir, int id, uint32_t *value )
{
    unsigned char header[16], *entries;
    unsigned int count, i;
    int found = 0;

    if (dir >= pe->resource_size || pe->resource_size - dir < sizeof(header)) return 0;
    if (!launcher_pe_read_rva( pe, pe->resource_rva + dir, header, sizeof(header) )) return 0;
    count = launcher_u16( header + 12 ) + launcher_u16( header + 14 );
    if (count > LAUNCHER_RES_MAX_ENTRIES) count = LAUNCHER_RES_MAX_ENTRIES;
    if (!count || (pe->resource_size - dir - sizeof(header)) / 8 < count) return 0;
    if (!(entries = malloc( count * 8 ))) return 0;
    if (launcher_pe_read_rva( pe, pe->resource_rva + dir + sizeof(header), entries, count * 8 ))
    {
        for (i = 0; i < count; i++)
        {
            uint32_t name = launcher_u32( entries + i * 8 );

            if (id >= 0 && ((name & 0x80000000) || name != (uint32_t)id)) continue;
            *value = launcher_u32( entries + i * 8 + 4 );
            found = 1;
            break;
        }
    }
    free( entries );
    return found;
}

/* Load a resource of this type: the one with this ID, or the first when id < 0,
 * in its first language. The caller frees *data. */
static inline int launcher_pe_load_resource( struct launcher_pe *pe, int type, int id,
                                             unsigned char **data, uint32_t *size )
{
    unsigned char leaf[16];
    uint32_t value, rva;

    if (!pe->resource_size) return 0;
    if (!launcher_pe_res_entry( pe, 0, type, &value ) || !(value & 0x80000000)) return 0;
    if (!launcher_pe_res_entry( pe, value & 0x7fffffff, id, &value ) || !(value & 0x80000000)) return 0;
    if (!launcher_pe_res_entry( pe, value & 0x7fffffff, -1, &value ) || (value & 0x80000000)) return 0;
    if (value >= pe->resource_size || pe->resource_size - value < sizeof(leaf)) return 0;
    if (!launcher_pe_read_rva( pe, pe->resource_rva + value, leaf, sizeof(leaf) )) return 0;
    rva = launcher_u32( leaf );
    *size = launcher_u32( leaf + 4 );
    if (!*size || *size > LAUNCHER_RES_MAX_SIZE || !(*data = malloc( *size ))) return 0;
    if (launcher_pe_read_rva( pe, rva, *data, *size )) return 1;
    free( *data );
    *data = NULL;
    return 0;
}

static inline int launcher_png_size( const unsigned char *data, size_t size, int *width, int *height )
{
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    uint32_t w, h;

    if (size < 24 || memcmp( data, signature, 8 ) || memcmp( data + 12, "IHDR", 4 )) return 0;
    w = ((uint32_t)data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    h = ((uint32_t)data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    if (!w || !h || w > LAUNCHER_ICON_MAX_SIDE || h > LAUNCHER_ICON_MAX_SIDE) return 0;
    *width = w;
    *height = h;
    return 1;
}

/* The icon group entry to show at about want pixels: the smallest image at least
 * that large, else the largest, and among equal sizes the most colours. Returns
 * the RT_ICON ID, or -1. */
static inline int launcher_icon_pick( const unsigned char *group, size_t size, int want )
{
    unsigned int count, i;
    int best = -1, best_side = 0, best_bits = 0;

    if (size < 6 || launcher_u16( group + 2 ) != 1) return -1;
    count = launcher_u16( group + 4 );
    if (count > (size - 6) / 14) count = (size - 6) / 14;
    for (i = 0; i < count; i++)
    {
        const unsigned char *entry = group + 6 + i * 14;
        int side = entry[0] ? entry[0] : 256, bits = launcher_u16( entry + 6 ), better;

        if (!bits) bits = entry[2] ? (entry[2] <= 2 ? 1 : entry[2] <= 16 ? 4 : 8) : 8;
        if (best < 0) better = 1;
        else if ((side >= want) != (best_side >= want)) better = side >= want;
        else if (side != best_side) better = side >= want ? side < best_side : side > best_side;
        else better = bits > best_bits;
        if (!better) continue;
        best = launcher_u16( entry + 12 );
        best_side = side;
        best_bits = bits;
    }
    return best;
}

/* An icon image stored as a DIB: the colour rows, then a 1-bit transparency mask,
 * both bottom-up, with the header's height counting both. */
static inline int launcher_icon_decode_dib( const unsigned char *data, size_t size, struct launcher_icon *icon )
{
    uint32_t header_size, colors = 0, compression;
    int32_t width, height;
    uint64_t stride, mask_stride, palette_size, needed;
    const unsigned char *palette, *pixels, *mask;
    unsigned int bits, x, y;
    int alpha_seen = 0;
    unsigned char *out;

    if (size < 40) return 0;
    header_size = launcher_u32( data );
    width = (int32_t)launcher_u32( data + 4 );
    height = (int32_t)launcher_u32( data + 8 );
    bits = launcher_u16( data + 14 );
    compression = launcher_u32( data + 16 );
    if (header_size < 40 || header_size > size) return 0;
    /* A top-down DIB has a negative height; the most negative has no positive
     * and is no icon. */
    if (height == INT32_MIN) return 0;
    if (height < 0) height = -height;
    height /= 2;
    if (width <= 0 || height <= 0 || width > LAUNCHER_ICON_MAX_SIDE || height > LAUNCHER_ICON_MAX_SIDE) return 0;
    if (compression != 0 && !(compression == 3 && bits == 32)) return 0;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return 0;
    if (bits <= 8)
    {
        colors = launcher_u32( data + 32 );
        if (!colors || colors > (1u << bits)) colors = 1u << bits;
    }
    /* BI_BITFIELDS keeps three masks after a 40-byte header; icons use BGRA order anyway. */
    palette_size = colors * 4 + (compression == 3 && header_size == 40 ? 12 : 0);
    stride = ((uint64_t)width * bits + 31) / 32 * 4;
    mask_stride = ((uint64_t)width + 31) / 32 * 4;
    needed = header_size + palette_size + stride * height;
    if (needed > size) return 0;
    palette = data + header_size;
    pixels = data + header_size + palette_size;
    /* Some icons leave the mask out; they are then fully opaque unless they have alpha. */
    mask = needed + mask_stride * height <= size ? pixels + stride * height : NULL;

    if (!(out = malloc( (size_t)width * height * 4 ))) return 0;
    for (y = 0; y < (unsigned int)height; y++)
    {
        const unsigned char *row = pixels + (height - 1 - y) * stride;
        const unsigned char *mask_row = mask ? mask + (height - 1 - y) * mask_stride : NULL;

        for (x = 0; x < (unsigned int)width; x++)
        {
            unsigned char *px = out + ((size_t)y * width + x) * 4;
            unsigned int index;

            switch (bits)
            {
            case 32:
                px[0] = row[x * 4 + 2]; px[1] = row[x * 4 + 1]; px[2] = row[x * 4]; px[3] = row[x * 4 + 3];
                alpha_seen |= px[3];
                break;
            case 24:
                px[0] = row[x * 3 + 2]; px[1] = row[x * 3 + 1]; px[2] = row[x * 3]; px[3] = 255;
                break;
            case 16:
            {
                unsigned int v = launcher_u16( row + x * 2 );
                px[0] = ((v >> 10) & 31) * 255 / 31; px[1] = ((v >> 5) & 31) * 255 / 31;
                px[2] = (v & 31) * 255 / 31; px[3] = 255;
                break;
            }
            default:
                if (bits == 8) index = row[x];
                else if (bits == 4) index = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                else index = (row[x / 8] >> (7 - x % 8)) & 1;
                if (index >= colors) index = 0;
                px[0] = palette[index * 4 + 2]; px[1] = palette[index * 4 + 1]; px[2] = palette[index * 4];
                px[3] = 255;
                break;
            }
            if (bits != 32 && mask_row && ((mask_row[x / 8] >> (7 - x % 8)) & 1)) px[3] = 0;
        }
    }
    /* 32-bit icons made before alpha channels leave it zero and use the mask. */
    if (bits == 32 && !alpha_seen)
    {
        for (y = 0; y < (unsigned int)height; y++)
        {
            const unsigned char *mask_row = mask ? mask + (height - 1 - y) * mask_stride : NULL;

            for (x = 0; x < (unsigned int)width; x++)
                out[((size_t)y * width + x) * 4 + 3] =
                    mask_row && ((mask_row[x / 8] >> (7 - x % 8)) & 1) ? 0 : 255;
        }
    }
    icon->kind = LAUNCHER_ICON_RGBA;
    icon->width = width;
    icon->height = height;
    icon->data = out;
    icon->size = (size_t)width * height * 4;
    return 1;
}

static inline void launcher_icon_free( struct launcher_icon *icon )
{
    free( icon->data );
    memset( icon, 0, sizeof(*icon) );
}

/* Shrink an RGBA icon to at most max_side pixels, averaging the source pixels
 * under each target pixel weighted by their alpha, so edges keep their colour. */
static inline int launcher_icon_fit( struct launcher_icon *icon, int max_side )
{
    int side = icon->width > icon->height ? icon->width : icon->height;
    int width, height, x, y, sx, sy;
    unsigned char *out;

    if (icon->kind != LAUNCHER_ICON_RGBA || side <= max_side) return 1;
    width = icon->width * max_side / side;
    height = icon->height * max_side / side;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (!(out = malloc( (size_t)width * height * 4 ))) return 0;
    for (y = 0; y < height; y++)
    {
        int y0 = y * icon->height / height, y1 = (y + 1) * icon->height / height;

        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < width; x++)
        {
            int x0 = x * icon->width / width, x1 = (x + 1) * icon->width / width;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            unsigned char *px = out + ((size_t)y * width + x) * 4;

            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1; sy++)
                for (sx = x0; sx < x1; sx++)
                {
                    const unsigned char *s = icon->data + ((size_t)sy * icon->width + sx) * 4;

                    r += (uint64_t)s[0] * s[3]; g += (uint64_t)s[1] * s[3]; b += (uint64_t)s[2] * s[3];
                    a += s[3];
                    n++;
                }
            px[0] = a ? r / a : 0;
            px[1] = a ? g / a : 0;
            px[2] = a ? b / a : 0;
            px[3] = a / n;
        }
    }
    free( icon->data );
    icon->data = out;
    icon->width = width;
    icon->height = height;
    icon->size = (size_t)width * height * 4;
    return 1;
}

/* A block of a version resource: length, value length, type, a UTF-16 key, a
 * value and children, each aligned to 4 bytes from the start of the resource. */
struct launcher_vs_block
{
    const unsigned char *key;  /* UTF-16, key_chars long */
    size_t key_chars;
    const unsigned char *value, *children, *end;
    size_t value_bytes;
    int text;
};

static inline int launcher_vs_block( const unsigned char *base, const unsigned char *p,
                                     const unsigned char *limit, struct launcher_vs_block *block )
{
    size_t length, offset;
    const unsigned char *q;

    if (limit - p < 6) return 0;
    length = launcher_u16( p );
    if (length < 6 || length > (size_t)(limit - p)) return 0;
    block->end = p + length;
    block->text = launcher_u16( p + 4 ) == 1;
    block->key = p + 6;
    for (q = block->key; block->end - q >= 2 && launcher_u16( q ); q += 2) {}
    if (block->end - q < 2) return 0;
    block->key_chars = (q - block->key) / 2;
    offset = (q + 2 - base + 3) & ~(size_t)3;
    block->value = base + offset > block->end ? block->end : base + offset;
    block->value_bytes = launcher_u16( p + 2 ) * (block->text ? 2 : 1);
    if (block->value_bytes > (size_t)(block->end - block->value)) block->value_bytes = block->end - block->value;
    offset = (block->value + block->value_bytes - base + 3) & ~(size_t)3;
    block->children = base + offset > block->end ? block->end : base + offset;
    return 1;
}

/* The sibling after block, aligned, but never past its parent's end. */
static inline const unsigned char *launcher_vs_next( const unsigned char *base, const struct launcher_vs_block *block,
                                                     const unsigned char *parent_end )
{
    size_t offset = (block->end - base + 3) & ~(size_t)3;

    return (size_t)(parent_end - base) < offset ? parent_end : base + offset;
}

/* Without regard to ASCII case, as VerQueryValue looks keys up. */
static inline int launcher_vs_key_is( const struct launcher_vs_block *block, const char *key )
{
    size_t i, len = strlen( key );

    if (block->key_chars != len) return 0;
    for (i = 0; i < len; i++)
    {
        unsigned int have = launcher_u16( block->key + i * 2 ), want = (unsigned char)key[i];

        if (have >= 'A' && have <= 'Z') have += 'a' - 'A';
        if (want >= 'A' && want <= 'Z') want += 'a' - 'A';
        if (have != want) return 0;
    }
    return 1;
}

/* UTF-16 from text up to its end or a zero, as UTF-8 with spaces trimmed. */
static inline void launcher_utf16_to_utf8( const unsigned char *text, size_t bytes, char *out, size_t size )
{
    size_t i, len = 0;

    if (!size) return;
    for (i = 0; i + 1 < bytes; i += 2)
    {
        uint32_t c = launcher_u16( text + i );
        char utf8[4];
        size_t n;

        if (!c) break;
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < bytes && (launcher_u16( text + i + 2 ) & 0xfc00) == 0xdc00)
        {
            c = 0x10000 + ((c - 0xd800) << 10) + (launcher_u16( text + i + 2 ) - 0xdc00);
            i += 2;
        }
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        if (c < 0x20) c = ' ';
        if (c < 0x80) { utf8[0] = c; n = 1; }
        else if (c < 0x800) { utf8[0] = 0xc0 | c >> 6; utf8[1] = 0x80 | (c & 0x3f); n = 2; }
        else if (c < 0x10000)
        {
            utf8[0] = 0xe0 | c >> 12; utf8[1] = 0x80 | ((c >> 6) & 0x3f); utf8[2] = 0x80 | (c & 0x3f); n = 3;
        }
        else
        {
            utf8[0] = 0xf0 | c >> 18; utf8[1] = 0x80 | ((c >> 12) & 0x3f);
            utf8[2] = 0x80 | ((c >> 6) & 0x3f); utf8[3] = 0x80 | (c & 0x3f); n = 4;
        }
        if (len + n >= size) break;
        if (!len && n == 1 && utf8[0] == ' ') continue;
        memcpy( out + len, utf8, n );
        len += n;
    }
    while (len && out[len - 1] == ' ') len--;
    out[len] = 0;
}

/* The first of keys (a NULL-terminated list) with a value in a version resource's
 * string table: US English tables first, then the others, each table asked for
 * every key in order. */
static inline int launcher_version_string( const unsigned char *data, size_t size, const char * const *keys,
                                           char *out, size_t out_size )
{
    const unsigned char *limit = data + size, *p, *t, *s;
    struct launcher_vs_block root, info, table, string;
    const char * const *key;
    int pass, english;

    if (!launcher_vs_block( data, data, limit, &root ) || !launcher_vs_key_is( &root, "VS_VERSION_INFO" )) return 0;
    for (pass = 0; pass < 2; pass++)
    {
        for (p = root.children; launcher_vs_block( data, p, root.end, &info ); p = launcher_vs_next( data, &info, root.end ))
        {
            if (!launcher_vs_key_is( &info, "StringFileInfo" )) continue;
            for (t = info.children; launcher_vs_block( data, t, info.end, &table );
                 t = launcher_vs_next( data, &table, info.end ))
            {
                /* The table's key is its language and code page as hex: 040904b0. */
                english = table.key_chars == 8 && launcher_u16( table.key ) == '0' &&
                          launcher_u16( table.key + 2 ) == '4' && launcher_u16( table.key + 4 ) == '0' &&
                          launcher_u16( table.key + 6 ) == '9';
                if (english == pass) continue;
                for (key = keys; *key; key++)
                {
                    for (s = table.children; launcher_vs_block( data, s, table.end, &string );
                         s = launcher_vs_next( data, &string, table.end ))
                    {
                        if (!launcher_vs_key_is( &string, *key )) continue;
                        launcher_utf16_to_utf8( string.value, string.value_bytes, out, out_size );
                        if (out[0]) return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* The icon (kind NONE when there is none) and title (empty when there is none)
 * of the program at path. Returns 0 when it is not a PE image. */
static inline int launcher_pe_describe( const char *path, int want, struct launcher_icon *icon,
                                        char *title, size_t title_size )
{
    struct launcher_pe pe;
    unsigned char *data;
    uint32_t size;
    int id;

    if (icon) memset( icon, 0, sizeof(*icon) );
    if (title_size) title[0] = 0;
    if (!launcher_pe_open( &pe, path )) return 0;

    if (title_size && launcher_pe_load_resource( &pe, LAUNCHER_RT_VERSION, -1, &data, &size ))
    {
        static const char * const keys[] = { "FileDescription", "ProductName", NULL };

        launcher_version_string( data, size, keys, title, title_size );
        free( data );
    }

    if (icon && launcher_pe_load_resource( &pe, LAUNCHER_RT_GROUP_ICON, -1, &data, &size ))
    {
        id = launcher_icon_pick( data, size, want );
        free( data );
        if (id >= 0 && launcher_pe_load_resource( &pe, LAUNCHER_RT_ICON, id, &data, &size ))
        {
            if (launcher_png_size( data, size, &icon->width, &icon->height ))
            {
                icon->kind = LAUNCHER_ICON_PNG;
                icon->data = data;
                icon->size = size;
            }
            else
            {
                launcher_icon_decode_dib( data, size, icon );
                free( data );
            }
        }
    }
    launcher_pe_close( &pe );
    return 1;
}

#endif
