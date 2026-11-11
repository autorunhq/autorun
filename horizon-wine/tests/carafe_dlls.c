/*
 * The Carafe builds of DXVK and FEX, as the launcher finds, downloads and
 * installs them, and as the runtime puts the FEX one in system32.
 *
 * Without arguments everything is made here: the release list is the shape
 * GitHub gives for this repository, the archive is packed the way
 * carafe/dlls/package.py packs one, and the DLLs are PE headers.
 *
 * With "live" the release list and the archives are the real ones: what a card
 * would download today, taken with the launcher's own code. A network that
 * cannot be reached ends it with status 77 and nothing said about the code.
 *
 * cc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined \
 *    -I horizon-wine/tests/graphics-shims horizon-wine/tests/carafe_dlls.c \
 *    -lcurl -lzstd -lz -o carafe_dlls
 */
#include <assert.h>
#include <unistd.h>

#include "../source/dxvk_releases.c"
#include "../source/fex_build.c"

static int traces;
void wine_nx_runtime_trace( const char *message )
{
    traces++;
    fprintf( stderr, "%s\n", message );
}

/* FIPS 180-4, for the digest GitHub gives with a release's file. */
static uint32_t rotr( uint32_t x, int n ) { return x >> n | x << (32 - n); }

static void sha256_block( uint32_t state[8], const unsigned char *p )
{
    static const uint32_t k[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64], v[8], t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 64; i++)
        w[i] = w[i - 16] + (rotr( w[i - 15], 7 ) ^ rotr( w[i - 15], 18 ) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (rotr( w[i - 2], 17 ) ^ rotr( w[i - 2], 19 ) ^ (w[i - 2] >> 10));
    memcpy( v, state, sizeof(v) );
    for (i = 0; i < 64; i++)
    {
        t1 = v[7] + (rotr( v[4], 6 ) ^ rotr( v[4], 11 ) ^ rotr( v[4], 25 )) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
        t2 = (rotr( v[0], 2 ) ^ rotr( v[0], 13 ) ^ rotr( v[0], 22 )) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove( v + 1, v, 7 * sizeof(*v) );
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) state[i] += v[i];
}

void sha256CalculateHash( void *dst, const void *src, size_t size )
{
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const unsigned char *p = src;
    unsigned char tail[128] = {0}, *out = dst;
    size_t left = size, used;
    int i;

    for (; left >= 64; left -= 64, p += 64) sha256_block( state, p );
    memcpy( tail, p, left );
    tail[left] = 0x80;
    used = left < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[used - 1 - i] = (unsigned char)((unsigned long long)size * 8 >> (8 * i));
    sha256_block( state, tail );
    if (used == 128) sha256_block( state, tail + 64 );
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = state[i] >> 24; out[i * 4 + 1] = state[i] >> 16;
        out[i * 4 + 2] = state[i] >> 8; out[i * 4 + 3] = state[i];
    }
}

/* As much of a PE file as the launcher reads: a DLL for the machine. */
static size_t pe_image( unsigned char *pe, size_t size, unsigned short machine, unsigned char fill )
{
    assert( size >= 128 );
    memset( pe, fill, size );
    memset( pe, 0, 128 );
    pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 64;
    pe[64] = 'P'; pe[65] = 'E'; pe[68] = machine; pe[69] = machine >> 8;
    pe[87] = 0x20; pe[88] = 0x0b; pe[89] = machine == 0x014c ? 1 : 2;
    return size;
}

static void put( const char *root, const char *name, const unsigned char *data, size_t size )
{
    char path[1024], *slash;
    FILE *file;

    snprintf( path, sizeof(path), "%s/%s", root, name );
    slash = strrchr( path, '/' );
    *slash = 0;
    assert( make_directories( path ) );
    *slash = '/';
    assert( (file = fopen( path, "wb" )) );
    assert( fwrite( data, 1, size, file ) == size );
    assert( !fclose( file ) );
}

