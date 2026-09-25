#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <switch.h>
#include <zlib.h>
#include <zstd.h>

#include "dxvk_releases.h"
#include "launcher_settings.h"

extern void wine_nx_runtime_trace( const char *message );

#define DXVK_API_BODY_MAX (24u * 1024u * 1024u)
#define DXVK_ARCHIVE_MAX  (128u * 1024u * 1024u)
struct release_backend
{
    const char *id, *repo, *prefix, *asset, *extension;
    const char *x32, *x64;
    const char *const *dlls;
    size_t dll_count;
    int gitlab;
};
static const char *const vkd3d_dlls[] = { "d3d12.dll", "d3d12core.dll" };

struct buffer
{
    unsigned char *data;
    size_t size, capacity, limit;
};

struct http_progress
{
    dxvk_progress_callback callback;
    void *opaque;
};

static const char *const dxvk_dlls[] =
{
    "d3d8.dll", "d3d9.dll", "d3d10.dll", "d3d10_1.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"
};
static const char *const sarek_dlls[] =
{
    "ddraw.dll", "d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"
};

static const struct release_backend dxvk_backend = {
    "dxvk", "doitsujin/dxvk", "https://github.com/doitsujin/dxvk/", "dxvk", "gz",
    "dxvk", "dxvk64", dxvk_dlls, sizeof(dxvk_dlls) / sizeof(dxvk_dlls[0]), 0
};
static const struct release_backend sarek_backend = {
    "dxvk-sarek", "pythonlover02/dxvk-sarek", "https://github.com/pythonlover02/dxvk-sarek/", "dxvk-sarek", "gz",
    "dxvk-sarek", "dxvk-sarek64", sarek_dlls, sizeof(sarek_dlls) / sizeof(sarek_dlls[0]), 0
};
static const struct release_backend gplasync_backend = {
    "dxvk-gplasync", "43488626", "https://gitlab.com/Ph42oN/dxvk-gplasync/", "dxvk-gplasync", "gz",
    "dxvk-gplasync", "dxvk-gplasync64", dxvk_dlls, sizeof(dxvk_dlls) / sizeof(dxvk_dlls[0]), 1
};
static const struct release_backend *const dxvk_backends[DXVK_SOURCE_COUNT] = {
    &dxvk_backend, &sarek_backend, &gplasync_backend
};
static const struct release_backend vkd3d_backend = {
    "vkd3d", "HansKristian-Work/vkd3d-proton", "https://github.com/HansKristian-Work/vkd3d-proton/",
    "vkd3d-proton", "zst", "vkd3d", "vkd3d64", vkd3d_dlls, 2, 0
};

static size_t receive_data( void *data, size_t size, size_t count, void *opaque )
{
    struct buffer *buffer = opaque;
    unsigned char *grown;
    size_t bytes, capacity;

    if (count && size > SIZE_MAX / count) return 0;
    bytes = size * count;
    if (bytes > buffer->limit - buffer->size) return 0;
    if (buffer->size + bytes + 1 > buffer->capacity)
    {
        capacity = buffer->capacity ? buffer->capacity : 16384;
        while (capacity < buffer->size + bytes + 1)
        {
            if (capacity > buffer->limit / 2) { capacity = buffer->limit + 1; break; }
            capacity *= 2;
        }
        if (capacity > buffer->limit + 1 || !(grown = realloc( buffer->data, capacity ))) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy( buffer->data + buffer->size, data, bytes );
    buffer->size += bytes;
    buffer->data[buffer->size] = 0;
    return bytes;
}

static int receive_progress( void *opaque, curl_off_t total, curl_off_t current,
                             curl_off_t upload_total, curl_off_t upload_current )
{
    struct http_progress *progress = opaque;

    (void)upload_total;
    (void)upload_current;
    return progress->callback( progress->opaque, DXVK_PROGRESS_DOWNLOAD,
                        current > 0 ? (unsigned long long)current : 0,
                        total > 0 ? (unsigned long long)total : 0 );
}

static enum dxvk_result http_get( const char *url, size_t limit, struct buffer *body,
                                  dxvk_progress_callback callback, void *opaque )
{
    CURL *curl;
    CURLcode code;
    struct curl_slist *headers = NULL;
    struct http_progress progress = { callback, opaque };
    long status = 0;
    char error[CURL_ERROR_SIZE] = "", diagnostic[512];

    memset( body, 0, sizeof(*body) );
    body->limit = limit;
    if (curl_global_init( CURL_GLOBAL_DEFAULT ) != CURLE_OK || !(curl = curl_easy_init()))
        return DXVK_NETWORK_ERROR;
    if (!strncmp( url, "https://api.github.com/", 23 ))
    {
        headers = curl_slist_append( headers, "Accept: application/vnd.github+json" );
        headers = curl_slist_append( headers, "X-GitHub-Api-Version: 2022-11-28" );
    }
    curl_easy_setopt( curl, CURLOPT_URL, url );
    curl_easy_setopt( curl, CURLOPT_HTTPHEADER, headers );
    curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, receive_data );
    curl_easy_setopt( curl, CURLOPT_WRITEDATA, body );
    curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, 1L );
    curl_easy_setopt( curl, CURLOPT_MAXREDIRS, 5L );
    curl_easy_setopt( curl, CURLOPT_CONNECTTIMEOUT, 12L );
    curl_easy_setopt( curl, CURLOPT_TIMEOUT, 180L );
    curl_easy_setopt( curl, CURLOPT_LOW_SPEED_LIMIT, 1024L );
    curl_easy_setopt( curl, CURLOPT_LOW_SPEED_TIME, 20L );
    curl_easy_setopt( curl, CURLOPT_NOSIGNAL, 1L );
    curl_easy_setopt( curl, CURLOPT_USERAGENT, "Autorun/Graphics" );
    curl_easy_setopt( curl, CURLOPT_ERRORBUFFER, error );
    if (callback)
    {
        curl_easy_setopt( curl, CURLOPT_NOPROGRESS, 0L );
        curl_easy_setopt( curl, CURLOPT_XFERINFOFUNCTION, receive_progress );
        curl_easy_setopt( curl, CURLOPT_XFERINFODATA, &progress );
    }
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt( curl, CURLOPT_PROTOCOLS_STR, "https" );
    curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS_STR, "https" );
