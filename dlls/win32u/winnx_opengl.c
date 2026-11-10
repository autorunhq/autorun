/*
 * OpenGL for the Switch display driver
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __SWITCH__

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "wine/opengl_driver.h"
#include "wine/debug.h"
#include "../../horizon-wine/source/osk.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* The runtime hands the screen's NWindow to one OpenGL surface at a time
 * (horizon-wine/source/runtime.c). */
extern void *wine_nx_gl_acquire_window( void );
extern void wine_nx_gl_release_window( void );
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs nx_drawable_funcs;

struct nx_gl_drawable
{
    struct opengl_drawable base;
    BOOL screen;  /* this drawable shares the screen's EGL surface */
    LONG generation;
};

struct nx_client_surface
{
    struct client_surface base;
    SIZE size;
    LONG generation;
};

static void nx_client_surface_destroy( struct client_surface *client )
{
}

static void nx_client_surface_update( struct client_surface *client )
{
    struct nx_client_surface *surface = CONTAINING_RECORD( client, struct nx_client_surface, base );
    const RECT *rect = client->raw ? &client->monitor_rect : &client->virtual_rect;
    SIZE size;
    BOOL offscreen, changed;

    if (!client->format) return; /* Vulkan clients have no WGL pixel format. */
    size = (SIZE){ max( 1, rect->right - rect->left ), max( 1, rect->bottom - rect->top ) };
    offscreen = !(get_window_long( client->hwnd, GWL_STYLE ) & WS_VISIBLE);
    if (!offscreen && client->hwnd != client->toplevel) offscreen = !is_window_visible( client->hwnd );
    changed = InterlockedExchange( &client->offscreen, offscreen ) != offscreen;

    if (changed || (offscreen && (size.cx != surface->size.cx || size.cy != surface->size.cy)))
        InterlockedIncrement( &surface->generation );
    surface->size = size;
}

static void nx_client_surface_present( struct client_surface *client, HDC hdc )
{
}

static const struct client_surface_funcs nx_client_surface_funcs =
{
    .size = sizeof(struct nx_client_surface),
    .destroy = nx_client_surface_destroy,
    .detach = nx_client_surface_destroy,
    .update = nx_client_surface_update,
    .present = nx_client_surface_present,
};

struct client_surface *wine_nx_drv_CreateClientSurface( HWND hwnd, int format, BOOL raw )
{
    struct client_surface *client = client_surface_create( &nx_client_surface_funcs, hwnd, format, raw );

    if (client) nx_client_surface_update( client );
    return client;
}

/* The one EGL surface on the screen's NWindow, and the drawables sharing it.
 * win32u gives a window a second drawable whenever another context is made
 * current on it while the first is still in use - wined3d does exactly that
 * when it replaces its caps context with a versioned one - so the drawables
 * of a window share the surface rather than each asking for the screen. */
static pthread_mutex_t nx_screen_mutex = PTHREAD_MUTEX_INITIALIZER;
static EGLSurface nx_screen_surface;
static unsigned int nx_screen_refs;
static int nx_screen_format;

static struct nx_gl_drawable *impl_from_opengl_drawable( struct opengl_drawable *base )
{
    return CONTAINING_RECORD( base, struct nx_gl_drawable, base );
}

BOOL wine_nx_gl_drawable_matches_window( struct opengl_drawable *base )
{
    struct nx_client_surface *client;

    if (base->funcs != &nx_drawable_funcs) return TRUE;
    if (!base->client || base->client->funcs != &nx_client_surface_funcs) return TRUE;
    client = CONTAINING_RECORD( base->client, struct nx_client_surface, base );
    return impl_from_opengl_drawable( base )->generation ==
           InterlockedCompareExchange( &client->generation, 0, 0 );
}

static void nx_log( const char *format, ... )
{
    char buffer[256];
    va_list args;

    if (!&wine_nx_runtime_trace) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    wine_nx_runtime_trace( buffer );
}

