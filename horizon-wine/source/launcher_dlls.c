#include "launcher_dlls.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "horizon_dlls.h"
#include "horizon_dlls_curl.h"
#include "horizon_dll_features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum dlls_job { JOB_NONE, JOB_CHECK, JOB_VERIFY, JOB_APPLY };

struct launcher_dlls
{
    struct ui *ui;
    char root[512];
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_atomic_t cancel, done;
    int job;
    /* Which repository: 0 Autorun's, 1 its fork (HORIZON_DLLS_SOURCE on the card). */
    int fork;
    /* The worker's while it runs, the launcher's between jobs. */
    struct horizon_dll_manifest remote, local;
    int have_remote, have_local, planned, notify;
    struct horizon_dlls_plan plan;
    enum horizon_dlls_result result;
    /* What the worker is doing, under the mutex. */
    char what[160];
    unsigned long long current, total;
    /* When the log last heard how the download goes, and how far it was. */
    Uint32 logged_at;
    unsigned long long logged_bytes;
};

#define FEATURE_COUNT (sizeof(horizon_dll_runtime_features) / sizeof(horizon_dll_runtime_features[0]))
/* Downloads at once, and the release's parts: the Switch's curl speaks HTTP/1.1
 * only, one request at a time on a connection. Sixteen had TLS handshakes fail
 * on hardware, and the card, not the network, is what holds eight back. */
#define DOWNLOADS 8
/* From how many files still to come the release is downloaded first. */
#define ARCHIVE_FILES 200

static int progress( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    struct launcher_dlls *d = opaque;
    Uint32 now = SDL_GetTicks();
    char line[320];
    int log = 0;

    SDL_LockMutex( d->mutex );
    if (what) snprintf( d->what, sizeof(d->what), "%s", what );
    d->current = current;
    d->total = total;
    /* Every few seconds, how fast it goes, so a download cut short still says. */
    if (d->job == JOB_APPLY && now - d->logged_at >= 5000 && current >= d->logged_bytes)
    {
        snprintf( line, sizeof(line), "[DLLS] %llu of %llu KB, %.0f KB/s over the last %.1f s; %s",
                  current >> 10, total >> 10, (current - d->logged_bytes) / 1024.0 * 1000 / (now - d->logged_at),
                  (now - d->logged_at) / 1000.0, d->what );
        d->logged_at = now;
        d->logged_bytes = current;
        log = 1;
    }
    SDL_UnlockMutex( d->mutex );
    if (log) wine_nx_runtime_trace( line );
    return SDL_AtomicGet( &d->cancel );
}

/* Which repository the card follows, and saying so on the card. */
static int read_source( const char *root )
{
    char path[640], word[16] = "";
    FILE *file;

    snprintf( path, sizeof(path), "%s/%s", root, HORIZON_DLLS_SOURCE );
    if (!(file = fopen( path, "r" ))) return 0;
    if (!fgets( word, sizeof(word), file )) word[0] = 0;
    fclose( file );
    return !strncmp( word, "fork", 4 );
}

static int write_source( const char *root, int fork )
{
    char path[640];
    FILE *file;
    int ok;

    snprintf( path, sizeof(path), "%s/%s", root, HORIZON_DLLS_SOURCE );
    if (!fork) { remove( path ); return 1; }
    if (!(file = fopen( path, "w" ))) return 0;
    ok = fputs( "fork\n", file ) >= 0;
    return !fclose( file ) && ok;
}

/* Whether a program can start on what the card holds, and if not, why. */
static int ready( struct launcher_dlls *d, char *why, size_t size )
{
    char ignored[160];

    return horizon_dlls_ready( d->root, horizon_dll_runtime_features, FEATURE_COUNT,
                               why ? why : ignored, why ? size : sizeof(ignored) );
}

static void set_what( struct launcher_dlls *d, const char *what )
{
    progress( d, what, 0, 0 );
}

/* The card's manifest, read again, and what it lacks of the repository's. */
static void replan( struct launcher_dlls *d, int verify )
{
    enum horizon_dlls_result result;

    horizon_dlls_free( &d->local );
    result = horizon_dlls_load( d->root, horizon_dll_runtime_features, FEATURE_COUNT, &d->local );
    d->have_local = result == HORIZON_DLLS_OK;
    if (!d->have_remote) return;
    set_what( d, verify ? "Reading the files on the card" : "Comparing with the card" );
    result = horizon_dlls_plan( d->root, &d->remote, d->have_local ? &d->local : NULL, verify, progress, d, &d->plan );
    d->planned = result == HORIZON_DLLS_OK;
    if (result != HORIZON_DLLS_OK) d->result = result;
}

