/*
 * Host run of the launcher (source/launcher.c) with SDL on the Mac, to see
 * every screen and check its actions without a Switch.
 *
 * Run it in a folder whose "sdmc:" holds switch/wine/drive_c (paths on the card
 * are relative there). A script of steps drives it, one step per line:
 *   key NAME    press a key (up, down, left, right, a, b, x, y, plus, minus, l, r, zl, zr)
 *   trigger NAME VALUE    send a controller trigger axis event (zl or zr, 0 to 32767)
 *   tap X Y     tap the touch screen
 *   wait N      let N frames pass
 *   shot FILE   save the next frame as a PNG
 *   type TEXT   the text the next keyboard prompt returns
 *
 * Build (see check-launcher-host.sh):
 *   clang -std=gnu11 -I source $(sdl2-config --cflags) tests/launcher_host.c source/launcher.c
 *     source/launcher_ui.c $(sdl2-config --libs) -lSDL2_ttf -lpng
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <sys/stat.h>

#include <png.h>

#include "launcher.h"
#include "launcher_catalog.h"
#include "launcher_ui.h"
#include "launcher_update.h"
#include "dxvk_releases.h"

static char script[256][300];
static int script_count, script_pos, wait_frames, frames;
static char prompt_text[512];
static int prompt_set;
static const char *font_path;
static unsigned char *font_data;

struct launcher_update *launcher_update_create( struct ui *ui, const char *root, int (*restart)(void) )
{
    (void)ui; (void)root; (void)restart;
    return NULL;
}
void launcher_update_tick( void *update ) { (void)update; }
void launcher_update_open( struct launcher_update *update ) { (void)update; }
void launcher_update_destroy( struct launcher_update *update ) { (void)update; }
int autorun_install_finish( const char *root ) { (void)root; return 1; }

int launcher_platform_font( const void **data, size_t *size )
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

int launcher_platform_prompt( const char *header, const char *initial, char *out, size_t size )
{
    printf( "prompt '%s' initial '%s' -> %s\n", header, initial, prompt_set ? prompt_text : "(cancelled)" );
    if (!prompt_set) return 0;
    snprintf( out, size, "%s", prompt_text );
    prompt_set = 0;
    return 1;
}

/* A fixed time and charge, so screenshots do not change from run to run. */
int launcher_platform_status( int *hour, int *minute, int *battery, int *charging )
{
    *hour = 13;
    *minute = 24;
    *battery = 86;
    *charging = 0;
    return LAUNCHER_STATUS_CLOCK | LAUNCHER_STATUS_BATTERY;
}

void wine_nx_runtime_trace( const char *msg )
{
    printf( "%s\n", msg );
}

static char graphics_installed[2][32];
static void graphics_resolve( int vkd3d, const char *requested, struct dxvk_version *selected );

static enum dxvk_result graphics_catalog( int vkd3d, struct dxvk_release *releases, int max_releases,
                                          int *count, int cache_only, dxvk_progress_callback progress, void *opaque )
{
    const char *mode = getenv( "LAUNCHER_TEST_GRAPHICS" );
    const char *versions[] = { vkd3d ? "3.1-rc1" : "4.0-rc1", vkd3d ? "3.0b" : "3.1.1",
                               vkd3d ? "2.14.1" : "2.7.1", "1.0", "0.9" };
    int i;

    *count = 0;
    if (!mode || (cache_only && !strcmp( mode, "fresh" ))) return DXVK_NOT_FOUND;
    if (!cache_only)
    {
        Uint32 until = SDL_GetTicks() + 3000;
        fprintf( stderr, "graphics catalog %d started\n", vkd3d );
        while (SDL_GetTicks() < until)
        {
            if (progress && progress( opaque, DXVK_PROGRESS_DOWNLOAD, 0, 0 )) return DXVK_CANCELLED;
            SDL_Delay( 10 );
        }
        if (!strcmp( mode, "offline" )) return DXVK_NETWORK_ERROR;
        fprintf( stderr, "graphics catalog %d finished\n", vkd3d );
    }
    for (i = cache_only ? (!strcmp( mode, "stale" ) ? 2 : 1) : 0; i < 5 && *count < max_releases; i++)
    {
        struct dxvk_release *release = releases + (*count)++;
        memset( release, 0, sizeof(*release) );
        strcpy( release->version, versions[i] );
        release->prerelease = !i;
        release->size = 10 * 1024 * 1024;
    }
    return DXVK_OK;
}

enum dxvk_result dxvk_release_catalog( const char *runtime_dir, struct dxvk_release *releases,
                                       int max_releases, int *count, int cache_only,
                                       dxvk_progress_callback progress, void *opaque )
{
    (void)runtime_dir;
    return graphics_catalog( 0, releases, max_releases, count, cache_only, progress, opaque );
}

