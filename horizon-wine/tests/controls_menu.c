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
#include <sys/stat.h>
#include <unistd.h>

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
static int g_mock_reg_ok;
u64 controls_test_tick = 19200000ull;

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
    return g_mock_reg_ok ? 0 : -1;
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
    "value\t0\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\n"
    "preset\tp1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tP1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\tH1\n"
    "set\tvsync\t1\n"
    "preset\tp2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tP2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\tH2\n"
    "set\tframe-limit\t1\n";

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

        int four_cores_suggest = 0;
        for (int i = 0; i < catalog->suggest_count; i++)
        {
            const CarafeSuggest *sg = &catalog->suggests[i];
            const CarafeSetting *st = &catalog->settings[sg->setting];
            assert( sg->value >= 0 && sg->value < st->value_count );
            if (strcmp( st->id, "four-cores" ) == 0)
            {
                assert( sg->value == 1 );
                assert( strcmp( st->values[sg->value].text, "0" ) == 0 );
                four_cores_suggest = 1;
            }
        }
        assert( four_cores_suggest == 1 );
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

    if (!g_custom_menu)
    {
        /* Asymmetric presets: applying p2 must reset p1-only settings to default. */
        assert( carafePresetApply( catalog, &catalog->sections[0].presets[0] ) );
        assert( carafePresetCurrent( catalog, &catalog->sections[0] ) == 0 );
        assert( carafePresetApply( catalog, &catalog->sections[0].presets[1] ) );
        assert( carafePresetCurrent( catalog, &catalog->sections[0] ) == 1 );
        assert( catalog->settings[vs_idx].current == catalog->settings[vs_idx].default_index );
        carafeUndoPending( catalog );
        assert( carafeSettingSet( catalog, fl_idx, 0 ) );
    }

    /* Oversized registry string must clamp cleanly without overflowing internal wide buffer. */
    {
        uint8_t oversized[CARAFE_REG_DATA + 64];
        char formatted[256] = "";
        for (size_t i = 0; i + 1 < sizeof(oversized); i += 2)
        {
            oversized[i] = 'A';
            oversized[i + 1] = 0;
        }
        carafeRegistryFormat( CARAFE_REG_SZ, oversized, sizeof(oversized), formatted, sizeof(formatted) );
        assert( formatted[0] == 'A' );
    }

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

    if (g_custom_menu)
    {
        char orig_cwd[512];
        char tmp_tpl[] = "/tmp/controls_menu_sd_XXXXXX";
        assert( getcwd( orig_cwd, sizeof(orig_cwd) ) != NULL );
        char *tmp_dir = mkdtemp( tmp_tpl );
        assert( tmp_dir != NULL );
        assert( chdir( tmp_dir ) == 0 );
        mkdir( "sdmc:", 0777 );
        mkdir( "sdmc:/switch", 0777 );
        mkdir( "sdmc:/switch/wine", 0777 );
        mkdir( "sdmc:/switch/wine/config", 0777 );
        mkdir( "sdmc:/switch/wine/carafe", 0777 );
        mkdir( "sdmc:/switch/wine/drive_c", 0777 );
        mkdir( "sdmc:/switch/wine/drive_c/Games", 0777 );

        FILE *f = fopen( "sdmc:/switch/wine/config/keys.txt", "wb" );
        assert( f != NULL );
        fputs( "button-layout=1\n", f );
        fclose( f );

        f = fopen( "sdmc:/switch/wine/drive_c/Games/test.wine-nx.txt", "wb" );
        assert( f != NULL );
        fputs( "button-layout=0\nvsync=0\n", f );
        fclose( f );

        f = fopen( "sdmc:/switch/wine/drive_c/Games/test.keys.txt", "wb" );
        assert( f != NULL );
        fputs( "keyboard-auto=0\n", f );
        fclose( f );

        wine_nx_swkbd_auto_enabled = 1;
        carafeSettingsSyncProgram();
        CarafeCatalog *disk_cat = carafeCatalogLoad();
        assert( disk_cat != NULL );
        int bl_idx = carafeCatalogFind( disk_cat, "button-layout" );
        int kb_idx = carafeCatalogFind( disk_cat, "keyboard-auto" );
        assert( bl_idx >= 0 && kb_idx >= 0 );
        assert( disk_cat->settings[bl_idx].base == 1 );

        assert( carafeSettingSet( disk_cat, kb_idx, 0 ) );
        assert( wine_nx_swkbd_auto_enabled == 0 );

        assert( carafeResetAll( disk_cat, true ) );
        assert( disk_cat->settings[bl_idx].current == 1 );
        assert( disk_cat->settings[bl_idx].start == 1 );
        assert( wine_nx_swkbd_auto_enabled == 1 );

        /* Legacy button-layout key was stripped from .wine-nx.txt too. */
        f = fopen( "sdmc:/switch/wine/drive_c/Games/test.wine-nx.txt", "rb" );
        assert( f != NULL );
        char buf[256] = "";
        size_t got = fread( buf, 1, sizeof(buf) - 1, f );
        buf[got] = '\0';
        fclose( f );
        assert( strstr( buf, "button-layout" ) == NULL );
        assert( fopen( "sdmc:/switch/wine/drive_c/Games/test.keys.txt", "rb" ) == NULL );

        /* Test 1: deferred "Later" changes flushed at startup must NOT leak into trial.txt. */
        assert( carafeWriteChangesText( "carafe-changes\t1\nprogram\tvsync\t0\n" ) );
        assert( carafeChangesExist() );
        carafeSettingsSyncProgram();
        assert( !carafeChangesExist() );

        carafeCatalogFree( disk_cat );
        disk_cat = carafeCatalogLoad();
        assert( disk_cat != NULL );
        int hud_idx = carafeCatalogFind( disk_cat, "dxvk-hud" );
        int vs2_idx = carafeCatalogFind( disk_cat, "vsync" );
        assert( hud_idx >= 0 && vs2_idx >= 0 );
        assert( carafeSettingSet( disk_cat, hud_idx, 1 ) );
        carafeTrialWrite( true );

        char trial_buf[512] = "";
        assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );
        assert( strstr( trial_buf, "program\tdxvk-hud\t-" ) != NULL );
        assert( strstr( trial_buf, "vsync" ) == NULL );

        /* Test 2: error-screen preparation leaves changes.txt intact and preserves prior trial entries. */
        assert( carafeWriteChangesText( "carafe-changes\t1\nprogram\tvsync\t1\n" ) );
        carafeSettingsPrepareError();
        assert( carafeChangesExist() );

        carafeCatalogFree( disk_cat );
        disk_cat = carafeCatalogLoad();
        assert( disk_cat != NULL );
        /* Modify the key that was already in trial.txt -> original value "-" must stay preserved. */
        assert( carafeSettingSet( disk_cat, hud_idx, 0 ) );
        carafeTrialWrite( true );

        memset( trial_buf, 0, sizeof(trial_buf) );
        assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );
        assert( strstr( trial_buf, "program\tdxvk-hud\t-" ) != NULL );
        assert( strstr( trial_buf, "program\tvsync\t0" ) != NULL );

        /* Test 3: carafeResetAll(..., false) resets both current AND start to default_index. */
        assert( carafeSettingSet( disk_cat, vs2_idx, 1 ) );
        assert( carafeResetAll( disk_cat, false ) );
        assert( disk_cat->settings[vs2_idx].current == disk_cat->settings[vs2_idx].default_index );
        assert( disk_cat->settings[vs2_idx].start == disk_cat->settings[vs2_idx].default_index );
        assert( !carafeChangesExist() );

        /* Test 4: tapping header [B] Back in error mode sub-group steps up instead of exiting menu. */
        assert( carafeMenuOpenError( CarafeKindNone ) );
        /* Switch from Graphics (tab 0) to Controls (tab 3), then enter button picker via OSK_A. */
        assert( carafeMenuInput( OSK_DOWN, false, 0, 0, 100000000ULL ) == CarafeMenuStay );
        for (int t = 0; t < 3; t++)
        {
            assert( carafeMenuInput( OSK_R, false, 0, 0, 200000000ULL + (u64)t * 100000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 250000000ULL + (u64)t * 100000000ULL ) == CarafeMenuStay );
        }
        assert( carafeMenuInput( OSK_A, false, 0, 0, 600000000ULL ) == CarafeMenuStay );
        assert( carafeMenuInput( 0, false, 0, 0, 650000000ULL ) == CarafeMenuStay );
        uint32_t *menu_px = calloc( 1280 * 720, 4 );
        assert( menu_px != NULL );
        CarafeImage menu_img = { menu_px, 1280, 720 };
        carafeMenuRender( &menu_img );
        /* Find header [B] Back pill on y=75 between x=550..1200. */
        uint32_t header_bg = menu_px[75 * 1280 + 550];
        int back_x = -1;
        for (int x = 550; x < 1200; x++)
        {
            if (menu_px[75 * 1280 + x] != header_bg)
            {
                back_x = x + 10;
                break;
            }
        }
        assert( back_x > 0 );
        /* Tapping header [B] Back must close picker and stay in menu, NOT return CarafeMenuBack. */
        assert( carafeMenuInput( 0, true, back_x, 60, 700000000ULL ) == CarafeMenuStay );
        assert( carafeMenuInput( 0, false, 0, 0, 750000000ULL ) == CarafeMenuStay );
        /* Now pressing B at root of error menu exits back to error screen. */
        assert( carafeMenuInput( OSK_B, false, 0, 0, 800000000ULL ) == CarafeMenuBack );
        free( menu_px );
        carafeMenuClose();

        /* Test 5: truncated/malformed UTF-8 in carafeWrappedHeight never reads past NUL. */
        {
            CarafeTextStyle wrap_style = { 18, CarafeRegular, 24, 0xffffffffu, false };
            assert( carafeWrappedHeight( "abc\xE4\xB8", &wrap_style, 200 ) > 0 );
            assert( carafeWrappedHeight( "\xF0\x9F", &wrap_style, 200 ) > 0 );
            assert( carafeWrappedHeight( "\xC3", &wrap_style, 200 ) > 0 );
        }

        /* Test 6: carafeRegistryBringBack reloads pending for carafeRegistryPendingLine / carafeRegistryList,
         * and CRLF in registry-removed.txt is stripped cleanly. */
        {
            FILE *rb = fopen( "sdmc:/switch/wine/carafe/0100000000001000/registry-rollback.txt", "wb" );
            assert( rb != NULL );
            fputs( "carafe-registry-pending\t1\nset\tHKCU\tSoftware\\Test\tRestored\t4\t2a000000\n", rb );
            fclose( rb );
            carafeRegistryBringBack();

            char pname[64] = "", pfrom[64] = "", pto[64] = "";
            assert( carafeRegistryPendingLine( 0, pname, sizeof(pname), pfrom, sizeof(pfrom), pto, sizeof(pto) ) );
            assert( strcmp( pname, "Restored" ) == 0 );
            assert( strstr( pto, "42" ) != NULL );

            /* BringBack again, then test carafeRegistryList directly without calling PendingCount first. */
            rb = fopen( "sdmc:/switch/wine/carafe/0100000000001000/registry-rollback.txt", "wb" );
            assert( rb != NULL );
            fputs( "carafe-registry-pending\t1\nset\tHKCU\tSoftware\\Test\tListed\t4\t07000000\n", rb );
            fclose( rb );
            carafeRegistryBringBack();

            g_mock_reg_ok = 1;
            CarafeRegEntry entries[8];
            int ecount = carafeRegistryList( CARAFE_REG_HIVE_USER, "Software\\Test", entries, 8 );
            assert( ecount == 1 );
            assert( strcmp( entries[0].name, "Listed" ) == 0 );
            assert( entries[0].state == CarafeRegAdded );
            g_mock_reg_ok = 0;

            /* CRLF registry-removed.txt must still match REMOVED_HEADER. */
            FILE *rem = fopen( "sdmc:/switch/wine/carafe/0100000000001000/registry-removed.txt", "wb" );
            assert( rem != NULL );
            fputs( "carafe-registry-removed\t1\r\ndelete\tHKCU\tSoftware\\Test\tEnabled\r\n", rem );
            fclose( rem );
            carafeRegistryUndoPending();
            carafeRegistryLoaded();
        }

        /* Test 7: carafeSettingsTick clears trial after 300s AND resets on subsequent carafeSettingsSyncProgram. */
        {
            controls_test_tick = 19200000ull;
            carafeTrialWrite( true );
            carafeSettingsSyncProgram();
            assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );
            carafeSettingsTick();
            assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );

            controls_test_tick = 19200000ull + 301ull * 19200000ull;
            carafeSettingsTick();
            assert( !carafeTrialRead( trial_buf, sizeof(trial_buf) ) );

            /* Second program session starts with trial.txt again: must wait 300s from new start, then clear it. */
            carafeTrialWrite( true );
            carafeSettingsSyncProgram();
            assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );
            carafeSettingsTick();
            assert( carafeTrialRead( trial_buf, sizeof(trial_buf) ) );

            controls_test_tick += 301ull * 19200000ull;
            carafeSettingsTick();
            assert( !carafeTrialRead( trial_buf, sizeof(trial_buf) ) );
        }

        /* Test 8: HitTab disarms reset-confirm, and registry DWORD base toggle rejects >32-bit overflow. */
        {
            /* Open registry browser via System tab + enter registry row. */
            const char *reg_menu =
                "carafe-menu\t2\n"
                "languages\tru\ten\tzh-Hans\tja\tko\tzh-Hant\tfr\tfr-CA\tde\tit\tes\tes-419\tnl\tpt\tpt-BR\n"
                "section\tgraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\tGraphics\n"
                "setting\tvsync\tprogram\tvsync\trestart\t0\t-\t-\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tVSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\tSync\n"
                "value\t1\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\tOn\n"
                "value\t0\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\tOff\n"
                "section\tsystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\tSystem\n"
                "reset\n";
            free( g_custom_menu );
            g_custom_menu = strdup( reg_menu );
            assert( g_custom_menu != NULL );

            /* 8a: Arm reset on System tab, tap Graphics tab, tap System tab again -> pressing A must only re-arm, not reset. */
            wine_nx_menu_poll( 0 );
            wine_nx_menu_poll( menu_chord );
            wine_nx_menu_poll( 0 );
            assert( carafeMenuVisible() );
            assert( carafeMenuInput( OSK_R, false, 0, 0, 900000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 920000000ULL ) == CarafeMenuStay );
            /* Move to Reset row (row 0 = registry row when g_mock_reg_ok=1, or row 0 = reset? system section with registry has registry at 0, reset at 1). */
            assert( carafeMenuInput( OSK_DOWN, false, 0, 0, 940000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 960000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( OSK_A, false, 0, 0, 980000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1000000000ULL ) == CarafeMenuStay );
            /* Render to populate hit targets for tabs. */
            uint32_t *tab_px = calloc( 1280 * 720, 4 );
            assert( tab_px != NULL );
            CarafeImage tab_img = { tab_px, 1280, 720 };
            carafeMenuRender( &tab_img );
            free( tab_px );
            /* Tap first tab (Graphics, left side of tab strip y=10..42, x=90) then return to System via R. */
            assert( carafeMenuInput( 0, true, 100, 104, 1020000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1040000000ULL ) == CarafeMenuStay );
            carafeMenuClose();

            /* 8b: In registry editor for DWORD, enter "4294967296" and press Y (toggle base):
             * must NOT convert/commit as 0x100000000. */
            g_mock_reg_ok = 1;
            wine_nx_menu_poll( menu_chord );
            wine_nx_menu_poll( 0 );
            assert( carafeMenuInput( OSK_R, false, 0, 0, 1100000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1110000000ULL ) == CarafeMenuStay );
            /* Open registry (row 0 = Browse registry), descend to HKCU (down + A). */
            assert( carafeMenuInput( OSK_A, false, 0, 0, 1120000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1130000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( OSK_DOWN, false, 0, 0, 1140000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1150000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( OSK_A, false, 0, 0, 1160000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1170000000ULL ) == CarafeMenuStay );
            /* Open New value with OSK_X. */
            assert( carafeMenuInput( OSK_X, false, 0, 0, 1180000000ULL ) == CarafeMenuStay );
            assert( carafeMenuInput( 0, false, 0, 0, 1190000000ULL ) == CarafeMenuStay );
            /* Type name "x" (key index 32 in KEYS_TEXT: row 3 col 2 -> down*3 + right*2). */
            for (int r = 0; r < 3; r++) {
                carafeMenuInput( OSK_DOWN, false, 0, 0, 1200000000ULL + (u64)r * 20000000ULL );
                carafeMenuInput( 0, false, 0, 0, 1210000000ULL + (u64)r * 20000000ULL );
            }
            for (int c = 0; c < 2; c++) {
                carafeMenuInput( OSK_RIGHT, false, 0, 0, 1270000000ULL + (u64)c * 20000000ULL );
                carafeMenuInput( 0, false, 0, 0, 1280000000ULL + (u64)c * 20000000ULL );
            }
            carafeMenuInput( OSK_A, false, 0, 0, 1320000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1330000000ULL );
            /* Move R twice to Value field (DWORD by default). */
            carafeMenuInput( OSK_R, false, 0, 0, 1340000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1350000000ULL );
            carafeMenuInput( OSK_R, false, 0, 0, 1360000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1370000000ULL );
            /* Enter "4294967296": KEYS_DEC = {1,2,3,4,5,<-, 6,7,8,9,0,CLR}.
             * Key 4 = index 3 (right*3 from 0). Just enter "9" ten times (index 9 = down + right*3) which overflows 32-bit DWORD! */
            carafeMenuInput( OSK_DOWN, false, 0, 0, 1380000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1390000000ULL );
            for (int c = 0; c < 3; c++) {
                carafeMenuInput( OSK_RIGHT, false, 0, 0, 1400000000ULL + (u64)c * 20000000ULL );
                carafeMenuInput( 0, false, 0, 0, 1410000000ULL + (u64)c * 20000000ULL );
            }
            for (int d = 0; d < 10; d++) {
                carafeMenuInput( OSK_A, false, 0, 0, 1500000000ULL + (u64)d * 20000000ULL );
                carafeMenuInput( 0, false, 0, 0, 1510000000ULL + (u64)d * 20000000ULL );
            }
            int before_pending = carafeRegistryPendingCount();
            /* Toggle base with Y -> must keep decimal & reject overflow, and '+' must not commit. */
            carafeMenuInput( OSK_Y, false, 0, 0, 1800000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1810000000ULL );
            carafeMenuInput( OSK_PLUS, false, 0, 0, 1820000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1830000000ULL );
            assert( carafeRegistryPendingCount() == before_pending );
            /* Now toggle only after removing two digits with X -> fits 32-bit DWORD, Y switches to hex & + commits! */
            carafeMenuInput( OSK_X, false, 0, 0, 1840000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1850000000ULL );
            carafeMenuInput( OSK_X, false, 0, 0, 1860000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1870000000ULL );
            carafeMenuInput( OSK_Y, false, 0, 0, 1880000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1890000000ULL );
            carafeMenuInput( OSK_PLUS, false, 0, 0, 1900000000ULL );
            carafeMenuInput( 0, false, 0, 0, 1910000000ULL );
            assert( carafeRegistryPendingCount() == before_pending + 1 );
            carafeMenuClose();
            g_mock_reg_ok = 0;
        }

        carafeCatalogFree( disk_cat );
        assert( chdir( orig_cwd ) == 0 );
        char rm_cmd[600];
        snprintf( rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmp_dir );
        assert( system( rm_cmd ) == 0 );
    }

    free( g_custom_menu );
    g_custom_menu = NULL;
    puts( "controls_menu tests passed" );
    return 0;
}