static void dll( const char *root, const char *name, unsigned short machine, size_t size, unsigned char fill )
{
    static unsigned char pe[4096];

    put( root, name, pe, pe_image( pe, size, machine, fill ) );
}

/* The first bytes after the header and the size say which DLL a file is. */
static int is_dll( const char *root, const char *name, size_t size, unsigned char fill )
{
    unsigned char pe[4096];
    char path[1024];
    FILE *file;
    size_t got;

    snprintf( path, sizeof(path), "%s/%s", root, name );
    if (!(file = fopen( path, "rb" ))) return 0;
    got = fread( pe, 1, sizeof(pe), file );
    fclose( file );
    return got == size && pe[200] == fill;
}

static int exists( const char *root, const char *name )
{
    char path[1024];

    snprintf( path, sizeof(path), "%s/%s", root, name );
    return !access( path, F_OK );
}

/* One file of a tar archive, as carafe/dlls/package.py writes it. */
static void tar_file( gzFile out, const char *name, const unsigned char *data, size_t size )
{
    unsigned char header[512] = {0}, zero[512] = {0};
    unsigned int sum = 0, i;

    snprintf( (char *)header, 100, "%s", name );
    snprintf( (char *)header + 100, 8, "%07o", 0644 );
    snprintf( (char *)header + 108, 8, "%07o", 0 );
    snprintf( (char *)header + 116, 8, "%07o", 0 );
    snprintf( (char *)header + 124, 12, "%011lo", (unsigned long)size );
    snprintf( (char *)header + 136, 12, "%011o", 0 );
    memset( header + 148, ' ', 8 );
    header[156] = '0';
    memcpy( header + 257, "ustar", 6 );
    memcpy( header + 263, "00", 2 );
    for (i = 0; i < 512; i++) sum += header[i];
    snprintf( (char *)header + 148, 8, "%06o", sum );
    header[155] = ' ';
    assert( gzwrite( out, header, 512 ) == 512 );
    assert( gzwrite( out, data, (unsigned int)size ) == (int)size );
    if (size % 512) assert( gzwrite( out, zero, (unsigned int)(512 - size % 512) ) == (int)(512 - size % 512) );
}

