/* Host test for the launcher's icon and title reader (source/launcher_pe.h):
 * a program built here with four icon images and two version tables, the same
 * program with random bytes changed, and real programs from the staged card
 * when they are there. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../source/launcher_pe.h"

struct buffer
{
    unsigned char data[1 << 16];
    size_t size;
};

static void put( struct buffer *b, const void *data, size_t size )
{
    assert( b->size + size <= sizeof(b->data) );
    memcpy( b->data + b->size, data, size );
    b->size += size;
}

static void put16( struct buffer *b, unsigned v ) { unsigned char p[2] = { v, v >> 8 }; put( b, p, 2 ); }
static void put32( struct buffer *b, unsigned v ) { unsigned char p[4] = { v, v >> 8, v >> 16, v >> 24 }; put( b, p, 4 ); }
static void set16( struct buffer *b, size_t at, unsigned v ) { b->data[at] = v; b->data[at + 1] = v >> 8; }
static void set32( struct buffer *b, size_t at, unsigned v )
{
    b->data[at] = v; b->data[at + 1] = v >> 8; b->data[at + 2] = v >> 16; b->data[at + 3] = v >> 24;
}
static void align4( struct buffer *b ) { while (b->size % 4) put( b, "", 1 ); }
static void put_wide( struct buffer *b, const char *s ) { while (*s) put16( b, (unsigned char)*s++ ); put16( b, 0 ); }

/* Palette index of the test image at (x, y): stripes, so rows and columns are checked. */
static unsigned pattern( unsigned x, unsigned y ) { return (x / 3 + y) % 4; }
/* Transparent where the mask bit is set: the top-left quarter. */
static int masked( unsigned x, unsigned y, unsigned side ) { return x < side / 2 && y < side / 2; }

static void put_mask( struct buffer *b, unsigned side )
{
    unsigned x, y, stride = (side + 31) / 32 * 4;

    for (y = side; y-- > 0;)
    {
        unsigned char row[64] = {0};
        for (x = 0; x < side; x++) if (masked( x, y, side )) row[x / 8] |= 0x80 >> (x % 8);
        put( b, row, stride );
    }
}

static void put_dib_header( struct buffer *b, unsigned side, unsigned bits, unsigned colors )
{
    put32( b, 40 ); put32( b, side ); put32( b, side * 2 ); put16( b, 1 ); put16( b, bits );
    put32( b, 0 ); put32( b, 0 ); put32( b, 0 ); put32( b, 0 ); put32( b, colors ); put32( b, 0 );
}

static const unsigned char palette_rgb[4][3] = { { 0, 0, 0 }, { 255, 0, 0 }, { 0, 200, 0 }, { 10, 20, 250 } };

static void make_dib4( struct buffer *b, unsigned side )
{
    unsigned i, x, y, stride = (side * 4 + 31) / 32 * 4;

    put_dib_header( b, side, 4, 4 );
    for (i = 0; i < 4; i++) { unsigned char c[4] = { palette_rgb[i][2], palette_rgb[i][1], palette_rgb[i][0], 0 }; put( b, c, 4 ); }
    for (y = side; y-- > 0;)
    {
        unsigned char row[64] = {0};
        for (x = 0; x < side; x++) row[x / 2] |= pattern( x, y ) << (x & 1 ? 0 : 4);
        put( b, row, stride );
    }
    put_mask( b, side );
}

/* 32 bits with an all-zero alpha channel, as icons made before alpha were. */
static void make_dib32( struct buffer *b, unsigned side, int with_alpha )
{
    unsigned x, y;

    put_dib_header( b, side, 32, 0 );
    for (y = side; y-- > 0;)
        for (x = 0; x < side; x++)
        {
            const unsigned char *c = palette_rgb[pattern( x, y )];
            unsigned char px[4] = { c[2], c[1], c[0], with_alpha ? (x * 255 / (side - 1)) : 0 };
            put( b, px, 4 );
        }
    put_mask( b, side );
}