#else
    curl_easy_setopt( curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS );
    curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS );
#endif
    code = curl_easy_perform( curl );
    curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &status );
    curl_slist_free_all( headers );
    curl_easy_cleanup( curl );
    if (code == CURLE_OK && status >= 200 && status < 300) return DXVK_OK;
    if (code != CURLE_ABORTED_BY_CALLBACK)
    {
        snprintf( diagnostic, sizeof(diagnostic), "[Graphics] request failed: curl=%d (%s), http=%ld, errno=%d",
                  (int)code, error[0] ? error : curl_easy_strerror( code ), status, errno );
        wine_nx_runtime_trace( diagnostic );
    }
    free( body->data );
    memset( body, 0, sizeof(*body) );
    return code == CURLE_ABORTED_BY_CALLBACK ? DXVK_CANCELLED : status == 404 ? DXVK_NOT_FOUND : DXVK_NETWORK_ERROR;
}

static const char *json_field( const char *object, const char *end, const char *name )
{
    char key[64];
    const char *p;
    size_t length;

    snprintf( key, sizeof(key), "\"%s\"", name );
    length = strlen( key );
    for (p = object; p && p < end; p = strstr( p + 1, key ))
    {
        const char *colon;

        if (strncmp( p, key, length )) continue;
        colon = strchr( p + length, ':' );
        if (!colon || colon >= end) return NULL;
        do colon++; while (colon < end && isspace( (unsigned char)*colon ));
        return colon;
    }
    return NULL;
}

static int json_string( const char *p, const char *end, char *out, size_t size )
{
    size_t used = 0;

    if (!p || p >= end || *p++ != '"') return 0;
    while (p < end && *p != '"')
    {
        unsigned char c = *p++;

        if (c == '\\' && p < end)
        {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'u') { if (end - p < 4) return 0; p += 4; c = '?'; }
        }
        if (used + 1 < size) out[used++] = c;
    }
    if (p >= end) return 0;
    out[used] = 0;
    return 1;
}

static int json_integer( const char *p, const char *end, unsigned long long *value )
{
    char *tail;

    if (!p || p >= end) return 0;
    *value = strtoull( p, &tail, 10 );
    return tail != p && tail <= end;
}

static int json_boolean( const char *p, const char *end, int *value )
{
    if (!p || p >= end) return 0;
    if (end - p >= 4 && !memcmp( p, "true", 4 )) { *value = 1; return 1; }
    if (end - p >= 5 && !memcmp( p, "false", 5 )) { *value = 0; return 1; }
    return 0;
}

static int next_object( const char **cursor, const char *end, const char **start, const char **finish )
{
    const char *p = *cursor;
    int depth = 0, quoted = 0, escaped = 0;

    while (p < end && *p != '{' && *p != ']') p++;
    if (p == end || *p == ']') return 0;
    *start = p;
    for (; p < end; p++)
    {
        char c = *p;

        if (quoted)
        {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
        }
        else if (c == '"') quoted = 1;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0)
        {
            *finish = p + 1;
            *cursor = p + 1;
            return 1;
        }
    }
    return 0;
}

static int json_array( const char *object, const char *end, const char *name,
                       const char **start, const char **finish )
{
    const char *p = json_field( object, end, name );
    int depth = 0, quoted = 0, escaped = 0;

    if (!p || p >= end || *p != '[') return 0;
    *start = p + 1;
    for (; p < end; p++)
    {
        char c = *p;

        if (quoted)
        {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
        }
        else if (c == '"') quoted = 1;
        else if (c == '[') depth++;
        else if (c == ']' && --depth == 0) { *finish = p; return 1; }
    }
    return 0;
}

static int asset_rank( const struct release_backend *backend, const char *name, const char *version )
{
    char expected[80];

    if (backend == &sarek_backend)
    {
        snprintf( expected, sizeof(expected), "dxvk-sarek-%s.tar.gz", version );
        if (!strcmp( name, expected )) return 3;
        snprintf( expected, sizeof(expected), "dxvk-sarek-v%s.tar.gz", version );
        if (!strcmp( name, expected )) return 2;
        snprintf( expected, sizeof(expected), "dxvk-sarek-dyasync-v%s.tar.gz", version );
        return !strcmp( name, expected );
    }
    snprintf( expected, sizeof(expected), "%s-%s%s.tar.%s", backend->asset,
              backend->gitlab ? "v" : "", version, backend->extension );
    return !strcmp( name, expected );
}