/* The EGL config of a pixel format, as win32u's egldrv chooses it for pbuffers. */
static EGLConfig nx_config_for_format( int format )
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void nx_drawable_destroy( struct opengl_drawable *base )
{
    struct nx_gl_drawable *gl = impl_from_opengl_drawable( base );
    EGLSurface surface = NULL;

    if (!gl->screen)
    {
        /* win32u destroys the EGL surface after this callback. */
        return;
    }

    /* The screen's surface outlives this drawable while another one shares it;
     * take it away from win32u either way, so the framebuffer only gets the
     * NWindow back once the surface is really gone. */
    base->surface = NULL;

    pthread_mutex_lock( &nx_screen_mutex );
    if (nx_screen_refs && !--nx_screen_refs)
    {
        surface = nx_screen_surface;
        nx_screen_surface = NULL;
    }
    pthread_mutex_unlock( &nx_screen_mutex );

    if (!surface) return;
    funcs->p_eglDestroySurface( egl->display, surface );
    wine_nx_gl_release_window();
}

static void nx_drawable_flush( struct opengl_drawable *base, UINT flags )
{
    TRACE( "drawable %s, flags %#x\n", debugstr_opengl_drawable( base ), flags );

    if (impl_from_opengl_drawable( base )->screen && (flags & GL_FLUSH_INTERVAL))
        funcs->p_eglSwapInterval( egl->display, abs( base->interval ) );
    if (flags & GL_FLUSH_UPDATED)
    {
        /* Said again whenever the window's size changes, which is what a
         * program asking for another display mode does: whether the picture
         * then fills the monitor or keeps its own size is the one thing the
         * points a finger sends are measured by. */
        static int told_cx, told_cy, told_mx, told_my;
        char line[128];

        if (base->virtual_size.cx != told_cx || base->virtual_size.cy != told_cy ||
            base->monitor_size.cx != told_mx || base->monitor_size.cy != told_my)
        {
            told_cx = base->virtual_size.cx;
            told_cy = base->virtual_size.cy;
            told_mx = base->monitor_size.cx;
            told_my = base->monitor_size.cy;
            snprintf( line, sizeof(line), "[NXGL] the window's surface is %dx%d, the monitor %dx%d%s",
                      told_cx, told_cy, told_mx, told_my,
                      impl_from_opengl_drawable( base )->screen ? "" : ", offscreen" );
            nx_log( "%s", line );
        }
    }
}

/* Frames presented and the time inside eglSwapBuffers, for [PROGRESS]. */
extern unsigned long long horizon_interrupt_time(void);
unsigned int wine_nx_gl_swaps;
unsigned long long wine_nx_gl_swap_time;  /* 100 ns */

/* The floating keyboard and the controls panel (horizon-wine/source/osk.c),
 * put into the back buffer just before it is shown: the picture in a texture
 * of each context's own, blitted from a framebuffer of its own onto the
 * window's. One surface, three views, so this asks for the picture rather than
 * for the keyboard: a program that draws through here is a program whose
 * frames the compositor does not draw, and the panel has to arrive this way
 * too. Everything the blit and the upload depend on is put back as the
 * program left it. */
struct nx_osk_gl
{
    EGLContext context;
    GLuint texture, framebuffer;
    int width, height;
    unsigned int generation;
    void *pixels;
};
static struct nx_osk_gl nx_osk_gl[8];
static unsigned int nx_osk_gl_next;
static PFN_glGenFramebuffers p_glGenFramebuffers;
static PFN_glBindFramebuffer p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glBlitFramebuffer p_glBlitFramebuffer;
static PFN_glIsFramebuffer p_glIsFramebuffer;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;

/* What taking a picture out of a framebuffer and putting it back elsewhere
 * needs, asked for once. Both the keyboard's own blit and the picture's
 * use it, so it lives here rather than in either of them. */
static BOOL nx_load_blit_procs(void)
{
    if (p_glBlitFramebuffer) return TRUE;
    p_glGenFramebuffers = (void *)funcs->p_eglGetProcAddress( "glGenFramebuffers" );
    p_glBindFramebuffer = (void *)funcs->p_eglGetProcAddress( "glBindFramebuffer" );
    p_glFramebufferTexture2D = (void *)funcs->p_eglGetProcAddress( "glFramebufferTexture2D" );
    p_glIsFramebuffer = (void *)funcs->p_eglGetProcAddress( "glIsFramebuffer" );
    p_glBindBuffer = (void *)funcs->p_eglGetProcAddress( "glBindBuffer" );
    p_glBlitFramebuffer = (void *)funcs->p_eglGetProcAddress( "glBlitFramebuffer" );
    p_glCheckFramebufferStatus = (void *)funcs->p_eglGetProcAddress( "glCheckFramebufferStatus" );
    if (!p_glGenFramebuffers || !p_glBindFramebuffer || !p_glFramebufferTexture2D || !p_glIsFramebuffer ||
        !p_glBindBuffer || !p_glBlitFramebuffer)
    {
        p_glBlitFramebuffer = NULL;
        return FALSE;
    }
    return TRUE;
}

