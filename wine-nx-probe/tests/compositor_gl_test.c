/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Host test of the compositor's drawing (source/compositor_gl.c) on the Mac's
 * OpenGL 3.2 core, into an offscreen framebuffer read back pixel by pixel:
 *   clang -Wall -Wextra -Werror -I source -framework OpenGL \
 *       -o compositor-gl-test tests/compositor_gl_test.c source/compositor_gl.c
 *   ./compositor-gl-test */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <stdlib.h>

#include "compositor_gl.h"

#define SCREEN_W 1280
#define SCREEN_H 720

static const struct compositor_gl_funcs funcs =
{
#define USE_GL_FUNC( ret, name, args ) .name = gl##name,
    COMPOSITOR_GL_FUNCS
#undef USE_GL_FUNC
};

static int failures, checks;

/* The screen pixel at x, y (top-left origin) as 0xRRGGBB. */
static uint32_t screen_pixel( int x, int y )
{
    unsigned char rgba[4];

    glReadPixels( x, SCREEN_H - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
    return (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
}

static void check_pixel( const char *what, int x, int y, uint32_t expect )
{
    uint32_t got = screen_pixel( x, y );

    checks++;
    if (got == expect) return;
    printf( "FAIL %s: pixel %d,%d is %06x, expected %06x\n", what, x, y, got, expect );
    failures++;
}

static void check_gl( const char *what )
{
    GLenum error = glGetError();

    checks++;
    if (error == GL_NO_ERROR) return;
    printf( "FAIL %s: OpenGL error 0x%x\n", what, error );
    failures++;
}

static uint32_t *filled( int width, int height, uint32_t value )
{
    uint32_t *pixels = malloc( sizeof(*pixels) * width * height );
    int i;

    for (i = 0; i < width * height; i++) pixels[i] = value;
    return pixels;
}

int main(void)
{
    static const CGLPixelFormatAttribute attribs[] =
    {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, 0
    };
    struct compositor_gl_texture red = {0}, green = {0}, edge = {0}, ramp = {0}, padded = {0}, white = {0};
    struct compositor_gl_quad quads[4];
    struct compositor_gl comp;
    CGLPixelFormatObj format;
    CGLContextObj context;
    GLuint framebuffer, target;
    uint32_t *pixels;
    GLint count;
    int x, y;

    if (CGLChoosePixelFormat( attribs, &format, &count ) || !format ||
        CGLCreateContext( format, NULL, &context ) || CGLSetCurrentContext( context ))
    {
        printf( "FAIL no OpenGL 3.2 core context\n" );
        return 1;
    }
    glGenFramebuffers( 1, &framebuffer );
    glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
    glGenTextures( 1, &target );
    glBindTexture( GL_TEXTURE_2D, target );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0 );
    if (glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE)
    {
        printf( "FAIL offscreen framebuffer incomplete\n" );
        return 1;
    }
    printf( "renderer: %s, %s\n", glGetString( GL_RENDERER ), glGetString( GL_VERSION ) );

    if (compositor_gl_init( &comp, &funcs, SCREEN_W, SCREEN_H ))
    {
        printf( "FAIL compositor_gl_init: %s\n", comp.error );
        return 1;
    }
    check_gl( "init" );

    /* Stacking: the later quad covers the earlier one, and the rest stays black. */
    pixels = filled( 128, 128, 0x00ff0000 );
    compositor_gl_upload( &comp, &red, 128, 128, pixels, 128, 0, 0, 128, 128 );
    free( pixels );
    pixels = filled( 64, 64, 0x0000ff00 );
    compositor_gl_upload( &comp, &green, 64, 64, pixels, 64, 0, 0, 64, 64 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &red, 100, 100, 128, 128, 0, 0, 0, 0 };
    quads[1] = (struct compositor_gl_quad){ &green, 150, 150, 64, 64, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 2, 0, 0, 0 );
    check_gl( "draw" );
    check_pixel( "background", 99, 99, 0x000000 );
    check_pixel( "bottom window", 110, 110, 0xff0000 );
    check_pixel( "top window", 150, 150, 0x00ff00 );
    check_pixel( "top window corner", 213, 213, 0x00ff00 );
    check_pixel( "bottom window past the top one", 214, 214, 0xff0000 );
    check_pixel( "bottom window corner", 227, 227, 0xff0000 );
    check_pixel( "past the bottom window", 228, 228, 0x000000 );
    quads[0] = (struct compositor_gl_quad){ &green, 150, 150, 64, 64, 0, 0, 0, 0 };
    quads[1] = (struct compositor_gl_quad){ &red, 100, 100, 128, 128, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 2, 0, 0, 0 );
    check_pixel( "order reversed", 160, 160, 0xff0000 );

    /* Channels: BGRX 0x00123456 is red 12, green 34, blue 56. Only the visible
     * part is drawn, not the white rest of a larger surface allocation. */
    pixels = filled( 128, 128, 0x00ffffff );
    for (y = 0; y < 128; y++) for (x = 0; x < 100; x++) pixels[y * 128 + x] = 0x00123456;
    compositor_gl_upload( &comp, &edge, 128, 128, pixels, 128, 0, 0, 128, 128 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &edge, 400, 100, 100, 128, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 1, 0, 0, 0 );
    check_pixel( "channel order", 450, 150, 0x123456 );
    check_pixel( "visible edge", 499, 150, 0x123456 );
    check_pixel( "allocation past the visible part", 500, 150, 0x000000 );

    /* Partial upload: only the dirty rectangle changes. */
    pixels = filled( 128, 128, 0x00ff0000 );
    for (y = 10; y < 40; y++) for (x = 10; x < 40; x++) pixels[y * 128 + x] = 0x000000ff;
    compositor_gl_upload( &comp, &red, 128, 128, pixels, 128, 10, 10, 20, 20 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &red, 100, 100, 128, 128, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 1, 0, 0, 0 );
    check_pixel( "dirty rectangle", 115, 115, 0x0000ff );
    check_pixel( "dirty rectangle edge", 119, 119, 0x0000ff );
    check_pixel( "changed pixels outside it", 125, 125, 0xff0000 );

    /* A source offset and a window partly off the left edge: red holds x. */
    pixels = filled( 256, 64, 0 );
    for (y = 0; y < 64; y++) for (x = 0; x < 256; x++) pixels[y * 256 + x] = (uint32_t)x << 16 | (uint32_t)y;
    compositor_gl_upload( &comp, &ramp, 256, 64, pixels, 256, 0, 0, 256, 64 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &ramp, -20, 300, 100, 50, 0, 0, 0, 0 };
    quads[1] = (struct compositor_gl_quad){ &ramp, 600, 300, 50, 50, 30, 5, 0, 0 };
    compositor_gl_draw( &comp, quads, 2, 0, 0, 0 );
    check_pixel( "off the left edge", 0, 310, 0x14000a );
    check_pixel( "off the left edge, right side", 79, 349, 0x630031 );
    check_pixel( "source offset", 600, 300, 0x1e0005 );
    check_pixel( "source offset corner", 649, 349, 0x4f0036 );

    /* Rows wider than the texture, as a stride. */
    pixels = filled( 200, 50, 0x00ffffff );
    for (y = 0; y < 50; y++) for (x = 0; x < 100; x++) pixels[y * 200 + x] = (uint32_t)y << 8;
    compositor_gl_upload( &comp, &padded, 100, 50, pixels, 200, 0, 0, 100, 50 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &padded, 800, 400, 100, 50, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 1, 0, 0, 0 );
    check_pixel( "stride row 0", 850, 400, 0x000000 );
    check_pixel( "stride row 49", 899, 449, 0x003100 );

    /* New storage when the size changes. */
    pixels = filled( 32, 16, 0x00abcdef );
    compositor_gl_upload( &comp, &red, 32, 16, pixels, 32, 0, 0, 32, 16 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &red, 10, 10, 32, 16, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 1, 0, 0, 0 );
    check_gl( "resize" );
    check_pixel( "resized texture", 41, 25, 0xabcdef );

    /* The pointer: tip black, fill white, clear parts show the window. */
    pixels = filled( 200, 200, 0x00ff0000 );
    compositor_gl_upload( &comp, &white, 200, 200, pixels, 200, 0, 0, 200, 200 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &white, 650, 450, 200, 200, 0, 0, 0, 0 };
    compositor_gl_draw( &comp, quads, 1, 700, 500, 1 );
    check_gl( "pointer" );
    check_pixel( "pointer tip", 700, 500, 0x000000 );
    check_pixel( "pointer fill", 701, 502, 0xffffff );
    check_pixel( "pointer clear part", 705, 500, 0xff0000 );
    check_pixel( "pointer tail", 707, 518, 0x000000 );
    check_pixel( "pointer tail clear", 700, 518, 0xff0000 );
    compositor_gl_draw( &comp, quads, 1, 700, 500, 0 );
    check_pixel( "pointer hidden", 700, 500, 0xff0000 );

    /* Stretch a 64x32 texture across 128x64 of the screen (src size != dest). */
    pixels = filled( 64, 32, 0x0000ff00 );
    compositor_gl_upload( &comp, &green, 64, 32, pixels, 64, 0, 0, 64, 32 );
    free( pixels );
    quads[0] = (struct compositor_gl_quad){ &green, 0, 0, 128, 64, 0, 0, 64, 32 };
    compositor_gl_draw( &comp, quads, 1, 0, 0, 0 );
    check_gl( "stretch" );
    check_pixel( "stretched origin", 0, 0, 0x00ff00 );
    check_pixel( "stretched far", 120, 50, 0x00ff00 );
    check_pixel( "outside stretch", 200, 200, 0x000000 );

    compositor_gl_release( &comp, &red );
    compositor_gl_release( &comp, &green );
    compositor_gl_release( &comp, &edge );
    compositor_gl_release( &comp, &ramp );
    compositor_gl_release( &comp, &padded );
    compositor_gl_release( &comp, &white );
    compositor_gl_destroy( &comp );
    check_gl( "release" );

    printf( "%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures );
    return failures != 0;
}