static void test_catalog(void)
{
    /* Both kinds of release are in one list, with a release of something else
     * and one whose file is elsewhere. */
    static const char page[] = "["
        "{\"tag_name\":\"dxvk-carafe-3.1.1-2\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"dxvk-carafe-3.1.1-2.tar.gz\",\"size\":16722303,"
        "\"digest\":\"sha256:4e2ecf1bd129bac0851d63168dd51f8dfd61a69c533c2e2700605a4e86df9d8d\","
        "\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/dxvk-carafe-3.1.1-2/dxvk-carafe-3.1.1-2.tar.gz\"}]},"
        "{\"tag_name\":\"fex-carafe-2609-1\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"fex-carafe-2609-1.tar.gz\",\"size\":1041730,"
        "\"digest\":\"sha256:0a075839a27f1853e14b302222f6ebb01024b46c07fe0f908e594fdd28943297\","
        "\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/fex-carafe-2609-1/fex-carafe-2609-1.tar.gz\"}]},"
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"autorun-123.zip\",\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/v1.2.3/autorun-123.zip\"}]},"
        "{\"tag_name\":\"dxvk-carafe-3.1.1-1\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"dxvk-carafe-3.1.1-1.tar.gz\",\"size\":16722303,"
        "\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/dxvk-carafe-3.1.1-1/dxvk-carafe-3.1.1-1.tar.gz\"}]},"
        "{\"tag_name\":\"fex-carafe-2609-9\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"fex-carafe-2609-9.tar.gz\","
        "\"browser_download_url\":\"https://github.com/someone-else/autorun/releases/download/fex-carafe-2609-9/"
        "fex-carafe-2609-9.tar.gz\"}]},"
        /* A tag that is not Carafe's, as long as what Carafe's begin with,
         * over a file named like one of Carafe's. */
        "{\"tag_name\":\"weekly-2026-3.1.1-7\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"dxvk-carafe-3.1.1-7.tar.gz\","
        "\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/weekly-2026-3.1.1-7/dxvk-carafe-3.1.1-7.tar.gz\"}]},"
        "{\"tag_name\":\"fex-carafe-2609-3\",\"draft\":true,\"prerelease\":false,\"assets\":[{"
        "\"name\":\"fex-carafe-2609-3.tar.gz\","
        "\"browser_download_url\":\"https://github.com/" WINE_NX_CARAFE_DLLS_REPO
        "/releases/download/fex-carafe-2609-3/fex-carafe-2609-3.tar.gz\"}]}"
        "]";
    struct dxvk_release found[8];
    int seen = 0;

    assert( strlen( "weekly-2026-" ) == strlen( carafe_backend.tag ) );

    assert( parse_release_page( &carafe_backend, (const unsigned char *)page, strlen( page ), found, 8, &seen ) == 2 );
    assert( seen == 7 );
    assert( !strcmp( found[0].version, "3.1.1-2" ) && !strcmp( found[1].version, "3.1.1-1" ) );
    assert( !strcmp( found[0].url, "https://github.com/" WINE_NX_CARAFE_DLLS_REPO
                     "/releases/download/dxvk-carafe-3.1.1-2/dxvk-carafe-3.1.1-2.tar.gz" ) );
    assert( !strcmp( found[0].digest, "4e2ecf1bd129bac0851d63168dd51f8dfd61a69c533c2e2700605a4e86df9d8d" ) );
    assert( found[0].size == 16722303 && !found[0].prerelease && !found[1].digest[0] );
    seen = 0;
    assert( parse_release_page( &fex_carafe_backend, (const unsigned char *)page, strlen( page ), found, 8, &seen ) == 1 );
    assert( !strcmp( found[0].version, "2609-1" ) && found[0].size == 1041730 );
    /* The other sources do not take them for theirs. */
    seen = 0;
    assert( parse_release_page( &dxvk_backend, (const unsigned char *)page, strlen( page ), found, 8, &seen ) == 0 );

    assert( backend_stable_version( &carafe_backend, "3.1.1-1" ) && !backend_stable_version( &carafe_backend, "3.1.1" ) );
    assert( backend_stable_version( &fex_carafe_backend, "2609-1" ) );
    assert( !backend_stable_version( &fex_carafe_backend, "2609-rc1" ) );
    assert( compare_versions( "2609-2", "2609-1" ) > 0 && compare_versions( "2610-1", "2609-9" ) > 0 );
    assert( compare_versions( "3.1.1-10", "3.1.1-9" ) > 0 );
}