static void make_dib24( struct buffer *b, unsigned side )
{
    unsigned x, y, stride = (side * 24 + 31) / 32 * 4;

    put_dib_header( b, side, 24, 0 );
    for (y = side; y-- > 0;)
    {
        size_t start = b->size;
        for (x = 0; x < side; x++)
        {
            const unsigned char *c = palette_rgb[pattern( x, y )];
            unsigned char px[3] = { c[2], c[1], c[0] };
            put( b, px, 3 );
        }
        while (b->size - start < stride) put( b, "", 1 );
    }
    put_mask( b, side );
}

static void make_png( struct buffer *b )
{
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                         0, 0, 1, 0, 0, 0, 1, 0, 8, 6, 0, 0, 0, 0x5c, 0x72, 0xa8, 0x66 };
    put( b, png, sizeof(png) );
}

static void make_group( struct buffer *b )
{
    static const struct { unsigned char side; unsigned bits, id; } entries[] = {
        { 16, 4, 1 }, { 32, 32, 2 }, { 0, 32, 3 }, { 48, 24, 4 },
    };
    unsigned i;

    put16( b, 0 ); put16( b, 1 ); put16( b, 4 );
    for (i = 0; i < 4; i++)
    {
        unsigned char head[4] = { entries[i].side, entries[i].side, 0, 0 };
        put( b, head, 4 ); put16( b, 1 ); put16( b, entries[i].bits ); put32( b, 1000 ); put16( b, entries[i].id );
    }
}

static size_t vs_begin( struct buffer *b, const char *key, int text, const unsigned char *value, size_t value_units )
{
    size_t start = b->size;

    put16( b, 0 ); put16( b, value_units ); put16( b, text );
    put_wide( b, key );
    align4( b );
    if (value) put( b, value, text ? value_units * 2 : value_units );
    align4( b );
    return start;
}

static void vs_end( struct buffer *b, size_t start ) { set16( b, start, b->size - start ); align4( b ); }

static void vs_string( struct buffer *b, const char *key, const char *utf16le, size_t chars )
{
    vs_end( b, vs_begin( b, key, 1, (const unsigned char *)utf16le, chars ) );
}

static void make_version( struct buffer *b, int with_description )
{
    unsigned char fixed[52] = { 0xbd, 0x04, 0xef, 0xfe };
    size_t root, info, table;

    root = vs_begin( b, "VS_VERSION_INFO", 0, fixed, sizeof(fixed) );
    info = vs_begin( b, "StringFileInfo", 1, NULL, 0 );
    table = vs_begin( b, "000004b0", 1, NULL, 0 );
    vs_string( b, "FileDescription", "N\0e\0u\0t\0r\0a\0l\0\0\0", 8 );
    vs_end( b, table );
    table = vs_begin( b, "040904b0", 1, NULL, 0 );
    vs_string( b, "CompanyName", "X\0\0\0", 2 );
    /* " Test Program ☃ " with spaces to trim and a character outside ASCII. */
    if (with_description) vs_string( b, "FileDescription", " \0T\0e\0s\0t\0 \0P\0r\0o\0g\0r\0a\0m\0 \0\x03\x26 \0\0\0", 17 );
    vs_string( b, "ProductName", "S\0u\0i\0t\0e\0\0\0", 6 );
    vs_end( b, table );
    vs_end( b, info );
    info = vs_begin( b, "VarFileInfo", 1, NULL, 0 );
    vs_end( b, info );
    vs_end( b, root );
}

struct resource { unsigned type, id; struct buffer *data; };

