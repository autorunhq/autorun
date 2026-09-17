/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The compositor's drawing: each window's pixels in a texture, drawn in
 * stacking order with the pointer on top, using OpenGL 3.2 core calls only.
 * The calls go through a table, which the runtime fills from
 * eglGetProcAddress (Mesa's static libraries export no gl* symbols) and the
 * host test (tests/compositor_gl_test.c) from the Mac's OpenGL. */
#ifndef WINE_NX_COMPOSITOR_GL_H
#define WINE_NX_COMPOSITOR_GL_H

#include <stdint.h>

#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#define COMPOSITOR_GL_FUNCS \
    USE_GL_FUNC( void, ActiveTexture, (GLenum) ) \
    USE_GL_FUNC( void, AttachShader, (GLuint, GLuint) ) \
    USE_GL_FUNC( void, BindAttribLocation, (GLuint, GLuint, const GLchar *) ) \
    USE_GL_FUNC( void, BindBuffer, (GLenum, GLuint) ) \
    USE_GL_FUNC( void, BindTexture, (GLenum, GLuint) ) \
    USE_GL_FUNC( void, BindVertexArray, (GLuint) ) \
    USE_GL_FUNC( void, BlendFunc, (GLenum, GLenum) ) \
    USE_GL_FUNC( void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum) ) \
    USE_GL_FUNC( void, Clear, (GLbitfield) ) \
    USE_GL_FUNC( void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat) ) \
    USE_GL_FUNC( void, CompileShader, (GLuint) ) \
    USE_GL_FUNC( GLuint, CreateProgram, (void) ) \
    USE_GL_FUNC( GLuint, CreateShader, (GLenum) ) \
    USE_GL_FUNC( void, DeleteBuffers, (GLsizei, const GLuint *) ) \
    USE_GL_FUNC( void, DeleteProgram, (GLuint) ) \
    USE_GL_FUNC( void, DeleteShader, (GLuint) ) \
    USE_GL_FUNC( void, DeleteTextures, (GLsizei, const GLuint *) ) \
    USE_GL_FUNC( void, DeleteVertexArrays, (GLsizei, const GLuint *) ) \
    USE_GL_FUNC( void, Disable, (GLenum) ) \
    USE_GL_FUNC( void, DrawArrays, (GLenum, GLint, GLsizei) ) \
    USE_GL_FUNC( void, Enable, (GLenum) ) \
    USE_GL_FUNC( void, EnableVertexAttribArray, (GLuint) ) \
    USE_GL_FUNC( void, GenBuffers, (GLsizei, GLuint *) ) \
    USE_GL_FUNC( void, GenTextures, (GLsizei, GLuint *) ) \
    USE_GL_FUNC( void, GenVertexArrays, (GLsizei, GLuint *) ) \
    USE_GL_FUNC( GLenum, GetError, (void) ) \
    USE_GL_FUNC( void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *) ) \
    USE_GL_FUNC( void, GetProgramiv, (GLuint, GLenum, GLint *) ) \
    USE_GL_FUNC( void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *) ) \
    USE_GL_FUNC( void, GetShaderiv, (GLuint, GLenum, GLint *) ) \
    USE_GL_FUNC( const GLubyte *, GetString, (GLenum) ) \
    USE_GL_FUNC( GLint, GetUniformLocation, (GLuint, const GLchar *) ) \
    USE_GL_FUNC( void, LinkProgram, (GLuint) ) \
    USE_GL_FUNC( void, PixelStorei, (GLenum, GLint) ) \
    USE_GL_FUNC( void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *) ) \
    USE_GL_FUNC( void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) ) \
    USE_GL_FUNC( void, TexParameteri, (GLenum, GLenum, GLint) ) \
    USE_GL_FUNC( void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) ) \
    USE_GL_FUNC( void, Uniform1i, (GLint, GLint) ) \
    USE_GL_FUNC( void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat) ) \
    USE_GL_FUNC( void, UseProgram, (GLuint) ) \
    USE_GL_FUNC( void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) ) \
    USE_GL_FUNC( void, Viewport, (GLint, GLint, GLsizei, GLsizei) )

struct compositor_gl_funcs
{
#define USE_GL_FUNC( ret, name, args ) ret (*name) args;
    COMPOSITOR_GL_FUNCS
#undef USE_GL_FUNC
};

struct compositor_gl_texture
{
    GLuint name;        /* 0 until the first upload */
    int width, height;  /* size of its storage */
};

/* One window on the screen: its pixels from (src_x, src_y) covering src_width
 * by src_height of the texture (0 means the dest size, no stretch), drawn at
 * the screen rectangle x, y, width, height, which may reach past the edges. */
struct compositor_gl_quad
{
    const struct compositor_gl_texture *texture;
    int x, y, width, height;
    int src_x, src_y;
    int src_width, src_height;
};

struct compositor_gl
{
    const struct compositor_gl_funcs *gl;
    int screen_width, screen_height;
    GLuint program, vertex_array, vertex_buffer;
    GLint rect_uniform, src_uniform, window_uniform, image_uniform;
    struct compositor_gl_texture cursor;
    char error[256];  /* why compositor_gl_init failed */
};

/* Build the program and the pointer, with a context current. Returns 0 on
 * success; otherwise comp->error says what failed. */
int compositor_gl_init( struct compositor_gl *comp, const struct compositor_gl_funcs *funcs,
                        int screen_width, int screen_height );
void compositor_gl_destroy( struct compositor_gl *comp );

/* Copy the columns [left, right) of the rows [top, bottom) of pixels (rows of
 * stride pixels, top row first) into the texture. A texture not yet width by
 * height gets new storage first, whose other pixels are undefined. The pixels
 * are uploaded as they are: window pixels are BGRX (0x00RRGGBB), and drawing
 * swaps the channels back. */
void compositor_gl_upload( struct compositor_gl *comp, struct compositor_gl_texture *texture,
                           int width, int height, const uint32_t *pixels, int stride,
                           int left, int top, int right, int bottom );
void compositor_gl_release( struct compositor_gl *comp, struct compositor_gl_texture *texture );

/* Clear the bound framebuffer to black and draw the quads, the first at the
 * bottom, then the pointer with its tip at cursor_x, cursor_y when visible. */
void compositor_gl_draw( struct compositor_gl *comp, const struct compositor_gl_quad *quads, int count,
                         int cursor_x, int cursor_y, int cursor_visible );

#endif
