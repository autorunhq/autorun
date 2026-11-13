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
extern int wine_nx_dxvk_wraps_ddraw __attribute__((weak));  /* runtime.c */
extern int wine_nx_vk_served __attribute__((weak));         /* winnx_vulkan.c */
extern void wine_nx_gl_release_window( void );
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
extern int  wine_nx_osk_translucent( void ) __attribute__((weak));
extern int  wine_nx_cursor_state( int *x, int *y, int *visible ) __attribute__((weak));
extern void wine_nx_cursor_bitmap( uint32_t *rgba ) __attribute__((weak));

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
    /* DXVK's own ddraw.dll over Wine's: the screen is for its Vulkan surface. */
    if (!offscreen && &wine_nx_dxvk_wraps_ddraw && wine_nx_dxvk_wraps_ddraw &&
        &wine_nx_vk_served && __atomic_load_n( &wine_nx_vk_served, __ATOMIC_ACQUIRE ))
    {
        static LONG logged;

        offscreen = TRUE;
        if (!InterlockedExchange( &logged, 1 ) && &wine_nx_runtime_trace)
            wine_nx_runtime_trace( "[NXGL] an OpenGL surface stays offscreen: the screen is left to DXVK's Vulkan surface" );
    }
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
        static int told_cx, told_cy, told_mx, told_my;

        if (base->virtual_size.cx != told_cx || base->virtual_size.cy != told_cy ||
            base->monitor_size.cx != told_mx || base->monitor_size.cy != told_my)
        {
            told_cx = base->virtual_size.cx;
            told_cy = base->virtual_size.cy;
            told_mx = base->monitor_size.cx;
            told_my = base->monitor_size.cy;
            nx_log( "[NXGL] the window's surface is %dx%d, the monitor %dx%d%s",
                    told_cx, told_cy, told_mx, told_my,
                    impl_from_opengl_drawable( base )->screen ? "" : ", offscreen" );
        }
    }
}

/* Frames presented and the time inside eglSwapBuffers, for [PROGRESS]. */
extern unsigned long long horizon_interrupt_time(void);
unsigned int wine_nx_gl_swaps;
unsigned long long wine_nx_gl_swap_time;  /* 100 ns */

/* The floating keyboard (horizon-wine/source/osk.c), put into the back
 * buffer just before it is shown: its picture in a texture of each context's
 * own, blitted from a framebuffer of its own onto the window's. Everything
 * the blit and the upload depend on is put back as the program left it. */
struct nx_osk_gl
{
    EGLContext context;
    GLuint texture, framebuffer;
    GLuint menu_program, menu_array;
    GLuint cursor_texture;
    BOOL menu_failed;
    int width, height;
    unsigned int generation;
    void *pixels;
};
static struct nx_osk_gl nx_osk_gl[8];
static unsigned int nx_osk_gl_next;
static pthread_mutex_t nx_osk_gl_lock = PTHREAD_MUTEX_INITIALIZER;
static PFN_glGenFramebuffers p_glGenFramebuffers;
static PFN_glBindFramebuffer p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glBlitFramebuffer p_glBlitFramebuffer;
static PFN_glIsFramebuffer p_glIsFramebuffer;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glCreateShader p_glCreateShader;
static PFN_glShaderSource p_glShaderSource;
static PFN_glCompileShader p_glCompileShader;
static PFN_glGetShaderiv p_glGetShaderiv;
static PFN_glDeleteShader p_glDeleteShader;
static PFN_glCreateProgram p_glCreateProgram;
static PFN_glAttachShader p_glAttachShader;
static PFN_glLinkProgram p_glLinkProgram;
static PFN_glGetProgramiv p_glGetProgramiv;
static PFN_glDeleteProgram p_glDeleteProgram;
static PFN_glUseProgram p_glUseProgram;
static PFN_glIsProgram p_glIsProgram;
static PFN_glActiveTexture p_glActiveTexture;
static PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
static PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
static PFN_glGenVertexArrays p_glGenVertexArrays;
static PFN_glBindVertexArray p_glBindVertexArray;
static PFN_glIsVertexArray p_glIsVertexArray;
static PFN_glBindSampler p_glBindSampler;
static BOOL nx_menu_gl_loaded, nx_menu_gl_usable, nx_menu_gl_logged;
#define NX_SCREEN_CLEARS 8
static BOOL nx_osk_shown;
static unsigned int nx_screen_clears;

