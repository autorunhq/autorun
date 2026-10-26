/* Which module a static unixlib pairs with (dlls/ntdll/unix/horizon_unixlib_names.h).
 *
 * A FEX variant build arrives as libwow64fex-opt.dll, and FEX asks for its
 * unixlib by the canonical stem with no extension. Both have to find the same
 * entry as the stock libwow64fex.dll, or the module starts with no unixlib
 * behind it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../dlls/ntdll/unix/horizon_unixlib_names.h"

/* Names reach the matcher as UNICODE_STRING.Length / sizeof(WCHAR) characters. */
static int matches( const char *name, const char *stem, int variants )
{
    unsigned short wide[128];
    size_t i, length = strlen( name );

    assert( length < sizeof(wide) / sizeof(wide[0]) );
    for (i = 0; i < length; i++) wide[i] = (unsigned char)name[i];
    return wine_nx_unixlib_name_matches( wide, length, stem, variants );
}

static int is_fex( const char *name )
{
    unsigned short wide[128];
    size_t i, length = strlen( name );

    for (i = 0; i < length; i++) wide[i] = (unsigned char)name[i];
    return wine_nx_unixlib_name_is_fex( wide, length );
}

int main( void )
{
    /* The stock modules, as they have always been named and as FEX asks. */
    assert( matches( "libwow64fex.dll", "libwow64fex", 1 ) );
    assert( matches( "libarm64ecfex.dll", "libarm64ecfex", 1 ) );
    assert( matches( "libwow64fex", "libwow64fex", 1 ) );
    assert( matches( "libarm64ecfex", "libarm64ecfex", 1 ) );

    /* A variant of either, with and without its extension. */
    assert( matches( "libwow64fex-opt.dll", "libwow64fex", 1 ) );
    assert( matches( "libarm64ecfex-opt.dll", "libarm64ecfex", 1 ) );
    assert( matches( "libwow64fex-opt", "libwow64fex", 1 ) );
    assert( matches( "libwow64fex-tune-2.dll", "libwow64fex", 1 ) );

    /* Windows names are not case sensitive and neither is this. */
    assert( matches( "LIBWOW64FEX-OPT.DLL", "libwow64fex", 1 ) );
    assert( matches( "libwow64fex.dll", "LIBWOW64FEX", 1 ) );

    /* Another module that merely begins the same way is not this one. */
    assert( !matches( "libwow64fex2.dll", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex.bak", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex.dll.bak", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex-opt.exe", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex-.dll", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex-", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex opt.dll", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex-opt/../x.dll", "libwow64fex", 1 ) );
    assert( !matches( "libwow64fex", "libarm64ecfex", 1 ) );
    assert( !matches( "libarm64ecfex", "libwow64fex", 1 ) );

    /* Entries that are not shipped under a variant name keep matching exactly,
     * which is what every other entry in the table relies on. */
    assert( matches( "ws2_32.dll", "ws2_32.dll", 0 ) );
    assert( matches( "WS2_32.DLL", "ws2_32.dll", 0 ) );
    assert( !matches( "ws2_32-opt.dll", "ws2_32.dll", 0 ) );
    assert( !matches( "ws2_32", "ws2_32.dll", 0 ) );
    assert( !matches( "ws2_32.dlls", "ws2_32.dll", 0 ) );

    /* A missing name is not a match, and does not fault. */
    assert( !wine_nx_unixlib_name_matches( NULL, 0, "libwow64fex", 1 ) );
    assert( !matches( "", "libwow64fex", 1 ) );

    /* The modules the runtime reports on when nothing pairs with them. */
    assert( is_fex( "libwow64fex.dll" ) );
    assert( is_fex( "libarm64ecfex.dll" ) );
    assert( is_fex( "libwow64fex-opt.dll" ) );
    assert( is_fex( "libarm64ecfex-opt.dll" ) );
    assert( !is_fex( "winebox64.dll" ) );
    assert( !is_fex( "winebox64ec.dll" ) );
    assert( !is_fex( "ws2_32.dll" ) );
    assert( !is_fex( "" ) );
    assert( !wine_nx_unixlib_name_is_fex( NULL, 0 ) );

    /* A name for the log, cut off rather than overrunning the buffer. */
    {
        char text[16];
        unsigned short wide[] = { 'l', 'i', 'b', 'w', 'o', 'w', '6', '4', 'f', 'e', 'x', '-', 'o', 'p', 't', '.', 'd', 'l', 'l' };
        unsigned short odd[] = { 0x2603, 'x' };

        wine_nx_unixlib_name_format( wide, sizeof(wide) / sizeof(wide[0]), text, sizeof(text) );
        assert( !strcmp( text, "libwow64fex-opt" ) );
        wine_nx_unixlib_name_format( wide, 11, text, sizeof(text) ); /* "libwow64fex" */
        assert( !strcmp( text, "libwow64fex" ) );
        wine_nx_unixlib_name_format( odd, 2, text, sizeof(text) );
        assert( !strcmp( text, "?x" ) );
        wine_nx_unixlib_name_format( NULL, 0, text, sizeof(text) );
        assert( !text[0] );
    }

    printf( "horizon unixlib names: ok\n" );
    return 0;
}