/* The picture of a program that asked for another display mode is as big as
 * the window it asked for, while the screen it is shown on is the console's.
 * Where the picture belongs is what the driver fits the window into, and it is
 * the same rectangle the points a finger sends are measured by: win32u gives
 * that in map_rect_virt_to_raw(). The window's own framebuffer knows nothing
 * of it, so what the program draws arrives in the corner at its own size.
 * Take it out of the framebuffer, make the screen black, and put it back
 * scaled into its place: then the picture is where the points are. */
struct nx_picture
{
    EGLContext context;
    GLuint texture, framebuffer;
    int width, height;
};
static struct nx_picture nx_pictures[8];
static unsigned int nx_picture_next;

static void nx_picture_present( struct opengl_drawable *base )
{
    EGLint screen_width = 0, screen_height = 0;
    GLint source_width, source_height, read_fb, draw_fb, texture;
    GLboolean scissor, srgb;
    struct nx_picture *picture = NULL;
    struct ratio dpi;
    EGLContext context;
    RECT client, place;
    unsigned int i;
    int pane_w, pane_h, left, top, right, bottom, from_x, from_y, to_x, to_y;

    if (!base->client || !nx_load_blit_procs()) return;
    if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &screen_width ) ||
        !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &screen_height ) ||
        screen_width <= 0 || screen_height <= 0 || !(context = funcs->p_eglGetCurrentContext()))
        return;
    source_width = base->virtual_size.cx;
    source_height = base->virtual_size.cy;
    if (source_width <= 0 || source_height <= 0) return;
    /* Only a picture that is the whole desktop: a program in a window of its
     * own on a desktop it shares is the compositor's business, and the screen
     * it would be put on is not this framebuffer's to blacken. */
    if (source_width != NtUserGetSystemMetrics( SM_CXSCREEN ) ||
        source_height != NtUserGetSystemMetrics( SM_CYSCREEN ))
        return;

    /* The window in the screen's own points, which is where the picture is
     * looked for: the whole screen for a program that fills it. */
    dpi = get_dpi_for_window( base->client->hwnd );
    if (!get_client_rect( base->client->hwnd, &client, dpi )) return;
    map_window_points( base->client->hwnd, 0, (POINT *)&client, 2, dpi );
    place = map_rect_virt_to_raw( client, dpi );
    pane_w = place.right - place.left;
    pane_h = place.bottom - place.top;

    /* What of the picture the screen shows: all of it when it is placed inside
     * the screen, the middle of it when it is larger -- a picture filling the
     * screen has lost its edges to the sides of it. */
    left = min( max( place.left, 0 ), screen_width );
    right = min( max( place.right, 0 ), screen_width );
    top = min( max( place.top, 0 ), screen_height );
    bottom = min( max( place.bottom, 0 ), screen_height );
    if (right - left <= 0 || bottom - top <= 0 || pane_w <= 0 || pane_h <= 0) return;
    if (!left && !top && !place.left && !place.top && right == screen_width && bottom == screen_height &&
        place.right == screen_width && place.bottom == screen_height &&
        source_width == pane_w && source_height == pane_h)
        return;

    /* The same rectangle in the picture's own pixels, so that a picture placed
     * larger than the screen is cropped rather than squashed into it. */
    from_x = (int)((INT64)(left - place.left) * source_width / pane_w);
    to_x = (int)((INT64)(right - place.left) * source_width / pane_w);
    from_y = (int)((INT64)(top - place.top) * source_height / pane_h);
    to_y = (int)((INT64)(bottom - place.top) * source_height / pane_h);
    if (from_x < 0) from_x = 0;
    if (from_y < 0) from_y = 0;
    if (to_x > source_width) to_x = source_width;
    if (to_y > source_height) to_y = source_height;
    if (to_x - from_x <= 0 || to_y - from_y <= 0) return;

    {
        static int told[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int now[8] = {source_width, source_height, left, top, right - left, bottom - top,
                      (int)screen_width, (int)screen_height};
        char line[160];

        if (memcmp( now, told, sizeof(now) ))
        {
            memcpy( told, now, sizeof(now) );
            snprintf( line, sizeof(line), "[NXGL] the %dx%d picture is drawn at %d,%d %dx%d of the %dx%d screen",
                      source_width, source_height, left, top, right - left, bottom - top,
                      (int)screen_width, (int)screen_height );
            nx_log( "%s", line );
        }
    }

    for (i = 0; i < ARRAY_SIZE(nx_pictures) && !picture; i++)
        if (nx_pictures[i].context == context) picture = &nx_pictures[i];
    if (!picture)
    {
        /* A context not seen before: its names are its own, so the slot's old
         * ones are only forgotten, never deleted from here. */
        picture = &nx_pictures[nx_picture_next++ % ARRAY_SIZE(nx_pictures)];
        memset( picture, 0, sizeof(*picture) );
        picture->context = context;
    }

    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fb );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb );
    funcs->p_glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB );

    if (!picture->texture || !funcs->p_glIsTexture( picture->texture ) ||
        !p_glIsFramebuffer( picture->framebuffer ))
    {
        funcs->p_glGenTextures( 1, &picture->texture );
        p_glGenFramebuffers( 1, &picture->framebuffer );
        picture->width = picture->height = 0;
    }
    funcs->p_glBindTexture( GL_TEXTURE_2D, picture->texture );
    if (picture->width != source_width || picture->height != source_height)
    {
        funcs->p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, source_width, source_height, 0, GL_RGBA,
                               GL_UNSIGNED_BYTE, NULL );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
        picture->width = source_width;
        picture->height = source_height;
    }
    if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );

    /* Out of the window's framebuffer at its own size: no scaling, no flip. */
    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, picture->framebuffer );
    p_glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, picture->texture, 0 );
    if (p_glCheckFramebufferStatus && p_glCheckFramebufferStatus( GL_DRAW_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE)
    {
        static BOOL told;
        char line[128];

        if (!told++)
        {
            snprintf( line, sizeof(line), "[NXGL] the picture cannot be moved: the screen's framebuffer is not usable" );
            nx_log( "%s", line );
        }
        goto done;
    }
    p_glBlitFramebuffer( 0, 0, source_width, source_height, 0, 0, source_width, source_height,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST );

    /* The screen black, then the picture scaled into its place. The screen's
     * first row is its top and the window's is its bottom. */
    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, picture->framebuffer );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
    funcs->p_glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    funcs->p_glClear( GL_COLOR_BUFFER_BIT );
    p_glBlitFramebuffer( from_x, source_height - to_y, to_x, source_height - from_y,
                         left, screen_height - bottom, right, screen_height - top,
                         GL_COLOR_BUFFER_BIT, GL_LINEAR );

