/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The compositor's drawing; see compositor_gl.h. */
#include <stdio.h>
#include <string.h>

#include "compositor_gl.h"
#include "pointer_cursor.h"

/* One quad for every window, its corners from a four-vertex buffer and the
 * rectangle and texture coordinates as uniforms. */
static const char vertex_source[] =
    "#version 150\n"
    "in vec2 corner;\n"     /* 0 or 1 in each direction */
    "uniform vec4 rect;\n"  /* left, top, right, bottom in clip space */
    "uniform vec4 src;\n"   /* the texture coordinates at those edges */
    "out vec2 coord;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);\n"
    "    coord = mix(src.xy, src.zw, corner);\n"
    "}\n";

/* Window pixels are BGRX bytes uploaded as RGBA, so they are swapped here
 * rather than converted on every upload; the pointer is real RGBA. */
static const char fragment_source[] =
    "#version 150\n"
    "uniform sampler2D image;\n"
    "uniform int window;\n"
    "in vec2 coord;\n"
    "out vec4 color;\n"
    "void main()\n"
    "{\n"
    "    vec4 texel = texture(image, coord);\n"
    "    color = window != 0 ? vec4(texel.bgr, 1.0) : texel;\n"
    "}\n";

static GLuint compile_shader( struct compositor_gl *comp, GLenum type, const char *source )
{
    const struct compositor_gl_funcs *gl = comp->gl;
    GLuint shader = gl->CreateShader( type );
    GLint status;

    gl->ShaderSource( shader, 1, &source, NULL );
    gl->CompileShader( shader );
    gl->GetShaderiv( shader, GL_COMPILE_STATUS, &status );
    if (status) return shader;
    gl->GetShaderInfoLog( shader, sizeof(comp->error), NULL, comp->error );
    gl->DeleteShader( shader );
    return 0;
}

int compositor_gl_init( struct compositor_gl *comp, const struct compositor_gl_funcs *funcs,
                        int screen_width, int screen_height )
{
    /* The corners of a triangle strip covering the quad. */
    static const GLfloat corners[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    uint32_t cursor[POINTER_CURSOR_W * POINTER_CURSOR_H];
    const struct compositor_gl_funcs *gl = funcs;
    GLuint vertex, fragment;
    GLenum error;
    GLint status;
    int i, j;

    memset( comp, 0, sizeof(*comp) );
    comp->gl = funcs;
    comp->screen_width = screen_width;
    comp->screen_height = screen_height;

    if (!(vertex = compile_shader( comp, GL_VERTEX_SHADER, vertex_source ))) return -1;
    if (!(fragment = compile_shader( comp, GL_FRAGMENT_SHADER, fragment_source )))
    {
        gl->DeleteShader( vertex );
        return -1;
    }
    comp->program = gl->CreateProgram();
    gl->AttachShader( comp->program, vertex );
    gl->AttachShader( comp->program, fragment );
    gl->BindAttribLocation( comp->program, 0, "corner" );
    gl->LinkProgram( comp->program );
    gl->DeleteShader( vertex );
    gl->DeleteShader( fragment );
    gl->GetProgramiv( comp->program, GL_LINK_STATUS, &status );
    if (!status)
    {
        gl->GetProgramInfoLog( comp->program, sizeof(comp->error), NULL, comp->error );
        gl->DeleteProgram( comp->program );
        comp->program = 0;
        return -1;
    }
    comp->rect_uniform = gl->GetUniformLocation( comp->program, "rect" );
    comp->src_uniform = gl->GetUniformLocation( comp->program, "src" );
    comp->window_uniform = gl->GetUniformLocation( comp->program, "window" );
    comp->image_uniform = gl->GetUniformLocation( comp->program, "image" );

    gl->GenVertexArrays( 1, &comp->vertex_array );
    gl->BindVertexArray( comp->vertex_array );
    gl->GenBuffers( 1, &comp->vertex_buffer );
    gl->BindBuffer( GL_ARRAY_BUFFER, comp->vertex_buffer );
    gl->BufferData( GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW );
    gl->VertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, NULL );
    gl->EnableVertexAttribArray( 0 );
    gl->BindVertexArray( 0 );

    for (j = 0; j < POINTER_CURSOR_H; j++)
    {
        for (i = 0; i < POINTER_CURSOR_W; i++)
        {
            char shape = pointer_cursor_shape[j][i];
            /* RGBA bytes, little-endian: opaque white, opaque black or clear. */
            cursor[j * POINTER_CURSOR_W + i] = shape == 'W' ? 0xffffffffu : shape == 'B' ? 0xff000000u : 0;
        }
    }
    compositor_gl_upload( comp, &comp->cursor, POINTER_CURSOR_W, POINTER_CURSOR_H, cursor, POINTER_CURSOR_W,
                          0, 0, POINTER_CURSOR_W, POINTER_CURSOR_H );

    if ((error = gl->GetError()) != GL_NO_ERROR)
    {
        snprintf( comp->error, sizeof(comp->error), "OpenGL error 0x%x while setting up", error );
        compositor_gl_destroy( comp );
        return -1;
    }
    return 0;
}

