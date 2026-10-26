/* Host test of the controls overlay (source/controls_overlay.c): what its
 * rows, grid and capture do to a program's controls, and its picture (written
 * as PNGs to the folder given, if any).
 *   clang -std=gnu11 -I source $(sdl2-config --cflags) \
 *       tests/controls_overlay.c source/controls_overlay.c source/input_profile.c \
 *       $(sdl2-config --libs) -lSDL2_ttf -lpng -lm
 */
#include <assert.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "controls_overlay.h"
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
    free( font_data );
    font_data = malloc( length );
    if (fread( font_data, 1, length, file ) != (size_t)length) length = 0;
    fclose( file );
    *data = font_data;
    *size = length;
    return length > 0;
}

/* The floating keyboard is not linked here: the overlay only hides it when it
 * opens, which the runtime does beside it. */
void wine_nx_osk_show( int show, unsigned int held )
{
    (void)show; (void)held;
}

/* The runtime's profile store is not linked either: record what the overlay
 * hands over, which is how remapping reaches a running game. */
static struct input_profile published;
void wine_nx_input_profile_publish( const struct input_profile *profile )
{
    published = *profile;
}

#define MS 1000000ull
static uint64_t now = 1000 * MS;

/* One reading, then 16 ms later. */
static void step( unsigned int held )
{
    wine_nx_controls_overlay_input( held, 0, 0, 0, 0, 0, 0, 0, now );
    now += 16 * MS;
}

static void press( unsigned int button )
{
    step( button );
    step( 0 );
}

static int file_has( const char *root, const char *name, const char *text )
{
    char path[768], buffer[8192];
    FILE *file;
    size_t got;

    snprintf( path, sizeof(path), "%s/%s", root, name );
    file = fopen( path, "rb" );
    if (!file) return 0;
    got = fread( buffer, 1, sizeof(buffer) - 1, file );
    fclose( file );
    buffer[got] = 0;
    return strstr( buffer, text ) != NULL;
}