static int worker( void *opaque )
{
    struct launcher_dlls *d = opaque;
    struct horizon_dlls_transport transports[DOWNLOADS] = {{0}};
    unsigned int connections = horizon_dlls_curl_open( transports, d->job == JOB_APPLY ? DOWNLOADS : 1 );
    struct horizon_dll_manifest fetched;
    SDL_Event event = {0};

    Uint32 started = SDL_GetTicks();

    d->result = HORIZON_DLLS_OK;
    if (!connections) d->result = HORIZON_DLLS_NETWORK;
    else if (d->job == JOB_APPLY)
    {
        /* A card that has little of it takes the whole set in one download,
         * the repository's release, and only then what that did not bring --
         * anything newer than the release -- one file at a time. */
        if (d->plan.pending >= ARCHIVE_FILES && d->plan.pending * 2 >= d->plan.files)
        {
            struct horizon_dlls_bundle_stats stats = {0};
            unsigned char *bundle;
            unsigned long long size;
            char line[400];

            /* All of it first, over every connection, a range each; then
             * unpacked from memory, each file to the card whole. */
            set_what( d, "Downloading the Windows DLLs" );
            d->logged_at = SDL_GetTicks();
            d->logged_bytes = 0;
            d->result = horizon_dlls_download( transports, connections,
                                               d->fork ? HORIZON_DLLS_FORK_BUNDLE_URL : HORIZON_DLLS_BUNDLE_URL, &bundle, &size,
                                               progress, d, &stats );
            snprintf( line, sizeof(line), "[DLLS] release of %s: %s; %llu KB over %u connections in %u ms (%.0f KB/s); %s",
                      d->fork ? "the fork" : "the repository", horizon_dlls_error( d->result ), stats.bytes >> 10,
                      stats.connections, stats.download_ms,
                      stats.download_ms ? (stats.bytes >> 10) * 1000.0 / stats.download_ms : 0,
                      horizon_dlls_curl_last( &transports[0] ) );
            wine_nx_runtime_trace( line );
            if (d->result == HORIZON_DLLS_OK)
            {
                set_what( d, "Unpacking the Windows DLLs" );
                d->result = horizon_dlls_apply_bundle( d->root, &d->remote, d->have_local ? &d->local : NULL,
                                                       bundle, size, progress, d, &stats );
                free( bundle );
                snprintf( line, sizeof(line), "[DLLS] unpacked: %s; %u files in %u ms, %u of it writing to the card; "
                          "%u left for single files", horizon_dlls_error( d->result ), stats.unpacked,
                          stats.unpack_ms, stats.write_ms, stats.skipped );
                wine_nx_runtime_trace( line );
            }
            replan( d, 0 );
            /* No release, or it could not be read: the files on their own. */
            if (d->result != HORIZON_DLLS_CANCELLED && d->result != HORIZON_DLLS_IO) d->result = HORIZON_DLLS_OK;
        }
        if (d->result == HORIZON_DLLS_OK && d->planned && d->plan.pending)
        {
            set_what( d, "Downloading the remaining files" );
            d->logged_at = SDL_GetTicks();
            d->logged_bytes = 0;
            d->result = horizon_dlls_apply( d->root, &d->remote, d->have_local ? &d->local : NULL, transports,
                                            connections, progress, d );
            {
                char line[400];

                snprintf( line, sizeof(line), "[DLLS] single files: %s; the last on the first connection: %s",
                          horizon_dlls_error( d->result ), horizon_dlls_curl_last( &transports[0] ) );
                wine_nx_runtime_trace( line );
            }
            replan( d, 0 );
        }
    }
    else
    {
        set_what( d, "Reading the DLL repository" );
        d->result = horizon_dlls_fetch_manifest( &transports[0],
                                                 d->fork ? HORIZON_DLLS_FORK_MANIFEST_URL : HORIZON_DLLS_MANIFEST_URL,
                                                 horizon_dll_runtime_features, FEATURE_COUNT, &fetched, progress, d );
        if (d->result == HORIZON_DLLS_OK)
        {
            horizon_dlls_free( &d->remote );
            d->remote = fetched;
            d->have_remote = 1;
        }
        /* Offline, the card is still worth reading: what it has, and what it
         * had of the repository the last time it was reached. */
        replan( d, d->job == JOB_VERIFY );
    }
    horizon_dlls_curl_close( transports, connections );
    {
        char line[400], why[160];

        snprintf( line, sizeof(line), "[DLLS] %s from %s in %u ms over %u connections: %s; %u files, %u to download (%llu KB), "
                  "%u need another runtime, %u in folders this build does not know; the card %s",
                  d->job == JOB_APPLY ? "download" : d->job == JOB_VERIFY ? "verify" : "check",
                  d->fork ? HORIZON_DLLS_FORK_OWNER "'s fork" : "the repository",
                  SDL_GetTicks() - started, connections, horizon_dlls_error( d->result ), d->have_remote ? d->remote.count : 0,
                  d->planned ? d->plan.pending : 0, d->planned ? d->plan.download_bytes >> 10 : 0,
                  d->planned ? d->plan.unsupported : 0, d->have_remote ? d->remote.skipped : 0,
                  ready( d, why, sizeof(why) ) ? "can start programs" : why );
        wine_nx_runtime_trace( line );
    }
    SDL_AtomicSet( &d->done, 1 );
    event.type = SDL_USEREVENT;
    SDL_PushEvent( &event );
    return 0;
}

