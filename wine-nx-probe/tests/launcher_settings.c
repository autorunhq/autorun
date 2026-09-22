/* Host test for the launcher's settings files (source/launcher_settings.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../source/launcher_settings.h"

static void load_text( struct launcher_kv *kv, const char *text )
{
    kv->size = strlen( text );
    memcpy( kv->text, text, kv->size + 1 );
}

static void test_get(void)
{
    struct launcher_kv kv;
    char value[16];

    load_text( &kv, "# comment=1\r\n  Theme = glow \r\ncolumns=6\nbad=6x\nempty=\ntitle=a=b" );
    assert( launcher_kv_get( &kv, "theme", value, sizeof(value) ) && !strcmp( value, "glow" ) );
    assert( launcher_kv_get_int( &kv, "columns", 5 ) == 6 );
    assert( launcher_kv_get_int( &kv, "bad", 5 ) == 5 && launcher_kv_get_int( &kv, "empty", 5 ) == 5 );
    assert( launcher_kv_get_int( &kv, "rows", 2 ) == 2 );
    assert( !launcher_kv_get( &kv, "comment", value, sizeof(value) ) );  /* "# comment" is not the key */
    assert( launcher_kv_get( &kv, "title", value, sizeof(value) ) && !strcmp( value, "a=b" ) );  /* no newline at the end */
    assert( launcher_kv_get( &kv, "title", value, 2 ) && !strcmp( value, "a" ) );
    assert( !launcher_kv_get( &kv, "them", value, sizeof(value) ) && !launcher_kv_get( &kv, "theme2", value, sizeof(value) ) );
}

static void test_set(void)
{
    struct launcher_kv kv;
    char big[1100];

    load_text( &kv, "# keep me\nverbose=1\r\nfuture=x" );
    assert( launcher_kv_set( &kv, "verbose", "0" ) );
    assert( !strcmp( kv.text, "# keep me\nverbose=0\nfuture=x" ) );
    assert( launcher_kv_set( &kv, "profile", "1" ) );
    assert( !strcmp( kv.text, "# keep me\nverbose=0\nfuture=x\nprofile=1\n" ) );
    assert( launcher_kv_set( &kv, "verbose", NULL ) );
    assert( !strcmp( kv.text, "# keep me\nfuture=x\nprofile=1\n" ) );
    assert( launcher_kv_set( &kv, "missing", NULL ) && kv.size == strlen( "# keep me\nfuture=x\nprofile=1\n" ) );
    assert( launcher_kv_set( &kv, "profile", NULL ) && !strcmp( kv.text, "# keep me\nfuture=x\n" ) );
    assert( !launcher_kv_set( &kv, "title", "two\nlines" ) );
    memset( big, 'a', sizeof(big) - 1 );
    big[sizeof(big) - 1] = 0;
    assert( !launcher_kv_set( &kv, "title", big ) );
    assert( !strcmp( kv.text, "# keep me\nfuture=x\n" ) );

    /* The file never grows past its buffer. */
    load_text( &kv, "" );
    {
        char key[128];
        int i = 0;

        do snprintf( key, sizeof(key), "%.90s%d", big, i++ );
        while (launcher_kv_set( &kv, key, "v" ));
        assert( i > 50 && kv.size < LAUNCHER_KV_MAX && strlen( kv.text ) == kv.size );
        assert( launcher_kv_set( &kv, key, NULL ) );  /* removing still works when full */
    }
}