static void save( const char *dir, const char *name )
{
    struct wine_nx_osk_frame frame;
    png_image image = {0};
    char path[512];
    void *pixels;

    assert( wine_nx_controls_overlay_frame( 1280, 720, &frame ) );
    pixels = malloc( (size_t)frame.width * frame.height * 4 );
    assert( wine_nx_controls_overlay_copy( 1280, 720, pixels, frame.width * 4, 0 ) == frame.generation );
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
    char root[] = "/tmp/autorun-overlay-XXXXXX", game[768], settings[768];
    struct wine_nx_osk_frame frame, again;
    FILE *file;
    int i;

    font_path = argc > 1 ? argv[1] : "/System/Library/Fonts/Supplemental/Arial.ttf";
    assert( mkdtemp( root ) );
    snprintf( game, sizeof(game), "%s/septerra.exe", root );
    snprintf( settings, sizeof(settings), "%s/septerra.wine-nx.txt", root );

    /* Hidden, it reads nothing and draws nothing. */
    assert( !wine_nx_controls_overlay_visible() );
    assert( !wine_nx_controls_overlay_input( OSK_A, 0, 0, 0, 0, 0, 0, 0, now ) );
    assert( !wine_nx_controls_overlay_frame( 1280, 720, &frame ) );

    /* It opens over Septerra Core with its recommended layout: ZL left-clicks
     * and the right stick's click right-clicks, in keyboard/mouse mode. */
    assert( wine_nx_controls_overlay_show( 1, 0, root, game ) );
    assert( wine_nx_controls_overlay_visible() );
    assert( wine_nx_controls_overlay_selected() == 0 );
    assert( wine_nx_controls_overlay_frame( 1280, 720, &frame ) );
    assert( frame.width == 800 && frame.height == 445 && frame.x == 240 && frame.y == 137 );
    assert( frame.generation & 0x80000000u );  /* the keyboard's never are */
    assert( wine_nx_controls_overlay_frame( 1280, 720, &again ) && again.generation == frame.generation );
    {
        void *pixels = malloc( (size_t)frame.width * frame.height * 4 );

        assert( wine_nx_controls_overlay_copy( 1280, 720, pixels, frame.width * 4, 0 ) == frame.generation );
        free( pixels );
    }
    save( dir, "controls-list.png" );

    /* The d-pad and the left stick walk the rows; L and R page. */
    press( OSK_DOWN );
    press( OSK_DOWN );
    press( OSK_DOWN );
    assert( wine_nx_controls_overlay_selected() == 3 );
    press( OSK_L );
    assert( wine_nx_controls_overlay_selected() == 0 );
    wine_nx_controls_overlay_input( 0, 0, -30000, 0, 0, 0, 0, 0, now );
    now += 16 * MS;
    step( 0 );
    assert( wine_nx_controls_overlay_selected() == 1 );
    press( OSK_UP );
    assert( wine_nx_controls_overlay_selected() == 0 );

    /* A opens the row; the grid adds and takes away chord pieces, and B takes
     * the chord back to the program's controls at once. A began as the game's
     * left click; it leaves as a right click. */
    press( OSK_A );
    save( dir, "controls-pick.png" );
    press( OSK_A );                 /* Left click: out */
    press( OSK_RIGHT );
    press( OSK_A );                 /* Right click: in */
    press( OSK_B );
    assert( file_has( root, "septerra.wine-nx.txt", "key.A=0x101" ) );

    /* X clears the row to nothing, Y clears the row to the game's layout. */
    press( OSK_X );
    assert( file_has( root, "septerra.wine-nx.txt", "key.A=none" ) );
    press( OSK_Y );
    assert( !file_has( root, "septerra.wine-nx.txt", "key.A" ) );
    assert( wine_nx_controls_overlay_visible() );

    /* The row under the bindings is the mouse speed. Its picker sets the
     * speed at once -- into the running game as well -- and Y gives the
     * game's own back. */
    for (i = 0; i < 24; i++) press( OSK_DOWN );
    assert( wine_nx_controls_overlay_selected() == 24 );
    press( OSK_A );
    save( dir, "controls-speed.png" );
    press( OSK_RIGHT );
    press( OSK_A );                 /* 150% -> 200% */
    assert( file_has( root, "septerra.wine-nx.txt", "mouse-speed=200" ) );
    assert( published.mouse_speed == 200 );
    press( OSK_A );
    press( OSK_Y );                 /* the game's own again */
    assert( !file_has( root, "septerra.wine-nx.txt", "mouse-speed" ) );

    /* B closes it; it opens again on the same file. Buttons held while it
     * opens are the opening itself, not a press. */
    press( OSK_B );
    assert( !wine_nx_controls_overlay_visible() );
    assert( !wine_nx_controls_overlay_frame( 1280, 720, &frame ) );
    assert( wine_nx_controls_overlay_show( 1, OSK_DOWN | OSK_A, root, game ) );
    step( OSK_DOWN | OSK_A );
    step( 0 );
    assert( wine_nx_controls_overlay_selected() == 0 );
    press( OSK_DOWN );
    assert( wine_nx_controls_overlay_selected() == 1 );

    /* A tap opens the row under the finger: the second row, on a 1280x720
     * screen with the list in the middle. */
    wine_nx_controls_overlay_input( 0, 0, 0, 0, 0, 1, 640, 234, now );
    now += 16 * MS;
    wine_nx_controls_overlay_input( 0, 0, 0, 0, 0, 0, 0, 0, now );
    now += 16 * MS;
    assert( wine_nx_controls_overlay_selected() == 1 );
    press( OSK_B );                 /* out of the row's grid */
    press( OSK_B );                 /* and out of the overlay */

    /* In controller mode the row captures what is pressed: ZL, six rows down,
     * becomes R when R is pressed. */
    file = fopen( settings, "wb" );
    assert( file );
    fputs( "input-mode=1\n", file );
    fclose( file );
    assert( wine_nx_controls_overlay_show( 1, 0, root, game ) );
    for (i = 0; i < 6; i++) press( OSK_DOWN );
    assert( wine_nx_controls_overlay_selected() == 6 );
    press( OSK_A );
    save( dir, "controls-capture.png" );
    press( OSK_R );
    assert( wine_nx_controls_overlay_selected() == 6 );
    assert( file_has( root, "septerra.wine-nx.txt", "pad.ZL=0x6" ) );
    press( OSK_MINUS );
    assert( !wine_nx_controls_overlay_visible() );

    wine_nx_controls_overlay_reset();
    assert( !unlink( settings ) );
    assert( !rmdir( root ) );
    free( font_data );
    puts( "Controls overlay: rows, grid chords, clear and reset, capture and picture passed" );
    return 0;
}