done:
    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fb );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fb );
    funcs->p_glBindTexture( GL_TEXTURE_2D, texture );
    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
}

static void nx_osk_draw( struct opengl_drawable *base )
{
    struct wine_nx_osk_frame frame;
    struct nx_osk_gl *osk = NULL;
    EGLint width = 0, height = 0;
    EGLContext context;
    GLint read_fb, draw_fb, texture, unpack_buffer, row_length, skip_pixels, skip_rows, alignment;
    GLboolean scissor, srgb;
    unsigned int i;

    if (!wine_nx_osk_visible() && !wine_nx_osk_map_visible()) return;
    if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) ||
        !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height ) ||
        !wine_nx_osk_frame( width, height, &frame ) || !(context = funcs->p_eglGetCurrentContext()))
        return;
    if (!nx_load_blit_procs()) return;
    for (i = 0; i < ARRAY_SIZE(nx_osk_gl) && !osk; i++)
        if (nx_osk_gl[i].context == context) osk = &nx_osk_gl[i];
    if (!osk)
    {
        /* A context not seen before: its names are its own, so the slot's
         * old ones are only forgotten, never deleted from here. */
        osk = &nx_osk_gl[nx_osk_gl_next++ % ARRAY_SIZE(nx_osk_gl)];
        free( osk->pixels );
        memset( osk, 0, sizeof(*osk) );
        osk->context = context;
    }

    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fb );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb );
    funcs->p_glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
    funcs->p_glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer );
    funcs->p_glGetIntegerv( GL_UNPACK_ROW_LENGTH, &row_length );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skip_pixels );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skip_rows );
    funcs->p_glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB );

    /* Names the program deleted, or a context that took an old one's place. */
    if (!osk->texture || !funcs->p_glIsTexture( osk->texture ) || !p_glIsFramebuffer( osk->framebuffer ))
    {
        funcs->p_glGenTextures( 1, &osk->texture );
        p_glGenFramebuffers( 1, &osk->framebuffer );
        osk->width = osk->height = 0;
    }
    funcs->p_glBindTexture( GL_TEXTURE_2D, osk->texture );
    p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
    if (osk->width != frame.width || osk->height != frame.height)
    {
        free( osk->pixels );
        osk->pixels = malloc( (size_t)frame.width * frame.height * 4 );
        funcs->p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0, GL_BGRA,
                               GL_UNSIGNED_BYTE, NULL );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        osk->width = frame.width;
        osk->height = frame.height;
        osk->generation = 0;
    }
    if (osk->pixels && osk->generation != frame.generation &&
        (osk->generation = wine_nx_osk_copy( width, height, osk->pixels, frame.width * 4, 0 )))
        funcs->p_glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_BGRA, GL_UNSIGNED_BYTE,
                                  osk->pixels );
    if (osk->generation)
    {
        p_glBindFramebuffer( GL_READ_FRAMEBUFFER, osk->framebuffer );
        p_glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, osk->texture, 0 );
        p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
        if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
        if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
        /* The picture's first row is its top; the window's first is its bottom. */
        p_glBlitFramebuffer( 0, 0, frame.width, frame.height, frame.x, height - frame.y,
                             frame.x + frame.width, height - frame.y - frame.height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
        if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
        if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
    }

    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fb );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fb );
    funcs->p_glBindTexture( GL_TEXTURE_2D, texture );
    p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, unpack_buffer );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, row_length );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, skip_pixels );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, skip_rows );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
}

