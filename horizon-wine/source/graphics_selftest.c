/*
 * The graphics stack tried step by step at start, before anything draws, each
 * step logged and the log written out before the next: when OpenGL cannot be
 * brought up, the last [GFXTEST] line is the step that failed, and a step that
 * crashes is the one after it. Then Mesa's own messages, which it writes to a
 * file (MESA_LOG_FILE), are copied into the runtime's log under [MESA], so one
 * log holds both.
 *
 * Run by the nxvk runtime always, and by the card's own with
 * graphics-selftest in its settings (source/runtime.c).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>

void wine_nx_runtime_trace( const char *msg );
void wine_nx_runtime_log_flush( void );

static unsigned long long selftest_start;

static void step( const char *format, ... ) __attribute__((format(printf, 1, 2)));
static void step( const char *format, ... )
{
    char line[512];
    va_list args;
    int len = snprintf( line, sizeof(line), "[GFXTEST] %4llu ms ",
                        armTicksToNs( armGetSystemTick() - selftest_start ) / 1000000ull );

    va_start( args, format );
    vsnprintf( line + len, sizeof(line) - len, format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
    wine_nx_runtime_log_flush();
}

static const char *egl_error_name( EGLint error )
{
    switch (error)
    {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "an unknown error";
    }
}

static int failed( const char *what )
{
    EGLint error = eglGetError();

    step( "%s failed: %s (%#x)", what, egl_error_name( error ), (unsigned int)error );
    return 0;
}

/* The environment Mesa reads its choices from, as it stands. */
static void environment( void )
{
    static const char *const names[] =
    {
        "MESA_SWITCH_GL_DRIVER", "NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "MESA_LOG_FILE", "MESA_LOG_LEVEL",
        "EGL_LOG_LEVEL", "MESA_GL_VERSION_OVERRIDE", "MESA_GLTHREAD", "ZINK_DEBUG", "NVK_DEBUG",
    };
    char line[400];
    unsigned int i;
    int len = snprintf( line, sizeof(line), "environment:" );

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        const char *value = getenv( names[i] );

        if (value) len += snprintf( line + len, sizeof(line) - len, " %s=%s", names[i], value );
    }
    step( "%s", len > 12 ? line : "environment: no Mesa variables set" );
}

static int run( void )
{
    typedef const unsigned char *(*get_string_fn)( unsigned int );
    static const EGLint config_attributes[] =
    {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE
    };
    static const EGLint surface_attributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLDisplay display;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;
    EGLint major = 0, minor = 0, configs = 0;
    get_string_fn get_string;
    const char *text;

    step( "eglGetDisplay" );
    if ((display = eglGetDisplay( EGL_DEFAULT_DISPLAY )) == EGL_NO_DISPLAY) return failed( "eglGetDisplay" );
    step( "eglInitialize (this creates the GPU screen: Zink and its Vulkan device, or nvc0)" );
    if (!eglInitialize( display, &major, &minor )) return failed( "eglInitialize" );
    text = eglQueryString( display, EGL_VENDOR );
    step( "EGL %d.%d by %s", major, minor, text ? text : "?" );
    text = eglQueryString( display, EGL_CLIENT_APIS );
    step( "client APIs: %s", text ? text : "?" );
    text = eglQueryString( display, EGL_EXTENSIONS );
    step( "extensions: %.380s", text ? text : "?" );
    if (!eglGetConfigs( display, NULL, 0, &configs )) failed( "eglGetConfigs" );
    step( "%d configs", configs );
    if (!eglBindAPI( EGL_OPENGL_API )) return failed( "eglBindAPI(OpenGL)" );
    if (!eglChooseConfig( display, config_attributes, &config, 1, &configs ) || !configs)
        return failed( "eglChooseConfig(RGBA8, depth 24, stencil 8, OpenGL, pbuffer)" );
    step( "eglCreatePbufferSurface 16x16" );
    if ((surface = eglCreatePbufferSurface( display, config, surface_attributes )) == EGL_NO_SURFACE)
        return failed( "eglCreatePbufferSurface" );
    step( "eglCreateContext" );
    if ((context = eglCreateContext( display, config, EGL_NO_CONTEXT, NULL )) == EGL_NO_CONTEXT)
    {
        failed( "eglCreateContext" );
        eglDestroySurface( display, surface );
        eglTerminate( display );
        return 0;
    }
    step( "eglMakeCurrent" );
    if (!eglMakeCurrent( display, surface, surface, context )) failed( "eglMakeCurrent" );
    else if ((get_string = (get_string_fn)eglGetProcAddress( "glGetString" )))
    {
        const unsigned char *vendor = get_string( 0x1F00 ), *renderer = get_string( 0x1F01 ),
                            *version = get_string( 0x1F02 );

        step( "OpenGL works: %s, %s, %s", vendor ? (const char *)vendor : "?",
              renderer ? (const char *)renderer : "?", version ? (const char *)version : "?" );
    }
    eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
    eglDestroyContext( display, context );
    eglDestroySurface( display, surface );
    step( "eglTerminate" );
    eglTerminate( display );
    return 1;
}

/* Mesa's own messages since the last copy, into the runtime's log. */
void wine_nx_mesa_log_copy( const char *why )
{
    static long copied;
    const char *path = getenv( "MESA_LOG_FILE" );
    char line[480], out[512];
    unsigned int lines = 0, skipped = 0;
    FILE *file;

    if (!path || !(file = fopen( path, "r" ))) return;
    if (fseek( file, copied, SEEK_SET )) fseek( file, 0, SEEK_SET );
    while (fgets( line, sizeof(line), file ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (!line[0]) continue;
        if (lines++ < 120)
        {
            snprintf( out, sizeof(out), "[MESA] %s", line );
            wine_nx_runtime_trace( out );
        }
        else skipped++;
    }
    copied = ftell( file );
    fclose( file );
    if (lines || why)
    {
        snprintf( out, sizeof(out), "[MESA] %u lines of %s%s%s%s", lines, path, why ? " (" : "", why ? why : "",
                  why ? ")" : "" );
        wine_nx_runtime_trace( out );
    }
    if (skipped)
    {
        snprintf( out, sizeof(out), "[MESA] %u more lines are only in %s", skipped, path );
        wine_nx_runtime_trace( out );
    }
    wine_nx_runtime_log_flush();
}

int wine_nx_graphics_selftest( void )
{
    int ok;

    selftest_start = armGetSystemTick();
    environment();
    ok = run();
    step( ok ? "OpenGL came up and went down again" : "OpenGL could not be brought up; Mesa's reasons follow" );
    wine_nx_mesa_log_copy( "after the graphics test" );
    return ok;
}
