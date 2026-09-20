#include <switch.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/server.h"
#include "unix_private.h"
#include "horizon_private.h"
#include "launcher.h"
#include "autorun_install.h"
#include "forwarder.h"
#include "launcher_list.h"
#include "launcher_settings.h"
#include "config_json.h"
#include "pointer_cursor.h"
#include "compositor.h"
#include "std_stream_lines.h"
#include "thread_profile.h"
#include "dxvk_releases.h"
#ifdef WINE_NX_MESA_SWITCH
#include "graphics_config.h"
#endif
#ifdef WINE_NX_LSFG
#include "lsfg_config.h"
#endif

/* The sampler finds an x86 context through these without Wine's headers. */
C_ASSERT( FIELD_OFFSET( TEB, TlsSlots[WOW64_TLS_CPURESERVED] ) == NX_PROF_TEB_CPU_AREA );
C_ASSERT( sizeof(WOW64_CPURESERVED) == NX_PROF_CPU_CONTEXT && TYPE_ALIGNMENT( I386_CONTEXT ) <= NX_PROF_CPU_CONTEXT );
C_ASSERT( FIELD_OFFSET( I386_CONTEXT, Ebp ) == NX_PROF_I386_EBP );
C_ASSERT( FIELD_OFFSET( I386_CONTEXT, Esp ) == NX_PROF_I386_ESP );

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 256 * 1024 * 1024;
unsigned char __attribute__((aligned(16))) __nx_exception_stack[0x10000];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);
/* Run __libnx_exception_handler even when hbloader/Atmosphere has attached
 * as the debugger (which is always the case for NRO launches). Without this,
 * libnx's exception.s short-circuits to abort before calling our handler. */
u32 __nx_exception_ignoredebug = 1;

#define WINE_ROOT "sdmc:/switch/wine"
#define WINE_DRIVE_C WINE_ROOT "/drive_c"
#define WINE_SYSTEM_DIR WINE_DRIVE_C "/windows/system32"
#define WINE_USER_DIR WINE_DRIVE_C "/users/wine"
#define RUNTIME_DIR WINE_ROOT
/* Everything a person sets, in one place. */
#define CONFIG_DIR  RUNTIME_DIR "/config"
#define CONFIG_FILE CONFIG_DIR "/settings.json"
#define DEFAULT_TARGET WINE_DRIVE_C "/curl/curl.exe"
#ifdef WINE_NX_AMD64
#define WINE_NX_RUNTIME_BUILD "nx-amd64-box64-3"
#elif defined(WINE_NX_BOX64_DYNAREC)
#define WINE_NX_RUNTIME_BUILD "nx-wow64-dynarec-218"
#else
#define WINE_NX_RUNTIME_BUILD "nx-wow64-console-11"
#endif
#define MAX_RUNTIME_MODULES 64
#define MAX_IMPORT_DEPTH 16

extern void wine_nx_runtime_platform_init(void);
extern void wine_nx_runtime_network_init(void);
extern void wine_nx_runtime_environment_init(void);
extern NTSTATUS wine_nx_loader_bootstrap( const UNICODE_STRING *main_nt_name );
extern NTSTATUS wine_nx_loader_fixup_main_imports(void);
extern NTSTATUS wine_nx_loader_attach_main(void);
extern const char *wine_nx_loader_last_import_dll(void);
extern NTSTATUS wine_nx_loader_last_import_status(void);
extern const char *wine_nx_loader_last_open_path(void);
extern NTSTATUS wine_nx_loader_last_open_status(void);
extern const char *wine_nx_loader_last_export_diag(void);
extern int wine_nx_sd_cache_install(void);
#ifdef WINE_NX_USB_STORAGE
extern int wine_nx_usb_list( struct wine_nx_launcher_usb_volume *volumes, int max );
#endif

static FILE *log_file;
/* A second copy, kept from the moment a program starts. The next run of the
 * launcher opens wine-nx-runtime.log afresh and what the program did is gone
 * with it, so a program's own log is a file of its own, which only the next
 * run of that same program writes over. */
static FILE *game_log_file;

struct runtime_module
{
    char path[512];
    char dir[512];
    char name[128];
    void *base;
    SIZE_T size;
    IMAGE_NT_HEADERS64 *nt;
    int is_main;
    int resolving_imports;
    int imports_scanned;
};

struct import_stats
{
    unsigned int dlls;
    unsigned int loaded_dlls;
    unsigned int missing_dlls;
    unsigned int imports;
    unsigned int bound;
    unsigned int unresolved;
    unsigned int forwarded;
};

static struct runtime_module modules[MAX_RUNTIME_MODULES];
static unsigned int module_count;

static pthread_t log_main_thread;
static int log_main_thread_set;

/* The text console and the Wine framebuffer both own the default nwindow, so
 * once a GUI app brings up the display driver we hand the screen over to the
 * framebuffer and stop driving the console (logs still go to the file). */
static int wine_nx_console_active = 1;
/* The console is left standing but is not written to. The start-up has a few
 * dozen lines to say and they all went to the screen, so every run began with a
 * terminal filling up -- in front of the launcher, or in front of the game when
 * no launcher was shown. They go to the log alone now. The console speaks for
 * the one line that says which game is starting, and again if the game cannot
 * be started, since then the screen is all there is to say so on. */
static int wine_nx_console_quiet = 1;
static Framebuffer wine_nx_fb;
static int wine_nx_fb_ready;
static pthread_mutex_t wine_nx_fb_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *wine_nx_fb_pending_bits;
static int wine_nx_fb_pending_stride;
static int wine_nx_fb_pending_dirty;
static int wine_nx_fb_lock_depth;
static u64 wine_nx_fb_last_present;
static unsigned int wine_nx_fb_frames; /* frames queued to the display, for [PROGRESS] */

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char log_file_buffer[64 * 1024];
static int log_flusher_running;

/* Lines that must reach the SD card even if the process dies right after. */
static int log_line_is_urgent( const char *line )
{
    return !strncmp( line, "[EXC]", 5 ) || !strncmp( line, "[EXIT]", 6 ) ||
           !strncmp( line, "[FAIL]", 6 ) || !strncmp( line, "[PE32 TEST]", 11 ) ||
           !strncmp( line, "[LIFECYCLE] final", 17 ) || !strncmp( line, "[LIFECYCLE] verdict", 19 );
}

static void runtime_tick_std_streams(void);
static void runtime_report_interpreter(void);

/* Set at startup unless gl-noclean.txt or gl-clean.txt chose: the cache clean
 * of pinned GPU buffers before each submission goes off and on. */
static int clean_alternates;
extern int wine_nx_nouveau_skip_clean __attribute__((weak));

/* A test of the clean libdrm_nouveau does before every GPU submission, ~12% of
 * Direct3D's drawing thread in NFSU2: from a minute in, 30 seconds off, 30
 * seconds on. [PROGRESS] cleans= stops growing while it is off, so one race
 * shows its cost, and whether anything flickers shows whether the GPU needs it. */
static void runtime_alternate_clean(void)
{
    static u64 start;
    static int last = -1;
    u64 now = armGetSystemTick(), seconds;
    int skip;

    if (!clean_alternates) return;
    if (!start) start = now;
    seconds = armTicksToNs( now - start ) / 1000000000ull;
    skip = seconds >= 60 && (seconds / 30) % 2 == 0;
    if (skip == last) return;
    last = skip;
    wine_nx_nouveau_skip_clean = skip;
    if (log_file)
    {
        pthread_mutex_lock( &log_mutex );
        fprintf( log_file, "[CLEAN] %s at %llus\n", skip ? "off" : "on", (unsigned long long)seconds );
        pthread_mutex_unlock( &log_mutex );
    }
}

/* Watches for the program stopping without stopping. It runs on a thread of its
 * own, and touches nothing but the kernel: the flusher below shares its lot with
 * whatever the program is stuck in -- the card, a lock of Wine's -- and a watch
 * kept there would be stuck in the same place and say nothing, which is what a
 * hang looked like until now. */
static void log_line( const char *fmt, ... ) __attribute__((format(printf,1,2)));

static volatile int stall_watch_quit;
static int stall_watch_running;
static Thread stall_watch_thread;

/* Flushing each line to the SD card serialized every thread behind the file
 * lock. Buffer instead and flush often enough that a hang loses under 200 ms.
 * The same thread emits idle partial output lines and reports interpreter speed. */
static int log_flusher_quit;
static pthread_t log_flusher_thread;

static void *log_flusher( void *arg )
{
    unsigned int ticks = 0;

    (void)arg;
    while (!__atomic_load_n( &log_flusher_quit, __ATOMIC_RELAXED ))
    {
        svcSleepThread( 200000000LL );
        runtime_tick_std_streams();
        if (++ticks % 25 == 0) runtime_report_interpreter();
        if (ticks % 5 == 0)
        {
            extern int horizon_registry_flush(void);
            horizon_registry_flush();
        }
        if (ticks % 10 == 0) wine_nx_thread_balance();
        runtime_alternate_clean();
        pthread_mutex_lock( &log_mutex );
        fflush( log_file );
        pthread_mutex_unlock( &log_mutex );
    }
    return NULL;
}

/* A running thread keeps its stack, which libnx maps out of the heap, lent to
 * the mapping: the loader then cannot reset the heap and gives up with
 * InvalidMemoryState. Every thread this runtime owns has to end before it does. */
/* Its stack is heap lent to it, like every thread's, so it has to end and be
 * waited for before the loader can take the process back. */
static void stop_stall_watch( void )
{
    if (!stall_watch_running) return;
    stall_watch_running = 0;
    __atomic_store_n( (int *)&stall_watch_quit, 1, __ATOMIC_RELAXED );
    if (R_SUCCEEDED( waitSingle( waiterForThread( &stall_watch_thread ), 3000000000ULL ) ))
        threadClose( &stall_watch_thread );
    else log_line( "[EXIT] the stall watch did not end; its stack stays lent out" );
}

static void stop_log_flusher( void )
{
    stop_stall_watch();
    if (!log_flusher_running) return;
    __atomic_store_n( &log_flusher_quit, 1, __ATOMIC_RELAXED );
    pthread_join( log_flusher_thread, NULL );
    log_flusher_running = 0;
}

/* Logging must not be able to stop the program. The flusher holds this lock
 * while it writes to the card, and a write that does not come back would
 * otherwise take every thread that logs a line down with it -- which looks
 * exactly like the game hanging. A line that cannot be written is dropped and
 * counted instead. */
static unsigned int log_lines_dropped;

static int log_lock_bounded( void )
{
    int i;

    for (i = 0; i < 50; i++)
    {
        if (!pthread_mutex_trylock( &log_mutex )) return 1;
        svcSleepThread( 1000000LL );
    }
    __atomic_add_fetch( &log_lines_dropped, 1, __ATOMIC_RELAXED );
    return 0;
}