static const char nx_menu_gl_vertex[] =
    "#version 130\n"
    "out vec2 coord;\n"
    "void main()\n"
    "{\n"
    "    vec2 c = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    coord = vec2(c.x, 1.0 - c.y);\n"
    "    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char nx_menu_gl_fragment[] =
    "#version 130\n"
    "in vec2 coord;\n"
    "out vec4 color;\n"
    "uniform sampler2D menu;\n"
    "void main()\n"
    "{\n"
    "    color = texture(menu, coord);\n"
    "}\n";

static BOOL nx_menu_gl_load( void )
{
    if (nx_menu_gl_loaded) return nx_menu_gl_usable;
    nx_menu_gl_loaded = TRUE;
    p_glCreateShader = (void *)funcs->p_eglGetProcAddress( "glCreateShader" );
    p_glShaderSource = (void *)funcs->p_eglGetProcAddress( "glShaderSource" );
    p_glCompileShader = (void *)funcs->p_eglGetProcAddress( "glCompileShader" );
    p_glGetShaderiv = (void *)funcs->p_eglGetProcAddress( "glGetShaderiv" );
    p_glDeleteShader = (void *)funcs->p_eglGetProcAddress( "glDeleteShader" );
    p_glCreateProgram = (void *)funcs->p_eglGetProcAddress( "glCreateProgram" );
    p_glAttachShader = (void *)funcs->p_eglGetProcAddress( "glAttachShader" );
    p_glLinkProgram = (void *)funcs->p_eglGetProcAddress( "glLinkProgram" );
    p_glGetProgramiv = (void *)funcs->p_eglGetProcAddress( "glGetProgramiv" );
    p_glDeleteProgram = (void *)funcs->p_eglGetProcAddress( "glDeleteProgram" );
    p_glUseProgram = (void *)funcs->p_eglGetProcAddress( "glUseProgram" );
    p_glIsProgram = (void *)funcs->p_eglGetProcAddress( "glIsProgram" );
    p_glActiveTexture = (void *)funcs->p_eglGetProcAddress( "glActiveTexture" );
    p_glBlendFuncSeparate = (void *)funcs->p_eglGetProcAddress( "glBlendFuncSeparate" );
    p_glBlendEquationSeparate = (void *)funcs->p_eglGetProcAddress( "glBlendEquationSeparate" );
    p_glGenVertexArrays = (void *)funcs->p_eglGetProcAddress( "glGenVertexArrays" );
    p_glBindVertexArray = (void *)funcs->p_eglGetProcAddress( "glBindVertexArray" );
    p_glIsVertexArray = (void *)funcs->p_eglGetProcAddress( "glIsVertexArray" );
    p_glBindSampler = (void *)funcs->p_eglGetProcAddress( "glBindSampler" );
    nx_menu_gl_usable = p_glCreateShader && p_glShaderSource && p_glCompileShader && p_glGetShaderiv &&
                        p_glDeleteShader && p_glCreateProgram && p_glAttachShader && p_glLinkProgram &&
                        p_glGetProgramiv && p_glDeleteProgram && p_glUseProgram && p_glIsProgram &&
                        p_glActiveTexture && p_glBlendFuncSeparate && p_glBlendEquationSeparate &&
                        p_glGenVertexArrays && p_glBindVertexArray && p_glIsVertexArray;
    return nx_menu_gl_usable;
}

static GLuint nx_menu_gl_shader( GLenum type, const char *source )
{
    GLuint shader = p_glCreateShader( type );
    GLint compiled = 0;

    if (!shader) return 0;
    p_glShaderSource( shader, 1, &source, NULL );
    p_glCompileShader( shader );
    p_glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
    if (compiled) return shader;
    p_glDeleteShader( shader );
    return 0;
}