static int start( struct launcher_dlls *d, int job )
{
    if (d->thread) return 0;
    d->job = job;
    d->current = d->total = 0;
    d->what[0] = 0;
    SDL_AtomicSet( &d->cancel, 0 );
    SDL_AtomicSet( &d->done, 0 );
    d->thread = SDL_CreateThreadWithStackSize( worker, "horizon-dlls", 256 * 1024, d );
    if (!d->thread) { d->job = JOB_NONE; d->result = HORIZON_DLLS_MEMORY; return 0; }
    return 1;
}

/* Done with the worker, if it is done. */
static int finish( struct launcher_dlls *d )
{
    if (!d->thread || !SDL_AtomicGet( &d->done )) return 0;
    SDL_WaitThread( d->thread, NULL );
    d->thread = NULL;
    d->job = JOB_NONE;
    return 1;
}

void launcher_dlls_tick( struct launcher_dlls *d )
{
    if (!d || !finish( d ) || !d->notify || d->ui->modal_depth) return;
    d->notify = 0;
    if (!ready( d, NULL, 0 ))
        ui_notice( d->ui, "Windows DLLs needed: Settings > System > Windows DLLs" );
    else if (d->planned && d->have_remote && d->plan.pending)
        ui_notice( d->ui, "Windows DLL updates available" );
}

struct launcher_dlls *launcher_dlls_create( struct ui *ui, const char *root )
{
    struct launcher_dlls *d = calloc( 1, sizeof(*d) );

    if (!d) return NULL;
    if (strlen( root ) >= sizeof(d->root) || !(d->mutex = SDL_CreateMutex())) { free( d ); return NULL; }
    d->ui = ui;
    snprintf( d->root, sizeof(d->root), "%s", root );
    /* No slash at the end: paths are joined with one. */
    while (strlen( d->root ) > 1 && d->root[strlen( d->root ) - 1] == '/') d->root[strlen( d->root ) - 1] = 0;
    d->notify = 1;
    d->fork = read_source( d->root );
    start( d, JOB_CHECK );
    return d;
}

void launcher_dlls_destroy( struct launcher_dlls *d )
{
    if (!d) return;
    SDL_AtomicSet( &d->cancel, 1 );
    if (d->thread) SDL_WaitThread( d->thread, NULL );
    horizon_dlls_free( &d->remote );
    horizon_dlls_free( &d->local );
    SDL_DestroyMutex( d->mutex );
    free( d );
}

