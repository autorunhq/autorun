#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/fex_options.h"
#include "../source/launcher_settings.h"

int main(void)
{
    struct launcher_kv kv = {{0}, 0};
    const struct nx_fex_option *option;
    char value[64];
    int main_count = 0, advanced_count = 0, i;

    for (i = 0; i < NX_FEX_OPTION_COUNT; i++)
    {
        option = nx_fex_options + i;
        assert( (int)option->id == i );
        assert( option->default_choice < option->value_count );
        assert( nx_fex_option_choice( option, nx_fex_option_default( option ) ) == option->default_choice );
        if (option->advanced) advanced_count++;
        else main_count++;
    }
    assert( main_count == 7 && advanced_count == 12 );
    option = nx_fex_options + NX_FEX_TSO;
    assert( !strcmp( option->name, "FEX_TSOENABLED" ) );
    assert( !strcmp( nx_fex_option_default( option ), "0" ) );
    assert( nx_fex_option_choice( option, "1" ) == 1 );
    assert( nx_fex_option_choice( option, "enabled" ) < 0 );
    option = nx_fex_options + NX_FEX_SMC;
    assert( nx_fex_option_choice( option, "MTRACK" ) == 1 );
    assert( nx_fex_option_choice( option, "invalid" ) < 0 );
    option = nx_fex_options + NX_FEX_MAXINST;
    assert( !strcmp( nx_fex_option_default( option ), "1000" ) );

    assert( launcher_kv_set( &kv, "cpu", "fex" ) );
    assert( launcher_kv_set( &kv, "FEX_TSOENABLED", "1" ) );
    assert( launcher_kv_set( &kv, "FEX_MAXINST", "2500" ) );
    assert( launcher_kv_get( &kv, "FEX_TSOENABLED", value, sizeof(value) ) && !strcmp( value, "1" ) );
    assert( nx_fex_option_choice( nx_fex_options + NX_FEX_MAXINST, "2500" ) == 3 );
    assert( launcher_kv_set( &kv, "FEX_TSOENABLED", NULL ) );
    assert( !launcher_kv_get( &kv, "FEX_TSOENABLED", value, sizeof(value) ) );
    assert( launcher_kv_get( &kv, "cpu", value, sizeof(value) ) && !strcmp( value, "fex" ) );

    puts( "FEX options: catalog, Horizon defaults and program settings passed" );
    return 0;
}
