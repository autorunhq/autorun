/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The screen compositor; see compositor.h. */
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compositor.h"
#include "compositor_gl.h"

struct wine_nx_layer
{
    struct wine_nx_layer *next;             /* in comp.layers */
    /* Guarded by comp.lock. */
    int x, y, visible_width, visible_height;
    int z;                                  /* drawn above the layers with a lower one */
    unsigned int stack_serial;              /* the restack that listed it */
    int destroyed;                          /* freed by the presenter */
    /* Guarded by pixels_lock; pixels is NULL once destroyed. */
    pthread_mutex_t pixels_lock;
    uint32_t *pixels;
    int width, height;
    int dirty_left, dirty_top, dirty_right, dirty_bottom;
    /* The presenter thread's only. */
    struct compositor_gl_texture texture;
};

enum compositor_state { STATE_STOPPED, STATE_STARTING, STATE_RUNNING, STATE_FAILED };

static struct
{
    pthread_mutex_t lock;
    pthread_cond_t wake;        /* for the presenter: something to draw, suspend or resume */
    pthread_cond_t idle;        /* for callers: the presenter started, or gave the screen up */
    const struct compositor_backend *backend;
    int width, height;
    enum compositor_state state;
    int frame;                  /* the screen needs drawing */
    int suspend;                /* an OpenGL program wants the screen */
    int suspended;              /* and the presenter has given it up */
    struct wine_nx_layer *layers;
    unsigned int stack_serial;
    int cursor_x, cursor_y, cursor_visible;
    unsigned int frames;
} comp =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wake = PTHREAD_COND_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

static void comp_log( const char *format, ... )
{
    char buffer[320];
    va_list args;

    if (!comp.backend->log) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    comp.backend->log( buffer );
}

/* With comp.lock held. */
static void request_frame( void )
{
    comp.frame = 1;
    pthread_cond_signal( &comp.wake );
}

static void clear_dirty( struct wine_nx_layer *layer )
{
    layer->dirty_left = layer->dirty_top = layer->dirty_right = layer->dirty_bottom = 0;
}

/* A 640x480 / 800x600 game window fills the 1280x720 screen. Tiny host-test
 * layers stay 1:1 so compositor_test pixel checks do not move. */
static void stretch_quads_to_screen( struct compositor_gl_quad *quads, int count,
                                     int *cursor_x, int *cursor_y, int screen_w, int screen_h )
{
    int i, max_w = 0, max_h = 0;

    for (i = 0; i < count; i++)
    {
        int right = quads[i].x + quads[i].width;
        int bottom = quads[i].y + quads[i].height;

        if (right > max_w) max_w = right;
        if (bottom > max_h) max_h = bottom;
    }
    if (max_w < 320 || max_h < 240) return;
    if (max_w >= screen_w && max_h >= screen_h) return;
    for (i = 0; i < count; i++)
    {
        if (quads[i].src_width <= 0) quads[i].src_width = quads[i].width;
        if (quads[i].src_height <= 0) quads[i].src_height = quads[i].height;
        quads[i].x = quads[i].x * screen_w / max_w;
        quads[i].y = quads[i].y * screen_h / max_h;
        quads[i].width = quads[i].width * screen_w / max_w;
        quads[i].height = quads[i].height * screen_h / max_h;
    }
    *cursor_x = *cursor_x * screen_w / max_w;
    *cursor_y = *cursor_y * screen_h / max_h;
}

struct wine_nx_layer *wine_nx_layer_create( int width, int height )
{
    struct wine_nx_layer *layer;

    if (width <= 0 || height <= 0 || !(layer = calloc( 1, sizeof(*layer) ))) return NULL;
    if (!(layer->pixels = calloc( (size_t)width * height, sizeof(*layer->pixels) )))
    {
        free( layer );
        return NULL;
    }
    pthread_mutex_init( &layer->pixels_lock, NULL );
    layer->width = width;
    layer->height = height;
    /* The first upload gives the texture its storage and the black start. */
    layer->dirty_right = width;
    layer->dirty_bottom = height;
    /* A new window starts on top until its window stack says otherwise. */
    layer->z = INT_MAX;

