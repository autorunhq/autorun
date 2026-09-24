#include "launcher_update.h"
#include "launcher_ui.h"
#include "autorun_update.h"
#include "autorun_install.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WINE_NX_BUILD_VERSION
#include "autorun_version.h"
#else
#define AUTORUN_BUILD_TAG ""
#define AUTORUN_BUILD_EPOCH 0
#endif

struct launcher_update
{
    struct ui *ui;
    char root[512], installed[64], phase[80];
    int (*restart)(void);
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_atomic_t cancel, done, committing;
    struct autorun_release release;
    enum autorun_update_result network_result;
    enum autorun_install_result install_result;
    unsigned long long current, total;
    int job, ready, available, notify, completed, fatal, revision;
    const char *error;
};

struct note_line { unsigned int offset, length; };

static uint64_t published_time( const char *date )
{
    static const unsigned int days_before[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    unsigned int y, m, d, h, min, s;
    int end = 0;
    uint64_t days;
    unsigned int leap;

    if (strlen( date ) != 20 || sscanf( date, "%4u-%2u-%2uT%2u:%2u:%2uZ%n", &y, &m, &d, &h, &min, &s, &end ) != 6 ||
        end != 20 || y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31 || h > 23 || min > 59 || s > 59) return 0;
    leap = !(y % 4) && ((y % 100) || !(y % 400));
    if (d > (m == 2 ? 28 + leap : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31)) return 0;
    days = (uint64_t)(y - 1970) * 365 + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400 - 477;
    days += days_before[m - 1] + d - 1 + (m > 2 && leap);
    return ((days * 24 + h) * 60 + min) * 60 + s;
}

static int new_release( const struct launcher_update *u )
{
    uint64_t published = published_time( u->release.published );
    if (!strcmp( u->release.tag, AUTORUN_BUILD_TAG ) || !strcmp( u->release.tag, u->installed )) return 0;
    const uint64_t built = AUTORUN_BUILD_EPOCH;
    if (!built) return 0;
    return published > built;
}

static int download_progress( void *opaque, unsigned long long current, unsigned long long total )
{
    struct launcher_update *u = opaque;
    SDL_LockMutex( u->mutex );
    u->current = current;
    u->total = total;
    SDL_UnlockMutex( u->mutex );
    return SDL_AtomicGet( &u->cancel );
}

static int install_progress( void *opaque, const char *phase, unsigned long long current, unsigned long long total )
{
    struct launcher_update *u = opaque;
    if (!strncmp( phase, "Installing", 10 ) || !strncmp( phase, "Restoring", 9 )) SDL_AtomicSet( &u->committing, 1 );
    SDL_LockMutex( u->mutex );
    snprintf( u->phase, sizeof(u->phase), "%s", phase );
    u->current = current;
    u->total = total;
    SDL_UnlockMutex( u->mutex );
    return !SDL_AtomicGet( &u->committing ) && SDL_AtomicGet( &u->cancel );
}

static int update_worker( void *opaque )
{
    struct launcher_update *u = opaque;
    SDL_Event event = {0};
    if (u->job == 1)
    {
        autorun_installed_release( u->root, u->installed, sizeof(u->installed) );
        u->network_result = autorun_update_check( &u->release, download_progress, u );
    }
    else
    {
        char archive[768];
        int amd64 = 0;
#ifdef WINE_NX_AMD64
        amd64 = 1;
#endif
        u->network_result = autorun_update_download( u->root, &u->release, archive, sizeof(archive), download_progress, u );
        if (u->network_result == AUTORUN_UPDATE_OK)
            u->install_result = autorun_install_archive( u->root, archive, u->release.tag, amd64, install_progress, u );
    }
    SDL_AtomicSet( &u->done, 1 );
    event.type = SDL_USEREVENT;
    SDL_PushEvent( &event );
    return 0;
}

static void start_job( struct launcher_update *u, int job )
{
    if (u->thread) return;
    u->job = job;
    u->error = NULL;
    u->current = u->total = 0;
    if (job == 1) u->ready = u->available = 0;
    u->completed = 0;
    snprintf( u->phase, sizeof(u->phase), "%s", job == 1 ? "Checking GitHub releases" : "Downloading update" );
    SDL_AtomicSet( &u->cancel, 0 );
    SDL_AtomicSet( &u->done, 0 );
    SDL_AtomicSet( &u->committing, 0 );
    u->thread = SDL_CreateThreadWithStackSize( update_worker, "autorun-update", 1024 * 1024, u );
    if (!u->thread) { u->job = 0; u->error = "Could not start the update worker."; }
}

void launcher_update_tick( void *opaque )
{
    struct launcher_update *u = opaque;
    if (!u) return;
    if (u->thread && SDL_AtomicGet( &u->done ))
    {
        SDL_WaitThread( u->thread, NULL );
        u->thread = NULL;
        if (u->network_result != AUTORUN_UPDATE_OK) u->error = autorun_update_error( u->network_result );
        else if (u->job == 1)
        {
            u->ready = 1;
            u->available = new_release( u );
            u->revision++;
        }
        else if (u->install_result != AUTORUN_INSTALL_OK)
        {
            u->error = autorun_install_error( u->install_result );
            u->fatal = u->install_result == AUTORUN_INSTALL_RECOVERY;
        }
        else u->completed = 1;
        u->job = 0;
        SDL_AtomicSet( &u->committing, 0 );
    }
    if (u->notify && u->ready && !u->ui->modal_depth)
    {
        if (u->available) ui_notice( u->ui, "New update available" );
        u->notify = 0;
    }
}

struct launcher_update *launcher_update_create( struct ui *ui, const char *root, int (*restart)(void) )
{
    struct launcher_update *u = calloc( 1, sizeof(*u) );
    if (!u) return NULL;
    if (strlen( root ) >= sizeof(u->root) || !(u->mutex = SDL_CreateMutex())) { free( u ); return NULL; }
    u->ui = ui;
    u->restart = restart;
    strcpy( u->root, root );
    u->notify = 1;
    start_job( u, 1 );
    return u;
}

void launcher_update_destroy( struct launcher_update *u )
{
    if (!u) return;
    SDL_AtomicSet( &u->cancel, 1 );
    if (u->thread) SDL_WaitThread( u->thread, NULL );
    SDL_DestroyMutex( u->mutex );
    free( u );
}

static int note_lines( struct ui *ui, const char *text, struct note_line *lines, int max )
{
    unsigned int offset = 0;
    int count = 0;
    while (text[offset] && count < max)
    {
        char line[192];
        size_t len = strcspn( text + offset, "\n" ), bytes = 0;
        int extent, chars;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        while (len && ((unsigned char)text[offset + len] & 0xc0) == 0x80) len--;
        memcpy( line, text + offset, len ); line[len] = 0;
        if (TTF_MeasureUTF8( ui->small, line, 568, &extent, &chars )) chars = 1;
        while (bytes < len && chars-- > 0)
        {
            bytes++;
            while (bytes < len && ((unsigned char)line[bytes] & 0xc0) == 0x80) bytes++;
        }
        if (bytes < len)
        {
            size_t space = bytes;
            while (space && line[space] != ' ') space--;
            if (space) bytes = space;
        }
        if (!bytes && len) { bytes = 1; while (bytes < len && ((unsigned char)line[bytes] & 0xc0) == 0x80) bytes++; }
        lines[count++] = (struct note_line){offset, bytes};
        offset += bytes;
        while (text[offset] == ' ' || text[offset] == '\r') offset++;
        if (text[offset] == '\n') offset++;
    }
    return count;
}

static void draw_update( struct launcher_update *u, const struct note_line *lines, int count, float scroll )
{
    struct ui *ui = u->ui;
    const int w = u->job ? 540 : 640, h = u->job ? 230 : 432;
    const int x = (ui->width - w) / 2, y = (ui->height - h) / 2;
    const int line_height = TTF_FontHeight( ui->small ) + 8;
    char status[256], phase[80];
    unsigned long long current, total;
    struct ui_hint hints[4];
    int hint_count = 0, i;
    SDL_Rect clip = {x + 28, y + 108, w - 68, 244 / line_height * line_height};

    if (ui->snapshot)
    {
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){4,7,11,205} );
    }
    else ui_background( ui );
    ui_rounded( ui, x + 6, y + 10, w, h, 22, (SDL_Color){0,0,0,120} );
    ui_rounded( ui, x, y, w, h, 22, (SDL_Color){22,27,30,250} );
    ui_rounded_texture( ui, ui_sheen( ui ), NULL, (SDL_Rect){x,y,w,h / 3}, 22, (SDL_Color){255,255,255,14} );
    ui_outline( ui, x, y, w, h, 22, 1, (SDL_Color){236,240,246,120} );
    const char *title = u->job == 1 ? "Checking for updates" : u->job == 2 ? "Autorun update" :
        u->error ? "Couldn't update Autorun" : u->available ? "New update available" : "Autorun updates";
    ui_text( ui, ui->normal, x + 28, y + 24, title, ui->value );
    if (!u->job && u->ready && !u->error)
    {
        ui_text( ui, ui->small, x + 28, y + 66, "What's new", ui->dim );
        ui_text_right( ui, ui->small, x + w - 28, y + 66, u->release.tag, ui->dim );
    }
    if (!u->job) ui_fill( ui, x + 28, y + 96, w - 56, 1, (SDL_Color){68,76,82,160} );
    if (u->job)
    {
        SDL_LockMutex( u->mutex );
        current = u->current; total = u->total;
        snprintf( phase, sizeof(phase), "%s", u->phase );
        SDL_UnlockMutex( u->mutex );
        if (u->job == 2) ui_text_fit( ui, ui->small, x + 28, y + 78, w - 56, phase, ui->dim, 0 );
        const int track_w = w - 56, track_x = x + 28, track_y = y + 120;
        ui_rounded( ui, track_x, track_y, track_w, 12, 6, (SDL_Color){55,62,70,255} );
        if (total)
        {
            double ratio = current < total ? (double)current / total : 1.0;
            int fill = (int)(track_w * ratio);
            if (fill) ui_rounded( ui, track_x, track_y, fill < 12 ? 12 : fill, 12, 6, ui->selection );
            snprintf( status, sizeof(status), "%.0f%%   %.1f / %.1f MiB", ratio * 100, current / 1048576.0, total / 1048576.0 );
        }
        else
        {
            int pos = (SDL_GetTicks() / 6) % (track_w + 96) - 96;
            int left = pos < 0 ? 0 : pos, right = pos + 96 > track_w ? track_w : pos + 96;
            if (right > left) ui_rounded( ui, track_x + left, track_y, right - left, 12, 6, ui->selection );
            snprintf( status, sizeof(status), "Please wait..." );
        }
        if (total) ui_text_right( ui, ui->small, x + w - 28, y + 146, status, ui->dim );
        ui->busy_until = SDL_GetTicks() + 50;
    }
    else if (u->error)
        ui_text_wrapped( ui, ui->small, x + 28, y + 124, w - 56, 7, u->error, ui->text, 0 );
    else
    {
        SDL_RenderSetClipRect( ui->renderer, &clip );
        for (i = (int)scroll / line_height; i < count && i * line_height - scroll < clip.h; i++)
        {
            char line[192];
            memcpy( line, u->release.notes + lines[i].offset, lines[i].length ); line[lines[i].length] = 0;
            for (char *p = line; *p; p++) if (*p == '\r' || *p == '\t') *p = ' ';
            ui_text( ui, ui->small, clip.x, clip.y + i * line_height - (int)scroll, line, ui->text );
        }
        if (!count) ui_text( ui, ui->small, clip.x, clip.y, "No release notes were provided.", ui->dim );
        SDL_RenderSetClipRect( ui->renderer, NULL );
        if (count * line_height > clip.h)
        {
            int thumb = clip.h * clip.h / (count * line_height);
            if (thumb < 24) thumb = 24;
            int offset = (int)(scroll * (clip.h - thumb) / (count * line_height - clip.h));
            ui_rounded( ui, x + w - 23, clip.y + offset, 4, thumb, 2, ui->dim );
        }
    }
    if (!u->job) ui_fill( ui, x + 28, y + h - 62, w - 56, 1, (SDL_Color){68,76,82,160} );
    if (!u->job && !u->error && u->available) hints[hint_count++] = (struct ui_hint){UI_A, "Update"};
    if (!u->job && !u->fatal) hints[hint_count++] = (struct ui_hint){UI_X, u->error ? "Retry" : "Refresh"};
    if (!SDL_AtomicGet( &u->committing )) hints[hint_count++] = (struct ui_hint){UI_B, u->job == 2 ? "Cancel" : "Close"};
    ui_hints_right( ui, hints, hint_count, x + w - 28, y + h - 30 );
    ui_fade( ui );
    ui_present( ui );
}