static BOOL nx_menu_gl_prepare( struct nx_osk_gl *osk )
{
    GLuint vertex, fragment, program;
    GLint linked = 0;

    if (osk->menu_failed || !nx_menu_gl_load()) return FALSE;
    if (osk->menu_program && p_glIsProgram( osk->menu_program ) && osk->menu_array &&
        p_glIsVertexArray( osk->menu_array ))
        return TRUE;
    osk->menu_program = osk->menu_array = 0;
    vertex = nx_menu_gl_shader( GL_VERTEX_SHADER, nx_menu_gl_vertex );
    fragment = nx_menu_gl_shader( GL_FRAGMENT_SHADER, nx_menu_gl_fragment );
    program = vertex && fragment ? p_glCreateProgram() : 0;
    if (program)
    {
        p_glAttachShader( program, vertex );
        p_glAttachShader( program, fragment );
        p_glLinkProgram( program );
        p_glGetProgramiv( program, GL_LINK_STATUS, &linked );
        if (!linked) p_glDeleteProgram( program );
    }
    if (vertex) p_glDeleteShader( vertex );
    if (fragment) p_glDeleteShader( fragment );
    if (linked) p_glGenVertexArrays( 1, &osk->menu_array );
    if (!linked || !osk->menu_array)
    {
        osk->menu_failed = TRUE;
        nx_log( "[NXGL] settings menu: the blending program could not be built, it is shown opaque" );
        return FALSE;
    }
    osk->menu_program = program;
    if (!nx_menu_gl_logged)
    {
        nx_menu_gl_logged = TRUE;
        nx_log( "[NXGL] settings menu drawn by the GPU over the frame" );
    }
    return TRUE;
}

static BOOL nx_quad_gl_draw( struct nx_osk_gl *osk, GLuint tex, int x, int y, int w, int h, EGLint height )
{
    GLint program, array, active, texture, sampler = 0, viewport[4];
    GLint src_rgb, dst_rgb, src_alpha, dst_alpha, equation_rgb, equation_alpha;
    GLboolean blend, depth, stencil, cull, discard, mask[4];

    if (!nx_menu_gl_prepare( osk )) return FALSE;

    funcs->p_glGetIntegerv( GL_CURRENT_PROGRAM, &program );
    funcs->p_glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
    funcs->p_glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
    funcs->p_glGetIntegerv( GL_VIEWPORT, viewport );
    funcs->p_glGetIntegerv( GL_BLEND_SRC_RGB, &src_rgb );
    funcs->p_glGetIntegerv( GL_BLEND_DST_RGB, &dst_rgb );
    funcs->p_glGetIntegerv( GL_BLEND_SRC_ALPHA, &src_alpha );
    funcs->p_glGetIntegerv( GL_BLEND_DST_ALPHA, &dst_alpha );
    funcs->p_glGetIntegerv( GL_BLEND_EQUATION_RGB, &equation_rgb );
    funcs->p_glGetIntegerv( GL_BLEND_EQUATION_ALPHA, &equation_alpha );
    funcs->p_glGetBooleanv( GL_COLOR_WRITEMASK, mask );
    blend = funcs->p_glIsEnabled( GL_BLEND );
    depth = funcs->p_glIsEnabled( GL_DEPTH_TEST );
    stencil = funcs->p_glIsEnabled( GL_STENCIL_TEST );
    cull = funcs->p_glIsEnabled( GL_CULL_FACE );
    discard = funcs->p_glIsEnabled( GL_RASTERIZER_DISCARD );
    p_glActiveTexture( GL_TEXTURE0 );
    funcs->p_glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
    if (p_glBindSampler) funcs->p_glGetIntegerv( GL_SAMPLER_BINDING, &sampler );

    p_glUseProgram( osk->menu_program );
    p_glBindVertexArray( osk->menu_array );
    funcs->p_glBindTexture( GL_TEXTURE_2D, tex );
    if (p_glBindSampler) p_glBindSampler( 0, 0 );
    funcs->p_glViewport( x, height - y - h, w, h );
    funcs->p_glEnable( GL_BLEND );
    p_glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
    p_glBlendEquationSeparate( GL_FUNC_ADD, GL_FUNC_ADD );
    funcs->p_glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    funcs->p_glDisable( GL_DEPTH_TEST );
    funcs->p_glDisable( GL_STENCIL_TEST );
    funcs->p_glDisable( GL_CULL_FACE );
    funcs->p_glDisable( GL_RASTERIZER_DISCARD );
    funcs->p_glDrawArrays( GL_TRIANGLES, 0, 3 );

    if (discard) funcs->p_glEnable( GL_RASTERIZER_DISCARD );
    if (cull) funcs->p_glEnable( GL_CULL_FACE );
    if (stencil) funcs->p_glEnable( GL_STENCIL_TEST );
    if (depth) funcs->p_glEnable( GL_DEPTH_TEST );
    if (!blend) funcs->p_glDisable( GL_BLEND );
    funcs->p_glColorMask( mask[0], mask[1], mask[2], mask[3] );
    p_glBlendEquationSeparate( equation_rgb, equation_alpha );
    p_glBlendFuncSeparate( src_rgb, dst_rgb, src_alpha, dst_alpha );
    funcs->p_glViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
    if (p_glBindSampler) p_glBindSampler( 0, sampler );
    funcs->p_glBindTexture( GL_TEXTURE_2D, texture );
    p_glActiveTexture( active );
    p_glBindVertexArray( array );
    p_glUseProgram( program );
    return TRUE;
}