static void draw_progress( struct launcher_dlls *d, const char *title )
{
    struct ui *ui = d->ui;
    const int w = 600, h = 230, margin = 34;
    const int x = (ui->width - w) / 2, y = (ui->height - h) / 2;
    const int track_x = x + margin, track_y = y + 126, track_w = w - 2 * margin, track_h = 12;
    struct ui_hint hint = { UI_B, SDL_AtomicGet( &d->cancel ) ? "Stopping..." : "Cancel" };
    unsigned long long current, total;
    char what[160], amount[96];

    SDL_LockMutex( d->mutex );
    snprintf( what, sizeof(what), "%s", d->what );
    current = d->current;
    total = d->total;
    SDL_UnlockMutex( d->mutex );

    if (ui->snapshot)
    {
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
    }
    else ui_background( ui );
    ui_rounded( ui, x + 6, y + 10, w, h, 22, (SDL_Color){ 0, 0, 0, 120 } );
    ui_rounded( ui, x, y, w, h, 22, (SDL_Color){ 22, 27, 30, 250 } );
    ui_rounded_texture( ui, ui_sheen( ui ), NULL, (SDL_Rect){ x, y, w, h / 3 }, 22, (SDL_Color){ 255, 255, 255, 14 } );
    ui_outline( ui, x, y, w, h, 22, 1, (SDL_Color){ 236, 240, 246, 120 } );
    ui_text_fit( ui, ui->normal, x + margin, y + 30, w - 2 * margin, title, ui->value, 0 );
    ui_text_fit( ui, ui->small, x + margin, y + 78, w - 2 * margin, what[0] ? what : "Please wait...", ui->text, 0 );
    ui_rounded( ui, track_x, track_y, track_w, track_h, track_h / 2, (SDL_Color){ 55, 62, 70, 255 } );
    if (total)
    {
        double ratio = current < total ? (double)current / total : 1.0;
        int fill = (int)(track_w * ratio);

        if (fill > 0 && fill < track_h) fill = track_h;
        if (fill) ui_rounded( ui, track_x, track_y, fill, track_h, track_h / 2, ui->selection );
        snprintf( amount, sizeof(amount), "%.0f%%   %.1f / %.1f MiB", ratio * 100.0, current / 1048576.0,
                  total / 1048576.0 );
        ui_text_right( ui, ui->small, x + w - margin, y + 150, amount, ui->dim );
    }
    else
    {
        int position = (SDL_GetTicks() / 6) % (track_w + 96) - 96;
        int left = position < 0 ? 0 : position, right = position + 96 > track_w ? track_w : position + 96;

        if (right > left) ui_rounded( ui, track_x + left, track_y, right - left, track_h, track_h / 2, ui->selection );
    }
    ui_hints_right( ui, &hint, 1, x + w - margin, y + h - 30 );
    ui->busy_until = SDL_GetTicks() + 50;
    ui_fade( ui );
    ui_present( ui );
}

/* A job with its progress over the screen, B to stop it; with JOB_NONE,
 * waits for the one already running. Returns its result. */
static enum horizon_dlls_result run( struct launcher_dlls *d, int job, const char *title )
{
    struct ui_input input;

    if (job != JOB_NONE && !d->thread && !start( d, job )) return d->result;
    ui_progress_begin( d->ui );
    while (d->thread && ui_begin_frame( d->ui ))
    {
        if (finish( d )) break;
        while (ui_poll( d->ui, &input ))
            if (input.button == UI_B && !SDL_AtomicGet( &d->cancel ))
            {
                ui_sound( d->ui, LAUNCHER_SOUND_BACK );
                SDL_AtomicSet( &d->cancel, 1 );
            }
        draw_progress( d, title );
        ui_wait( d->ui );
    }
    if (d->thread)  /* the launcher is closing */
    {
        SDL_AtomicSet( &d->cancel, 1 );
        SDL_WaitThread( d->thread, NULL );
        d->thread = NULL;
        d->job = JOB_NONE;
    }
    ui_progress_end( d->ui );
    return d->result;
}

static const char *size_text( unsigned long long bytes, char *buffer, size_t size )
{
    if (bytes >= 1024ull * 1048576) snprintf( buffer, size, "%.1f GB", bytes / (1024.0 * 1048576) );
    else snprintf( buffer, size, "%.0f MB", bytes < 1048576 && bytes ? 1.0 : bytes / 1048576.0 );
    return buffer;
}

static int update( struct launcher_dlls *d )
{
    char text[512], amount[32], unpacked[32];
    enum horizon_dlls_result result;

    if (!d->have_remote || !d->planned || !d->plan.pending) return 1;
    snprintf( text, sizeof(text),
              "%u files: %s to download from the DLL repository on GitHub, %s on the SD card. What is "
              "downloaded is kept if it stops part of the way, and the rest comes the next time.%s",
              d->plan.pending, size_text( d->plan.download_bytes, amount, sizeof(amount) ),
              size_text( d->plan.pending_bytes, unpacked, sizeof(unpacked) ),
              d->plan.unsupported ? "\n\nSome files need a newer Autorun and are left as they are." : "" );
    if (!ui_confirm( d->ui, ready( d, NULL, 0 ) ? "Update the Windows DLLs?" :
                     "Download the Windows DLLs?", text, "Download" ))
        return 0;
    result = run( d, JOB_APPLY, "Downloading Windows DLLs" );
    if (result != HORIZON_DLLS_OK && d->ui->running)
        ui_message( d->ui, "Windows DLLs", horizon_dlls_error( result ) );
    return result == HORIZON_DLLS_OK;
}