static int parse_release_page( const struct release_backend *backend, const unsigned char *json, size_t size, struct dxvk_release *releases,
                               int max_releases, int *seen )
{
    const char *begin = (const char *)json, *end = begin + size, *cursor, *object, *object_end;
    int added = 0;

    cursor = memchr( begin, '[', size );
    if (!cursor) return -1;
    cursor++;
    while (next_object( &cursor, end, &object, &object_end ))
    {
        const char *assets, *assets_end, *asset_cursor, *asset, *asset_end;
        struct dxvk_release release;
        char tag[40], asset_name[80], digest[80];
        int draft = 0, best = 0;

        (*seen)++;
        memset( &release, 0, sizeof(release) );
        if (!json_string( json_field( object, object_end, "tag_name" ), object_end, tag, sizeof(tag) ) ||
            (!backend->gitlab && (!json_boolean( json_field( object, object_end, "draft" ), object_end, &draft ) || draft)))
            continue;
        json_boolean( json_field( object, object_end, "prerelease" ), object_end, &release.prerelease );
        if (tag[0] == 'v' || tag[0] == 'V') memmove( tag, tag + 1, strlen( tag ) );
        if (!launcher_dxvk_version_valid( tag )) continue;
        memcpy( release.version, tag, strlen( tag ) + 1 );
        if (backend->gitlab)
        {
            const char *value = json_field( object, object_end, "assets" );
            if (!value || *value != '{' || !next_object( &value, object_end, &asset, &asset_end ) ||
                !json_array( asset, asset_end, "links", &assets, &assets_end )) continue;
        }
        else if (!json_array( object, object_end, "assets", &assets, &assets_end )) continue;
        asset_cursor = assets;
        while (next_object( &asset_cursor, assets_end, &asset, &asset_end ))
        {
            struct dxvk_release candidate = release;
            int rank;

            candidate.url[0] = candidate.digest[0] = 0;
            candidate.size = 0;
            if (!json_string( json_field( asset, asset_end, "name" ), asset_end,
                              asset_name, sizeof(asset_name) ) || !(rank = asset_rank( backend, asset_name, tag )) ||
                rank <= best) continue;
            if (!json_string( json_field( asset, asset_end, "browser_download_url" ), asset_end,
                              candidate.url, sizeof(candidate.url) ) &&
                !json_string( json_field( asset, asset_end, "direct_asset_url" ), asset_end,
                              candidate.url, sizeof(candidate.url) )) continue;
            if (strncmp( candidate.url, backend->prefix, strlen(backend->prefix) )) continue;
            json_integer( json_field( asset, asset_end, "size" ), asset_end, &candidate.size );
            if (json_string( json_field( asset, asset_end, "digest" ), asset_end, digest, sizeof(digest) ) &&
                !strncmp( digest, "sha256:", 7 ) && strlen( digest + 7 ) == 64)
                snprintf( candidate.digest, sizeof(candidate.digest), "%s", digest + 7 );
            release = candidate;
            best = rank;
        }
        if (!release.url[0]) continue;
        if (added < max_releases) releases[added++] = release;
    }
    return added;
}

static int make_directory( const char *path )
{
    if (!mkdir( path, 0777 ) || errno == EEXIST) return 1;
    return 0;
}

static int make_directories( const char *path )
{
    char copy[768], *p;

    if (strlen( path ) >= sizeof(copy)) return 0;
    strcpy( copy, path );
    for (p = strchr( copy, '/' ); p; p = strchr( p + 1, '/' ))
    {
        if (p == copy || p[-1] == ':') continue;
        *p = 0;
        if (!make_directory( copy )) return 0;
        *p = '/';
    }
    return make_directory( copy );
}

static int remove_tree( const char *path )
{
    DIR *directory = opendir( path );
    struct dirent *entry;
    int ok = 1;

    if (!directory) return errno == ENOENT;
    while ((entry = readdir( directory )))
    {
        char child[896];
        struct stat st;

        if (!strcmp( entry->d_name, "." ) || !strcmp( entry->d_name, ".." )) continue;
        if ((size_t)snprintf( child, sizeof(child), "%s/%s", path, entry->d_name ) >= sizeof(child) ||
            lstat( child, &st )) { ok = 0; continue; }
        if (S_ISDIR( st.st_mode )) { if (!remove_tree( child )) ok = 0; }
        else if (remove( child )) ok = 0;
    }
    closedir( directory );
    if (rmdir( path )) ok = 0;
    return ok;
}

static int write_atomic( const char *path, const void *data, size_t size )
{
    char temp[896];
    FILE *file;
    int ok;

    if ((size_t)snprintf( temp, sizeof(temp), "%s.new", path ) >= sizeof(temp) || !(file = fopen( temp, "wb" )))
        return 0;
    ok = fwrite( data, 1, size, file ) == size && !fflush( file );
    if (fclose( file )) ok = 0;
    if (ok) remove( path );
    if (!ok || rename( temp, path )) { remove( temp ); return 0; }
    return 1;
}

static int cache_path( const struct release_backend *backend, const char *runtime_dir, char *folder, size_t folder_size, char *path, size_t path_size )
{
    int folder_length = snprintf( folder, folder_size, "%s/cache", runtime_dir );
    int path_length;

    if (folder_length <= 0 || (size_t)folder_length >= folder_size) return 0;
    path_length = snprintf( path, path_size, "%s/%s-releases.txt", folder, backend->id );
    return path_length > 0 && (size_t)path_length < path_size;
}

