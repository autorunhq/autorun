/* Host test for a program's Box64 options file lines (source/box64_options.h). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/box64_options.h"
#include "../source/launcher_settings.h"

static void load_text( struct launcher_kv *kv, const char *text )
{
    kv->size = strlen( text );
    assert( kv->size < sizeof(kv->text) );
    memcpy( kv->text, text, kv->size + 1 );
}

int main(void)
{
    const struct nx_box64_option *option;
    struct launcher_kv kv;
    char name[32];
    char text[64];
    int main_count = 0, advanced_count = 0, i;
    long value;

    assert( nx_box64_option_line( "BOX64_DYNAREC_SAFEFLAGS=0\n", name, sizeof(name), &value ) );
    assert( !strcmp( name, "BOX64_DYNAREC_SAFEFLAGS" ) && value == 0 );
    /* Spaces around both sides, a trailing comment and CRLF. */
    assert( nx_box64_option_line( "  BOX64_DYNAREC_FORWARD = 1024  # longer jumps\r\n", name, sizeof(name), &value ) );
    assert( !strcmp( name, "BOX64_DYNAREC_FORWARD" ) && value == 1024 );
    assert( nx_box64_option_line( "BOX64_DYNAREC_BIGBLOCK=0x3", name, sizeof(name), &value ) && value == 3 );
    /* Nothing to apply: comments, blank lines, no value, junk after it, no name, a name too long. */
    assert( !nx_box64_option_line( "# BOX64_DYNAREC_BIGBLOCK=3\n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "   \n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "BOX64_DYNAREC_BIGBLOCK=\n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "BOX64_DYNAREC_BIGBLOCK=3 fast\n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "=3\n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "BOX64_DYNAREC_ALIGNED_ATOMICS_AND_MORE=1\n", name, sizeof(name), &value ) );
    assert( !nx_box64_option_line( "BOX64_DYNAREC_BIGBLOCK 3\n", name, sizeof(name), &value ) );

    for (i = 0; i < NX_BOX64_OPTION_COUNT; i++)
    {
        assert( (int)nx_box64_options[i].id == i );
        assert( nx_box64_option_choice( nx_box64_options + i, nx_box64_options[i].default_value ) >= 0 );
        if (nx_box64_options[i].advanced) advanced_count++;
        else main_count++;
    }
    assert( main_count == 6 && advanced_count == 12 );
    option = nx_box64_option_find( "BOX64_DYNAREC_PURGE_AGE" );
    assert( option && nx_box64_option_choice( option, 1000 ) >= 0 && nx_box64_option_choice( option, 4096 ) < 0 );
    option = nx_box64_option_find( "BOX64_DYNAREC_FORWARD" );
    assert( option && nx_box64_option_choice( option, 1024 ) >= 0 );
    assert( nx_box64_option_choice( option, 64 ) < 0 );
    option = nx_box64_option_find( "BOX64_DYNAREC_STRONGMEM" );
    assert( option && nx_box64_option_choice( option, 4 ) >= 0 );
    assert( nx_box64_option_find( "BOX64_DYNAREC_NOARCH" ) );
    assert( !nx_box64_option_find( "BOX64_LINUX_ONLY" ) );

    load_text( &kv, "# keep this comment\nFUTURE_FLAG=9\nBOX64_DYNAREC_BIGBLOCK=3\n" );
    assert( launcher_kv_get( &kv, "BOX64_DYNAREC_BIGBLOCK", text, sizeof(text) ) );
    assert( nx_box64_option_parse_value( text, &value ) && value == 3 );
    assert( launcher_kv_set( &kv, "BOX64_DYNAREC_BIGBLOCK", "2" ) );
    assert( strstr( kv.text, "# keep this comment\n" ) && strstr( kv.text, "FUTURE_FLAG=9\n" ) );
    assert( strstr( kv.text, "BOX64_DYNAREC_BIGBLOCK=2\n" ) );
    assert( launcher_kv_set( &kv, "BOX64_DYNAREC_BIGBLOCK", NULL ) );
    assert( !strstr( kv.text, "BOX64_DYNAREC_BIGBLOCK" ) );
    assert( strstr( kv.text, "# keep this comment\n" ) && strstr( kv.text, "FUTURE_FLAG=9\n" ) );

    puts( "Box64 options: catalog, validation and preserved file entries passed" );
    return 0;
}
