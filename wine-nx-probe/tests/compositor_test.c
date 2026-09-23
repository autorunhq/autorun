/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Host test of the screen compositor (source/compositor.c): its presenter
 * thread on a Mac OpenGL backend that reads every presented frame back, driven
 * the way the display driver drives it, including the screen handed to an
 * OpenGL program and back while other threads keep updating windows:
 *   clang -Wall -Wextra -Werror -I source -framework OpenGL -o compositor-test \
 *       tests/compositor_test.c source/compositor.c source/compositor_gl.c
 *   ./compositor-test */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "compositor.h"
#include "compositor_gl.h"
#include "osk.h"

#define SCREEN_W 1280
#define SCREEN_H 720

static const struct compositor_gl_funcs funcs =
{
#define USE_GL_FUNC( ret, name, args ) .name = gl##name,
    COMPOSITOR_GL_FUNCS
#undef USE_GL_FUNC
};

/* The profiler's thread list (thread_profile.c) is the Switch's. */
void wine_nx_thread_register( char kind, unsigned int tid, void *teb ) { (void)kind; (void)tid; (void)teb; }
void wine_nx_thread_unregister( void ) {}

/* A floating keyboard (osk.c) of one colour, shown when the test says. */
static int keyboard_shown;
static unsigned int keyboard_generation = 1;
static uint32_t keyboard_color;

int wine_nx_osk_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame )
{
    if (!__atomic_load_n( &keyboard_shown, __ATOMIC_ACQUIRE ) || screen_width != SCREEN_W || screen_height != SCREEN_H)
        return 0;
    frame->x = 120;
    frame->y = 120;
    frame->width = frame->height = 40;
    frame->generation = __atomic_load_n( &keyboard_generation, __ATOMIC_ACQUIRE );
    return 1;
}

unsigned int wine_nx_osk_copy( int screen_width, int screen_height, void *pixels, int stride, int rgba )
{
    int x, y;

    (void)screen_width; (void)screen_height; (void)rgba;
    if (!__atomic_load_n( &keyboard_shown, __ATOMIC_ACQUIRE )) return 0;
    for (y = 0; y < 40; y++)
        for (x = 0; x < 40; x++) ((uint32_t *)((char *)pixels + y * stride))[x] = 0xff000000u | keyboard_color;
    return __atomic_load_n( &keyboard_generation, __ATOMIC_ACQUIRE );
}

static void show_keyboard( int shown, uint32_t color )
{
    keyboard_color = color;
    __atomic_add_fetch( &keyboard_generation, 1, __ATOMIC_RELEASE );
    __atomic_store_n( &keyboard_shown, shown, __ATOMIC_RELEASE );
    wine_nx_compositor_redraw();
}

/* What the backend saw, guarded by shot_lock. */
static pthread_mutex_t shot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shot_cond = PTHREAD_COND_INITIALIZER;
static unsigned char shot[SCREEN_W * SCREEN_H * 4], frame_pixels[SCREEN_W * SCREEN_H * 4];
static unsigned int shots, attaches, detaches;
static int attached, swapped_detached;

static CGLContextObj context;
static GLuint framebuffer, target;

static const struct compositor_gl_funcs *host_init( char *error, int size )
{
    static const CGLPixelFormatAttribute attribs[] =
    {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, 0
    };
    CGLPixelFormatObj format;
    GLint count;

    if (CGLChoosePixelFormat( attribs, &format, &count ) || !format || CGLCreateContext( format, NULL, &context ))
    {
        snprintf( error, size, "no OpenGL 3.2 core context" );
        return NULL;
    }
    CGLSetCurrentContext( context );
    return &funcs;
}

static int host_attach( int width, int height, char *error, int size )
{
    CGLSetCurrentContext( context );
    if (!framebuffer)
    {
        glGenFramebuffers( 1, &framebuffer );
        glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
        glGenTextures( 1, &target );
        glBindTexture( GL_TEXTURE_2D, target );
        glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
        glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0 );
        if (glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE)
        {
            snprintf( error, size, "offscreen framebuffer incomplete" );
            return -1;
        }
    }
    glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
    pthread_mutex_lock( &shot_lock );
    attaches++;
    attached = 1;
    pthread_mutex_unlock( &shot_lock );
    return 0;
}

static void host_detach( void )
{
    glFinish();
    CGLSetCurrentContext( NULL );
    pthread_mutex_lock( &shot_lock );
    detaches++;
    attached = 0;
    pthread_cond_broadcast( &shot_cond );
    pthread_mutex_unlock( &shot_lock );
}

