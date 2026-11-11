/* What the settings menu shows in the console's language, for
 * tests/check_menu_languages.py: the catalog is read by the menu's own
 * reader (source/carafe_settings.c, built in here so its static functions
 * can be asked) and every text is printed as the menu would draw it.
 *
 *   menu_languages CATALOG [KEY=TEXT ...] [set:KEY=TEXT ...]
 *
 * CONTROLS_TEST_LANGUAGE in the environment names the console's language
 * (tests/controls-shims/switch.h). Each KEY=TEXT asks which value row a
 * settings file holding that text for the key would select. Each
 * set:KEY=TEXT puts the menu's row on that value; the rows the menu then
 * shows and the ones it greys out are listed after them
 * (tests/check_menu_builds.py). */
#include "../source/carafe_settings.c"
#include "../source/dxvk_options.h"

#include <assert.h>

static const char *catalog_path;

void *carafeOverlayReadRomfsFile( const char *path, size_t limit, size_t *size )
{
    FILE *file;
    long length;
    char *data;

    assert( !strcmp( path, MENU_FILE ) );
    file = fopen( catalog_path, "rb" );
    assert( file != NULL );
    assert( fseek( file, 0, SEEK_END ) == 0 && (length = ftell( file )) > 0 && (size_t)length <= limit );
    assert( fseek( file, 0, SEEK_SET ) == 0 );
    data = malloc( (size_t)length );
    assert( data != NULL && fread( data, 1, (size_t)length, file ) == (size_t)length );
    fclose( file );
    *size = (size_t)length;
    return data;
}

bool carafeOverlayReadRomfsText( const char *path, char *out, size_t size )
{
    (void)path;
    if (size) out[0] = 0;
    return false;
}

void carafeOverlayLog( const char *line ) { fprintf( stderr, "%s\n", line ); }
void carafeOverlayCommit( const char *reason ) { (void)reason; }
void carafeRegistryReset( void ) {}
void carafeRegistryTrialClear( void ) {}
int wine_nx_swkbd_auto_enabled = 1;
struct input_profile wine_nx_input_profile;
u64 controls_test_tick;