    pthread_mutex_lock( &comp.lock );
    layer->next = comp.layers;
    comp.layers = layer;
    pthread_mutex_unlock( &comp.lock );
    return layer;
}

void wine_nx_layer_destroy( struct wine_nx_layer *layer )
{
    if (!layer) return;
    pthread_mutex_lock( &layer->pixels_lock );
    free( layer->pixels );
    layer->pixels = NULL;
    pthread_mutex_unlock( &layer->pixels_lock );

    /* The texture can only go on the presenter thread, so it frees the rest. */
    pthread_mutex_lock( &comp.lock );
    layer->destroyed = 1;
    request_frame();
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_layer_update( struct wine_nx_layer *layer, const uint32_t *pixels, int stride,
                           int left, int top, int right, int bottom )
{
    int y;

    if (!layer) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > layer->width) right = layer->width;
    if (bottom > layer->height) bottom = layer->height;
    if (left >= right || top >= bottom) return;

    pthread_mutex_lock( &layer->pixels_lock );
    if (!layer->pixels)
    {
        pthread_mutex_unlock( &layer->pixels_lock );
        return;
    }
    for (y = top; y < bottom; y++)
        memcpy( layer->pixels + (size_t)y * layer->width + left, pixels + (size_t)y * stride + left,
                (size_t)(right - left) * sizeof(*pixels) );
    if (layer->dirty_left >= layer->dirty_right)
    {
        layer->dirty_left = left;
        layer->dirty_top = top;
        layer->dirty_right = right;
        layer->dirty_bottom = bottom;
    }
    else
    {
        if (left < layer->dirty_left) layer->dirty_left = left;
        if (top < layer->dirty_top) layer->dirty_top = top;
        if (right > layer->dirty_right) layer->dirty_right = right;
        if (bottom > layer->dirty_bottom) layer->dirty_bottom = bottom;
    }
    pthread_mutex_unlock( &layer->pixels_lock );

    pthread_mutex_lock( &comp.lock );
    request_frame();
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_layer_place( struct wine_nx_layer *layer, int x, int y, int width, int height )
{
    if (!layer) return;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    pthread_mutex_lock( &comp.lock );
    if (layer->x != x || layer->y != y || layer->visible_width != width || layer->visible_height != height)
    {
        layer->x = x;
        layer->y = y;
        layer->visible_width = width;
        layer->visible_height = height;
        request_frame();
    }
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_compositor_restack( struct wine_nx_layer **layers, int count )
{
    struct wine_nx_layer *layer;
    int changed = 0, i;

    pthread_mutex_lock( &comp.lock );
    comp.stack_serial++;
    for (i = 0; i < count; i++)
    {
        if (!layers[i]) continue;
        if (layers[i]->z != count - i) changed = 1;
        layers[i]->z = count - i;
        layers[i]->stack_serial = comp.stack_serial;
    }
    for (layer = comp.layers; layer; layer = layer->next)
    {
        if (layer->stack_serial == comp.stack_serial || layer->z == 0) continue;
        layer->z = 0;
        changed = 1;
    }
    if (changed) request_frame();
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_compositor_cursor( int x, int y, int visible )
{
    visible = !!visible;
    pthread_mutex_lock( &comp.lock );
    if (comp.cursor_x != x || comp.cursor_y != y || comp.cursor_visible != visible)
    {
        comp.cursor_x = x;
        comp.cursor_y = y;
        comp.cursor_visible = visible;
        request_frame();
    }
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_compositor_suspend( void )
{
    pthread_mutex_lock( &comp.lock );
    comp.suspend = 1;
    pthread_cond_signal( &comp.wake );
    while (comp.state == STATE_STARTING || (comp.state == STATE_RUNNING && !comp.suspended))
        pthread_cond_wait( &comp.idle, &comp.lock );
    pthread_mutex_unlock( &comp.lock );
}

void wine_nx_compositor_resume( void )
{
    pthread_mutex_lock( &comp.lock );
    comp.suspend = 0;
    comp.suspended = 0;
    request_frame();
    pthread_mutex_unlock( &comp.lock );
}

unsigned int wine_nx_compositor_frames( void )
{
    unsigned int frames;

    pthread_mutex_lock( &comp.lock );
    frames = comp.frames;
    pthread_mutex_unlock( &comp.lock );
    return frames;
}

int wine_nx_compositor_running( void )
{
    int running;

    pthread_mutex_lock( &comp.lock );
    running = comp.state == STATE_RUNNING;
    pthread_mutex_unlock( &comp.lock );
    return running;
}

static void upload_layer( struct compositor_gl *gl, struct wine_nx_layer *layer )
{
    pthread_mutex_lock( &layer->pixels_lock );
    if (layer->pixels && layer->dirty_left < layer->dirty_right)
    {
        compositor_gl_upload( gl, &layer->texture, layer->width, layer->height, layer->pixels, layer->width,
                              layer->dirty_left, layer->dirty_top, layer->dirty_right, layer->dirty_bottom );
        clear_dirty( layer );
    }
    pthread_mutex_unlock( &layer->pixels_lock );
}

static void *presenter_thread( void *arg )
{
    const struct compositor_backend *backend = comp.backend;
    const struct compositor_gl_funcs *funcs;
    struct wine_nx_layer **drawn = NULL, *layer, *dead;
    struct compositor_gl_quad *quads = NULL;
    struct compositor_gl gl;
    int capacity = 0, attached, attach_failed = 0, count, cursor_x, cursor_y, cursor_visible, i, j;
    char error[256] = "";

    (void)arg;
    if (!(funcs = backend->init( error, sizeof(error) ))) goto failed;
    if (backend->attach( comp.width, comp.height, error, sizeof(error) )) goto failed;
    if (compositor_gl_init( &gl, funcs, comp.width, comp.height ))
    {
        snprintf( error, sizeof(error), "%s", gl.error );
        backend->detach();
        goto failed;
    }
    attached = 1;
    comp_log( "[NXCOMP] presenting the screen through OpenGL on %s, %s",
              (const char *)funcs->GetString( GL_RENDERER ), (const char *)funcs->GetString( GL_VERSION ) );

    pthread_mutex_lock( &comp.lock );
    comp.state = STATE_RUNNING;
    comp.frame = 1;
    pthread_cond_broadcast( &comp.idle );
    for (;;)
    {
        if (comp.suspend)
        {
            if (attached)
            {
                pthread_mutex_unlock( &comp.lock );
                backend->detach();
                comp_log( "[NXCOMP] screen handed to an OpenGL program" );
                pthread_mutex_lock( &comp.lock );
                attached = 0;
            }
            comp.suspended = 1;
            pthread_cond_broadcast( &comp.idle );
            pthread_cond_wait( &comp.wake, &comp.lock );
            continue;
        }
        if (!attached)
        {
            int ret;

            pthread_mutex_unlock( &comp.lock );
            if (!(ret = backend->attach( comp.width, comp.height, error, sizeof(error) )))
                comp_log( "[NXCOMP] screen taken back from the OpenGL program" );
            else if (!attach_failed)
                comp_log( "[NXCOMP] cannot take the screen back yet: %s", error );
            pthread_mutex_lock( &comp.lock );
            attach_failed = !!ret;
            if (ret)
            {
                /* Try again on the next change. */
                comp.frame = 0;
                while (!comp.frame && !comp.suspend) pthread_cond_wait( &comp.wake, &comp.lock );
                continue;
            }
            attached = 1;
            comp.frame = 1;
        }
        if (!comp.frame)
        {
            pthread_cond_wait( &comp.wake, &comp.lock );
            continue;
        }
        comp.frame = 0;

        /* Take the destroyed layers out, and the shown ones in stacking order. */
        dead = NULL;
        count = 0;
        for (struct wine_nx_layer **link = &comp.layers; (layer = *link);)
        {
            if (layer->destroyed)
            {
                *link = layer->next;
                layer->next = dead;
                dead = layer;
                continue;
            }
            if (layer->visible_width && layer->visible_height) count++;
            link = &layer->next;
        }
        if (count > capacity)
        {
            struct compositor_gl_quad *new_quads = realloc( quads, sizeof(*quads) * count * 2 );
            struct wine_nx_layer **new_drawn = realloc( drawn, sizeof(*drawn) * count * 2 );

            if (new_quads) quads = new_quads;
            if (new_drawn) drawn = new_drawn;
            if (new_quads && new_drawn) capacity = count * 2;
        }
        count = 0;
        for (layer = comp.layers; layer && count < capacity; layer = layer->next)
        {
            if (!layer->visible_width || !layer->visible_height) continue;
            /* Insertion by z, bottom first; equal ones keep the list's order. */
            for (i = count; i > 0 && drawn[i - 1]->z > layer->z; i--) drawn[i] = drawn[i - 1];
            drawn[i] = layer;
            count++;
        }
        for (i = 0; i < count; i++)
        {
            layer = drawn[i];
            quads[i].texture = &layer->texture;
            quads[i].x = layer->x;
            quads[i].y = layer->y;
            quads[i].width = layer->visible_width < layer->width ? layer->visible_width : layer->width;
            quads[i].height = layer->visible_height < layer->height ? layer->visible_height : layer->height;
            quads[i].src_x = quads[i].src_y = 0;
            quads[i].src_width = quads[i].width;
            quads[i].src_height = quads[i].height;
        }
        cursor_x = comp.cursor_x;
        cursor_y = comp.cursor_y;
        cursor_visible = comp.cursor_visible;
        stretch_quads_to_screen( quads, count, &cursor_x, &cursor_y, comp.width, comp.height );
        pthread_mutex_unlock( &comp.lock );

        /* Only this thread frees layers, so drawn stays valid; a layer destroyed
         * meanwhile has no pixels to upload and goes on the next frame. */
        while ((layer = dead))
        {
            dead = layer->next;
            compositor_gl_release( &gl, &layer->texture );
            pthread_mutex_destroy( &layer->pixels_lock );
            free( layer );
        }
        for (j = 0; j < count; j++) upload_layer( &gl, drawn[j] );
        compositor_gl_draw( &gl, quads, count, cursor_x, cursor_y, cursor_visible );
        backend->swap();

        pthread_mutex_lock( &comp.lock );
        comp.frames++;
    }

failed:
    comp_log( "[NXCOMP] cannot present through OpenGL, keeping the framebuffer: %s", error );
    pthread_mutex_lock( &comp.lock );
    comp.state = STATE_FAILED;
    pthread_cond_broadcast( &comp.idle );
    pthread_mutex_unlock( &comp.lock );
    return NULL;
}

int wine_nx_compositor_start( const struct compositor_backend *backend, int width, int height )
{
    pthread_t thread;
    int ret;

    pthread_mutex_lock( &comp.lock );
    if (comp.state == STATE_STOPPED)
    {
        pthread_attr_t attr;

        comp.backend = backend;
        comp.width = width;
        comp.height = height;
        comp.state = STATE_STARTING;
        pthread_attr_init( &attr );
        /* Mesa compiles the shaders on this thread, which recurses deeply. */
        pthread_attr_setstacksize( &attr, 1024 * 1024 );
        /* Never joined: the presenter runs for the whole session. */
        if (pthread_create( &thread, &attr, presenter_thread, NULL )) comp.state = STATE_FAILED;
        pthread_attr_destroy( &attr );
    }
    while (comp.state == STATE_STARTING) pthread_cond_wait( &comp.idle, &comp.lock );
    ret = comp.state == STATE_RUNNING ? 0 : -1;
    pthread_mutex_unlock( &comp.lock );
    return ret;
}
