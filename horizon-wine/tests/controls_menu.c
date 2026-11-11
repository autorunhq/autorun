/* Host ASan/UBSan test for carafe_menu + carafe_settings + carafe_draw +
 * carafe_registry + osk.c menu delegation hooks.
 *   cc -std=gnu11 -D_GNU_SOURCE -D__SWITCH__ -Ihorizon-wine/source -Ihorizon-wine/tests/controls-shims \
 *      -Ihorizon-wine/tests/controls-shims/SDL2 horizon-wine/tests/controls_menu.c \
 *      horizon-wine/source/carafe_{menu,settings,draw,text,registry,overlay}.c horizon-wine/source/osk.c \
 *      horizon-wine/source/input_profile.c -lpthread -lm
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "../source/carafe_draw.h"
#include "../source/carafe_menu.h"
#include "../source/carafe_overlay.h"
#include "../source/carafe_registry.h"
#include "../source/carafe_settings.h"
#include "../source/input_profile.h"
#include "../source/osk.h"

extern void wine_nx_menu_poll( u64 held );
extern int wine_nx_osk_translucent( void );
extern void carafeRegistryLoaded( void );

int wine_nx_swkbd_auto_enabled = 1;
struct input_profile wine_nx_input_profile;

static int g_gfx_calls;
static int g_gfx_limit = -1;
static int g_gfx_vsync = -1;
static int g_quit_calls;
static int g_hid_registered;
static int g_reg_locks;
static int g_reg_sets;

void wine_nx_runtime_trace( const char *msg )
{
    (void)msg;
}

int wine_nx_osk_font( const void **data, size_t *size )
{
    (void)data; (void)size;
    return 0;
}

void wine_nx_graphics_configure( int frame_limit, int vsync )
{
    g_gfx_calls++;
    g_gfx_limit = frame_limit;
    g_gfx_vsync = vsync;
}

void wine_nx_request_quit( const char *why )
{
    (void)why;
    g_quit_calls++;
}

void carafeHidRegister( void )
{
    g_hid_registered++;
}

int wine_nx_registry_lock( void )
{
    g_reg_locks++;
    return 0;
}

void wine_nx_registry_unlock( void )
{
    g_reg_locks--;
}

unsigned int wine_nx_registry_set( int hive, const unsigned short *path, unsigned int path_len,
                                   const unsigned short *name, unsigned int name_len,
                                   unsigned int type, const void *data, unsigned int size )
{
    (void)hive; (void)path; (void)path_len; (void)name; (void)name_len;
    (void)type; (void)data; (void)size;
    g_reg_sets++;
    return 0;
}

unsigned int wine_nx_registry_delete_value( int hive, const unsigned short *path, unsigned int path_len,
                                            const unsigned short *name, unsigned int name_len )
{
    (void)hive; (void)path; (void)path_len; (void)name; (void)name_len;
    return 0;
}

unsigned int wine_nx_registry_delete_key( int hive, const unsigned short *path, unsigned int path_len )
{
    (void)hive; (void)path; (void)path_len;
    return 0;
}

int wine_nx_registry_key_info( int hive, const unsigned short *path, unsigned int path_len,
                               unsigned int *subkeys, unsigned int *values )
{
    (void)hive; (void)path; (void)path_len;
    *subkeys = 0;
    *values = 0;
    return -1;
}

int wine_nx_registry_subkey( int hive, const unsigned short *path, unsigned int path_len, unsigned int index,
                             const unsigned short **name, unsigned int *name_len )
{
    (void)hive; (void)path; (void)path_len; (void)index; (void)name; (void)name_len;
    return -1;
}

int wine_nx_registry_value( int hive, const unsigned short *path, unsigned int path_len, unsigned int index,
                            const unsigned short **name, unsigned int *name_len,
                            unsigned int *type, const unsigned char **data, unsigned int *size )
{
    (void)hive; (void)path; (void)path_len; (void)index;
    (void)name; (void)name_len; (void)type; (void)data; (void)size;
    return -1;
}

int wine_nx_registry_get( int hive, const unsigned short *path, unsigned int path_len,
                          const unsigned short *name, unsigned int name_len,
                          unsigned int *type, const unsigned char **data, unsigned int *size )
{
    (void)hive; (void)path; (void)path_len; (void)name; (void)name_len;
    (void)type; (void)data; (void)size;
    return -1;
}

static const char TEST_MENU[] =
    "carafe-menu\t2\n"
    "languages\tru\ten\tzh-Hans\tja\tko\tzh-Hant\tfr\tfr-CA\tde\tit\tes\tes-419\tnl\tpt\tpt-BR\n"
    "section\tgraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\n"
    "setting\tframe-limit\tprogram\tframe-limit\tnow\t0\t-\t-\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tFrame limit\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\tLimit FPS\n"
    "value\t0\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\n"
    "value\t30\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\t30 FPS\n"
    "setting\tvsync\tprogram\tvsync\trestart\t0\t-\t-\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\tSync frames\n"
    "value\t1\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\n"
    "value\t0\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\n";

static const char TEST_REGISTRY[] =
    "carafe-registry\t1\n"
    "set\tHKCU\tSoftware\\Test\tEnabled\t4\t01000000\n";

static const char TEST_PROGRAM_CFG[] = "frame-limit = 0\nvsync = 1\n";
static char* g_custom_menu;

void *carafeOverlayReadRomfsFile( const char *path, size_t limit, size_t *size )
{
    const char *src = NULL;
    size_t len = 0;

    if (size) *size = 0;
    if (!path) return NULL;
    if (!strcmp( path, "/carafe/menu.txt" )) src = g_custom_menu ? g_custom_menu : TEST_MENU;
    else if (!strcmp( path, "/carafe/registry.txt" )) src = TEST_REGISTRY;
    else if (strstr( path, ".wine-nx.txt" )) src = TEST_PROGRAM_CFG;
    else return NULL;

    len = strlen( src );
    if (len > limit) return NULL;
    char *copy = malloc( len + 1 );
    if (!copy) return NULL;
    memcpy( copy, src, len + 1 );
    if (size) *size = len;
    return copy;
}

bool carafeOverlayReadRomfsText( const char *path, char *out, size_t size )
{
    if (!path || !out || !size) return false;
    if (!strcmp( path, "/carafe/title" ))
    {
        snprintf( out, size, "Test Game" );
        return true;
    }
    if (!strcmp( path, "/carafe/argv" ))
    {
        snprintf( out, size, "\"sdmc:/switch/wine/drive_c/Games/test.exe\"" );
        return true;
    }
    out[0] = '\0';
    return false;
}

void carafeOverlayLog( const char *line ) { (void)line; }
void carafeOverlayCommit( const char *reason ) { (void)reason; }

int main( int argc, char **argv )
{
    const u64 menu_chord = HidNpadButton_Minus | HidNpadButton_Plus | HidNpadButton_L | HidNpadButton_R;
    struct wine_nx_osk_frame frame;
    uint32_t *pixels;
    unsigned int gen0, gen1, copied;
    CarafeCatalog *catalog;
    int fl_idx, vs_idx;

    (void)argc;
    if (argc > 1 && argv[1] && argv[1][0])
    {
        FILE *f = fopen( argv[1], "rb" );
        assert( f != NULL );
        assert( fseek( f, 0, SEEK_END ) == 0 );
        long len = ftell( f );
        assert( len > 0 && fseek( f, 0, SEEK_SET ) == 0 );
        g_custom_menu = malloc( (size_t)len + 1 );
        assert( g_custom_menu != NULL );
        assert( fread( g_custom_menu, 1, (size_t)len, f ) == (size_t)len );
        g_custom_menu[len] = '\0';
        fclose( f );
    }

    input_profile_defaults( &wine_nx_input_profile, 1 );

    /* Registry boot hook registers HID gamepad and applies entries. */
    carafeRegistryLoaded();
    assert( g_hid_registered == 1 );
    assert( g_reg_sets >= 1 );
    assert( g_reg_locks == 0 );

    /* Catalog & live vs restart settings. */
    carafeSettingsSyncProgram();
    assert( strcmp( getenv( "NVK_SWITCH_CPU_WRITE_MEM_UNCACHED" ), "0" ) == 0 );
    catalog = carafeCatalogLoad();
    assert( catalog != NULL );
    if (g_custom_menu)
    {
        assert( catalog->section_count == 4 );
        assert( catalog->setting_count == 116 );
        assert( catalog->group_count == 2 );
        assert( catalog->suggest_count == 11 );
        assert( catalog->pad_count == 24 );
        assert( catalog->key_count == 120 );
        assert( carafeCatalogFindKey( catalog, CarafeFileProgram, "FEX_TSOENABLED" ) >= 0 );
        assert( carafeCatalogFind( catalog, "large-pages" ) >= 0 );
        assert( carafeCatalogFindKey( catalog, CarafeFileProgram, "lazy-file-views" ) >= 0 );
        assert( carafeCatalogFind( catalog, "button-layout" ) >= 0 );
    }
    else
    {
        assert( catalog->section_count == 1 );
        assert( catalog->setting_count == 2 );
    }
    fl_idx = carafeCatalogFind( catalog, "frame-limit" );
    vs_idx = carafeCatalogFind( catalog, "vsync" );
    assert( fl_idx >= 0 && vs_idx >= 0 );

    assert( carafeSettingSet( catalog, fl_idx, 1 ) );
    assert( g_gfx_calls == 1 && g_gfx_limit == 30 && g_gfx_vsync == 1 );
    assert( carafePendingCount( catalog ) == 0 );

    assert( carafeSettingSet( catalog, vs_idx, 1 ) );
    assert( carafePendingCount( catalog ) == 1 );
    carafeUndoPending( catalog );
    assert( carafePendingCount( catalog ) == 0 );
    carafeCatalogFree( catalog );
    assert( carafeResetAll( NULL, true ) );

    /* Menu open via 0041 chord + OSK delegation. */
    assert( !carafeMenuVisible() );
    assert( !wine_nx_osk_visible() );
    assert( !wine_nx_osk_translucent() );

    wine_nx_menu_poll( menu_chord );
    assert( carafeMenuVisible() );
    assert( wine_nx_osk_visible() );
    assert( wine_nx_osk_translucent() );

    gen0 = wine_nx_osk_generation();
    memset( &frame, 0, sizeof(frame) );
    assert( wine_nx_osk_frame( CARAFE_SCREEN_WIDTH, CARAFE_SCREEN_HEIGHT, &frame ) == 1 );
    assert( frame.x == 0 && frame.y == 0 );
    assert( frame.width == CARAFE_SCREEN_WIDTH && frame.height == CARAFE_SCREEN_HEIGHT );

    pixels = calloc( (size_t)CARAFE_SCREEN_WIDTH * CARAFE_SCREEN_HEIGHT, 4 );
    assert( pixels != NULL );
    copied = wine_nx_osk_copy( CARAFE_SCREEN_WIDTH, CARAFE_SCREEN_HEIGHT,
                               pixels, CARAFE_SCREEN_WIDTH * 4, 1 );
    assert( copied != 0 );
    assert( pixels[0] != 0 );
    free( pixels );

    /* Docked 1920x1080 and smaller swapchains (800x600) also render and scale. */
    memset( &frame, 0, sizeof(frame) );
    assert( wine_nx_osk_frame( 1920, 1080, &frame ) == 1 );
    assert( frame.width == 1920 && frame.height == 1080 );
    pixels = calloc( 1920 * 1080, 4 );
    assert( pixels != NULL );
    assert( wine_nx_osk_copy( 1920, 1080, pixels, 1920 * 4, 0 ) == frame.generation );
    assert( pixels[0] != 0 && pixels[1920 * 1080 - 1] != 0 );
    free( pixels );

    pixels = calloc( 800 * 600, 4 );
    assert( pixels != NULL );
    assert( wine_nx_osk_copy( 800, 600, pixels, 800 * 4, 1 ) != 0 );
    assert( pixels[0] != 0 && pixels[800 * 600 - 1] != 0 );
    free( pixels );

    /* Input routes through osk.c -> carafeMenuHandleInput and increments generation. */
    assert( wine_nx_osk_input( OSK_RIGHT, 0, 0, NULL, 0, 2000000000ULL ) == 1 );
    gen1 = wine_nx_osk_generation();
    assert( gen1 != gen0 );

    /* Holding the chord does not re-toggle; releasing then pressing toggles closed. */
    wine_nx_menu_poll( menu_chord );
    assert( carafeMenuVisible() );
    wine_nx_menu_poll( 0 );
    wine_nx_menu_poll( menu_chord );
    assert( !carafeMenuVisible() );
    assert( !wine_nx_osk_visible() );

    /* wine_nx_osk_reset also closes an open menu cleanly. */
    wine_nx_menu_poll( 0 );
    wine_nx_menu_poll( menu_chord );
    assert( carafeMenuVisible() );
    wine_nx_osk_reset();
    assert( !carafeMenuVisible() );
    assert( !wine_nx_osk_visible() );

    free( g_custom_menu );
    g_custom_menu = NULL;
    puts( "controls_menu tests passed" );
    return 0;
}