void compositor_gl_destroy( struct compositor_gl *comp )
{
    const struct compositor_gl_funcs *gl = comp->gl;

    compositor_gl_release( comp, &comp->cursor );
    if (comp->vertex_buffer) gl->DeleteBuffers( 1, &comp->vertex_buffer );
    if (comp->vertex_array) gl->DeleteVertexArrays( 1, &comp->vertex_array );
    if (comp->program) gl->DeleteProgram( comp->program );
    comp->vertex_buffer = 0;
    comp->vertex_array = 0;
    comp->program = 0;
}

void compositor_gl_upload( struct compositor_gl *comp, struct compositor_gl_texture *texture,
                           int width, int height, const uint32_t *pixels, int stride,
                           int left, int top, int right, int bottom )
{
    const struct compositor_gl_funcs *gl = comp->gl;

    if (width <= 0 || height <= 0) return;
    if (!texture->name) gl->GenTextures( 1, &texture->name );
    gl->BindTexture( GL_TEXTURE_2D, texture->name );
    if (texture->width != width || texture->height != height)
    {
        /* One level, sampled texel for pixel. The parameters go first: Mesa
         * gives a texture whose filter still wants mipmaps a whole mipmap
         * chain when its first level is defined. */
        gl->TexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
        gl->TexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        gl->TexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        gl->TexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
        gl->TexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
        gl->TexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
        texture->width = width;
        texture->height = height;
    }

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (left >= right || top >= bottom) return;

    gl->PixelStorei( GL_UNPACK_ALIGNMENT, 4 );
    gl->PixelStorei( GL_UNPACK_ROW_LENGTH, stride );
    gl->TexSubImage2D( GL_TEXTURE_2D, 0, left, top, right - left, bottom - top, GL_RGBA, GL_UNSIGNED_BYTE,
                       pixels + (size_t)top * stride + left );
    gl->PixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
}

void compositor_gl_release( struct compositor_gl *comp, struct compositor_gl_texture *texture )
{
    if (texture->name) comp->gl->DeleteTextures( 1, &texture->name );
    memset( texture, 0, sizeof(*texture) );
}

static void draw_quad( struct compositor_gl *comp, const struct compositor_gl_texture *texture,
                       int x, int y, int width, int height, int src_x, int src_y,
                       int src_width, int src_height )
{
    const struct compositor_gl_funcs *gl = comp->gl;
    float sw = (float)comp->screen_width, sh = (float)comp->screen_height;
    float tw = (float)texture->width, th = (float)texture->height;

    if (src_width <= 0) src_width = width;
    if (src_height <= 0) src_height = height;
    gl->Uniform4f( comp->rect_uniform, 2.0f * x / sw - 1.0f, 1.0f - 2.0f * y / sh,
                   2.0f * (x + width) / sw - 1.0f, 1.0f - 2.0f * (y + height) / sh );
    gl->Uniform4f( comp->src_uniform, src_x / tw, src_y / th,
                   (src_x + src_width) / tw, (src_y + src_height) / th );
    gl->BindTexture( GL_TEXTURE_2D, texture->name );
    gl->DrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
}

void compositor_gl_draw( struct compositor_gl *comp, const struct compositor_gl_quad *quads, int count,
                         int cursor_x, int cursor_y, int cursor_visible )
{
    const struct compositor_gl_funcs *gl = comp->gl;
    int i;

    gl->Viewport( 0, 0, comp->screen_width, comp->screen_height );
    gl->ClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    gl->Clear( GL_COLOR_BUFFER_BIT );
    gl->UseProgram( comp->program );
    gl->BindVertexArray( comp->vertex_array );
    gl->ActiveTexture( GL_TEXTURE0 );
    gl->Uniform1i( comp->image_uniform, 0 );

    gl->Disable( GL_BLEND );
    gl->Uniform1i( comp->window_uniform, 1 );
    for (i = 0; i < count; i++)
    {
        const struct compositor_gl_quad *quad = &quads[i];

        if (!quad->texture->name || quad->width <= 0 || quad->height <= 0) continue;
        draw_quad( comp, quad->texture, quad->x, quad->y, quad->width, quad->height,
                   quad->src_x, quad->src_y, quad->src_width, quad->src_height );
    }

    if (cursor_visible && comp->cursor.name)
    {
        gl->Enable( GL_BLEND );
        gl->BlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        gl->Uniform1i( comp->window_uniform, 0 );
        draw_quad( comp, &comp->cursor, cursor_x, cursor_y, POINTER_CURSOR_W, POINTER_CURSOR_H, 0, 0, 0, 0 );
        gl->Disable( GL_BLEND );
    }
}
