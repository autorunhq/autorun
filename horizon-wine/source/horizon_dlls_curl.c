/* The DLL repository over HTTPS: a few connections kept open for every file,
 * since an update is hundreds of small downloads from the same host. They share
 * one DNS lookup and TLS session, as sphaira's downloader does, so the second
 * connection does not pay for a handshake the first already made. */
#include "horizon_dlls_curl.h"
#include "game_fixes.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

struct horizon_dlls_share
{
    CURLSH *share;
    pthread_mutex_t locks[CURL_LOCK_DATA_LAST];
    unsigned int users;
};

struct horizon_dlls_curl
{
    CURL *curl;
    struct horizon_dlls_share *share;
    /* How the last download went, for the log. */
    char last[320];
};

static void share_lock( CURL *curl, curl_lock_data data, curl_lock_access access, void *opaque )
{
    struct horizon_dlls_share *s = opaque;

    (void)curl;
    (void)access;
    pthread_mutex_lock( &s->locks[data] );
}

static void share_unlock( CURL *curl, curl_lock_data data, void *opaque )
{
    struct horizon_dlls_share *s = opaque;

    (void)curl;
    pthread_mutex_unlock( &s->locks[data] );
}

static struct horizon_dlls_share *share_create( void )
{
    struct horizon_dlls_share *s = calloc( 1, sizeof(*s) );
    int i;