static int save_catalog( const struct release_backend *backend, const char *runtime_dir, const struct dxvk_release *releases, int count )
{
    char folder[768], path[800], temp[808];
    FILE *file;
    int i, ok = 1;

    if (!cache_path( backend, runtime_dir, folder, sizeof(folder), path, sizeof(path) ) || !make_directories( folder ) ||
        (size_t)snprintf( temp, sizeof(temp), "%s.new", path ) >= sizeof(temp) || !(file = fopen( temp, "wb" )))
        return 0;
    for (i = 0; i < count; i++)
        if (fprintf( file, "%s\t%d\t%llu\t%s\t%s\n", releases[i].version, releases[i].prerelease,
                     releases[i].size, releases[i].digest, releases[i].url ) < 0) { ok = 0; break; }
    if (fclose( file )) ok = 0;
    if (ok) remove( path );
    if (!ok || rename( temp, path )) { remove( temp ); return 0; }
    return 1;
}

static int split_field( char **cursor, char **value )
{
    char *start = *cursor, *tab;

    if (!start) return 0;
    tab = strchr( start, '\t' );
    if (tab) { *tab = 0; *cursor = tab + 1; }
    else *cursor = NULL;
    *value = start;
    return 1;
}

static int load_catalog( const struct release_backend *backend, const char *runtime_dir, struct dxvk_release *releases, int max_releases,
                         int *count )
{
    char folder[768], path[800], line[1024];
    FILE *file;
    int n = 0;
    if (!cache_path( backend, runtime_dir, folder, sizeof(folder), path, sizeof(path) ) ||
        !(file = fopen( path, "rb" ))) return 0;
    while (n < max_releases && fgets( line, sizeof(line), file ))
    {
        struct dxvk_release release;
        char *cursor = line, *version, *prerelease, *size, *digest, *url, *end;

        memset( &release, 0, sizeof(release) );
        if (!split_field( &cursor, &version ) || !split_field( &cursor, &prerelease ) ||
            !split_field( &cursor, &size ) || !split_field( &cursor, &digest ) ||
            !split_field( &cursor, &url )) continue;
        url[strcspn( url, "\r\n" )] = 0;
        if (!launcher_dxvk_version_valid( version ) ||
            strncmp( url, backend->prefix, strlen(backend->prefix) )) continue;
        release.prerelease = strtol( prerelease, &end, 10 );
        if (*end || (release.prerelease != 0 && release.prerelease != 1)) continue;
        release.size = strtoull( size, &end, 10 );
        if (*end || (digest[0] && strlen( digest ) != 64)) continue;
        memcpy( release.version, version, strlen( version ) + 1 );
        snprintf( release.digest, sizeof(release.digest), "%s", digest );
        snprintf( release.url, sizeof(release.url), "%s", url );
        releases[n++] = release;
    }
    fclose( file );
    *count = n;
    return n > 0;
}

static enum dxvk_result backend_release_catalog( const struct release_backend *backend, const char *runtime_dir, struct dxvk_release *releases,
                                       int max_releases, int *count, int cache_only,
                                       dxvk_progress_callback progress, void *opaque )
{
    int total = 0, page;

    if (!runtime_dir || !releases || max_releases <= 0 || !count) return DXVK_INVALID_RESPONSE;
    if (max_releases > DXVK_MAX_RELEASES) max_releases = DXVK_MAX_RELEASES;
    *count = 0;
    if (cache_only) return load_catalog( backend, runtime_dir, releases, max_releases, count ) ? DXVK_OK : DXVK_NOT_FOUND;
    for (page = 1; total < max_releases; page++)
    {
        char url[160];
        struct buffer body;
        enum dxvk_result result;
        int seen = 0, added;

        if (backend->gitlab)
            snprintf( url, sizeof(url),
                      "https://gitlab.com/api/v4/projects/%s/releases?per_page=100&page=%d", backend->repo, page );
        else
            snprintf( url, sizeof(url),
                      "https://api.github.com/repos/%s/releases?per_page=100&page=%d", backend->repo, page );
        if ((result = http_get( url, DXVK_API_BODY_MAX, &body, progress, opaque )) != DXVK_OK) return result;
        added = parse_release_page( backend, body.data, body.size, releases + total, max_releases - total, &seen );
        free( body.data );
        if (added < 0) return DXVK_INVALID_RESPONSE;
        total += added;
        if (seen < 100 || page >= 4) break;
    }
    if (!total) return DXVK_NOT_FOUND;
    *count = total;
    save_catalog( backend, runtime_dir, releases, total );
    return DXVK_OK;
}

static int approved_dll( const struct release_backend *backend, const char *name )
{
    size_t i;

    for (i = 0; i < backend->dll_count; i++)
        if (!strcasecmp( name, backend->dlls[i] )) return 1;
    return 0;
}

static unsigned long long tar_size( const unsigned char *field, size_t length, int *ok )
{
    unsigned long long value = 0;
    size_t i = 0;

    while (i < length && (field[i] == ' ' || field[i] == 0)) i++;
    for (; i < length && field[i]; i++)
    {
        if (field[i] == ' ') break;
        if (field[i] < '0' || field[i] > '7') { *ok = 0; return 0; }
        if (value > (UINT64_MAX - 7) / 8) { *ok = 0; return 0; }
        value = value * 8 + field[i] - '0';
    }
    return value;
}

struct archive_reader
{
    gzFile gzip;
    FILE *file;
    ZSTD_DStream *zstd;
    ZSTD_inBuffer input;
    unsigned char buffer[32768];
};