static BOOL nx_menu_gl_draw( struct nx_osk_gl *osk, const struct wine_nx_osk_frame *frame, EGLint height )
{
    return nx_quad_gl_draw( osk, osk->texture, frame->x, frame->y, frame->width, frame->height, height );
}

static void nx_cursor_gl_draw( struct nx_osk_gl *osk, int cx, int cy, EGLint width, EGLint height )
{
    int draw_x, draw_y, draw_w, draw_h;

    if (!wine_nx_cursor_bitmap || !nx_menu_gl_prepare( osk )) return;
    if (!osk->cursor_texture || !funcs->p_glIsTexture( osk->cursor_texture ))
    {
        uint32_t pixels[12 * 19];

        wine_nx_cursor_bitmap( pixels );
        funcs->p_glGenTextures( 1, &osk->cursor_texture );
        funcs->p_glBindTexture( GL_TEXTURE_2D, osk->cursor_texture );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
        funcs->p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 12, 19, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
    }
    draw_x = cx * (int)width / 1280;
    draw_y = cy * (int)height / 720;
    draw_w = width > 1280 ? (12 * (int)width + 640) / 1280 : 12;
    draw_h = height > 720 ? (19 * (int)height + 360) / 720 : 19;
    nx_quad_gl_draw( osk, osk->cursor_texture, draw_x, draw_y, draw_w, draw_h, height );
}

static void nx_osk_draw( struct opengl_drawable *base )
{
    struct wine_nx_osk_frame frame;
    struct nx_osk_gl *osk = NULL;
    EGLint width = 0, height = 0;
    EGLContext context;
    GLint read_fb, draw_fb, texture, unpack_buffer, row_length, skip_pixels, skip_rows, alignment;
    GLboolean scissor, srgb;
    BOOL overlay_visible = wine_nx_osk_visible() || wine_nx_osk_map_visible();
    int cursor_x = 0, cursor_y = 0, cursor_visible = 0;
    BOOL cursor_on = !overlay_visible && wine_nx_cursor_state && wine_nx_cursor_bitmap &&
                     wine_nx_cursor_state( &cursor_x, &cursor_y, &cursor_visible ) && cursor_visible;
    unsigned int i;

    if (!overlay_visible && !cursor_on) return;
    if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) ||
        !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height ) ||
        !(context = funcs->p_eglGetCurrentContext()))
        return;
    if (overlay_visible && !wine_nx_osk_frame( width, height, &frame ))
    {
        overlay_visible = FALSE;
        cursor_on = wine_nx_cursor_state && wine_nx_cursor_bitmap &&
                    wine_nx_cursor_state( &cursor_x, &cursor_y, &cursor_visible ) && cursor_visible;
    }
    if (!overlay_visible && !cursor_on) return;

    pthread_mutex_lock( &nx_osk_gl_lock );
    if (!__atomic_load_n( &p_glBlitFramebuffer, __ATOMIC_ACQUIRE ))
    {
        PFN_glBindFramebuffer bind_fb;
        PFN_glBlitFramebuffer blit;

        p_glGenFramebuffers = (void *)funcs->p_eglGetProcAddress( "glGenFramebuffers" );
        bind_fb = (void *)funcs->p_eglGetProcAddress( "glBindFramebuffer" );
        p_glFramebufferTexture2D = (void *)funcs->p_eglGetProcAddress( "glFramebufferTexture2D" );
        p_glIsFramebuffer = (void *)funcs->p_eglGetProcAddress( "glIsFramebuffer" );
        p_glBindBuffer = (void *)funcs->p_eglGetProcAddress( "glBindBuffer" );
        blit = (void *)funcs->p_eglGetProcAddress( "glBlitFramebuffer" );
        if (!p_glGenFramebuffers || !bind_fb || !p_glFramebufferTexture2D || !p_glIsFramebuffer ||
            !p_glBindBuffer || !blit)
        {
            pthread_mutex_unlock( &nx_osk_gl_lock );
            return;
        }
        __atomic_store_n( &p_glBindFramebuffer, bind_fb, __ATOMIC_RELEASE );
        __atomic_store_n( &p_glBlitFramebuffer, blit, __ATOMIC_RELEASE );
    }
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

    p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

    if (overlay_visible)
    {
        /* Names the program deleted, or a context that took an old one's place. */
        if (!osk->texture || !funcs->p_glIsTexture( osk->texture ) || !p_glIsFramebuffer( osk->framebuffer ))
        {
            funcs->p_glGenTextures( 1, &osk->texture );
            p_glGenFramebuffers( 1, &osk->framebuffer );
            osk->width = osk->height = 0;
        }
        funcs->p_glBindTexture( GL_TEXTURE_2D, osk->texture );
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
            BOOL drawn;

            p_glBindFramebuffer( GL_READ_FRAMEBUFFER, osk->framebuffer );
            p_glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, osk->texture, 0 );
            p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
            if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
            if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
            drawn = wine_nx_osk_translucent && wine_nx_osk_translucent() && nx_menu_gl_draw( osk, &frame, height );
            /* The picture's first row is its top; the window's first is its bottom. */
            if (!drawn)
                p_glBlitFramebuffer( 0, 0, frame.width, frame.height, frame.x, height - frame.y,
                                     frame.x + frame.width, height - frame.y - frame.height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
            if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
            if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
            __atomic_store_n( &nx_osk_shown, TRUE, __ATOMIC_RELEASE );
        }
    }
    else if (cursor_on)
    {
        p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
        if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
        if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
        nx_cursor_gl_draw( osk, cursor_x, cursor_y, width, height );
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
    pthread_mutex_unlock( &nx_osk_gl_lock );
}

