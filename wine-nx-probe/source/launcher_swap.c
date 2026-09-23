#include "launcher_swap.h"
#include "swap_poc.h"
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
    char result[512];
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
    wine_nx_swap_test( job->directory, job->megabytes, progress, job, job->result, sizeof(job->result) );
    SDL_AtomicSet( &job->done, 1 );
    return 0;
}

void launcher_swap_test( struct ui *ui, const char *directory, unsigned int megabytes )
{
    struct swap_job job = { .directory = directory, .megabytes = megabytes, .phase = "Preparing SD swap" };
    SDL_Thread *thread;
    const struct ui_hint hints[] = { { UI_B, "Cancel" } };
    char text[96];

    snprintf( text, sizeof(text), "Preallocate %u MiB on SD and test private pages. This does not enable game swap.", megabytes );
    if (!ui_confirm( ui, "SD swap prototype", text, "Run test" )) return;
    job.mutex = SDL_CreateMutex();
    if (!job.mutex) { ui_message( ui, "SD swap", "Could not create the worker lock." ); return; }
    thread = SDL_CreateThreadWithStackSize( worker, "swap validation", 256 * 1024, &job );
    if (!thread)
    {
        SDL_DestroyMutex( job.mutex );
        ui_message( ui, "SD swap", "Could not start the validation worker." );
        return;
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
            if (input.button == UI_B) SDL_AtomicSet( &job.cancel, 1 );
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
        ui_text( ui, ui->normal, x + 32, y + 28, "SD swap prototype", ui->value );
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
    if (ui->running) ui_message( ui, "SD swap prototype", job.result );
    ui_start_screen( ui );
}
