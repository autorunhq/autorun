/* What the settings menu shows in the console's language, for
 * tests/check_menu_languages.py: the catalog is read by the menu's own
 * reader (source/carafe_settings.c, built in here so its static functions
 * can be asked) and every text is printed as the menu would draw it.
 *
 *   menu_languages CATALOG [KEY=TEXT ...]
 *
 * CONTROLS_TEST_LANGUAGE in the environment names the console's language
 * (tests/controls-shims/switch.h). Each KEY=TEXT asks which value row a
 * settings file holding that text for the key would select. */
#include "../source/carafe_settings.c"

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

    for (k = 2; k < argc; k++)
    {
        char key[KEY_SIZE];
        const char *equals = strchr( argv[k], '=' );
        int index;

        assert( equals != NULL && (size_t)(equals - argv[k]) < sizeof(key) );
        snprintf( key, sizeof(key), "%.*s", (int)(equals - argv[k]), argv[k] );
        index = carafeCatalogFindKey( catalog, CarafeFileProgram, key );
        assert( index >= 0 );
        j = valueIndex( &catalog->settings[index], equals + 1, true );
        printf( "stored\t%s\t%s\t%s\n", key, equals + 1, j < 0 ? "?" : catalog->settings[index].values[j].text );
    }
    carafeCatalogFree( catalog );
    return 0;
}