/* A resource section at rva 0x1000: type, ID and language directories, then data. */
static void make_resources( struct buffer *out, const struct resource *res, unsigned count )
{
    unsigned types[8], type_count = 0, i, j, k;
    size_t type_dir[8], lang_dir[16], leaf[16], blob[16], pos;

    for (i = 0; i < count; i++)
    {
        for (j = 0; j < type_count && types[j] != res[i].type; j++) {}
        if (j == type_count) types[type_count++] = res[i].type;
    }
    pos = 16 + type_count * 8;
    for (j = 0; j < type_count; j++)
    {
        type_dir[j] = pos;
        pos += 16;
        for (i = 0; i < count; i++) if (res[i].type == types[j]) pos += 8;
    }
    for (i = 0; i < count; i++) { lang_dir[i] = pos; pos += 24; }
    for (i = 0; i < count; i++) { leaf[i] = pos; pos += 16; }
    for (i = 0; i < count; i++) { blob[i] = pos; pos = (pos + res[i].data->size + 3) & ~3u; }

    out->size = 0;
    memset( out->data, 0, pos );
    put32( out, 0 ); put32( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, type_count );
    for (j = 0; j < type_count; j++) { put32( out, types[j] ); put32( out, 0x80000000 | type_dir[j] ); }
    for (j = 0; j < type_count; j++)
    {
        unsigned n = 0;
        for (i = 0; i < count; i++) if (res[i].type == types[j]) n++;
        put32( out, 0 ); put32( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, n );
        for (i = 0; i < count; i++)
            if (res[i].type == types[j]) { put32( out, res[i].id ); put32( out, 0x80000000 | lang_dir[i] ); }
    }
    for (i = 0; i < count; i++)
    {
        put32( out, 0 ); put32( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, 0 ); put16( out, 1 );
        put32( out, 0x409 ); put32( out, leaf[i] );
    }
    for (i = 0; i < count; i++) { put32( out, 0x1000 + blob[i] ); put32( out, res[i].data->size ); put32( out, 0 ); put32( out, 0 ); }
    for (i = 0; i < count; i++)
    {
        out->size = blob[i];
        for (k = 0; k < res[i].data->size; k++) out->data[out->size++] = res[i].data->data[k];
    }
    out->size = pos;
}

static void make_pe( struct buffer *pe, const struct buffer *rsrc )
{
    unsigned raw = (rsrc->size + 0x1ff) & ~0x1ffu;

    pe->size = 0;
    memset( pe->data, 0, sizeof(pe->data) );
    put16( pe, 0x5a4d ); pe->size = 0x3c; put32( pe, 64 );
    pe->size = 64;
    put( pe, "PE\0\0", 4 );
    put16( pe, 0x14c ); put16( pe, 1 ); put32( pe, 0 ); put32( pe, 0 ); put32( pe, 0 ); put16( pe, 224 ); put16( pe, 0x102 );
    put16( pe, 0x10b );
    set32( pe, 64 + 24 + 92, 16 );
    set32( pe, 64 + 24 + 96 + 16, 0x1000 );
    set32( pe, 64 + 24 + 96 + 20, rsrc->size );
    pe->size = 64 + 24 + 224;
    put( pe, ".rsrc\0\0\0", 8 ); put32( pe, rsrc->size ); put32( pe, 0x1000 ); put32( pe, raw ); put32( pe, 0x400 );
    pe->size = 0x400;
    put( pe, rsrc->data, rsrc->size );
    pe->size = 0x400 + raw;
}

static void write_file( const char *path, const struct buffer *b )
{
    FILE *f = fopen( path, "wb" );
    assert( f && fwrite( b->data, 1, b->size, f ) == b->size );
    fclose( f );
}

static const unsigned char *pixel( const struct launcher_icon *icon, unsigned x, unsigned y )
{
    return icon->data + ((size_t)y * icon->width + x) * 4;
}

static void check_image( const struct launcher_icon *icon, unsigned side, int alpha_ramp )
{
    unsigned x, y;

    assert( icon->kind == LAUNCHER_ICON_RGBA && icon->width == (int)side && icon->height == (int)side );
    for (y = 0; y < side; y++)
        for (x = 0; x < side; x++)
        {
            const unsigned char *p = pixel( icon, x, y ), *c = palette_rgb[pattern( x, y )];
            unsigned alpha = alpha_ramp ? x * 255 / (side - 1) : masked( x, y, side ) ? 0 : 255;

            assert( p[0] == c[0] && p[1] == c[1] && p[2] == c[2] && p[3] == alpha );
        }
}