static void test_installed( const char *root )
{
    struct dxvk_version v;
    char path[1024];

    /* Nothing yet, and the folder the stock FEX is kept in is not a release. */
    dll( root, "drive_c/fex-carafe/stock-libwow64fex.dll", 0xaa64, 600, 's' );
    dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "", &v );
    assert( !v.installed && !v.version[0] );
    assert( !dxvk_release_installed( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "" ) );
    snprintf( path, sizeof(path), "%s/drive_c/fex-carafe", root );
    assert( remove_tree( path ) );

    /* FEX for 32-bit programs is an ARM64 module; an x86 file of its name is not it. */
    dll( root, "drive_c/fex-carafe/versions/2609-1/libwow64fex.dll", 0xaa64, 700, 'a' );
    dll( root, "drive_c/fex-carafe/versions/2609-2/libwow64fex.dll", 0xaa64, 800, 'b' );
    dll( root, "drive_c/fex-carafe/versions/2609-3/libwow64fex.dll", 0x014c, 800, 'x' );
    dll( root, "drive_c/fex-carafe/versions/2609-4.new/libwow64fex.dll", 0xaa64, 800, 'n' );
    dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "", &v );
    assert( v.installed && !v.bundled && !strcmp( v.version, "2609-2" ) );
    dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "2609-1", &v );
    assert( v.installed && !strcmp( v.version, "2609-1" ) );
    dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "2609-3", &v );
    assert( !v.installed );
    assert( !dxvk_release_installed( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "2609-3" ) );
    /* There is none for 64-bit programs. */
    dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x8664, "", &v );
    assert( !v.installed );
    assert( dxvk_release_path( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "2609-1", path, sizeof(path) ) );
    assert( !strncmp( path, root, strlen( root ) ) &&
            !strcmp( path + strlen( root ), "/drive_c/fex-carafe/versions/2609-1" ) );
    assert( !dxvk_release_path( DXVK_BACKEND_FEX_CARAFE, root, 0x8664, "2609-1", path, sizeof(path) ) );
    assert( !dxvk_release_path( DXVK_BACKEND_COUNT, root, 0x014c, "2609-1", path, sizeof(path) ) );

    /* Carafe's DXVK is a DXVK for both machines, in folders of its own. */
    dll( root, "drive_c/dxvk-carafe/versions/3.1.1-1/d3d9.dll", 0x014c, 300, 'd' );
    dll( root, "drive_c/dxvk-carafe64/versions/3.1.1-1/d3d11.dll", 0x8664, 300, 'd' );
    dxvk_resolve_version( DXVK_SOURCE_CARAFE, root, 0x014c, "", &v );
    assert( v.installed && !strcmp( v.version, "3.1.1-1" ) );
    dxvk_resolve_version( DXVK_SOURCE_CARAFE, root, 0x8664, "", &v );
    assert( v.installed && !strcmp( v.version, "3.1.1-1" ) );
    dxvk_resolve_version( DXVK_SOURCE_OFFICIAL, root, 0x8664, "", &v );
    assert( !v.installed );
    assert( !strcmp( launcher_dxvk_directory( 0x014c, DXVK_SOURCE_CARAFE ), "dxvk-carafe" ) );
    assert( !strcmp( launcher_dxvk_directory( 0x8664, DXVK_SOURCE_CARAFE ), "dxvk-carafe64" ) );
    assert( !launcher_dxvk_directory( 0x014c, DXVK_BACKEND_FEX_CARAFE ) );
    assert( launcher_dxvk_vkd3d_compatible( DXVK_SOURCE_CARAFE, "3.1.1-1" ) );
}

static void test_archive( const char *root )
{
    static unsigned char pe[4096];
    char archive[1024], x32[1024], x64[1024];
    int x32_files = 0, x64_files = 0;
    gzFile out;

    snprintf( archive, sizeof(archive), "%s/fex-carafe-9999-1.tar.gz", root );
    assert( (out = gzopen( archive, "wb" )) );
    tar_file( out, "fex-carafe-9999-1/fex-carafe-manifest.json", (const unsigned char *)"{}", 2 );
    tar_file( out, "fex-carafe-9999-1/licenses/FEX-MIT.txt", (const unsigned char *)"MIT", 3 );
    tar_file( out, "fex-carafe-9999-1/x32/libwow64fex.dll", pe, pe_image( pe, 3000, 0xaa64, 'f' ) );
    /* Not what this is a release of: left in the archive. */
    tar_file( out, "fex-carafe-9999-1/x32/d3d9.dll", pe, pe_image( pe, 300, 0x014c, 'd' ) );
    tar_file( out, "fex-carafe-9999-1/x64/libwow64fex.dll", pe, pe_image( pe, 300, 0x8664, 'w' ) );
    assert( gzwrite( out, (memset( pe, 0, 1024 ), pe), 1024 ) == 1024 );
    assert( gzclose( out ) == Z_OK );

    snprintf( x32, sizeof(x32), "%s/fex-x32", root );
    assert( make_directory( x32 ) );
    assert( extract_archive( &fex_carafe_backend, archive, x32, NULL, &x32_files, &x64_files ) );
    assert( x32_files == 1 && !x64_files );
    assert( is_dll( x32, "libwow64fex.dll", 3000, 'f' ) && !exists( x32, "d3d9.dll" ) );
    assert( validate_payload( &fex_carafe_backend, x32, 0x014c, "9999-1" ) );

    snprintf( archive, sizeof(archive), "%s/dxvk-carafe-9.9.9-1.tar.gz", root );
    assert( (out = gzopen( archive, "wb" )) );
    tar_file( out, "dxvk-carafe-9.9.9-1/x32/d3d9.dll", pe, pe_image( pe, 300, 0x014c, 'd' ) );
    tar_file( out, "dxvk-carafe-9.9.9-1/x32/dxgi.dll", pe, pe_image( pe, 300, 0x014c, 'g' ) );
    tar_file( out, "dxvk-carafe-9.9.9-1/x64/d3d11.dll", pe, pe_image( pe, 300, 0x8664, 'e' ) );
    tar_file( out, "dxvk-carafe-9.9.9-1/dxvk-carafe-manifest.json", (const unsigned char *)"{}", 2 );
    assert( gzwrite( out, (memset( pe, 0, 1024 ), pe), 1024 ) == 1024 );
    assert( gzclose( out ) == Z_OK );
    snprintf( x32, sizeof(x32), "%s/dxvk-x32", root );
    snprintf( x64, sizeof(x64), "%s/dxvk-x64", root );
    assert( make_directory( x32 ) && make_directory( x64 ) );
    x32_files = x64_files = 0;
    assert( extract_archive( &carafe_backend, archive, x32, x64, &x32_files, &x64_files ) );
    assert( x32_files == 2 && x64_files == 1 );
    assert( validate_payload( &carafe_backend, x32, 0x014c, "9.9.9-1" ) );
    assert( validate_payload( &carafe_backend, x64, 0x8664, "9.9.9-1" ) );
    assert( !validate_payload( &carafe_backend, x64, 0x014c, "9.9.9-1" ) );
}

