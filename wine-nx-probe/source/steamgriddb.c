/* SteamGridDB portrait-cover client, adapted to Wine-NX's small C launcher
 * from dolphin-nx's CoverDownload implementation. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "steamgriddb.h"

extern void wine_nx_runtime_trace( const char *message );

#define SGDB_BODY_MAX (24u * 1024u * 1024u)
#define SGDB_GAMES 32
#define SGDB_ART 100

struct buffer { unsigned char *data; size_t size, capacity; };
struct game { long id; char name[192]; int score; };
struct artwork { char url[768]; int width, height, score; long reputation; };

static size_t receive_data( void *data, size_t size, size_t count, void *opaque )
{
    struct buffer *buffer = opaque;
    size_t bytes;
    unsigned char *grown;
    if (count && size > SIZE_MAX / count) return 0;
    bytes = size * count;
    if (bytes > SGDB_BODY_MAX - buffer->size) return 0;
    if (buffer->size + bytes + 1 > buffer->capacity)
    {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->size + bytes + 1) capacity *= 2;
        if (!(grown = realloc( buffer->data, capacity ))) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy( buffer->data + buffer->size, data, bytes );
    buffer->size += bytes;
    buffer->data[buffer->size] = 0;
    return bytes;
}

static enum steamgriddb_result http_get( const char *url, const char *key, struct buffer *body )
{
    static int initialized;
    CURL *curl;
    CURLcode code;
    struct curl_slist *headers = NULL;
    char authorization[640];
    long status = 0;
    char curl_error[CURL_ERROR_SIZE] = "";
    char diagnostic[512];
    memset( body, 0, sizeof(*body) );
    if (!initialized)
    {
        if (curl_global_init( CURL_GLOBAL_DEFAULT ) != CURLE_OK) return STEAMGRIDDB_NETWORK_ERROR;
        initialized = 1;
    }
    if (!(curl = curl_easy_init())) return STEAMGRIDDB_NETWORK_ERROR;
    if (key && key[0])
    {
        snprintf( authorization, sizeof(authorization), "Authorization: Bearer %s", key );
        headers = curl_slist_append( headers, authorization );
    }
    curl_easy_setopt( curl, CURLOPT_URL, url );
    curl_easy_setopt( curl, CURLOPT_HTTPHEADER, headers );
    curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, receive_data );
    curl_easy_setopt( curl, CURLOPT_WRITEDATA, body );
    curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, 1L );
    curl_easy_setopt( curl, CURLOPT_MAXREDIRS, 5L );
    curl_easy_setopt( curl, CURLOPT_CONNECTTIMEOUT, 10L );
    curl_easy_setopt( curl, CURLOPT_TIMEOUT, 25L );
    curl_easy_setopt( curl, CURLOPT_NOSIGNAL, 1L );
    curl_easy_setopt( curl, CURLOPT_USERAGENT, "Wine-NX/SteamGridDB" );
    curl_easy_setopt( curl, CURLOPT_ERRORBUFFER, curl_error );
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
    if (code != CURLE_OK)
    {
        snprintf( diagnostic, sizeof(diagnostic), "[STEAMGRIDDB] request failed: curl=%d (%s), http=%ld, errno=%d",
                  (int)code, curl_error[0] ? curl_error : curl_easy_strerror( code ), status, errno );
        wine_nx_runtime_trace( diagnostic );
        free( body->data ); memset( body, 0, sizeof(*body) );
        return STEAMGRIDDB_NETWORK_ERROR;
    }
    if (status == 401 || status == 403) { free( body->data ); return STEAMGRIDDB_NO_KEY; }
    if (status < 200 || status >= 300) { free( body->data ); return STEAMGRIDDB_NOT_FOUND; }
    return STEAMGRIDDB_OK;
}

static void url_encode( const char *text, char *out, size_t size )
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    while (*text && used + 4 < size)
    {
        unsigned char c = *text++;
        if (isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~') out[used++] = c;
        else { out[used++] = '%'; out[used++] = hex[c >> 4]; out[used++] = hex[c & 15]; }
    }
    out[used] = 0;
}

static const char *field( const char *object, const char *end, const char *name )
{
    char key[48];
    const char *p;
    snprintf( key, sizeof(key), "\"%s\"", name );
    for (p = object; p && p < end; p = strstr( p + 1, key ))
    {
        const char *colon;
        if (strncmp( p, key, strlen(key) )) continue;
        colon = strchr( p + strlen(key), ':' );
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
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c == 'u') { if (end - p < 4) return 0; p += 4; c = '?'; }
        }
        if (used + 1 < size) out[used++] = c;
    }
    if (p >= end) return 0;
    out[used] = 0;
    return 1;
}

static int json_long( const char *p, const char *end, long *value )
{
    char *tail;
    if (!p || p >= end) return 0;
    *value = strtol( p, &tail, 10 );
    return tail != p && tail <= end;
}

static int next_object( const char **cursor, const char *end, const char **start, const char **finish )
{
    const char *p = *cursor;
    int depth = 0, quoted = 0, escaped = 0;
    while (p < end && *p != '{') p++;
    if (p == end) return 0;
    *start = p;
    for (; p < end; p++)
    {
        char c = *p;
        if (quoted) { if (escaped) escaped = 0; else if (c == '\\') escaped = 1; else if (c == '"') quoted = 0; }
        else if (c == '"') quoted = 1;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) { *finish = p + 1; *cursor = p + 1; return 1; }
    }
    return 0;
}

/* SteamGridDB wraps result objects in {"success":true,"data":[...]}. Start
 * after that array's opening bracket so next_object returns each result rather
 * than treating the whole response as one object. */
