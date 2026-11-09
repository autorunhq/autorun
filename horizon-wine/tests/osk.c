/* Host test of the floating keyboard (source/osk.c): what a key sends while
 * it is held and when it is let go, what several fingers at once send, and its
 * picture (written as PNGs to the folder given, if any).
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
    wine_nx_osk_input( held, 0, 0, NULL, 0, now );
    now += 16 * MS;
}

/* The fingers down, for one reading. */
static void step_fingers( unsigned int held, const struct wine_nx_osk_point *fingers, int count )
{
    wine_nx_osk_input( held, 0, 0, fingers, count, now );
    now += 16 * MS;
}

/* A button pressed and let go, a frame apart. */
static void press( unsigned int button )
{
    step( button );
    step( 0 );
}

/* A key of the picture at 720p: 64 px a unit, 50 px a key, 6 px between them,
 * 12 px of padding and 30 px of hint above the rows, the keyboard at the
 * bottom. Only the keys these tests press. */
static void key_point( const struct wine_nx_osk_frame *frame, int row, int col, int *x, int *y )
{
    int centers[][3] =
    {
        { 0, 4, 372 },   /* F5 */
        { 1, 1, 44 },    /* 1 */
        { 2, 1, 140 },   /* q */
        { 3, 0, 68 },    /* Caps */
        { 4, 0, 84 },    /* Shift */
        { 4, 5, 444 },   /* b */
        { 5, 1, 300 },   /* Space */
    };
    int i;

    for (i = 0; i < (int)(sizeof(centers) / sizeof(centers[0])); i++)
        if (centers[i][0] == row && centers[i][1] == col)
        {
            *x = frame->x + centers[i][2];
            *y = frame->y + 12 + 30 + row * 56 + 25;
            return;
        }
    assert( !"no such key in this test" );
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
    struct wine_nx_osk_point fingers[2];
    unsigned int before;
    uint8_t bgra[4], rgba[4];
    int i, shift_x, shift_y, q_x, q_y;

    font_path = argc > 1 ? argv[1] : "/System/Library/Fonts/Supplemental/Arial.ttf";

    /* Hidden, it reads nothing and draws nothing. */
    assert( !wine_nx_osk_visible() && !wine_nx_osk_input( OSK_A, 0, 0, NULL, 0, now ) );
    assert( !wine_nx_osk_frame( 1280, 720, &frame ) );
    expect( "" );

    /* Opened with Minus and the right stick held: those do nothing until let
     * go, so opening it does not close it again. */
    wine_nx_osk_show( 1, OSK_MINUS | OSK_STICKR );
    step( OSK_MINUS | OSK_STICKR );
    step( OSK_MINUS );
    step( 0 );
    assert( wine_nx_osk_visible() );
    expect( "" );

    /* It starts on g. A holds it down while it is pressed and lets it go
     * when it comes up: the key alone, lower case. */
    press( OSK_A );
    expect( "+47 -47" );

    /* The function keys are the top row, as they are on a keyboard: three up
     * from g is it, and the key above g is F5. */
    press( OSK_UP );
    press( OSK_UP );
    press( OSK_UP );
    press( OSK_A );
    expect( "+74 -74" );

    /* Back to g. */
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );

    /* X is Shift. Let go at once, it stays down for the key after it (sticky):
     * the key comes between its down and its up, as it would from a hand. */
    press( OSK_X );
    expect( "+10" );
    press( OSK_A );
    expect( "+47 -47 -10" );

    /* Pressed again, it lets go: a second tap leaves Shift off, and the key
     * after it goes out unshifted. */
    press( OSK_X );
    expect( "+10" );
    press( OSK_X );
    expect( "-10" );
    press( OSK_A );
    expect( "+47 -47" );

    /* A finger holds Shift while another presses a key: the two are down at
     * once, Shift first, and it goes up after the key. */
    assert( wine_nx_osk_frame( 1280, 720, &frame ) );
    key_point( &frame, 4, 0, &shift_x, &shift_y );
    key_point( &frame, 2, 1, &q_x, &q_y );
    fingers[0] = (struct wine_nx_osk_point){ shift_x, shift_y };
    assert( wine_nx_osk_input( 0, 0, 0, fingers, 1, now ) );
    now += 16 * MS;
    expect( "+10" );
    fingers[1] = (struct wine_nx_osk_point){ q_x, q_y };
    step_fingers( 0, fingers, 2 );
    expect( "+51" );
    /* Both let go: the key first, Shift after it. */
    step_fingers( 0, NULL, 0 );
    expect( "-51 -10" );
    now += 100 * MS;

    /* A function key by finger, held and let go like any other. */
    {
        int f_x, f_y;

        key_point( &frame, 0, 4, &f_x, &f_y );
        fingers[0] = (struct wine_nx_osk_point){ f_x, f_y };
    }
    step_fingers( 0, fingers, 1 );
    expect( "+74" );
    step_fingers( 0, NULL, 0 );
    expect( "-74" );
    now += 100 * MS;

    /* Two fingers, two keys at once: both down and both up, as a hand holds
     * two keys, so keys that must be held together work. */
    {
        int b_x, b_y;

        key_point( &frame, 4, 5, &b_x, &b_y );
        fingers[0] = (struct wine_nx_osk_point){ q_x, q_y };
        fingers[1] = (struct wine_nx_osk_point){ b_x, b_y };
        step_fingers( 0, fingers, 2 );
        expect( "+51 +42" );
        step_fingers( 0, NULL, 0 );
        expect( "-51 -42" );
        now += 100 * MS;
    }

    /* A finger on Shift and off at once, with nothing typed, leaves it down
     * for the next key, as X does; the next key takes it up. */
    fingers[0] = (struct wine_nx_osk_point){ shift_x, shift_y };
    step_fingers( 0, fingers, 1 );
    step_fingers( 0, NULL, 0 );
    expect( "+10" );
    fingers[0] = (struct wine_nx_osk_point){ q_x, q_y };
    step_fingers( 0, fingers, 1 );
    expect( "+51" );
    step_fingers( 0, NULL, 0 );
    expect( "-51 -10" );
    now += 100 * MS;
    expect( "" );

    /* From here the controller: the keyboard starts again on g. */
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );

    /* Caps is VK_CAPITAL: the lock is the system's, not the keyboard's. Left
     * from g five times is Caps. */
    for (i = 0; i < 5; i++) press( OSK_LEFT );
    expect( "" );
    press( OSK_A );
    expect( "+14 -14" );
    /* With the lock on, a letter goes out as itself: the system makes it a
     * capital, as it does for a keyboard. */
    for (i = 0; i < 5; i++) press( OSK_RIGHT );
    press( OSK_A );
    expect( "+47 -47" );

    /* A key held repeats: the key stays down and another down goes out every
     * 60 ms after the first 500, which is what a held key of a keyboard
     * does. Here B, held by the controller. */
    {
        const char *got;
        uint64_t t = now;
        int i, downs = 0;

        wine_nx_osk_input( OSK_B, 0, 0, NULL, 0, t );
        expect( "+08" );
        /* Held, it is quiet for the first half second. */
        wine_nx_osk_input( OSK_B, 0, 0, NULL, 0, t + 400 * MS );
        expect( "" );
        /* Then a down every 60 ms, and the key never goes up while it is
         * held: the program sees one key down, repeated. */
        for (i = 0; i < 100; i++)
            wine_nx_osk_input( OSK_B, 0, 0, NULL, 0, t + (400 + i * 10) * MS );
        got = sent();
        assert( !strstr( got, "-08" ) );
        for (i = 0; got[i]; )
            if (!strncmp( got + i, "+08", 3 )) { downs++; i += 3; }
            else i++;
        assert( downs >= 5 );
        now = t + 1500 * MS;
        step( 0 );
        expect( "-08" );
    }

    /* Every key the controller presses is held while its button is: B is
     * Backspace, Y space, L and R the arrows, + Enter. */
    step( OSK_Y );
    step( 0 );
    expect( "+20 -20" );
    step( OSK_L );
    step( OSK_R );
    step( 0 );
    expect( "+25 +27 -25 -27" );
    step( OSK_PLUS );
    step( 0 );
    expect( "+0d -0d" );

    /* Closing lets go of everything that is down: nothing is left held. */
    step( OSK_B );
    expect( "+08" );
    press( OSK_MINUS );
    expect( "-08" );
    assert( !wine_nx_osk_visible() );

    /* The picture: ZL puts it at the top, ZR back at the bottom. */
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );
    assert( wine_nx_osk_frame( 1280, 720, &again ) && again.y == 324 && again.x == 148 );
    before = again.generation;
    press( OSK_ZL );
    assert( wine_nx_osk_frame( 1280, 720, &frame ) && frame.y == 12 && frame.generation != before );
    save( dir, "osk-720-top.png", 1280, 720 );
    press( OSK_ZR );
    save( dir, "osk-720.png", 1280, 720 );

    /* A key that is down lights up, and so does the one the selection is on. */
    assert( wine_nx_osk_frame( 1280, 720, &frame ) );
    {
        void *held = malloc( (size_t)frame.width * frame.height * 4 );
        uint32_t *pixels = held;

        assert( wine_nx_osk_copy( 1280, 720, held, frame.width * 4, 0 ) == frame.generation );
        key_point( &frame, 4, 0, &shift_x, &shift_y );
        before = pixels[(shift_y - frame.y) * frame.width + shift_x - frame.x];
        fingers[0] = (struct wine_nx_osk_point){ shift_x, shift_y };
        step_fingers( 0, fingers, 1 );
        assert( wine_nx_osk_copy( 1280, 720, held, frame.width * 4, 0 ) );
        assert( pixels[(shift_y - frame.y) * frame.width + shift_x - frame.x] != before );
        step_fingers( 0, NULL, 0 );
        free( held );
    }

    /* Docked, the same keyboard, larger; the picture comes in either order. */
    assert( wine_nx_osk_frame( 1920, 1080, &frame ) && frame.width == 1476 && frame.height == 576 );
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

    /* A finger that was down when the keyboard opened presses nothing until
     * it lifts, as a button held when it opened does. */
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );
    assert( wine_nx_osk_frame( 1280, 720, &frame ) );
    key_point( &frame, 2, 1, &q_x, &q_y );
    fingers[0] = (struct wine_nx_osk_point){ q_x, q_y };
    /* The finger is still the keyboard's, not the program's. */
    assert( wine_nx_osk_input( 0, 0, 0, fingers, 1, now ) );
    now += 16 * MS;
    expect( "" );
    step_fingers( 0, NULL, 0 );
    expect( "" );
    step_fingers( 0, fingers, 1 );
    expect( "+51" );
    step_fingers( 0, NULL, 0 );
    expect( "-51" );

    /* Close, the key itself: down from g is v, then Space; left is Esc, and
     * left again wraps round to Close. */
    wine_nx_osk_reset();
    wine_nx_osk_show( 1, 0 );
    press( OSK_DOWN );
    press( OSK_DOWN );
    press( OSK_LEFT );
    press( OSK_LEFT );
    press( OSK_A );
    assert( !wine_nx_osk_visible() );
    expect( "" );

    /* The controls panel: what the controller sends, over the program. It is
     * drawn where it says, at either size, and the keyboard takes the screen
     * from it: they are never up at once. */
    {
        static const struct wine_nx_osk_map_row rows[] =
        {
            { "A", "Left click" }, { "B", "Right click" }, { "ZR", "Left click" },
            { "D-pad up", "Arrow up" }, { "Left stick up", "W" }, { "Right stick up", "Mouse up" },
        };
        struct wine_nx_osk_frame panel;

        wine_nx_osk_map_show( 1, "Controls: the keyboard and mouse it gets", rows, 6 );
        assert( wine_nx_osk_map_visible() && !wine_nx_osk_visible() );
        assert( wine_nx_osk_frame( 1280, 720, &panel ) );
        assert( panel.width > 0 && panel.height > 0 && panel.x >= 0 && panel.y >= 0 );
        assert( panel.x + panel.width <= 1280 && panel.y + panel.height <= 720 );
        save( dir, "controls-720.png", 1280, 720 );
        assert( wine_nx_osk_frame( 1920, 1080, &panel ) );
        assert( panel.x + panel.width <= 1920 && panel.y + panel.height <= 1080 );
        save( dir, "controls-1080.png", 1920, 1080 );

        /* The keyboard takes the screen from it. */
        wine_nx_osk_show( 1, 0 );
        assert( wine_nx_osk_visible() && !wine_nx_osk_map_visible() );
        assert( wine_nx_osk_frame( 1280, 720, &panel ) && panel.width == 984 );
        wine_nx_osk_show( 0, 0 );
        assert( !wine_nx_osk_map_visible() );

        /* Put up again and taken down, the picture goes with it. */
        wine_nx_osk_map_show( 1, "Controls: the keyboard and mouse it gets", rows, 6 );
        before = wine_nx_osk_generation();
        wine_nx_osk_map_show( 0, NULL, NULL, 0 );
        assert( !wine_nx_osk_map_visible() && wine_nx_osk_generation() != before );
        assert( !wine_nx_osk_frame( 1280, 720, &panel ) );
        wine_nx_osk_reset();
        assert( !wine_nx_osk_map_visible() && !wine_nx_osk_visible() );
    }

    printf( "osk: keys held and let go, function keys, two keys at once, Shift held and "
            "sticky, Caps, repeats, closing, placement, the picture at 720p and 1080p, "
            "and the controls panel\n" );
    return 0;
}