static int read_archive( struct archive_reader *archive, void *data, size_t size )
{
    unsigned char *out = data;
    if (archive->gzip)
    {
        while (size)
        {
            int count = gzread( archive->gzip, out, size > 65536 ? 65536 : (unsigned int)size );
            if (count <= 0) return 0;
            out += count;
            size -= count;
        }
        return 1;
    }
    ZSTD_outBuffer output = { data, size, 0 };
    while (output.pos < output.size)
    {
        if (archive->input.pos == archive->input.size)
        {
            archive->input.size = fread( archive->buffer, 1, sizeof(archive->buffer), archive->file );
            archive->input.pos = 0;
            if (!archive->input.size) return 0;
        }
        if (ZSTD_isError( ZSTD_decompressStream( archive->zstd, &output, &archive->input ) )) return 0;
    }
    return 1;
}

static int discard_archive( struct archive_reader *archive, unsigned long long size )
{
    unsigned char buffer[32768];

    while (size)
    {
        size_t part = size < sizeof(buffer) ? (size_t)size : sizeof(buffer);

        if (!read_archive( archive, buffer, part )) return 0;
        size -= part;
    }
    return 1;
}

static int extract_archive( const struct release_backend *backend, const char *archive_path, const char *x32, const char *x64,
                            int *x32_files, int *x64_files )
{
    unsigned char header[512], buffer[65536];
    struct archive_reader reader = {0}, *archive = &reader;
    int zero_blocks = 0, ok = 1;

    if (!strcmp( backend->extension, "zst" ))
    {
        archive->file = fopen( archive_path, "rb" );
        archive->zstd = ZSTD_createDStream();
        if (!archive->file || !archive->zstd || ZSTD_isError( ZSTD_initDStream( archive->zstd ) ))
        {
            if (archive->file) fclose( archive->file );
            ZSTD_freeDStream( archive->zstd );
            return 0;
        }
        archive->input.src = archive->buffer;
    }
    else if (!(archive->gzip = gzopen( archive_path, "rb" ))) return 0;
    while (read_archive( archive, header, sizeof(header) ))
    {
        char name[101], *slash, *arch;
        const char *destination = NULL;
        unsigned long long size, padded, left;
        FILE *output = NULL;
        int field_ok = 1, regular;

        if (!memcmp( header, (unsigned char[512]){0}, 512))
        {
            if (++zero_blocks == 2) break;
            continue;
        }
        zero_blocks = 0;
        memcpy( name, header, 100 );
        name[100] = 0;
        if (!memchr( header, 0, 100)) { ok = 0; break; }
        size = tar_size( header + 124, 12, &field_ok );
        if (!field_ok || size > 64u * 1024u * 1024u) { ok = 0; break; }
        regular = !header[156] || header[156] == '0';
        slash = strrchr( name, '/' );
        if (regular && slash && approved_dll( backend, slash + 1 ))
        {
            *slash = 0;
            arch = strrchr( name, '/' );
            arch = arch ? arch + 1 : name;
            if (!strcmp( arch, backend == &vkd3d_backend ? "x86" : "x32" )) destination = x32;
            else if (!strcmp( arch, "x64" )) destination = x64;
            *slash = '/';
        }
        if (destination)
        {
            char output_path[896];

            if ((size_t)snprintf( output_path, sizeof(output_path), "%s/%s", destination, slash + 1 ) >=
                sizeof(output_path) || !(output = fopen( output_path, "wb" ))) { ok = 0; break; }
        }
        left = size;
        while (left)
        {
            size_t part = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);

            if (!read_archive( archive, buffer, part ) || (output && fwrite( buffer, 1, part, output ) != part))
            { ok = 0; break; }
            left -= part;
        }
        if (output && fclose( output )) ok = 0;
        if (!ok) break;
        if (destination)
        {
            if (destination == x32) (*x32_files)++;
            else (*x64_files)++;
        }
        padded = (512 - size % 512) % 512;
        if (padded && !discard_archive( archive, padded )) { ok = 0; break; }
    }
    if (archive->gzip && gzclose( archive->gzip ) != Z_OK) ok = 0;
    if (archive->file) fclose( archive->file );
    ZSTD_freeDStream( archive->zstd );
    if (zero_blocks != 2) ok = 0;
    return ok;
}

static int valid_pe( const char *path, unsigned short machine )
{
    unsigned char dos[64], header[26];
    uint32_t offset;
    unsigned short actual, characteristics, magic;
    FILE *file = fopen( path, "rb" );

    if (!file) return 0;
    if (fread( dos, 1, sizeof(dos), file ) != sizeof(dos) || dos[0] != 'M' || dos[1] != 'Z')
    { fclose( file ); return 0; }
    offset = (uint32_t)dos[0x3c] | (uint32_t)dos[0x3d] << 8 |
             (uint32_t)dos[0x3e] << 16 | (uint32_t)dos[0x3f] << 24;
    if (offset < 64 || offset > 0x7fffffff || fseek( file, offset, SEEK_SET ) ||
        fread( header, 1, sizeof(header), file ) != sizeof(header))
    { fclose( file ); return 0; }
    fclose( file );
    actual = header[4] | header[5] << 8;
    characteristics = header[22] | header[23] << 8;
    magic = header[24] | header[25] << 8;
    return !memcmp( header, "PE\0\0", 4 ) && actual == machine &&
           magic == (machine == 0x8664 ? 0x20b : 0x10b) && (characteristics & 0x2000);
}

