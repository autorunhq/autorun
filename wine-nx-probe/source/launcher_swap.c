#include "launcher_swap.h"
#include "swap_store.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct swap_job
{
    SDL_mutex *mutex;
    SDL_atomic_t done, cancel;
    unsigned int megabytes;
    const char *directory;
    const char *phase;
    uint64_t current, total;
    int success;
    char result[192];
};

static int progress( void *context, const char *phase, uint64_t current, uint64_t total )
{
    struct swap_job *job = context;
    SDL_LockMutex( job->mutex );
    job->phase = phase;
    job->current = current;
    job->total = total;
    SDL_UnlockMutex( job->mutex );
    return !SDL_AtomicGet( &job->cancel );
}

static int worker( void *context )
{
    struct swap_job *job = context;
    struct swap_store store;
    if (swap_store_open( &store, job->directory, job->megabytes, progress, job ))
        snprintf( job->result, sizeof(job->result), "Could not prepare SD swap (%s): %s (fs 0x%x)",
                  store.operation, strerror( errno ), store.fs_error );
    else
    {
        swap_store_close( &store );
        if (swap_store_open_existing( &store, job->directory, job->megabytes ))
            snprintf( job->result, sizeof(job->result), "Could not open SD swap for paging: %s (fs 0x%x)",
                      strerror( errno ), store.fs_error );
        else
        {
            swap_store_close( &store );
            job->success = 1;
        }
    }
    SDL_AtomicSet( &job->done, 1 );
    return 0;
}

int launcher_swap_prepare( struct ui *ui, const char *directory, unsigned int megabytes )
{
    struct swap_job job = { .directory = directory, .megabytes = megabytes, .phase = "Allocating SD space" };
    SDL_Thread *thread;
    const struct ui_hint hints[] = { { UI_B, "Cancel" } };
    char text[96];

    snprintf( text, sizeof(text), "Reserve %u MiB on SD and enable in-game paging?", megabytes );
    if (!ui_confirm( ui, "SD swap", text, "Enable" )) return 0;
    job.mutex = SDL_CreateMutex();
    if (!job.mutex) { ui_message( ui, "SD swap", "Could not create the worker lock." ); return 0; }
    thread = SDL_CreateThreadWithStackSize( worker, "swap setup", 256 * 1024, &job );
    if (!thread)
    {
        SDL_DestroyMutex( job.mutex );
        ui_message( ui, "SD swap", "Could not start the setup worker." );
        return 0;
    }
    ui_progress_begin( ui );
    while (!SDL_AtomicGet( &job.done ))
    {
        struct ui_input input;
        const int width = 600, height = 236, x = (ui->width - width) / 2, y = (ui->height - height) / 2;
        const char *phase;
        uint64_t current, total;
        int fill;
        if (!ui_begin_frame( ui )) SDL_AtomicSet( &job.cancel, 1 );
        while (ui_poll( ui, &input ))
            if (input.button == UI_B && !SDL_AtomicGet( &job.cancel ))
            { ui_sound( ui, LAUNCHER_SOUND_BACK ); SDL_AtomicSet( &job.cancel, 1 ); }
        SDL_LockMutex( job.mutex );
        phase = job.phase;
        current = job.current;
        total = job.total;
        SDL_UnlockMutex( job.mutex );
        if (ui->snapshot) SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        else ui_background( ui );
        ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
        ui_rounded( ui, x, y, width, height, 22, ui->panel );
        ui_outline( ui, x, y, width, height, 22, 1, ui->dim );
        ui_text( ui, ui->normal, x + 32, y + 28, "Preparing SD swap", ui->value );
        ui_text( ui, ui->small, x + 32, y + 76,
                 SDL_AtomicGet( &job.cancel ) ? "Stopping safely..." : phase, ui->text );
        fill = total ? (int)((width - 64) * (double)current / total) : 0;
        ui_rounded( ui, x + 32, y + 120, width - 64, 10, 5, ui->dim );
        if (fill > 0) ui_rounded( ui, x + 32, y + 120, fill, 10, 5, ui->selection );
        snprintf( text, sizeof(text), "%llu / %llu MiB", (unsigned long long)(current / 1048576),
                  (unsigned long long)(total / 1048576) );
        ui_text( ui, ui->small, x + 32, y + 148, text, ui->dim );
        ui_hints_right( ui, hints, 1, x + width - 32, y + 202 );
        ui_present( ui );
        SDL_Delay( 16 );
    }
    SDL_WaitThread( thread, NULL );
    SDL_DestroyMutex( job.mutex );
    ui_progress_end( ui );
    if (ui->running && !job.success && !SDL_AtomicGet( &job.cancel )) ui_message( ui, "SD swap", job.result );
    ui_start_screen( ui );
    return job.success && !SDL_AtomicGet( &job.cancel ) && ui->running;
}