static int data_array( const unsigned char *json, size_t size, const char **cursor, const char **end )
{
    const char *begin = (const char *)json;
    const char *limit = begin + size;
    const char *data = strstr( begin, "\"data\"" );
    const char *array;
    if (!data || data >= limit || !(array = strchr( data + 6, '[' )) || array >= limit) return 0;
    *cursor = array + 1;
    *end = limit;
    return 1;
}

static void normalize( const char *text, char *out, size_t size )
{
    size_t used = 0; int space = 1;
    while (*text && used + 1 < size)
    {
        unsigned char c = *text++;
        if (isalnum( c )) { out[used++] = tolower( c ); space = 0; }
        else if (!space && used + 1 < size) { out[used++] = ' '; space = 1; }
    }
    if (used && out[used - 1] == ' ') used--;
    out[used] = 0;
}

static int title_score( const char *wanted, const char *name )
{
    char actual[192], words[192], *word; int score = 0;
    normalize( name, actual, sizeof(actual) );
    if (!strcmp( wanted, actual )) return 100000;
    if (!strncmp( actual, wanted, strlen(wanted) ) || !strncmp( wanted, actual, strlen(actual) )) score += 10000;
    else if (strstr( actual, wanted ) || strstr( wanted, actual )) score += 6000;
    snprintf( words, sizeof(words), "%s", wanted );
    for (word = strtok( words, " " ); word; word = strtok( NULL, " " ))
    {
        const char *found = strstr( actual, word );
        if (found && (found == actual || found[-1] == ' ') &&
            (!found[strlen(word)] || found[strlen(word)] == ' ')) score += 500;
    }
    score -= abs( (int)strlen( actual ) - (int)strlen( wanted ) );
    return score;
}

static enum steamgriddb_result search_games( const char *key, const char *title, struct game *games, int max_games,
                                             int *count )
{
    char encoded[512], url[768], wanted[192]; struct buffer body; enum steamgriddb_result result;
    const char *cursor, *end, *start, *finish; int n = 0;
    url_encode( title, encoded, sizeof(encoded) );
    snprintf( url, sizeof(url), "https://www.steamgriddb.com/api/v2/search/autocomplete/%s", encoded );
    if ((result = http_get( url, key, &body )) != STEAMGRIDDB_OK) return result;
    normalize( title, wanted, sizeof(wanted) );
    if (!data_array( body.data, body.size, &cursor, &end ))
    { free( body.data ); return STEAMGRIDDB_INVALID_RESPONSE; }
    while (n < max_games && next_object( &cursor, end, &start, &finish ))
    {
        long id;
        if (!json_long( field( start, finish, "id" ), finish, &id ) || id <= 0 ||
            !json_string( field( start, finish, "name" ), finish, games[n].name, sizeof(games[n].name) )) continue;
        games[n].id = id; games[n].score = title_score( wanted, games[n].name ); n++;
    }
    free( body.data ); *count = n;
    return n ? STEAMGRIDDB_OK : STEAMGRIDDB_NOT_FOUND;
}