#define SYSTEM "drive_c/windows/system32/libwow64fex.dll"
#define STOCK  "drive_c/fex-carafe/stock-libwow64fex.dll"
#define MARKED "drive_c/fex-carafe/in-system32.txt"

/* What the runtime does when a 32-bit program starts on FEX, and the launcher
 * when it opens. Sizes and fill bytes stand for the builds. */
static void test_system32( const char *root )
{
    char one[1024], two[1024], none[1024];
    int changed;

    snprintf( one, sizeof(one), "%s/drive_c/fex-carafe/versions/2609-1/libwow64fex.dll", root );
    snprintf( two, sizeof(two), "%s/drive_c/fex-carafe/versions/2609-2/libwow64fex.dll", root );
    snprintf( none, sizeof(none), "%s/drive_c/fex-carafe/versions/2609-7/libwow64fex.dll", root );

    /* A card with no FEX at all, and a program on the stock one: nothing to do. */
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && !changed );
    assert( !exists( root, SYSTEM ) && !exists( root, MARKED ) );

    dll( root, SYSTEM, 0xaa64, 600, 's' );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && !changed );
    assert( is_dll( root, SYSTEM, 600, 's' ) && !exists( root, STOCK ) );

    /* A Carafe build that is not on the card leaves system32 alone. */
    assert( wine_nx_fex_build_select( root, none, &changed ) == FEX_BUILD_NO_CARAFE && !changed );
    assert( is_dll( root, SYSTEM, 600, 's' ) && !exists( root, MARKED ) );

    /* The first program on Carafe's: the stock DLL is kept, Carafe's stands in. */
    assert( wine_nx_fex_build_select( root, one, &changed ) == FEX_BUILD_CARAFE && changed );
    assert( is_dll( root, SYSTEM, 700, 'a' ) && is_dll( root, STOCK, 600, 's' ) && exists( root, MARKED ) );
    assert( !exists( root, SYSTEM ".new" ) && !exists( root, STOCK ".new" ) );
    /* The next one on the same build copies nothing. */
    assert( wine_nx_fex_build_select( root, one, &changed ) == FEX_BUILD_CARAFE && !changed );
    /* Another Carafe build: the kept stock DLL is not overwritten with a Carafe one. */
    assert( wine_nx_fex_build_select( root, two, &changed ) == FEX_BUILD_CARAFE && changed );
    assert( is_dll( root, SYSTEM, 800, 'b' ) && is_dll( root, STOCK, 600, 's' ) );
    /* The installed release still counts as installed: the stock copy beside
     * the versions is not taken for a release. */
    assert( dxvk_release_installed( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "2609-2" ) );
    assert( !dxvk_release_installed( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "" ) );

    /* Back to the stock one, as the launcher asks for when it opens. */
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && changed );
    assert( is_dll( root, SYSTEM, 600, 's' ) && !exists( root, MARKED ) );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && !changed );

    /* A Windows DLLs update replaced what was in system32 while a Carafe build
     * stood there: what is there now is the stock DLL, a newer one. */
    assert( wine_nx_fex_build_select( root, one, &changed ) == FEX_BUILD_CARAFE && changed );
    dll( root, SYSTEM, 0xaa64, 640, 'u' );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && !changed );
    assert( is_dll( root, SYSTEM, 640, 'u' ) && !exists( root, MARKED ) );
    /* And the next Carafe program keeps the newer one, not the copy from before. */
    assert( wine_nx_fex_build_select( root, one, &changed ) == FEX_BUILD_CARAFE && changed );
    dll( root, SYSTEM, 0xaa64, 660, 'v' );
    assert( wine_nx_fex_build_select( root, two, &changed ) == FEX_BUILD_CARAFE && changed );
    assert( is_dll( root, SYSTEM, 800, 'b' ) && is_dll( root, STOCK, 660, 'v' ) );

    /* The release in system32 was removed from the card: its size still says
     * that system32 holds it, and the stock DLL comes back. */
    assert( !remove( two ) );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && changed );
    assert( is_dll( root, SYSTEM, 660, 'v' ) );
    dll( root, "drive_c/fex-carafe/versions/2609-2/libwow64fex.dll", 0xaa64, 800, 'b' );

    /* No copy of the stock DLL: a Carafe build stays, and it is said. */
    assert( wine_nx_fex_build_select( root, two, &changed ) == FEX_BUILD_CARAFE && changed );
    {
        char path[1024];
        snprintf( path, sizeof(path), "%s/" STOCK, root );
        assert( !remove( path ) );
    }
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_NO_STOCK && !changed );
    assert( is_dll( root, SYSTEM, 800, 'b' ) && exists( root, MARKED ) );
    /* Until the DLLs are downloaded again. */
    dll( root, SYSTEM, 0xaa64, 600, 's' );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && !changed );
    assert( !exists( root, MARKED ) );

    /* A card that never had the stock FEX can still run on Carafe's. */
    {
        char path[1024];
        snprintf( path, sizeof(path), "%s/" SYSTEM, root );
        assert( !remove( path ) );
    }
    assert( wine_nx_fex_build_select( root, one, &changed ) == FEX_BUILD_CARAFE && changed );
    assert( is_dll( root, SYSTEM, 700, 'a' ) && !exists( root, STOCK ) );
    assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_NO_STOCK );

    assert( wine_nx_fex_build_select( NULL, NULL, &changed ) == FEX_BUILD_IO );
    assert( wine_nx_fex_build_select( root, one, NULL ) == FEX_BUILD_CARAFE );
}