int main( int argc, char **argv )
{
    CarafeCatalog *catalog;
    int i, j, k;

    assert( argc >= 2 );
    catalog_path = argv[1];
    catalog = carafeCatalogLoad();
    assert( catalog != NULL );

    printf( "language\t%d\n", (int)carafeLanguage() );
    for (i = 0; i < catalog->section_count; i++)
    {
        const CarafeSection *section = &catalog->sections[i];
        printf( "section\t%s\t%s\n", section->id, carafeText( section->name ) );
        for (j = 0; j < section->preset_count; j++)
            printf( "preset\t%s\t%s\t%s\n", section->presets[j].id, carafeText( section->presets[j].name ),
                    carafeText( section->presets[j].hint ) );
    }
    for (i = 0; i < catalog->group_count; i++)
    {
        const CarafeGroup *group = &catalog->groups[i];
        printf( "group\t%s\t%s\n", group->id, carafeText( group->name ) );
        for (j = 0; j < group->page_count; j++)
            printf( "page\t%s\t%s\n", group->pages[j].id, carafeText( group->pages[j].name ) );
    }
    for (i = 0; i < catalog->setting_count; i++)
    {
        const CarafeSetting *setting = &catalog->settings[i];
        printf( "setting\t%s\t%s\t%s\t%s\n", setting->id, setting->key, carafeText( setting->name ),
                carafeText( setting->hint ) );
        for (j = 0; j < setting->value_count; j++)
            printf( "value\t%s\t%s\t%s\n", setting->key, setting->values[j].text,
                    carafeText( setting->values[j].label ) );
    }
    for (i = 0; i < catalog->suggest_count; i++)
        printf( "suggest\t%s\t%d\t%s\n", catalog->settings[catalog->suggests[i].setting].key,
                catalog->suggests[i].value, carafeText( catalog->suggests[i].why ) );
    for (i = 0; i < catalog->pad_count; i++)
        printf( "pad\t0x%x\t%s\n", catalog->pads[i].code, carafeText( catalog->pads[i].label ) );
    for (i = 0; i < catalog->key_count; i++)
        printf( "key\t0x%x\t%s\n", catalog->keys[i].code, carafeText( catalog->keys[i].label ) );

    /* What FEX does when a program's file says nothing of an option. */
    for (k = 0; k < (int)NX_FEX_OPTION_COUNT; k++)
        printf( "fex\t%s\t%s\n", nx_fex_options[k].name, nx_fex_option_default( &nx_fex_options[k] ) );

    /* Which of FEX's options only the build with Carafe's patches reads, and
     * for each DXVK option and each DXVK build whether some version of the
     * build reads it, as the launcher and the runtime decide it. */
    for (k = 0; k < (int)NX_FEX_OPTION_COUNT; k++)
        printf( "fexcarafe\t%s\t%d\n", nx_fex_options[k].name, nx_fex_option_carafe( &nx_fex_options[k] ) );
    for (k = 0; k < (int)dxvk_option_count; k++)
    {
        static const char *const builds[DXVK_SOURCE_COUNT] =
        {
            [DXVK_SOURCE_OFFICIAL] = "-", [DXVK_SOURCE_SAREK] = "sarek",
            [DXVK_SOURCE_GPLASYNC] = "gplasync", [DXVK_SOURCE_CARAFE] = "carafe",
        };
        int source;

        for (source = 0; source < DXVK_SOURCE_COUNT; source++)
        {
            unsigned int version, read = 0;

            /* Every version a release can have: 1.0.0 to 3.1.1. */
            for (version = 10000; version <= 30101 && !read; version++)
            {
                if (source == DXVK_SOURCE_SAREK && version > 11300) break;
                if (source == DXVK_SOURCE_GPLASYNC && version < 20100) continue;
                read = dxvk_option_supported( &dxvk_options[k], source, version );
            }
            printf( "dxvkreads\t%s\t%s\t%u\n", dxvk_options[k].name, builds[source], read );
        }
    }

    for (k = 2; k < argc; k++)
    {
        char key[KEY_SIZE];
        const char *pair = strncmp( argv[k], "set:", 4 ) ? argv[k] : argv[k] + 4;
        const char *equals = strchr( pair, '=' );
        int index;

        assert( equals != NULL && (size_t)(equals - pair) < sizeof(key) );
        snprintf( key, sizeof(key), "%.*s", (int)(equals - pair), pair );
        index = carafeCatalogFindKey( catalog, CarafeFileProgram, key );
        assert( index >= 0 );
        j = valueIndex( &catalog->settings[index], equals + 1, true );
        if (pair == argv[k])
        {
            /* The row the menu shows for it, and what the launcher and the
             * runtime make of a program's file that holds just this. */
            struct launcher_kv *kv = calloc( 1, sizeof(*kv) );
            struct launcher_settings settings;

            assert( kv != NULL && launcher_kv_set( kv, key, equals + 1 ) );
            launcher_settings_read( kv, &settings );
            printf( "stored\t%s\t%s\t%s\n", key, equals + 1, j < 0 ? "?" : catalog->settings[index].values[j].text );
            printf( "launcher\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n", key, equals + 1, settings.orchestrator,
                    settings.lock_boost, settings.server_pool, settings.fex_carafe, (int)settings.dxvk_source );
            free( kv );
        }
        else
        {
            assert( j >= 0 );
            catalog->settings[index].current = j;
        }
    }
    for (i = 0; i < catalog->setting_count; i++)
        printf( "row\t%s\t%d\t%d\n", catalog->settings[i].key,
                carafeSettingVisible( catalog, &catalog->settings[i] ),
                carafeWhenHolds( catalog, &catalog->settings[i].when, CarafeWhenOn ) );
    for (i = 0; i < catalog->group_count; i++)
        printf( "grouprow\t%s\t%d\n", catalog->groups[i].id,
                carafeWhenHolds( catalog, &catalog->groups[i].when, CarafeWhenShow ) );
    carafeCatalogFree( catalog );
    return 0;
}