enum steamgriddb_result steamgriddb_search_games( const char *key, const char *title,
                                                  struct steamgriddb_game *games, int max_games,
                                                  int *count )
{
    struct game ranked[SGDB_GAMES];
    int found = 0, i, j;
    enum steamgriddb_result result;
    if (!games || !count || max_games <= 0) return STEAMGRIDDB_INVALID_RESPONSE;
    if ((result = search_games( key, title, ranked, SGDB_GAMES, &found )) != STEAMGRIDDB_OK) return result;
    for (i = 0; i < found; i++) for (j = i + 1; j < found; j++) if (ranked[j].score > ranked[i].score)
    { struct game swap = ranked[i]; ranked[i] = ranked[j]; ranked[j] = swap; }
    if (found > max_games) found = max_games;
    for (i = 0; i < found; i++)
    { games[i].id = ranked[i].id; snprintf( games[i].name, sizeof(games[i].name), "%s", ranked[i].name ); }
    *count = found;
    return STEAMGRIDDB_OK;
}

static int best_artwork( const char *key, long id, const char *kind, const char *dimensions,
                         double aspect, struct artwork *best )
{
    char url[512]; struct buffer body; enum steamgriddb_result result;
    const char *cursor, *end, *start, *finish; int n = 0; best->score = INT_MIN;
    snprintf( url, sizeof(url),
              "https://www.steamgriddb.com/api/v2/%s/game/%ld?dimensions=%s&types=static&mimes=image/png&order=score",
              kind, id, dimensions );
    if ((result = http_get( url, key, &body )) != STEAMGRIDDB_OK) return result;
    if (!data_array( body.data, body.size, &cursor, &end ))
    { free( body.data ); return STEAMGRIDDB_INVALID_RESPONSE; }
    while (n++ < SGDB_ART && next_object( &cursor, end, &start, &finish ))
    {
        struct artwork art; long width = 0, height = 0, reputation = 0;
        if (!json_string( field( start, finish, "url" ), finish, art.url, sizeof(art.url) )) continue;
        json_long( field( start, finish, "width" ), finish, &width );
        json_long( field( start, finish, "height" ), finish, &height );
        json_long( field( start, finish, "score" ), finish, &reputation );
        art.width = width; art.height = height;
        art.reputation = reputation;
        art.score = width > 0 && height > 0 ? (int)fmin( (double)width * height / 1000.0, 5000.0 ) -
                    (int)(fabs( (double)width / height - aspect ) * 10000.0) : INT_MIN;
        /* A community vote outweighs any resolution tie breaker. */
        art.score += reputation > INT_MAX / 100000 ? INT_MAX / 2 : (int)reputation * 100000;
        if (art.score > best->score) *best = art;
    }
    free( body.data );
    return best->score == INT_MIN ? STEAMGRIDDB_NOT_FOUND : STEAMGRIDDB_OK;
}

static enum steamgriddb_result download_image( const char *url, const char *output )
{
    struct buffer body; enum steamgriddb_result result; char temp[640], backup[640]; FILE *file; int had;
    if ((result = http_get( url, NULL, &body )) != STEAMGRIDDB_OK) return result;
    if (body.size < 64 || memcmp( body.data, "\x89PNG\r\n\x1a\n", 8 )) { free( body.data ); return STEAMGRIDDB_INVALID_RESPONSE; }
    if (snprintf( temp, sizeof(temp), "%s.tmp", output ) >= (int)sizeof(temp) ||
        snprintf( backup, sizeof(backup), "%s.bak", output ) >= (int)sizeof(backup)) { free( body.data ); return STEAMGRIDDB_IO_ERROR; }
    if (!(file = fopen( temp, "wb" ))) { free( body.data ); return STEAMGRIDDB_IO_ERROR; }
    {
        int ok = fwrite( body.data, 1, body.size, file ) == body.size;
        if (ok) ok = !fflush( file );
        if (fclose( file )) ok = 0;
        if (!ok) { remove( temp ); free( body.data ); return STEAMGRIDDB_IO_ERROR; }
    }
    free( body.data ); had = !rename( output, backup );
    if (rename( temp, output )) { if (had) rename( backup, output ); remove( temp ); return STEAMGRIDDB_IO_ERROR; }
    if (had) remove( backup );
    return STEAMGRIDDB_OK;
}

