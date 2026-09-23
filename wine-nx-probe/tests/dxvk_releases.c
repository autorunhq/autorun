#include <assert.h>
#include <unistd.h>

#include "../source/dxvk_releases.c"

void wine_nx_runtime_trace( const char *message ) { (void)message; }
void sha256CalculateHash( void *dst, const void *src, size_t size )
{
    (void)dst; (void)src; (void)size;
    abort();
}

static void dll( const char *root, const char *directory, const char *name, unsigned short machine )
{
    char path[1024];
    unsigned char pe[128] = {0};
    FILE *file;

    snprintf( path, sizeof(path), "%s/drive_c/%s", root, directory );
    assert( make_directories( path ) );
    snprintf( path, sizeof(path), "%s/drive_c/%s/%s", root, directory, name );
    pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 64;
    pe[64] = 'P'; pe[65] = 'E'; pe[68] = machine; pe[69] = machine >> 8;
    pe[87] = 0x20; pe[88] = 0x0b; pe[89] = machine == 0x8664 ? 2 : 1;
    assert( (file = fopen( path, "wb" )) );
    assert( fwrite( pe, 1, sizeof(pe), file ) == sizeof(pe) );
    assert( !fclose( file ) );
}

static void check( const char *root, int vkd3d, unsigned short machine, const char *requested,
                    const char *expected, int installed, int bundled )
{
    struct dxvk_version selected;
    (vkd3d ? vkd3d_resolve_version : dxvk_resolve_version)( root, machine, requested, &selected );
    assert( !strcmp( selected.version, expected ) );
    assert( selected.installed == installed && selected.bundled == bundled );
}

int main(void)
{
    char root[] = "graphics-releases-XXXXXX", path[1024];
    struct dxvk_release releases[4] = {0}, readback[4];
    int count;

    assert( mkdtemp( root ) );
    check( root, 0, 0x8664, "", "", 0, 0 );
    assert( compare_versions( "1.10", "1.9" ) > 0 );
    assert( compare_versions( "3.0b", "3.0" ) > 0 );
    assert( compare_versions( "3.0.1", "3.0b" ) > 0 );
    assert( compare_versions( "3.0", "3.0.0" ) == 0 );
    assert( !stable_version( "3.0-rc1" ) && !stable_version( "3.0.new" ) );
    dll( root, "dxvk64", "d3d9.dll", 0x8664 );
    snprintf( path, sizeof(path), "%s/drive_c/dxvk64/dxvk-manifest.json", root );
    assert( write_atomic( path, "{\"version\":\"3.0b\"}", 18 ) );
    check( root, 0, 0x8664, "", "3.0b", 1, 1 );
    dll( root, "dxvk64/versions/2.7.1", "d3d9.dll", 0x8664 );
    dll( root, "dxvk64/versions/3.1.1", "d3d9.dll", 0x8664 );
    dll( root, "dxvk64/versions/4.0", "d3d9.dll", 0x014c );
    dll( root, "dxvk64/versions/99.0-rc1", "d3d9.dll", 0x8664 );
    dll( root, "dxvk64/versions/99.0.new", "d3d9.dll", 0x8664 );
    check( root, 0, 0x8664, "", "3.1.1", 1, 0 );
    check( root, 0, 0x8664, "3.0b", "3.0b", 1, 1 );
    check( root, 0, 0x8664, "2.7.1", "2.7.1", 1, 0 );
    check( root, 0, 0x8664, "3.0", "3.0", 0, 0 );
    check( root, 0, 0x8664, "99.0-rc1", "99.0-rc1", 1, 0 );
    check( root, 0, 0x014c, "", "", 0, 0 );
    dll( root, "dxvk/versions/0.9", "d3d9.dll", 0x014c );
    check( root, 0, 0x014c, "", "", 0, 0 );
    dll( root, "dxvk/versions/1.9", "d3d9.dll", 0x014c );
    dll( root, "dxvk/versions/1.10", "d3d9.dll", 0x014c );
    check( root, 0, 0x014c, "", "1.10", 1, 0 );
    dll( root, "vkd3d64/versions/3.0b", "d3d12.dll", 0x8664 );
    check( root, 1, 0x8664, "", "", 0, 0 );
    dll( root, "vkd3d64/versions/3.0b", "d3d12core.dll", 0x8664 );
    check( root, 1, 0x8664, "", "3.0b", 1, 0 );
    check( root, 1, 0x014c, "", "", 0, 0 );

    assert( dxvk_release_catalog( root, readback, 4, &count, 1, NULL, NULL ) == DXVK_NOT_FOUND );
    assert( !count );
    strcpy( releases[0].version, "3.1.1" );
    strcpy( releases[0].url, "https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz" );
    strcpy( releases[1].version, "2.7.1" );
    strcpy( releases[1].url, "https://github.com/doitsujin/dxvk/releases/download/v2.7.1/dxvk-2.7.1.tar.gz" );
    assert( save_catalog( &dxvk_backend, root, releases, 2 ) );
    assert( dxvk_release_catalog( root, readback, 1, &count, 1, NULL, NULL ) == DXVK_OK );
    assert( count == 1 && !strcmp( readback[0].version, "3.1.1" ) );
    assert( dxvk_release_catalog( root, readback, 4, &count, 1, NULL, NULL ) == DXVK_OK );
    assert( count == 2 && !strcmp( readback[1].version, "2.7.1" ) );
    assert( vkd3d_release_catalog( root, readback, 4, &count, 1, NULL, NULL ) == DXVK_NOT_FOUND );
    assert( remove_tree( root ) );
    puts( "graphics version resolution and cached catalogs: OK" );
    return 0;
}
