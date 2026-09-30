#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_artwork.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "launcher_image.h"

enum artwork_job_kind { ARTWORK_SEARCH, ARTWORK_LIST, ARTWORK_IMAGE, ARTWORK_SAVE, ARTWORK_BATCH };

struct artwork_job
{
    enum artwork_job_kind kind;
    SDL_atomic_t done, cancel, current, completed;
    const char *key, *query, *destination;
    long game_id;
    enum steamgriddb_kind artwork;
    struct steamgriddb_game games[STEAMGRIDDB_MAX_GAMES];
    struct steamgriddb_picture pictures[STEAMGRIDDB_MAX_PICTURES];
    int count, downloaded, failed;
    unsigned char *data;
    size_t size;
    struct launcher_icon image;
    struct launcher_artwork_entry *entries;
    enum steamgriddb_result result;
};

static int cancelled( void *data ) { return SDL_AtomicGet( &((struct artwork_job *)data)->cancel ); }

static enum steamgriddb_result read_picture( struct artwork_job *job, const char *url,
                                            const struct steamgriddb_request *request )
{
    enum steamgriddb_result result = steamgriddb_picture_data( url, &job->data, &job->size, request );
    if (result == STEAMGRIDDB_OK && (!launcher_image_decode( job->data, job->size, &job->image ) ||
                                   !launcher_icon_fit( &job->image, 560 )))
        result = STEAMGRIDDB_INVALID_RESPONSE;
    return result;
}

static void release_picture( struct artwork_job *job )
{
    free( job->data );
    job->data = NULL;
    job->size = 0;
    launcher_icon_free( &job->image );
}

static void download_batch( struct artwork_job *job, const struct steamgriddb_request *request )
{
    int i, kind, found;
    for (i = 0; i < job->count && !cancelled( job ); i++)
    {
        struct launcher_artwork_entry *entry = &job->entries[i];
        if (!entry->missing) continue;
        SDL_AtomicSet( &job->current, i );
        job->result = steamgriddb_search_games( job->key, entry->title, job->games, 1, &found, request );
        for (kind = 0; kind < 3 && !cancelled( job ); kind++)
        {
            enum steamgriddb_result result = job->result;
            if (!(entry->missing & (1u << kind))) continue;
            if (result == STEAMGRIDDB_OK)
                result = steamgriddb_pictures( job->key, job->games[0].id, kind, job->pictures, 1, &found, request );
            if (result == STEAMGRIDDB_OK) result = read_picture( job, job->pictures[0].url, request );
            if (result == STEAMGRIDDB_OK)
                result = cancelled( job ) ? STEAMGRIDDB_CANCELLED :
                         steamgriddb_save_picture( job->data, job->size, entry->paths[kind] );
            release_picture( job );
            if (result == STEAMGRIDDB_OK) { entry->downloaded |= 1u << kind; job->downloaded++; }
            else if (cancelled( job ) || result == STEAMGRIDDB_CANCELLED) break;
            else job->failed++;
            if (result == STEAMGRIDDB_NO_KEY || result == STEAMGRIDDB_IO_ERROR)
            { job->result = result; return; }
        }
        if (job->result == STEAMGRIDDB_NO_KEY) return;
        SDL_AtomicAdd( &job->completed, 1 );
    }
    job->result = cancelled( job ) ? STEAMGRIDDB_CANCELLED : STEAMGRIDDB_OK;
}

static int artwork_worker( void *data )
{
    struct artwork_job *job = data;
    const struct steamgriddb_request request = { cancelled, job };
    switch (job->kind)
    {
    case ARTWORK_SEARCH:
        job->result = steamgriddb_search_games( job->key, job->query, job->games,
                                              STEAMGRIDDB_MAX_GAMES, &job->count, &request );
        break;
    case ARTWORK_LIST:
        job->result = steamgriddb_pictures( job->key, job->game_id, job->artwork, job->pictures,
                                          STEAMGRIDDB_MAX_PICTURES, &job->count, &request );
        break;
    case ARTWORK_IMAGE:
        job->result = read_picture( job, job->query, &request );
        break;
    case ARTWORK_SAVE:
        job->result = cancelled( job ) ? STEAMGRIDDB_CANCELLED :
                      steamgriddb_save_picture( job->data, job->size, job->destination );
        break;
    case ARTWORK_BATCH:
        download_batch( job, &request );
        break;
    }
    SDL_AtomicSet( &job->done, 1 );
    return 0;
}