void launcher_update_open( struct launcher_update *u )
{
    struct ui_input input;
    struct note_line *lines;
    int count = 0, revision = -1, top = 0;
    float scroll = 0;
    if (!u) return;
    if (!(lines = calloc( sizeof(u->release.notes), sizeof(*lines) ))) return;
    u->notify = 0;
    u->ui->toast[0] = 0;
    if (!u->thread) start_job( u, 1 );
    ui_progress_begin( u->ui );
    while (ui_begin_frame( u->ui ))
    {
        launcher_update_tick( u );
        if (u->completed) break;
        if (u->ready && revision != u->revision)
        {
            count = note_lines( u->ui, u->release.notes, lines, sizeof(u->release.notes) );
            revision = u->revision;
            top = 0; scroll = 0;
        }
        while (ui_poll( u->ui, &input ))
        {
            if (input.button == UI_B)
            {
                if (u->job == 2)
                {
                    if (!SDL_AtomicGet( &u->committing )) { ui_sound( u->ui, LAUNCHER_SOUND_BACK ); SDL_AtomicSet( &u->cancel, 1 ); }
                }
                else { ui_sound( u->ui, LAUNCHER_SOUND_BACK ); goto done; }
            }
            if (input.button == UI_X && !u->job && !u->fatal) { ui_sound( u->ui, LAUNCHER_SOUND_ACCEPT ); start_job( u, 1 ); }
            if (input.button == UI_A && !u->job && !u->error && u->available)
            {
                ui_sound( u->ui, LAUNCHER_SOUND_ACCEPT );
                ui_progress_end( u->ui );
                if (!u->restart) ui_message( u->ui, "Restart unavailable",
                    "Start Autorun through its forwarder or the Homebrew Menu to install an update." );
                else if (ui_confirm( u->ui, "Install Autorun update?",
                    "Install this release and restart? Your games and settings will be kept.\n\n"
                    "Keep the console powered on during installation.", "Install & restart" )) start_job( u, 2 );
                ui_progress_begin( u->ui );
            }
            if (input.button == UI_UP) top -= 1;
            if (input.button == UI_DOWN) top += 1;
            if (input.touch == UI_TOUCH_SCROLL_UP) top += input.steps;
            if (input.touch == UI_TOUCH_SCROLL_DOWN) top -= input.steps;
        }
        int line_height = TTF_FontHeight( u->ui->small ) + 8;
        int max_scroll = count * line_height - 244 / line_height * line_height;
        if (max_scroll < 0) max_scroll = 0;
        if (top < 0) top = 0;
        if (top * line_height > max_scroll) top = (max_scroll + line_height - 1) / line_height;
        float target = top * line_height > max_scroll ? max_scroll : top * line_height;
        scroll += (target - scroll) * 0.3f;
        if (scroll - target > 0.2f || target - scroll > 0.2f) u->ui->busy_until = SDL_GetTicks() + 50;
        else scroll = target;
        draw_update( u, lines, count, scroll );
        ui_wait( u->ui );
    }
done:
    ui_progress_end( u->ui );
    free( lines );
    if (u->completed)
    {
        if (!u->restart || !u->restart()) ui_message( u->ui, "Update installed", "Close and reopen Autorun to use the new version." );
        u->ui->running = 0;
    }
    else if (u->fatal) u->ui->running = 0;
}