static void nx_screen_clear( void )
{
    GLint draw_fb, draw_buffer;
    GLfloat color[4];
    GLboolean mask[4], scissor;

    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb );
    funcs->p_glGetFloatv( GL_COLOR_CLEAR_VALUE, color );
    funcs->p_glGetBooleanv( GL_COLOR_WRITEMASK, mask );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
    funcs->p_glGetIntegerv( GL_DRAW_BUFFER, &draw_buffer );
    funcs->p_glDrawBuffer( GL_BACK );
    if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
    funcs->p_glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    funcs->p_glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    funcs->p_glClear( GL_COLOR_BUFFER_BIT );
    funcs->p_glClearColor( color[0], color[1], color[2], color[3] );
    funcs->p_glColorMask( mask[0], mask[1], mask[2], mask[3] );
    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
    funcs->p_glDrawBuffer( draw_buffer );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fb );
}

static BOOL nx_drawable_swap( struct opengl_drawable *base )
{
    unsigned long long start;
    BOOL ret;

    if (!impl_from_opengl_drawable( base )->screen)
        return funcs->p_eglSwapBuffers( egl->display, base->surface );
    __atomic_store_n( &nx_osk_shown, FALSE, __ATOMIC_RELAXED );
    nx_osk_draw( base );
    start = horizon_interrupt_time();
    ret = funcs->p_eglSwapBuffers( egl->display, base->surface );
    __atomic_add_fetch( &wine_nx_gl_swap_time, horizon_interrupt_time() - start, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_gl_swaps, 1, __ATOMIC_RELAXED );
    if (__atomic_load_n( &nx_osk_shown, __ATOMIC_ACQUIRE ))
        __atomic_store_n( &nx_screen_clears, NX_SCREEN_CLEARS, __ATOMIC_RELAXED );
    if (ret && __atomic_load_n( &p_glBindFramebuffer, __ATOMIC_ACQUIRE ))
    {
        unsigned int clears = __atomic_load_n( &nx_screen_clears, __ATOMIC_RELAXED );

        while (clears && !__atomic_compare_exchange_n( &nx_screen_clears, &clears, clears - 1, 1,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED ))
            ;
        if (clears) nx_screen_clear();
    }
    return ret;
}

