#include "launcher_forwarder.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "launcher_image.h"
#include "steamgriddb.h"
#include "forwarder.h"
#include "forwarder_launch.h"

enum job_kind { JOB_INITIAL, JOB_FILE, JOB_SEARCH, JOB_PICTURES, JOB_DOWNLOAD, JOB_INSTALL };

struct forwarder_job
{
    enum job_kind kind;
    SDL_atomic_t done;
    const struct wine_nx_launcher_options *options;
    const struct launcher_forwarder_game *game;
    struct wine_nx_forwarder request;
    struct launcher_icon image;
    struct steamgriddb_game games[6];
    struct steamgriddb_picture pictures[STEAMGRIDDB_MAX_PICTURES];
    const char *key, *path;
    long game_id;
    int count, ok;
    char error[256];
};

static int forwarder_worker( void *opaque )
{
    struct forwarder_job *job = opaque;
    enum steamgriddb_result result = STEAMGRIDDB_OK;
    unsigned char *data = NULL;
    size_t size = 0;
    int image_job = 0;
    job->ok = 1;
    switch (job->kind)
    {
    case JOB_INITIAL:
        if (!launcher_image_load( job->game->artwork, &job->image ))
        {
            launcher_pe_describe( job->game->path, 256, &job->image, NULL, 0 );
            if (job->image.kind == LAUNCHER_ICON_PNG)
            {
                struct launcher_icon decoded = {0};
                launcher_image_decode( job->image.data, job->image.size, &decoded );
                launcher_icon_free( &job->image );
                job->image = decoded;
            }
            if (job->image.kind != LAUNCHER_ICON_RGBA)
                launcher_image_decode( wine_nx_icon_any, wine_nx_icon_any_size, &job->image );
        }
        image_job = 1;
        break;
    case JOB_FILE:
        job->ok = launcher_image_load( job->path, &job->image );
        image_job = 1;
        break;
    case JOB_SEARCH:
        result = steamgriddb_search_games( job->key, job->path, job->games, 6, &job->count );
        break;
    case JOB_PICTURES:
        result = steamgriddb_square_pictures( job->key, job->game_id, job->pictures, STEAMGRIDDB_MAX_PICTURES, &job->count );
        break;
    case JOB_DOWNLOAD:
        result = steamgriddb_picture_data( job->path, &data, &size );
        job->ok = result == STEAMGRIDDB_OK && launcher_image_decode( data, size, &job->image );
        free( data );
        image_job = 1;
        break;
    case JOB_INSTALL:
    {
        const char *step = NULL;
        unsigned int rc = job->options->install_game_forwarder( &job->request, &step );
        job->ok = !rc;
        if (rc) snprintf( job->error, sizeof(job->error), "Could not install while %s (0x%X).",
                          step ? step : "writing the forwarder", rc );
        break;
    }
    }
    if (image_job && job->ok) job->ok = launcher_image_square( &job->image );
    if (image_job && !job->ok) snprintf( job->error, sizeof(job->error), "Use a PNG or JPEG up to 2048 x 2048 pixels and 16 MiB." );
    if (result != STEAMGRIDDB_OK)
    {
        job->ok = 0;
        snprintf( job->error, sizeof(job->error), "%s", result == STEAMGRIDDB_NOT_FOUND ?
                  "No matching games or square artwork found." : steamgriddb_result_message( result ) );
    }
    SDL_AtomicSet( &job->done, 1 );
    return 0;
}

static int run_job( struct ui *ui, struct forwarder_job *job, const char *status )
{
    SDL_Thread *thread;
    job->error[0] = 0;
    SDL_AtomicSet( &job->done, 0 );
    thread = SDL_CreateThreadWithStackSize( forwarder_worker, "autorun-forwarder", 512 * 1024, job );
    if (!thread) { ui_message( ui, "Forwarder", "Could not start the worker." ); return 0; }
    ui_progress_begin( ui );
    while (!SDL_AtomicGet( &job->done ))
    {
        ui_progress_update( ui, "Game forwarder", status, 0, 0 );
        SDL_Delay( 16 );
    }
    SDL_WaitThread( thread, NULL );
    ui_progress_end( ui );
    if (!job->ok && ui->running) ui_message( ui, "Forwarder", job->error );
    return job->ok && ui->running;
}

