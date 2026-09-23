/* Host test for moving the registry's hives into registry/
 * (dlls/ntdll/unix/horizon_registry_paths.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../dlls/ntdll/unix/horizon_registry_paths.h"

static char dir[256];

static void put( const char *name, const char *text )
{
    char path[512];
    FILE *file;

    snprintf( path, sizeof(path), "%s%s", dir, name );
    assert( (file = fopen( path, "w" )) );
    fputs( text, file );
    fclose( file );
}

static int holds( const char *name, const char *text )
{
    char path[512], buffer[256] = "";
    FILE *file;

    snprintf( path, sizeof(path), "%s%s", dir, name );
    if (!(file = fopen( path, "r" ))) return 0;
    if (!fgets( buffer, sizeof(buffer), file )) buffer[0] = 0;
    fclose( file );
    return !strcmp( buffer, text );
}

static int exists( const char *name )
{
    char path[512];

    snprintf( path, sizeof(path), "%s%s", dir, name );
    return !access( path, F_OK );
}

static void clear(void)
{
    static const char *const names[] = { "system.reg", "user.reg", "system.reg.tmp", "user.reg.tmp",
                                         "registry/system.reg", "registry/user.reg",
                                         "registry/system.reg.tmp", "registry/user.reg.tmp" };
    char path[512];
    unsigned int i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        snprintf( path, sizeof(path), "%s%s", dir, names[i] );
        unlink( path );
    }
}

int main(void)
{
    char path[512];

    snprintf( dir, sizeof(dir), "%s/registry-paths-XXXXXX", getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" );
    assert( mkdtemp( dir ) );
    strcat( dir, "/" );

    /* The hives of an earlier build move, with what the games wrote in them. */
    put( "system.reg", "machine keys of every setup\n" );
    put( "user.reg", "user keys\n" );
    horizon_registry_move_hives( dir );
    assert( !exists( "system.reg" ) && !exists( "user.reg" ) );
    assert( holds( "registry/system.reg", "machine keys of every setup\n" ) );
    assert( holds( "registry/user.reg", "user keys\n" ) );

    /* Every later start: nothing to move, nothing touched. */
    horizon_registry_move_hives( dir );
    assert( holds( "registry/system.reg", "machine keys of every setup\n" ) );

    /* An old hive never goes over one already in registry/: that one is the
     * newer, since the runtime only writes there. */
    put( "system.reg", "stale copy put back by hand\n" );
    horizon_registry_move_hives( dir );
    assert( holds( "registry/system.reg", "machine keys of every setup\n" ) );
    assert( holds( "system.reg", "stale copy put back by hand\n" ) );
    clear();

    /* A save cut short left only the .tmp, which the loader reads: it moves too. */
    put( "user.reg.tmp", "the only copy\n" );
    horizon_registry_move_hives( dir );
    assert( holds( "registry/user.reg.tmp", "the only copy\n" ) && !exists( "user.reg.tmp" ) );
    clear();

    /* A hive and its .tmp go together, and a registry/ holding only a .tmp
     * counts as having that hive. */
    put( "system.reg", "hive\n" );
    put( "system.reg.tmp", "newer, half written\n" );
    horizon_registry_move_hives( dir );
    assert( holds( "registry/system.reg", "hive\n" ) && holds( "registry/system.reg.tmp", "newer, half written\n" ) );
    clear();
    put( "registry/user.reg.tmp", "already moved\n" );
    put( "user.reg", "old\n" );
    horizon_registry_move_hives( dir );
    assert( holds( "user.reg", "old\n" ) && holds( "registry/user.reg.tmp", "already moved\n" ) );
    clear();

    snprintf( path, sizeof(path), "%sregistry", dir );
    rmdir( path );
    dir[strlen( dir ) - 1] = 0;
    rmdir( dir );
    puts( "registry paths: hives and their .tmp move to registry/ once, never over what is already there" );
    return 0;
}
