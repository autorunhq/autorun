/* Host test of the floating keyboard (source/osk.c): what each button and tap
 * sends, and its picture (written as PNGs to the folder given, if any).
 *   clang -std=gnu11 -I source $(sdl2-config --cflags) tests/osk.c source/osk.c $(sdl2-config --libs) -lSDL2_ttf -lpng
 */
#include <assert.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osk.h"

static const char *font_path;
static void *font_data;

int wine_nx_osk_font( const void **data, size_t *size )
{
    FILE *file = fopen( font_path, "rb" );
    long length;

    if (!file) return 0;
    fseek( file, 0, SEEK_END );
    length = ftell( file );
    fseek( file, 0, SEEK_SET );
    font_data = malloc( length );
    if (fread( font_data, 1, length, file ) != (size_t)length) length = 0;
    fclose( file );
    *data = font_data;
    *size = length;
    return length > 0;
}

#define MS 1000000ull
static uint64_t now = 1000 * MS;

/* One reading, then 16 ms later. */
static void step( unsigned int held )
{
    wine_nx_osk_input( held, 0, 0, 0, 0, 0, now );
    now += 16 * MS;
}

static void press( unsigned int button )
{
    step( button );
    step( 0 );
}

/* The keys queued since the last call, as "+VK -VK ..." in hex, however far
 * off they are due. */
static const char *sent( void )
{
    static char text[1024];
    unsigned short vk;
    int up, n = 0;

    text[0] = 0;
    while (wine_nx_osk_next_key( ~0ull, &vk, &up ))
        n += snprintf( text + n, sizeof(text) - n, "%s%c%02x", n ? " " : "", up ? '-' : '+', vk );
    return text;
}

static void expect( const char *want )
{
    const char *got = sent();
    if (strcmp( got, want )) { fprintf( stderr, "sent '%s', expected '%s'\n", got, want ); exit( 1 ); }
}

static void save( const char *dir, const char *name, int screen_w, int screen_h )
{
    struct wine_nx_osk_frame frame;
    png_image image = {0};
    char path[512];
    void *pixels;

    assert( wine_nx_osk_frame( screen_w, screen_h, &frame ) );
    pixels = malloc( (size_t)frame.width * frame.height * 4 );
    assert( wine_nx_osk_copy( screen_w, screen_h, pixels, frame.width * 4, 0 ) == frame.generation );
    if (dir)
    {
        snprintf( path, sizeof(path), "%s/%s", dir, name );
        image.version = PNG_IMAGE_VERSION;
        image.width = frame.width;
        image.height = frame.height;
        image.format = PNG_FORMAT_BGRA;
        assert( png_image_write_to_file( &image, path, 0, pixels, 0, NULL ) );
    }
    free( pixels );
}