void launcher_dlls_open( struct launcher_dlls *d )
{
    struct ui *ui = d ? d->ui : NULL;
    struct ui_row rows[4 + HORIZON_DLLS_CATEGORIES];
    char context[96], titles[HORIZON_DLLS_CATEGORIES][48], amount[32];
    struct ui_list list = {0};
    enum ui_action action;
    unsigned int i, count;

    if (!d) return;
    d->notify = 0;
    if (d->thread) run( d, JOB_NONE, "Checking the DLL repository" );
    else if (!d->planned) run( d, JOB_CHECK, "Checking the DLL repository" );
    while (ui->running)
    {
        const struct horizon_dll_manifest *m = d->have_remote ? &d->remote : &d->local;
        int installed = ready( d, NULL, 0 ), offline = !d->have_remote;

        memset( rows, 0, sizeof(rows) );
        snprintf( rows[0].label, sizeof(rows[0].label), "%s", installed ? "Update" : "Download" );
        if (offline) snprintf( rows[0].value, sizeof(rows[0].value), "%s",
                               d->result == HORIZON_DLLS_NETWORK ? "Offline" : "Unavailable" );
        else if (d->plan.pending)
            snprintf( rows[0].value, sizeof(rows[0].value), "%u files, %s", d->plan.pending,
                      size_text( d->plan.download_bytes, amount, sizeof(amount) ) );
        else snprintf( rows[0].value, sizeof(rows[0].value), "Up to date" );
        rows[0].kind = UI_ROW_ACTION;
        rows[0].disabled = offline || !d->plan.pending;
        rows[0].value_tone = offline ? UI_VALUE_DANGER : d->plan.pending ? UI_VALUE_NORMAL : UI_VALUE_SUCCESS;
        rows[0].help = offline ? horizon_dlls_error( d->result ) :
                       "Downloads the files that are new or changed in the DLL repository.";
        snprintf( rows[1].label, sizeof(rows[1].label), "Check again" );
        rows[1].kind = UI_ROW_ACTION;
        rows[1].help = offline ? horizon_dlls_error( d->result ) : "Reads the DLL repository's manifest again.";
        snprintf( rows[2].label, sizeof(rows[2].label), "Verify files" );
        rows[2].kind = UI_ROW_ACTION;
        rows[2].disabled = offline;
        rows[2].help = "Reads every DLL on the card and downloads again any that is not what it should be. "
                       "Takes a few minutes.";
        snprintf( rows[3].label, sizeof(rows[3].label), "Source" );
        snprintf( rows[3].value, sizeof(rows[3].value), "%s", d->fork ? HORIZON_DLLS_FORK_OWNER "'s fork" : "Autorun" );
        rows[3].kind = UI_ROW_ACTION;
        rows[3].help = "Which DLL repository on GitHub the card follows: Autorun's, or " HORIZON_DLLS_FORK_OWNER
                       "'s fork of it with its own builds. Changing it downloads nothing by itself: Update "
                       "then replaces the files that differ, and changing back does the same the other way.";
        count = 4;
        for (i = 0; i < m->category_count && d->planned; i++)
        {
            const struct horizon_dlls_category_plan *c = &d->plan.categories[i];
            struct ui_row *row = &rows[count++];

            if (!c->files) { count--; continue; }
            snprintf( row->label, sizeof(row->label), "%s",
                      horizon_dlls_category_title( m->categories[i].key, titles[i], sizeof(titles[i]) ) );
            if (c->pending)
                snprintf( row->value, sizeof(row->value), "%u of %u to download", c->pending, c->files );
            else if (c->unsupported)
                snprintf( row->value, sizeof(row->value), "%u need a newer Autorun", c->unsupported );
            else snprintf( row->value, sizeof(row->value), "%u files, %s", c->files,
                           size_text( c->bytes, amount, sizeof(amount) ) );
            row->value_tone = c->pending || c->unsupported ? UI_VALUE_NORMAL : UI_VALUE_SUCCESS;
            row->kind = UI_ROW_INFO;
            row->help = m->categories[i].description[0] ? m->categories[i].description : NULL;
        }
        if (m->commit[0]) snprintf( context, sizeof(context), "Wine %s, %.8s", m->wine, m->commit );
        else context[0] = 0;
        action = ui_list_run( ui, &list, "Windows DLLs", context[0] ? context : NULL, rows, count, 0 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        if (action != UI_ACTION_CHOOSE) continue;
        switch (list.selection)
        {
        case 0: update( d ); break;
        case 1:
            if (run( d, JOB_CHECK, "Checking the DLL repository" ) != HORIZON_DLLS_OK && ui->running)
                ui_message( ui, "Windows DLLs", horizon_dlls_error( d->result ) );
            break;
        case 2:
            if (run( d, JOB_VERIFY, "Verifying the Windows DLLs" ) == HORIZON_DLLS_OK && ui->running)
            {
                if (d->plan.pending) update( d );
                else ui_message( ui, "Windows DLLs", "Every file on the card is what it should be." );
            }
            break;
        case 3:
            d->fork = !d->fork;
            if (!write_source( d->root, d->fork ) && ui->running)
                ui_message( ui, "Windows DLLs", "The choice could not be written to the card; it holds until "
                            "Autorun closes." );
            /* What the other repository had is not this one's. */
            horizon_dlls_free( &d->remote );
            memset( &d->remote, 0, sizeof(d->remote) );
            d->have_remote = d->planned = 0;
            if (run( d, JOB_CHECK, "Checking the DLL repository" ) != HORIZON_DLLS_OK && ui->running)
                ui_message( ui, "Windows DLLs", horizon_dlls_error( d->result ) );
            break;
        }
    }
}

int launcher_dlls_is_ready( struct launcher_dlls *d )
{
    return !d || ready( d, NULL, 0 );
}

/* The check and a plan, waiting for the one running in the background. */
static void settle( struct launcher_dlls *d )
{
    if (d->thread) run( d, JOB_NONE, "Checking the DLL repository" );
    if (!d->have_remote) run( d, JOB_CHECK, "Checking the DLL repository" );
}

int launcher_dlls_install( struct launcher_dlls *d )
{
    if (!d) return 1;
    settle( d );
    if (!d->have_remote)
    {
        if (d->ui->running) ui_message( d->ui, "Windows DLLs", horizon_dlls_error( d->result ) );
        return 0;
    }
    return update( d ) && ready( d, NULL, 0 );
}

int launcher_dlls_ready( struct launcher_dlls *d )
{
    char why[160], text[512];

    if (!d || ready( d, why, sizeof(why) )) return 1;
    snprintf( text, sizeof(text), "This game needs the Windows DLLs Autorun downloads from its DLL repository. %s\n\n"
              "Download them in Windows DLLs; the game starts when you come back with them there.", why );
    if (!ui_confirm( d->ui, "Windows DLLs needed", text, "Open Windows DLLs" )) return 0;
    launcher_dlls_open( d );
    return ready( d, NULL, 0 );
}

const char *launcher_dlls_describe( struct launcher_dlls *d, char *buffer, size_t size )
{
    char download[32], card[32];

    if (!d) return "Windows DLLs are not available in this build.";
    if (d->thread) return "Checking the DLL repository...";
    if (!d->have_remote || !d->planned)
    {
        snprintf( buffer, size, "%s%s", ready( d, NULL, 0 ) ? "The card has the Windows DLLs. " : "",
                  horizon_dlls_error( d->result ) );
        return buffer;
    }
    if (!d->plan.pending) return "The card has the Windows DLLs, up to date.";
    snprintf( buffer, size, "%u files: %s to download, %s on the SD card.", d->plan.pending,
              size_text( d->plan.download_bytes, download, sizeof(download) ),
              size_text( d->plan.pending_bytes, card, sizeof(card) ) );
    return buffer;
}

const char *launcher_dlls_status( struct launcher_dlls *d, char *buffer, size_t size, int *tone )
{
    *tone = UI_VALUE_NORMAL;
    if (!d) return "Unavailable";
    if (d->thread) return "Checking...";
    if (!ready( d, NULL, 0 )) { *tone = UI_VALUE_DANGER; return "Not installed"; }
    if (!d->have_remote || !d->planned) return "Installed";
    if (d->plan.pending)
    {
        snprintf( buffer, size, "%u updates", d->plan.pending );
        return buffer;
    }
    *tone = UI_VALUE_SUCCESS;
    return "Up to date";
}