static int run_job( struct ui *ui, struct artwork_job *job, const char *title, const char *status )
{
    SDL_Thread *thread;
    unsigned int total = 0;
    int i;
    SDL_AtomicSet( &job->done, 0 );
    SDL_AtomicSet( &job->cancel, 0 );
    SDL_AtomicSet( &job->current, 0 );
    SDL_AtomicSet( &job->completed, 0 );
    if (job->kind == ARTWORK_BATCH)
        for (i = 0; i < job->count; i++) total += !!job->entries[i].missing;
    thread = SDL_CreateThreadWithStackSize( artwork_worker, "autorun-artwork", 512 * 1024, job );
    if (!thread)
    { job->result = STEAMGRIDDB_IO_ERROR; ui_message( ui, "Artwork", "Could not start the download." ); return 0; }
    ui_progress_begin( ui );
    while (!SDL_AtomicGet( &job->done ))
    {
        const char *text = job->kind == ARTWORK_BATCH ? job->entries[SDL_AtomicGet( &job->current )].title : status;
        if (ui_progress_update_cancellable( ui, title, text, SDL_AtomicGet( &job->completed ), total ))
            SDL_AtomicSet( &job->cancel, 1 );
        SDL_Delay( 16 );
    }
    SDL_WaitThread( thread, NULL );
    ui_progress_end( ui );
    if (job->result != STEAMGRIDDB_OK && job->result != STEAMGRIDDB_CANCELLED && ui->running)
        ui_message( ui, "Artwork", steamgriddb_result_message( job->result ) );
    return job->result == STEAMGRIDDB_OK && (job->kind == ARTWORK_SAVE || (ui->running && !cancelled( job )));
}

long launcher_artwork_search( struct ui *ui, const char *key, const char *title )
{
    struct artwork_job *job = calloc( 1, sizeof(*job) );
    char query[192];
    const char *names[STEAMGRIDDB_MAX_GAMES + 1];
    long id = 0;
    int i, chosen;
    if (!job) return 0;
    snprintf( query, sizeof(query), "%s", title );
    job->key = key;
    job->kind = ARTWORK_SEARCH;
    while (ui->running)
    {
        job->query = query;
        if (!run_job( ui, job, "Download artwork", "Searching SteamGridDB..." ))
        {
            if (job->result != STEAMGRIDDB_NOT_FOUND || !ui->running || cancelled( job ) ||
                !launcher_platform_prompt( "Search SteamGridDB", query, query, sizeof(query) ) || !query[0]) break;
            continue;
        }
        for (i = 0; i < job->count; i++) names[i] = job->games[i].name;
        names[job->count] = "Search another title";
        chosen = ui_menu( ui, "Choose game", names, job->count + 1, 0 );
        if (chosen < 0) break;
        if (chosen < job->count) { id = job->games[chosen].id; break; }
        if (!launcher_platform_prompt( "Search SteamGridDB", query, query, sizeof(query) ) || !query[0]) break;
    }
    free( job );
    return id;
}

static SDL_Texture *keep_background( struct ui *ui )
{
    SDL_Texture *background = NULL;
    if (!ui->screen) return NULL;
    background = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_TARGET, ui->width, ui->height );
    if (background)
    {
        SDL_SetRenderTarget( ui->renderer, background );
        SDL_RenderCopy( ui->renderer, ui->screen, NULL, NULL );
        SDL_SetRenderTarget( ui->renderer, ui->screen );
    }
    return background;
}