    if (!s || !(s->share = curl_share_init()))
    {
        free( s );
        return NULL;
    }
    for (i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init( &s->locks[i], NULL );
    curl_share_setopt( s->share, CURLSHOPT_LOCKFUNC, share_lock );
    curl_share_setopt( s->share, CURLSHOPT_UNLOCKFUNC, share_unlock );
    curl_share_setopt( s->share, CURLSHOPT_USERDATA, s );
    curl_share_setopt( s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS );
    curl_share_setopt( s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION );
    curl_share_setopt( s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT );
    return s;
}

static void share_release( struct horizon_dlls_share *s )
{
    int i;

    if (!s || --s->users) return;
    curl_share_cleanup( s->share );
    for (i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_destroy( &s->locks[i] );
    free( s );
}

struct transfer
{
    horizon_dlls_sink sink;
    void *context;
    horizon_dlls_progress progress;
    void *opaque;
    const char *url;
    int stopped, cancelled;
};

static size_t receive( void *data, size_t size, size_t count, void *opaque )
{
    struct transfer *t = opaque;

    if (count && size > (size_t)-1 / count) { t->stopped = 1; return 0; }
    if (!t->sink( t->context, data, size * count )) { t->stopped = 1; return 0; }
    return size * count;
}

static int report( void *opaque, curl_off_t total, curl_off_t current, curl_off_t upload_total,
                   curl_off_t upload_current )
{
    struct transfer *t = opaque;

    (void)upload_total;
    (void)upload_current;
    if (t->progress && t->progress( t->opaque, t->url, current > 0 ? (unsigned long long)current : 0,
                                    total > 0 ? (unsigned long long)total : 0 ))
        t->cancelled = 1;
    return t->cancelled;
}

/* Only the repository and its release, over HTTPS; the engine checks the rest
 * of the URL, and what comes back against the manifest. */
/* Only the two repositories these connections are for: the DLL repository
 * (its files and its release bundle) and the game fixes repository, whose
 * index and fix files launcher_fixes.c fetches through the same transport. */
static int allowed( const char *url )
{
    return !strncmp( url, HORIZON_DLLS_RAW, sizeof(HORIZON_DLLS_RAW) - 1 ) ||
           !strncmp( url, HORIZON_DLLS_RELEASE, sizeof(HORIZON_DLLS_RELEASE) - 1 ) ||
           !strncmp( url, GAME_FIXES_RAW, sizeof(GAME_FIXES_RAW) - 1 );
}

static enum horizon_dlls_result perform( struct horizon_dlls_curl *c, const char *url, const char *range,
        horizon_dlls_sink sink, void *context, horizon_dlls_progress progress, void *progress_opaque );

static enum horizon_dlls_result fetch( void *opaque, const char *url, horizon_dlls_sink sink, void *context,
                                       horizon_dlls_progress progress, void *progress_opaque )
{
    return perform( opaque, url, NULL, sink, context, progress, progress_opaque );
}

static enum horizon_dlls_result fetch_range( void *opaque, const char *url, unsigned long long offset,
                                             unsigned long long length, horizon_dlls_sink sink, void *context,
                                             horizon_dlls_progress progress, void *progress_opaque )
{
    char range[64];

    if (!length) return HORIZON_DLLS_OK;
    snprintf( range, sizeof(range), "%llu-%llu", offset, offset + length - 1 );
    return perform( opaque, url, range, sink, context, progress, progress_opaque );
}

/* The size of what url names, from a HEAD that follows its redirects. */
static enum horizon_dlls_result size_of( void *opaque, const char *url, unsigned long long *size )
{
    struct horizon_dlls_curl *c = opaque;
    curl_off_t length = -1;
    CURLcode code;
    long status = 0;

    if (!allowed( url )) return HORIZON_DLLS_INVALID;
    curl_easy_setopt( c->curl, CURLOPT_RANGE, NULL );
    if (curl_easy_setopt( c->curl, CURLOPT_URL, url ) || curl_easy_setopt( c->curl, CURLOPT_NOBODY, 1L ))
        return HORIZON_DLLS_NETWORK;
    code = curl_easy_perform( c->curl );
    curl_easy_getinfo( c->curl, CURLINFO_RESPONSE_CODE, &status );
    curl_easy_getinfo( c->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length );
    curl_easy_setopt( c->curl, CURLOPT_NOBODY, 0L );
    curl_easy_setopt( c->curl, CURLOPT_HTTPGET, 1L );
    if (code != CURLE_OK) return HORIZON_DLLS_NETWORK;
    if (status == 404) return HORIZON_DLLS_NOT_FOUND;
    if (status < 200 || status >= 300 || length <= 0) return HORIZON_DLLS_NETWORK;
    *size = (unsigned long long)length;
    return HORIZON_DLLS_OK;
}

static enum horizon_dlls_result perform( struct horizon_dlls_curl *c, const char *url, const char *range,
        horizon_dlls_sink sink, void *context, horizon_dlls_progress progress, void *progress_opaque )
{
    struct transfer t = { sink, context, progress, progress_opaque, url, 0, 0 };
    CURLcode code;
    long status = 0;

    if (!allowed( url )) return HORIZON_DLLS_INVALID;
    if (curl_easy_setopt( c->curl, CURLOPT_RANGE, range ) ||
        curl_easy_setopt( c->curl, CURLOPT_URL, url ) ||
        curl_easy_setopt( c->curl, CURLOPT_WRITEFUNCTION, receive ) ||
        curl_easy_setopt( c->curl, CURLOPT_WRITEDATA, &t ) ||
        curl_easy_setopt( c->curl, CURLOPT_XFERINFOFUNCTION, report ) ||
        curl_easy_setopt( c->curl, CURLOPT_XFERINFODATA, &t ))
        return HORIZON_DLLS_NETWORK;
    code = curl_easy_perform( c->curl );
    curl_easy_getinfo( c->curl, CURLINFO_RESPONSE_CODE, &status );
    {
        double bytes = 0, seconds = 0, connect = 0, tls = 0;
        const char *where = NULL, *host;

        curl_easy_getinfo( c->curl, CURLINFO_SIZE_DOWNLOAD, &bytes );
        curl_easy_getinfo( c->curl, CURLINFO_TOTAL_TIME, &seconds );
        curl_easy_getinfo( c->curl, CURLINFO_CONNECT_TIME, &connect );
        curl_easy_getinfo( c->curl, CURLINFO_APPCONNECT_TIME, &tls );
        curl_easy_getinfo( c->curl, CURLINFO_EFFECTIVE_URL, &where );
        /* The host alone: a release's URL after its redirect is signed. */
        host = where && strstr( where, "://" ) ? strstr( where, "://" ) + 3 : "?";
        snprintf( c->last, sizeof(c->last), "curl %d (%s), HTTP %ld, %.0f KB in %.1f s (%.0f KB/s; connect %.2f s, "
                  "TLS %.2f s) from %.*s", (int)code, curl_easy_strerror( code ), status, bytes / 1024, seconds,
                  seconds > 0 ? bytes / 1024 / seconds : 0, connect, tls, (int)strcspn( host, "/?" ), host );
    }
    if (t.cancelled) return HORIZON_DLLS_CANCELLED;
    if (t.stopped) return HORIZON_DLLS_IO;
    if (code != CURLE_OK) return HORIZON_DLLS_NETWORK;
    if (status == 404) return HORIZON_DLLS_NOT_FOUND;
    if (status < 200 || status >= 300) return HORIZON_DLLS_NETWORK;
    /* Asked for a part and sent the whole: the engine sees the bytes overflow. */
    if (range && status != 206) return HORIZON_DLLS_INVALID;
    return HORIZON_DLLS_OK;
}

static int open_one( struct horizon_dlls_transport *transport, struct horizon_dlls_share *share )
{
    struct horizon_dlls_curl *c;
    CURL *curl;

    if (!(curl = curl_easy_init())) return 0;
    if (curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_MAXREDIRS, 5L ) ||
        curl_easy_setopt( curl, CURLOPT_CONNECTTIMEOUT, 12L ) ||
        /* No whole-transfer limit: a large file on a slow connection is fine
         * as long as it keeps coming. */
        curl_easy_setopt( curl, CURLOPT_LOW_SPEED_LIMIT, 1024L ) ||
        curl_easy_setopt( curl, CURLOPT_LOW_SPEED_TIME, 30L ) ||
        curl_easy_setopt( curl, CURLOPT_NOSIGNAL, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_NOPROGRESS, 0L ) ||
        /* Fewer, larger reads, as sphaira has them. */
        curl_easy_setopt( curl, CURLOPT_BUFFERSIZE, 512L * 1024 ) ||
        curl_easy_setopt( curl, CURLOPT_TCP_NODELAY, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_SSL_VERIFYPEER, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_SSL_VERIFYHOST, 2L ) ||
        curl_easy_setopt( curl, CURLOPT_USERAGENT, "Autorun/DLLs" ) ||
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt( curl, CURLOPT_PROTOCOLS_STR, "https" ) ||
        curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS_STR, "https" ) ||
#else
        curl_easy_setopt( curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS ) ||
        curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS ) ||
#endif
        (share && curl_easy_setopt( curl, CURLOPT_SHARE, share->share )) ||
        !(c = calloc( 1, sizeof(*c) )))
    {
        curl_easy_cleanup( curl );
        return 0;
    }
    c->curl = curl;
    c->share = share;
    if (share) share->users++;
    transport->fetch = fetch;
    transport->size = size_of;
    transport->fetch_range = fetch_range;
    transport->opaque = c;
    return 1;
}

unsigned int horizon_dlls_curl_open( struct horizon_dlls_transport *transports, unsigned int count )
{
    struct horizon_dlls_share *share;
    unsigned int opened = 0;

    if (curl_global_init( CURL_GLOBAL_DEFAULT ) != CURLE_OK) return 0;
    /* Without a share each connection still works, only slower to start. */
    share = share_create();
    while (opened < count && open_one( &transports[opened], share )) opened++;
    if (share && !share->users)
    {
        share->users = 1;
        share_release( share );
    }
    return opened;
}

const char *horizon_dlls_curl_last( const struct horizon_dlls_transport *transport )
{
    const struct horizon_dlls_curl *c = transport->opaque;

    return c && c->last[0] ? c->last : "no download";
}

void horizon_dlls_curl_close( struct horizon_dlls_transport *transports, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        struct horizon_dlls_curl *c = transports[i].opaque;

        if (!c) continue;
        curl_easy_cleanup( c->curl );
        share_release( c->share );
        free( c );
        transports[i].opaque = NULL;
    }
}