static void test_dibs(void)
{
    static struct buffer b;
    struct launcher_icon icon = {0};

    b.size = 0; make_dib4( &b, 16 );
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) ); check_image( &icon, 16, 0 ); launcher_icon_free( &icon );
    b.size = 0; make_dib32( &b, 32, 0 );
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) ); check_image( &icon, 32, 0 ); launcher_icon_free( &icon );
    /* With real alpha the mask is ignored. */
    b.size = 0; make_dib32( &b, 32, 1 );
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) ); check_image( &icon, 32, 1 ); launcher_icon_free( &icon );
    b.size = 0; make_dib24( &b, 48 );
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) ); check_image( &icon, 48, 0 ); launcher_icon_free( &icon );
    /* Without its mask a 24-bit icon is opaque; cut short in its pixels it is refused. */
    b.size -= (48 + 31) / 32 * 4 * 48;
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) && pixel( &icon, 0, 0 )[3] == 255 );
    launcher_icon_free( &icon );
    assert( !launcher_icon_decode_dib( b.data, b.size - 1, &icon ) && icon.kind == LAUNCHER_ICON_NONE );
    /* A height of -2^31 has no positive to turn into: refused, not negated. */
    b.size = 0; make_dib24( &b, 48 );
    b.data[8] = b.data[9] = b.data[10] = 0; b.data[11] = 0x80;
    assert( !launcher_icon_decode_dib( b.data, b.size, &icon ) && icon.kind == LAUNCHER_ICON_NONE );

    /* Fitting 48 pixels into 16 averages 3x3 blocks, ignoring transparent colour. */
    b.size = 0; make_dib24( &b, 48 );
    assert( launcher_icon_decode_dib( b.data, b.size, &icon ) );
    assert( launcher_icon_fit( &icon, 16 ) && icon.width == 16 && icon.height == 16 );
    assert( pixel( &icon, 0, 0 )[3] == 0 && pixel( &icon, 15, 15 )[3] == 255 );
    {
        const unsigned char *p = pixel( &icon, 15, 0 ), *c = palette_rgb[pattern( 45, 0 )];
        /* Rows 0-2 of columns 45-47 hold indexes 3, 0, 1: the average of three colours. */
        assert( p[0] == (palette_rgb[3][0] + palette_rgb[0][0] + palette_rgb[1][0]) / 3 && c == palette_rgb[3] );
    }
    launcher_icon_free( &icon );
}

static void test_pick(void)
{
    static struct buffer g;

    g.size = 0; make_group( &g );
    assert( launcher_icon_pick( g.data, g.size, 128 ) == 3 );
    assert( launcher_icon_pick( g.data, g.size, 40 ) == 4 );
    assert( launcher_icon_pick( g.data, g.size, 32 ) == 2 );
    assert( launcher_icon_pick( g.data, g.size, 8 ) == 1 );
    assert( launcher_icon_pick( g.data, g.size, 600 ) == 3 );
    assert( launcher_icon_pick( g.data, 5, 32 ) == -1 );
    assert( launcher_icon_pick( g.data, 6 + 14, 128 ) == 1 );  /* a count larger than the data */
}

static void build_program( struct buffer *pe, int with_description, int with_icons )
{
    static struct buffer i1, i2, i3, i4, group, version, rsrc;
    struct resource res[6];
    unsigned n = 0;

    i1.size = i2.size = i3.size = i4.size = group.size = version.size = 0;
    make_dib4( &i1, 16 ); make_dib32( &i2, 32, 0 ); make_png( &i3 ); make_dib24( &i4, 48 );
    make_group( &group ); make_version( &version, with_description );
    if (with_icons)
    {
        res[n++] = (struct resource){ 3, 1, &i1 };
        res[n++] = (struct resource){ 3, 2, &i2 };
        res[n++] = (struct resource){ 3, 3, &i3 };
        res[n++] = (struct resource){ 3, 4, &i4 };
        res[n++] = (struct resource){ 14, 101, &group };
    }
    res[n++] = (struct resource){ 16, 1, &version };
    make_resources( &rsrc, res, n );
    make_pe( pe, &rsrc );
}