static SDL_Texture *image_texture( struct ui *ui, const struct launcher_icon *image )
{
    SDL_Texture *texture = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 256, 256 );
    if (texture && SDL_UpdateTexture( texture, NULL, image->data, 256 * 4 ))
    { SDL_DestroyTexture( texture ); texture = NULL; }
    return texture;
}

static SDL_Texture *keep_background( struct ui *ui )
{
    SDL_Texture *background = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET, ui->width, ui->height );
    if (background && ui->screen)
    {
        SDL_SetRenderTarget( ui->renderer, background );
        SDL_RenderCopy( ui->renderer, ui->screen, NULL, NULL );
        SDL_SetRenderTarget( ui->renderer, ui->screen );
    }
    else { SDL_DestroyTexture( background ); background = NULL; }
    return background;
}

static void panel( struct ui *ui, SDL_Texture *background, SDL_Rect rect, const char *title )
{
    if (background) SDL_RenderCopy( ui->renderer, background, NULL, NULL );
    else ui_background( ui );
    ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
    ui_rounded( ui, rect.x + 6, rect.y + 10, rect.w, rect.h, 24, (SDL_Color){ 0, 0, 0, 120 } );
    ui_rounded( ui, rect.x, rect.y, rect.w, rect.h, 24, (SDL_Color){ 22, 27, 30, 255 } );
    ui_outline( ui, rect.x, rect.y, rect.w, rect.h, 24, 1, (SDL_Color){ 236, 240, 246, 100 } );
    ui_text( ui, ui->normal, rect.x + 28, rect.y + 24, title, ui->value );
}

static int choose_picture( struct ui *ui, struct forwarder_job *job )
{
    const struct ui_hint hints[] = { {UI_L, "Previous"}, {UI_R, "Next"}, {UI_A, "Use icon"}, {UI_B, "Back"} };
    SDL_Texture *background = keep_background( ui ), *preview = NULL;
    SDL_Rect rect = { (ui->width - 640) / 2, (ui->height - 448) / 2, 640, 448 };
    struct ui_input input;
    struct launcher_icon images[STEAMGRIDDB_MAX_PICTURES] = {0};
    int index = 0, loaded = -1, chosen = 0;
    char label[128];

    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        if (loaded != index)
        {
            if (!images[index].data)
            {
                job->kind = JOB_DOWNLOAD;
                job->path = job->pictures[index].url;
                if (!run_job( ui, job, "Downloading icon..." )) break;
                images[index] = job->image;
                memset( &job->image, 0, sizeof(job->image) );
            }
            SDL_DestroyTexture( preview );
            preview = image_texture( ui, &images[index] );
            if (!preview) { ui_message( ui, "Icon", "Could not create the icon preview." ); break; }
            loaded = index;
            ui_start_screen( ui );
        }
        while (ui_poll( ui, &input ))
        {
            int previous = index;
            if (input.button == UI_B) { ui_sound( ui, LAUNCHER_SOUND_BACK ); goto done; }
            if (input.button == UI_A && loaded == index) { ui_sound( ui, LAUNCHER_SOUND_ACCEPT ); chosen = 1; goto done; }
            if (input.button == UI_L || input.button == UI_LEFT) index = (index + job->count - 1) % job->count;
            if (input.button == UI_R || input.button == UI_RIGHT) index = (index + 1) % job->count;
            if (index != previous) ui_sound( ui, LAUNCHER_SOUND_MOVE );
        }
        panel( ui, background, rect, "SteamGridDB icon" );
        ui_rounded_texture( ui, preview, NULL, (SDL_Rect){ rect.x + 192, rect.y + 70, 256, 256 },
                            16, (SDL_Color){ 255, 255, 255, 255 } );
        snprintf( label, sizeof(label), "%d / %d%s%s", loaded + 1, job->count,
                  job->pictures[loaded].author[0] ? "  ·  " : "", job->pictures[loaded].author );
        ui_text_fit( ui, ui->small, rect.x + 28, rect.y + 345, rect.w - 56, label, ui->dim, 1 );
        ui_hints_right( ui, hints, 4, rect.x + rect.w - 28, rect.y + rect.h - 30 );
        ui_present( ui );
        ui_wait( ui );
    }
done:
    if (chosen)
    {
        job->image = images[loaded];
        memset( &images[loaded], 0, sizeof(images[loaded]) );
    }
    for (int i = 0; i < STEAMGRIDDB_MAX_PICTURES; i++) launcher_icon_free( &images[i] );
    SDL_DestroyTexture( preview );
    SDL_DestroyTexture( background );
    return chosen;
}

