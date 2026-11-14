/* The DLL repository's files onto a card and kept up to date (source/horizon_dlls.c),
 * with a folder standing in for GitHub.
 *
 *     horizon_dlls PHASE REPO SERVED FEATURES CARD
 *
 * PHASE is install (onto an empty card), update (SERVED changed one file and
 * dropped two, one of which the card changed since) or damaged (SERVED names
 * a hash its file does not have).
 * REPO is the DLL repository's checkout; SERVED is a folder laid out the same
 * that serves what the test changes (the manifest, a changed file) over it;
 * FEATURES lists what the runtime reports, one to a line; CARD is an empty
 * folder that stands for switch/wine on the card. */
#include "horizon_dlls.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *repo, *served, *card;
static unsigned long long fetched_bytes;
static const char *features[64];
static size_t feature_count;
static int cancel_after = -1, fetches;
/* The release's parts drop on their first attempt, after their first chunk. */
static int flaky;
static pthread_mutex_t flaky_lock = PTHREAD_MUTEX_INITIALIZER;
static char dropped[8][256];

#define CHECK( cond ) do { if (!(cond)) { fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond ); exit( 1 ); } } while (0)

/* A URL of the repository's main branch, from SERVED if it has the file and
 * from REPO otherwise. */
static enum horizon_dlls_result fetch( void *opaque, const char *url, horizon_dlls_sink sink, void *context,
                                       horizon_dlls_progress progress, void *progress_opaque )
{
    static const char prefix[] = HORIZON_DLLS_RAW "main/";
    static const char release[] = "https://github.com/autorunhq/autorun-horizon-dlls/";
    char path[1024], buffer[65536];
    unsigned long long done = 0;
    struct stat st;
    size_t got;
    FILE *file;

    (void)opaque;
    __atomic_add_fetch( &fetches, 1, __ATOMIC_RELAXED );
    if (!strncmp( url, release, sizeof(release) - 1 ))
        snprintf( path, sizeof(path), "%s/%s", served, url + sizeof(release) - 1 );
    else if (strncmp( url, prefix, sizeof(prefix) - 1 )) return HORIZON_DLLS_NOT_FOUND;
    else
    {
        snprintf( path, sizeof(path), "%s/%s", served, url + sizeof(prefix) - 1 );
        if (stat( path, &st )) snprintf( path, sizeof(path), "%s/%s", repo, url + sizeof(prefix) - 1 );
    }
    if (!(file = fopen( path, "rb" ))) return HORIZON_DLLS_NOT_FOUND;
    while ((got = fread( buffer, 1, sizeof(buffer), file )) > 0)
    {
        if (!sink( context, buffer, got )) { fclose( file ); return HORIZON_DLLS_IO; }
        done += got;
        __atomic_add_fetch( &fetched_bytes, got, __ATOMIC_RELAXED );
        if (progress && progress( progress_opaque, url, done, 0 )) { fclose( file ); return HORIZON_DLLS_CANCELLED; }
    }
    fclose( file );
    return HORIZON_DLLS_OK;
}

/* Four connections at once, as the launcher has six. */
#define CONNECTIONS 4
static const char *local_path( const char *url, char *path, size_t size )
{
    static const char release[] = "https://github.com/autorunhq/autorun-horizon-dlls/";

    if (strncmp( url, release, sizeof(release) - 1 )) return NULL;
    snprintf( path, size, "%s/%s", served, url + sizeof(release) - 1 );
    return path;
}

static enum horizon_dlls_result size_of( void *opaque, const char *url, unsigned long long *size )
{
    char path[1024];
    struct stat st;

    (void)opaque;
    if (!local_path( url, path, sizeof(path) ) || stat( path, &st )) return HORIZON_DLLS_NOT_FOUND;
    *size = st.st_size;
    return HORIZON_DLLS_OK;
}