static BOOL nx_drawable_swap( struct opengl_drawable *base )
{
    unsigned long long start;
    BOOL ret;

    if (!impl_from_opengl_drawable( base )->screen)
        return funcs->p_eglSwapBuffers( egl->display, base->surface );
    nx_picture_present( base );
    nx_osk_draw( base );
    start = horizon_interrupt_time();
    ret = funcs->p_eglSwapBuffers( egl->display, base->surface );
    __atomic_add_fetch( &wine_nx_gl_swap_time, horizon_interrupt_time() - start, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_gl_swaps, 1, __ATOMIC_RELAXED );
    return ret;
}

static const struct opengl_drawable_funcs nx_drawable_funcs =
{
    .size = sizeof(struct nx_gl_drawable),
    .destroy = nx_drawable_destroy,
    .flush = nx_drawable_flush,
    .swap = nx_drawable_swap,
};

/* Visible OpenGL windows share the screen; hidden windows render offscreen. */
static BOOL nx_surface_create( struct client_surface *client, int format, struct opengl_drawable **drawable )
{
    HWND hwnd = client->hwnd;
    struct opengl_drawable *previous;
    struct nx_gl_drawable *gl;
    struct nx_client_surface *nx_client = CONTAINING_RECORD( client, struct nx_client_surface, base );
    LONG generation = client->funcs == &nx_client_surface_funcs ?
                      InterlockedCompareExchange( &nx_client->generation, 0, 0 ) : 0;
    void *window;

    TRACE( "hwnd %p, format %d\n", hwnd, format );

    if ((previous = *drawable) && previous->format == format && wine_nx_gl_drawable_matches_window( previous ))
        return TRUE;
    /* A previous surface may hold the screen: let it go first. */
    if (previous)
    {
        opengl_drawable_release( previous );
        *drawable = NULL;
    }

    gl = opengl_drawable_create( &nx_drawable_funcs, format, client, NULL );
    if (!gl) return FALSE;
    gl->generation = generation;
    gl->base.buffer_map[0] = GL_BACK_LEFT;
    gl->base.buffer_map[1] = GL_BACK_RIGHT;
    gl->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = GL_BACK;
    gl->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = GL_BACK;

    if (InterlockedCompareExchange( &client->offscreen, 0, 0 ))
    {
        /* Capability probes must not take the screen away from Vulkan. */
        SIZE size = client->raw ? gl->base.monitor_size : gl->base.virtual_size;
        const EGLint attribs[] = { EGL_WIDTH, size.cx, EGL_HEIGHT, size.cy, EGL_NONE };

        if (!(gl->base.surface = funcs->p_eglCreatePbufferSurface( egl->display, nx_config_for_format( format ), attribs )))
        {
            ERR( "hwnd %p: eglCreatePbufferSurface failed, error %#x\n", hwnd, funcs->p_eglGetError() );
            goto err;
        }
        TRACE( "hwnd %p: offscreen surface %p, size %s\n", hwnd, gl->base.surface,
               wine_dbgstr_point( (POINT *)&size ) );
        *drawable = &gl->base;
        return TRUE;
    }

    pthread_mutex_lock( &nx_screen_mutex );
    if (nx_screen_surface && nx_screen_format == format)
    {
        /* Another drawable of this window still has the screen; share it. */
        gl->base.surface = nx_screen_surface;
        gl->screen = TRUE;
        nx_screen_refs++;
        pthread_mutex_unlock( &nx_screen_mutex );
        TRACE( "hwnd %p: sharing the screen surface %p\n", hwnd, nx_screen_surface );
        *drawable = &gl->base;
        return TRUE;
    }
    if (nx_screen_surface)
    {
        pthread_mutex_unlock( &nx_screen_mutex );
        ERR( "hwnd %p: the screen has a format %d surface, cannot serve format %d\n",
             hwnd, nx_screen_format, format );
        goto err;
    }
    pthread_mutex_unlock( &nx_screen_mutex );

    if (!(window = wine_nx_gl_acquire_window()))
    {
        ERR( "hwnd %p: the screen already has an OpenGL surface\n", hwnd );
        goto err;
    }
    gl->screen = TRUE;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface( egl->display, nx_config_for_format( format ),
                                                              (EGLNativeWindowType)window, NULL )))
    {
        ERR( "hwnd %p: eglCreateWindowSurface failed, error %#x\n", hwnd, funcs->p_eglGetError() );
        /* nothing is sharing the screen yet, so give it back here */
        gl->screen = FALSE;
        wine_nx_gl_release_window();
        goto err;
    }

    pthread_mutex_lock( &nx_screen_mutex );
    nx_screen_surface = gl->base.surface;
    nx_screen_format = format;
    nx_screen_refs = 1;
    pthread_mutex_unlock( &nx_screen_mutex );

    TRACE( "created drawable %s with EGL surface %p\n", debugstr_opengl_drawable( &gl->base ), gl->base.surface );
    {
        /* The surface the program's picture is drawn into and the monitor it
         * is shown on: a point on the screen means the same thing to both
         * only when the two numbers agree. */
        static int told;

        if (!told)
        {
            told = 1;
            nx_log( "[NXGL] the monitor is %dx%d, the window's surface is %dx%d",
                    (int)gl->base.monitor_size.cx, (int)gl->base.monitor_size.cy,
                    (int)gl->base.virtual_size.cx, (int)gl->base.virtual_size.cy );
        }
    }
    {
        static BOOL logged;
        const char *vendor = funcs->p_eglQueryString( egl->display, EGL_VENDOR );
        const char *version = funcs->p_eglQueryString( egl->display, EGL_VERSION );

        /* raw 0: win32u draws through its framebuffer surface (DPI scaling or gamma) */
        if (!logged) nx_log( "[NXGL] EGL %s %s, %u configs; window surface for format %d, raw %u",
                             vendor ? vendor : "?", version ? version : "?", egl->config_count, format, client->raw );
        logged = TRUE;
    }
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release( &gl->base );
    return FALSE;
}