static enum dxvk_result graphics_install( int vkd3d, const struct dxvk_release *release,
                                          dxvk_progress_callback progress, void *opaque )
{
    int i;

    if (!getenv( "LAUNCHER_TEST_GRAPHICS" )) return DXVK_IO_ERROR;
    fprintf( stderr, "graphics install %d %s\n", vkd3d, release->version );
    for (i = 0; i <= 100; i++)
    {
        if (progress && progress( opaque, DXVK_PROGRESS_DOWNLOAD, release->size * i / 100, release->size ))
            return DXVK_CANCELLED;
        SDL_Delay( 25 );
    }
    strcpy( graphics_installed[vkd3d], release->version );
    return DXVK_OK;
}

enum dxvk_result dxvk_install_release( const char *runtime_dir, const struct dxvk_release *release,
                                       dxvk_progress_callback progress, void *opaque )
{
    (void)runtime_dir;
    return graphics_install( 0, release, progress, opaque );
}

int dxvk_release_installed( const char *runtime_dir, unsigned short machine, const char *version )
{
    struct dxvk_version selected;
    (void)runtime_dir; (void)machine;
    graphics_resolve( 0, version, &selected );
    return version[0] && selected.installed;
}

int dxvk_root_version( const char *runtime_dir, unsigned short machine, char *version, size_t size )
{
    (void)runtime_dir; (void)machine;
    if (size) version[0] = 0;
    return 0;
}

const char *dxvk_result_message( enum dxvk_result result )
{
    (void)result;
    return "DXVK is unavailable in the host launcher test.";
}

enum dxvk_result vkd3d_release_catalog( const char *runtime_dir, struct dxvk_release *releases,
                                       int max_releases, int *count, int cache_only,
                                       dxvk_progress_callback progress, void *opaque )
{
    (void)runtime_dir;
    return graphics_catalog( 1, releases, max_releases, count, cache_only, progress, opaque );
}

enum dxvk_result vkd3d_install_release( const char *runtime_dir, const struct dxvk_release *release,
                                       dxvk_progress_callback progress, void *opaque )
{
    (void)runtime_dir;
    return graphics_install( 1, release, progress, opaque );
}

int vkd3d_release_installed( const char *runtime_dir, unsigned short machine, const char *version )
{
    struct dxvk_version selected;
    (void)runtime_dir; (void)machine;
    graphics_resolve( 1, version, &selected );
    return version[0] && selected.installed;
}

int vkd3d_root_version( const char *runtime_dir, unsigned short machine, char *version, size_t size )
{
    return dxvk_root_version( runtime_dir, machine, version, size );
}

static void graphics_resolve( int vkd3d, const char *requested, struct dxvk_version *selected )
{
    const char *mode = getenv( "LAUNCHER_TEST_GRAPHICS" );
    const char *version = graphics_installed[vkd3d];

    memset( selected, 0, sizeof(*selected) );
    if (!version[0] && mode && strcmp( mode, "fresh" ) && strcmp( mode, "stale" ))
        version = vkd3d ? "3.0b" : "3.1.1";
    strcpy( selected->version, requested[0] ? requested : version );
    selected->installed = version[0] && !strcmp( version, selected->version );
}

void dxvk_resolve_version( const char *runtime_dir, unsigned short machine, const char *requested,
                           struct dxvk_version *selected )
{
    (void)runtime_dir; (void)machine;
    graphics_resolve( 0, requested, selected );
}

void vkd3d_resolve_version( const char *runtime_dir, unsigned short machine, const char *requested,
                            struct dxvk_version *selected )
{
    (void)runtime_dir; (void)machine;
    graphics_resolve( 1, requested, selected );
}

static int machine_of( const char *path, unsigned short *machine )
{
    unsigned char header[0x40], nt[6];
    FILE *file = fopen( path, "rb" );
    int ok = 0;

    if (!file) return 1;
    if (fread( header, 1, sizeof(header), file ) == sizeof(header) && header[0] == 'M' && header[1] == 'Z' &&
        !fseek( file, header[0x3c] | (header[0x3d] << 8) | (header[0x3e] << 16), SEEK_SET ) &&
        fread( nt, 1, sizeof(nt), file ) == sizeof(nt) && !memcmp( nt, "PE\0\0", 4 ))
    {
        *machine = nt[4] | (nt[5] << 8);
        ok = *machine == 0x014c || *machine == 0xaa64;
    }
    fclose( file );
    return !ok;
}