static int steam_icon( struct ui *ui, struct forwarder_job *job, char *key, size_t key_size, const char *name )
{
    const char *names[6];
    char edited[256], query[128];
    int i, chosen;
    if (!key[0])
    {
        if (!launcher_platform_prompt( "SteamGridDB API key", "", edited,
                                       key_size < sizeof(edited) ? key_size : sizeof(edited) ) || !edited[0]) return 0;
        snprintf( key, key_size, "%s", edited );
    }
    if (!launcher_platform_prompt( "Search SteamGridDB", name, query, sizeof(query) ) || !query[0]) return 0;
    job->kind = JOB_SEARCH;
    job->key = key;
    job->path = query;
    if (!run_job( ui, job, "Searching SteamGridDB..." )) return 0;
    for (i = 0; i < job->count; i++) names[i] = job->games[i].name;
    chosen = job->count == 1 ? 0 : ui_menu( ui, "Choose game", names, job->count, 0 );
    if (chosen < 0) return 0;
    job->game_id = job->games[chosen].id;
    job->kind = JOB_PICTURES;
    if (!run_job( ui, job, "Finding square artwork..." )) return 0;
    return choose_picture( ui, job );
}

void launcher_forwarder_run( struct ui *ui, const struct wine_nx_launcher_options *options,
    const struct launcher_forwarder_game *game, char *api_key, size_t key_size,
    int (*pick_image)( void *, char *, size_t ), void *picker_data )
{
    const struct ui_hint hints[] = { { UI_A, "Select" }, { UI_B, "Close" } };
    struct forwarder_job job = { .options = options, .game = game, .kind = JOB_INITIAL };
    struct launcher_icon icon = {0};
    struct ui_input input;
    SDL_Texture *background = keep_background( ui ), *preview = NULL;
    SDL_Rect card = { (ui->width - 760) / 2, (ui->height - 428) / 2, 760, 428 };
    SDL_Rect rows[4];
    char name[128], author[128] = "Autorun", path[512], edited[128];
    int selected = 1, right = 1, i, updated;

    if (!game->id || !options->install_game_forwarder) goto done;
    snprintf( name, sizeof(name), "%s", game->name );
    if (!run_job( ui, &job, "Preparing icon..." )) goto done;
    icon = job.image;
    memset( &job.image, 0, sizeof(job.image) );
    preview = image_texture( ui, &icon );
    if (!preview) { ui_message( ui, "Forwarder", "Could not create the icon preview." ); goto done; }
    rows[0] = (SDL_Rect){ card.x + 30, card.y + 86, 224, 224 };
    for (i = 1; i < 4; i++)
        rows[i] = (SDL_Rect){ card.x + 286, card.y + 86 + (i - 1) * 80, 446, i < 3 ? 68 : 58 };
    ui->modal_depth++;
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        int action = 0;
        while (ui_poll( ui, &input ))
        {
            int previous = selected;
            if (input.button == UI_B) { ui_sound( ui, LAUNCHER_SOUND_BACK ); goto closed; }
            if (input.button == UI_LEFT && selected) { right = selected; selected = 0; }
            if (input.button == UI_RIGHT && !selected) selected = right;
            if (input.button == UI_UP && selected > 1) selected--;
            if (input.button == UI_DOWN && selected && selected < 3) selected++;
            if (selected != previous) ui_sound( ui, LAUNCHER_SOUND_MOVE );
            if (input.button == UI_A) action = 1;
            if (input.touch == UI_TOUCH_TAP)
                for (i = 0; i < 4; i++)
                    if (input.x >= rows[i].x && input.x < rows[i].x + rows[i].w &&
                        input.y >= rows[i].y && input.y < rows[i].y + rows[i].h)
                    { selected = i; action = 1; break; }
        }
        updated = 0;
        if (action)
        {
            ui_sound( ui, LAUNCHER_SOUND_ACCEPT );
            if (selected == 1 || selected == 2)
            {
                char *value = selected == 2 ? author : name;
                if (launcher_platform_prompt( selected == 2 ? "Author" : "Name", value, edited, sizeof(edited) ))
                    snprintf( value, sizeof(name), "%s", edited );
            }
            else if (!selected)
            {
                const char *sources[] = { "SteamGridDB", "SD / USB" };
                int source = ui_menu( ui, "Choose icon", sources, 2, 0 );
                if (!source) updated = steam_icon( ui, &job, api_key, key_size, name );
                else if (source == 1 && pick_image( picker_data, path, sizeof(path) ))
                {
                    job.kind = JOB_FILE;
                    job.path = path;
                    updated = run_job( ui, &job, "Reading icon..." );
                }
            }
            else if (selected == 3)
            {
                unsigned char *jpeg = NULL;
                size_t bytes = 0;
                const char *warning = options->emummc > 0 ?
                    "Install this HOME Menu shortcut? Autorun's main forwarder will also be installed if missing." :
                    "Installing unofficial titles can lead to a console ban. Use emuMMC. Autorun's main forwarder will also be installed if missing.";
                if (!name[strspn( name, " \t\r\n" )]) ui_message( ui, "Forwarder", "Enter a name first." );
                else if (ui_confirm( ui, "Install game forwarder?", warning, "Install" ))
                {
                    if (!launcher_image_jpeg( &icon, &jpeg, &bytes ))
                        ui_message( ui, "Forwarder", "Could not prepare the HOME Menu icon." );
                    else
                    {
                        job.kind = JOB_INSTALL;
                        job.request = (struct wine_nx_forwarder){ .nro_path = AUTORUN_NRO, .name = name,
                            .author = author, .icon = jpeg, .icon_size = bytes, .game_id = game->id };
                        i = run_job( ui, &job, "Installing on the HOME Menu..." );
                        free( jpeg );
                        if (i) { ui_toast( ui, "Forwarder installed", 2400 ); goto closed; }
                    }
                }
            }
            if (updated)
            {
                SDL_Texture *next = image_texture( ui, &job.image );
                if (next)
                {
                    launcher_icon_free( &icon );
                    icon = job.image;
                    memset( &job.image, 0, sizeof(job.image) );
                    SDL_DestroyTexture( preview );
                    preview = next;
                }
                else ui_message( ui, "Icon", "Could not create the icon preview." );
            }
            launcher_icon_free( &job.image );
            ui_start_screen( ui );
        }
        panel( ui, background, card, "Create game forwarder" );
        ui_rounded_texture( ui, preview, NULL, rows[0],
                            18, (SDL_Color){ 255, 255, 255, 255 } );
        ui_rounded( ui, rows[0].x + 12, rows[0].y + 176, 200, 36, 10, (SDL_Color){ 4, 7, 11, 225 } );
        ui_text_centered( ui, ui->small, rows[0].x + 112, rows[0].y + 182, "Change icon", ui->value );
        if (!selected) ui_animated_border( ui, rows[0].x - 3, rows[0].y - 3, 230, 230, 20, 2,
                                           (SDL_Color){ 120, 165, 163, 170 }, (SDL_Color){ 216, 255, 235, 255 } );
        for (i = 1; i < 4; i++)
        {
            const char *label = i == 1 ? "Name" : i == 2 ? "Author" : "Install forwarder";
            SDL_Rect r = rows[i];
            ui_rounded( ui, r.x, r.y, r.w, r.h, 12, i == 3 ? (SDL_Color){ 34, 67, 55, 255 } : ui->panel );
            if (i == selected) ui_animated_border( ui, r.x, r.y, r.w, r.h, 12, 2,
                (SDL_Color){ 120, 165, 163, 170 }, (SDL_Color){ 216, 255, 235, 255 } );
            ui_text_fit( ui, i < 3 ? ui->small : ui->normal, r.x + 16, r.y + (i < 3 ? 7 : 15),
                         r.w - 32, label, i < 3 ? ui->dim : ui->value, 0 );
            if (i < 3) ui_text_fit( ui, ui->normal, r.x + 16, r.y + 31, r.w - 32,
                                    i == 2 ? author : name, ui->value, i == selected );
        }
        ui_text( ui, ui->small, card.x + 28, card.y + 337, "Launch directly with this game's settings.", ui->dim );
        ui_hints_right( ui, hints, 2, card.x + card.w - 28, card.y + card.h - 28 );
        ui_present( ui );
        ui_wait( ui );
    }
closed:
    ui->modal_depth--;
done:
    launcher_icon_free( &job.image );
    launcher_icon_free( &icon );
    SDL_DestroyTexture( preview );
    SDL_DestroyTexture( background );
    ui_start_screen( ui );
}