static void log_line( const char *fmt, ... )
{
    /* The software console aborts the process (framebufferBegin →
     * diagAbortWithResult) when driven from any thread but the one that
     * called consoleInit, including exception handlers running on Wine
     * secondary threads.  Off the main thread, log to the file only. */
    int on_main = wine_nx_console_active && !wine_nx_console_quiet &&
                  (!log_main_thread_set || pthread_equal( pthread_self(), log_main_thread ));
    char line[1024];
    va_list args;
    int len;

    va_start( args, fmt );
    len = vsnprintf( line, sizeof(line) - 1, fmt, args );
    va_end( args );
    if (len < 0) return;
    if (len > (int)sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = 0;

    /* Syscall traces stay in the file: each console update presents a frame. */
    if (on_main && strncmp( line, "[SYSCALL]", 9 )) fputs( line, stdout );

    if (log_file && log_lock_bounded())
    {
        /* One write per line, so concurrent threads never interleave. */
        unsigned int dropped = __atomic_exchange_n( &log_lines_dropped, 0, __ATOMIC_RELAXED );

        if (dropped) fprintf( log_file, "[LOG] %u lines dropped while the card was busy\n", dropped );
        fwrite( line, 1, len, log_file );
        if (!log_flusher_running || log_line_is_urgent( line )) fflush( log_file );
        if (game_log_file)
        {
            fwrite( line, 1, len, game_log_file );
            if (!log_flusher_running || log_line_is_urgent( line )) fflush( game_log_file );
        }
        pthread_mutex_unlock( &log_mutex );
    }
    if (on_main && strncmp( line, "[SYSCALL]", 9 )) consoleUpdate( NULL );
}

/* A program's own log, kept from the moment it is about to start: everything
 * the runtime has said so far, and everything it says from here. The launcher's
 * next run opens wine-nx-runtime.log afresh, and without this the run that
 * mattered is gone before it can be read off the card. */
static void open_game_log( const char *target )
{
    char path[512], name[128];
    const char *base = strrchr( target, '/' );
    FILE *sofar;
    size_t i, len;

    if (!log_file) return;
    base = base ? base + 1 : target;
    if (!base[0]) return;
    for (i = 0; base[i] && i < sizeof(name) - 1; i++)
    {
        char c = base[i];

        /* A name a card can hold, and one word: "Halo - Combat Evolved" is a
         * folder, but HALO.EXE is what the file is called. */
        name[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_' ? c : '-';
    }
    name[i] = 0;
    if ((len = strlen( name )) > 4 && !strcasecmp( name + len - 4, ".exe" )) name[len - 4] = 0;
    snprintf( path, sizeof(path), "%s/game-%s.log", RUNTIME_DIR, name );

    pthread_mutex_lock( &log_mutex );
    fflush( log_file );
    if ((game_log_file = fopen( path, "w" )))
    {
        /* What was said before this point, so the file stands on its own. */
        if ((sofar = fopen( RUNTIME_DIR "/wine-nx-runtime.log", "r" )))
        {
            char chunk[4096];
            size_t got;

            while ((got = fread( chunk, 1, sizeof(chunk), sofar )) > 0) fwrite( chunk, 1, got, game_log_file );
            fclose( sofar );
        }
        fflush( game_log_file );
    }
    pthread_mutex_unlock( &log_mutex );
    log_line( "[LOG] this run is kept in %s", path );
}

static void stall_watch( void *arg )
{
    extern unsigned int wine_nx_gl_swaps __attribute__((weak));
    extern unsigned int wine_nx_vk_presents __attribute__((weak));
    unsigned int quiet = 0, last_frames = ~0u, reported = 0;

    (void)arg;
    while (!__atomic_load_n( (int *)&stall_watch_quit, __ATOMIC_RELAXED ))
    {
        unsigned int frames;
        int i;

        /* Five seconds, in slices, so quitting does not wait for them. */
        for (i = 0; i < 50 && !__atomic_load_n( (int *)&stall_watch_quit, __ATOMIC_RELAXED ); i++)
            svcSleepThread( 100000000LL );
        frames = __atomic_load_n( &wine_nx_fb_frames, __ATOMIC_RELAXED ) +
                 (&wine_nx_gl_swaps ? __atomic_load_n( &wine_nx_gl_swaps, __ATOMIC_RELAXED ) : 0) +
                 wine_nx_compositor_frames_fast() +
                 (&wine_nx_vk_presents ? __atomic_load_n( &wine_nx_vk_presents, __ATOMIC_RELAXED ) : 0);
        if (frames != last_frames) { quiet = 0; reported = 0; }
        else quiet++;
        last_frames = frames;
        /* Ten seconds without a frame. A game loading a level does that too, so
         * this says its piece three times and then leaves the log alone. */
        if (quiet >= 2 && reported < 3)
        {
            log_line( "[STALL] no frame drawn for %u s; where the threads are standing", quiet * 5 );
            wine_nx_threads_report_stalled();
            reported++;
        }
    }
}


void wine_nx_runtime_trace( const char *msg )
{
    log_line( "%s", msg );
}

/* Per-operation traces (system calls, server requests, fonts, window painting)
 * are formatted and written to the SD card as they happen, which slows the
 * whole program down. Their call sites check this first; it is set from
 * sdmc:/switch/wine/verbose.txt containing 1. */
int wine_nx_runtime_verbose;
/* The sampling profiler's [PROF] lines (thread_profile.c): sdmc:/switch/wine/profile.txt
 * containing 1, which the launcher's X toggles like Y does verbose.txt. */
static int runtime_profile;
static int runtime_dxvk;
static int runtime_dxvk_hud;
static char runtime_vkd3d_version[32];
static char runtime_dxvk_version[32];

/* libdrm_nouveau's switch for CPU-cacheable pinned GPU memory, cleared by
 * sdmc:/switch/wine/gl-uncached.txt containing 1. */
extern int wine_nx_nouveau_pin_cached __attribute__((weak));
/* Set by sdmc:/switch/wine/gl-noclean.txt containing 1: submissions skip the CPU
 * cache clean of pinned GPU buffers, to see whether the GPU needs it. */
extern int wine_nx_nouveau_skip_clean __attribute__((weak));

/* Whether the display driver registers its GPU, source and monitor with
 * win32u's device manager, which programs enumerate and wined3d insists on.
 * sdmc:/switch/wine/no-display-devices.txt containing 1 goes back to the
 * forced virtual screen, in case that walk of the registry misbehaves. */
int wine_nx_display_devices = 1;

/***********************************************************************
 * Framebuffer platform hooks used by the win32u Switch display driver
 * (dlls/win32u/winnx_drv.c).  The driver renders into ordinary DIB memory;
 * these present the dirty pixels to the libnx framebuffer.
 */
#define WINE_NX_FB_W 1280
#define WINE_NX_FB_H 720

/* The drawn cursor, guarded by wine_nx_fb_mutex. */
static struct pointer_cursor wine_nx_cursor =
    { .x = WINE_NX_FB_W / 2, .y = WINE_NX_FB_H / 2, .width = WINE_NX_FB_W, .height = WINE_NX_FB_H };
static int wine_nx_cursor_moved;
static int wine_nx_cursor_visible = 1;  /* 0 while the program hides the mouse cursor */
static int wine_nx_gl_window;  /* an OpenGL window surface owns the screen's NWindow */
/* Controller and touchscreen state, guarded by wine_nx_pointer_mutex. */
static pthread_mutex_t wine_nx_pointer_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pointer_cursor wine_nx_pointer =
    { .x = WINE_NX_FB_W / 2, .y = WINE_NX_FB_H / 2, .width = WINE_NX_FB_W, .height = WINE_NX_FB_H };
static PadState wine_nx_pad;
static u64 wine_nx_pointer_tick;
static int wine_nx_pointer_ready;
/* What the polls saw since the last wine_nx_pointer_take(). */
static struct pointer_buttons wine_nx_pointer_buttons;
static int wine_nx_pointer_moved;
/* A touch points at a place, and the place is what Wine is given. */
static int wine_nx_pointer_placed;
/* A finger sending keys: where it went down, and how far it has gone since.
 * Half a centimetre of a 1280-pixel screen, so that a tap is not a direction. */
#define WINE_NX_TOUCH_STEP 40
static int wine_nx_touch_held, wine_nx_touch_x, wine_nx_touch_y, wine_nx_touch_dx, wine_nx_touch_dy;
/* The position Wine last had, from a take or the program's SetCursorPos. */
static int wine_nx_pointer_sent_x = WINE_NX_FB_W / 2, wine_nx_pointer_sent_y = WINE_NX_FB_H / 2;

/* Take the screen from the text console and bring up a linear framebuffer. */
int wine_nx_fb_init(void)
{
    Result rc;
    if (wine_nx_fb_ready) return 0;
    if (wine_nx_gl_window) return -1;  /* an OpenGL surface has the screen */
    log_line( "[NXFB] fb_init: taking screen from console" );
    if (wine_nx_console_active)
    {
        consoleExit( NULL );
        wine_nx_console_active = 0;
    }
    rc = framebufferCreate( &wine_nx_fb, nwindowGetDefault(),
                            WINE_NX_FB_W, WINE_NX_FB_H, PIXEL_FORMAT_RGBA_8888, 3 );
    if (R_FAILED( rc ))
    {
        log_line( "[NXFB] framebufferCreate FAILED rc=0x%x", rc );
        return -1;
    }
    framebufferMakeLinear( &wine_nx_fb );
    wine_nx_fb_ready = 1;
    log_line( "[NXFB] framebuffer ready %dx%d", WINE_NX_FB_W, WINE_NX_FB_H );
    return 0;
}

/* Acquire the back buffer for writing; returns linear RGBA8888 pixels. */
void *wine_nx_fb_lock( int *width, int *height, int *stride_px )
{
    u32 stride = 0;
    void *bits = NULL;

    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (!wine_nx_fb_ready && wine_nx_fb_init())
    {
        pthread_mutex_unlock( &wine_nx_fb_mutex );
        return NULL;
    }
    if (!wine_nx_fb_pending_bits)
    {
        wine_nx_fb_pending_bits = framebufferBegin( &wine_nx_fb, &stride );
        wine_nx_fb_pending_stride = (int)(stride / 4);
    }
    bits = wine_nx_fb_pending_bits;
    if (!bits)
    {
        pthread_mutex_unlock( &wine_nx_fb_mutex );
        return NULL;
    }
    wine_nx_fb_lock_depth++;
    if (width)     *width     = WINE_NX_FB_W;
    if (height)    *height    = WINE_NX_FB_H;
    if (stride_px) *stride_px = wine_nx_fb_pending_stride;
    return bits;
}

void wine_nx_fb_unlock(void)
{
    wine_nx_fb_pending_dirty = 1;
    if (wine_nx_fb_lock_depth > 0) wine_nx_fb_lock_depth--;
    pthread_mutex_unlock( &wine_nx_fb_mutex );
}

/* The OpenGL compositor (compositor.c) presents the screen, unless
 * sdmc:/switch/wine/framebuffer.txt containing 1 keeps the framebuffer. */
static int wine_nx_compositor_mode = 1;
extern const struct compositor_backend wine_nx_compositor_egl_backend;

/* Stop driving the text console, which shares the NWindow; for the compositor. */
void wine_nx_screen_leave_console(void)
{
    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (wine_nx_console_active)
    {
        consoleExit( NULL );
        wine_nx_console_active = 0;
    }
    pthread_mutex_unlock( &wine_nx_fb_mutex );
}

/* Whether the compositor presents the screen, starting it on the first call.
 * The display driver asks before giving a window surface a layer, and an
 * OpenGL surface asks before it takes the screen, so the compositor never
 * starts, and never draws, while a program's OpenGL has the screen. */
int wine_nx_compositor_enabled(void)
{
    static int cursor_synced;
    int x, y, visible;

    if (!wine_nx_compositor_mode) return 0;
    /* The console shares the NWindow. Leave it from this Wine thread, as the
     * framebuffer does, not from the presenter. */
    if (!wine_nx_compositor_running()) wine_nx_screen_leave_console();
    if (wine_nx_compositor_start( &wine_nx_compositor_egl_backend, WINE_NX_FB_W, WINE_NX_FB_H )) return 0;
    if (!__atomic_exchange_n( &cursor_synced, 1, __ATOMIC_ACQ_REL ))
    {
        pthread_mutex_lock( &wine_nx_fb_mutex );
        x = (int)wine_nx_cursor.x;
        y = (int)wine_nx_cursor.y;
        visible = wine_nx_cursor_visible;
        pthread_mutex_unlock( &wine_nx_fb_mutex );
        wine_nx_compositor_cursor( x, y, visible );
    }
    return 1;
}

/* An OpenGL window surface takes the screen. libnx's framebuffer and EGL cannot
 * both queue buffers to the default NWindow, so the framebuffer is closed, or
 * the compositor gives the screen up, while the surface exists; GDI keeps
 * drawing into window surfaces, shown again once the surface is gone. Returns
 * NULL while another surface has the screen or the framebuffer is being drawn. */
void *wine_nx_gl_acquire_window(void)
{
    int compositor = wine_nx_compositor_enabled();
    NWindow *window = NULL;

    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (!wine_nx_gl_window && !wine_nx_fb_lock_depth)
    {
        if (compositor)
            ;  /* suspended below, without this lock, as it waits for the presenter */
        else if (wine_nx_fb_ready)
        {
            framebufferClose( &wine_nx_fb );
            wine_nx_fb_ready = 0;
            wine_nx_fb_pending_bits = NULL;
            wine_nx_fb_pending_stride = 0;
            wine_nx_fb_pending_dirty = 0;
        }
        else if (wine_nx_console_active)
        {
            consoleExit( NULL );
            wine_nx_console_active = 0;
        }
        window = nwindowGetDefault();
        wine_nx_gl_window = 1;
    }
    pthread_mutex_unlock( &wine_nx_fb_mutex );
    if (window && compositor) wine_nx_compositor_suspend();
    if (window) nwindowSetDimensions( window, WINE_NX_FB_W, WINE_NX_FB_H );
    log_line( "[NXGL] %s", window ? "screen handed to an OpenGL surface" : "screen busy; OpenGL surface refused" );
    return window;
}

/* The OpenGL surface is destroyed; the framebuffer or the compositor may take
 * the screen back. */
void wine_nx_gl_release_window(void)
{
    int compositor = wine_nx_compositor_running();

    pthread_mutex_lock( &wine_nx_fb_mutex );
    wine_nx_gl_window = 0;
    pthread_mutex_unlock( &wine_nx_fb_mutex );
    log_line( "[NXGL] screen returned to the %s", compositor ? "compositor" : "framebuffer" );
    if (compositor) wine_nx_compositor_resume();
}

/* Each present converts the whole screen, so frames that only move the
 * cursor are held to the display rate. */
#define WINE_NX_CURSOR_FRAME_NS 16666667ull

void wine_nx_fb_present(void)
{
    u64 now = armGetSystemTick();

    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (wine_nx_fb_ready && !wine_nx_fb_lock_depth &&
        ((wine_nx_fb_pending_bits && wine_nx_fb_pending_dirty) ||
         (wine_nx_cursor_moved && armTicksToNs( now - wine_nx_fb_last_present ) >= WINE_NX_CURSOR_FRAME_NS)))
    {
        if (!wine_nx_fb_pending_bits)
        {
            u32 stride = 0;

            wine_nx_fb_pending_bits = framebufferBegin( &wine_nx_fb, &stride );
            wine_nx_fb_pending_stride = (int)(stride / 4);
        }
        if (wine_nx_fb_pending_bits)
        {
            if (wine_nx_cursor_visible)
                pointer_cursor_paint( &wine_nx_cursor, wine_nx_fb_pending_bits, wine_nx_fb_pending_stride, 1 );
            framebufferEnd( &wine_nx_fb );
            if (wine_nx_cursor_visible)
                pointer_cursor_paint( &wine_nx_cursor, wine_nx_fb_pending_bits, wine_nx_fb_pending_stride, 0 );
            wine_nx_fb_pending_bits = NULL;
            wine_nx_fb_pending_stride = 0;
            wine_nx_fb_pending_dirty = 0;
            wine_nx_cursor_moved = 0;
            wine_nx_fb_last_present = now;
            __atomic_add_fetch( &wine_nx_fb_frames, 1, __ATOMIC_RELAXED );
        }
    }
    pthread_mutex_unlock( &wine_nx_fb_mutex );
}

static void wine_nx_cursor_move( int x, int y )
{
    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (x != (int)wine_nx_cursor.x || y != (int)wine_nx_cursor.y)
    {
        pointer_cursor_place( &wine_nx_cursor, x, y );
        if (wine_nx_cursor_visible) wine_nx_cursor_moved = 1;
    }
    x = (int)wine_nx_cursor.x;
    y = (int)wine_nx_cursor.y;
    int visible = wine_nx_cursor_visible;
    pthread_mutex_unlock( &wine_nx_fb_mutex );
    wine_nx_compositor_cursor( x, y, visible );
}

/* The program showed or hid the mouse cursor. Programs that draw their own,
 * like OpenTTD, hide it; the arrow must not be drawn over theirs. */
void wine_nx_cursor_show( int visible )
{
    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (wine_nx_cursor_visible != !!visible)
    {
        wine_nx_cursor_visible = !!visible;
        wine_nx_cursor_moved = 1;  /* present the change */
    }
    int x = (int)wine_nx_cursor.x, y = (int)wine_nx_cursor.y;
    pthread_mutex_unlock( &wine_nx_fb_mutex );
    wine_nx_compositor_cursor( x, y, visible );
}

/* Buttons reported by wine_nx_pointer_poll(). */
#define WINE_NX_POINTER_LEFT  0x1
#define WINE_NX_POINTER_RIGHT 0x2

/* The console has no keyboard, so the controller stands in for one. These are
 * the controls that send keys, in the order of the bits in
 * wine_nx_pad_key_state. A and B are the mouse buttons unless given a key.
 * sdmc:/switch/wine/keys.txt overrides the virtual-key codes, one NAME=code
 * line each, and a program's own NAME.keys.txt next to it overrides those, so
 * a game that wants other keys needs no new build. */
enum
{
    WINE_NX_KEY_UP, WINE_NX_KEY_DOWN, WINE_NX_KEY_LEFT, WINE_NX_KEY_RIGHT,
    WINE_NX_KEY_X, WINE_NX_KEY_Y, WINE_NX_KEY_L, WINE_NX_KEY_R,
    WINE_NX_KEY_ZL, WINE_NX_KEY_ZR, WINE_NX_KEY_PLUS, WINE_NX_KEY_MINUS,
    WINE_NX_KEY_STICKL, WINE_NX_KEY_STICKR, WINE_NX_KEY_A, WINE_NX_KEY_B,
    /* Each of the three things that point, for a game that walks with one set
     * of keys and works its menus with another. The left stick sends what the
     * d-pad does until it is given keys of its own. */
    WINE_NX_KEY_LUP, WINE_NX_KEY_LDOWN, WINE_NX_KEY_LLEFT, WINE_NX_KEY_LRIGHT,
    WINE_NX_KEY_RUP, WINE_NX_KEY_RDOWN, WINE_NX_KEY_RLEFT, WINE_NX_KEY_RRIGHT,
    WINE_NX_KEY_TUP, WINE_NX_KEY_TDOWN, WINE_NX_KEY_TLEFT, WINE_NX_KEY_TRIGHT,
    WINE_NX_KEY_COUNT
};

/* What each of them does: move the mouse, or send its four keys. */
enum { WINE_NX_DEVICE_LEFT, WINE_NX_DEVICE_RIGHT, WINE_NX_DEVICE_DPAD, WINE_NX_DEVICE_TOUCH,
       WINE_NX_DEVICE_COUNT };
#define WINE_NX_POINTS  0   /* moves the mouse */
#define WINE_NX_PRESSES 1   /* sends its four keys */
static const char *const wine_nx_device_names[WINE_NX_DEVICE_COUNT] =
    { "LSTICK", "RSTICK", "DPAD", "TOUCH" };
/* The left stick and the d-pad have always sent keys; the others have pointed. */
static unsigned char wine_nx_device_mode[WINE_NX_DEVICE_COUNT] =
    { WINE_NX_PRESSES, WINE_NX_POINTS, WINE_NX_PRESSES, WINE_NX_POINTS };

static const char *const wine_nx_pad_key_names[WINE_NX_KEY_COUNT] =
{
    "UP", "DOWN", "LEFT", "RIGHT", "X", "Y", "L", "R",
    "ZL", "ZR", "PLUS", "MINUS", "STICKL", "STICKR", "A", "B",
    "LUP", "LDOWN", "LLEFT", "LRIGHT",
    "RUP", "RDOWN", "RLEFT", "RRIGHT",
    "TUP", "TDOWN", "TLEFT", "TRIGHT"
};

/* Defaults that suit a game: the d-pad and left stick steer, the triggers
 * accelerate and brake, and the face and shoulder buttons carry what a keyboard
 * usually has under the left hand. */
unsigned short wine_nx_pad_keys[WINE_NX_KEY_COUNT] =
{
    0x26, 0x28, 0x25, 0x27,  /* arrows */
    0x20, 0x46,              /* X space, Y f */
    0x09, 0x10,              /* L tab, R shift */
    0x28, 0x26,              /* ZL down, ZR up */
    0x1b, 0x09,              /* plus escape, minus tab */
    0x11, 0x12,              /* stick presses: control, alt */
    0, 0,                    /* A and B: none, so they click */
    0, 0, 0, 0,              /* the left stick: none, so it steers with the d-pad */
    0x26, 0x28, 0x25, 0x27,  /* the right stick, were it to send keys: arrows */
    0x26, 0x28, 0x25, 0x27,  /* and a finger dragged across the screen */
};

/* Which of those controls are held, read by the display driver's ProcessEvents
 * (dlls/win32u/winnx_drv.c), which turns the changes into key events. */
unsigned int wine_nx_pad_key_state;

/* When a program last read the controller through XInput (xinput_unix.c). */
extern u64 wine_nx_xinput_last_poll;

/* How long + and - must be held together before the program is closed. */
#define WINE_NX_QUIT_CHORD_NS 1000000000ull

void wine_nx_leave_process( const char *why );
void wine_nx_request_quit( const char *why );

/* One mouse for win32u, in native 1280x720 display coordinates: the right
 * analog stick moves the cursor, A holds the left button and B the right,
 * and a touchscreen contact puts the cursor under the finger with the left
 * button held.  Returns nonzero when the position changed. */
int wine_nx_pointer_poll( int *x, int *y, unsigned int *buttons )
{
    HidTouchScreenState touch = {0};
    HidAnalogStickState stick;
    unsigned int pressed = 0;
    u64 now, held, xinput_poll;
    int moved, gamepad, leave = 0;

    pthread_mutex_lock( &wine_nx_pointer_mutex );
    if (!wine_nx_pointer_ready)
    {
        hidInitializeTouchScreen();
        padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
        padInitializeDefault( &wine_nx_pad );
        wine_nx_pointer_tick = armGetSystemTick();
        wine_nx_pointer_ready = 1;
        if (wine_nx_runtime_verbose)
            log_line( "[NXINPUT] pointer ready: touchscreen, right stick cursor, A left button, B right button" );
    }
    padUpdate( &wine_nx_pad );
    now = armGetSystemTick();
    held = padGetButtons( &wine_nx_pad );
    stick = padGetStickPos( &wine_nx_pad, 1 );
    /* A program reading the controller through XInput gets it whole: no keys,
     * clicks or cursor come from it meanwhile. The touchscreen still points. */
    xinput_poll = wine_nx_xinput_last_poll;
    gamepad = xinput_poll && (xinput_poll >= now || armTicksToNs( now - xinput_poll ) < 1000000000ull);
    moved = 0;
    if (hidGetTouchScreenStates( &touch, 1 ) && touch.count > 0)
    {
        if (wine_nx_device_mode[WINE_NX_DEVICE_TOUCH] == WINE_NX_POINTS)
        {
            int old_x = (int)wine_nx_pointer.x, old_y = (int)wine_nx_pointer.y;

            pointer_cursor_place( &wine_nx_pointer, touch.touches[0].x, touch.touches[0].y );
            moved = (int)wine_nx_pointer.x != old_x || (int)wine_nx_pointer.y != old_y;
            wine_nx_pointer_placed |= moved;
            pressed |= WINE_NX_POINTER_LEFT;
        }
        else
        {
            /* Sending keys: which way the finger has gone from where it went
             * down, far enough that a tap is not a direction. */
            if (!wine_nx_touch_held)
            {
                wine_nx_touch_x = touch.touches[0].x;
                wine_nx_touch_y = touch.touches[0].y;
            }
            wine_nx_touch_dx = (int)touch.touches[0].x - wine_nx_touch_x;
            wine_nx_touch_dy = (int)touch.touches[0].y - wine_nx_touch_y;
            wine_nx_touch_held = 1;
        }
    }
    else
    {
        wine_nx_touch_held = wine_nx_touch_dx = wine_nx_touch_dy = 0;
        if (wine_nx_device_mode[WINE_NX_DEVICE_RIGHT] == WINE_NX_POINTS)
            moved = gamepad ? 0 : pointer_cursor_step( &wine_nx_pointer, stick.x, stick.y,
                                                       armTicksToNs( now - wine_nx_pointer_tick ) );
    }
    /* The left stick points as well when it is set to, so a game played with
     * the mouse alone has both of them for it. */
    if (!gamepad && wine_nx_device_mode[WINE_NX_DEVICE_LEFT] == WINE_NX_POINTS)
    {
        HidAnalogStickState left = padGetStickPos( &wine_nx_pad, 0 );

        moved |= pointer_cursor_step( &wine_nx_pointer, left.x, left.y,
                                      armTicksToNs( now - wine_nx_pointer_tick ) );
    }
    /* And the d-pad, which has no tilt to speak of: a direction held is the
     * stick pushed the whole way. */
    if (!gamepad && wine_nx_device_mode[WINE_NX_DEVICE_DPAD] == WINE_NX_POINTS)
    {
        int dpad_x = 0, dpad_y = 0;

        if (held & HidNpadButton_Left) dpad_x -= POINTER_CURSOR_STICK_MAX;
        if (held & HidNpadButton_Right) dpad_x += POINTER_CURSOR_STICK_MAX;
        if (held & HidNpadButton_Down) dpad_y -= POINTER_CURSOR_STICK_MAX;
        if (held & HidNpadButton_Up) dpad_y += POINTER_CURSOR_STICK_MAX;
        if (dpad_x || dpad_y)
            moved |= pointer_cursor_step( &wine_nx_pointer, dpad_x, dpad_y,
                                          armTicksToNs( now - wine_nx_pointer_tick ) );
    }
    wine_nx_pointer_tick = now;
    if (!gamepad && (held & HidNpadButton_A) && !wine_nx_pad_keys[WINE_NX_KEY_A]) pressed |= WINE_NX_POINTER_LEFT;
    if (!gamepad && (held & HidNpadButton_B) && !wine_nx_pad_keys[WINE_NX_KEY_B]) pressed |= WINE_NX_POINTER_RIGHT;
    {
        /* The left stick steers as well as the d-pad, past a dead zone. */
        HidAnalogStickState steer = padGetStickPos( &wine_nx_pad, 0 );
        static const struct { u64 button; int key; } buttons[] =
        {
            { HidNpadButton_X, WINE_NX_KEY_X }, { HidNpadButton_Y, WINE_NX_KEY_Y },
            { HidNpadButton_L, WINE_NX_KEY_L }, { HidNpadButton_R, WINE_NX_KEY_R },
            { HidNpadButton_ZL, WINE_NX_KEY_ZL }, { HidNpadButton_ZR, WINE_NX_KEY_ZR },
            { HidNpadButton_Plus, WINE_NX_KEY_PLUS }, { HidNpadButton_Minus, WINE_NX_KEY_MINUS },
            { HidNpadButton_StickL, WINE_NX_KEY_STICKL }, { HidNpadButton_StickR, WINE_NX_KEY_STICKR },
            /* The d-pad's four are left out when it is moving the mouse. */
            { HidNpadButton_Up, WINE_NX_KEY_UP }, { HidNpadButton_Down, WINE_NX_KEY_DOWN },
            { HidNpadButton_Left, WINE_NX_KEY_LEFT }, { HidNpadButton_Right, WINE_NX_KEY_RIGHT },
            { HidNpadButton_A, WINE_NX_KEY_A }, { HidNpadButton_B, WINE_NX_KEY_B },  /* sent only if given a key */
        };
        unsigned int keys = 0, i;

        for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        {
            if (wine_nx_device_mode[WINE_NX_DEVICE_DPAD] == WINE_NX_POINTS &&
                buttons[i].key >= WINE_NX_KEY_UP && buttons[i].key <= WINE_NX_KEY_RIGHT)
                continue;
            if (held & buttons[i].button) keys |= 1u << buttons[i].key;
        }
        /* The left stick steers with the d-pad unless it was given keys of
         * its own: Halo walks with w, a, s and d and works its menus with the
         * arrows, and one controller has to do both. */
        if (wine_nx_device_mode[WINE_NX_DEVICE_LEFT] == WINE_NX_PRESSES)
        {
            if (steer.y >  12000) keys |= 1u << (wine_nx_pad_keys[WINE_NX_KEY_LUP] ? WINE_NX_KEY_LUP : WINE_NX_KEY_UP);
            if (steer.y < -12000) keys |= 1u << (wine_nx_pad_keys[WINE_NX_KEY_LDOWN] ? WINE_NX_KEY_LDOWN : WINE_NX_KEY_DOWN);
            if (steer.x < -12000) keys |= 1u << (wine_nx_pad_keys[WINE_NX_KEY_LLEFT] ? WINE_NX_KEY_LLEFT : WINE_NX_KEY_LEFT);
            if (steer.x >  12000) keys |= 1u << (wine_nx_pad_keys[WINE_NX_KEY_LRIGHT] ? WINE_NX_KEY_LRIGHT : WINE_NX_KEY_RIGHT);
        }
        if (wine_nx_device_mode[WINE_NX_DEVICE_RIGHT] == WINE_NX_PRESSES)
        {
            if (stick.y >  12000) keys |= 1u << WINE_NX_KEY_RUP;
            if (stick.y < -12000) keys |= 1u << WINE_NX_KEY_RDOWN;
            if (stick.x < -12000) keys |= 1u << WINE_NX_KEY_RLEFT;
            if (stick.x >  12000) keys |= 1u << WINE_NX_KEY_RRIGHT;
        }
        /* A finger held away from where it went down, by more than a tap. */
        if (wine_nx_touch_held)
        {
            if (wine_nx_touch_dy < -WINE_NX_TOUCH_STEP) keys |= 1u << WINE_NX_KEY_TUP;
            if (wine_nx_touch_dy >  WINE_NX_TOUCH_STEP) keys |= 1u << WINE_NX_KEY_TDOWN;
            if (wine_nx_touch_dx < -WINE_NX_TOUCH_STEP) keys |= 1u << WINE_NX_KEY_TLEFT;
            if (wine_nx_touch_dx >  WINE_NX_TOUCH_STEP) keys |= 1u << WINE_NX_KEY_TRIGHT;
        }
        if (gamepad) keys = 0;
        __atomic_store_n( &wine_nx_pad_key_state, keys, __ATOMIC_RELAXED );
    }
    *x = (int)wine_nx_pointer.x;
    *y = (int)wine_nx_pointer.y;
    *buttons = pressed;
    pointer_buttons_update( &wine_nx_pointer_buttons, pressed );
    wine_nx_pointer_moved |= moved;
    /* + and - held together close the program, whether or not it still draws:
     * this poll runs on the display driver's thread, outside it. */
    {
        static u64 chord_since;
        const u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;

        if ((held & chord) != chord) chord_since = 0;
        else if (!chord_since) chord_since = now;
        else if (armTicksToNs( now - chord_since ) >= WINE_NX_QUIT_CHORD_NS) leave = 1;
    }
    pthread_mutex_unlock( &wine_nx_pointer_mutex );
    if (leave) wine_nx_request_quit( "+ and - held" );

    wine_nx_cursor_move( *x, *y );
    return moved;
}

/* Hand over what the polls saw since the previous take: the position, whether
 * it changed, the buttons held now, and those pressed or released in between.
 * The display driver polls from a background thread, which has no TEB and
 * must not call into Wine, and delivers the input from a Wine thread. */
/* The movement the stick has made since the last call, in whole pixels. */
int wine_nx_pointer_take_motion( int *dx, int *dy )
{
    int any;

    pthread_mutex_lock( &wine_nx_pointer_mutex );
    any = pointer_cursor_take_motion( &wine_nx_pointer, dx, dy );
    pthread_mutex_unlock( &wine_nx_pointer_mutex );
    return any;
}

/* Whether a touch pointed at a place since the last call. */
int wine_nx_pointer_take_placed( void )
{
    int placed;

    pthread_mutex_lock( &wine_nx_pointer_mutex );
    placed = wine_nx_pointer_placed;
    wine_nx_pointer_placed = 0;
    pthread_mutex_unlock( &wine_nx_pointer_mutex );
    return placed;
}

int wine_nx_pointer_take( int *x, int *y, unsigned int *buttons, unsigned int *pressed, unsigned int *released )
{
    struct pointer_buttons taken;
    int moved;

    pthread_mutex_lock( &wine_nx_pointer_mutex );
    *x = wine_nx_pointer_sent_x = (int)wine_nx_pointer.x;
    *y = wine_nx_pointer_sent_y = (int)wine_nx_pointer.y;
    taken = pointer_buttons_take( &wine_nx_pointer_buttons );
    moved = wine_nx_pointer_moved;
    wine_nx_pointer_moved = 0;
    pthread_mutex_unlock( &wine_nx_pointer_mutex );

    *buttons = taken.held;
    *pressed = taken.pressed;
    *released = taken.released;
    return moved;
}

/* Follow a position set by the application (SetCursorPos), keeping the stick
 * motion Wine has not been handed yet (pointer_cursor_warp). */
void wine_nx_pointer_set_pos( int x, int y )
{
    pthread_mutex_lock( &wine_nx_pointer_mutex );
    wine_nx_pointer_moved = pointer_cursor_warp( &wine_nx_pointer, wine_nx_pointer_sent_x,
                                                 wine_nx_pointer_sent_y, x, y );
    wine_nx_pointer_sent_x = x;
    wine_nx_pointer_sent_y = y;
    x = (int)wine_nx_pointer.x;
    y = (int)wine_nx_pointer.y;
    pthread_mutex_unlock( &wine_nx_pointer_mutex );
    wine_nx_cursor_move( x, y );
}

/* The cursor is where the server put it rather than where the stick pushed:
 * clipped to the screen, or held still for a program that took the mouse for
 * itself. The arrow goes there, and nothing is lost by it -- the movement has
 * been sent already, and what is left of it waits in the pointer's own count,
 * not in where the arrow happens to be. */
void wine_nx_pointer_follow( int x, int y )
{
    pthread_mutex_lock( &wine_nx_pointer_mutex );
    pointer_cursor_place( &wine_nx_pointer, x, y );
    wine_nx_pointer_sent_x = x;
    wine_nx_pointer_sent_y = y;
    x = (int)wine_nx_pointer.x;
    y = (int)wine_nx_pointer.y;
    pthread_mutex_unlock( &wine_nx_pointer_mutex );
    wine_nx_cursor_move( x, y );
}

static int call_pe_entry_point( void *entry )
{
    extern void wine_nx_set_active_pe_teb( TEB *teb );
    uintptr_t ret;
    uintptr_t teb = (uintptr_t)NtCurrentTeb();

    wine_nx_set_active_pe_teb( (TEB *)teb );
    __asm__ volatile(
        "mov x16, %[entry]\n\t"
        "mov x17, %[teb]\n\t"
        "mov x20, x18\n\t"
        "mov x18, x17\n\t"
        "blr x16\n\t"
        "mov x18, x20\n\t"
        "mov %[ret], x0\n\t"
        : [ret] "=r"(ret)
        : [entry] "r"(entry), [teb] "r"(teb)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
          "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x20",
          "x30", "memory", "cc" );

    return (int)ret;
}

static void park_forever(void)
{
    log_line( "[EXIT] parked after runtime handoff; close from HOME" );
    for (;;) svcSleepThread( 1000000000LL );
}

static void trim_line( char *line )
{
    size_t len = strlen( line );

    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                   line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = 0;
}

/* The program's standard output and error are files (see
 * runtime_open_std_file). NtWriteFile hands every write to them to
 * wine_nx_runtime_std_write, which copies it into this log line by line. */
struct std_stream
{
    const char *path;
    const char *tag;
    struct std_stream_lines lines;
};

static struct std_stream std_streams[] =
{
    { .path = RUNTIME_DIR "/stdout.txt", .tag = "STDOUT" },
    { .path = RUNTIME_DIR "/stderr.txt", .tag = "STDERR" },
};
static pthread_mutex_t std_stream_mutex = PTHREAD_MUTEX_INITIALIZER;

#define STD_STREAM_LOG_LINES 2000
#define STD_STREAM_IDLE_TICKS 5  /* flusher ticks (1 s) before a partial line is shown */

static void std_stream_log( void *ctx, const char *line )
{
    struct std_stream *stream = ctx;

    if (stream->lines.lines < STD_STREAM_LOG_LINES) log_line( "[%s] %s", stream->tag, line );
    else if (stream->lines.lines == STD_STREAM_LOG_LINES)
        log_line( "[%s] (further output only in %s)", stream->tag, stream->path );
}

/* stream: 1 standard output, 2 standard error (horizon_mark_std_stream). */
void wine_nx_runtime_std_write( int stream, const char *data, size_t size )
{
    struct std_stream *target;

    if (stream < 1 || stream > 2) return;
    target = &std_streams[stream - 1];
    pthread_mutex_lock( &std_stream_mutex );
    std_stream_lines_feed( &target->lines, data, size, std_stream_log, target );
    pthread_mutex_unlock( &std_stream_mutex );
}

static void runtime_tick_std_streams(void)
{
    unsigned int i;

    pthread_mutex_lock( &std_stream_mutex );
    for (i = 0; i < sizeof(std_streams) / sizeof(std_streams[0]); i++)
        std_stream_lines_tick( &std_streams[i].lines, STD_STREAM_IDLE_TICKS, std_stream_log, &std_streams[i] );
    pthread_mutex_unlock( &std_stream_mutex );
}

/* Called from NtTerminateProcess before the final lifecycle report. */
void wine_nx_runtime_dump_std_streams(void)
{
    unsigned int i;

    pthread_mutex_lock( &std_stream_mutex );
    for (i = 0; i < sizeof(std_streams) / sizeof(std_streams[0]); i++)
    {
        struct std_stream *stream = &std_streams[i];
        struct stat st;
        int rc;

        std_stream_lines_emit( &stream->lines, std_stream_log, stream );
        /* Plain stat() of a file still open for writing fails (EIO in console-3);
         * record the file system's result code and the fallback Wine now uses. */
        errno = 0;
        rc = stat( stream->path, &st );
        log_line( "[STDIO] %s bytes=%llu lines=%u; stat while open: rc=%d errno=%d fs=0x%x; open-file stat=%d",
                  stream->tag, stream->lines.bytes, stream->lines.lines, rc, errno,
                  rc ? (unsigned int)fsdevGetLastResult() : 0, horizon_stat_open_file( stream->path, &st ) );
    }
    pthread_mutex_unlock( &std_stream_mutex );
}

/* Present only in runtimes linked with the Box64 interpreter. */
extern ULONGLONG wine_nx_box64_executed_total __attribute__((weak));
extern ULONGLONG wine_nx_box64_runs_total __attribute__((weak));

/* Guest instruction throughput since the previous report. */
static void runtime_report_interpreter(void)
{
    static ULONGLONG last_executed, last_runs;
    static u64 last_tick;
    ULONGLONG executed, runs;
    u64 now = armGetSystemTick();
    double seconds;

    if (!wine_nx_runtime_verbose)
    {
        /* Without verbose traces a white screen says nothing about whether a
         * program is still loading, computing or drawing. Every 10 seconds, if
         * anything changed: completed file reads and the time inside NtReadFile,
         * read requests to the SD card, their time and the reads the cache
         * served, system calls, frames shown and dynarec entries. */
        extern unsigned int wine_nx_file_reads __attribute__((weak));
        extern unsigned long long wine_nx_file_read_100ns __attribute__((weak));
        extern unsigned int wine_nx_syscalls __attribute__((weak));
        extern unsigned int wine_nx_audio_underruns __attribute__((weak));
        extern unsigned int wine_nx_sd_reads, wine_nx_sd_hits;
        extern unsigned long long wine_nx_sd_read_ns;
        extern unsigned int wine_nx_gl_swaps __attribute__((weak)), wine_nx_gl_calls __attribute__((weak));
        extern unsigned int wine_nx_vk_presents __attribute__((weak));
        extern unsigned int wine_nx_gl_persistent_failures __attribute__((weak));
        extern unsigned long long wine_nx_gl_swap_time __attribute__((weak)), wine_nx_gl_call_time __attribute__((weak));
        extern unsigned long long wine_nx_gl_copy_bytes __attribute__((weak));
        extern void wine_nx_gl_profile( char *buffer, size_t size ) __attribute__((weak));
        extern unsigned long long wine_nx_nouveau_fence_wait_ns __attribute__((weak));
        extern unsigned int wine_nx_nouveau_tex_direct __attribute__((weak)), wine_nx_nouveau_tex_staging __attribute__((weak));
        extern unsigned int wine_nx_nouveau_buf_readback __attribute__((weak)), wine_nx_nouveau_fence_waits __attribute__((weak));
        extern unsigned int wine_nx_nouveau_pinned_buffers __attribute__((weak));
        extern unsigned int wine_nx_nouveau_wrap_result __attribute__((weak));
        extern unsigned int wine_nx_nouveau_bo_new __attribute__((weak)), wine_nx_nouveau_bo_reused __attribute__((weak));
        extern unsigned int wine_nx_nouveau_bo_evicted __attribute__((weak));
        extern unsigned long long wine_nx_nouveau_bo_new_ns __attribute__((weak));
        extern unsigned int wine_nx_nouveau_cache_cleans __attribute__((weak));
        extern unsigned long long wine_nx_nouveau_cache_clean_ns __attribute__((weak));
        extern unsigned long long wine_nx_nouveau_cache_clean_bytes __attribute__((weak));
        extern unsigned int wine_nx_gl_explicit_flushes __attribute__((weak));
        extern int wine_nx_gl_pinned_memory __attribute__((weak));
        extern unsigned int wine_nx_syscall_counts[] __attribute__((weak));
        static unsigned int calls, last_reads = ~0u, last_frames = ~0u;
        static u64 start;
        unsigned int reads = &wine_nx_file_reads ? __atomic_load_n( &wine_nx_file_reads, __ATOMIC_RELAXED ) : 0;
        unsigned int gl_frames = &wine_nx_gl_swaps ? __atomic_load_n( &wine_nx_gl_swaps, __ATOMIC_RELAXED ) : 0;
        unsigned int frames = __atomic_load_n( &wine_nx_fb_frames, __ATOMIC_RELAXED ) + gl_frames +
                              wine_nx_compositor_frames() +
                              (&wine_nx_vk_presents ? __atomic_load_n( &wine_nx_vk_presents, __ATOMIC_RELAXED ) : 0);
        unsigned long long read_ms = &wine_nx_file_read_100ns
                                     ? __atomic_load_n( &wine_nx_file_read_100ns, __ATOMIC_RELAXED ) / 10000 : 0;
        unsigned int syscalls = &wine_nx_syscalls ? __atomic_load_n( &wine_nx_syscalls, __ATOMIC_RELAXED ) : 0;
        char native[256] = "", gl[512] = "", audio[32] = "", systop[64] = "";

        if (!start) start = now;
        if (++calls % 2) return;
        if (reads == last_reads && frames == last_frames) return;
        last_reads = reads;
        last_frames = frames;
        /* The three system calls made most since the last line, as id:calls: a
         * program's busy loop shows here without verbose traces. */
        if (wine_nx_syscall_counts)
        {
            static unsigned int last_counts[0x2000];
            unsigned int best_id[3] = {0}, best_n[3] = {0}, id, k, j;
            int len;

            for (id = 0; id < 0x2000; id++)
            {
                unsigned int count = __atomic_load_n( &wine_nx_syscall_counts[id], __ATOMIC_RELAXED );
                unsigned int n = count - last_counts[id];

                last_counts[id] = count;
                for (k = 0; k < 3; k++)
                {
                    if (n <= best_n[k]) continue;
                    for (j = 2; j > k; j--)
                    {
                        best_n[j] = best_n[j - 1];
                        best_id[j] = best_id[j - 1];
                    }
                    best_n[k] = n;
                    best_id[k] = id;
                    break;
                }
            }
            len = snprintf( systop, sizeof(systop), " sys_top=" );
            for (k = 0; k < 3 && best_n[k] && len > 0 && len < (int)sizeof(systop); k++)
                len += snprintf( systop + len, sizeof(systop) - len, "%s%x:%u", k ? "," : "", best_id[k], best_n[k] );
            if (!best_n[0]) systop[0] = 0;
        }
#ifdef WINE_NX_BOX64_DYNAREC
        {
            extern unsigned long long wine_nx_box64_native_entries;
            extern unsigned int wine_nx_box64_block_tests;
            extern unsigned int wine_nx_box64_invalidations, wine_nx_box64_marked_lookups;
            extern unsigned int wine_nx_box64_callret_clean, wine_nx_box64_callret_dirty;
            extern unsigned int wine_nx_box64_translator_locks, wine_nx_box64_inline_unix_calls;
            extern uint64_t wine_nx_box64_dynarec_bytes, wine_nx_box64_arena_bytes;
            snprintf( native, sizeof(native), " native_entries=%llu block_tests=%u invalidations=%u marked_lookups=%u"
                      " callret_clean=%u callret_dirty=%u translator_locks=%u inline_unix=%u code_mb=%llu/%llu",
                      __atomic_load_n( &wine_nx_box64_native_entries, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_block_tests, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_invalidations, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_marked_lookups, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_callret_clean, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_callret_dirty, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_translator_locks, __ATOMIC_RELAXED ),
                      __atomic_load_n( &wine_nx_box64_inline_unix_calls, __ATOMIC_RELAXED ),
                      (unsigned long long)(__atomic_load_n( &wine_nx_box64_dynarec_bytes, __ATOMIC_RELAXED ) >> 20),
                      (unsigned long long)(wine_nx_box64_arena_bytes >> 20) );
        }
#endif
        /* OpenGL: frames swapped and the time in eglSwapBuffers, calls into opengl32's unix
         * side and their time, megabytes copied to 32-bit buffer mappings, persistent
         * mappings refused, whether pinned memory works (1), was refused (-1) or is not
         * needed because a 32-bit address space keeps every mapping below 4 GB (2),
         * and the slowest opengl32 functions of the last 10 seconds. */
        if (gl_frames || (&wine_nx_gl_calls && wine_nx_gl_calls))
        {
            int len = snprintf( gl, sizeof(gl), " gl_frames=%u swap_ms=%llu gl_calls=%u gl_ms=%llu copy_mb=%llu persistent_fail=%u pinned=%d",
                                gl_frames, __atomic_load_n( &wine_nx_gl_swap_time, __ATOMIC_RELAXED ) / 10000,
                                __atomic_load_n( &wine_nx_gl_calls, __ATOMIC_RELAXED ),
                                __atomic_load_n( &wine_nx_gl_call_time, __ATOMIC_RELAXED ) / 10000,
                                (&wine_nx_gl_copy_bytes ? __atomic_load_n( &wine_nx_gl_copy_bytes, __ATOMIC_RELAXED ) : 0) >> 20,
                                &wine_nx_gl_persistent_failures ? wine_nx_gl_persistent_failures : 0,
                                &wine_nx_gl_pinned_memory ? wine_nx_gl_pinned_memory : 0 );
            /* Mesa's nouveau: texture transfers mapped in place or through staging
             * buffers, buffer reads through a GPU copy, waits for the GPU with their
             * time, pinned buffers created and nvservices' last refusal to pin. */
            if (&wine_nx_nouveau_tex_direct && len > 0 && len < (int)sizeof(gl))
                len += snprintf( gl + len, sizeof(gl) - len,
                                 " tex_direct=%u tex_staging=%u buf_readback=%u fence_waits=%u fence_ms=%llu pinned_bufs=%u pin_rc=%#x",
                                 wine_nx_nouveau_tex_direct, wine_nx_nouveau_tex_staging,
                                 wine_nx_nouveau_buf_readback, wine_nx_nouveau_fence_waits,
                                 wine_nx_nouveau_fence_wait_ns / 1000000,
                                 &wine_nx_nouveau_pinned_buffers ? wine_nx_nouveau_pinned_buffers : 0,
                                 &wine_nx_nouveau_wrap_result ? wine_nx_nouveau_wrap_result : 0 );
            /* Buffer objects created for the GPU, taken from the reuse cache instead,
             * destroyed to make room in it, and the time creating them (each costs a
             * heap block and nvservices calls). */
            if (&wine_nx_nouveau_bo_new && len > 0 && len < (int)sizeof(gl))
                len += snprintf( gl + len, sizeof(gl) - len,
                                 " bo_new=%u bo_reuse=%u bo_evict=%u bo_ms=%llu pin_cached=%d cleans=%u clean_ms=%llu clean_mb=%llu range_flushes=%u",
                                 wine_nx_nouveau_bo_new, wine_nx_nouveau_bo_reused,
                                 &wine_nx_nouveau_bo_evicted ? wine_nx_nouveau_bo_evicted : 0,
                                 wine_nx_nouveau_bo_new_ns / 1000000,
                                 &wine_nx_nouveau_pin_cached ? wine_nx_nouveau_pin_cached : 0,
                                 &wine_nx_nouveau_cache_cleans ? wine_nx_nouveau_cache_cleans : 0,
                                 &wine_nx_nouveau_cache_clean_ns ? wine_nx_nouveau_cache_clean_ns / 1000000 : 0,
                                 &wine_nx_nouveau_cache_clean_bytes ? wine_nx_nouveau_cache_clean_bytes / (1024 * 1024) : 0,
                                 &wine_nx_gl_explicit_flushes ? wine_nx_gl_explicit_flushes : 0 );
            if (&wine_nx_gl_profile && len > 0 && len < (int)sizeof(gl)) wine_nx_gl_profile( gl + len, sizeof(gl) - len );
        }
        /* Gaps in playback: audout ran out of queued frames. */
        if (&wine_nx_audio_underruns && wine_nx_audio_underruns)
            snprintf( audio, sizeof(audio), " audio_under=%u",
                      __atomic_load_n( &wine_nx_audio_underruns, __ATOMIC_RELAXED ) );
        /* The libnx heap backs everything: Wine's guest memory, the GPU's
         * buffers and translated code. Under a 32-bit address space it is only
         * the heap region (1 GiB, or 2 GiB without the alias region). Free is
         * what malloc holds unused plus what it has not taken from the heap. */
        struct mallinfo heap = mallinfo();
        extern char *fake_heap_start, *fake_heap_end;
        unsigned long long heap_size = (unsigned long long)(fake_heap_end - fake_heap_start);
        unsigned long long heap_free = heap.fordblks + (heap_size > heap.arena ? heap_size - heap.arena : 0);

        log_line( "[PROGRESS] %llus reads=%u read_ms=%llu sd_reads=%u sd_ms=%llu cache_hits=%u syscalls=%u "
                  "frames=%u heap_used_mb=%llu heap_free_mb=%llu%s%s%s%s",
                  (unsigned long long)(armTicksToNs( now - start ) / 1000000000ull), reads, read_ms,
                  __atomic_load_n( &wine_nx_sd_reads, __ATOMIC_RELAXED ),
                  __atomic_load_n( &wine_nx_sd_read_ns, __ATOMIC_RELAXED ) / 1000000,
                  __atomic_load_n( &wine_nx_sd_hits, __ATOMIC_RELAXED ), syscalls, frames,
                  (unsigned long long)heap.uordblks >> 20, heap_free >> 20, systop, native, gl, audio );
        {
            extern void wine_nx_thread_report( void );
            extern void horizon_memory_pool_stats( char *buffer, size_t size );
            char pool_stats[256];
            horizon_memory_pool_stats( pool_stats, sizeof(pool_stats) );
            log_line( "%s", pool_stats );
            wine_nx_thread_report();
        }
        return;
    }

#ifdef WINE_NX_BOX64_DYNAREC
    {
        extern unsigned long long wine_nx_box64_native_entries;
        extern uint64_t wine_nx_box64_dynarec_bytes;
        static unsigned long long last_entries = ~0ull;
        unsigned long long entries = __atomic_load_n( &wine_nx_box64_native_entries, __ATOMIC_RELAXED );

        /* Nothing to report in the launcher or once the program has parked. */
        if (entries != last_entries)
            log_line( "[DYNAREC] native_entries=%llu emitted_bytes=%llu", entries,
                      (unsigned long long)__atomic_load_n( &wine_nx_box64_dynarec_bytes, __ATOMIC_RELAXED ) );
        last_entries = entries;
    }
#endif
    if (!&wine_nx_box64_executed_total || !&wine_nx_box64_runs_total) return;
    executed = __atomic_load_n( &wine_nx_box64_executed_total, __ATOMIC_RELAXED );
    runs = __atomic_load_n( &wine_nx_box64_runs_total, __ATOMIC_RELAXED );
    if (last_tick && executed != last_executed)
    {
        seconds = armTicksToNs( now - last_tick ) / 1e9;
        log_line( "[BOX64] instructions=%llu (%.2fM/s) runs=%llu (%.0f/s)",
                  executed, (executed - last_executed) / seconds / 1e6,
                  runs, (runs - last_runs) / seconds );
    }
    last_executed = executed;
    last_runs = runs;
    last_tick = now;
}

static int read_first_line( const char *path, char *line, size_t size )
{
    FILE *file = fopen( path, "r" );

    if (!file) return 0;
    if (!fgets( line, size, file ))
    {
        fclose( file );
        return 0;
    }
    fclose( file );
    trim_line( line );
    return line[0] != 0;
}

static int read_bool_file( const char *path )
{
    char line[32];

    if (!read_first_line( path, line, sizeof(line) )) return 0;
    return !strcmp( line, "1" ) || !strcasecmp( line, "true" ) ||
           !strcasecmp( line, "yes" ) || !strcasecmp( line, "run" );
}

static struct wine_nx_config runtime_config;
static int runtime_config_moved;  /* a setting was found in the file it used to be */
/* Read at the start, wanted on the way out, when the card may be busy. */
static int runtime_loader_anyway, runtime_reopen_launcher, runtime_dxvk_on_add;

/* A setting, with the file it used to be for a card written by an earlier
 * build. The file is read only when the settings file has nothing to say, and
 * what it said is written into the settings file and the file itself taken
 * away: a card is moved over once and is tidy afterwards. A name here says
 * what it turns on, where half the files said what they turned off, so `flip`
 * marks the ones whose answer is the other way round. */
static int config_bool( const char *key, int fallback, const char *was, int flip )
{
    char path[512];

    if (wine_nx_config_find( &runtime_config, key ) >= 0)
        return wine_nx_config_bool( &runtime_config, key, fallback );
    snprintf( path, sizeof(path), "%s/%s", RUNTIME_DIR, was );
    if (!access( path, F_OK ))
    {
        int value = read_bool_file( path );

        if (flip) value = !value;
        wine_nx_config_set_bool( &runtime_config, key, value );
        runtime_config_moved = 1;
        remove( path );
        log_line( "[CONFIG] %s moved into settings.json as %s: %s", was, key, value ? "true" : "false" );
        return value;
    }
    wine_nx_config_set_bool( &runtime_config, key, fallback );
    return fallback;
}

/* switch/wine/keys.txt: one NAME=code line for each control whose key should
 * differ from the default, where code is a Windows virtual-key code, decimal or
 * 0x-prefixed. Unknown names and malformed lines are reported and skipped, so a
 * typo costs one control rather than the file. */
static void read_key_map( const char *path )
{
    char line[80];
    FILE *file = fopen( path, "r" );
    unsigned int changed = 0;

    if (!file) return;
    while (fgets( line, sizeof(line), file ))
    {
        char *equals, *name = line, *value;
        unsigned int i;
        int c;

        /* Drop the rest of a line longer than the buffer: the tail of a long
         * comment must not be read as a control. */
        if (!strchr( line, '\n' ) && !feof( file ))
            while ((c = fgetc( file )) != EOF && c != '\n') {}
        trim_line( line );
        if (!line[0] || line[0] == '#') continue;
        if (!(equals = strchr( line, '=' )))
        {
            log_line( "[NXINPUT] %s: no '=' in '%s'", path, line );
            continue;
        }
        *equals = 0;
        value = equals + 1;
        while (*name == ' ') name++;
        while (*value == ' ') value++;
        /* The three that point say what they do rather than which key they
         * are: LSTICK=mouse, RSTICK=keys. */
        for (i = 0; i < WINE_NX_DEVICE_COUNT; i++)
            if (!strcasecmp( name, wine_nx_device_names[i] ))
            {
                if (!strcasecmp( value, "mouse" )) wine_nx_device_mode[i] = WINE_NX_POINTS;
                else if (!strcasecmp( value, "keys" )) wine_nx_device_mode[i] = WINE_NX_PRESSES;
                else
                {
                    log_line( "[NXINPUT] %s: %s is mouse or keys, not '%s'", path, name, value );
                    break;
                }
                changed++;
                break;
            }
        if (i < WINE_NX_DEVICE_COUNT) continue;
        for (i = 0; i < WINE_NX_KEY_COUNT; i++)
            if (!strcasecmp( name, wine_nx_pad_key_names[i] ))
            {
                wine_nx_pad_keys[i] = (unsigned short)strtoul( value, NULL, 0 );
                changed++;
                break;
            }
        if (i == WINE_NX_KEY_COUNT) log_line( "[NXINPUT] %s: unknown control '%s'", path, name );
    }
    fclose( file );
    log_line( "[NXINPUT] %s: %u controls remapped", path, changed );
}

static unsigned int close_handle_object( HANDLE handle )
{
    unsigned int status;

    SERVER_START_REQ( close_handle )
    {
        req->handle = wine_server_obj_handle( handle );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int runtime_init_process_done( BOOL *suspend )
{
    unsigned int status;

    SERVER_START_REQ( init_process_done )
    {
        req->teb = wine_server_client_ptr( NtCurrentTeb() );
        req->peb = wine_server_client_ptr( NtCurrentTeb()->Peb );
        status = wine_server_call( req );
        if (suspend) *suspend = !status && reply->suspend;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int runtime_open_exe( const char *path, HANDLE *handle )
{
    OBJECT_ATTRIBUTES attr;

    memset( &attr, 0, sizeof(attr) );
    attr.Length = sizeof(attr);
    return open_unix_file( handle, path, FILE_READ_DATA | SYNCHRONIZE, &attr,
                           FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
}

static int file_exists( const char *path )
{
    struct stat st;

    return !stat( path, &st ) && S_ISREG( st.st_mode );
}

static const char *path_basename( const char *path )
{
    const char *slash = strrchr( path, '/' );
    const char *backslash = strrchr( path, '\\' );

    if (!slash || backslash > slash) slash = backslash;
    return slash ? slash + 1 : path;
}

static void path_dirname( const char *path, char *dir, size_t size )
{
    const char *base = path_basename( path );
    size_t len = base > path ? (size_t)(base - path - 1) : 0;

    if (!len)
    {
        snprintf( dir, size, "%s", WINE_DRIVE_C );
        return;
    }
    if (len >= size) len = size - 1;
    memcpy( dir, path, len );
    dir[len] = 0;
}

static int join_path( char *out, size_t size, const char *dir, const char *name )
{
    int ret = snprintf( out, size, "%s/%s", dir, name );

    return ret > 0 && (size_t)ret < size;
}

static void slash_to_backslash( char *path )
{
    for (; *path; path++) if (*path == '/') *path = '\\';
}

static int target_to_dos_path( const char *target, char *dos_path, size_t size )
{
    int ret;

    if (strlen( target ) > 2 && target[1] == ':')
    {
        ret = snprintf( dos_path, size, "%s", target );
        if (ret <= 0 || (size_t)ret >= size) return 0;
        slash_to_backslash( dos_path );
        return 1;
    }

    /* A file on the card or a USB drive, as file.c maps them. */
    if (!strncmp( target, "sdmc:", 5 ) || !strncmp( target, "ums", 3 ))
        return launcher_dos_path( target, dos_path, size );
    ret = snprintf( dos_path, size, "C:\\%s", path_basename( target ) );

    if (ret <= 0 || (size_t)ret >= size) return 0;
    slash_to_backslash( dos_path );
    return 1;
}

static void dos_dirname( const char *path, char *dir, size_t size )
{
    const char *slash = strrchr( path, '\\' );
    size_t len;

    if (!slash)
    {
        snprintf( dir, size, "C:\\" );
        return;
    }
    len = slash - path;
    if (len < 3) len = 3;
    if (len >= size) len = size - 1;
    memcpy( dir, path, len );
    dir[len] = 0;
}

static void put_process_string( WCHAR **cursor, UNICODE_STRING *string, const char *value )
{
    size_t i, len = value ? strlen( value ) : 0;

    string->Buffer = *cursor;
    string->Length = len * sizeof(WCHAR);
    string->MaximumLength = (len + 1) * sizeof(WCHAR);
    for (i = 0; i < len; i++) (*cursor)[i] = (unsigned char)value[i];
    (*cursor)[len] = 0;
    *cursor += len + 1;
}

/* Minimal environment (sorted, NUL-separated; the literal's own terminator
 * ends the block). Console programs and Wine's DLLs look these up. The user
 * profile is where programs keep saves and settings, and where DXVK keeps
 * its shader cache (LOCALAPPDATA); its directories are made at start-up. */
static const char runtime_environment[] =
    "APPDATA=C:\\users\\wine\\AppData\\Roaming\0"
    "DXVK_CONFIG_FILE=C:\\users\\wine\\AppData\\Local\\Autorun\\dxvk.conf\0"
    "DXVK_HUD=0\0"
    "HOMEDRIVE=C:\0"
    "HOMEPATH=\\users\\wine\0"
    "LOCALAPPDATA=C:\\users\\wine\\AppData\\Local\0"
    "PATH=C:\\windows\\system32;C:\\windows\0"
    "SystemDrive=C:\0"
    "SystemRoot=C:\\windows\0"
    "TEMP=C:\\windows\\temp\0"
    "TMP=C:\\windows\\temp\0"
    "USERNAME=wine\0"
    "USERPROFILE=C:\\users\\wine\0"
    "windir=C:\\windows\0"
    "WINE_D3D_CONFIG=cs_spin_count=64,explicit_buffer_flush=1\0";

/* Horizon has no console device: the standard handles are files next to the
 * runtime, copied into this log when the process exits. */
static HANDLE runtime_open_std_file( const char *path, ACCESS_MASK access, ULONG disposition )
{
    OBJECT_ATTRIBUTES attr;
    HANDLE handle = 0;

    memset( &attr, 0, sizeof(attr) );
    attr.Length = sizeof(attr);
    attr.Attributes = OBJ_INHERIT;
    if (open_unix_file( &handle, path, access | SYNCHRONIZE, &attr, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0 ))
        return 0;
    return handle;
}

static RTL_USER_PROCESS_PARAMETERS *runtime_create_process_params( const char *target,
                                                                   UNICODE_STRING *main_nt_name,
                                                                   char *dos_path, size_t dos_path_size )
{
    RTL_USER_PROCESS_PARAMETERS *params;
    char nt_path[640], dll_path[1024], current_dir[512];
    char cmdline[1024], args_buf[896], args_path[512];
    size_t chars, size, i;
    WCHAR *cursor;
    const char *cmdline_str, *dxvk_hud = launcher_hud_values[runtime_dxvk_hud];
    char dxvk_dir[96], vkd3d_dir[96], vkd3d_path[104] = "", graphics_path[208] = "";
    int dxvk_path = launcher_dxvk_version_directory( main_image_info.Machine, runtime_dxvk_version,
                                                     dxvk_dir, sizeof(dxvk_dir) );

    if (!target_to_dos_path( target, dos_path, dos_path_size )) return NULL;
    dos_dirname( dos_path, current_dir, sizeof(current_dir) );
    snprintf( nt_path, sizeof(nt_path), "\\??\\%s", dos_path );
    if (runtime_dxvk &&
        launcher_vkd3d_version_directory( main_image_info.Machine, runtime_vkd3d_version,
                                          vkd3d_dir, sizeof(vkd3d_dir) ) &&
        vkd3d_release_installed( RUNTIME_DIR, main_image_info.Machine, runtime_vkd3d_version ))
    {
        snprintf( vkd3d_path, sizeof(vkd3d_path), "C:\\%s;", vkd3d_dir );
        log_line( "[VKD3D] payload C:\\%s; application-local DLLs take priority", vkd3d_dir );
    }
    /* Keep native DXVK DLLs separate for each guest architecture. */
    if (runtime_dxvk && dxvk_path &&
        dxvk_release_installed( RUNTIME_DIR, main_image_info.Machine, runtime_dxvk_version ))
    {
        snprintf( graphics_path, sizeof(graphics_path), "%sC:\\%s;", vkd3d_path, dxvk_dir );
        if (runtime_dxvk_version[0])
            log_line( "[DXVK] %s payload C:\\%s (version %s); application-local DLLs take priority",
                      main_image_info.Machine == IMAGE_FILE_MACHINE_AMD64 ? "AMD64" : "x86", dxvk_dir,
                      runtime_dxvk_version );
        else
            log_line( "[DXVK] %s bundled payload C:\\%s; application-local DLLs take priority",
                      main_image_info.Machine == IMAGE_FILE_MACHINE_AMD64 ? "AMD64" : "x86", dxvk_dir );
    }
    else if (runtime_dxvk) log_line( "[DXVK] selected payload is not installed; using Wine Direct3D" );
    snprintf( dll_path, sizeof(dll_path), "%s;%sC:\\windows\\system32;C:\\windows;C:\\",
              current_dir, graphics_path );
    /* The current directory ends in a backslash, as RtlSetCurrentDirectory_U
     * stores it; relative paths are appended to it directly. */
    if ((chars = strlen( current_dir )) && current_dir[chars - 1] != '\\' && chars + 1 < sizeof(current_dir))
        memcpy( current_dir + chars, "\\", 2 );

    /* Read args.txt next to the target NRO (sdmc:/switch/wine/args.txt).
     * Format expected: "<argv[0]> <args...>" — a full Win32 command line.
     * If present, use it verbatim as CommandLine so curl etc. see args via
     * GetCommandLineA/W. Otherwise fall back to the dos_path alone. */
    /* A program's own controls, over the shared ones: SPEED2.EXE reads
     * SPEED2.keys.txt, unless its settings say to use Autorun's alone. The
     * file is left where it is either way, so turning it back on brings back
     * the keys that were set rather than the defaults. */
    {
        char keys_path[512], settings_path[520];
        struct launcher_settings settings;
        struct launcher_kv kv;
        int own = -1;

        if (target[1] != ':' &&
            launcher_program_settings_path( RUNTIME_DIR, target, settings_path, sizeof(settings_path) ) &&
            launcher_kv_load( &kv, settings_path ) && kv.size)
        {
            launcher_settings_read( &kv, &settings );
            own = settings.own_controls;
        }
        if (own != 0 && target[1] != ':' && launcher_keys_path( target, keys_path, sizeof(keys_path) ))
            read_key_map( keys_path );
        else if (own == 0) log_line( "[NXINPUT] %s: Autorun's controls, not its own", target );
    }
    /* Its own Box64 options, read when its first x86 code runs: SPEED2.box64.txt. */
    {
        extern char wine_nx_box64_options_path[] __attribute__((weak));

        if (&wine_nx_box64_options_path && target[1] != ':')
            launcher_sibling_path( target, ".box64.txt", wine_nx_box64_options_path, 512 );
    }

    cmdline_str = dos_path;
    if (target[1] != ':' && launcher_args_path( target, args_path, sizeof(args_path) ) &&
        read_first_line( args_path, args_buf, sizeof(args_buf) ) &&
        launcher_command_line( dos_path, args_buf, cmdline, sizeof(cmdline) ))
    {
        cmdline_str = cmdline;
        log_line( "[ARGS] from %s; CommandLine='%s'", args_path, cmdline_str );
    }
    else if (!read_first_line( RUNTIME_DIR "/args.txt", args_buf, sizeof(args_buf) ) || !args_buf[0])
        log_line( "[ARGS] no args.txt; CommandLine='%s'", cmdline_str );
    else if (!launcher_args_match( args_buf, dos_path ))
        log_line( "[ARGS] args.txt is for another program; CommandLine='%s'", cmdline_str );
    else
    {
        snprintf( cmdline, sizeof(cmdline), "%s", args_buf );
        cmdline_str = cmdline;
        log_line( "[ARGS] CommandLine='%s'", cmdline_str );
    }

    chars = strlen( current_dir ) + 1;
    chars += strlen( dll_path ) + 1;
    chars += strlen( dos_path ) + 1;
    chars += strlen( cmdline_str ) + 1;
    chars += strlen( dos_path ) + 1;
    chars += strlen( nt_path ) + 1;
    chars += sizeof(runtime_environment) + strlen( graphics_path ) + strlen( dxvk_hud ) - 1;
    size = sizeof(*params) + chars * sizeof(WCHAR);

    if (!(params = calloc( 1, size ))) return NULL;
    params->AllocationSize = size;
    params->Size = size;
    params->Flags = PROCESS_PARAMS_FLAG_NORMALIZED;
    /* The Switch runtime presents one foreground desktop application.  Use
     * the standard Win32 startup hint so applications maximize their own
     * top-level window while dialogs and child windows keep normal sizing. */
    params->dwFlags = STARTF_USESHOWWINDOW;
    params->wShowWindow = SW_SHOWMAXIMIZED;
    params->ProcessGroupId = GetCurrentProcessId();

    cursor = (WCHAR *)(params + 1);
    put_process_string( &cursor, &params->CurrentDirectory.DosPath, current_dir );
    put_process_string( &cursor, &params->DllPath, dll_path );
    put_process_string( &cursor, &params->ImagePathName, dos_path );
    put_process_string( &cursor, &params->CommandLine, cmdline_str );
    put_process_string( &cursor, &params->WindowTitle, dos_path );
    put_process_string( &cursor, main_nt_name, nt_path );
    params->Environment = cursor;
    for (const char *entry = runtime_environment; *entry; entry += strlen( entry ) + 1)
    {
        const char *value = entry;

        if (!runtime_dxvk && !strncmp( entry, "DXVK_", 5 )) continue;
        if (!strncmp( entry, "DXVK_HUD=", 9 ))
        {
            for (i = 0; i < 9; i++) *cursor++ = (unsigned char)*value++;
            value = dxvk_hud;
        }
        if (!strncmp( entry, "PATH=", 5 ))
        {
            for (i = 0; i < 5; i++) *cursor++ = (unsigned char)*value++;
            for (i = 0; graphics_path[i]; i++) *cursor++ = (unsigned char)graphics_path[i];
        }
        do *cursor++ = (unsigned char)*value; while (*value++);
    }
    *cursor++ = 0;
    params->EnvironmentSize = (cursor - (WCHAR *)params->Environment) * sizeof(WCHAR);

    params->hStdInput = runtime_open_std_file( RUNTIME_DIR "/stdin.txt", GENERIC_READ, FILE_OPEN_IF );
    params->hStdOutput = runtime_open_std_file( RUNTIME_DIR "/stdout.txt", GENERIC_WRITE, FILE_OVERWRITE_IF );
    params->hStdError = runtime_open_std_file( RUNTIME_DIR "/stderr.txt", GENERIC_WRITE, FILE_OVERWRITE_IF );
    log_line( "[STDIO] stdin=%p stdout=%p stderr=%p (" RUNTIME_DIR "/std*.txt)",
              params->hStdInput, params->hStdOutput, params->hStdError );
    horizon_mark_std_stream( params->hStdOutput, 1 );
    horizon_mark_std_stream( params->hStdError, 2 );
    return params;
}

static void runtime_init_peb_process( TEB *teb, void *module,
                                      RTL_USER_PROCESS_PARAMETERS *params )
{
    PEB *peb = teb->Peb;

    peb->ImageBaseAddress           = module;
    peb->ProcessParameters          = params;
    peb->OSMajorVersion             = 10;
    peb->OSMinorVersion             = 0;
    peb->OSBuildNumber              = 19045;
    peb->OSPlatformId               = VER_PLATFORM_WIN32_NT;
    peb->ImageSubSystem             = main_image_info.SubSystemType;
    peb->ImageSubSystemMajorVersion = main_image_info.MajorSubsystemVersion;
    peb->ImageSubSystemMinorVersion = main_image_info.MinorSubsystemVersion;
}

static int dll_name_matches( const char *loaded, const char *wanted )
{
    return !strcasecmp( loaded, wanted );
}

static struct runtime_module *find_module_by_name( const char *name )
{
    unsigned int i;

    for (i = 0; i < module_count; i++)
        if (dll_name_matches( modules[i].name, name )) return &modules[i];
    return NULL;
}

static void *rva_ptr( const struct runtime_module *module, DWORD rva, SIZE_T bytes )
{
    if (!rva || rva >= module->size) return NULL;
    if (bytes > module->size - rva) return NULL;
    return (char *)module->base + rva;
}

static IMAGE_NT_HEADERS64 *runtime_nt_headers( void *module )
{
    IMAGE_DOS_HEADER *dos = module;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    return (IMAGE_NT_HEADERS64 *)((char *)module + dos->e_lfanew);
}

static unsigned int map_pe_image( const char *path, void **module, SIZE_T *view_size )
{
    HANDLE file = 0, section = 0;
    unsigned int status;

    *module = NULL;
    *view_size = 0;

    status = runtime_open_exe( path, &file );
    if (status) return status;

    status = NtCreateSection( &section, SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_QUERY,
                              NULL, NULL, PAGE_EXECUTE_READ, SEC_IMAGE, file );
    if (!status)
    {
        status = NtMapViewOfSection( section, NtCurrentProcess(), module, 0, 0, NULL,
                                     view_size, ViewShare, 0, PAGE_EXECUTE_READ );
        if (status == STATUS_IMAGE_NOT_AT_BASE) status = STATUS_SUCCESS;
        close_handle_object( section );
    }
    close_handle_object( file );
    return status;
}

static struct runtime_module *register_module( const char *path, void *base, SIZE_T size, int is_main )
{
    struct runtime_module *module;
    IMAGE_NT_HEADERS64 *nt;

    if (module_count >= MAX_RUNTIME_MODULES)
    {
        log_line( "[FAIL] module table full" );
        return NULL;
    }

    nt = runtime_nt_headers( base );
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        log_line( "[FAIL] %s mapped image does not look like PE32+", path );
        return NULL;
    }

    module = &modules[module_count++];
    memset( module, 0, sizeof(*module) );
    snprintf( module->path, sizeof(module->path), "%s", path );
    snprintf( module->name, sizeof(module->name), "%s", path_basename( path ) );
    path_dirname( path, module->dir, sizeof(module->dir) );
    module->base = base;
    module->size = size;
    module->nt = nt;
    module->is_main = is_main;

    log_line( "[LOAD] %s base=%p size=0x%lx entry=0x%x",
              module->name, module->base, (unsigned long)module->size,
              module->nt->OptionalHeader.AddressOfEntryPoint );
    return module;
}

static int find_dll_path( const struct runtime_module *parent, const char *dll, char *path, size_t size )
{
    if (parent && join_path( path, size, parent->dir, dll ) && file_exists( path )) return 1;
    if (join_path( path, size, WINE_SYSTEM_DIR, dll ) && file_exists( path )) return 1;
    if (join_path( path, size, WINE_DRIVE_C, dll ) && file_exists( path )) return 1;
    if (join_path( path, size, RUNTIME_DIR, dll ) && file_exists( path )) return 1;
    return 0;
}

static struct runtime_module *load_dll_module( const struct runtime_module *parent, const char *dll,
                                               struct import_stats *stats )
{
    char path[512];
    void *base;
    SIZE_T size;
    unsigned int status;
    struct runtime_module *module;

    if ((module = find_module_by_name( dll ))) return module;
    if (!find_dll_path( parent, dll, path, sizeof(path) ))
    {
        log_line( "[MISS] DLL %s not found in local runtime paths", dll );
        stats->missing_dlls++;
        return NULL;
    }

    status = map_pe_image( path, &base, &size );
    if (status)
    {
        log_line( "[FAIL] load DLL %s status=%08x", path, status );
        stats->missing_dlls++;
        return NULL;
    }

    module = register_module( path, base, size, 0 );
    if (module) stats->loaded_dlls++;
    return module;
}

static void *resolve_forwarder( const struct runtime_module *parent, const char *forwarder,
                                struct import_stats *stats, int depth );

static void *resolve_export( const struct runtime_module *module, const char *name, WORD ordinal,
                             const struct runtime_module *parent, struct import_stats *stats, int depth )
{
    const IMAGE_DATA_DIRECTORY *dir = &module->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *exports;
    DWORD *functions, *names;
    WORD *ordinals;
    DWORD function_rva = 0;
    DWORD index;
    unsigned int i;

    if (!dir->VirtualAddress || !dir->Size) return NULL;
    exports = rva_ptr( module, dir->VirtualAddress, sizeof(*exports) );
    if (!exports) return NULL;

    functions = rva_ptr( module, exports->AddressOfFunctions, exports->NumberOfFunctions * sizeof(*functions) );
    names = rva_ptr( module, exports->AddressOfNames, exports->NumberOfNames * sizeof(*names) );
    ordinals = rva_ptr( module, exports->AddressOfNameOrdinals, exports->NumberOfNames * sizeof(*ordinals) );
    if (!functions || (!names && exports->NumberOfNames) || (!ordinals && exports->NumberOfNames)) return NULL;

    if (name)
    {
        for (i = 0; i < exports->NumberOfNames; i++)
        {
            const char *export_name = rva_ptr( module, names[i], 1 );

            if (!export_name || strcmp( export_name, name )) continue;
            index = ordinals[i];
            if (index >= exports->NumberOfFunctions) return NULL;
            function_rva = functions[index];
            break;
        }
        if (!function_rva) return NULL;
    }
    else
    {
        if (ordinal < exports->Base) return NULL;
        index = ordinal - exports->Base;
        if (index >= exports->NumberOfFunctions) return NULL;
        function_rva = functions[index];
    }

    if (function_rva >= dir->VirtualAddress && function_rva < dir->VirtualAddress + dir->Size)
    {
        const char *forwarder = rva_ptr( module, function_rva, 1 );

        if (!forwarder) return NULL;
        stats->forwarded++;
        return resolve_forwarder( parent, forwarder, stats, depth + 1 );
    }

    return rva_ptr( module, function_rva, 1 );
}

static void *resolve_forwarder( const struct runtime_module *parent, const char *forwarder,
                                struct import_stats *stats, int depth )
{
    char dll[128], name[128];
    const char *dot = strrchr( forwarder, '.' );
    struct runtime_module *module;

    if (!dot || dot == forwarder || depth > MAX_IMPORT_DEPTH) return NULL;
    if ((size_t)(dot - forwarder) >= sizeof(dll)) return NULL;
    memcpy( dll, forwarder, dot - forwarder );
    dll[dot - forwarder] = 0;
    if (!strchr( dll, '.' )) strncat( dll, ".dll", sizeof(dll) - strlen(dll) - 1 );
    snprintf( name, sizeof(name), "%s", dot + 1 );

    module = load_dll_module( parent, dll, stats );
    if (!module) return NULL;
    if (name[0] == '#') return resolve_export( module, NULL, (WORD)strtoul( name + 1, NULL, 10 ),
                                               parent, stats, depth + 1 );
    return resolve_export( module, name, 0, parent, stats, depth + 1 );
}

static int write_iat_entry( ULONGLONG *slot, void *value )
{
    void *protect_base = (void *)((uintptr_t)slot & ~(uintptr_t)0xfff);
    SIZE_T protect_size = ((uintptr_t)slot - (uintptr_t)protect_base) + sizeof(*slot);
    ULONG old_protect = 0;
    unsigned int status;

    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size,
                                     PAGE_READWRITE, &old_protect );
    if (status)
    {
        log_line( "[FAIL] NtProtectVirtualMemory(IAT) status=%08x", status );
        return 0;
    }

    *slot = (ULONGLONG)(uintptr_t)value;

    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size,
                                     old_protect, &old_protect );
    if (status) log_line( "[WARN] restore IAT protection status=%08x", status );
    return 1;
}

static void __attribute__((unused)) resolve_module_imports( struct runtime_module *module,
                                                            struct import_stats *stats, int depth )
{
    const IMAGE_DATA_DIRECTORY *dir = &module->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc;
    unsigned int desc_count = 0;

    if (module->imports_scanned || module->resolving_imports) return;
    if (depth > MAX_IMPORT_DEPTH)
    {
        log_line( "[MISS] import recursion limit at %s", module->name );
        stats->unresolved++;
        return;
    }

    module->resolving_imports = 1;
    if (!dir->VirtualAddress || !dir->Size)
    {
        module->imports_scanned = 1;
        module->resolving_imports = 0;
        return;
    }

    desc = rva_ptr( module, dir->VirtualAddress, sizeof(*desc) );
    if (!desc)
    {
        log_line( "[FAIL] invalid import directory in %s", module->name );
        stats->unresolved++;
        module->resolving_imports = 0;
        return;
    }

    for (; desc->Name || desc->FirstThunk || desc->OriginalFirstThunk; desc++, desc_count++)
    {
        const char *dll_name;
        IMAGE_THUNK_DATA64 *lookup, *iat;
        DWORD lookup_rva;
        struct runtime_module *dll_module;
        unsigned int thunk_count = 0;

        if (desc_count > 512)
        {
            log_line( "[FAIL] too many import descriptors in %s", module->name );
            stats->unresolved++;
            break;
        }

        dll_name = rva_ptr( module, desc->Name, 1 );
        if (!dll_name)
        {
            log_line( "[FAIL] invalid import DLL name in %s", module->name );
            stats->unresolved++;
            continue;
        }

        stats->dlls++;
        log_line( "[IMPORT] %s -> %s", module->name, dll_name );
        dll_module = load_dll_module( module, dll_name, stats );
        if (!dll_module)
        {
            stats->unresolved++;
            continue;
        }

        resolve_module_imports( dll_module, stats, depth + 1 );

        lookup_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        lookup = rva_ptr( module, lookup_rva, sizeof(*lookup) );
        iat = rva_ptr( module, desc->FirstThunk, sizeof(*iat) );
        if (!lookup || !iat)
        {
            log_line( "[FAIL] invalid thunk table for %s in %s", dll_name, module->name );
            stats->unresolved++;
            continue;
        }

        for (; lookup->u1.AddressOfData; lookup++, iat++, thunk_count++)
        {
            const char *import_name = NULL;
            WORD ordinal = 0;
            void *target;

            if (thunk_count > 8192)
            {
                log_line( "[FAIL] too many thunks for %s in %s", dll_name, module->name );
                stats->unresolved++;
                break;
            }

            stats->imports++;
            if (IMAGE_SNAP_BY_ORDINAL64( lookup->u1.Ordinal ))
            {
                ordinal = IMAGE_ORDINAL64( lookup->u1.Ordinal );
                target = resolve_export( dll_module, NULL, ordinal, module, stats, depth + 1 );
            }
            else
            {
                IMAGE_IMPORT_BY_NAME *by_name = rva_ptr( module, (DWORD)lookup->u1.AddressOfData,
                                                          sizeof(*by_name) );

                if (!by_name)
                {
                    log_line( "[MISS] invalid import name rva=0x%llx in %s",
                              (unsigned long long)lookup->u1.AddressOfData, module->name );
                    stats->unresolved++;
                    continue;
                }
                import_name = by_name->Name;
                target = resolve_export( dll_module, import_name, 0, module, stats, depth + 1 );
            }

            if (!target)
            {
                if (import_name) log_line( "[MISS] %s!%s", dll_name, import_name );
                else log_line( "[MISS] %s ordinal %u", dll_name, ordinal );
                stats->unresolved++;
                continue;
            }

            if (write_iat_entry( &iat->u1.Function, target ))
            {
                stats->bound++;
                if (import_name) log_line( "[BIND] %s!%s -> %p", dll_name, import_name, target );
                else log_line( "[BIND] %s ordinal %u -> %p", dll_name, ordinal, target );
            }
            else stats->unresolved++;
        }
    }

    module->imports_scanned = 1;
    module->resolving_imports = 0;
}

/* Establish the process machine before server initialization and SEC_IMAGE. */
static NTSTATUS runtime_target_machine( const char *path, USHORT *machine )
{
    IMAGE_DOS_HEADER dos;
    struct { DWORD signature; IMAGE_FILE_HEADER file; WORD magic; } nt;
    FILE *file = fopen( path, "rb" );
    NTSTATUS status = STATUS_INVALID_IMAGE_FORMAT;
    if (!file) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (fread( &dos, sizeof(dos), 1, file ) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew >= sizeof(dos) && !fseek( file, dos.e_lfanew, SEEK_SET ) &&
        fread( &nt, sizeof(nt), 1, file ) == 1 && nt.signature == IMAGE_NT_SIGNATURE)
    {
        if ((nt.file.Machine == IMAGE_FILE_MACHINE_ARM64 && nt.magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
#ifdef WINE_NX_BOX64_INTERPRETER
            || (nt.file.Machine == IMAGE_FILE_MACHINE_I386 && nt.magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
#endif
#ifdef WINE_NX_AMD64
            || (nt.file.Machine == IMAGE_FILE_MACHINE_AMD64 && nt.magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
#endif
           ) { *machine = nt.file.Machine; status = STATUS_SUCCESS; }
    }
    fclose( file );
    return status;
}

static int launcher_machine( const char *path, unsigned short *machine )
{
    return runtime_target_machine( path, machine ) != STATUS_SUCCESS;
}

#ifdef WINE_NX_BOX64_INTERPRETER
extern NTSTATUS wine_nx_init_wow64_peb( RTL_USER_PROCESS_PARAMETERS *, void * );
extern NTSTATUS wine_nx_prepare_wow64_ntdll( HMODULE, HMODULE );
extern NTSTATUS wine_nx_loader_prepare_wow64( HMODULE *, void ** );

static void *runtime_wow64_initialize;
extern void (*wine_nx_wow64_thread_start)( PRTL_THREAD_START_ROUTINE, void *, BOOL, TEB * );

static NTSTATUS runtime_init_x86_context( TEB *teb, void *entry, void *arg )
{
    I386_CONTEXT *ctx = get_cpu_area( IMAGE_FILE_MACHINE_I386 );
    XMM_SAVE_AREA32 fx = {0};
    if (!ctx || !get_wow_teb(teb) || !pLdrSystemDllInitBlock ||
        !pLdrSystemDllInitBlock->pRtlUserThreadStart || (ULONG_PTR)entry > 0xffffffff ||
        (ULONG_PTR)arg > 0xffffffff) return STATUS_INVALID_PARAMETER;
    memset( ctx, 0, sizeof(*ctx) );
    ctx->ContextFlags = CONTEXT_I386_ALL;
    ctx->Eax = PtrToUlong(entry); ctx->Ebx = PtrToUlong(arg);
    ctx->Esp = get_wow_teb(teb)->Tib.StackBase - 16;
    ctx->Eip = pLdrSystemDllInitBlock->pRtlUserThreadStart;
    ctx->SegCs = 0x23; ctx->SegDs = ctx->SegEs = ctx->SegGs = ctx->SegSs = 0x2b;
    ctx->SegFs = 0x53; ctx->EFlags = 0x202;
    ctx->FloatSave.ControlWord = 0x27f; ctx->FloatSave.TagWord = 0xffff;
    fx.ControlWord = 0x27f; fx.MxCsr = 0x1f80;
    memcpy( ctx->ExtendedRegisters, &fx, sizeof(fx) );
    return STATUS_SUCCESS;
}

static void runtime_start_x86_thread( PRTL_THREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    NTSTATUS status;
    if (suspend || !runtime_wow64_initialize) status = STATUS_NOT_SUPPORTED;
    else status = runtime_init_x86_context( teb, (void *)entry, arg );
    log_line( "[WOW64 THREAD] entry=%p TEB32=%p status=%08x", entry, get_wow_teb(teb), status );
    if (!status) call_pe_entry_point( runtime_wow64_initialize );
}

extern void wine_nx_load_apiset_dll(void);

static NTSTATUS runtime_start_wow64( void *module, void *entry,
                                     RTL_USER_PROCESS_PARAMETERS *params,
                                     const UNICODE_STRING *main_nt_name, BOOL autorun )
{
    HMODULE native, guest = NULL;
    void *initialize = NULL;
    SIZE_T size;
    NTSTATUS status;
    I386_CONTEXT *ctx;
    TEB *teb = NtCurrentTeb();
    status = wine_nx_init_wow64_peb( params, module );
    log_line( "[WOW64] PEB32 status=%08x", status );
    if (status) return status;
    /* Both PEBs point at the one schema, so it goes after the 32-bit PEB. */
    wine_nx_load_apiset_dll();
    log_line( "[WOW64] api set schema=%p", teb->Peb->ApiSetMap );
    status = wine_nx_loader_bootstrap( main_nt_name );
    log_line( "[WOW64] native loader bootstrap status=%08x", status );
    if (status) return status;
    status = wine_nx_loader_prepare_wow64( &native, &initialize );
    log_line( "[WOW64] native DLLs status=%08x", status );
    if (status) return status;
    status = map_pe_image( WINE_DRIVE_C "/windows/syswow64/ntdll.dll", (void **)&guest, &size );
    if (status) return status;
    /* ntdll cannot relocate itself (cf. load_wow64_ntdll); the main image is
     * relocated by the x86 loader because it is the PEB's ImageBaseAddress. */
    if ((status = virtual_relocate_module( guest ))) return status;
    if ((ULONG_PTR)guest > 0xffffffff || size > 0x100000000ULL - (ULONG_PTR)guest)
        return STATUS_INVALID_ADDRESS;
    status = wine_nx_prepare_wow64_ntdll( native, guest );
    log_line( "[WOW64] guest ntdll=%p init block status=%08x", guest, status );
    if (status) return status;
    status = init_thread_stack( teb, 0x7fffffff, main_image_info.MaximumStackSize,
                                 main_image_info.CommittedStackSize );
    if (status) return status;
    status = runtime_init_x86_context( teb, entry, wow_peb );
    if (status) return status;
    ctx = get_cpu_area( IMAGE_FILE_MACHINE_I386 );
    runtime_wow64_initialize = initialize;
    wine_nx_wow64_thread_start = runtime_start_x86_thread;
    log_line( "[WOW64] loader ready: TEB32=%p stack=%08x entry=%08x", get_wow_teb(teb), ctx->Esp, ctx->Eax );
    if (autorun)
    {
        s32 priority = -1;
        Result rc;

        /* Horizon round-robins only priority 59 on cores 0-2 (every 10 ms);
         * libnx creates every worker at 59. Left at hbloader's higher priority,
         * a main thread spinning on a lock (e.g. an RtlWaitOnAddress bucket)
         * never lets a worker holding it on the same core run. */
        svcGetThreadPriority( &priority, CUR_THREAD_HANDLE );
        rc = svcSetThreadPriority( CUR_THREAD_HANDLE, 0x3b );
        log_line( "[WOW64] main thread priority %d -> 59 rc=%x", (int)priority, rc );

        /* Wow64LdrpInitialize currently ignores its native context argument.
         * It changes the saved x86 PC to LdrInitializeThunk and never returns. */
        log_line( "[WOW64] entering Wine's x86 LdrInitializeThunk via ARM64 wow64.dll" );
        wine_nx_thread_register( 'w', HandleToULong( teb->ClientId.UniqueThread ), teb );
        call_pe_entry_point( initialize );
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}
#endif

#ifdef WINE_NX_AMD64
static NTSTATUS runtime_create_registry_path( const char *path, HANDLE *key )
{
    WCHAR key_name[256];
    UNICODE_STRING name = {0};
    OBJECT_ATTRIBUTES attr;
    HANDLE next;
    NTSTATUS status;
    size_t i, length = strlen( path );

    *key = NULL;
    if (!length || path[0] != '\\') return STATUS_OBJECT_PATH_SYNTAX_BAD;
    if (length >= ARRAY_SIZE(key_name)) return STATUS_NAME_TOO_LONG;
    for (i = 0; i <= length; i++) key_name[i] = (unsigned char)path[i];
    name.Buffer = key_name;
    name.MaximumLength = sizeof(key_name);

    for (i = 1; i <= length; i++)
    {
        WCHAR end = key_name[i];

        if (end && end != '\\') continue;
        key_name[i] = 0;
        name.Length = i * sizeof(WCHAR);
        InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL );
        /* Persistent, like the key on Windows: a volatile parent created here
         * would refuse every non-volatile key later made below Software\Microsoft. */
        status = NtCreateKey( &next, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, &attr, 0, NULL,
                              REG_OPTION_NON_VOLATILE, NULL );
        key_name[i] = end;
        if (status) return status;
        if (!end)
        {
            *key = next;
            return STATUS_SUCCESS;
        }
        NtClose( next );
    }
    return STATUS_OBJECT_PATH_SYNTAX_BAD;
}

static NTSTATUS runtime_prepare_arm64ec(void)
{
    static const char key_path[] = "\\Registry\\Machine\\Software\\Microsoft\\Wow64\\amd64";
    static const WCHAR cpu_name[] = {'w','i','n','e','b','o','x','6','4','e','c','.','d','l','l',0};
    UNICODE_STRING value = {0};
    HMODULE ntdll = NULL;
    HANDLE key;
    SIZE_T size;
    NTSTATUS status;
    TEB *teb = NtCurrentTeb();
    extern NTSTATUS wine_nx_prepare_arm64ec_ntdll( HMODULE );

    status = runtime_create_registry_path( key_path, &key );
    log_line( "[AMD64] CPU registry status=%08x", status );
    if (status) return status;
    status = NtSetValueKey( key, &value, 0, REG_SZ, cpu_name, sizeof(cpu_name) );
    NtClose( key );
    if (status) return status;

    wine_nx_load_apiset_dll();
    status = map_pe_image( WINE_SYSTEM_DIR "/ntdll.dll", (void **)&ntdll, &size );
    if (!status) status = virtual_relocate_module( ntdll );
    if (!status) status = wine_nx_prepare_arm64ec_ntdll( ntdll );
    log_line( "[AMD64] ARM64EC ntdll=%p status=%08x", ntdll, status );
    if (status) return status;
    status = init_thread_stack( teb, 0, main_image_info.MaximumStackSize, main_image_info.CommittedStackSize );
    if (status) return status;
    log_line( "[AMD64] loader ready: TEB=%p stack=%p CPU area=%p", teb, teb->Tib.StackBase,
              teb->ChpeV2CpuAreaInfo );
    return STATUS_SUCCESS;
}
#endif

static int runtime_describe_image( void *module, SIZE_T size, void **entry )
{
    IMAGE_NT_HEADERS64 *nt = runtime_nt_headers( module );
    IMAGE_NT_HEADERS32 *nt32 = (IMAGE_NT_HEADERS32 *)nt;
    IMAGE_DATA_DIRECTORY *imports;
    BOOL guest32;

    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
         nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC))
    {
        log_line( "[FAIL] mapped image has no recognized PE optional header" );
        return 0;
    }

    guest32 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#define IMAGE_FIELD(name) (guest32 ? nt32->OptionalHeader.name : nt->OptionalHeader.name)
    imports = guest32 ? &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] :
                        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    *entry = (char *)module + IMAGE_FIELD(AddressOfEntryPoint);

    main_image_info.TransferAddress = *entry;
    main_image_info.MaximumStackSize = IMAGE_FIELD(SizeOfStackReserve);
    main_image_info.CommittedStackSize = IMAGE_FIELD(SizeOfStackCommit);
    main_image_info.SubSystemType = IMAGE_FIELD(Subsystem);
    main_image_info.MajorSubsystemVersion = IMAGE_FIELD(MajorSubsystemVersion);
    main_image_info.MinorSubsystemVersion = IMAGE_FIELD(MinorSubsystemVersion);
    main_image_info.MajorOperatingSystemVersion = IMAGE_FIELD(MajorOperatingSystemVersion);
    main_image_info.MinorOperatingSystemVersion = IMAGE_FIELD(MinorOperatingSystemVersion);
    main_image_info.ImageCharacteristics = nt->FileHeader.Characteristics;
    main_image_info.DllCharacteristics = IMAGE_FIELD(DllCharacteristics);
    main_image_info.Machine = nt->FileHeader.Machine;
    main_image_info.ImageContainsCode = TRUE;
    main_image_info.ImageFlags = 0;
    if (IMAGE_FIELD(DllCharacteristics) & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)
        main_image_info.ImageDynamicallyRelocated = 1;
    main_image_info.LoaderFlags = IMAGE_FIELD(LoaderFlags);
    main_image_info.ImageFileSize = IMAGE_FIELD(SizeOfImage);
    main_image_info.CheckSum = IMAGE_FIELD(CheckSum);

    log_line( "[IMAGE] base=%p size=0x%lx preferred=0x%llx entry_rva=0x%x machine=0x%x",
              module, (unsigned long)size,
              (unsigned long long)IMAGE_FIELD(ImageBase),
              IMAGE_FIELD(AddressOfEntryPoint), nt->FileHeader.Machine );
    /* A program with no relocations only works at the address it was linked for.
     * The low addresses belong to this runtime unless Horizon gave the process a
     * 32-bit address space, which is what the forwarders are for; started any
     * other way the program reads and writes the wrong addresses and dies. */
    {
        const IMAGE_DATA_DIRECTORY *relocs = guest32 ?
            &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] :
            &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

        if ((ULONG_PTR)module != (ULONG_PTR)IMAGE_FIELD(ImageBase) && !relocs->Size &&
            !(IMAGE_FIELD(DllCharacteristics) & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE))
            log_line( "[IMAGE] this program cannot be moved: no relocations, linked for 0x%llx, mapped at %p. "
                      "It needs Wine-NX started through a 32-bit forwarder.",
                      (unsigned long long)IMAGE_FIELD(ImageBase), module );
    }
    log_line( "[IMAGE] subsystem=%u dll_char=0x%x imports=0x%x/0x%x sections=%u",
              IMAGE_FIELD(Subsystem), IMAGE_FIELD(DllCharacteristics),
              imports->VirtualAddress, imports->Size, nt->FileHeader.NumberOfSections );
    return 1;
#undef IMAGE_FIELD
}

/***********************************************************************
 * Leaving the process
 *
 * The homebrew loader takes the process back when main returns, unmaps this
 * program and resets the heap. Pages the graphics driver lent to nvservices are
 * pages the kernel will not let it reset: it gives up with InvalidMemoryState
 * (0xd401) and writes a crash report naming hbl. Ending the process instead is
 * no better, because the loader runs inside the album applet and killing that
 * leaves the system to notice on its own.
 *
 * So the lent pages go back first, by closing the driver session, and the
 * program then leaves the ordinary way.
 */
static void log_step( const char *step )
{
    log_line( "[EXIT] %s", step );
    pthread_mutex_lock( &log_mutex );
    if (log_file) fflush( log_file );
    pthread_mutex_unlock( &log_mutex );
}

/* What the loader had already lent out when this program started: it maps its
 * own NRO out of the heap, so those pages read as borrowed and are its business.
 * Anything lent that is not one of these was lent by this program, and is what
 * the loader will refuse to reset. */
#define LOADER_LENT_MAX 32
static struct { u64 addr, size; } loader_lent[LOADER_LENT_MAX];
static int loader_lent_count;

static int lent_by_loader( u64 addr, u64 size )
{
    int i;

    for (i = 0; i < loader_lent_count; i++)
        if (loader_lent[i].addr == addr && loader_lent[i].size == size) return 1;
    return 0;
}

/* Regions the loader cannot cope with: pages lent to another process, and heap
 * pages carrying an attribute of any kind. The loader reads the next program
 * into the whole heap in one file read, and the kernel maps that buffer into the
 * filesystem process for the length of the call, which it refuses over a page
 * that is not plain memory. Logs them when report is set, saying which were
 * already there when this program started, and returns how many there are; ours
 * come back through mine when it is given. */
static int memory_left_behind_ex( int report, int *mine )
{
    static const char *types[] = { "unmapped", "io", "normal", "code", "code-rw", "heap", "shared", "weird",
                                   "module", "module-rw", "ipc0", "stack", "thread-local", "transfer-iso",
                                   "transfer", "process", "reserved", "ipc1", "ipc3", "kernel-stack", "code-ro",
                                   "code-w" };
    u64 address = 0;
    int lines = 0, regions = 0, ours = 0;

    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        /* The loader's own module pages carry MemAttr_IsPermissionLocked and are
         * its business; the heap is the part it has to be able to use. */
        if ((info.attr & (MemAttr_IsBorrowed | MemAttr_IsIpcMapped | MemAttr_IsDeviceMapped)) ||
            (info.type == MemType_Heap && info.attr))
        {
            const char *type = info.type < sizeof(types) / sizeof(types[0]) ? types[info.type] : "?";
            int loaders = lent_by_loader( info.addr, info.size );

            /* Every one of ours is named: whatever is left is what the next run
             * has to give back, and there is no telling beforehand how many. */
            if (report && (!loaders || lines < 8) && lines < 96)
            {
                log_line( "[EXIT] still held: %010llx-%010llx %lluKB %s perm=%x attr=%x%s%s%s%s",
                          (unsigned long long)info.addr, (unsigned long long)(info.addr + info.size),
                          (unsigned long long)(info.size / 1024), type,
                          (unsigned)info.perm, (unsigned)info.attr,
                          info.attr & MemAttr_IsDeviceMapped ? " device" : "",
                          info.attr & MemAttr_IsBorrowed ? " borrowed" : "",
                          info.attr & MemAttr_IsUncached ? " uncached" : "",
                          loaders ? " (the loader's)" : "" );
                lines++;
            }
            if (!loaders) ours++;
            regions++;
        }
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
    if (mine) *mine = ours;
    return regions;
}

static int memory_left_behind( int report )
{
    return memory_left_behind_ex( report, NULL );
}

/* Remembers what the loader had lent when this program started, so the end can
 * tell the loader's own pages from the ones this program failed to give back. */
static void note_loader_lent_memory( void )
{
    u64 address = 0;

    loader_lent_count = 0;
    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        if ((info.attr & (MemAttr_IsBorrowed | MemAttr_IsIpcMapped | MemAttr_IsDeviceMapped)) &&
            loader_lent_count < LOADER_LENT_MAX)
        {
            loader_lent[loader_lent_count].addr = info.addr;
            loader_lent[loader_lent_count].size = info.size;
            loader_lent_count++;
        }
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
}

/* The whole address space, to compare what this program leaves behind with what
 * it was given: the loader undoes its own mappings when the program returns,
 * and refuses when a region is not in the state it expects. */
static void log_memory_map( const char *when )
{
    static const char *types[] = { "unmapped", "io", "normal", "code", "code-rw", "heap", "shared", "weird",
                                   "module", "module-rw", "ipc0", "stack", "thread-local", "transfer-iso",
                                   "transfer", "process", "reserved", "ipc1", "ipc3", "kernel-stack", "code-ro",
                                   "code-w" };
    u64 address = 0, heap_base = 0, heap_size = 0;
    int lines = 0;

    svcGetInfo( &heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0 );
    svcGetInfo( &heap_size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0 );
    log_line( "[MAP] %s: heap region %010llx+%lluKB", when, (unsigned long long)heap_base,
              (unsigned long long)(heap_size / 1024) );
    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        if (info.type != MemType_Unmapped && lines < 40)
        {
            const char *type = info.type < sizeof(types) / sizeof(types[0]) ? types[info.type] : "?";

            log_line( "[MAP] %s: %010llx-%010llx %s perm=%x attr=%x", when, (unsigned long long)info.addr,
                      (unsigned long long)(info.addr + info.size), type, (unsigned)info.perm, (unsigned)info.attr );
            lines++;
        }
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
}

/* Says how much is lent away at a point in the start-up, so the step that lends
 * it can be told apart from the ones that do not. */
static void log_lent_memory( const char *after )
{
    u64 address = 0, total = 0;
    int regions = 0;

    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        if (info.attr & (MemAttr_IsBorrowed | MemAttr_IsIpcMapped | MemAttr_IsDeviceMapped))
        {
            regions++;
            total += info.size;
        }
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
    log_line( "[MEM] after %s: %d regions lent away, %llu KB", after, regions, (unsigned long long)(total / 1024) );
}

/* A line the card has before the next step runs: the flusher thread is gone by
 * the time these are written, so a step that never returns would otherwise take
 * its own account of itself with it. */
static void log_flushed( const char *fmt, ... )
{
    char line[320];
    va_list args;

    va_start( args, fmt );
    vsnprintf( line, sizeof(line), fmt, args );
    va_end( args );
    log_line( "%s", line );
    pthread_mutex_lock( &log_mutex );
    if (log_file) fflush( log_file );
    pthread_mutex_unlock( &log_mutex );
}

static int device_regions( void );

/* Mesa says which driver session it is closing; on the card before the next one
 * starts, so a log that stops names the one that did not come back. The count
 * is what the stage before it left, which is how much each one gave back. */
static void log_graphics_step( const char *what )
{
    char line[96];

    snprintf( line, sizeof(line), "closing the %s session, %d pages still with the GPU",
              what, device_regions() );
    log_step( line );
}

/* Heap pages the GPU still has: Mesa registers its buffers with nvservices, and
 * the game that owned them is gone without giving them back. */
static int device_regions( void )
{
    u64 address = 0;
    int regions = 0;

    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        if (info.attr & MemAttr_IsDeviceMapped) regions++;
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
    return regions;
}

/* The screen's own buffers: libnx registers the text console's and Wine's
 * framebuffer with the graphics driver, so each is heap the GPU holds and each
 * counts as one open of the driver session. Nothing may draw afterwards. */
static void release_screen_buffers( void )
{
    pthread_mutex_lock( &wine_nx_fb_mutex );
    if (wine_nx_fb_ready)
    {
        framebufferClose( &wine_nx_fb );
        wine_nx_fb_ready = 0;
        wine_nx_fb_pending_bits = NULL;
        wine_nx_fb_pending_stride = 0;
        wine_nx_fb_pending_dirty = 0;
        log_line( "[EXIT] framebuffer closed" );
    }
    if (wine_nx_console_active)
    {
        consoleExit( NULL );
        wine_nx_console_active = 0;
    }
    /* A program's OpenGL or Vulkan surface that was never destroyed left its
     * images with the display, which holds them -- and through them the driver's
     * buffers -- until the window they were configured on lets them go. */
    nwindowReleaseBuffers( nwindowGetDefault() );
    pthread_mutex_unlock( &wine_nx_fb_mutex );
}

/* A step of the closing that waits on something outside this program, run on a
 * thread of its own so that it cannot take the way out with it: libnx waits
 * inside the driver close for nvservices to unmap the transfer memory, and that
 * wait has no end -- build 161 stopped there and the console never came back.
 * A step that does not finish now costs its few seconds and a line in the log,
 * and the thread it was left on shows in what is still lent out. */
struct closing_step
{
    void (*run)( void );
    volatile int done;
};

static void *closing_step_thread( void *arg )
{
    struct closing_step *step = arg;

    step->run();
    __atomic_store_n( &step->done, 1, __ATOMIC_RELEASE );
    return NULL;
}

/* Returns whether the step ran to the end within seconds. */
static int run_closing_step( void (*run)( void ), int seconds, const char *what )
{
    struct closing_step *step = calloc( 1, sizeof(*step) );  /* left behind if it hangs */
    pthread_t thread;
    int i;

    if (!step) return 0;
    step->run = run;
    log_flushed( "[EXIT] %s", what );
    if (pthread_create( &thread, NULL, closing_step_thread, step ))
    {
        log_flushed( "[EXIT] no thread to %s on", what );
        free( step );
        return 0;
    }
    for (i = 0; i < seconds * 20 && !__atomic_load_n( &step->done, __ATOMIC_ACQUIRE ); i++)
        svcSleepThread( 50000000LL );
    if (!__atomic_load_n( &step->done, __ATOMIC_ACQUIRE ))
    {
        log_flushed( "[EXIT] %s did not finish in %d s", what, seconds );
        return 0;
    }
    pthread_join( thread, NULL );
    free( step );
    return 1;
}

/* Closing the last nvdrv session is what makes nvservices give a process's
 * buffers back, and it is also what returns the driver's own 8 MB transfer
 * memory, which comes out of this heap. libnx counts the opens -- the text
 * console, Wine's framebuffer and Mesa each took one -- and ignores a close once
 * the count is at zero, so this closes it more often than it was opened. */
static void close_graphics_driver( void )
{
    int i;

    for (i = 0; i < 16; i++) nvExit();
}

/* mesa-switch (u_queue.c): Mesa's worker threads, which take 8 MB stacks of
 * heap on this platform (u_thread.c), and are ended and joined the way its own
 * atexit handler ends them. */
static void stop_mesa_workers( void )
{
    extern void util_queue_kill_all_threads( void ) __attribute__((weak));

    if (&util_queue_kill_all_threads) util_queue_kill_all_threads();
}

/* Gives the graphics driver's pages back: the buffers of the screen, then the
 * driver taken apart object by object, then the session itself. */
static void release_lent_memory( void )
{
    /* mesa-switch (nouveau_horizon_runtime.c): closes the driver sessions
     * whatever still holds them, which is the only way left once the program
     * that owned the buffers has gone without freeing them. */
    extern void nouveau_horizon_runtime_shutdown( void (*step)( const char *what ) ) __attribute__((weak));
    int before = device_regions(), i;

    /* The screen first: the text console and Wine's framebuffer are buffers of
     * libnx's own, registered with the driver, and each holds one open. */
    release_screen_buffers();
    log_flushed( "[QUIT] the screen gave its buffers back: %d pages with the GPU, was %d",
                 device_regions(), before );
    if (&nouveau_horizon_runtime_shutdown) nouveau_horizon_runtime_shutdown( log_graphics_step );
    log_flushed( "[QUIT] the driver was taken apart: %d pages with the GPU, was %d",
                 device_regions(), before );
    run_closing_step( close_graphics_driver, 5, "closing the driver session" );
    /* nvservices unmaps on its own, a moment after the session closes. */
    for (i = 0; i < 40 && device_regions(); i++) svcSleepThread( 50000000LL );
    log_flushed( "[QUIT] graphics driver closed: %d pages held by the GPU, was %d",
                 device_regions(), before );
}

/* The graphics driver's buffers are marked uncached while it has them, by
 * libnx's nvMapCreate, and the mark is taken off again by nvMapClose. The
 * program that owned them never got that far, and closing the driver's session
 * gives the pages back without touching the mark. It has to go: the loader reads
 * the next program into the whole heap in one file read, and the kernel refuses
 * to lend the filesystem process a buffer with a marked page anywhere in it --
 * InvalidCurrentMemory, which the loader then stops the console with. Returns
 * how many it could not clear. */
static int clear_heap_attributes( void )
{
    u64 address = 0, bytes = 0;
    int cleared = 0, refused = 0;

    for (;;)
    {
        MemoryInfo info = {0};
        u32 page_info = 0;

        if (R_FAILED( svcQueryMemory( &info, &page_info, address ) )) break;
        if (info.type == MemType_Heap && (info.attr & MemAttr_IsUncached))
        {
            if (R_SUCCEEDED( svcSetMemoryAttribute( (void *)(uintptr_t)info.addr, info.size,
                                                    MemAttr_IsUncached, 0 ) ))
            {
                bytes += info.size;
                cleared++;
            }
            else refused++;
        }
        if (!info.size || info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
    if (cleared || refused)
        log_flushed( "[QUIT] %d uncached heap regions made plain again, %lluKB, %d refused",
                     cleared, (unsigned long long)(bytes / 1024), refused );
    return refused;
}

/* Gives the pages back and says what is left; nonzero when the loader will
 * still refuse to clean up. */
static int leave_cleanly( void )
{
    int left = memory_left_behind( 0 ), after;

    /* Services opened for the whole run hold heap pages of their own: the
     * sockets take a transfer memory at start-up and nothing ever gave it back.
     * Close them and say what each one returns, so the one that matters shows. */
    socketExit();
    after = memory_left_behind( 0 );
    log_line( "[EXIT] sockets closed: %d regions lent, was %d", after, left );
    left = after;
    stop_log_flusher();
    after = memory_left_behind( 0 );
    log_line( "[EXIT] flusher thread ended: %d regions lent, was %d", after, left );
    left = after;
    log_memory_map( "exit" );
    log_step( "leaving through the loader" );
    return left;
}

/***********************************************************************
 * Returning to the launcher
 *
 * Horizon cannot end one thread from another, so each ends itself at its next
 * system call, and the main thread jumps back to where it started the program.
 * With every thread gone and every service closed, the loader can take the
 * process back and start this program again, which opens the launcher.
 */
volatile int wine_nx_quit_requested;
/* libnx's weak default is 0: when this program ends, leave through the loader.
 * 1 closes the application itself, the way the HOME menu closes it. Set at the
 * end of return_to_launcher, so only the way out chooses it. */
u32 __nx_applet_exit_mode = 0;
static jmp_buf quit_jump;
static int quit_jump_ready;
static char own_nro[512];

static int launcher_schedule_restart(void)
{
    return envHasNextLoad() && R_SUCCEEDED( envSetNextLoad( RUNTIME_DIR "/wine-nx-runtime.nro",
                                                           RUNTIME_DIR "/wine-nx-runtime.nro" ) );
}

/* Called wherever a thread can leave off what it is doing. Never returns while
 * a quit is under way: the thread it is called on ends, or, for the one that
 * started the program, unwinds to main. */
static volatile int quit_go;        /* the parked threads may end */
static volatile int quit_parked;    /* how many of them are waiting to hear */
/* libnx has no pthread_detach, so a thread's stack is given back only when it is
 * joined. Each one that ends leaves itself here to be joined. */
#define QUIT_JOIN_MAX 256
static pthread_t quit_joinable[QUIT_JOIN_MAX];
static volatile int quit_joinable_count;

/* A thread that has stopped where it can be ended waits here. It ends only once
 * every one of the program's threads has arrived: a thread ended while another
 * is still running takes a lock or a buffer with it, and the program is left
 * unable to go on. If they do not all arrive, they all carry on instead. */
void wine_nx_quit_point( void )
{
    unsigned int left = 0;
    int i;

    if (!wine_nx_quit_requested) return;
    if (!quit_jump_ready || !log_main_thread_set || !pthread_equal( pthread_self(), log_main_thread ))
    {
        int waited;

        wine_nx_thread_parked( 1 );
        __atomic_add_fetch( &quit_parked, 1, __ATOMIC_SEQ_CST );
        /* Bounded: if the thread that started the program never gets to decide,
         * this one goes back to what it was doing rather than wait for ever. */
        for (waited = 0; wine_nx_quit_requested && !quit_go && waited < 1600; waited++)
            svcSleepThread( 5000000LL );
        if (quit_go)
        {
            int slot = __atomic_fetch_add( &quit_joinable_count, 1, __ATOMIC_SEQ_CST );

            if (slot < QUIT_JOIN_MAX) quit_joinable[slot] = pthread_self();
            if (wine_nx_thread_unregister) wine_nx_thread_unregister();
            pthread_exit( NULL );
        }
        __atomic_sub_fetch( &quit_parked, 1, __ATOMIC_SEQ_CST );
        wine_nx_thread_parked( 0 );
        return;
    }
    /* The thread that started the program waits here, inside the system call it
     * was making, for the others to park. */
    for (i = 0; i < 500; i++)
    {
        left = wine_nx_threads_program();
        if ((int)left <= __atomic_load_n( &quit_parked, __ATOMIC_SEQ_CST )) break;
        wine_nx_threads_wake();
        svcSleepThread( 10000000LL );
    }
    log_line( "[QUIT] %d of the program's %u threads parked after %d ms",
              __atomic_load_n( &quit_parked, __ATOMIC_SEQ_CST ), left, i * 10 );
    if ((int)left > __atomic_load_n( &quit_parked, __ATOMIC_SEQ_CST ))
    {
        /* Not all of them: nothing has ended, so the program carries on. */
        wine_nx_threads_report_unparked();
        __atomic_store_n( (int *)&wine_nx_quit_requested, 0, __ATOMIC_SEQ_CST );
        log_line( "[QUIT] not all of them stopped; the program keeps running" );
        return;
    }
    __atomic_store_n( (int *)&quit_go, 1, __ATOMIC_SEQ_CST );
    for (i = 0; i < 200 && wine_nx_threads_program(); i++) svcSleepThread( 10000000LL );
    log_line( "[QUIT] %u of the program's threads left after letting them end", wine_nx_threads_program() );
    quit_jump_ready = 0;
    longjmp( quit_jump, 1 );
}

/* + and - held together. The threads take it from here. */
void wine_nx_request_quit( const char *why )
{
    if (__atomic_exchange_n( (int *)&wine_nx_quit_requested, 1, __ATOMIC_SEQ_CST )) return;
    log_line( "[QUIT] %s; ending %u threads to return to the launcher", why, wine_nx_threads_other() );
    wine_nx_threads_wake();
}

/* Wine calls this where it used to park after the program it ran terminated.
 * It cannot return to main from there, so the kernel ends the process. */
void wine_nx_leave_process( const char *why )
{
    log_line( "[EXIT] %s; returning to the launcher", why );
    /* The same road as the chord: the threads stop, this one parks with them if
     * it is not the one that started the program, and that one takes over. */
    wine_nx_request_quit( why );
    wine_nx_quit_point();
    /* Only here when the threads would not all stop, or there is no way back to
     * main: close as before, which the loader survives but does not like. */
    log_line( "[EXIT] the launcher cannot be reached from here; closing" );
    leave_cleanly();
    svcExitProcess();
    __builtin_unreachable();
}

/* Every thread the program left has to end before the loader takes over. Waits
 * for them, closes what the runtime opened, and asks the loader for this
 * program again, with no arguments, which is what opens the launcher. */
static int return_to_launcher( void )
{
    int i, still_lent = 0;

    /* Only reached with the program's threads already gone. Both of these wait
     * for the thread they end: a thread of the runtime's own has no quit point
     * to stop at, and while it runs it holds the heap pages of its stack. */
    {
        /* dlls/win32u/winnx_drv.c: polls the sticks and presents the screen. */
        extern void wine_nx_input_thread_stop( void ) __attribute__((weak));

        if (&wine_nx_input_thread_stop) wine_nx_input_thread_stop();
    }
    wine_nx_compositor_stop();
    wine_nx_profile_stop();
    /* Mesa's worker threads outlive the program that made work for them. */
    run_closing_step( stop_mesa_workers, 5, "ending the graphics library's worker threads" );
    for (i = 0; i < 200 && wine_nx_threads_other(); i++) svcSleepThread( 10000000LL );
    /* A thread that has unregistered is not finished: it still runs its own
     * teardown, which touches memory that is about to be taken away. Its stack
     * is given back as it really ends, so wait for the lent regions to settle
     * before touching anything. */
    {
        int previous = -1, now, still = 0;

        for (i = 0; i < 60 && still < 3; i++)
        {
            svcSleepThread( 50000000LL );
            now = memory_left_behind( 0 );
            still = now == previous ? still + 1 : 0;
            previous = now;
        }
        log_line( "[QUIT] threads finished after %d ms, %d regions lent", i * 50, previous );
    }
    {
        /* Joining an ended thread is what gives its stack back to the heap. */
        int i, count = __atomic_load_n( &quit_joinable_count, __ATOMIC_SEQ_CST );
        int joined = 0;

        if (count > QUIT_JOIN_MAX) count = QUIT_JOIN_MAX;
        for (i = 0; i < count; i++)
            if (!pthread_join( quit_joinable[i], NULL )) joined++;
        log_line( "[QUIT] %d of %d ended threads joined, %d regions lent", joined, count,
                  memory_left_behind( 0 ) );
    }
    {
        /* dlls/win32u/font.c: the console's fonts are shared memory this process
         * keeps while it draws. The loader starts this program again in the same
         * process, so one left mapped is left for good. */
        extern void wine_nx_release_shared_fonts( void ) __attribute__((weak));

        if (&wine_nx_release_shared_fonts) wine_nx_release_shared_fonts();
    }
    {
        /* Translated code lives in kernel code memory over heap pages, which are
         * lent to it while the arena lives. Nothing runs guest code any more. */
        extern unsigned int wine_nx_box64_release_arenas( unsigned long long *bytes )
            __attribute__((weak));
        unsigned long long bytes = 0;

        if (&wine_nx_box64_release_arenas)
        {
            unsigned int closed = wine_nx_box64_release_arenas( &bytes );

            log_line( "[QUIT] %u code arenas given back, %lluMB, %d regions lent",
                      closed, bytes >> 20, memory_left_behind( 0 ) );
        }
    }
    {
        /* The thread that ends is joined by the next one to end, in Wine and in
         * the server both, so the last of each is still holding its stack. */
        extern unsigned int horizon_release_thread_stacks( void ) __attribute__((weak));

        if (&horizon_release_thread_stacks)
        {
            unsigned int joined = horizon_release_thread_stacks();

            log_line( "[QUIT] %u stacks of ended threads given back, %d regions lent",
                      joined, memory_left_behind( 0 ) );
        }
    }
    socketExit();
    stop_log_flusher();
    {
        /* Wine's code mappings outlive its threads; the loader must not find them. */
        extern void horizon_release_code_mappings( unsigned int *released, unsigned int *failed )
            __attribute__((weak));
        unsigned int released = 0, failed = 0;

        if (&horizon_release_code_mappings)
        {
            horizon_release_code_mappings( &released, &failed );
            log_line( "[QUIT] %u code mappings given back, %u refused", released, failed );
        }
    }
    /* Name whatever is left: at this point there should be nothing but the
     * pages the loader itself lent out before this program started. */
    release_lent_memory();
    clear_heap_attributes();
    {
        int mine = 0, left = memory_left_behind_ex( 1, &mine );

        log_line( "[QUIT] %u threads and %d lent regions left, %d of them ours",
                  wine_nx_threads_other(), left, mine );
        /* The loader takes the process back by unmapping this program and
         * resetting the heap, and the kernel refuses both over a page that is
         * still lent: it gives up with InvalidCurrentMemory (0xd401) and the
         * console dies with a crash report. So a page of ours left over is
         * reason enough not to go that way, and the log says which. With
         * switch/wine/loader-anyway.txt the loader is handed the process as it
         * is, to find out what it will still take. */
        if (mine && !runtime_loader_anyway)
        {
            log_step( "pages are still lent out; the loader must not take the process back" );
            log_memory_map( "exit" );
            still_lent = 1;
        }
    }
    /* Two ways out, and which one works is the loader's business, not ours.
     *
     * Through the loader: it unloads this program and starts it again, which
     * opens the launcher without leaving the console. It is what
     * switch/wine/reload-launcher.txt asks for. sphaira's forwarder cannot do
     * it: its loader checks the result of svcBreak, a system call that returns
     * nothing at all (svc 0x26 is declared void in the kernel's own table), so
     * it reads whatever the register happens to hold and stops the console with
     * it -- the crash report says 2001-0106 whatever this program leaves behind,
     * with a clean heap as readily as with a dirty one. Upstream nx-hbloader
     * makes the same four svcBreak calls without looking at any of them.
     *
     * Otherwise: close the application the way the HOME menu does, through
     * libnx's applet exit. The console goes back to the menu with no error, and
     * the launcher is one press away. */
    if (!still_lent && runtime_reopen_launcher &&
        envHasNextLoad() && own_nro[0] && R_SUCCEEDED( envSetNextLoad( own_nro, own_nro ) ))
    {
        log_step( "starting this program again for the launcher" );
        return 0;
    }
    __nx_applet_exit_mode = 1;
    log_step( "closing this program; the console goes back to the menu" );
    return 0;
}

/* What the kernel left this process to map things in. A 32-bit address space is
 * the low 4 GB and nothing else, which is the only place a program linked for a
 * fixed low address can go. */
static int runtime_address_space_bits( void )
{
    u64 base = 0, size = 0, limit;

    if (R_FAILED( svcGetInfo( &base, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0 ) ) ||
        R_FAILED( svcGetInfo( &size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0 ) ))
        return 0;
    limit = base + size;
    if (limit <= 0x100000000ull) return 32;
    if (limit <= 0x1000000000ull) return 36;
    return 39;
}

/* The applications installed beside this one. The forwarders a user made for
 * Wine-NX are among them, which is how a game can be sent to the one with the
 * address space it needs. */
static int launcher_titles( struct wine_nx_launcher_title *titles, int max )
{
    NsApplicationRecord *records = calloc( max, sizeof(*records) );
    NsApplicationControlData *control = calloc( 1, sizeof(*control) );
    s32 found = 0;
    int written = 0;

    if (records && control && R_SUCCEEDED( nsInitialize() ))
    {
        if (R_SUCCEEDED( nsListApplicationRecord( records, max, 0, &found ) ))
        {
            for (s32 i = 0; i < found && written < max; i++)
            {
                NacpLanguageEntry *entry = NULL;
                u64 size = 0;

                titles[written].id = records[i].application_id;
                /* Its name when the console has one, its id when it does not. */
                snprintf( titles[written].name, sizeof(titles[written].name), "%016llX",
                          (unsigned long long)records[i].application_id );
                if (R_SUCCEEDED( nsGetApplicationControlData( NsApplicationControlSource_Storage,
                                                              records[i].application_id, control,
                                                              sizeof(*control), &size ) ) &&
                    R_SUCCEEDED( nacpGetLanguageEntry( &control->nacp, &entry ) ) && entry && entry->name[0])
                    snprintf( titles[written].name, sizeof(titles[written].name), "%s", entry->name );
                written++;
            }
        }
        nsExit();
    }
    free( records );
    free( control );
    return written;
}

/* Which system memory the console booted from. Atmosphere answers through a
 * configuration item of its own; without it there is no way to tell, and the
 * caller says so rather than guessing. */
static int runtime_on_emummc( void )
{
    const SplConfigItem ExosphereEmummcType = (SplConfigItem)65007;
    u64 type = 0;
    int result = -1;

    if (R_SUCCEEDED( splInitialize() ))
    {
        if (R_SUCCEEDED( splGetConfig( ExosphereEmummcType, &type ) )) result = type != 0;
        splExit();
    }
    return result;
}

/* Build a forwarder that starts this NRO in the address space bits asks for,
 * and install it, so a game that needs the low 4 GB has somewhere to go. */
/* What the forwarder installer has to say, as it says it. */
static void log_line_plain( const char *line )
{
    log_line( "%s", line );
}

static unsigned int launcher_install_forwarder( int bits, const char *name, unsigned long long *id,
                                                const char **step )
{
    struct wine_nx_forwarder request =
    {
        .nro_path = own_nro,
        .args = NULL,
        .name = name,
        .author = "ticoverse.com",
        .address_space = bits == 32 ? WINE_NX_SPACE_32BIT_NO_ALIAS : WINE_NX_SPACE_39BIT,
        .icon = bits == 32 ? wine_nx_icon_32bit : wine_nx_icon_any,
        .icon_size = bits == 32 ? wine_nx_icon_32bit_size : wine_nx_icon_any_size,
    };
    unsigned int rc;

    wine_nx_forwarder_report = log_line_plain;
    if (id) *id = wine_nx_forwarder_title_id( own_nro, NULL, request.address_space );
    rc = wine_nx_forwarder_install( &request, step );
    log_line( "[LAUNCHER] %d-bit forwarder %016llx: rc=0x%x%s%s", bits,
              id ? *id : 0ull, rc, rc && step && *step ? " at " : "", rc && step && *step ? *step : "" );
    return rc;
}

/* Whether an application is still installed. The records alone answer it, so
 * this does not ask the console for every application's name as the listing
 * above does. */
static int launcher_title_installed( unsigned long long id )
{
    NsApplicationRecord *records;
    const int page = 64;
    s32 offset = 0, found = 0;
    int installed = 0;

    if (!id) return 0;
    if (!(records = calloc( page, sizeof(*records) ))) return 0;
    if (R_SUCCEEDED( nsInitialize() ))
    {
        do
        {
            if (R_FAILED( nsListApplicationRecord( records, page, offset, &found ) )) break;
            for (s32 i = 0; i < found && !installed; i++)
                if (records[i].application_id == id) installed = 1;
            offset += found;
        } while (found == page && !installed);
        nsExit();
    }
    free( records );
    return installed;
}

/* Asks the console to close this application and open that one. */
static int launcher_launch_title( unsigned long long id )
{
    Result rc = appletRequestLaunchApplication( id, NULL );

    if (R_FAILED( rc )) log_line( "[LAUNCHER] could not start %016llx: rc=0x%x", id, (unsigned)rc );
    return R_SUCCEEDED( rc );
}

/* This forwarder's own application id, to tell it from the others in the list. */
static unsigned long long runtime_title_id( void )
{
    u64 id = 0;

    if (R_FAILED( svcGetInfo( &id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0 ) )) return 0;
    return id;
}

int main( int argc, char **argv )
{
    char target[512] = DEFAULT_TARGET;
    TEB *teb;
    void *module = NULL;
    void *entry = NULL;
    SIZE_T view_size = 0;
    struct runtime_module *main_module;
    RTL_USER_PROCESS_PARAMETERS *params;
    UNICODE_STRING main_nt_name;
    char dos_path[512];
    unsigned int status;
    unsigned int ldr_status = STATUS_INVALID_IMAGE_FORMAT;
    unsigned int attach_status = STATUS_INVALID_IMAGE_FORMAT;
    int autorun, handed_over = 0;
    USHORT target_machine;
    int sd_cache = wine_nx_sd_cache_install();  /* before any file on the card is opened */

    log_main_thread = pthread_self();
    log_main_thread_set = 1;
    if (argc > 0 && argv[0] && strstr( argv[0], ".nro" )) snprintf( own_nro, sizeof(own_nro), "%s", argv[0] );
    else snprintf( own_nro, sizeof(own_nro), "%s", RUNTIME_DIR "/wine-nx-runtime.nro" );
    /* Before the console, whose framebuffer is lent to the graphics driver:
     * what is lent now is the loader's, and everything after it is ours. */
    note_loader_lent_memory();
    consoleInit( NULL );
    /* One empty frame, so the screen is this program's and blank from the start
     * rather than whatever was on it before. */
    consoleUpdate( NULL );
    mkdir( "sdmc:/switch", 0777 );
    mkdir( RUNTIME_DIR, 0777 );
    mkdir( WINE_DRIVE_C, 0777 );
    mkdir( WINE_DRIVE_C "/windows", 0777 );
    mkdir( WINE_DRIVE_C "/windows/temp", 0777 );
    mkdir( WINE_SYSTEM_DIR, 0777 );
    mkdir( WINE_DRIVE_C "/users", 0777 );
    mkdir( WINE_USER_DIR, 0777 );
    mkdir( WINE_USER_DIR "/AppData", 0777 );
    mkdir( WINE_USER_DIR "/AppData/Local", 0777 );
    mkdir( WINE_USER_DIR "/AppData/Roaming", 0777 );
    mkdir( WINE_USER_DIR "/Documents", 0777 );
    log_file = fopen( RUNTIME_DIR "/wine-nx-runtime.log", "w" );
    if (log_file)
    {
        setvbuf( log_file, log_file_buffer, _IOFBF, sizeof(log_file_buffer) );
        log_flusher_running = !pthread_create( &log_flusher_thread, NULL, log_flusher, NULL );
        /* Above the program's threads (59) and below the audio feeder (56), so
         * it is read even when they are all busy waiting. */
        if (R_SUCCEEDED( threadCreate( &stall_watch_thread, stall_watch, NULL, NULL, 0x8000, 0x38, -2 ) ) &&
            R_FAILED( threadStart( &stall_watch_thread ) ))
            threadClose( &stall_watch_thread );
        else stall_watch_running = 1;
    }
    /* First, before anything else: which build this is and which file it was
     * started from. Without it a log from an older NRO on the card reads just
     * like one from the new one. */
    log_line( "[BUILD] %s from %s (address space %d bits)", WINE_NX_RUNTIME_BUILD, own_nro,
              runtime_address_space_bits() );
    {
        int recovered = autorun_install_recover( RUNTIME_DIR, strstr( own_nro, "/updates/previous.nro" ) != NULL );
        if (recovered < 0)
        {
            wine_nx_console_quiet = 0;
            PadState pad;
            log_line( "[UPDATE] Recovery failed. Restore switch/wine/updates/previous.nro before starting a game. Press + to close." );
            padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
            padInitializeDefault( &pad );
            while (appletMainLoop())
            {
                padUpdate( &pad );
                if (padGetButtonsDown( &pad ) & HidNpadButton_Plus) break;
                consoleUpdate( NULL );
                svcSleepThread( 16000000 );
            }
            consoleExit( NULL );
            leave_cleanly();
            return 0;
        }
        if (recovered == 2)
        {
            log_line( "[UPDATE] Restored the previous runtime; restarting" );
            if (!launcher_schedule_restart()) log_line( "[UPDATE] Restart Autorun from the HOME menu" );
            consoleExit( NULL );
            leave_cleanly();
            return 0;
        }
    }
    /* The launcher can access SteamGridDB before a game is selected. */
    log_memory_map( "start-up" );
    log_line( "[MAP] start-up: %d regions the loader already had lent", loader_lent_count );
    wine_nx_runtime_network_init();
    log_lent_memory( "the network" );

#ifdef WINE_NX_USB_STORAGE
    {
        extern void wine_nx_usb_start(void);

        wine_nx_usb_start();
    }
#endif
    mkdir( CONFIG_DIR, 0777 );
    wine_nx_config_load( &runtime_config, CONFIG_FILE );
    autorun = config_bool( "run-the-chosen-program", 0, "run-entry.txt", 0 );
    wine_nx_runtime_verbose = config_bool( "verbose-log", 0, "verbose.txt", 0 );
    /* Pinned GPU buffers are CPU-cacheable unless asked for the old mapping,
     * which is there to compare the two. */
    if (&wine_nx_nouveau_pin_cached && !config_bool( "gl-pinned-buffers-cached", 1, "gl-uncached.txt", 1 ))
        wine_nx_nouveau_pin_cached = 0;
    if (&wine_nx_nouveau_skip_clean && !config_bool( "gl-clean-before-submit", 1, "gl-noclean.txt", 1 ))
        wine_nx_nouveau_skip_clean = 1;
    else if (&wine_nx_nouveau_skip_clean && config_bool( "gl-clean-test", 0, "gl-clean-test.txt", 0 ))
        clean_alternates = 1;
    if (&wine_nx_nouveau_pin_cached && &wine_nx_nouveau_skip_clean)
        log_line( "[INIT] pinned GPU buffers %s, cache clean before submissions %s",
                  wine_nx_nouveau_pin_cached ? "cacheable" : "uncached",
                  wine_nx_nouveau_skip_clean ? "off" : clean_alternates ? "alternating from 60 s, 30 s off/30 s on" : "on" );
    if (!config_bool( "core-balancing", 1, "no-balance.txt", 1 )) wine_nx_balance_enabled = 0;
    log_line( "[INIT] core balancing %s", wine_nx_balance_enabled ? "on" : "off" );
    runtime_profile = config_bool( "profiler", 0, "profile.txt", 0 );
    /* The key map keeps a file of its own: it is a line for each control, with
     * room for the comments that say what the codes mean. */
    read_key_map( CONFIG_DIR "/keys.txt" );
    read_key_map( RUNTIME_DIR "/keys.txt" );
    if (!config_bool( "display-devices", 1, "no-display-devices.txt", 1 )) wine_nx_display_devices = 0;
    log_line( "[INIT] display devices %s", wine_nx_display_devices ? "registered" : "off" );
    if (!config_bool( "windows-through-opengl", 1, "framebuffer.txt", 1 )) wine_nx_compositor_mode = 0;
    log_line( "[INIT] windows shown by %s",
              wine_nx_compositor_mode ? "the OpenGL compositor" : "the framebuffer" );
    /* Both are wanted on the way out, when the card is a poor thing to ask. */
    runtime_loader_anyway = config_bool( "hand-the-process-back-anyway", 0, "loader-anyway.txt", 0 );
    runtime_reopen_launcher = config_bool( "reopen-the-launcher-on-exit", 1, "reload-launcher.txt", 0 );
    runtime_dxvk_on_add = wine_nx_config_bool( &runtime_config, "dxvk-for-new-games", 1 );
    if (runtime_config_moved && wine_nx_config_save( &runtime_config, CONFIG_FILE ))
        log_line( "[CONFIG] settings written to %s", CONFIG_FILE );
#ifdef WINE_NX_MESA_SWITCH
    /* This runtime links mesa-switch (build-mesa-switch.sh); vulkan-probe.txt
     * reports what its NVK offers, for Vulkan and DXVK (vulkan_probe.c). */
    {
        int vulkan_probe = config_bool( "vulkan-probe", 0, "vulkan-probe.txt", 0 );

        log_line( "[INIT] Mesa from mesa-switch: OpenGL through nvc0, Vulkan through NVK; Vulkan probe %s",
                  vulkan_probe ? "on" : "off" );
        if (vulkan_probe)
        {
            extern void wine_nx_vulkan_probe( void );

            wine_nx_vulkan_probe();
            log_lent_memory( "the Vulkan probe" );
        }
    }
#endif
#ifdef WINE_NX_USB_STORAGE
    {
        extern void wine_nx_usb_wait(void);

        wine_nx_usb_wait();
    }
#endif
    /* A launcher in another forwarder sent this game here, because it needs the
     * address space this forwarder was made with and that one was not. It is
     * ours to start once: the file goes before the game does, so a game that
     * cannot start does not meet the same handoff on the way back. */
    {
        char handoff[512];

        if (read_first_line( RUNTIME_DIR "/run-next.txt", handoff, sizeof(handoff) ) && handoff[0])
        {
            remove( RUNTIME_DIR "/run-next.txt" );
            snprintf( target, sizeof(target), "%s", handoff );
            autorun = handed_over = 1;
            log_line( "[LAUNCHER] started here by another forwarder: %s", target );
        }
    }
    if (handed_over || (argc > 1 && argv[1] && argv[1][0]))
    {
        const char *name;

        if (!handed_over) snprintf( target, sizeof(target), "%s", argv[1] );
        name = strrchr( target, '/' );
        /* The one thing the screen is told, before the game has it. */
        wine_nx_console_quiet = 0;
        log_line( "[TARGET] starting %s", name ? name + 1 : target );
        wine_nx_console_quiet = 1;
    }
    else
    {
        struct wine_nx_launcher_options options =
        {
            .runtime_dir = RUNTIME_DIR,
            .nro_path = own_nro,
            .emummc = runtime_on_emummc(),
            .build = WINE_NX_RUNTIME_BUILD,
            .machine_of = launcher_machine,
#ifdef WINE_NX_USB_STORAGE
            .list_usb = wine_nx_usb_list,
#endif
            .address_space_bits = runtime_address_space_bits(),
            .reopen_launcher = runtime_reopen_launcher,
            .dxvk_on_add = runtime_dxvk_on_add,
            .title_id = runtime_title_id(),
            .list_titles = launcher_titles,
            .launch_title = launcher_launch_title,
            .title_installed = launcher_title_installed,
            .install_forwarder = launcher_install_forwarder,
            .schedule_restart = envHasNextLoad() ? launcher_schedule_restart : NULL,
#ifdef WINE_NX_MESA_SWITCH
            .vulkan = 1,
#endif
            .verbose = wine_nx_runtime_verbose,
            .profile = runtime_profile,
            .framebuffer = !wine_nx_compositor_mode,
        };
        int chosen;

        /* Without a program on the command line, let the user choose one;
         * target.txt only preselects the last choice. The launcher draws with
         * SDL, so the console gives up the screen until it returns. */
        read_first_line( RUNTIME_DIR "/target.txt", target, sizeof(target) );
        pthread_mutex_lock( &log_mutex );
        if (log_file) fflush( log_file );
        pthread_mutex_unlock( &log_mutex );
        log_line( "[LAUNCHER] bringing the screen up: closing the console" );
        consoleExit( NULL );
        wine_nx_console_active = 0;
        log_lent_memory( "the settings" );
        log_line( "[LAUNCHER] bringing the screen up: the launcher" );
        chosen = wine_nx_launcher_run( &options, target, sizeof(target) );
        log_lent_memory( "the launcher" );
        /* The console stays off from here: after SDL's EGL surface let the
         * screen go, libnx's console was set up but could not dequeue a buffer,
         * and its first line aborted in framebufferBegin (build 106). The log
         * goes to the file until the compositor or the framebuffer, which set
         * up every buffer as EGL does, takes the screen. */
        pthread_mutex_lock( &log_mutex );
        if (log_file) fflush( log_file );
        pthread_mutex_unlock( &log_mutex );
        wine_nx_runtime_verbose = options.verbose;
        runtime_profile = options.profile;
        wine_nx_compositor_mode = !options.framebuffer;
        /* The launcher changes settings; keeping them is the runtime's, which
         * owns the file and knows every other setting in it. */
        wine_nx_config_set_bool( &runtime_config, "verbose-log", options.verbose );
        wine_nx_config_set_bool( &runtime_config, "profiler", options.profile );
        wine_nx_config_set_bool( &runtime_config, "windows-through-opengl", !options.framebuffer );
        wine_nx_config_set_bool( &runtime_config, "reopen-the-launcher-on-exit", options.reopen_launcher );
        runtime_reopen_launcher = options.reopen_launcher;
        wine_nx_config_set_bool( &runtime_config, "dxvk-for-new-games", options.dxvk_on_add );
        runtime_dxvk_on_add = options.dxvk_on_add;
        wine_nx_config_save( &runtime_config, CONFIG_FILE );
        if (!chosen)
        {
            log_line( "[LAUNCHER] closed without starting a program" );
            consoleExit( NULL );
            leave_cleanly();
            return 0;
        }
        autorun = 1;
    }

    /* From here a thread may be asked to end; this one comes back here. */
    if (setjmp( quit_jump )) return return_to_launcher();
    quit_jump_ready = 1;

    /* The program's own settings, written by the launcher next to it, over the global files. */
    {
        struct launcher_settings settings;
        struct launcher_kv kv;
        char settings_path[520];

        runtime_dxvk = 0;
        runtime_dxvk_hud = 0;
#ifdef WINE_NX_MESA_SWITCH
        wine_nx_graphics_configure( 0, 1 );
        wine_nx_upscaling_configure( 0, 0.4f );
#endif
#ifdef WINE_NX_LSFG
        wine_nx_lsfg_configure( 0, 1, 1 );
#endif
        runtime_vkd3d_version[0] = 0;
        runtime_dxvk_version[0] = 0;
        if (target[1] != ':' &&
            launcher_program_settings_path( RUNTIME_DIR, target, settings_path, sizeof(settings_path) ) &&
            launcher_kv_load( &kv, settings_path ) && kv.size)
        {
            launcher_settings_read( &kv, &settings );
            if (settings.verbose >= 0) wine_nx_runtime_verbose = settings.verbose;
            if (settings.profile >= 0) runtime_profile = settings.profile;
            if (settings.framebuffer >= 0) wine_nx_compositor_mode = !settings.framebuffer;
#ifdef WINE_NX_MESA_SWITCH
            runtime_dxvk = settings.dxvk;
            runtime_dxvk_hud = settings.dxvk_hud;
            wine_nx_graphics_configure( launcher_frame_limits[settings.frame_limit], settings.vsync );
            wine_nx_upscaling_configure( settings.upscaling, launcher_sharpness_values[settings.upscaling_sharpness] );
#ifdef WINE_NX_LSFG
            wine_nx_lsfg_configure( settings.lsfg_enabled, settings.lsfg_performance, settings.lsfg_flow );
#endif
            memcpy( runtime_vkd3d_version, settings.vkd3d_version, sizeof(runtime_vkd3d_version) );
            memcpy( runtime_dxvk_version, settings.dxvk_version, sizeof(runtime_dxvk_version) );
            if (runtime_dxvk)
            {
                struct launcher_kv graphics;

                mkdir( WINE_USER_DIR "/AppData/Local/Autorun", 0777 );
                if (!launcher_dxvk_config( &settings, graphics.text, sizeof(graphics.text) ))
                    return return_to_launcher();
                graphics.size = strlen( graphics.text );
                if (!launcher_kv_save( &graphics, WINE_USER_DIR "/AppData/Local/Autorun/dxvk.conf" ))
                {
                    log_line( "[DXVK] could not write graphics settings" );
                    return return_to_launcher();
                }
                log_line( "[DXVK] HUD %s, frame limit %s, VSync %s",
                          launcher_hud_labels[settings.dxvk_hud], launcher_frame_limit_labels[settings.frame_limit],
                          settings.vsync ? "on" : "off" );
            }
#endif
            log_line( "[SETTINGS] %s: verbose %s, profiler %s, windows %s, Direct3D %s", settings_path,
                      settings.verbose < 0 ? "global" : settings.verbose ? "on" : "off",
                      settings.profile < 0 ? "global" : settings.profile ? "on" : "off",
                      settings.framebuffer < 0 ? "global" : settings.framebuffer ? "framebuffer" : "compositor",
#ifdef WINE_NX_MESA_SWITCH
                      settings.dxvk ? "DXVK + VKD3D" : "Wine" );
#else
                      settings.dxvk ? "Wine (DXVK needs the Vulkan runtime)" : "Wine" );
#endif
        }
    }

    open_game_log( target );
    log_line( "wine-nx-runtime: generic Wine ntdll PE loader path" );
    log_line( "[BUILD] %s", WINE_NX_RUNTIME_BUILD );
    log_line( "[SDCACHE] %s", sd_cache ? "sdmc reads cached: 128 KB chunks, 8 per file, 32 MB in all"
                                      : "no sdmc device; reads are not cached" );
    log_line( "[INIT] verbose traces %s (verbose.txt)", wine_nx_runtime_verbose ? "on" : "off" );
    log_line( "[INIT] profiler %s (profile.txt)", runtime_profile ? "on" : "off" );
    log_line( "[INIT] windows shown by %s", wine_nx_compositor_mode ? "the OpenGL compositor" : "the framebuffer" );
    /* After the launcher, where X may have turned it on or off. */
    if (runtime_profile)
    {
        extern void wine_nx_profile_start( void );
        wine_nx_profile_start();
    }
    log_line( "[TARGET] %s", target );

    status = runtime_target_machine( target, &target_machine );
    if (status || (status = horizon_set_process_machine( target_machine )))
    {
        log_line( "[FAIL] target machine status=%08x", status );
        park_forever();
    }
    main_image_info.Machine = target_machine;
#ifdef WINE_NX_AMD64
    if (target_machine == IMAGE_FILE_MACHINE_AMD64)
    {
        void *start, *end;

        horizon_get_address_space_limits( &start, &end );
        if ((ULONG_PTR)end < 0x8000000000ULL)
        {
            log_line( "[FAIL] AMD64 requires the 39-bit forwarder; current address space %p-%p", start, end );
            park_forever();
        }
    }
#endif
    wine_nx_runtime_platform_init();
    log_line( "[INIT] Wine paths/unix bridge ready" );
    virtual_init();
    log_line( "[INIT] virtual memory ready" );

    /* TEMPORARY: verify __libnx_exception_handler wiring. Set to 0 to disable. */
#define WINE_NX_TEST_FAULT 0
#if WINE_NX_TEST_FAULT
    log_line( "[TEST] about to deliberately deref NULL to verify exception handler" );
    fflush( log_file );
    {
        volatile int *null_ptr = (volatile int *)(uintptr_t)0;
        volatile int observed = *null_ptr;
        log_line( "[TEST] NULL deref did NOT fault, value=%d (handler not wired correctly)", observed );
    }
#endif
    wine_nx_runtime_environment_init();
    log_line( "[INIT] Wine NLS/environment ready" );
    teb = virtual_alloc_first_teb();
    if (!teb || NtCurrentTeb() != teb || !teb->Peb)
    {
        log_line( "[FAIL] virtual_alloc_first_teb" );
        park_forever();
    }
    /* Upstream's start_main_thread does this; without it the PEB (and the
     * WoW64 PEB copied from it) reports zero processors to GetSystemInfo. */
    init_cpu_info();
    /* Upstream's dbg_init also copies the debug channels to the page after
     * the WoW64 PEB, where the Windows-side ntdlls look them up. Left zeroed,
     * every channel is off there, so loader errors such as a missing DLL never
     * reach the log. Only the default entry is written: errors, and fixmes
     * with verbose traces. dbg_init itself is not called because it moves the
     * unix-side debug buffers into TEBs, which the runtime's threads lack. */
    {
        struct __wine_debug_channel *options = (void *)((char *)teb->Peb + 2 * page_size);

        options[0].name[0] = 0;
        /* Wine reports a good deal at warning level and returns quietly after
         * it, which is where wined3d refuses to start, so verbose runs want it. */
        options[0].flags = (1 << __WINE_DBCL_ERR) |
                           (wine_nx_runtime_verbose ? (1 << __WINE_DBCL_FIXME) | (1 << __WINE_DBCL_WARN) : 0);
    }
    {
        unsigned long long total, used;

        horizon_get_memory_info( &total, &used );
        log_line( "[INIT] processors=%u memory=%llu MB used=%llu MB", (unsigned int)teb->Peb->NumberOfProcessors,
                  total >> 20, used >> 20 );
    }
    wine_nx_start_user_shared_data_clock();
    log_line( "[INIT] shared data clock initialized" );

    server_init_process();
    log_line( "[INIT] server process initialized" );
#ifdef WINE_NX_AMD64
    if (target_machine != IMAGE_FILE_MACHINE_AMD64)
#endif
    {
        status = runtime_init_process_done( NULL );
        if (status)
        {
            log_line( "[FAIL] init_process_done status=%08x", status );
            park_forever();
        }
    }

    status = map_pe_image( target, &module, &view_size );
    if (status)
    {
        log_line( "[FAIL] map target status=%08x", status );
        park_forever();
    }

    if (runtime_describe_image( module, view_size, &entry ))
    {
        params = runtime_create_process_params( target, &main_nt_name, dos_path, sizeof(dos_path) );
        if (!params)
        {
            log_line( "[FAIL] process parameter allocation" );
            park_forever();
        }
        runtime_init_peb_process( teb, module, params );
        log_line( "[PEB] image=%s nt=\\??\\%s", dos_path, dos_path );

#ifdef WINE_NX_AMD64
        if (target_machine == IMAGE_FILE_MACHINE_AMD64)
        {
            BOOL suspend = FALSE;
            extern void wine_nx_start_arm64ec_thread( PRTL_THREAD_START_ROUTINE, void *, BOOL, TEB * );

            status = runtime_prepare_arm64ec();
            if (!status) status = runtime_init_process_done( &suspend );
            log_line( "[AMD64] startup status=%08x", status );
            if (!status && autorun)
            {
                svcSetThreadPriority( CUR_THREAD_HANDLE, 0x3b );
                wine_nx_thread_register( 'w', HandleToULong( teb->ClientId.UniqueThread ), teb );
                wine_nx_start_arm64ec_thread( (PRTL_THREAD_START_ROUTINE)entry, teb->Peb, suspend, teb );
            }
            park_forever();
        }
#endif
#ifdef WINE_NX_BOX64_INTERPRETER
        if (target_machine == IMAGE_FILE_MACHINE_I386)
        {
            status = runtime_start_wow64( module, entry, params, &main_nt_name, autorun );
            log_line( "[WOW64] startup status=%08x", status );
            park_forever();
        }
#endif
        main_module = register_module( target, module, view_size, 1 );
        if (main_module)
        {
            status = wine_nx_loader_bootstrap( &main_nt_name );
            log_line( "[LDR] bootstrap status=%08x", status );
            if (!status)
            {
                ldr_status = wine_nx_loader_fixup_main_imports();
                log_line( "[LDR] fixup_imports status=%08x", ldr_status );
                if (ldr_status && wine_nx_loader_last_import_dll()[0])
                    log_line( "[LDR] last failed import=%s status=%08x",
                              wine_nx_loader_last_import_dll(),
                              wine_nx_loader_last_import_status() );
                if (ldr_status && wine_nx_loader_last_open_path()[0])
                    log_line( "[LDR] last dll open=%s status=%08x",
                              wine_nx_loader_last_open_path(),
                              wine_nx_loader_last_open_status() );
                if (ldr_status && wine_nx_loader_last_export_diag()[0])
                    log_line( "[LDR] export diag=%s", wine_nx_loader_last_export_diag() );
                if (!ldr_status)
                {
                    attach_status = wine_nx_loader_attach_main();
                    log_line( "[LDR] process_attach status=%08x", attach_status );
                }
            }
        }
        log_line( "[READY] PE image is mapped by Wine ntdll; entry=%p", entry );
        if (autorun && !ldr_status && !attach_status)
        {
            log_line( "[RUN] run-entry.txt enabled; jumping to PE entry after Wine loader attach" );
            log_line( "[RUN] entry returned %d", call_pe_entry_point( entry ) );
        }
        else if (autorun)
        {
            /* Nothing will draw now, so the screen goes back to being the only
             * place this can be read without a computer. */
            wine_nx_console_quiet = 0;
            log_line( "[BLOCK] run-entry.txt enabled, but loader status import=%08x attach=%08x",
                      ldr_status, attach_status );
        }
    }

    park_forever();
    return 0;
}