/* A range, dropped once for each range when flaky, after its first chunk. */
static enum horizon_dlls_result fetch_range( void *opaque, const char *url, unsigned long long offset,
                                             unsigned long long length, horizon_dlls_sink sink, void *context,
                                             horizon_dlls_progress progress, void *progress_opaque )
{
    char path[1024], buffer[65536], key[300];
    unsigned long long done = 0;
    FILE *file;
    int drop = 0, i;

    (void)opaque;
    __atomic_add_fetch( &fetches, 1, __ATOMIC_RELAXED );
    if (!local_path( url, path, sizeof(path) ) || !(file = fopen( path, "rb" ))) return HORIZON_DLLS_NOT_FOUND;
    snprintf( key, sizeof(key), "%llu", offset );
    if (flaky)
    {
        pthread_mutex_lock( &flaky_lock );
        for (i = 0, drop = 1; i < 8 && dropped[i][0]; i++) if (!strcmp( dropped[i], key )) drop = 0;
        if (drop && i < 8) snprintf( dropped[i], sizeof(dropped[i]), "%s", key );
        pthread_mutex_unlock( &flaky_lock );
    }
    fseek( file, (long)offset, SEEK_SET );
    while (done < length)
    {
        size_t want = length - done < sizeof(buffer) ? (size_t)(length - done) : sizeof(buffer);
        size_t got = fread( buffer, 1, want, file );

        if (!got) break;
        if (!sink( context, buffer, got )) { fclose( file ); return HORIZON_DLLS_IO; }
        done += got;
        __atomic_add_fetch( &fetched_bytes, got, __ATOMIC_RELAXED );
        if (drop) { fclose( file ); return HORIZON_DLLS_NETWORK; }
        if (progress && progress( progress_opaque, url, done, length )) { fclose( file ); return HORIZON_DLLS_CANCELLED; }
    }
    fclose( file );
    return HORIZON_DLLS_OK;
}

static const struct horizon_dlls_transport transport = { fetch, size_of, fetch_range, NULL };
static const struct horizon_dlls_transport transports[CONNECTIONS] =
    { { fetch, size_of, fetch_range, NULL }, { fetch, size_of, fetch_range, NULL },
      { fetch, size_of, fetch_range, NULL }, { fetch, size_of, fetch_range, NULL } };
static pthread_mutex_t counting = PTHREAD_MUTEX_INITIALIZER;

static int ready( size_t count )
{
    char why[160];

    return horizon_dlls_ready( card, features, count, why, sizeof(why) );
}

static int count_files( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    static char last[128];
    int stop;

    (void)opaque; (void)current; (void)total;
    /* Stop when the given number of files have been started, as the next starts. */
    if (!what || !strstr( what, " of " )) return 0;
    pthread_mutex_lock( &counting );
    stop = strcmp( what, last ) && cancel_after >= 0 && !cancel_after--;
    snprintf( last, sizeof(last), "%s", what );
    pthread_mutex_unlock( &counting );
    return stop;
}

static struct horizon_dll_manifest remote, local;
static struct horizon_dlls_plan plan;

static enum horizon_dlls_result check( int verify )
{
    enum horizon_dlls_result result;

    horizon_dlls_free( &remote );
    horizon_dlls_free( &local );
    result = horizon_dlls_fetch_manifest( &transport, HORIZON_DLLS_MANIFEST_URL, features, feature_count,
                                          &remote, NULL, NULL );
    if (result) return result;
    result = horizon_dlls_load( card, features, feature_count, &local );
    CHECK( result == HORIZON_DLLS_OK || result == HORIZON_DLLS_NOT_FOUND );
    return horizon_dlls_plan( card, &remote, result ? NULL : &local, verify, NULL, NULL, &plan );
}

static enum horizon_dlls_result update( void )
{
    return horizon_dlls_apply( card, &remote, local.count || local.schema ? &local : NULL, transports,
                               CONNECTIONS, count_files, NULL );
}

static const struct horizon_dll_file *named( const char *name )
{
    unsigned int i;

    for (i = 0; i < remote.count; i++) if (!strcmp( remote.files[i].name, name )) return &remote.files[i];
    return NULL;
}

static char *read_all( const char *path, size_t *size )
{
    FILE *file = fopen( path, "rb" );
    char *data;
    long length;

    if (!file) return NULL;
    fseek( file, 0, SEEK_END );
    length = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = malloc( length + 1 );
    CHECK( data && fread( data, 1, length, file ) == (size_t)length );
    fclose( file );
    data[length] = 0;
    if (size) *size = length;
    return data;
}

static void card_path( char *out, size_t size, const struct horizon_dll_file *f )
{
    snprintf( out, size, "%s/%s/%s", card, horizon_dlls_folders[f->folder], f->name );
}