static int validate_payload( const struct release_backend *backend, const char *path, unsigned short machine,
                             const char *version )
{
    size_t i;
    int valid = 0;

    if (backend == &vkd3d_backend)
    {
        char dll[896];
        if ((size_t)snprintf( dll, sizeof(dll), "%s/d3d12.dll", path ) >= sizeof(dll) ||
            !valid_pe( dll, machine )) return 0;
    }
    for (i = 0; i < backend->dll_count; i++)
    {
        char dll[896];
        struct stat st;

        if ((size_t)snprintf( dll, sizeof(dll), "%s/%s", path, backend->dlls[i] ) >= sizeof(dll) ||
            stat( dll, &st )) continue;
        if (!S_ISREG( st.st_mode ) || !valid_pe( dll, machine )) return 0;
        valid++;
    }
    if (backend == &vkd3d_backend)
    {
        unsigned int major = 0, minor = 0;
        if (!version || !version[0] ||
            (sscanf( version, "%u.%u", &major, &minor ) == 2 &&
             (major > 2 || (major == 2 && minor >= 9)))) return valid == 2;
    }
    return valid > 0;
}

static int write_release_info( const char *path, const struct dxvk_release *release )
{
    char info[1024], file[896];
    int length;

    length = snprintf( info, sizeof(info), "version=%s\nsource=%s\nsha256=%s\n",
                       release->version, release->url, release->digest );
    if (length <= 0 || (size_t)length >= sizeof(info) ||
        (size_t)snprintf( file, sizeof(file), "%s/release.ini", path ) >= sizeof(file)) return 0;
    return write_atomic( file, info, length );
}

