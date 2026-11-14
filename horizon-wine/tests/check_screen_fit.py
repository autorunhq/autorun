#!/usr/bin/env python3
"""Display mode emulation off: the screen's surface takes the program's mode.

A program then draws straight into the screen's EGL surface. That surface was
always 1280x720, so a game in 800x600 drew into a corner of it at its own size.
nx_screen_fit resizes it to the mode (mesa-switch's eglResizeSurfaceMESA) and
the console's scaler stretches it; win32u's pointer mapping says stretched too."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
nx = (root / 'dlls/win32u/winnx_opengl.c').read_text()
sysparams = (root / 'dlls/win32u/sysparams.c').read_text()
runtime = (root / 'horizon-wine/source/runtime.c').read_text()

start = nx.index('static void nx_screen_fit( struct opengl_drawable *base )\n{')
fit = nx[start:nx.index('\n}\n', start) + 3]

harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL, EGLint, EGLBoolean;
typedef void *EGLDisplay, *EGLSurface;
typedef struct { int cx, cy; } SIZE;
#define TRUE 1
#define FALSE 0
struct opengl_drawable { EGLSurface surface; SIZE virtual_size, monitor_size; };
static struct { EGLDisplay display; } egl_data, *egl = &egl_data;
static int resizes, refuse, have = 1, lookups, width = 1280, height = 720;
static EGLBoolean mock_resize( EGLDisplay display, EGLSurface surface, EGLint w, EGLint h )
{
    resizes++;
    if (refuse) return 0;
    width = w; height = h;
    return 1;
}
static void *mock_proc( const char *name )
{
    lookups++;
    assert( !strcmp( name, "eglResizeSurfaceMESA" ) );
    return have ? (void *)mock_resize : NULL;
}
static EGLint mock_error( void ) { return 0x3003; }
static struct { void *(*p_eglGetProcAddress)( const char * ); EGLint (*p_eglGetError)( void ); }
    functions = { mock_proc, mock_error }, *funcs = &functions;
static char last[256];
static void nx_log( const char *format, ... )
{
    va_list args; va_start( args, format ); vsnprintf( last, sizeof(last), format, args ); va_end( args );
}
static BOOL emulate_modeset;
int wine_nx_picture_mode __attribute__((weak));
typedef EGLBoolean (*PFN_eglResizeSurfaceMESA)( EGLDisplay display, EGLSurface surface, EGLint width, EGLint height );
static SIZE nx_screen_size = { 1280, 720 };
static SIZE nx_screen_refused;
''' + fit + r'''
int main( void )
{
    struct opengl_drawable d = { NULL, { 1280, 720 }, { 1280, 720 } };

    /* The screen's own size: nothing to do, the driver is not even asked. */
    nx_screen_fit( &d );
    assert( !resizes && !lookups );

    /* Emulation on: win32u's framebuffer object does it. */
    d.virtual_size = (SIZE){ 800, 600 };
    emulate_modeset = 1;
    nx_screen_fit( &d );
    assert( !resizes );
    emulate_modeset = 0;

    /* Own size: left where it was. */
    wine_nx_picture_mode = 3;
    nx_screen_fit( &d );
    assert( !resizes );
    wine_nx_picture_mode = 0;

    /* A mode of the program's own over the whole screen: once. */
    nx_screen_fit( &d );
    assert( resizes == 1 && width == 800 && height == 600 && strstr( last, "800x600, was 1280x720" ) );
    nx_screen_fit( &d );
    assert( resizes == 1 && lookups == 1 );

    /* A larger mode than the screen is scaled down the same way. */
    d.virtual_size = (SIZE){ 1600, 1200 };
    nx_screen_fit( &d );
    assert( resizes == 2 && width == 1600 && height == 1200 );

    /* A window that is not the whole screen draws at the screen's size. */
    d.virtual_size = d.monitor_size = (SIZE){ 640, 480 };
    nx_screen_fit( &d );
    assert( resizes == 3 && width == 1280 && height == 720 );

    /* Sizes nobody means are not asked for. */
    d.monitor_size = (SIZE){ 1280, 720 };
    d.virtual_size = (SIZE){ 1, 1 };
    nx_screen_fit( &d );
    d.virtual_size = (SIZE){ 5000, 3000 };
    nx_screen_fit( &d );
    assert( resizes == 3 );

    /* A refusal is reported and not asked for again every frame, but another
     * size is tried, and after a success the refused one may be again. */
    refuse = 1;
    d.virtual_size = (SIZE){ 1024, 768 };
    nx_screen_fit( &d );
    nx_screen_fit( &d );
    assert( resizes == 4 && width == 1280 && strstr( last, "resizing it to 1024x768 failed" ) );
    refuse = 0;
    d.virtual_size = (SIZE){ 640, 480 };
    nx_screen_fit( &d );
    assert( resizes == 5 && width == 640 );
    d.virtual_size = (SIZE){ 1024, 768 };
    nx_screen_fit( &d );
    assert( resizes == 6 && width == 1024 );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-screen-fit-') as tmp:
    source = pathlib.Path(tmp) / 'screen_fit.c'
    source.write_text(harness)
    binary = pathlib.Path(tmp) / 'screen_fit'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-address',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# After a swap, when the surface is the thread's draw surface, and only the screen's.
swap = nx[nx.index('static BOOL nx_drawable_swap( struct opengl_drawable *base )\n{'):]
swap = swap[:swap.index('\n}\n')]
assert swap.index('p_eglSwapBuffers( egl->display, base->surface );\n    __atomic_add_fetch') < swap.index('if (ret) nx_screen_fit( base );')
# A new surface is the screen's size again, and the window is when it goes back.
create = nx[nx.index('static BOOL nx_surface_create('):]
assert 'nx_screen_size.cx = 1280;' in create and 'nx_screen_refused.cx = nx_screen_refused.cy = 0;' in create
release = runtime[runtime.index('void wine_nx_gl_release_window(void)'):]
release = release[:release.index('\n}\n')]
assert release.index('nwindowSetDimensions( nwindowGetDefault(), WINE_NX_FB_W, WINE_NX_FB_H );') < release.index('wine_nx_compositor_resume();')
# A finger lands where the picture is.
ratios = sysparams[sysparams.index('static void source_virt_to_raw_ratios('):]
assert ratios.index('if (!emulate_modeset && mode != 3) mode = 2;') < ratios.index('if (mode == 3)')
print('screen surface at the program\'s mode: ok')