/* win32u's egldrv creates contexts without a config, but switch-mesa 20.1's EGL
 * driver lacks EGL_KHR_no_config_context (such a context fails with
 * EGL_BAD_CONFIG) and EGL_KHR_create_context_no_error. A context gets the config
 * of its pixel format, the one this format's window surfaces and pbuffers use,
 * so EGL lets them be made current together. */
static struct opengl_context *nx_context_create( int format, struct opengl_context *share,
                                                  const int *attribs, BOOL *shared )
{
    EGLint egl_attribs[16], *end = egl_attribs, error;
    struct opengl_context *context;

    TRACE( "format %d, share %p, attribs %p\n", format, share, attribs );

    for (; attribs && attribs[0]; attribs += 2)
    {
        EGLint name, *dst;

        switch (attribs[0])
        {
        case WGL_CONTEXT_MAJOR_VERSION_ARB:
            name = EGL_CONTEXT_MAJOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_MINOR_VERSION_ARB:
            name = EGL_CONTEXT_MINOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_FLAGS_ARB:
            name = EGL_CONTEXT_FLAGS_KHR;
            break;
        case WGL_CONTEXT_PROFILE_MASK_ARB:
            if (attribs[1] & WGL_CONTEXT_ES2_PROFILE_BIT_EXT)
            {
                ERR( "OpenGL ES contexts are not supported\n" );
                return NULL;
            }
            name = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
            break;
        case WGL_CONTEXT_OPENGL_NO_ERROR_ARB:
            FIXME( "no-error contexts are unavailable, ignoring %#x\n", attribs[1] );
            continue;
        default:
            FIXME( "unhandled attribute %#x %#x\n", attribs[0], attribs[1] );
            continue;
        }

        /* A repeated attribute replaces the earlier one. */
        for (dst = egl_attribs; dst != end && *dst != name; dst += 2) continue;
        if (dst == end)
        {
            if (end - egl_attribs >= (int)ARRAY_SIZE(egl_attribs) - 2) continue;
            end += 2;
        }
        dst[0] = name;
        dst[1] = attribs[1];
    }
    *end = EGL_NONE;

    if (!(context = calloc( 1, sizeof(*context) ))) return NULL;
    funcs->p_eglBindAPI( EGL_OPENGL_API );
    context->host_context = funcs->p_eglCreateContext( egl->display, nx_config_for_format( format ),
                                                        share ? share->host_context : NULL,
                                                        end != egl_attribs ? egl_attribs : NULL );
    if ((error = funcs->p_eglGetError()) != EGL_SUCCESS || !context->host_context)
    {
        ERR( "context creation failed for format %d, share %p, error %#x\n", format, share, error );
        free( context );
        return NULL;
    }
    TRACE( "created context %p\n", context );
    return context;
}