static int verify_digest( const struct buffer *body, const char *expected )
{
    unsigned char digest[SHA256_HASH_SIZE];
    char hex[SHA256_HASH_SIZE * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (!expected[0]) return 1;
    sha256CalculateHash( digest, body->data, body->size );
    for (i = 0; i < sizeof(digest); i++)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = 0;
    return !strcasecmp( hex, expected );
}

static enum dxvk_result backend_install_release( const struct release_backend *backend, const char *runtime_dir, const struct dxvk_release *release,
                                       dxvk_progress_callback progress, void *opaque )
{
    char cache[768], archive_path[896], x32_base[768], x64_base[768];
    char x32_new[896], x64_new[896], x32_final[896], x64_final[896];
    struct buffer body;
    enum dxvk_result result;
    int x32_files = 0, x64_files = 0;

    if (!runtime_dir || !release || !launcher_dxvk_version_valid( release->version ) ||
        strncmp( release->url, backend->prefix, strlen(backend->prefix) )) return DXVK_INVALID_RESPONSE;
    if ((size_t)snprintf( cache, sizeof(cache), "%s/cache", runtime_dir ) >= sizeof(cache) ||
        !make_directories( cache ) ||
        (size_t)snprintf( archive_path, sizeof(archive_path), "%s/%s-%s.download", cache,
                          backend->id, release->version ) >= sizeof(archive_path) ||
        (size_t)snprintf( x32_base, sizeof(x32_base), "%s/drive_c/%s/versions", runtime_dir, backend->x32 ) >= sizeof(x32_base) ||
        (size_t)snprintf( x64_base, sizeof(x64_base), "%s/drive_c/%s/versions", runtime_dir, backend->x64 ) >= sizeof(x64_base) ||
        !make_directories( x32_base ) || !make_directories( x64_base )) return DXVK_IO_ERROR;
    snprintf( x32_new, sizeof(x32_new), "%s/%s.new", x32_base, release->version );
    snprintf( x64_new, sizeof(x64_new), "%s/%s.new", x64_base, release->version );
    snprintf( x32_final, sizeof(x32_final), "%s/%s", x32_base, release->version );
    snprintf( x64_final, sizeof(x64_final), "%s/%s", x64_base, release->version );
    remove_tree( x32_new );
    remove_tree( x64_new );
    if (!make_directory( x32_new ) || !make_directory( x64_new )) return DXVK_IO_ERROR;
    if (progress && progress( opaque, DXVK_PROGRESS_DOWNLOAD, 0, release->size ))
    { result = DXVK_CANCELLED; goto failed; }
    if ((result = http_get( release->url, DXVK_ARCHIVE_MAX, &body, progress, opaque )) != DXVK_OK) goto failed;
    if (progress && progress( opaque, DXVK_PROGRESS_VERIFY, body.size, body.size ))
    { result = DXVK_CANCELLED; goto free_failed; }
    if (body.size < 4 || (strcmp( backend->extension, "zst" ) ?
        (body.data[0] != 0x1f || body.data[1] != 0x8b) : memcmp( body.data, "\x28\xb5\x2f\xfd", 4 )))
    { result = DXVK_INVALID_ARCHIVE; goto free_failed; }
    if (!verify_digest( &body, release->digest )) { result = DXVK_HASH_MISMATCH; goto free_failed; }
    if (!write_atomic( archive_path, body.data, body.size )) { result = DXVK_IO_ERROR; goto free_failed; }
    free( body.data );
    memset( &body, 0, sizeof(body) );
    if (progress && progress( opaque, DXVK_PROGRESS_INSTALL, 0, 0 ))
    { result = DXVK_CANCELLED; goto failed; }
    if (!extract_archive( backend, archive_path, x32_new, x64_new, &x32_files, &x64_files ) ||
        !x32_files || !x64_files || !validate_payload( backend, x32_new, 0x014c, release->version ) ||
        !validate_payload( backend, x64_new, 0x8664, release->version )) { result = DXVK_INVALID_ARCHIVE; goto failed; }
    if (!write_release_info( x32_new, release ) || !write_release_info( x64_new, release ))
    { result = DXVK_IO_ERROR; goto failed; }
    remove_tree( x32_final );
    remove_tree( x64_final );
    if (rename( x32_new, x32_final ) || rename( x64_new, x64_final ))
    { result = DXVK_IO_ERROR; goto failed; }
    remove( archive_path );
    return DXVK_OK;

free_failed:
    free( body.data );
failed:
    remove( archive_path );
    remove_tree( x32_new );
    remove_tree( x64_new );
    return result;
}

static int payload_path( const struct release_backend *backend, const char *runtime_dir, unsigned short machine, const char *version,
                         char *path, size_t size )
{
    const char *base = machine == 0x014c ? backend->x32 : machine == 0x8664 ? backend->x64 : NULL;
    int length;

    if (!base || (version && version[0] && !launcher_dxvk_version_valid( version ))) return 0;
    if (version && version[0])
        length = snprintf( path, size, "%s/drive_c/%s/versions/%s", runtime_dir, base, version );
    else length = snprintf( path, size, "%s/drive_c/%s", runtime_dir, base );
    return length >= 0 && (size_t)length < size;
}

static int backend_release_installed( const struct release_backend *backend, const char *runtime_dir, unsigned short machine, const char *version )
{
    char path[896];
    struct stat st;

    return payload_path( backend, runtime_dir, machine, version, path, sizeof(path) ) &&
           !stat( path, &st ) && S_ISDIR( st.st_mode ) && validate_payload( backend, path, machine, version );
}

static int manifest_version( const char *path, const char *name, char *version, size_t size )
{
    char *text;
    const char *begin, *end, *object, *object_end, *cursor;
    FILE *file;
    long length;
    int valid = 0;

    if (!(file = fopen( path, "rb" ))) return 0;
    if (fseek( file, 0, SEEK_END ) || (length = ftell( file )) <= 0 || length > 4 * 1024 * 1024 ||
        fseek( file, 0, SEEK_SET ) || !(text = malloc( (size_t)length + 1 )))
    {
        fclose( file );
        return 0;
    }
    if (fread( text, 1, (size_t)length, file ) != (size_t)length) goto done;
    text[length] = 0;
    begin = text;
    end = text + length;
    object = begin;
    object_end = end;
    if (name)
    {
        if (!(cursor = json_field( begin, end, name )) || *cursor != '{' ||
            !next_object( &cursor, end, &object, &object_end )) goto done;
    }
    valid = json_string( json_field( object, object_end, "version" ), object_end, version, size ) &&
            launcher_dxvk_version_valid( version );
done:
    fclose( file );
    free( text );
    return valid;
}

static int backend_root_version( const struct release_backend *backend, const char *runtime_dir, unsigned short machine, char *version, size_t size )
{
    char root[896], path[920], found[32];

    if (!version || !size) return 0;
    version[0] = 0;
    if (!payload_path( backend, runtime_dir, machine, "", root, sizeof(root) ) ||
        (size_t)snprintf( path, sizeof(path), "%s/%s-manifest.json", root, backend->id ) >= sizeof(path)) return 0;
    if (!manifest_version( path, NULL, found, sizeof(found) ))
    {
        if (!validate_payload( backend, root, machine, "" ) ||
            (size_t)snprintf( path, sizeof(path), "%s/build-manifest.json", runtime_dir ) >= sizeof(path) ||
            !manifest_version( path, backend->id, found, sizeof(found) )) return 0;
    }
    if (strlen( found ) >= size) return 0;
    strcpy( version, found );
    return 1;
}

static int stable_version( const char *version )
{
    const unsigned char *p = (const unsigned char *)version;

    if (!launcher_dxvk_version_valid( version )) return 0;
    for (;;)
    {
        if (!isdigit( *p )) return 0;
        while (isdigit( *p )) p++;
        if (*p != '.') break;
        p++;
    }
    return !*p || (isalpha( *p ) && !p[1]);
}

static int backend_stable_version( const struct release_backend *backend, const char *version )
{
    const char *suffix;

    if (backend != &gplasync_backend) return stable_version( version );
    if (!launcher_dxvk_version_selectable( version ) || !(suffix = strrchr( version, '-' )) || !suffix[1])
        return 0;
    for (const char *p = suffix + 1; *p; p++) if (!isdigit( (unsigned char)*p )) return 0;
    for (const char *p = version; p < suffix; p++)
        if (!isdigit( (unsigned char)*p ) && *p != '.') return 0;
    return 1;
}

static int compare_versions( const char *a, const char *b )
{
    while (*a || *b)
    {
        if (isdigit( (unsigned char)*a ) || isdigit( (unsigned char)*b ))
        {
            char *end_a, *end_b;
            unsigned long va = strtoul( a, &end_a, 10 ), vb = strtoul( b, &end_b, 10 );

            if (va != vb) return va > vb ? 1 : -1;
            a = end_a;
            b = end_b;
        }
        else if (*a == '.' || *b == '.')
        {
            if (*a == '.') a++;
            if (*b == '.') b++;
        }
        else return strcasecmp( a, b );
    }
    return 0;
}

static void backend_resolve_version( const struct release_backend *backend, const char *runtime_dir,
                                    unsigned short machine, const char *requested, struct dxvk_version *selected )
{
    char root[896], path[920], bundled[32] = "";
    struct dirent *entry;
    DIR *directory;

    memset( selected, 0, sizeof(*selected) );
    if (!payload_path( backend, runtime_dir, machine, "", root, sizeof(root) )) return;
    if (requested && requested[0])
    {
        snprintf( selected->version, sizeof(selected->version), "%s", requested );
        if (!launcher_dxvk_version_valid( requested )) return;
        if (backend_release_installed( backend, runtime_dir, machine, requested ))
        {
            selected->installed = 1;
            return;
        }
        backend_root_version( backend, runtime_dir, machine, bundled, sizeof(bundled) );
        if (strcmp( bundled, requested )) return;
    }
    if (backend_release_installed( backend, runtime_dir, machine, "" ))
    {
        selected->installed = selected->bundled = 1;
        backend_root_version( backend, runtime_dir, machine, selected->version, sizeof(selected->version) );
    }
    if (requested && requested[0]) return;
    if ((size_t)snprintf( path, sizeof(path), "%s/versions", root ) >= sizeof(path) ||
        !(directory = opendir( path ))) return;
    while ((entry = readdir( directory )))
    {
        if (!backend_stable_version( backend, entry->d_name ) ||
            (backend != &vkd3d_backend && !launcher_dxvk_version_selectable( entry->d_name )) ||
            (selected->version[0] && compare_versions( entry->d_name, selected->version ) <= 0) ||
            !backend_release_installed( backend, runtime_dir, machine, entry->d_name )) continue;
        strcpy( selected->version, entry->d_name );
        selected->installed = 1;
        selected->bundled = 0;
    }
    closedir( directory );
}

const char *dxvk_result_message( enum dxvk_result result )
{
    switch (result)
    {
    case DXVK_OK: return "The selected release is ready.";
    case DXVK_NETWORK_ERROR: return "Could not connect to the release server.";
    case DXVK_NOT_FOUND: return "No compatible release asset was found.";
    case DXVK_INVALID_RESPONSE: return "The release catalog is invalid.";
    case DXVK_INVALID_ARCHIVE: return "The downloaded archive is invalid.";
    case DXVK_HASH_MISMATCH: return "The downloaded archive failed SHA-256 verification.";
    case DXVK_IO_ERROR: return "The release could not be written to the SD card.";
    case DXVK_CANCELLED: return "Download cancelled.";
    }
    return "Installation failed.";
}

enum dxvk_result dxvk_release_catalog( enum dxvk_source source, const char *runtime_dir, struct dxvk_release *releases,
                                        int max_releases, int *count, int cache_only,
                                        dxvk_progress_callback progress, void *opaque )
{
    if (source < 0 || source >= DXVK_SOURCE_COUNT) return DXVK_INVALID_RESPONSE;
    return backend_release_catalog( dxvk_backends[source], runtime_dir, releases, max_releases, count, cache_only, progress, opaque );
}

enum dxvk_result dxvk_install_release( enum dxvk_source source, const char *runtime_dir, const struct dxvk_release *release,
                                        dxvk_progress_callback progress, void *opaque )
{
    if (source < 0 || source >= DXVK_SOURCE_COUNT) return DXVK_INVALID_RESPONSE;
    return backend_install_release( dxvk_backends[source], runtime_dir, release, progress, opaque );
}

int dxvk_release_installed( enum dxvk_source source, const char *runtime_dir, unsigned short machine, const char *version )
{
    return source >= 0 && source < DXVK_SOURCE_COUNT &&
           backend_release_installed( dxvk_backends[source], runtime_dir, machine, version );
}

int dxvk_root_version( enum dxvk_source source, const char *runtime_dir, unsigned short machine, char *version, size_t size )
{
    if (source < 0 || source >= DXVK_SOURCE_COUNT) return 0;
    return backend_root_version( dxvk_backends[source], runtime_dir, machine, version, size );
}

void dxvk_resolve_version( enum dxvk_source source, const char *runtime_dir, unsigned short machine, const char *requested,
                           struct dxvk_version *selected )
{
    if (source < 0 || source >= DXVK_SOURCE_COUNT) { memset( selected, 0, sizeof(*selected) ); return; }
    backend_resolve_version( dxvk_backends[source], runtime_dir, machine, requested, selected );
}

enum dxvk_result vkd3d_release_catalog( const char *runtime_dir, struct dxvk_release *releases,
                                        int max_releases, int *count, int cache_only,
                                        dxvk_progress_callback progress, void *opaque )
{
    return backend_release_catalog( &vkd3d_backend, runtime_dir, releases, max_releases, count, cache_only, progress, opaque );
}

enum dxvk_result vkd3d_install_release( const char *runtime_dir, const struct dxvk_release *release,
                                        dxvk_progress_callback progress, void *opaque )
{
    return backend_install_release( &vkd3d_backend, runtime_dir, release, progress, opaque );
}

int vkd3d_release_installed( const char *runtime_dir, unsigned short machine, const char *version )
{
    return backend_release_installed( &vkd3d_backend, runtime_dir, machine, version );
}

int vkd3d_root_version( const char *runtime_dir, unsigned short machine, char *version, size_t size )
{
    return backend_root_version( &vkd3d_backend, runtime_dir, machine, version, size );
}

void vkd3d_resolve_version( const char *runtime_dir, unsigned short machine, const char *requested,
                            struct dxvk_version *selected )
{
    backend_resolve_version( &vkd3d_backend, runtime_dir, machine, requested, selected );
}