/* The first frame of a surface that takes the screen while the loading screen
 * or the floating keyboard is up: drawn with the context this thread has
 * current, the program's, and skipped when it has none. It used to be drawn
 * with a context of its own, made, drawn with and destroyed, and on mesa-switch
 * a context that drew must not be destroyed while a program runs. nvc0 gives
 * every context of the screen the screen's one pushbuffer, and keeps a
 * context's buffer lists linked to it from one submission to the next
 * (NOUVEAU_SWITCH_INCREMENTAL_REFS, its default). nvc0_destroy frees them while
 * they are still linked (nouveau_bufctx_del), and the submissions after it
 * write through them into whatever the heap gives that memory to next. Vampire:
 * The Masquerade - Redemption takes the screen with the loading screen up, and
 * froze about a minute later on "corrupt or oversized bufctx list" in every
 * build with this frame. Upstream Autorun has no first frame, so no context
 * that drew is destroyed there while a game runs. This fork's loading screen
 * (carafe_loading.c) shows through the floating keyboard, so every program
 * that takes the screen while it is up gets this frame. The program's context
 * is left as it was found: nx_screen_clear and nx_osk_draw put back what they
 * change. */
static void nx_screen_first_frame( struct nx_gl_drawable *gl, int format )
{
    EGLContext context = funcs->p_eglGetCurrentContext();
    EGLSurface draw = funcs->p_eglGetCurrentSurface( EGL_DRAW ), read = funcs->p_eglGetCurrentSurface( EGL_READ );
    PFN_glBindFramebuffer bind_fb;

    (void)format;
    if (!wine_nx_osk_visible()) return;
    if (!context)
    {
        nx_log( "[NXGL] first frame: no context current on this thread; the screen keeps its last picture" );
        return;
    }
    if (!__atomic_load_n( &p_glBindFramebuffer, __ATOMIC_ACQUIRE ) &&
        (bind_fb = (void *)funcs->p_eglGetProcAddress( "glBindFramebuffer" )))
        __atomic_store_n( &p_glBindFramebuffer, bind_fb, __ATOMIC_RELEASE );
    if (!__atomic_load_n( &p_glBindFramebuffer, __ATOMIC_ACQUIRE ) ||
        !funcs->p_eglMakeCurrent( egl->display, gl->base.surface, gl->base.surface, context ))
    {
        nx_log( "[NXGL] first frame: the thread's context cannot draw to the new surface, error %#x",
                funcs->p_eglGetError() );
        return;
    }
    nx_screen_clear();
    nx_osk_draw( &gl->base );
    nx_log( "[NXGL] first frame: the floating keyboard's picture, drawn with the thread's context, swap %s",
            funcs->p_eglSwapBuffers( egl->display, gl->base.surface ) ? "done" : "failed" );
    if (!funcs->p_eglMakeCurrent( egl->display, draw, read, context ))
        nx_log( "[NXGL] first frame: the thread's context could not be made current again, error %#x",
                funcs->p_eglGetError() );
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
    if (!previous) nx_screen_first_frame( gl, format );

    TRACE( "created drawable %s with EGL surface %p\n", debugstr_opengl_drawable( &gl->base ), gl->base.surface );
    {
        static BOOL logged;
        const char *vendor = funcs->p_eglQueryString( egl->display, EGL_VENDOR );
        const char *version = funcs->p_eglQueryString( egl->display, EGL_VERSION );

        EGLConfig config = nx_config_for_format( format );
        EGLint id = 0, r = 0, g = 0, b = 0, a = 0, depth = 0, stencil = 0, samples = 0;

        /* raw 0: win32u draws through its framebuffer surface (DPI scaling or gamma).
         * What the format has, too: a game that draws in passes over an equal
         * depth, as Doom 3 does, shows stripes and patches on a shallow depth
         * buffer, and the log is the only place that says which one it got. */
        if (!logged)
        {
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_CONFIG_ID, &id );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_RED_SIZE, &r );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_GREEN_SIZE, &g );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_BLUE_SIZE, &b );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_ALPHA_SIZE, &a );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_DEPTH_SIZE, &depth );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_STENCIL_SIZE, &stencil );
            funcs->p_eglGetConfigAttrib( egl->display, config, EGL_SAMPLES, &samples );
            nx_log( "[NXGL] EGL %s %s, %u configs; window surface for format %d, raw %u; "
                    "config %d: rgba %d%d%d%d, depth %d, stencil %d, samples %d",
                    vendor ? vendor : "?", version ? version : "?", egl->config_count, format, client->raw,
                    id, r, g, b, a, depth, stencil, samples );
        }
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
