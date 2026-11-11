#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fex_build.h"

#define SYSTEM_DLL "drive_c/windows/system32/libwow64fex.dll"
#define KEEP_DIR   "drive_c/fex-carafe"
/* Not libwow64fex.dll: a file of that name here would read as a release
 * installed without a version. */
#define STOCK_COPY KEEP_DIR "/stock-libwow64fex.dll"
/* Which Carafe DLL system32 holds, and its size: there only while one does. */
#define MARK       KEEP_DIR "/in-system32.txt"

static int join( char *out, size_t size, const char *root, const char *name )
{
    int length = snprintf( out, size, "%s/%s", root, name );

    return length > 0 && (size_t)length < size;
}

static long long file_size( const char *path )
{
    struct stat st;

    if (stat( path, &st ) || !S_ISREG( st.st_mode )) return -1;
    return (long long)st.st_size;
}

/* 1 the same bytes, 0 not, -1 one of them cannot be read. */
static int same_content( const char *a, const char *b )
{
    static unsigned char left[65536], right[65536];
    long long size_a = file_size( a ), size_b = file_size( b );
    FILE *fa, *fb;
    int same = 1;

    if (size_a < 0 || size_b < 0) return -1;
    if (size_a != size_b) return 0;
    if (!(fa = fopen( a, "rb" ))) return -1;
    if (!(fb = fopen( b, "rb" ))) { fclose( fa ); return -1; }
    while (same == 1)
    {
        size_t got_a = fread( left, 1, sizeof(left), fa ), got_b = fread( right, 1, sizeof(right), fb );

        if (ferror( fa ) || ferror( fb )) same = -1;
        else if (got_a != got_b || memcmp( left, right, got_a )) same = 0;
        else if (!got_a) break;
    }
    fclose( fa );
    fclose( fb );
    return same;
}

/* The copy is whole under another name before it takes the place of what was
 * there; the card's file system does not rename over a file. */
static int copy_file( const char *from, const char *to )
{
    static unsigned char buffer[65536];
    char temp[1024];
    FILE *in, *out;
    size_t got;
    int ok = 1;

    if ((size_t)snprintf( temp, sizeof(temp), "%s.new", to ) >= sizeof(temp)) return 0;
    if (!(in = fopen( from, "rb" ))) return 0;
    if (!(out = fopen( temp, "wb" ))) { fclose( in ); return 0; }
    while ((got = fread( buffer, 1, sizeof(buffer), in )))
        if (fwrite( buffer, 1, got, out ) != got) { ok = 0; break; }
    if (ferror( in ) || fflush( out )) ok = 0;
    fclose( in );
    if (fclose( out )) ok = 0;
    if (ok && file_size( temp ) != file_size( from )) ok = 0;
    if (!ok) { remove( temp ); return 0; }
    remove( to );
    if (rename( temp, to )) { remove( temp ); return 0; }
    return 1;
}

static int read_mark( const char *mark, char *path, size_t size, long long *bytes )
{
    char line[1100];
    FILE *file = fopen( mark, "rb" );
    int ok = 0;

    path[0] = 0;
    *bytes = -1;
    if (!file) return 0;
    if (fgets( line, sizeof(line), file ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (line[0] && strlen( line ) < size)
        {
            strcpy( path, line );
            ok = 1;
            if (fgets( line, sizeof(line), file )) *bytes = strtoll( line, NULL, 10 );
        }
    }
    fclose( file );
    return ok;
}

static int write_mark( const char *mark, const char *path, long long bytes )
{
    FILE *file = fopen( mark, "wb" );
    int ok;

    if (!file) return 0;
    ok = fprintf( file, "%s\n%lld\n", path, bytes ) > 0 && !fflush( file );
    if (fclose( file )) ok = 0;
    if (!ok) remove( mark );
    return ok;
}

/* Whether system32's file is the Carafe DLL the mark names: 1, 0, or -1 when
 * there is no file in system32. Its bytes say, while the DLL it was copied
 * from is still installed; its size, once that release has been removed. */
static int system_is_marked( const char *system, const char *marked, long long bytes )
{
    long long size = file_size( system );
    int same;

    if (size < 0) return -1;
    if ((same = same_content( system, marked )) >= 0) return same;
    return bytes >= 0 && size == bytes;
}

enum fex_build_result wine_nx_fex_build_select( const char *runtime_dir, const char *carafe, int *changed )
{
    char system[1024], keep[1024], stock[1024], mark[1024], marked[1024];
    long long bytes;
    int have_mark, is_marked;

    if (changed) *changed = 0;
    if (!runtime_dir || !join( system, sizeof(system), runtime_dir, SYSTEM_DLL ) ||
        !join( keep, sizeof(keep), runtime_dir, KEEP_DIR ) ||
        !join( stock, sizeof(stock), runtime_dir, STOCK_COPY ) ||
        !join( mark, sizeof(mark), runtime_dir, MARK )) return FEX_BUILD_IO;
    have_mark = read_mark( mark, marked, sizeof(marked), &bytes );

    if (carafe)
    {
        long long wanted = file_size( carafe );
        int same;

        if (wanted <= 0 || strlen( carafe ) >= sizeof(marked)) return FEX_BUILD_NO_CARAFE;
        if ((same = same_content( system, carafe )) == 1)
        {
            if (!have_mark || strcmp( marked, carafe ) || bytes != wanted) write_mark( mark, carafe, wanted );
            return FEX_BUILD_CARAFE;
        }
        mkdir( keep, 0777 );
        /* What system32 holds is the stock DLL unless the mark says it is a
         * Carafe build: then the copy kept from before it is the stock one. */
        is_marked = have_mark ? system_is_marked( system, marked, bytes ) : 0;
        if (!is_marked && file_size( system ) >= 0 && !copy_file( system, stock )) return FEX_BUILD_IO;
        /* The mark first: were the copy to fail after it, system32 would not
         * match it and would be taken for the stock DLL, which it would be. */
        if (!write_mark( mark, carafe, wanted )) return FEX_BUILD_IO;
        if (!copy_file( carafe, system ))
        {
            if (file_size( system ) < 0 && file_size( stock ) >= 0) copy_file( stock, system );
            return FEX_BUILD_IO;
        }
        if (changed) *changed = 1;
        return FEX_BUILD_CARAFE;
    }

    if (!have_mark) return FEX_BUILD_STOCK;
    is_marked = system_is_marked( system, marked, bytes );
    if (!is_marked)
    {
        /* Something else is there since: the DLL repository's, from an update. */
        remove( mark );
        return FEX_BUILD_STOCK;
    }
    if (file_size( stock ) < 0) return FEX_BUILD_NO_STOCK;
    if (!copy_file( stock, system )) return FEX_BUILD_IO;
    remove( mark );
    if (changed) *changed = 1;
    return FEX_BUILD_STOCK;
}