static void test_settings( const char *dir )
{
    struct launcher_settings settings, back;
    struct launcher_kv kv;
    char path[768], other[768], config[512];

    load_text( &kv, "" );
    launcher_settings_read( &kv, &settings );
    assert( !settings.fast_sync && !settings.fex );
    load_text( &kv, "cpu=fex\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.fex );
    assert( launcher_settings_write( &kv, &settings ) );
    launcher_settings_read( &kv, &back );
    assert( back.fex );
    settings.fex = 0;
    assert( launcher_settings_write( &kv, &settings ) && !strstr( kv.text, "cpu=" ) );
    load_text( &kv, "cpu=unknown\n" );
    launcher_settings_read( &kv, &settings );
    assert( !settings.fex );
    load_text( &kv, "sync=horizon\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.fast_sync );
    assert( launcher_settings_write( &kv, &settings ) );
    launcher_settings_read( &kv, &back );
    assert( back.fast_sync );
    settings.fast_sync = 0;
    assert( launcher_settings_write( &kv, &settings ) && !strstr( kv.text, "sync=" ) );
    load_text( &kv, "sync=fsync\n" );
    launcher_settings_read( &kv, &settings );
    assert( !settings.fast_sync );

    assert( launcher_settings_path( "sdmc:/switch/wine/drive_c/nfsu2/SPEED2.EXE", path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/switch/wine/drive_c/nfsu2/SPEED2.wine-nx.txt" ) );
    assert( !launcher_settings_path( "sdmc:/readme.txt", path, sizeof(path) ) );
    assert( !launcher_settings_path( "sdmc:/a.exe", path, 19 ) && launcher_settings_path( "sdmc:/a.exe", path, 20 ) );

    load_text( &kv, "verbose=on\nprofile=off\nwindows=framebuffer\nd3d9=DXVK\nhidden=yes\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.verbose == 1 && settings.profile == 0 && settings.framebuffer == 1 && settings.dxvk == 1 );
    assert( settings.hidden == 0 && !settings.title[0] );  /* only 1 or on hides */
    assert( settings.address_space == -1 );                /* absent: read it from the program */
    load_text( &kv, "address-space=32\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.address_space == 1 );
    load_text( &kv, "address-space=Any\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.address_space == 0 );

    load_text( &kv, "d3d=wine\nd3d9=dxvk\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.dxvk == 0 );
    load_text( &kv, "d3d=DXVK\ndxvk-version=2.7.1\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.dxvk == 1 && !strcmp( settings.dxvk_version, "2.7.1" ) );
    load_text( &kv, "d3d=DXVK\ndxvk-version=../../bad\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.dxvk == 1 && !settings.dxvk_version[0] );
    assert( launcher_dxvk_version_valid( "3.1.1" ) && launcher_dxvk_version_valid( "2.0-rc1" ) );
    assert( !launcher_dxvk_version_valid( "" ) && !launcher_dxvk_version_valid( "../3.1" ) &&
            !launcher_dxvk_version_valid( "3.1/other" ) );
    assert( launcher_dxvk_version_selectable( "1.0" ) && launcher_dxvk_version_selectable( "3.1.1" ) );
    assert( !launcher_dxvk_version_selectable( "0.96" ) && !launcher_dxvk_version_selectable( "bad" ) );
    assert( !strcmp( launcher_dxvk_directory( 0x014c ), "dxvk" ) );
    assert( !strcmp( launcher_dxvk_directory( 0x8664 ), "dxvk64" ) );
    assert( !launcher_dxvk_directory( 0xaa64 ) && !launcher_dxvk_directory( 0 ) );
    assert( LAUNCHER_HUD_COUNT == 4 );
    assert( !strcmp( launcher_hud_values[2], "api,fps,frametimes" ) );
    assert( !strcmp( launcher_hud_values[3],
                     "version,api,devinfo,fps,memory,frametimes,compiler" ) );
    load_text( &kv, "dxvk-hud=api,fps,frametimes\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.dxvk_hud == 2 );
    load_text( &kv, "dxvk-hud=version,api,devinfo,fps,memory,frametimes,compiler\n" );
    launcher_settings_read( &kv, &settings );
    assert( settings.dxvk_hud == 3 );
    settings.dxvk_hud = LAUNCHER_HUD_COUNT;
    assert( !launcher_settings_write( &kv, &settings ) );
    settings.dxvk_hud = 0;
    assert( LAUNCHER_FRAME_LIMIT_COUNT == 8 );
    assert( launcher_dxvk_config( &settings, config, sizeof(config) ) );
    assert( strstr( config, "dxgi.syncInterval = 1" ) );
    assert( launcher_settings_write( &kv, &settings ) && !strstr( kv.text, "frame-limit=" ) );
    assert( launcher_dxvk_version_directory( 0x8664, "2.7.1", path, sizeof(path) ) &&
            !strcmp( path, "dxvk64\\versions\\2.7.1" ) );
    assert( launcher_dxvk_version_directory( 0x014c, "", path, sizeof(path) ) && !strcmp( path, "dxvk" ) );
    assert( !launcher_dxvk_version_directory( 0x8664, "../bad", path, sizeof(path) ) );

    snprintf( path, sizeof(path), "%s/game.wine-nx.txt", dir );
    load_text( &kv, "# written by hand\n" );
    memset( &settings, 0, sizeof(settings) );
    settings.own_controls = settings.address_space = -1;
    settings.vsync = settings.lsfg_performance = settings.lsfg_flow = 1;
    strcpy( settings.title, "Need for Speed" );
    settings.hidden = 1;
    settings.verbose = -1;
    settings.profile = 1;
    settings.framebuffer = 0;
    settings.dxvk = 1;
    strcpy( settings.dxvk_version, "2.7.1" );
    assert( launcher_settings_write( &kv, &settings ) && launcher_kv_save( &kv, path ) );
    assert( launcher_kv_load( &kv, path ) );
    assert( !strcmp( kv.text, "# written by hand\ntitle=Need for Speed\nhidden=1\nprofile=1\nwindows=compositor\nd3d=dxvk\ndxvk-version=2.7.1\n" ) );
    launcher_settings_read( &kv, &back );
    assert( !strcmp( back.title, settings.title ) && back.hidden == 1 && back.verbose == -1 && back.profile == 1 );
    assert( back.framebuffer == 0 && back.dxvk == 1 && !strcmp( back.dxvk_version, "2.7.1" ) );

    /* Back to the global settings: only the comment stays; without it the file goes. */
    memset( &settings, 0, sizeof(settings) );
    settings.verbose = settings.profile = settings.framebuffer = settings.own_controls = settings.address_space = -1;
    settings.vsync = settings.lsfg_performance = settings.lsfg_flow = 1;
    assert( launcher_settings_write( &kv, &settings ) && !strcmp( kv.text, "# written by hand\n" ) );
    assert( launcher_kv_save( &kv, path ) && !access( path, F_OK ) );
    load_text( &kv, "" );
    assert( launcher_kv_save( &kv, path ) && access( path, F_OK ) );
    assert( launcher_kv_save( &kv, path ) );  /* already gone */
    snprintf( path, sizeof(path), "%s/game.wine-nx.txt.new", dir );
    assert( access( path, F_OK ) );

    snprintf( path, sizeof(path), "%s/missing.txt", dir );
    assert( launcher_kv_load( &kv, path ) && kv.size == 0 );

    assert( launcher_settings_on_usb( "ums0:/Games/Test/Game.EXE" ) );
    assert( !launcher_settings_on_usb( "sdmc:/switch/wine/drive_c/Game.EXE" ) );
    assert( launcher_program_settings_path( dir, "ums0:/Games/Test/Game.EXE", path, sizeof(path) ) );
    assert( launcher_program_settings_path( dir, "UMS0:\\games\\test\\game.exe", other, sizeof(other) ) );
    assert( !strcmp( path, other ) );
    assert( strstr( path, "/program-settings/" ) );
    assert( launcher_program_settings_path( dir, "sdmc:/Games/Test/Game.EXE", path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Games/Test/Game.wine-nx.txt" ) );
}

int main(void)
{
    char dir[] = "/tmp/launcher-settings.XXXXXX", command[64];

    assert( mkdtemp( dir ) );
    test_get();
    test_set();
    test_settings( dir );
    snprintf( command, sizeof(command), "rm -rf '%s'", dir );
    system( command );
    printf( "launcher_settings: all tests passed\n" );
    return 0;
}
