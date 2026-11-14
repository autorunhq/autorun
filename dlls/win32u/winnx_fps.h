/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Autorun's frame rate counter over a game drawn through OpenGL
 * (winnx_opengl.c, nx_fps_gl_draw). The only frame rate a game's settings
 * could show was DXVK's HUD, and a game drawn through OpenGL, Wine's own
 * Direct3D among them (Vampire: The Masquerade - Redemption), never passes
 * through DXVK. The label, such as "60 FPS", is in a 5x7 font of its own drawn
 * at three times its size, light on a dark translucent box, in RGBA bytes
 * (glTexImage2D's GL_RGBA, GL_UNSIGNED_BYTE) with the first row at the top.
 */
#ifndef WINNX_FPS_H
#define WINNX_FPS_H

#include <stdint.h>

#define WINE_NX_FPS_SCALE  3
#define WINE_NX_FPS_PAD    6
#define WINE_NX_FPS_CHARS  8   /* "9999 FPS" */
#define WINE_NX_FPS_W      (2 * WINE_NX_FPS_PAD + WINE_NX_FPS_CHARS * 6 * WINE_NX_FPS_SCALE - WINE_NX_FPS_SCALE)
#define WINE_NX_FPS_H      (2 * WINE_NX_FPS_PAD + 7 * WINE_NX_FPS_SCALE)
/* As a little-endian word: alpha in the top byte, red in the bottom one. */
#define WINE_NX_FPS_BOX    0xb0000000u  /* black, a little translucent */
#define WINE_NX_FPS_INK    0xff60ffffu  /* pale yellow */

/* A character's seven rows, the leftmost of its five columns in bit 4; NULL
 * for a space. */
static inline const unsigned char *wine_nx_fps_glyph( char c )
{
    static const unsigned char digits[10][7] =
    {
        { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },
        { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
        { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
        { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },
        { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
        { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
        { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },
        { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
        { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },
    };
    static const unsigned char f[7] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 };
    static const unsigned char p[7] = { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 };
    static const unsigned char s[7] = { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e };

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == 'F') return f;
    if (c == 'P') return p;
    if (c == 'S') return s;
    return NULL;
}

/* "fps FPS" into pixels, WINE_NX_FPS_W by WINE_NX_FPS_H; the box is as wide
 * as the text, which width returns, and the rest is left clear. */
static inline unsigned int wine_nx_fps_draw( unsigned int fps, uint32_t *pixels )
{
    char text[WINE_NX_FPS_CHARS], digits[4];
    unsigned int len = 0, n = 0, width, i, x, y, row, col;

    if (fps > 9999) fps = 9999;
    do digits[n++] = '0' + fps % 10; while ((fps /= 10));
    while (n) text[len++] = digits[--n];
    text[len++] = ' ';
    text[len++] = 'F';
    text[len++] = 'P';
    text[len++] = 'S';
    width = 2 * WINE_NX_FPS_PAD + len * 6 * WINE_NX_FPS_SCALE - WINE_NX_FPS_SCALE;

    for (y = 0; y < WINE_NX_FPS_H; y++)
        for (x = 0; x < WINE_NX_FPS_W; x++)
            pixels[y * WINE_NX_FPS_W + x] = x < width ? WINE_NX_FPS_BOX : 0;
    for (i = 0; i < len; i++)
    {
        const unsigned char *glyph = wine_nx_fps_glyph( text[i] );

        if (!glyph) continue;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
            {
                if (!(glyph[row] & (0x10 >> col))) continue;
                for (y = 0; y < WINE_NX_FPS_SCALE; y++)
                    for (x = 0; x < WINE_NX_FPS_SCALE; x++)
                        pixels[(WINE_NX_FPS_PAD + row * WINE_NX_FPS_SCALE + y) * WINE_NX_FPS_W +
                               WINE_NX_FPS_PAD + (i * 6 + col) * WINE_NX_FPS_SCALE + x] = WINE_NX_FPS_INK;
            }
    }
    return width;
}

#endif /* WINNX_FPS_H */