/* The newest release of a source, downloaded and installed as the launcher does. */
static int live( const char *root, enum dxvk_source source, const char *name, unsigned short machine,
                 const char *dll_name, unsigned short dll_machine )
{
    static struct dxvk_release releases[DXVK_MAX_RELEASES];
    struct dxvk_version v;
    char path[1024], file[1100];
    enum dxvk_result result;
    int count = 0;

    result = dxvk_release_catalog( source, root, releases, DXVK_MAX_RELEASES, &count, 0, NULL, NULL );
    if (result == DXVK_NETWORK_ERROR)
    {
        printf( "%s: the release list could not be fetched\n", name );
        return 77;
    }
    if (result != DXVK_OK || count < 1)
    {
        printf( "%s: no release the launcher accepts (%s)\n", name, dxvk_result_message( result ) );
        return 1;
    }
    printf( "%s: %d release%s, newest %s, %llu bytes, sha256 %s\n  %s\n", name, count, count == 1 ? "" : "s",
            releases[0].version, releases[0].size, releases[0].digest[0] ? releases[0].digest : "not given",
            releases[0].url );
    result = dxvk_install_release( source, root, releases, NULL, NULL );
    if (result == DXVK_NETWORK_ERROR)
    {
        printf( "%s: the archive could not be fetched\n", name );
        return 77;
    }
    if (result != DXVK_OK)
    {
        printf( "%s: %s did not install (%s)\n", name, releases[0].version, dxvk_result_message( result ) );
        return 1;
    }
    dxvk_resolve_version( source, root, machine, "", &v );
    assert( v.installed && !strcmp( v.version, releases[0].version ) );
    assert( dxvk_release_path( source, root, machine, v.version, path, sizeof(path) ) );
    snprintf( file, sizeof(file), "%s/%s", path, dll_name );
    assert( valid_pe( file, dll_machine ) );
    printf( "  installed: %s\n", file + strlen( root ) + 1 );
    return 0;
}

