/* Shortcuts and batch files in the launcher's file browser (launcher_shortcut.h). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/launcher_shortcut.h"

static const char *present[8];

static int exists( const char *path )
{
    size_t i;

    for (i = 0; i < sizeof(present) / sizeof(present[0]); i++)
        if (present[i] && !strcasecmp( present[i], path )) return 1;
    return 0;
}

static void only( const char *a, const char *b )
{
    memset( present, 0, sizeof(present) );
    present[0] = a;
    present[1] = b;
}

static size_t put16( unsigned char *p, unsigned int v ) { p[0] = v & 0xff; p[1] = v >> 8; return 2; }
static size_t put32( unsigned char *p, unsigned int v ) { put16( p, v & 0xffff ); put16( p + 2, v >> 16 ); return 4; }

static size_t put_string( unsigned char *p, const char *text, int unicode )
{
    size_t len = strlen( text ), i, used = put16( p, (unsigned int)len );

    for (i = 0; i < len; i++)
    {
        p[used++] = (unsigned char)text[i];
        if (unicode) p[used++] = 0;
    }
    return used;
}

/* A shortcut as Windows writes one: an ID list, link info with a local base
 * path, then relative path, working directory and arguments. */
static size_t make_lnk( unsigned char *data, const char *base, const char *relative, const char *workdir,
                        const char *args, int unicode, int wide_info )
{
    static const unsigned char clsid[16] = { 0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
    unsigned int flags = 0x1 | (base ? 0x2 : 0) | 0x4 | (relative ? 0x8 : 0) | (workdir ? 0x10 : 0) | (args ? 0x20 : 0) |
                         (unicode ? 0x80 : 0);
    size_t used = 0x4c, i;

    memset( data, 0, 0x4c );
    put32( data, 0x4c );
    memcpy( data + 4, clsid, sizeof(clsid) );
    put32( data + 0x14, flags );
    used += put16( data + used, 6 );            /* ID list: one item of four bytes and the terminator */
    used += put16( data + used, 4 );
    used += put16( data + used, 0x1234 );
    used += put16( data + used, 0 );
    if (base)
    {
        unsigned char *info = data + used;
        size_t header = wide_info ? 0x24 : 0x1c, at = header, len = strlen( base );

        memset( info, 0, header );
        put32( info + 4, (unsigned int)header );
        put32( info + 8, 1 );
        if (!wide_info)
        {
            put32( info + 0x10, (unsigned int)at );
            memcpy( info + at, base, len + 1 );
            at += len + 1;
            put32( info + 0x18, (unsigned int)at );
            info[at++] = 0;
        }
        else
        {
            put32( info + 0x10, (unsigned int)at );  /* an ANSI path that lost its characters */
            memcpy( info + at, "C:\\?", 5 );
            at += 5;
            put32( info + 0x18, (unsigned int)at );
            info[at++] = 0;
            put32( info + 0x1c, (unsigned int)at );
            for (i = 0; i <= len; i++) at += put16( info + at, (unsigned char)base[i] );
            put32( info + 0x20, (unsigned int)at );
            at += put16( info + at, 0 );
        }
        put32( info, (unsigned int)at );
        used += at;
    }
    used += put_string( data + used, "A game", unicode );
    if (relative) used += put_string( data + used, relative, unicode );
    if (workdir) used += put_string( data + used, workdir, unicode );
    if (args) used += put_string( data + used, args, unicode );
    return used;
}

static void test_names( void )
{
    assert( launcher_is_shortcut( "Game.lnk" ) && launcher_is_shortcut( "GAME.LNK" ) );
    assert( !launcher_is_shortcut( ".lnk" ) && !launcher_is_shortcut( "._Game.lnk" ) && !launcher_is_shortcut( "a.exe" ) );
    assert( launcher_is_batch( "run.bat" ) && launcher_is_batch( "RUN.CMD" ) );
    assert( !launcher_is_batch( "run.bat.txt" ) && !launcher_is_batch( ".bat" ) );
}

static void test_lnk( void )
{
    unsigned char data[2048];
    struct launcher_shortcut link;
    size_t len;

    len = make_lnk( data, "C:\\Games\\Foo\\bin\\foo.exe", "..\\Foo\\bin\\foo.exe", "C:\\Games\\Foo", "-window -nointro", 1, 0 );
    assert( launcher_lnk_parse( data, len, &link ) );
    assert( !strcmp( link.target, "C:\\Games\\Foo\\bin\\foo.exe" ) );
    assert( !strcmp( link.relative, "..\\Foo\\bin\\foo.exe" ) );
    assert( !strcmp( link.workdir, "C:\\Games\\Foo" ) );
    assert( !strcmp( link.args, "-window -nointro" ) );

    /* ANSI strings, no arguments. */
    len = make_lnk( data, "D:\\x\\y.exe", NULL, NULL, NULL, 0, 0 );
    assert( launcher_lnk_parse( data, len, &link ) );
    assert( !strcmp( link.target, "D:\\x\\y.exe" ) && !link.relative[0] && !link.args[0] );

    /* The Unicode base path wins over the ANSI one. */
    len = make_lnk( data, "C:\\Hry\\hra.exe", NULL, NULL, "-w", 1, 1 );
    assert( launcher_lnk_parse( data, len, &link ) );
    assert( !strcmp( link.target, "C:\\Hry\\hra.exe" ) && !strcmp( link.args, "-w" ) );

    /* No link info: the relative path alone. */
    len = make_lnk( data, NULL, ".\\game.exe", NULL, NULL, 1, 0 );
    assert( launcher_lnk_parse( data, len, &link ) );
    assert( !link.target[0] && !strcmp( link.relative, ".\\game.exe" ) );

    /* Not shortcuts, and cut ones: refused or read as far as they go, never past the end. */
    assert( !launcher_lnk_parse( "MZ", 2, &link ) );
    memset( data, 0, sizeof(data) );
    assert( !launcher_lnk_parse( data, 0x4c, &link ) );
    len = make_lnk( data, "C:\\Games\\Foo\\bin\\foo.exe", "foo.exe", "C:\\Games", "-a", 1, 0 );
    for (size_t cut = 0; cut < len; cut++)
    {
        unsigned char copy[2048];

        memcpy( copy, data, cut );
        launcher_lnk_parse( copy, cut, &link );
    }
    put32( data + 0x4c + 8, 0xfffffff0 );  /* link info that claims to be huge */
    assert( !launcher_lnk_parse( data, len, &link ) );
}

static void test_bat( void )
{
    struct launcher_shortcut bat;

    assert( launcher_bat_parse( "@echo off\r\nrem start the game\r\ncd /d \"%~dp0bin\"\r\nstart \"\" game.exe -window %*\r\n", &bat ) );
    assert( !strcmp( bat.target, "game.exe" ) && !strcmp( bat.workdir, "bin" ) && !strcmp( bat.args, "-window" ) );

    assert( launcher_bat_parse( "\"%~dp0Game Folder\\My Game.exe\" -skipintro\n", &bat ) );
    assert( !strcmp( bat.target, "Game Folder\\My Game.exe" ) && !strcmp( bat.args, "-skipintro" ) && !bat.workdir[0] );

    assert( launcher_bat_parse( "@START /D \"C:\\Games\\Foo\" /wait /high foo -x 1\n", &bat ) );
    assert( !strcmp( bat.target, "foo.exe" ) && !strcmp( bat.workdir, "C:\\Games\\Foo" ) && !strcmp( bat.args, "-x 1" ) );

    assert( launcher_bat_parse( ":: comment\n:label\nset X=1\ncall setup.bat\ncall tool.exe /q\n", &bat ) );
    assert( !strcmp( bat.target, "tool.exe" ) && !strcmp( bat.args, "/q" ) );

    assert( launcher_bat_parse( "\xef\xbb\xbf" "cd..\ncd bin\ngame.exe\n", &bat ) );
    assert( !strcmp( bat.target, "game.exe" ) && !strcmp( bat.workdir, "bin" ) && !bat.args[0] );

    /* A line that needs a variable only the PC had is passed over. */
    assert( launcher_bat_parse( "%GAMEDIR%\\game.exe\nother.exe -a\n", &bat ) );
    assert( !strcmp( bat.target, "other.exe" ) );
    assert( launcher_bat_parse( "game.exe -dir %APPDATA%\n", &bat ) && !bat.args[0] );

    assert( !launcher_bat_parse( "", &bat ) );
    assert( !launcher_bat_parse( "@echo off\necho hello\npause\n", &bat ) );
    assert( !launcher_bat_parse( "start readme.txt\ncall other.bat\n", &bat ) );
}

static void test_locate( void )
{
    struct launcher_shortcut link;
    char path[512];

    assert( launcher_shortcut_join( "sdmc:/Roms/Foo", "..\\Bar\\.\\x.exe", path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Bar/x.exe" ) );
    assert( launcher_shortcut_join( "sdmc:/", "..\\..\\x.exe", path, sizeof(path) ) && !strcmp( path, "sdmc:/x.exe" ) );
    assert( launcher_shortcut_join( "ums0:/Wine/", "a/b", path, sizeof(path) ) && !strcmp( path, "ums0:/Wine/a/b" ) );

    /* A shortcut's relative path, from its own folder. */
    memset( &link, 0, sizeof(link) );
    strcpy( link.target, "C:\\Games\\Foo\\bin\\foo.exe" );
    strcpy( link.relative, ".\\bin\\foo.exe" );
    only( "sdmc:/Roms/Foo/bin/foo.exe", NULL );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/foo.exe" ) );

    /* The relative path is the PC's desktop's; the end of the target is found under the folder. */
    strcpy( link.relative, "..\\..\\Games\\Foo\\bin\\foo.exe" );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/foo.exe" ) );

    /* The shortcut lies beside the game's folder, or inside it. */
    assert( launcher_shortcut_resolve( "sdmc:/Roms", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/foo.exe" ) );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo/bin", &link, exists, path, sizeof(path) ) );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo/bin/extra/deep", &link, exists, path, sizeof(path) ) );
    assert( !launcher_shortcut_resolve( "sdmc:/Roms/Foo/bin/a/b/c/d/e", &link, exists, path, sizeof(path) ) );

    /* The longest end wins over a file of the same name nearer by. */
    only( "sdmc:/Roms/foo.exe", "sdmc:/Roms/Foo/bin/foo.exe" );
    assert( launcher_shortcut_resolve( "sdmc:/Roms", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/foo.exe" ) );

    /* Wine-NX's own drives. */
    link.relative[0] = 0;
    only( "sdmc:/switch/wine/drive_c/Games/Foo/bin/foo.exe", NULL );
    assert( launcher_shortcut_resolve( "sdmc:/Elsewhere", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/switch/wine/drive_c/Games/Foo/bin/foo.exe" ) );
    strcpy( link.target, "Z:\\Roms\\a.exe" );
    only( "sdmc:/Roms/a.exe", NULL );
    assert( launcher_shortcut_resolve( "ums0:/x", &link, exists, path, sizeof(path) ) && !strcmp( path, "sdmc:/Roms/a.exe" ) );
    strcpy( link.target, "E:\\Wine\\a.exe" );
    only( "ums1:/Wine/a.exe", NULL );
    assert( launcher_shortcut_resolve( "sdmc:/x", &link, exists, path, sizeof(path) ) && !strcmp( path, "ums1:/Wine/a.exe" ) );

    /* A batch file's cd, then its program. */
    memset( &link, 0, sizeof(link) );
    strcpy( link.target, "game.exe" );
    strcpy( link.workdir, "bin" );
    only( "sdmc:/Roms/Foo/bin/game.exe", "sdmc:/Roms/Foo/game.exe" );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/game.exe" ) );
    strcpy( link.workdir, "C:\\Games\\Foo\\bin" );
    assert( launcher_shortcut_resolve( "sdmc:/Roms/Foo", &link, exists, path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/Roms/Foo/bin/game.exe" ) );

    /* Nothing there, a target that is no program, a network path. */
    only( NULL, NULL );
    assert( !launcher_shortcut_resolve( "sdmc:/Roms/Foo", &link, exists, path, sizeof(path) ) );
    memset( &link, 0, sizeof(link) );
    strcpy( link.target, "C:\\Docs\\readme.txt" );
    only( "sdmc:/Roms/readme.txt", NULL );
    assert( !launcher_shortcut_resolve( "sdmc:/Roms", &link, exists, path, sizeof(path) ) );
    strcpy( link.target, "\\\\server\\share\\a.exe" );
    only( "sdmc:/Roms/a.exe", NULL );
    assert( !launcher_shortcut_resolve( "sdmc:/Roms", &link, exists, path, sizeof(path) ) );
}

int main( void )
{
    test_names();
    test_lnk();
    test_bat();
    test_locate();
    puts( "shortcuts and batch files: all tests passed" );
    return 0;
}