int main( int argc, char **argv )
{
    const char *dir = argc > 2 ? argv[2] : NULL;
    struct wine_nx_osk_frame frame, again;
    unsigned int before;
    uint8_t bgra[4], rgba[4];
    int i;

    font_path = argc > 1 ? argv[1] : "/System/Library/Fonts/Supplemental/Arial.ttf";

    /* Hidden, it reads nothing and draws nothing. */
    assert( !wine_nx_osk_visible() && !wine_nx_osk_input( OSK_A, 0, 0, 0, 0, 0, now ) );
    assert( !wine_nx_osk_frame( 1280, 720, &frame ) );
    expect( "" );

    /* Opened with Minus and the right stick held: those do nothing until let
     * go, so opening it does not close it again. */
    wine_nx_osk_show( 1, OSK_MINUS | OSK_STICKR );
    step( OSK_MINUS | OSK_STICKR );
    step( OSK_MINUS );
    step( 0 );
    assert( wine_nx_osk_visible() );

    /* It starts on g. A types it, lower case: the key alone. */
    press( OSK_A );
    expect( "+47 -47" );
    /* X is Shift for one character, held around it. */
    press( OSK_X );
    press( OSK_A );
    press( OSK_A );
    expect( "+10 +47 -47 -10 +47 -47" );
    /* Each key comes when it is due, as a finger holds keys: Shift at the
     * press, the key 20 ms later and held 40 ms, Shift let go 20 ms after;
     * the next key 10 ms after that, held 40 ms. */
    {
        uint64_t t;
        unsigned short vk;
        int up;

        now += 500 * MS;  /* the keys above have all gone by */
        press( OSK_X );
        t = now;
        press( OSK_A );
        press( OSK_A );
        assert( wine_nx_osk_next_key( t, &vk, &up ) && vk == 0x10 && !up );
        assert( !wine_nx_osk_next_key( t + 19 * MS, &vk, &up ) );
        assert( wine_nx_osk_next_key( t + 20 * MS, &vk, &up ) && vk == 0x47 && !up );
        assert( !wine_nx_osk_next_key( t + 59 * MS, &vk, &up ) );
        assert( wine_nx_osk_next_key( t + 60 * MS, &vk, &up ) && vk == 0x47 && up );
        assert( wine_nx_osk_next_key( t + 80 * MS, &vk, &up ) && vk == 0x10 && up );
        assert( !wine_nx_osk_next_key( t + 89 * MS, &vk, &up ) );
        assert( wine_nx_osk_next_key( t + 90 * MS, &vk, &up ) && vk == 0x47 && !up );
        assert( !wine_nx_osk_next_key( t + 129 * MS, &vk, &up ) );
        expect( "-47" );
        now += 200 * MS;
    }
    /* Up from g is t, right is y; B is Backspace, Y space, L and R the arrows,
     * + Enter. */
    press( OSK_UP );
    press( OSK_RIGHT );
    press( OSK_A );
    press( OSK_B );
    press( OSK_Y );
    press( OSK_L );
    press( OSK_R );
    press( OSK_PLUS );
    expect( "+59 -59 +08 -08 +20 -20 +25 -25 +27 -27 +0d -0d" );

    /* Held, B repeats: once at once, again after 350 ms, then every 70 ms. */
    wine_nx_osk_input( OSK_B, 0, 0, 0, 0, 0, now );
    expect( "+08 -08" );
    wine_nx_osk_input( OSK_B, 0, 0, 0, 0, 0, now + 349 * MS );
    expect( "" );
    wine_nx_osk_input( OSK_B, 0, 0, 0, 0, 0, now + 350 * MS );
    expect( "+08 -08" );
    wine_nx_osk_input( OSK_B, 0, 0, 0, 0, 0, now + 420 * MS );
    expect( "+08 -08" );
    now += 500 * MS;
    step( 0 );

    /* The left stick moves the selection like the d-pad: down from y is h. */
    wine_nx_osk_input( 0, 0, -30000, 0, 0, 0, now );
    now += 16 * MS;
    step( 0 );
    press( OSK_A );
    expect( "+48 -48" );

    /* Caps makes letters capitals without Shift; Shift then makes them small
     * again, and a digit still takes Shift. Up from h goes to y, up again
     * to 6. */
    for (i = 0; i < 6; i++) press( OSK_LEFT );   /* h g f d s a Caps */
    press( OSK_A );
    expect( "" );
    for (i = 0; i < 6; i++) press( OSK_RIGHT );  /* back to h */
    press( OSK_A );
    press( OSK_X );
    press( OSK_A );
    press( OSK_UP );
    press( OSK_UP );
    press( OSK_X );
    press( OSK_A );
    expect( "+10 +48 -48 -10 +48 -48 +10 +36 -36 -10" );

    /* A tap types the key under the finger when it comes up: q, on a 720p
     * screen with the keyboard at the bottom. Caps is still on: a capital. */
    assert( wine_nx_osk_frame( 1280, 720, &frame ) );
    assert( frame.width == 984 && frame.height == 328 && frame.x == 148 && frame.y == 380 );
    {
        int qx = frame.x + 12 + 96 + 32, qy = frame.y + 12 + 30 + 56 + 25;  /* after Tab, one row down */

        assert( wine_nx_osk_input( 0, 0, 0, 1, qx, qy, now ) );
        expect( "" );
        assert( !wine_nx_osk_input( 0, 0, 0, 0, 0, 0, now + 50 * MS ) );
        expect( "+10 +51 -51 -10" );
        /* Off the keyboard the finger is the program's. */
        assert( !wine_nx_osk_input( 0, 0, 0, 1, 640, 100, now + 100 * MS ) );
        assert( !wine_nx_osk_input( 0, 0, 0, 0, 0, 0, now + 150 * MS ) );
        expect( "" );
        now += 200 * MS;
    }

    /* ZL puts it at the top, ZR back at the bottom; the picture changes. */
    before = frame.generation;
    press( OSK_ZL );
    assert( wine_nx_osk_frame( 1280, 720, &again ) && again.y == 12 && again.generation != before );
    save( dir, "osk-720-top.png", 1280, 720 );
    press( OSK_ZR );
    save( dir, "osk-720.png", 1280, 720 );

    /* Docked, the same keyboard, larger; the picture comes in either order. */
    assert( wine_nx_osk_frame( 1920, 1080, &frame ) && frame.width == 1476 && frame.height == 492 );
    save( dir, "osk-1080.png", 1920, 1080 );
    {
        void *pixels = malloc( (size_t)frame.width * frame.height * 4 );

        wine_nx_osk_copy( 1920, 1080, pixels, frame.width * 4, 0 );
        memcpy( bgra, pixels, 4 );
        wine_nx_osk_copy( 1920, 1080, pixels, frame.width * 4, 1 );
        memcpy( rgba, pixels, 4 );
        assert( bgra[0] == rgba[2] && bgra[1] == rgba[1] && bgra[2] == rgba[0] && bgra[3] == 0xff );
        free( pixels );
    }

    /* Minus closes it, and so does the Close key: down from g is v, then
     * Space; left is Esc, and left again wraps round to Close. */
    press( OSK_MINUS );
    assert( !wine_nx_osk_visible() );
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );
    press( OSK_DOWN );
    press( OSK_DOWN );
    press( OSK_LEFT );
    press( OSK_LEFT );
    press( OSK_A );
    assert( !wine_nx_osk_visible() );
    expect( "" );
    printf( "osk: keys, Shift and Caps, repeats, taps, placement and the picture at 720p and 1080p\n" );
    return 0;
}