static int same_as_repo( const struct horizon_dll_file *f )
{
    char a[1024], b[1024];
    size_t sa, sb;
    char *da, *db;
    int same;

    card_path( a, sizeof(a), f );
    snprintf( b, sizeof(b), "%s/switch/wine/%s/%s", repo, horizon_dlls_folders[f->folder], f->name );
    da = read_all( a, &sa );
    db = read_all( b, &sb );
    same = da && db && sa == sb && !memcmp( da, db, sa );
    free( da );
    free( db );
    return same;
}

static void rejects( const char *manifest )
{
    struct horizon_dll_manifest m;

    CHECK( horizon_dlls_parse( manifest, strlen( manifest ), features, feature_count, &m ) == HORIZON_DLLS_INVALID );
}

/* The repository changed d3d9.dll, and no longer has dsound.dll or
 * xinput1_3.dll, the last of which the player replaced with their own. */
static int phase_update( void )
{
    char path[1024];
    struct stat st;

    CHECK( check( 0 ) == HORIZON_DLLS_OK );
    CHECK( plan.pending == 1 && named( "d3d9.dll" )->state == HORIZON_DLL_CHANGED && plan.removed == 2 );
    CHECK( update() == HORIZON_DLLS_OK );
    card_path( path, sizeof(path), named( "d3d9.dll" ) );
    CHECK( !stat( path, &st ) && (unsigned long long)st.st_size == named( "d3d9.dll" )->size );
    snprintf( path, sizeof(path), "%s/drive_c/windows/syswow64/dsound.dll", card );
    CHECK( stat( path, &st ) && errno == ENOENT );
    snprintf( path, sizeof(path), "%s/drive_c/windows/syswow64/xinput1_3.dll", card );
    CHECK( !stat( path, &st ) );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 && plan.removed == 0 );
    printf( "update: the changed d3d9.dll replaced, dsound.dll removed, the player's own xinput1_3.dll kept\n" );
    return 0;
}

/* An empty card from the release's zip, in one download, and then the one file
 * the zip had another version of, on its own. */
static int phase_release( void )
{
    struct horizon_dlls_bundle_stats stats;
    unsigned char *bundle;
    unsigned long long size;
    unsigned int i;

    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == remote.count );
    /* All of it first, a range on each of four connections, each dropped once. */
    fetches = 0;
    flaky = 1;
    CHECK( horizon_dlls_download( transports, CONNECTIONS, HORIZON_DLLS_BUNDLE_URL, &bundle, &size, NULL, NULL,
                                  &stats ) == HORIZON_DLLS_OK );
    flaky = 0;
    CHECK( stats.connections == CONNECTIONS || size < (unsigned long long)CONNECTIONS << 20 );
    CHECK( fetches == 2 * (int)stats.connections );
    /* Then unpacked: all but the one it had another version of. */
    CHECK( horizon_dlls_apply_bundle( card, &remote, NULL, bundle, size, NULL, NULL, &stats ) == HORIZON_DLLS_OK );
    free( bundle );
    CHECK( stats.unpacked == remote.count - 1 );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 1 && named( "quartz.dll" )->state == HORIZON_DLL_NEW );
    CHECK( update() == HORIZON_DLLS_OK );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 && ready( feature_count ) );
    for (i = 0; i < remote.count; i++) CHECK( same_as_repo( &remote.files[i] ) );
    printf( "release: %llu KB over %u connections, each range dropped once and tried again; %u files unpacked, "
            "the one it had another version of on its own\n", size >> 10, stats.connections, stats.unpacked );
    return 0;
}

/* A download whose hash is not the manifest's is not put in place. */
static int phase_damaged( void )
{
    char path[1024], *before, *after;
    size_t size_before, size_after;

    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 2 );
    card_path( path, sizeof(path), named( "quartz.dll" ) );
    before = read_all( path, &size_before );
    CHECK( update() == HORIZON_DLLS_HASH );
    after = read_all( path, &size_after );
    CHECK( before && after && size_before == size_after && !memcmp( before, after, size_before ) );
    free( before );
    free( after );
    strcat( path, ".part" );
    CHECK( access( path, F_OK ) );
    /* The download stopped at the first file that failed; the other, whose
     * compressed copy is not the manifest's, fails the same way on its own. */
    for (unsigned int i = 0; i < remote.count; i++)
        if (strcmp( remote.files[i].name, "devenum.dll" )) remote.files[i].state = HORIZON_DLL_CURRENT;
    CHECK( update() == HORIZON_DLLS_HASH );
    printf( "damaged: a download that fails its SHA-256 leaves the card's file and no .part behind\n" );
    return 0;
}