static void host_swap( void )
{
    glReadPixels( 0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, frame_pixels );
    pthread_mutex_lock( &shot_lock );
    if (!attached) swapped_detached = 1;
    memcpy( shot, frame_pixels, sizeof(shot) );
    shots++;
    pthread_cond_broadcast( &shot_cond );
    pthread_mutex_unlock( &shot_lock );
}

static void host_log( const char *message )
{
    printf( "%s\n", message );
}

static const struct compositor_backend backend =
{
    .init = host_init,
    .attach = host_attach,
    .detach = host_detach,
    .swap = host_swap,
    .log = host_log,
};

static int failures, checks;

/* With shot_lock held: the last frame's pixel at x, y (top-left origin). */
static uint32_t shot_pixel( int x, int y )
{
    const unsigned char *p = shot + ((size_t)(SCREEN_H - 1 - y) * SCREEN_W + x) * 4;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static void deadline_in( struct timespec *ts, int ms )
{
    struct timeval now;

    gettimeofday( &now, NULL );
    ts->tv_sec = now.tv_sec + ms / 1000;
    ts->tv_nsec = now.tv_usec * 1000 + (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

/* Wait up to two seconds for a presented frame with that pixel. */
static void wait_pixel( const char *what, int x, int y, uint32_t expect )
{
    struct timespec deadline;
    uint32_t got = 0;
    int ok = 0;

    deadline_in( &deadline, 2000 );
    pthread_mutex_lock( &shot_lock );
    for (;;)
    {
        if (shots && (got = shot_pixel( x, y )) == expect) { ok = 1; break; }
        if (pthread_cond_timedwait( &shot_cond, &shot_lock, &deadline )) break;
    }
    pthread_mutex_unlock( &shot_lock );
    checks++;
    if (ok) return;
    printf( "FAIL %s: pixel %d,%d is %06x, expected %06x\n", what, x, y, got, expect );
    failures++;
}

static void check( const char *what, int ok )
{
    checks++;
    if (ok) return;
    printf( "FAIL %s\n", what );
    failures++;
}

static uint32_t *filled( int width, int height, uint32_t value )
{
    uint32_t *pixels = malloc( sizeof(*pixels) * width * height );
    int i;

    for (i = 0; i < width * height; i++) pixels[i] = value;
    return pixels;
}

static void fill_layer( struct wine_nx_layer *layer, int width, int height, uint32_t value )
{
    uint32_t *pixels = filled( width, height, value );

    wine_nx_layer_update( layer, pixels, width, 0, 0, width, height );
    free( pixels );
}

/* Two threads update, move and restack their windows as fast as they can. */
static int stress_running;
static struct wine_nx_layer *stress_layers[2];

static void *stress_thread( void *arg )
{
    int index = (int)(intptr_t)arg, n = 0;
    uint32_t *pixels = filled( 64, 64, 0 );

    while (__atomic_load_n( &stress_running, __ATOMIC_RELAXED ))
    {
        int i;

        for (i = 0; i < 64 * 64; i++) pixels[i] = n & 1 ? 0x00ffff00 : 0x0000ffff;
        wine_nx_layer_update( stress_layers[index], pixels, 64, n % 64, 0, 64, 64 );
        wine_nx_layer_place( stress_layers[index], 900 + index * 100 + n % 7, 50, 64, 64 );
        wine_nx_compositor_restack( &stress_layers[index], 1 );
        n++;
    }
    free( pixels );
    return NULL;
}

int main(void)
{
    struct wine_nx_layer *a, *b, *c, *stack[3];
    pthread_t threads[2];
    unsigned int before;
    uint32_t *pixels;
    int x, y, i, ok;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (wine_nx_compositor_start( &backend, SCREEN_W, SCREEN_H ))
    {
        printf( "FAIL compositor did not start\n" );
        return 1;
    }
    check( "running", wine_nx_compositor_running() );

    /* A placed layer shows; a later restack decides which covers which. */
    a = wine_nx_layer_create( 128, 128 );
    b = wine_nx_layer_create( 64, 64 );
    fill_layer( a, 128, 128, 0x00ff0000 );
    fill_layer( b, 64, 64, 0x0000ff00 );
    wine_nx_layer_place( a, 100, 100, 128, 128 );
    wait_pixel( "placed layer", 110, 110, 0xff0000 );
    wait_pixel( "unplaced layer stays hidden", 160, 160, 0xff0000 );
    wine_nx_layer_place( b, 150, 150, 64, 64 );
    stack[0] = b;
    stack[1] = a;
    wine_nx_compositor_restack( stack, 2 );
    wait_pixel( "topmost layer", 160, 160, 0x00ff00 );
    stack[0] = a;
    stack[1] = b;
    wine_nx_compositor_restack( stack, 2 );
    wait_pixel( "restacked", 160, 160, 0xff0000 );
    wine_nx_compositor_restack( &b, 1 );
    wait_pixel( "left out of the stack goes below", 160, 160, 0x00ff00 );
    wine_nx_layer_place( b, 150, 150, 0, 0 );
    wait_pixel( "hidden layer", 160, 160, 0xff0000 );

    /* Only the visible part of an allocation is drawn. */
    c = wine_nx_layer_create( 128, 128 );
    pixels = filled( 128, 128, 0x00ffffff );
    for (y = 0; y < 128; y++) for (x = 0; x < 100; x++) pixels[y * 128 + x] = 0x00123456;
    wine_nx_layer_update( c, pixels, 128, 0, 0, 128, 128 );
    free( pixels );
    wine_nx_layer_place( c, 400, 100, 100, 128 );
    wait_pixel( "visible part", 499, 150, 0x123456 );
    wait_pixel( "allocation past it", 500, 150, 0x000000 );

    /* An update reaches the screen, and only its rectangle is copied. */
    pixels = filled( 128, 128, 0x000000ff );
    wine_nx_layer_update( a, pixels, 128, 10, 10, 20, 20 );
    free( pixels );
    wait_pixel( "updated rectangle", 115, 115, 0x0000ff );
    wait_pixel( "outside the rectangle", 125, 125, 0xff0000 );

    /* A destroyed layer goes away. */
    wine_nx_layer_destroy( c );
    wait_pixel( "destroyed layer", 450, 150, 0x000000 );

    /* The pointer. */
    wine_nx_compositor_cursor( 700, 500, 1 );
    wait_pixel( "pointer fill", 701, 502, 0xffffff );
    wine_nx_compositor_cursor( 700, 500, 0 );
    wait_pixel( "pointer hidden", 701, 502, 0x000000 );

    /* The floating keyboard goes over the windows, takes a new picture when
     * it changes, and leaves them as they were when it goes. */
    show_keyboard( 1, 0x00aabbcc );
    wait_pixel( "keyboard over a window", 130, 130, 0xaabbcc );
    show_keyboard( 1, 0x00445566 );
    wait_pixel( "keyboard redrawn", 130, 130, 0x445566 );
    show_keyboard( 0, 0 );
    wait_pixel( "keyboard gone", 130, 130, 0xff0000 );

    /* Suspended: the screen is given up at once, and nothing is drawn until the
     * resume, which brings the changes made meanwhile. */
    wine_nx_compositor_suspend();
    pthread_mutex_lock( &shot_lock );
    ok = !attached && detaches == 1;
    before = shots;
    pthread_mutex_unlock( &shot_lock );
    check( "suspend returns with the screen given up", ok );
    fill_layer( a, 128, 128, 0x00ff00ff );
    usleep( 200000 );
    pthread_mutex_lock( &shot_lock );
    ok = shots == before;
    pthread_mutex_unlock( &shot_lock );
    check( "no frames while suspended", ok );
    wine_nx_compositor_resume();
    wait_pixel( "change made while suspended", 140, 140, 0xff00ff );
    pthread_mutex_lock( &shot_lock );
    ok = attaches == 2;
    pthread_mutex_unlock( &shot_lock );
    check( "screen taken back once", ok );

    /* Suspends and resumes against two threads updating windows. */
    stress_layers[0] = wine_nx_layer_create( 64, 64 );
    stress_layers[1] = wine_nx_layer_create( 64, 64 );
    stress_running = 1;
    pthread_create( &threads[0], NULL, stress_thread, (void *)(intptr_t)0 );
    pthread_create( &threads[1], NULL, stress_thread, (void *)(intptr_t)1 );
    for (i = 0; i < 50; i++)
    {
        wine_nx_compositor_suspend();
        pthread_mutex_lock( &shot_lock );
        ok = !attached;
        pthread_mutex_unlock( &shot_lock );
        if (!ok) break;
        usleep( 2000 );
        wine_nx_compositor_resume();
        usleep( 3000 );
    }
    check( "every suspend gave the screen up", ok );
    stress_running = 0;
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );
    pthread_mutex_lock( &shot_lock );
    ok = !swapped_detached;
    pthread_mutex_unlock( &shot_lock );
    check( "never presented without the screen", ok );
    wine_nx_layer_destroy( stress_layers[0] );
    wine_nx_layer_destroy( stress_layers[1] );
    fill_layer( a, 128, 128, 0x00336699 );
    wait_pixel( "drawing after the stress", 140, 140, 0x336699 );
    wait_pixel( "stress layers destroyed", 920, 60, 0x000000 );
    printf( "frames presented: %u, screen given up %u times\n", wine_nx_compositor_frames(), detaches );

    printf( "%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures );
    return failures != 0;
}