int launcher_artwork_pick( struct ui *ui, const char *key, long game_id, enum steamgriddb_kind kind,
                           const char *destination, struct launcher_icon *image )
{
    static const char *const titles[] = { "Choose icon", "Choose cover", "Choose background" };
    const struct ui_hint hints[] = { {UI_A, "Use image"}, {UI_B, "Back"} };
    struct artwork_job *job = calloc( 1, sizeof(*job) );
    SDL_Texture *background = NULL, *preview = NULL;
    SDL_Rect card = { (ui->width - 680) / 2, (ui->height - 490) / 2, 680, 490 };
    SDL_Rect picture = {0};
    struct ui_input input;
    int index = 0, loaded = -1, chosen = 0;
    char label[128];
    if (!job || kind < 0 || kind > STEAMGRIDDB_BACKGROUND) { free( job ); return 0; }
    job->key = key;
    job->game_id = game_id;
    job->artwork = kind;
    job->kind = ARTWORK_LIST;
    if (!run_job( ui, job, titles[kind], "Finding artwork..." )) goto done;
    background = keep_background( ui );
    ui->modal_depth++;
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        if (loaded != index)
        {
            release_picture( job );
            SDL_DestroyTexture( preview );
            preview = NULL;
            job->kind = ARTWORK_IMAGE;
            job->query = job->pictures[index].url;
            if (!run_job( ui, job, titles[kind], "Loading preview..." )) break;
            preview = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                         job->image.width, job->image.height );
            if (!preview || SDL_UpdateTexture( preview, NULL, job->image.data, job->image.width * 4 ))
            { ui_message( ui, "Artwork", "Could not create the preview." ); break; }
            SDL_SetTextureBlendMode( preview, SDL_BLENDMODE_BLEND );
            double scale = fmin( 560.0 / job->image.width, 306.0 / job->image.height );
            picture.w = job->image.width * scale;
            picture.h = job->image.height * scale;
            picture.x = card.x + (card.w - picture.w) / 2;
            picture.y = card.y + 76 + (306 - picture.h) / 2;
            loaded = index;
            ui_start_screen( ui );
        }
        while (ui_poll( ui, &input ))
        {
            int previous = index;
            if (input.button == UI_B) { ui_sound( ui, LAUNCHER_SOUND_BACK ); goto closed; }
            if (input.button == UI_A && loaded == index)
            { ui_sound( ui, LAUNCHER_SOUND_ACCEPT ); chosen = 1; goto closed; }
            if (input.button == UI_L || input.button == UI_LEFT || input.touch == UI_TOUCH_SWIPE_RIGHT)
                index = (index + job->count - 1) % job->count;
            if (input.button == UI_R || input.button == UI_RIGHT || input.touch == UI_TOUCH_SWIPE_LEFT)
                index = (index + 1) % job->count;
            if (index != previous) ui_sound( ui, LAUNCHER_SOUND_MOVE );
        }
        if (index != loaded) continue;
        if (background) SDL_RenderCopy( ui->renderer, background, NULL, NULL );
        else ui_background( ui );
        ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
        ui_rounded( ui, card.x + 6, card.y + 10, card.w, card.h, 24, (SDL_Color){ 0, 0, 0, 120 } );
        ui_rounded( ui, card.x, card.y, card.w, card.h, 24, (SDL_Color){ 22, 27, 30, 255 } );
        ui_outline( ui, card.x, card.y, card.w, card.h, 24, 1, (SDL_Color){ 236, 240, 246, 100 } );
        ui_text( ui, ui->normal, card.x + 28, card.y + 24, titles[kind], ui->value );
        ui_rounded_texture( ui, preview, NULL, picture, 12, (SDL_Color){ 255, 255, 255, 255 } );
        snprintf( label, sizeof(label), "%d / %d%s%s", loaded + 1, job->count,
                  job->pictures[loaded].author[0] ? "  ·  " : "", job->pictures[loaded].author );
        ui_text_fit( ui, ui->small, card.x + 28, card.y + 402, card.w - 56, label, ui->dim, 1 );
        ui_hints_right( ui, hints, 2, card.x + card.w - 28, card.y + card.h - 30 );
        ui_present( ui );
        ui_wait( ui );
    }
closed:
    ui->modal_depth--;
    if (chosen && destination)
    {
        job->kind = ARTWORK_SAVE;
        job->destination = destination;
        chosen = run_job( ui, job, titles[kind], "Saving artwork..." );
    }
    if (chosen && image)
    { *image = job->image; memset( &job->image, 0, sizeof(job->image) ); }
done:
    release_picture( job );
    free( job );
    SDL_DestroyTexture( preview );
    SDL_DestroyTexture( background );
    ui_start_screen( ui );
    return chosen;
}

void launcher_artwork_download_all( struct ui *ui, const char *key,
                                    struct launcher_artwork_entry *entries, int count )
{
    struct artwork_job *job = calloc( 1, sizeof(*job) );
    char summary[192];
    if (!job) { ui_message( ui, "Artwork", "Not enough memory to start the download." ); return; }
    job->kind = ARTWORK_BATCH;
    job->key = key;
    job->entries = entries;
    job->count = count;
    run_job( ui, job, "Download all covers", "Searching SteamGridDB..." );
    if (ui->running && (job->result == STEAMGRIDDB_OK || job->result == STEAMGRIDDB_CANCELLED))
    {
        snprintf( summary, sizeof(summary), "%d images downloaded%s%s", job->downloaded,
                  job->failed ? ", some artwork could not be found or downloaded." : ".",
                  job->result == STEAMGRIDDB_CANCELLED ? " Download cancelled." : "" );
        ui_message( ui, "Artwork", summary );
    }
    free( job );
}