static void save_png( SDL_Renderer *renderer, const char *name )
{
    int w = 1280, h = 720;
    unsigned char *pixels = malloc( w * h * 4 );
    png_image image;

    if (SDL_RenderReadPixels( renderer, NULL, SDL_PIXELFORMAT_RGBA32, pixels, w * 4 ))
    {
        printf( "read pixels failed: %s\n", SDL_GetError() );
        free( pixels );
        return;
    }
    memset( &image, 0, sizeof(image) );
    image.version = PNG_IMAGE_VERSION;
    image.width = w;
    image.height = h;
    image.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file( &image, name, 0, pixels, 0, NULL )) printf( "writing %s failed\n", name );
    else printf( "frame %d saved to %s\n", frames, name );
    free( pixels );
}

static void push_key( SDL_Keycode key )
{
    SDL_Event event;

    memset( &event, 0, sizeof(event) );
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = key;
    SDL_PushEvent( &event );
}

static void push_tap( int x, int y )
{
    SDL_Event event;

    memset( &event, 0, sizeof(event) );
    event.type = SDL_FINGERDOWN;
    event.tfinger.x = x / 1280.0f;
    event.tfinger.y = y / 720.0f;
    SDL_PushEvent( &event );
    event.type = SDL_FINGERUP;
    SDL_PushEvent( &event );
}

static void push_swipe( int x, int y, int end_x, int end_y )
{
    SDL_Event event = {0};
    event.type = SDL_FINGERDOWN;
    event.tfinger.x = x / 1280.0f;
    event.tfinger.y = y / 720.0f;
    SDL_PushEvent( &event );
    event.type = SDL_FINGERMOTION;
    event.tfinger.x = end_x / 1280.0f;
    event.tfinger.y = end_y / 720.0f;
    SDL_PushEvent( &event );
    event.type = SDL_FINGERUP;
    SDL_PushEvent( &event );
}

static SDL_Keycode key_code( const char *name )
{
    static const struct { const char *name; SDL_Keycode key; } keys[] =
    {
        { "up", SDLK_UP }, { "down", SDLK_DOWN }, { "left", SDLK_LEFT }, { "right", SDLK_RIGHT },
        { "a", SDLK_RETURN }, { "b", SDLK_ESCAPE }, { "x", SDLK_x }, { "y", SDLK_y }, { "plus", SDLK_PLUS },
        { "minus", SDLK_MINUS }, { "l", SDLK_PAGEUP }, { "r", SDLK_PAGEDOWN },
        { "zl", SDLK_LEFTBRACKET }, { "zr", SDLK_RIGHTBRACKET },
    };
    size_t i;

    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp( name, keys[i].name )) return keys[i].key;
    fprintf( stderr, "unknown key %s\n", name );
    exit( 2 );
}

/* Each frame runs the script until a step needs a later frame. */
static void on_frame( SDL_Renderer *renderer )
{
    frames++;
    if (wait_frames > 0)
    {
        wait_frames--;
        return;
    }
    while (script_pos < script_count)
    {
        char *line = script[script_pos++], arg[256];
        int x, y, end_x, end_y;

        fprintf( stderr, "launcher host step: %s\n", line );

        if (sscanf( line, "swipe %d %d %d %d", &x, &y, &end_x, &end_y ) == 4)
        {
            push_swipe( x, y, end_x, end_y );
            wait_frames = 1;
            return;
        }

        if (sscanf( line, "key %255s", arg ) == 1)
        {
            push_key( key_code( arg ) );
            wait_frames = 1;
            return;
        }
        if (sscanf( line, "trigger %255s %d", arg, &x ) == 2)
        {
            SDL_Event event = { .type = SDL_CONTROLLERAXISMOTION };

            assert( !strcmp( arg, "zl" ) || !strcmp( arg, "zr" ) );
            assert( x >= 0 && x <= 32767 );
            event.caxis.axis = !strcmp( arg, "zl" ) ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            event.caxis.value = x;
            SDL_PushEvent( &event );
            wait_frames = 1;
            return;
        }
        if (sscanf( line, "tap %d %d", &x, &y ) == 2)
        {
            push_tap( x, y );
            wait_frames = 1;
            return;
        }
        if (sscanf( line, "wait %d", &x ) == 1)
        {
            wait_frames = x;
            return;
        }
        if (sscanf( line, "shot %255s", arg ) == 1)
        {
            save_png( renderer, arg );
            continue;
        }
        if (!strncmp( line, "type ", 5 ))
        {
            snprintf( prompt_text, sizeof(prompt_text), "%s", line + 5 );
            prompt_set = 1;
            continue;
        }
    }
    if (script_pos >= script_count && wait_frames <= 0)
    {
        /* The script is over and the launcher still runs: quit it. */
        SDL_Event event = { .type = SDL_QUIT };
        SDL_PushEvent( &event );
    }
}