int main( int argc, char **argv )
{
    char path[1024], line[256];
    const struct horizon_dll_file *f;
    unsigned int i, total;
    char *text;
    FILE *list;

    CHECK( argc == 6 );
    repo = argv[2]; served = argv[3]; card = argv[5];
    CHECK( (list = fopen( argv[4], "r" )) );
    while (fgets( line, sizeof(line), list ) && feature_count < 64)
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (line[0]) features[feature_count++] = strdup( line );
    }
    fclose( list );
    if (!strcmp( argv[1], "update" )) return phase_update();
    if (!strcmp( argv[1], "release" )) return phase_release();
    if (!strcmp( argv[1], "damaged" )) return phase_damaged();
    CHECK( !strcmp( argv[1], "install" ) );

    /* A card with nothing: every file is new, and a program cannot start yet. */
    CHECK( !ready( feature_count ) );
    CHECK( check( 0 ) == HORIZON_DLLS_OK );
    total = remote.count;
    CHECK( total > 20 && plan.pending == total && plan.current == 0 && !plan.unsupported );
    CHECK( !strcmp( remote.flavor, HORIZON_DLLS_FLAVOR ) && remote.category_count > 5 );
    CHECK( plan.download_bytes < plan.pending_bytes );
    printf( "fresh card: %u files, %.1f MB on the card, %.1f MB to download in %u categories\n", total,
            plan.pending_bytes / 1048576.0, plan.download_bytes / 1048576.0, remote.category_count );

    /* Stopped a few files in: what finished is kept and recorded, the rest,
     * those being downloaded when it stopped too, wait. Which finished first
     * depends on the four connections. */
    cancel_after = 6;
    CHECK( update() == HORIZON_DLLS_CANCELLED );
    cancel_after = -1;
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.current < total && plan.current + plan.pending == total );
    CHECK( local.count == plan.current );
    CHECK( !ready( feature_count ) );
    printf( "cancelled a few files in: %u installed and recorded, %u waiting\n", plan.current, plan.pending );

    /* The rest, and the card has everything, byte for byte, having fetched the
     * compressed copies rather than the files. */
    fetched_bytes = 0;
    CHECK( update() == HORIZON_DLLS_OK );
    CHECK( fetched_bytes < plan.pending_bytes );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 && plan.current == total );
    for (i = 0; i < remote.count; i++) CHECK( same_as_repo( &remote.files[i] ) );
    CHECK( ready( feature_count ) );
    /* Not for a runtime that reports none of what they need. */
    CHECK( !ready( 0 ) );
    snprintf( path, sizeof(path), "%s/%s", card, HORIZON_DLLS_CLASSES );
    CHECK( (text = read_all( path, NULL )) );
    CHECK( !strncmp( text, "WINE REGISTRY Version 2\n", 24 ) );
    CHECK( strstr( text, "[Software\\\\Classes\\\\CLSID\\\\{e436ebb3-524f-11ce-9f53-0020af0ba770}\\\\InprocServer32]\n"
                         "@=\"quartz.dll\"\n\"ThreadingModel\"=\"Both\"" ) );
    free( text );
    printf( "installed: all %u files match the repository; classes.reg has quartz's filter graph\n", total );

    /* Checking again downloads nothing and reads nothing it has a record of. */
    fetches = 0;
    CHECK( update() == HORIZON_DLLS_OK && fetches == 0 );

    /* A file changed on the card: the record is trusted until Verify reads it. */
    f = named( "quartz.dll" );
    card_path( path, sizeof(path), f );
    text = read_all( path, NULL );
    text[100] ^= 0xff;
    CHECK( (list = fopen( path, "wb" )) && fwrite( text, 1, f->size, list ) == f->size && !fclose( list ) );
    free( text );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 );
    CHECK( check( 1 ) == HORIZON_DLLS_OK && plan.pending == 1 && named( "quartz.dll" )->state == HORIZON_DLL_CHANGED );
    CHECK( update() == HORIZON_DLLS_OK && same_as_repo( named( "quartz.dll" ) ) );
    printf( "verify: a damaged quartz.dll is found only by reading it, and replaced\n" );

    /* A copy of the repository made by hand is an installation: its manifest
     * is the record, so nothing is read or fetched. */
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 );

    /* A file on the card with no record, as an earlier release left them: the
     * same bytes are kept, other bytes replaced. */
    snprintf( path, sizeof(path), "%s/%s", card, HORIZON_DLLS_MANIFEST );
    CHECK( !remove( path ) );
    /* Files with no record, as an earlier release left them: a program does
     * not start on them until they are known to be the repository's. */
    CHECK( !ready( feature_count ) );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 && plan.current == total );
    fetches = 0;
    CHECK( update() == HORIZON_DLLS_OK && fetches == 0 );
    CHECK( check( 0 ) == HORIZON_DLLS_OK && plan.pending == 0 && local.count == total );
    CHECK( ready( feature_count ) );
    printf( "no record: the files already there are read, kept and recorded; ready only once recorded\n" );

    /* What the runtime cannot run is left alone. */
    horizon_dlls_free( &remote );
    CHECK( horizon_dlls_fetch_manifest( &transport, HORIZON_DLLS_MANIFEST_URL, features, 0, &remote, NULL, NULL )
           == HORIZON_DLLS_OK );
    horizon_dlls_free( &local );
    CHECK( horizon_dlls_load( card, features, 0, &local ) == HORIZON_DLLS_OK );
    for (i = 0; i < remote.count; i++) if (!remote.files[i].satisfied) break;
    CHECK( i < remote.count && remote.files[i].missing[0] );
    printf( "unsupported: %s needs %s, which this runtime does not report\n", remote.files[i].name,
            remote.files[i].missing );

    /* Refused: files outside the known folders, names with paths, URLs from elsewhere. */
    rejects( "{\"schema\":2,\"flavor\":\"arm64x\",\"files\":[{\"name\":\"x.dll\",\"path\":\"drive_c/windows/../../..\","
             "\"size\":1,\"sha256\":\"" "0000000000000000000000000000000000000000000000000000000000000000" "\","
             "\"url\":\"" HORIZON_DLLS_RAW "main/switch/wine/drive_c/windows/../../../x.dll\"}]}" );
    rejects( "{\"schema\":2,\"flavor\":\"arm64x\",\"files\":[{\"name\":\"../x.dll\",\"path\":\"drive_c/windows/system32\","
             "\"size\":1,\"sha256\":\"" "0000000000000000000000000000000000000000000000000000000000000000" "\","
             "\"url\":\"" HORIZON_DLLS_RAW "main/switch/wine/drive_c/windows/system32/../x.dll\"}]}" );
    rejects( "{\"schema\":2,\"flavor\":\"arm64x\",\"files\":[{\"name\":\"x.dll\",\"path\":\"drive_c/windows/system32\","
             "\"size\":1,\"sha256\":\"" "0000000000000000000000000000000000000000000000000000000000000000" "\","
             "\"url\":\"https://example.com/switch/wine/drive_c/windows/system32/x.dll\"}]}" );
    printf( "refused: a path out of the known folders, a name with a path, a URL from elsewhere\n" );

    /* A folder a later Autorun knows: that file is left out, not the manifest. */
    {
        static const char later[] =
            "{\"schema\":2,\"flavor\":\"arm64x\",\"files\":["
            "{\"name\":\"later.dll\",\"path\":\"drive_c/later/runtime\",\"size\":1,\"sha256\":\""
            "0000000000000000000000000000000000000000000000000000000000000000" "\",\"url\":\"" HORIZON_DLLS_RAW
            "main/switch/wine/drive_c/later/runtime/later.dll\",\"classes\":[{\"clsid\":"
            "\"00000000-0000-0000-0000-000000000001\",\"name\":\"X\"}]},"
            "{\"name\":\"x.dll\",\"path\":\"drive_c/windows/system32\",\"size\":1,\"sha256\":\""
            "0000000000000000000000000000000000000000000000000000000000000000" "\",\"url\":\"" HORIZON_DLLS_RAW
            "main/switch/wine/drive_c/windows/system32/x.dll\"}]}";
        struct horizon_dll_manifest m;

        CHECK( horizon_dlls_parse( later, strlen( later ), features, feature_count, &m ) == HORIZON_DLLS_OK );
        CHECK( m.count == 1 && m.skipped == 1 && !strcmp( m.files[0].name, "x.dll" ) && m.class_count == 0 );
        horizon_dlls_free( &m );
    }
    printf( "later folders: a file in a folder this build does not know is left out, the rest read\n" );
    return 0;
}
