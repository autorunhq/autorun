#!/usr/bin/env python3
"""The runtime's settings file (horizon-wine/source/config_json.h): one JSON
object, read whole and written whole, where a dozen files whose names were the
question and whose presence was the answer used to be.

A key this build does not know has to survive being read and written again, or
a card moved between builds would lose a setting every time."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'horizon-wine/source/config_json.h').read_text()

fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@HEADER@

static const char *written( const struct wine_nx_config *config, char *out, size_t size )
{
    FILE *file = tmpfile();
    long len;

    assert( file && wine_nx_config_write( config, file ) );
    fflush( file );
    len = ftell( file );
    rewind( file );
    assert( (size_t)len < size && fread( out, 1, (size_t)len, file ) == (size_t)len );
    out[len] = 0;
    fclose( file );
    return out;
}

int main( int argc, char **argv )
{
    struct wine_nx_config config;
    char text[4096], value[256];
    const char *path = argc > 1 ? argv[1] : "settings.json";

    /* Nothing at all is every setting at its default. */
    assert( !wine_nx_config_load( &config, "/nonexistent/settings.json" ) );
    assert( wine_nx_config_bool( &config, "core-balancing", 1 ) == 1 );
    assert( wine_nx_config_bool( &config, "verbose-log", 0 ) == 0 );

    assert( wine_nx_config_parse( &config,
        "{\n"
        "  \"core-balancing\": true,\n"
        "  \"verbose-log\": false,\n"
        "  \"profiler\": 1,\n"
        "  \"scale\": 2.5,\n"
        "  \"target\": \"C:\\\\Halo\\\\HALO.EXE\",\n"
        "  \"something-a-later-build-added\": {\"deep\": [1, 2]}\n"
        "}\n" ) );
    assert( config.count == 6 );
    assert( wine_nx_config_bool( &config, "core-balancing", 0 ) == 1 );
    assert( wine_nx_config_bool( &config, "verbose-log", 1 ) == 0 );
    assert( wine_nx_config_bool( &config, "profiler", 0 ) == 1 );   /* 1 reads as true */
    assert( wine_nx_config_bool( &config, "missing", 1 ) == 1 );
    /* A string is not a yes or a no, and must not be read as one. */
    assert( wine_nx_config_bool( &config, "target", 1 ) == 1 );
    assert( wine_nx_config_bool( &config, "target", 0 ) == 0 );
    assert( wine_nx_config_string_value( &config, "target", value, sizeof(value) ) );
    assert( !strcmp( value, "C:\\Halo\\HALO.EXE" ) );
    assert( !wine_nx_config_string_value( &config, "core-balancing", value, sizeof(value) ) );

    /* Changing one leaves the rest, the unknown one included and in its place. */
    assert( wine_nx_config_set_bool( &config, "verbose-log", 1 ) );
    assert( wine_nx_config_set_bool( &config, "windows-through-opengl", 0 ) );
    written( &config, text, sizeof(text) );
    assert( strstr( text, "\"verbose-log\": true" ) );
    assert( strstr( text, "\"something-a-later-build-added\": {\"deep\": [1, 2]}" ) );
    assert( strstr( text, "\"windows-through-opengl\": false" ) );
    assert( strstr( text, "\"scale\": 2.5" ) );
    /* And what was written reads back the same. */
    {
        struct wine_nx_config again;

        assert( wine_nx_config_parse( &again, text ) && again.count == 7 );
        assert( wine_nx_config_bool( &again, "verbose-log", 0 ) == 1 );
        assert( wine_nx_config_string_value( &again, "target", value, sizeof(value) ) );
        assert( !strcmp( value, "C:\\Halo\\HALO.EXE" ) );
    }

    /* Through a file, which is how it is really used. */
    assert( wine_nx_config_save( &config, path ) );
    assert( wine_nx_config_load( &config, path ) && config.count == 7 );
    assert( wine_nx_config_bool( &config, "windows-through-opengl", 1 ) == 0 );

    /* Half a file gives up what it holds rather than everything. */
    assert( !wine_nx_config_parse( &config, "{ \"core-balancing\": true, \"verbose-log\": fal" ) );
    assert( wine_nx_config_bool( &config, "core-balancing", 0 ) == 1 );
    /* And something that is not an object at all is nothing. */
    assert( !wine_nx_config_parse( &config, "core-balancing=1\n" ) && !config.count );
    /* Cut short just after a backslash, in a value or in a key: the end is not
     * escaped, and nothing past it is read (the address sanitizer watches a
     * buffer of exactly that size). */
    {
        static const char *const cut[] = { "{ \"core-balancing\": true, \"target\": \"C:\\\\Games\\",
                                           "{ \"core-balancing\": true, \"tar\\" };
        unsigned int k;

        for (k = 0; k < 2; k++)
        {
            size_t len = strlen( cut[k] );
            char *copy = malloc( len + 1 );

            assert( copy );
            memcpy( copy, cut[k], len + 1 );
            assert( !wine_nx_config_parse( &config, copy ) && config.count == 1 );
            assert( wine_nx_config_bool( &config, "core-balancing", 0 ) == 1 );
            free( copy );
        }
    }
    /* A key with a quote or a backslash in it is written so that it reads back. */
    {
        struct wine_nx_config again;

        assert( wine_nx_config_parse( &config, "{\"a\\\"b\\\\c\": 1, \"after\": true}" ) && config.count == 2 );
        assert( !strcmp( config.entries[0].key, "a\"b\\c" ) );
        written( &config, text, sizeof(text) );
        assert( wine_nx_config_parse( &again, text ) && again.count == 2 );
        assert( !strcmp( again.entries[0].key, "a\"b\\c" ) );
        assert( wine_nx_config_bool( &again, "after", 0 ) == 1 );
    }

    printf( "settings: booleans, strings, defaults, a key a later build added, and a file cut short\n" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'config.c'
    source.write_text(fixture.replace('@HEADER@', header))
    binary = Path(tmp) / 'config'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-o', str(binary), str(source)], check=True)
    out = subprocess.run([str(binary), str(Path(tmp) / 'settings.json')], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(out.stdout + out.stderr)
    print(out.stdout.strip())
