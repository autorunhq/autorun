#include <assert.h>
#include <stdio.h>
#include "../source/dxvk_options.h"

static const struct dxvk_option *find( const char *name, enum dxvk_source source, const char *version )
{
    unsigned int release = dxvk_options_version( source, version );
    const struct dxvk_option *found = NULL;

    for (unsigned int i = 0; i < dxvk_option_count; i++)
        if (!strcmp( dxvk_options[i].name, name ) && dxvk_option_supported( dxvk_options + i, source, release ))
        {
            assert( !found );
            found = dxvk_options + i;
        }
    return found;
}

static void versions(void)
{
    assert( dxvk_options_version( DXVK_SOURCE_OFFICIAL, "1.0" ) == 10000 );
    assert( dxvk_options_version( DXVK_SOURCE_OFFICIAL, "v3.1.1" ) == 30101 );
    assert( dxvk_options_version( DXVK_SOURCE_GPLASYNC, "2.7.1-1" ) == 20701 );
    assert( dxvk_options_version( DXVK_SOURCE_SAREK, "1.10.3-20230507" ) == 11003 );
    assert( !dxvk_options_version( DXVK_SOURCE_OFFICIAL, "3.2" ) );
    assert( !dxvk_options_version( DXVK_SOURCE_SAREK, "2.0" ) );
    assert( !dxvk_options_version( DXVK_SOURCE_GPLASYNC, "2.0-1" ) );
    static const char *const invalid[] = { "", "Bundled", "../3.1", "3.", "3.1.", "3.1.1.1", "3.0-rc1", "999999999999999999999.0" };
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert( !dxvk_options_version( DXVK_SOURCE_OFFICIAL, invalid[i] ) );
    assert( !dxvk_options_version( DXVK_SOURCE_COUNT, "3.1.1" ) );
    assert( !dxvk_options_version( DXVK_SOURCE_OFFICIAL, NULL ) );

    assert( find( "dxvk.enableStateCache", DXVK_SOURCE_OFFICIAL, "2.6.2" ) );
    assert( !find( "dxvk.enableStateCache", DXVK_SOURCE_OFFICIAL, "2.7" ) );
    assert( find( "dxvk.enableStateCache", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( find( "dxvk.enableGraphicsPipelineLibrary", DXVK_SOURCE_OFFICIAL, "2.0" ) );
    assert( !find( "dxvk.enableGraphicsPipelineLibrary", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( !find( "dxvk.maxMemoryBudget", DXVK_SOURCE_SAREK, "1.12.0" ) );
    assert( find( "dxvk.maxMemoryBudget", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( find( "dxvk.enableDyasync", DXVK_SOURCE_SAREK, "1.12.0" ) );
    assert( !find( "dxvk.enableDyasync", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( !find( "dxvk.shaderCompilationMethod", DXVK_SOURCE_SAREK, "1.12.0" ) );
    assert( find( "dxvk.shaderCompilationMethod", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( find( "dxvk.gplAsyncCache", DXVK_SOURCE_GPLASYNC, "2.6-1" ) );
    assert( !find( "dxvk.gplAsyncCache", DXVK_SOURCE_GPLASYNC, "2.7-1" ) );
    assert( !find( "dxvk.gplAsyncCache", DXVK_SOURCE_OFFICIAL, "2.6" ) );
    assert( !find( "dxvk.gplAsyncCache", DXVK_SOURCE_GPLASYNC, "2.1-1" ) );
    assert( !find( "d3d9.modeCountCompatibility", DXVK_SOURCE_OFFICIAL, "2.7" ) );
    assert( find( "d3d9.modeCountCompatibility", DXVK_SOURCE_OFFICIAL, "2.7.1" ) );
    assert( !find( "d3d11.disableDirectImageMapping", DXVK_SOURCE_OFFICIAL, "2.6.1" ) );
    assert( find( "d3d11.disableDirectImageMapping", DXVK_SOURCE_OFFICIAL, "2.6.2" ) );
    assert( find( "d3d11.zeroInitWorkgroupMemory", DXVK_SOURCE_OFFICIAL, "1.0" ) );
    assert( !find( "d3d11.zeroWorkgroupMemory", DXVK_SOURCE_OFFICIAL, "1.10.3" ) );
    assert( !find( "d3d11.relaxedBarriers", DXVK_SOURCE_OFFICIAL, "3.1.1" ) );
    assert( !find( "dxvk.enableDescriptorBuffer", DXVK_SOURCE_OFFICIAL, "2.6.2" ) );
    assert( find( "dxvk.enableDescriptorBuffer", DXVK_SOURCE_OFFICIAL, "2.7" ) );
    assert( find( "dxvk.enableDescriptorBuffer", DXVK_SOURCE_GPLASYNC, "2.7-1" ) );
    assert( !find( "dxvk.enableDescriptorBuffer", DXVK_SOURCE_SAREK, "1.13.0" ) );
    assert( !find( "dxvk.enableDescriptorHeap", DXVK_SOURCE_OFFICIAL, "2.7.1" ) );
    assert( find( "dxvk.enableDescriptorHeap", DXVK_SOURCE_OFFICIAL, "3.0" ) );
    assert( !find( "dxvk.enableImplicitResolves", DXVK_SOURCE_OFFICIAL, "2.7.1" ) );
    assert( find( "dxvk.enableImplicitResolves", DXVK_SOURCE_OFFICIAL, "3.0" ) );
    assert( !find( "dxvk.latencySleep", DXVK_SOURCE_OFFICIAL, "2.5.3" ) );
    assert( find( "dxvk.latencySleep", DXVK_SOURCE_OFFICIAL, "2.6" ) );
}

static void configuration(void)
{
    struct launcher_kv kv = {0};
    struct launcher_settings settings;
    char config[LAUNCHER_KV_MAX], before[LAUNCHER_KV_MAX];
    const struct dxvk_option *option;

    assert( launcher_kv_set( &kv, "FEX_MAXINST", "5000" ) );
    assert( launcher_kv_set( &kv, "dxvk.numCompilerThreads", "3" ) );
    assert( launcher_kv_set( &kv, "dxvk.enableStateCache", "False" ) );
    assert( launcher_kv_set( &kv, "dxvk.maxMemoryBudget", "1536" ) );
    assert( launcher_kv_set( &kv, "dxvk.shaderCompilationMethod", "\"async\"" ) );
    assert( launcher_kv_set( &kv, "dxvk.gplAsyncCache", "True" ) );
    assert( launcher_kv_set( &kv, "d3d9.floatEmulation", "Strict" ) );
    assert( launcher_kv_set( &kv, "d3d9.deviceLocalConstantBuffers", "Auto" ) );
    launcher_settings_read( &kv, &settings );
    settings.dxvk_hud = 2;
    settings.frame_limit = 1;
    assert( launcher_settings_write( &kv, &settings ) );
    assert( strstr( kv.text, "dxvk.numCompilerThreads=3" ) && strstr( kv.text, "FEX_MAXINST=5000" ) );
    assert( launcher_dxvk_config( &settings, config, sizeof(config) ) );
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.numCompilerThreads = 3\n" ) );
    assert( strstr( config, "dxvk.maxMemoryBudget = 1536\n" ) );
    assert( strstr( config, "d3d9.deviceLocalConstantBuffers = Auto\n" ) );
    assert( !strstr( config, "enableStateCache" ) && !strstr( config, "shaderCompilationMethod" ) );
    assert( !strstr( config, "gplAsyncCache" ) && !strstr( config, "FEX_MAXINST" ) );
    assert( strstr( config, "dxvk.enableDescriptorBuffer = False" ) );
    assert( strstr( config, "dxvk.maxFrameRate = 30" ) );
    static const char game[] = "dxvk.numCompilerThreads = 2";
    assert( launcher_dxvk_config_add( config, sizeof(config), game, sizeof(game) - 1 ) );
    assert( strstr( config, "dxvk.numCompilerThreads = 3" ) < strstr( config, game ) );
    strcpy( before, config );
    assert( !dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, strlen(config) + 2 ) );
    assert( !strcmp( before, config ) );
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "unknown", config, sizeof(config) ) );
    assert( !strcmp( before, config ) );
    assert( !dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, 0 ) );

    config[0] = 0;
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "1.5", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.enableStateCache = False" ) );
    assert( !strstr( config, "floatEmulation" ) && !strstr( config, "maxMemoryBudget" ) );
    config[0] = 0;
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_GPLASYNC, "2.6-1", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.gplAsyncCache = True" ) );
    assert( !strstr( config, "deviceLocalConstantBuffers" ) );
    config[0] = 0;
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_SAREK, "1.13.0", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.shaderCompilationMethod = \"async\"" ) );
    assert( strstr( config, "d3d9.deviceLocalConstantBuffers = Auto" ) );
    assert( !strstr( config, "gplAsyncCache" ) );

    option = find( "dxvk.numCompilerThreads", DXVK_SOURCE_OFFICIAL, "3.1.1" );
    assert( option && dxvk_option_value( &kv, option ) > 0 );
    assert( launcher_kv_set( &kv, option->name, NULL ) );
    assert( !dxvk_option_value( &kv, option ) );
    assert( launcher_kv_set( &kv, option->name, "99999" ) );
    assert( dxvk_option_value( &kv, option ) == -1 );
    config[0] = 0;
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( !strstr( config, "numCompilerThreads" ) );
    option = find( "d3d9.maxAvailableMemory", DXVK_SOURCE_OFFICIAL, "3.1.1" );
    assert( option && launcher_kv_set( &kv, option->name, "0" ) );
    assert( dxvk_option_value( &kv, option ) == -1 );
}

static void defaults(void)
{
    struct launcher_kv kv = {0};
    struct launcher_settings settings;
    const struct dxvk_option *option;
    char config[LAUNCHER_KV_MAX];

    launcher_settings_read( &kv, &settings );
    option = find( "dxvk.enableDescriptorBuffer", DXVK_SOURCE_OFFICIAL, "3.1.1" );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_OFFICIAL, 30101, 0 ), "Auto" ) );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_OFFICIAL, 30101, 1 ), "False / HUD" ) );
    settings.dxvk_hud = 0;
    assert( launcher_dxvk_config( &settings, config, sizeof(config) ) );
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( !strstr( config, "enableDescriptorBuffer" ) );
    settings.dxvk_hud = 1;
    assert( launcher_dxvk_config( &settings, config, sizeof(config) ) );
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.enableDescriptorBuffer = False" ) );
    assert( launcher_kv_set( &kv, option->name, "True" ) );
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( strstr( config, "dxvk.enableDescriptorBuffer = True" ) > strstr( config, "dxvk.enableDescriptorBuffer = False" ) );
    assert( launcher_kv_set( &kv, option->name, NULL ) );
    config[0] = 0;
    assert( dxvk_options_apply( &kv, DXVK_SOURCE_OFFICIAL, "3.1.1", config, sizeof(config) ) );
    assert( !config[0] );
    option = find( "d3d9.floatEmulation", DXVK_SOURCE_OFFICIAL, "1.9.3" );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_OFFICIAL, 10903, 0 ), "True" ) );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_OFFICIAL, 10904, 0 ), "Auto" ) );
    option = find( "d3d9.useFP16", DXVK_SOURCE_OFFICIAL, "3.1.1" );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_OFFICIAL, 30101, 0 ), "False" ) );
    option = find( "dxvk.gplAsyncCache", DXVK_SOURCE_GPLASYNC, "2.6-1" );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_GPLASYNC, 20503, 0 ), "False" ) );
    assert( !strcmp( dxvk_option_default( option, DXVK_SOURCE_GPLASYNC, 20600, 0 ), "True" ) );
}