/* Original procedural artwork for layout QA; these are not real game covers. */
static void carousel_fixture(void)
{
    static const char *titles[] = { "Afterlight", "Drift", "Ember", "Frostline", "Horizon", "Nocturne", "Solstice", "Vanguard" };
    static const unsigned char colors[][3] = { {160, 105, 75}, {42, 130, 160}, {170, 60, 40}, {65, 120, 150},
                                               {125, 130, 72}, {66, 70, 125}, {164, 123, 48}, {65, 122, 108} };
    struct launcher_catalog *catalog = calloc( 1, sizeof(*catalog) );
    unsigned char *pixels = malloc( 320 * 480 * 4 );
    int i, x, y;
    assert( catalog && pixels );
    launcher_catalog_init( catalog );
    mkdir( "sdmc:", 0700 ); mkdir( "sdmc:/switch", 0700 ); mkdir( "sdmc:/switch/wine", 0700 );
    mkdir( "sdmc:/switch/wine/drive_c", 0700 );
    for (i = 0; i < 8; i++)
    {
        char folder[512], path[512];
        unsigned char pe[128] = {0};
        FILE *file;
        int index;
        png_image png = {0};
        snprintf( folder, sizeof(folder), "sdmc:/switch/wine/drive_c/%s", titles[i] );
        mkdir( folder, 0700 );
        snprintf( path, sizeof(path), "%s/Game.exe", folder );
        pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 64;
        pe[64] = 'P'; pe[65] = 'E'; pe[68] = 0x4c; pe[69] = 1;
        file = fopen( path, "wb" ); assert( file );
        assert( fwrite( pe, 1, sizeof(pe), file ) == sizeof(pe) ); fclose( file );
        index = launcher_catalog_add( catalog, path, titles[i] ); assert( index >= 0 );
        /* Home lists what has been played, most recent first: play them in reverse
         * so the row reads in the same order as the library's. */
        catalog->entries[index].launched_order = 8 - i;
        snprintf( path, sizeof(path), "%s/Game.wine-nx.txt", folder );
        file = fopen( path, "w" ); assert( file ); fprintf( file, "title=%s\n", titles[i] ); fclose( file );
        for (y = 0; y < 480; y++) for (x = 0; x < 320; x++)
        {
            int channel, offset = (y * 320 + x) * 4;
            float light = 1.1f - y / 700.0f;
            int sun = (x - 205) * (x - 205) + (y - 120) * (y - 120) < 36 * 36;
            if (y > 255 + 55 * sinf( x * 0.016f + i )) light *= 0.7f;
            if (y > 320 + 60 * sinf( x * 0.023f - i )) light *= 0.55f;
            if (y > 405 + 25 * sinf( x * 0.04f )) light *= 0.5f;
            for (channel = 0; channel < 3; channel++)
                pixels[offset + channel] = sun ? 228 - channel * 12 : colors[i][channel] * light;
            pixels[offset + 3] = 255;
        }
        snprintf( path, sizeof(path), "%s/cover.png", folder );
        png.version = PNG_IMAGE_VERSION; png.width = 320; png.height = 480; png.format = PNG_FORMAT_RGBA;
        assert( png_image_write_to_file( &png, path, 0, pixels, 0, NULL ) );
    }
    assert( launcher_catalog_save( catalog, "sdmc:/switch/wine/" LAUNCHER_CATALOG_FILE ) );
    free( pixels ); free( catalog );
}

/* Nothing can be installed from here: the screens leading to it are what this
 * harness is for, and the failure they show is the one a console without
 * Atmosphere would give. */
static unsigned int install_forwarder( const char **step )
{
    if (step) *step = "opening the content store";
    return 0x4A8;
}


int main( int argc, char **argv )
{
    struct wine_nx_launcher_options options = { .runtime_dir = "sdmc:/switch/wine", .build = "nx-host-test",
                                                .emummc = -1, .address_space_bits = 39,
                                                .low_window = 1, .four_cores_available = 1,
                                                .machine_of = machine_of, .vulkan = 1,
                                                .install_forwarder = install_forwarder };
    char target[512] = "", line[300];
    FILE *file;
    int chosen;

    if (argc < 3)
    {
        fprintf( stderr, "usage: %s FONT SCRIPT [TARGET]\n", argv[0] );
        return 2;
    }
    font_path = argv[1];
    if (!(file = fopen( argv[2], "r" ))) return 2;
    while (script_count < 256 && fgets( line, sizeof(line), file ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (line[0] && line[0] != '#') snprintf( script[script_count++], sizeof(script[0]), "%s", line );
    }
    fclose( file );
    if (argc > 3 && !strcmp( argv[3], "--carousel-fixture" )) carousel_fixture();
    else if (argc > 3) snprintf( target, sizeof(target), "%s", argv[3] );

    ui_present_hook = on_frame;
    chosen = wine_nx_launcher_run( &options, target, sizeof(target) );
    printf( "launcher returned %d target '%s' verbose %d profile %d framebuffer %d after %d frames\n",
            chosen, target, options.verbose, options.profile, options.framebuffer, frames );
    free( font_data );
    return 0;
}