static void test_program( const char *dir )
{
    static struct buffer pe;
    struct launcher_icon icon;
    char path[512], title[128];

    snprintf( path, sizeof(path), "%s/program.exe", dir );
    build_program( &pe, 1, 1 );
    write_file( path, &pe );

    assert( launcher_pe_describe( path, 128, &icon, title, sizeof(title) ) );
    assert( !strcmp( title, "Test Program \xe2\x98\x83" ) );
    assert( icon.kind == LAUNCHER_ICON_PNG && icon.width == 256 && icon.height == 256 && icon.size == 33 );
    launcher_icon_free( &icon );
    assert( launcher_pe_describe( path, 32, &icon, title, sizeof(title) ) );
    check_image( &icon, 32, 0 );
    launcher_icon_free( &icon );
    assert( launcher_pe_describe( path, 16, &icon, title, 8 ) && !strcmp( title, "Test Pr" ) );
    check_image( &icon, 16, 0 );
    launcher_icon_free( &icon );

    /* The US English table's product name wins over the neutral table's description. */
    build_program( &pe, 0, 0 );
    write_file( path, &pe );
    assert( launcher_pe_describe( path, 32, &icon, title, sizeof(title) ) );
    assert( icon.kind == LAUNCHER_ICON_NONE && !strcmp( title, "Suite" ) );

    /* Not a program: no icon, no title. */
    pe.size = 100;
    write_file( path, &pe );
    assert( !launcher_pe_describe( path, 32, &icon, title, sizeof(title) ) && !title[0] );
    snprintf( path, sizeof(path), "%s/missing.exe", dir );
    assert( !launcher_pe_describe( path, 32, &icon, title, sizeof(title) ) );
}

/* Change a few random bytes of the program many times; the reader must never
 * read outside what it loaded (ASan) or loop. */
static void test_damaged( const char *dir )
{
    static struct buffer pe, damaged;
    struct launcher_icon icon;
    char path[512], title[64];
    unsigned i, j, described = 0;

    snprintf( path, sizeof(path), "%s/damaged.exe", dir );
    build_program( &pe, 1, 1 );
    srand( 7 );
    for (i = 0; i < 4000; i++)
    {
        damaged = pe;
        for (j = 0, damaged.size = pe.size; j < 1 + (unsigned)rand() % 8; j++)
        {
            /* Mostly the headers and resource directories, where the offsets are. */
            size_t at = rand() % 3 ? (size_t)(rand() % 0x200) : (size_t)(0x400 + rand() % 0x200);
            damaged.data[at] = rand() % 4 ? rand() : 0xff;
        }
        if (i % 5 == 0) damaged.size = rand() % pe.size;
        write_file( path, &damaged );
        if (launcher_pe_describe( path, 1 + rand() % 300, &icon, title, sizeof(title) )) described++;
        if (icon.kind == LAUNCHER_ICON_RGBA)
            assert( icon.width > 0 && icon.width <= LAUNCHER_ICON_MAX_SIDE && icon.size == (size_t)icon.width * icon.height * 4 );
        launcher_icon_free( &icon );
    }
    printf( "damaged programs: %u of 4000 still read as programs\n", described );
}

static void test_real( const char *path, int want_title )
{
    struct launcher_icon icon;
    char title[128];

    if (access( path, R_OK )) { printf( "skipped %s (not staged)\n", path ); return; }
    assert( launcher_pe_describe( path, 128, &icon, title, sizeof(title) ) );
    printf( "%s: title '%s', icon %s %dx%d\n", path, title,
            icon.kind == LAUNCHER_ICON_PNG ? "PNG" : icon.kind == LAUNCHER_ICON_RGBA ? "RGBA" : "none",
            icon.width, icon.height );
    assert( icon.kind != LAUNCHER_ICON_NONE );
    if (want_title) assert( title[0] );
    launcher_icon_free( &icon );
}

int main( int argc, char **argv )
{
    char dir[] = "/tmp/launcher-pe.XXXXXX";
    char path[512];

    assert( mkdtemp( dir ) );
    test_dibs();
    test_pick();
    test_program( dir );
    test_damaged( dir );
    if (argc > 1)
    {
        snprintf( path, sizeof(path), "%s/openttd/openttd.exe", argv[1] );
        test_real( path, 1 );
        snprintf( path, sizeof(path), "%s/notepad.exe", argv[1] );
        test_real( path, 0 );
    }
    snprintf( path, sizeof(path), "rm -rf '%s'", dir );
    system( path );
    printf( "launcher_pe: all tests passed\n" );
    return 0;
}