static void catalog(void)
{
    struct launcher_kv kv;
    char config[LAUNCHER_KV_MAX];
    static const char *const versions[] = { "1.0", "1.5", "1.9.2", "1.9.3", "1.10.3", "1.12.0", "1.13.0",
        "2.0", "2.1", "2.4.1", "2.6.2", "2.7", "2.7.1", "3.0", "3.1.1" };

    for (int source = 0; source < DXVK_SOURCE_COUNT; source++)
        for (unsigned int v = 0; v < sizeof(versions) / sizeof(versions[0]); v++)
        {
            memset( &kv, 0, sizeof(kv) );
            for (unsigned int i = 0; i < dxvk_option_count; i++)
            {
                const struct dxvk_option *option = dxvk_options + i;
                unsigned int release = dxvk_options_version( source, versions[v] );
                assert( option->choice_count >= 3 && !option->choices[0].value );
                assert( option->default_value && *option->default_value );
                if (!dxvk_option_supported( option, source, release )) continue;
                assert( find( option->name, source, versions[v] ) == option );
                for (unsigned int j = 1; j < option->choice_count; j++)
                {
                    assert( !strpbrk( option->choices[j].value, "\r\n" ) );
                    assert( launcher_kv_set( &kv, option->name, option->choices[j].value ) );
                    assert( dxvk_option_value( &kv, option ) == (int)j );
                }
            }
            config[0] = 0;
            assert( dxvk_options_apply( &kv, source, versions[v], config, sizeof(config) ) );
            assert( strlen(config) < 4096 );
        }
}

int main(void)
{
    versions();
    configuration();
    defaults();
    catalog();
    puts("DXVK options: version filtering, defaults, values and configuration precedence passed");
    return 0;
}