/* Mesa's Switch platform is the default display; its windows are NWindows. */
static void nx_init_egl_platform( struct egl_platform *platform )
{
    platform->type = 0;
    platform->native_display = 0;
    egl = platform;
}

static struct opengl_driver_funcs nx_driver_funcs =
{
    .p_init_egl_platform = nx_init_egl_platform,
    .p_surface_create = nx_surface_create,
    .p_context_create = nx_context_create,
};

UINT wine_nx_drv_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs,
                             const struct opengl_driver_funcs **driver_funcs )
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but the driver has %u\n", version, WINE_OPENGL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    nx_driver_funcs.p_get_proc_address = (*driver_funcs)->p_get_proc_address;
    nx_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    nx_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    nx_driver_funcs.p_init_extensions = (*driver_funcs)->p_init_extensions;
    nx_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    nx_driver_funcs.p_context_activate = (*driver_funcs)->p_context_activate;
    nx_driver_funcs.p_pbuffer_create = (*driver_funcs)->p_pbuffer_create;
    nx_driver_funcs.p_pbuffer_updated = (*driver_funcs)->p_pbuffer_updated;
    nx_driver_funcs.p_pbuffer_bind = (*driver_funcs)->p_pbuffer_bind;
    nx_driver_funcs.p_null_surface_create = (*driver_funcs)->p_null_surface_create;
    nx_driver_funcs.p_cleanup_thread = (*driver_funcs)->p_cleanup_thread;

    *driver_funcs = &nx_driver_funcs;
    return STATUS_SUCCESS;
}

#endif /* __SWITCH__ */
