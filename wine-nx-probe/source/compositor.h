/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The screen compositor. The Switch has one screen, and libnx's framebuffer and
 * an OpenGL surface cannot share it; with the framebuffer, each window's
 * changed pixels were copied straight to the screen, so the last window to
 * draw won and anything drawn over the edge of a window stayed there. Here one
 * OpenGL surface holds the screen, each top-level window is a layer holding a
 * copy of its pixels, and a presenter thread draws the layers in stacking
 * order with the pointer on top whenever something changed.
 *
 * Mesa's nouveau is not safe with two threads drawing at once, so the presenter
 * gives the screen up entirely (no OpenGL calls at all) while a program's
 * OpenGL surface has it: wine_nx_compositor_suspend() and _resume(). */
#ifndef WINE_NX_COMPOSITOR_H
#define WINE_NX_COMPOSITOR_H

#include <stdint.h>

struct compositor_gl_funcs;
struct wine_nx_layer;

/* The platform under the presenter; every call is made on the presenter thread. */
struct compositor_backend
{
    /* Set up the display and a context. Returns the OpenGL calls, or NULL with
     * the reason in error. */
    const struct compositor_gl_funcs *(*init)( char *error, int size );
    /* Put a surface on the screen and make the context current on it. Returns
     * 0, or -1 with the reason in error. */
    int (*attach)( int width, int height, char *error, int size );
    /* Release the context and destroy the surface, giving the screen away. */
    void (*detach)( void );
    /* Gives up the graphics API, for a program that is closing; may be NULL. */
    void (*quit)( void );
    void (*swap)( void );
    void (*log)( const char *message );
};

/* Start the presenter, or wait for the start already under way. Returns 0 when
 * it presents the screen, -1 when it could not (the caller keeps the
 * framebuffer). */
int wine_nx_compositor_start( const struct compositor_backend *backend, int width, int height );
int wine_nx_compositor_running( void );

/* A layer holds width by height BGRX pixels (0x00RRGGBB), black at first, and
 * is hidden until placed. Destroying it may be done from any thread. */
struct wine_nx_layer *wine_nx_layer_create( int width, int height );
void wine_nx_layer_destroy( struct wine_nx_layer *layer );
/* Copy the columns [left, right) of the rows [top, bottom) from pixels, rows of
 * stride pixels laid out like the layer's. */
void wine_nx_layer_update( struct wine_nx_layer *layer, const uint32_t *pixels, int stride,
                           int left, int top, int right, int bottom );
/* Show the layer's top-left width by height pixels with their corner at x, y on
 * the screen; a width or height of 0 hides it. */
void wine_nx_layer_place( struct wine_nx_layer *layer, int x, int y, int width, int height );
/* The stacking order, topmost first. Layers left out go below all of these. */
void wine_nx_compositor_restack( struct wine_nx_layer **layers, int count );

void wine_nx_compositor_cursor( int x, int y, int visible );
/* Draw the screen again: something over the windows, the floating keyboard,
 * changed. */
void wine_nx_compositor_redraw( void );

/* An OpenGL program takes the screen: returns once the presenter has given it
 * up. _resume() lets the presenter take it back. */
/* The frames drawn, read without the presenter's lock: for a watch that cannot
 * afford to wait on it. */
unsigned int wine_nx_compositor_frames_fast( void );
/* Ends the presenter thread and waits for it, before the program closes. */
void wine_nx_compositor_stop( void );
void wine_nx_compositor_suspend( void );
void wine_nx_compositor_resume( void );

/* Frames presented so far, for [PROGRESS]. */
unsigned int wine_nx_compositor_frames( void );

#endif