int main( int argc, char **argv )
{
    char root[] = "carafe-dlls-XXXXXX";
    int status = 0;

    assert( mkdtemp( root ) );
    if (argc > 1 && !strcmp( argv[1], "live" ))
    {
        int fex, dxvk32, dxvk64 = 0;

        fex = live( root, DXVK_BACKEND_FEX_CARAFE, "FEX Carafe", 0x014c, "libwow64fex.dll", 0xaa64 );
        dxvk32 = live( root, DXVK_SOURCE_CARAFE, "DXVK Carafe", 0x014c, "d3d9.dll", 0x014c );
        if (!dxvk32)
        {
            struct dxvk_version v;

            dxvk_resolve_version( DXVK_SOURCE_CARAFE, root, 0x8664, "", &v );
            assert( v.installed );
            dxvk64 = !dxvk_release_installed( DXVK_SOURCE_CARAFE, root, 0x8664, v.version );
        }
        if (!fex)
        {
            /* And the downloaded FEX goes to system32 and back. */
            struct dxvk_version v;
            char path[1024], file[1100];
            int changed;

            dll( root, SYSTEM, 0xaa64, 600, 's' );
            dxvk_resolve_version( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, "", &v );
            assert( dxvk_release_path( DXVK_BACKEND_FEX_CARAFE, root, 0x014c, v.version, path, sizeof(path) ) );
            snprintf( file, sizeof(file), "%s/libwow64fex.dll", path );
            assert( wine_nx_fex_build_select( root, file, &changed ) == FEX_BUILD_CARAFE && changed );
            snprintf( path, sizeof(path), "%s/" SYSTEM, root );
            assert( same_content( path, file ) == 1 && valid_pe( path, 0xaa64 ) );
            assert( wine_nx_fex_build_select( root, NULL, &changed ) == FEX_BUILD_STOCK && changed );
            assert( is_dll( root, SYSTEM, 600, 's' ) );
        }
        status = fex == 1 || dxvk32 == 1 || dxvk64 ? 1 : fex == 77 || dxvk32 == 77 ? 77 : 0;
        assert( remove_tree( root ) );
        puts( status == 77 ? "Carafe DLL releases: not reachable from here" :
              status ? "Carafe DLL releases: FAILED" : "Carafe DLL releases, downloaded and installed: OK" );
        return status;
    }
    test_catalog();
    test_installed( root );
    test_archive( root );
    {
        char path[1024];
        snprintf( path, sizeof(path), "%s/drive_c/windows", root );
        remove_tree( path );
    }
    test_system32( root );
    assert( remove_tree( root ) );
    assert( !traces );
    puts( "Carafe DLLs: catalog, archives, installed versions and system32: OK" );
    return 0;
}