enum steamgriddb_result steamgriddb_square_pictures( const char *key, long game_id,
    struct steamgriddb_picture *pictures, int max, int *count )
{
    char url[512];
    struct buffer body;
    const char *cursor, *end, *start, *finish;
    enum steamgriddb_result result;
    if (!count || !pictures || max <= 0 || game_id <= 0) return STEAMGRIDDB_INVALID_RESPONSE;
    *count = 0;
    if (!key || !key[0]) return STEAMGRIDDB_NO_KEY;
    snprintf( url, sizeof(url), "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=512x512,1024x1024&types=static&mimes=image/png&nsfw=false&humor=false", game_id );
    if ((result = http_get( url, key, &body )) != STEAMGRIDDB_OK) return result;
    if (!data_array( body.data, body.size, &cursor, &end ))
    { free( body.data ); return STEAMGRIDDB_INVALID_RESPONSE; }
    while (*count < max && next_object( &cursor, end, &start, &finish ))
    {
        struct steamgriddb_picture picture = {0};
        if (!json_string( field( start, finish, "url" ), finish, picture.url, sizeof(picture.url) ) ||
            strncmp( picture.url, "https://", 8 )) continue;
        json_string( field( start, finish, "name" ), finish, picture.author, sizeof(picture.author) );
        pictures[(*count)++] = picture;
    }
    free( body.data );
    return *count ? STEAMGRIDDB_OK : STEAMGRIDDB_NOT_FOUND;
}

enum steamgriddb_result steamgriddb_picture_data( const char *url, unsigned char **data, size_t *size )
{
    struct buffer body;
    enum steamgriddb_result result;
    *data = NULL;
    *size = 0;
    if (!url || strncmp( url, "https://", 8 )) return STEAMGRIDDB_INVALID_RESPONSE;
    if ((result = http_get( url, NULL, &body )) != STEAMGRIDDB_OK) return result;
    if (body.size < 8 || memcmp( body.data, "\x89PNG\r\n\x1a\n", 8 ))
    { free( body.data ); return STEAMGRIDDB_INVALID_RESPONSE; }
    *data = body.data;
    *size = body.size;
    return STEAMGRIDDB_OK;
}

enum steamgriddb_result steamgriddb_download_game_bundle( const char *key, long game_id,
                                                          const char *square_path, const char *portrait_path,
                                                          const char *hero_path )
{
    struct artwork square, portrait, hero;
    enum steamgriddb_result result = STEAMGRIDDB_NOT_FOUND;
    if (!key || !key[0]) return STEAMGRIDDB_NO_KEY;
    result = best_artwork( key, game_id, "grids", "512x512,1024x1024", 1.0, &square );
    if (result != STEAMGRIDDB_OK) return result;
    result = best_artwork( key, game_id, "grids", "600x900,342x482,660x930", 2.0 / 3.0, &portrait );
    if (result != STEAMGRIDDB_OK) return result;
    result = best_artwork( key, game_id, "heroes", "1920x620,3840x1240,1600x650", 1920.0 / 620.0, &hero );
    if (result != STEAMGRIDDB_OK) return result;
    if ((result = download_image( square.url, square_path )) != STEAMGRIDDB_OK) return result;
    if ((result = download_image( portrait.url, portrait_path )) != STEAMGRIDDB_OK) return result;
    return download_image( hero.url, hero_path );
}

enum steamgriddb_result steamgriddb_download_bundle( const char *key, const char *title,
                                                     const char *square_path, const char *portrait_path,
                                                     const char *hero_path, char *matched,
                                                     size_t matched_size )
{
    struct steamgriddb_game games[5];
    int count = 0, i;
    enum steamgriddb_result result;
    if ((result = steamgriddb_search_games( key, title, games, 5, &count )) != STEAMGRIDDB_OK) return result;
    for (i = 0; i < count; i++)
    {
        result = steamgriddb_download_game_bundle( key, games[i].id, square_path, portrait_path, hero_path );
        if (result == STEAMGRIDDB_OK)
        { if (matched && matched_size) snprintf( matched, matched_size, "%s", games[i].name ); return result; }
        if (result == STEAMGRIDDB_NO_KEY || result == STEAMGRIDDB_NETWORK_ERROR) return result;
    }
    return result;
}

const char *steamgriddb_result_message( enum steamgriddb_result result )
{
    switch (result)
    {
    case STEAMGRIDDB_OK: return "Cover downloaded.";
    case STEAMGRIDDB_NO_KEY: return "The SteamGridDB API key is missing or was rejected.";
    case STEAMGRIDDB_NETWORK_ERROR: return "Could not connect to SteamGridDB.";
    case STEAMGRIDDB_NOT_FOUND: return "No matching square, portrait and hero artwork set was found.";
    case STEAMGRIDDB_INVALID_RESPONSE: return "SteamGridDB returned an invalid image.";
    case STEAMGRIDDB_IO_ERROR: return "The downloaded cover could not be saved.";
    }
    return "SteamGridDB failed.";
}
