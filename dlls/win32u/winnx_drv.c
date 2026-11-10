/*
 * Minimal Nintendo Switch (Horizon) display driver for win32u.
 *
 * Windows render into ordinary DIB memory (handled by the GDI engine), and the
 * window surface's flush() copies the dirty pixels to the runtime's OpenGL
 * compositor, one layer per window surface (horizon-wine/source/compositor.c),
 * or, with switch/wine/framebuffer.txt, straight to the libnx framebuffer via
 * the runtime hooks wine_nx_fb_*().
 *
 * The driver reuses the null_user_driver for everything else; only window
 * creation and surface presentation are Switch-specific (see driver.c, which
 * plugs these in under __SWITCH__).
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "win32u_private.h"
#include "wine/gdi_driver.h"
#include "../../horizon-wine/source/compositor.h"
#include "../../horizon-wine/source/input_events.h"

/* Framebuffer hooks implemented in the runtime (horizon-wine/source/runtime.c). */
extern void *wine_nx_fb_lock( int *width, int *height, int *stride_px );
extern void  wine_nx_fb_unlock( void );
extern void  wine_nx_fb_present( void );
extern int   wine_nx_pointer_poll( int *x, int *y, unsigned int *buttons );
extern void  wine_nx_input_take( struct input_snapshot *out );
extern void  wine_nx_pointer_set_pos( int x, int y );
extern void  wine_nx_pointer_follow( int x, int y );
extern void  wine_nx_cursor_show( int visible );
extern void  wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
extern int   wine_nx_runtime_verbose __attribute__((weak));
/* Whether the runtime's OpenGL compositor presents the screen instead of the
 * framebuffer (starting it on the first call); each window surface then feeds
 * a layer of it (horizon-wine/source/compositor.h). */
extern int   wine_nx_compositor_enabled( void );

/* Buttons reported by wine_nx_pointer_poll(). */
#define WINE_NX_POINTER_LEFT  0x1
#define WINE_NX_POINTER_RIGHT 0x2

struct wine_nx_surface
{
    struct window_surface surface;
    BOOL initial_redraw_done;
    RECT present_rect;
    POINT screen_origin;
    RECT clip_rect;
    BOOL has_clip;
    struct wine_nx_layer *layer;  /* the compositor's copy of the pixels, when it presents */
};

struct wine_nx_surface_entry
{
    HWND hwnd;
    RECT screen_rect;
    BOOL visible;
    struct wine_nx_layer *layer;  /* of the window's current surface */
    struct wine_nx_surface_entry *next;
};

/* The entries are only ever added to, from the threads of their windows. */
static pthread_mutex_t wine_nx_surface_entries_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct wine_nx_surface_entry *wine_nx_surface_entries;
static volatile int wine_nx_input_thread_started;
static volatile int wine_nx_input_thread_quit;
static volatile int wine_nx_input_thread_ended;
static pthread_t wine_nx_input_thread_id;
static void nxdrv_trace( const char *fmt, int a, int b, int c, int d );
static void nxdrv_trace_hot( const char *fmt, int a, int b, int c, int d );

/* The screen the arrow is drawn on and the framebuffer's size: the monitor
 * reported to win32u below, and what a finger's pixels mean. */
#define WINE_NX_SCREEN_W 1280
#define WINE_NX_SCREEN_H 720

/* What the log is told whether or not verbose tracing is on: a few lines about
 * where the input goes, not the stream of every event. */
static void nxdrv_say( const char *text )
{
    if (&wine_nx_runtime_trace) wine_nx_runtime_trace( text );
}

/* The same, for the OpenGL path, which has its own idea of where the picture
 * goes and should say so when it differs from the desktop's. */
void wine_nx_say( const char *text )
{
    nxdrv_say( text );
}

/* Said from the threads of this driver only, and only for the reasons below. */
static volatile unsigned int wine_nx_input_pumps;   /* calls to ProcessEvents */

/* Moves the drawn cursor while the program is too busy to pump messages, such
 * as OpenTTD loading its sprites. This thread has no TEB, so it must not call
 * into Wine: the first server call dereferences a NULL TEB. It only polls the
 * controller and presents; ProcessEvents sends the input from a Wine thread. */
/* Key messages a program has taken out of its queue since the runtime started
 * (message.c counts them): what says whether a key the floating keyboard sent
 * was ever read, or is still sitting in a queue nobody looks at, or never
 * reached one because a low-level hook stopped it. */
unsigned int wine_nx_keys_taken;

static BYTE pending_key_vk;
static BOOL pending_key_up;
static unsigned int pending_key_time, pending_key_taken;

/* Notes what was just sent, to be answered for in the input thread. Called
 * where the key is sent, which may be any thread. */
static void nxdrv_note_key_sent( BYTE vk, BOOL up )
{
    pending_key_vk = vk;
    pending_key_up = up;
    pending_key_time = NtGetTickCount();
    pending_key_taken = __atomic_load_n( &wine_nx_keys_taken, __ATOMIC_RELAXED );
}

/* Four hundred milliseconds later: did the program take it? Said once, and
 * only when it did not: a film that ignores the keyboard says here whether the
 * keyboard was ever offered to it. */
static void nxdrv_check_key_taken(void)
{
    char line[112];

    if (!pending_key_time || NtGetTickCount() - pending_key_time < 400) return;
    if (__atomic_load_n( &wine_nx_keys_taken, __ATOMIC_RELAXED ) == pending_key_taken)
    {
        snprintf( line, sizeof(line), "[KEY] the program did not take key %02x %s from its queue in 400 ms",
                  pending_key_vk, pending_key_up ? "up" : "down" );
        nxdrv_say( line );
    }
    pending_key_time = 0;
}

static void *wine_nx_input_thread( void *arg )
{
    unsigned int pumps = __atomic_load_n( &wine_nx_input_pumps, __ATOMIC_RELAXED );
    unsigned int waited = 0;
    int paused = 0;

    (void)arg;
    while (!__atomic_load_n( &wine_nx_input_thread_quit, __ATOMIC_ACQUIRE ))
    {
        unsigned int buttons = 0, now;
        int x, y;

        wine_nx_pointer_poll( &x, &y, &buttons );
        wine_nx_fb_present();
        nxdrv_check_key_taken();
        /* Nothing is delivered to a program that is not taking its messages:
         * it waits in the queue, or the server drops it. Say so when five
         * seconds pass without a call to ProcessEvents, with the pad as it is
         * at the time, so a film that cannot be skipped says whether the pad
         * was even seen, and say when the program takes messages again. This
         * thread has no TEB: only the runtime's own log may be written here. */
        now = __atomic_load_n( &wine_nx_input_pumps, __ATOMIC_RELAXED );
        if (now != pumps)
        {
            if (paused) nxdrv_say( "[NXINPUT] the program pumps its messages again" );
            pumps = now;
            waited = 0;
            paused = 0;
        }
        else if (++waited >= 300)
        {
            waited = 0;
            if (!paused)
            {
                char line[96];

                paused = 1;
                snprintf( line, sizeof(line), "[NXINPUT] no messages pumped for 5 s, pad %x", buttons );
                nxdrv_say( line );
            }
        }
        usleep( 16000 );
    }
    __atomic_store_n( &wine_nx_input_thread_ended, 1, __ATOMIC_RELEASE );
    return NULL;
}

/* The runtime calls this on its way back to the launcher. This thread polls and
 * presents on its own, so it has to stop before the screen it presents to is
 * closed, and it has to be joined: libnx has no pthread_detach, and a thread
 * that was never joined keeps the heap pages of its stack, which the loader
 * cannot give back. */
void wine_nx_input_thread_stop(void)
{
    int i;

    if (!__atomic_load_n( &wine_nx_input_thread_started, __ATOMIC_ACQUIRE )) return;
    __atomic_store_n( &wine_nx_input_thread_quit, 1, __ATOMIC_RELEASE );
    /* Bounded: a thread stuck in the display driver must not stop the program
     * from closing, and its stack is then reported as still lent out. */
    for (i = 0; i < 300 && !__atomic_load_n( &wine_nx_input_thread_ended, __ATOMIC_ACQUIRE ); i++)
        usleep( 10000 );
    if (!__atomic_load_n( &wine_nx_input_thread_ended, __ATOMIC_ACQUIRE ))
    {
        nxdrv_trace( "[NXINPUT] polling did not stop; its stack stays lent out", 0, 0, 0, 0 );
        return;
    }
    pthread_join( wine_nx_input_thread_id, NULL );
    __atomic_store_n( &wine_nx_input_thread_started, 0, __ATOMIC_RELEASE );
    nxdrv_trace( "[NXINPUT] background polling ended", 0, 0, 0, 0 );
}

static void wine_nx_start_input_thread(void)
{
    if (__atomic_exchange_n( &wine_nx_input_thread_started, 1, __ATOMIC_ACQ_REL )) return;
    if (pthread_create( &wine_nx_input_thread_id, NULL, wine_nx_input_thread, NULL ))
    {
        __atomic_store_n( &wine_nx_input_thread_started, 0, __ATOMIC_RELEASE );
        nxdrv_trace( "[NXINPUT] thread create failed", 0, 0, 0, 0 );
        return;
    }
    nxdrv_trace( "[NXINPUT] background polling started", 0, 0, 0, 0 );
}

static struct wine_nx_surface *wine_nx_surface_from_base( struct window_surface *surface )
{
    return CONTAINING_RECORD( surface, struct wine_nx_surface, surface );
}

static void nxdrv_trace( const char *fmt, int a, int b, int c, int d )
{
    char buf[160];
    if (!&wine_nx_runtime_trace || !&wine_nx_runtime_verbose || !wine_nx_runtime_verbose) return;
    snprintf( buf, sizeof(buf), fmt, a, b, c, d );
    wine_nx_runtime_trace( buf );
}

static void nxdrv_trace_hot( const char *fmt, int a, int b, int c, int d )
{
    static unsigned int count;

    if (count++ >= 120) return;
    nxdrv_trace( fmt, a, b, c, d );
}

/* get_dib_stride / get_dib_image_size come from the win32u private headers. */

static void trace_surface_samples( const BITMAPINFO *color_info, const void *color_bits )
{
    static int sample_count;
    const DWORD *bits = color_bits;
    int width = color_info->bmiHeader.biWidth;
    int height = abs( color_info->bmiHeader.biHeight );
    DWORD p0, pc;
    int nonwhite = 0;
    int x, y;

    if (sample_count >= 40 || width <= 0 || height <= 0 || !bits) return;

    p0 = bits[0];
    pc = bits[(size_t)(height / 2) * width + width / 2];
    for (y = 0; y < height; y += 64)
    {
        for (x = 0; x < width; x += 64)
        {
            if ((bits[(size_t)y * width + x] & 0x00ffffff) != 0x00ffffff) nonwhite++;
        }
    }

    nxdrv_trace( "[NXDRV] samples p0=%08x pc=%08x nonwhite=%d",
                 (int)p0, (int)pc, nonwhite, 0 );
    sample_count++;
}

static BOOL wine_nx_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits );
static const struct window_surface_funcs wine_nx_surface_funcs;

static void wine_nx_surface_mark_full_dirty( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    RECT dirty = nx_surface->present_rect;

    if (dirty.right <= 0 || dirty.bottom <= 0) return;

    window_surface_lock( surface );
    surface->bounds = dirty;
    window_surface_unlock( surface );
}

static BOOL wine_nx_surface_present_full( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    char color_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *color_info = (BITMAPINFO *)color_buf;
    RECT dirty = nx_surface->present_rect;
    void *color_bits;
    BOOL ret = FALSE;

    if (dirty.right <= 0 || dirty.bottom <= 0) return FALSE;

    window_surface_lock( surface );
    color_bits = window_surface_get_color( surface, color_info );
    nxdrv_trace_hot( "[NXDRV] present_full bits=%d %dx%d",
                     color_bits ? 1 : 0, dirty.right, dirty.bottom, 0 );
    if (color_bits)
        ret = wine_nx_surface_flush( surface, &surface->rect, &dirty, color_info, color_bits,
                                     FALSE, NULL, NULL );
    if (ret) SetRectEmpty( &surface->bounds );
    window_surface_unlock( surface );
    return ret;
}

static BOOL wine_nx_surface_present_screen_rect( struct window_surface *surface, const RECT *screen_rect )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    char color_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *color_info = (BITMAPINFO *)color_buf;
    RECT dirty = *screen_rect;
    void *color_bits;
    BOOL ret = FALSE;

    OffsetRect( &dirty, -nx_surface->screen_origin.x, -nx_surface->screen_origin.y );
    if (!intersect_rect( &dirty, &dirty, &nx_surface->present_rect )) return FALSE;

    window_surface_lock( surface );
    color_bits = window_surface_get_color( surface, color_info );
    if (color_bits)
        ret = wine_nx_surface_flush( surface, &surface->rect, &dirty, color_info, color_bits,
                                     FALSE, NULL, NULL );
    window_surface_unlock( surface );
    return ret;
}

/* With wine_nx_surface_entries_mutex held. */
static struct wine_nx_surface_entry *find_surface_entry_locked( HWND hwnd, BOOL create )
{
    struct wine_nx_surface_entry *entry;

    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
        if (entry->hwnd == hwnd) return entry;

    if (!create) return NULL;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return NULL;
    entry->hwnd = hwnd;
    entry->next = wine_nx_surface_entries;
    wine_nx_surface_entries = entry;
    return entry;
}

static struct wine_nx_surface_entry *wine_nx_find_surface_entry( HWND hwnd, BOOL create )
{
    struct wine_nx_surface_entry *entry;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    entry = find_surface_entry_locked( hwnd, create );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
    return entry;
}

/* With the compositor, each window shows through the layer of its current
 * surface. A surface forgets its layer here before destroying it, so a layer
 * found under the entries mutex stays alive while the mutex is held. */
static void nxdrv_layer_rect( int *x, int *y, int *w, int *h );

static void wine_nx_show_window_layer( HWND hwnd, struct wine_nx_layer *layer, const POINT *origin,
                                       const RECT *present )
{
    struct wine_nx_surface_entry *entry;
    static POINT last_origin;
    static RECT last_present;
    static unsigned int told;
    int x = origin->x, y = origin->y;
    int w = present->right - present->left, h = present->bottom - present->top;

    /* Where the picture goes on the screen: its desktop rectangle, fitted with
     * the desktop when a program asked for a mode of its own. Asked before the
     * mutex below, which is not worth holding the display devices for. */
    nxdrv_layer_rect( &x, &y, &w, &h );

    /* Where a window's picture goes, whenever that changes. Said before the
     * mutex, since the log is not worth holding it for. */
    if (told < 24 && (origin->x != last_origin.x || origin->y != last_origin.y ||
                      present->right != last_present.right || present->bottom != last_present.bottom))
    {
        char line[176];

        last_origin = *origin;
        last_present = *present;
        told++;
        if (x != origin->x || y != origin->y || w != present->right - present->left ||
            h != present->bottom - present->top)
            snprintf( line, sizeof(line), "[NXCOMP] hwnd %x picture at %d,%d %dx%d of the desktop, drawn at %d,%d %dx%d of the %dx%d screen",
                      (int)(ULONG_PTR)hwnd, (int)origin->x, (int)origin->y,
                      (int)(present->right - present->left), (int)(present->bottom - present->top),
                      x, y, w, h, WINE_NX_SCREEN_W, WINE_NX_SCREEN_H );
        else
            snprintf( line, sizeof(line), "[NXCOMP] hwnd %x picture at %d,%d %dx%d of the %dx%d screen",
                      (int)(ULONG_PTR)hwnd, (int)origin->x, (int)origin->y,
                      (int)(present->right - present->left), (int)(present->bottom - present->top),
                      WINE_NX_SCREEN_W, WINE_NX_SCREEN_H );
        nxdrv_say( line );
    }

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    if ((entry = find_surface_entry_locked( hwnd, TRUE )))
    {
        /* A resized window gets a new surface; the old one can outlive this. */
        if (entry->layer && entry->layer != layer) wine_nx_layer_place( entry->layer, 0, 0, 0, 0 );
        entry->layer = layer;
    }
    /* The rect's sides are coordinates, not sizes: a window away from the
     * corner would get a layer as large again as its own offset. */
    wine_nx_layer_place( layer, x, y, w, h );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

static BOOL wine_nx_hide_window_layer( HWND hwnd )
{
    struct wine_nx_surface_entry *entry;
    BOOL ret = FALSE;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    if ((entry = find_surface_entry_locked( hwnd, FALSE )) && entry->layer)
    {
        wine_nx_layer_place( entry->layer, 0, 0, 0, 0 );
        ret = TRUE;
    }
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
    return ret;
}

static void wine_nx_forget_layer( struct wine_nx_layer *layer )
{
    struct wine_nx_surface_entry *entry;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
        if (entry->layer == layer) entry->layer = NULL;
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

/* Give the compositor the stacking order of the top-level windows, topmost first. */
static void wine_nx_restack_layers( void )
{
    struct wine_nx_layer *layers[256];
    struct wine_nx_surface_entry *entry;
    int count = 0;
    HWND hwnd;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    for (hwnd = get_window_relative( get_desktop_window(), GW_CHILD ); hwnd && count < (int)ARRAY_SIZE(layers);
         hwnd = get_window_relative( hwnd, GW_HWNDNEXT ))
    {
        if ((entry = find_surface_entry_locked( hwnd, FALSE )) && entry->layer) layers[count++] = entry->layer;
    }
    wine_nx_compositor_restack( layers, count );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

static BOOL wine_nx_get_cached_screen_rect( HWND hwnd, RECT *rect )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, FALSE );

    if (!entry || IsRectEmpty( &entry->screen_rect )) return FALSE;
    *rect = entry->screen_rect;
    return TRUE;
}

static void wine_nx_note_surface_present( HWND hwnd, const POINT *origin, const RECT *present_rect )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, TRUE );

    if (!entry) return;
    entry->screen_rect = *present_rect;
    OffsetRect( &entry->screen_rect, origin->x, origin->y );
    entry->visible = !IsRectEmpty( &entry->screen_rect );
}

static void wine_nx_note_surface_hidden( HWND hwnd )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, FALSE );

    if (entry) entry->visible = FALSE;
}

static BOOL wine_nx_present_hwnd_surface( HWND hwnd, const RECT *screen_rect )
{
    struct window_surface *surface, *driver_surface;
    struct ratio raw_dpi = {0};
    BOOL ret = FALSE;

    if (!(surface = window_surface_get( hwnd ))) return FALSE;
    get_win_monitor_dpi( hwnd, &raw_dpi );
    driver_surface = get_driver_window_surface( surface, raw_dpi );
    if (driver_surface && driver_surface->funcs == &wine_nx_surface_funcs)
        ret = screen_rect ? wine_nx_surface_present_screen_rect( driver_surface, screen_rect ) :
                            wine_nx_surface_present_full( driver_surface );
    window_surface_release( surface );
    return ret;
}

static void wine_nx_present_visible_popups( HWND except )
{
    struct wine_nx_surface_entry *entry;

    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
    {
        if (!entry->visible || entry->hwnd == except) continue;
        if (!(get_window_long( entry->hwnd, GWL_STYLE ) & WS_POPUP)) continue;
        wine_nx_present_hwnd_surface( entry->hwnd, NULL );
    }
}

static HWND wine_nx_find_popup_restore_owner( HWND hwnd, HWND owner_hint, const RECT *old_rect )
{
    HWND owner = owner_hint;

    if (!owner) owner = get_window_relative( hwnd, GW_OWNER );
    if (!owner && old_rect && !IsRectEmpty( old_rect ))
        owner = NtUserWindowFromPoint( (old_rect->left + old_rect->right) / 2,
                                      (old_rect->top + old_rect->bottom) / 2 );
    if (!owner) owner = get_active_window();
    if (!owner) owner = get_focus();
    if (owner) owner = NtUserGetAncestor( owner, GA_ROOT );
    if (owner == hwnd) owner = 0;
    return owner;
}

static void wine_nx_restore_popup_owner( HWND hwnd, HWND owner_hint, const RECT *old_rect )
{
    HWND owner;
    BOOL presented;

    if (!(get_window_long( hwnd, GWL_STYLE ) & WS_POPUP)) return;
    wine_nx_note_surface_hidden( hwnd );
    owner = wine_nx_find_popup_restore_owner( hwnd, owner_hint, old_rect );
    if (!owner || owner == hwnd) return;

    presented = wine_nx_present_hwnd_surface( owner, old_rect );
    wine_nx_present_visible_popups( hwnd );
    nxdrv_trace( "[NXDRV] restore popup=%x owner=%x presented=%d",
                 (int)(ULONG_PTR)hwnd, (int)(ULONG_PTR)owner, presented, 0 );
    if (!presented)
        NtUserRedrawWindow( owner, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                           RDW_ALLCHILDREN | RDW_UPDATENOW );
}

static void wine_nx_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    RECT clip = {0};
    UINT i;

    nx_surface->has_clip = FALSE;
    if (!rects || !count) return;

    for (i = 0; i < count; i++)
    {
        RECT rect;

        if (!intersect_rect( &rect, &rects[i], &nx_surface->present_rect )) continue;
        if (!nx_surface->has_clip) clip = rect;
        else union_rect( &clip, &clip, &rect );
        nx_surface->has_clip = TRUE;
    }
    nx_surface->clip_rect = clip;
}

/* Blit the dirty region of the (BGRX, top-down) DIB to the linear RGBA8888
 * framebuffer at the surface's screen position, clipped to the screen. */
static BOOL wine_nx_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( surface->hwnd, FALSE );
    RECT blit = *dirty;
    int sw = color_info->bmiHeader.biWidth;
    int sh = abs( color_info->bmiHeader.biHeight );
    int fbw = 0, fbh = 0, fbstride = 0;
    DWORD *fb;
    int sy, sx;

    (void)rect;

    /* The compositor keeps every pixel; where the window shows is its placement. */
    if (nx_surface->layer)
    {
        wine_nx_layer_update( nx_surface->layer, color_bits, sw, dirty->left, dirty->top, dirty->right, dirty->bottom );
        return TRUE;
    }

    /* A queued paint can flush after SWP_HIDEWINDOW.  Keep the DIB contents,
     * but do not put the closed popup back over its restored owner. */
    if (entry && !entry->visible) return TRUE;

    if (!intersect_rect( &blit, &blit, &nx_surface->present_rect )) return TRUE;
    if (nx_surface->has_clip && !intersect_rect( &blit, &blit, &nx_surface->clip_rect )) return TRUE;

    nxdrv_trace_hot( "[NXDRV] flush dirty=%d,%d-%d,%d", blit.left, blit.top, blit.right, blit.bottom );
    trace_surface_samples( color_info, color_bits );
    fb = wine_nx_fb_lock( &fbw, &fbh, &fbstride );
    nxdrv_trace_hot( "[NXDRV] fb_lock -> fb=%d fbw=%d fbh=%d stride=%d", fb ? 1 : 0, fbw, fbh, fbstride );
    if (!fb) return TRUE;

    for (sy = blit.top; sy < blit.bottom; sy++)
    {
        int v = sy;
        int dy = nx_surface->screen_origin.y + sy;
        const DWORD *src;
        DWORD *dst;

        if (v < 0 || v >= sh || dy < 0 || dy >= fbh) continue;
        src = (const DWORD *)color_bits + (size_t)v * sw;
        dst = fb + (size_t)dy * fbstride;
        for (sx = blit.left; sx < blit.right; sx++)
        {
            int u = sx;
            int dx = nx_surface->screen_origin.x + sx;
            DWORD p;
            if (u < 0 || u >= sw || dx < 0 || dx >= fbw) continue;
            p = src[u];
            /* BGRX (0x00RRGGBB) -> RGBA8888 (R,G,B,A bytes), opaque */
            dst[dx] = 0xff000000u | (p & 0x0000ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
        }
    }

    wine_nx_fb_unlock();
    /* Some applications (including OpenTTD's GDI backend) flush a surface
     * from an update path that does not pass through NtUserEndPaint.  The
     * dirty pixels are already copied above; publish that buffer here so the
     * Switch display cannot remain on the initial white frame until another
     * unrelated message arrives. */
    wine_nx_fb_present();
    return TRUE;
}

static void wine_nx_surface_destroy( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );

    /* The generic window_surface_release() frees the header storage. */
    if (!nx_surface->layer) return;
    wine_nx_forget_layer( nx_surface->layer );
    wine_nx_layer_destroy( nx_surface->layer );
    nx_surface->layer = NULL;
}

static const struct window_surface_funcs wine_nx_surface_funcs =
{
    .size = sizeof(struct wine_nx_surface),
    .set_clip = wine_nx_surface_set_clip,
    .flush = wine_nx_surface_flush,
    .destroy = wine_nx_surface_destroy,
};

/**********************************************************************
 *           wine_nx_drv_UpdateDisplayDevices
 *
 * Report a single 1280x720 primary monitor so win32u has a real virtual
 * screen; without it the desktop is 0x0 and every window clips to nothing.
 */

UINT wine_nx_drv_UpdateDisplayDevices( const struct gdi_device_manager *dm, void *param )
{
    static const DWORD source_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP |
                                      DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_VGA_COMPATIBLE;
    RECT rc = { 0, 0, WINE_NX_SCREEN_W, WINE_NX_SCREEN_H };
    struct pci_id pci_id = { 0 };
    struct gdi_monitor monitor = { .rc_monitor = rc, .rc_work = rc };
    DEVMODEW mode =
    {
        .dmSize   = sizeof(mode),
        .dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY,
        .dmBitsPerPel = 32, .dmPelsWidth = WINE_NX_SCREEN_W, .dmPelsHeight = WINE_NX_SCREEN_H,
        .dmDisplayFrequency = 60,
    };
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    DEVMODEW current = mode;

    dm->add_gpu( "Tegra X1 GM20B", &pci_id, NULL, param );
    dm->add_source( "Default", source_flags, dpi, param );
    dm->add_monitor( &monitor, param );
    current.dmFields |= DM_POSITION;
    dm->add_modes( &current, 1, &mode, param );
    nxdrv_trace( "[NXDRV] UpdateDisplayDevices -> %dx%d", WINE_NX_SCREEN_W, WINE_NX_SCREEN_H, 0, 0 );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *           wine_nx_drv_ProcessEvents
 */
/* The button flags that take the delivered buttons (last) to the held ones,
 * in two steps: a button pressed and released since the last delivery clicks
 * (down, then up), and one released and pressed again goes up, then down. */
static void wine_nx_pointer_flags( unsigned int last, unsigned int held, unsigned int pressed,
                                   unsigned int released, DWORD *first, DWORD *second )
{
    static const struct { unsigned int button; DWORD down, up; } map[] =
    {
        { WINE_NX_POINTER_LEFT, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP },
        { WINE_NX_POINTER_RIGHT, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP },
        { 4, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
    };
    unsigned int i;

    *first = *second = 0;
    for (i = 0; i < ARRAY_SIZE(map); i++)
    {
        unsigned int button = map[i].button;

        if (last & button)
        {
            if ((held & button) && !(released & button)) continue;
            *first |= map[i].up;
            if (held & button) *second |= map[i].down;
        }
        else if ((held | pressed) & button)
        {
            *first |= map[i].down;
            if (!(held & button)) *second |= map[i].up;
        }
    }
}

/* A key goes to the focus window of the foreground thread's input, and to its
 * active window when it has no focus; with neither, the server drops it and
 * only the key state is left for a program that asks (server/queue.c,
 * queue_hardware_message). A program showing a film before its first dialog
 * can be in that state, so say where a key would go, once a second while keys
 * are being sent: it tells a key the program ignored from one that never
 * arrived. Both windows are named, the foreground thread's and this thread's,
 * because a key sent to no window in particular takes the first. */
static void nxdrv_trace_key_target(void)
{
    static DWORD last;
    GUITHREADINFO foreground = { .cbSize = sizeof(foreground) };
    GUITHREADINFO mine = { .cbSize = sizeof(mine) };
    char line[176];
    DWORD now = NtGetTickCount();

    if (!&wine_nx_runtime_trace) return;
    if (last && now - last < 1000) return;
    last = now;
    if (!NtUserGetGUIThreadInfo( 0, &foreground )) memset( &foreground, 0, sizeof(foreground) );
    if (!NtUserGetGUIThreadInfo( GetCurrentThreadId(), &mine )) memset( &mine, 0, sizeof(mine) );
    snprintf( line, sizeof(line), "[NXINPUT] keys: foreground %x focus %x active %x, ours focus %x active %x",
              (int)(ULONG_PTR)NtUserGetForegroundWindow(), (int)(ULONG_PTR)foreground.hwndFocus,
              (int)(ULONG_PTR)foreground.hwndActive, (int)(ULONG_PTR)mine.hwndFocus,
              (int)(ULONG_PTR)mine.hwndActive );
    nxdrv_say( line );
}

static void wine_nx_send_key( const struct input_key_event *event )
{
    INPUT input = {0};
    UINT scan;
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = event->code == INPUT_NUMPAD_ENTER ? VK_RETURN : event->code;
    input.ki.dwFlags = event->down ? 0 : KEYEVENTF_KEYUP;
    scan = NtUserMapVirtualKeyEx( input.ki.wVk, MAPVK_VK_TO_VSC_EX, NtUserGetKeyboardLayout( 0 ) );
    input.ki.wScan = scan & 0xff;
    if ((scan & 0xff00) == 0xe000 || event->code == INPUT_NUMPAD_ENTER) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
    nxdrv_note_key_sent( input.ki.wVk, !event->down );
    nxdrv_trace_key_target();
}

static void wine_nx_send_extra_mouse( unsigned int last, unsigned int held, unsigned int pressed,
                                      unsigned int released, int vertical, int horizontal )
{
    for (unsigned int i = 3; i < 5; i++)
    {
        unsigned int bit = 1u << i;
        int was = !!(last & bit), now = !!(held & bit);
        INPUT input = { .type = INPUT_MOUSE };
        input.mi.mouseData = i == 3 ? XBUTTON1 : XBUTTON2;
        if (was ? (now && !(released & bit)) : !(now || (pressed & bit))) continue;
        input.mi.dwFlags = was ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
        NtUserSendHardwareInput( 0, 0, &input, 0 );
        if (was == now)
        {
            input.mi.dwFlags = was ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            NtUserSendHardwareInput( 0, 0, &input, 0 );
        }
    }
    if (vertical)
    {
        INPUT input = { .type = INPUT_MOUSE };
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = vertical;
        NtUserSendHardwareInput( 0, 0, &input, 0 );
    }
    if (horizontal)
    {
        INPUT input = { .type = INPUT_MOUSE };
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = horizontal;
        NtUserSendHardwareInput( 0, 0, &input, 0 );
    }
}

/* A touch points at a place on the screen. */
static void wine_nx_send_mouse( int x, int y, DWORD flags )
{
    INPUT input = {0};

    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | flags;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/* The stick moves by an amount, which is what a mouse does: the cursor may be
 * clipped to the screen or held still by a program that has taken the mouse
 * for itself, and the movement still has to be told in full. */
static void wine_nx_send_mouse_motion( int dx, int dy )
{
    INPUT input = {0};

    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/* The screen the panel shows and the desktop the server keeps. A program that
 * asks for a display mode smaller than the screen (a 640x480 game on the
 * 1280x720 panel) makes the desktop that size, and the screen shows that
 * desktop fitted and centred, its proportions kept: 960x720 at 160,0, with
 * bars either side. That rectangle is Wine's own (`map_rect_virt_to_raw()` in
 * `sysparams.c`, the one the server clips the cursor to), and the OpenGL path
 * draws the picture in it, so a finger, the arrow and the picture all say the
 * same numbers. Without a mode change the two are the same rectangle and this
 * is a plain copy. */
struct nxdrv_screen_map
{
    int desktop_w, desktop_h;  /* the desktop, in the program's pixels */
    int pane_x, pane_y;        /* where the desktop's picture starts on the screen */
    int pane_w, pane_h;        /* and how many screen pixels it takes */
};

/* The rectangle a picture of this size takes on the screen: its proportions
 * kept, as large as fits, centred. A 640x480 picture is 960x720 at 160,0 of
 * the 1280x720 panel, with bars either side. */
static BOOL nxdrv_fit_rect( int w, int h, RECT *pane )
{
    int pane_w, pane_h;

    if (w <= 0 || h <= 0) return FALSE;
    if ((long long)h * WINE_NX_SCREEN_W >= (long long)w * WINE_NX_SCREEN_H)
    {
        /* Taller than the screen, so the height is what fills it. */
        pane_h = WINE_NX_SCREEN_H;
        pane_w = (int)(((long long)w * WINE_NX_SCREEN_H + h / 2) / h);
    }
    else
    {
        pane_w = WINE_NX_SCREEN_W;
        pane_h = (int)(((long long)h * WINE_NX_SCREEN_W + w / 2) / w);
    }
    if (pane_w < 1) pane_w = 1;
    if (pane_h < 1) pane_h = 1;
    pane->left = (WINE_NX_SCREEN_W - pane_w) / 2;
    pane->top = (WINE_NX_SCREEN_H - pane_h) / 2;
    pane->right = pane->left + pane_w;
    pane->bottom = pane->top + pane_h;
    return TRUE;
}

/* The display mode a program asked for and did not get: the desktop is virtual,
 * so the change is answered "done" and changes nothing, and this is what the
 * picture the program draws is fitted to. Zero when it asked for nothing. */
static int nxdrv_mode_w, nxdrv_mode_h;

/* Told as the program asks, before the desktop decides to leave itself alone.
 * The picture is fitted by it, so a game in a mode of its own is 960x720 at
 * 160,0 on the 1280x720 panel rather than a small window in the corner. */
LONG wine_nx_drv_ChangeDisplaySettings( DEVMODEW *devmode, LPCWSTR devname, HWND hwnd, DWORD flags, void *lparam )
{
    int w = devmode ? (int)devmode->dmPelsWidth : 0;
    int h = devmode ? (int)devmode->dmPelsHeight : 0;

    if (!(flags & CDS_TEST) && (w != nxdrv_mode_w || h != nxdrv_mode_h))
    {
        char line[176];
        RECT pane;

        nxdrv_mode_w = w;
        nxdrv_mode_h = h;
        if (w > 0 && h > 0 && nxdrv_fit_rect( w, h, &pane ))
            snprintf( line, sizeof(line), "[NXDRV] the program asks for %dx%d; its picture is drawn at %d,%d %dx%d of the %dx%d screen",
                      w, h, (int)pane.left, (int)pane.top, (int)(pane.right - pane.left),
                      (int)(pane.bottom - pane.top), WINE_NX_SCREEN_W, WINE_NX_SCREEN_H );
        else
            snprintf( line, sizeof(line), "[NXDRV] the program asks for no mode of its own; the picture is the screen" );
        nxdrv_say( line );
    }
    return DISP_CHANGE_SUCCESSFUL;
}

/* Where a picture of this size goes on the screen: the fit, when it is the
 * picture the program asked for a mode for. Anything else -- a window that is
 * not the whole of the program's own picture -- is where it stands. */
BOOL wine_nx_picture_rect( SIZE size, RECT *pane )
{
    if (size.cx != nxdrv_mode_w || size.cy != nxdrv_mode_h) return FALSE;
    return nxdrv_fit_rect( size.cx, size.cy, pane );
}

/* The screen a picture is drawn on, for the OpenGL path, which has to place
 * the picture in the same rectangle this file maps the finger through. */
int wine_nx_screen_width( void )
{
    return WINE_NX_SCREEN_W;
}

int wine_nx_screen_height( void )
{
    return WINE_NX_SCREEN_H;
}

/* One desktop for everything, the screen's size, marked virtual: a program
 * that asks for a display mode of its own is told the change was made and goes
 * on drawing in its own window, which stays inside this desktop instead of
 * shrinking it. Nothing then moves the desktop out from under the pointer --
 * a point on the screen is the point in the program -- and what the program
 * does not cover is the desktop's own background, as it is on Windows.
 * Called by the runtime before the program starts. */
#define NXDRV_DESKTOP_ALL_ACCESS 0x01ff   /* as winstation.c counts it */

BOOL wine_nx_make_virtual_desktop( void )
{
    /* No wide literal here: the unix side is built where wchar_t is not the
     * Windows WCHAR, and this file never had one before. */
    static WCHAR name[] = {'D', 'e', 'f', 'a', 'u', 'l', 't', 0};
    UNICODE_STRING string = { sizeof(name) - sizeof(WCHAR), sizeof(name), name };
    OBJECT_ATTRIBUTES attr = {0};
    DEVMODEW devmode = {0};
    HDESK desktop;

    attr.Length = sizeof(attr);
    attr.ObjectName = &string;
    devmode.dmSize = sizeof(devmode);
    devmode.dmPelsWidth = WINE_NX_SCREEN_W;
    devmode.dmPelsHeight = WINE_NX_SCREEN_H;

    /* The desktop is already there: this marks it, and keeps the driver's own
     * size for it rather than letting a program's mode change take it. */
    if (!(desktop = NtUserCreateDesktopEx( &attr, NULL, &devmode, DF_WINE_VIRTUAL_DESKTOP,
                                          NXDRV_DESKTOP_ALL_ACCESS, 0 )))
        return FALSE;
    NtUserCloseDesktop( desktop );
    return TRUE;
}

/* The desktop, and where the screen shows its pixels. */
static BOOL nxdrv_screen_map_get( struct nxdrv_screen_map *map )
{
    int screen_w = NtUserGetSystemMetrics( SM_CXSCREEN ), screen_h = NtUserGetSystemMetrics( SM_CYSCREEN );
    int w = nxdrv_mode_w > 0 ? nxdrv_mode_w : screen_w;
    int h = nxdrv_mode_h > 0 ? nxdrv_mode_h : screen_h;
    RECT pane;

    if (w <= 0 || h <= 0) return FALSE;
    map->desktop_w = w;
    map->desktop_h = h;
    if (!nxdrv_fit_rect( w, h, &pane ))
    {
        /* Nothing to ask: the picture is the screen, where it stands. */
        pane.left = pane.top = 0;
        pane.right = w;
        pane.bottom = h;
    }
    map->pane_x = pane.left;
    map->pane_y = pane.top;
    map->pane_w = pane.right - pane.left;
    map->pane_h = pane.bottom - pane.top;
    if (map->pane_w != w || map->pane_h != h)
    {
        static int last_w, last_h, last_x, last_y, told;
        char line[176];

        /* Said once per shape: the picture's rectangle on the screen, which is
         * also what the pointer is mapped by and the cursor clipped to. */
        if (told++ < 12 && (w != last_w || h != last_h || pane.left != last_x || pane.top != last_y))
        {
            last_w = w;
            last_h = h;
            last_x = pane.left;
            last_y = pane.top;
            snprintf( line, sizeof(line), "[NXDRV] the %dx%d desktop is drawn at %d,%d %dx%d of the %dx%d screen",
                      w, h, map->pane_x, map->pane_y, map->pane_w, map->pane_h, WINE_NX_SCREEN_W, WINE_NX_SCREEN_H );
            nxdrv_say( line );
        }
    }
    return TRUE;
}

static int nxdrv_scale( int value, int from, int to )
{
    long long scaled;

    if (from <= 0) return 0;
    scaled = ((long long)value * to + (value < 0 ? -from / 2 : from / 2)) / from;
    return (int)scaled;
}

/* A point on the screen, as the desktop sees it: inside the desktop, so a
 * finger in a bar still clicks the nearest edge of the picture rather than
 * nothing at all. */
static void nxdrv_screen_point_to_desktop( const struct nxdrv_screen_map *map, int *x, int *y )
{
    int dx = nxdrv_scale( *x - map->pane_x, map->pane_w, map->desktop_w );
    int dy = nxdrv_scale( *y - map->pane_y, map->pane_h, map->desktop_h );

    *x = dx < 0 ? 0 : dx >= map->desktop_w ? map->desktop_w - 1 : dx;
    *y = dy < 0 ? 0 : dy >= map->desktop_h ? map->desktop_h - 1 : dy;
}

/* The other way: a point of the desktop's own pixels, where the screen shows
 * it. The server answers in the desktop's pixels, while the finger and the
 * arrow live on the screen. */
static void nxdrv_desktop_point_to_screen( const struct nxdrv_screen_map *map, int *x, int *y )
{
    *x = map->pane_x + nxdrv_scale( *x, map->desktop_w, map->pane_w );
    *y = map->pane_y + nxdrv_scale( *y, map->desktop_h, map->pane_h );
}

/* A window's rectangle in the picture's pixels, as the screen shows it. The
 * program's own window -- the one of the size it asked a mode for -- goes into
 * the fit, wherever the desktop has put it; everything else is scaled with the
 * picture around it. */
static void nxdrv_layer_rect( int *x, int *y, int *w, int *h )
{
    struct nxdrv_screen_map map;
    RECT pane;

    if (nxdrv_mode_w > 0 && *w == nxdrv_mode_w && *h == nxdrv_mode_h && nxdrv_fit_rect( *w, *h, &pane ))
    {
        *x = pane.left;
        *y = pane.top;
        *w = pane.right - pane.left;
        *h = pane.bottom - pane.top;
        return;
    }
    if (!nxdrv_screen_map_get( &map )) return;
    nxdrv_desktop_point_to_screen( &map, x, y );
    *w = nxdrv_scale( *w, map.desktop_w, map.pane_w );
    *h = nxdrv_scale( *h, map.desktop_h, map.pane_h );
}

/* Said once, and with the first few points: the two numbers the mapping is
 * made of, so a click that lands beside the finger is a number to read. */
static void nxdrv_trace_pointer_target( const struct nxdrv_screen_map *map, int screen_x, int screen_y,
                                        int x, int y, DWORD flags, BOOL finger )
{
    static unsigned int count;
    char line[208];

    if (count >= 8) return;
    if (!count++)
    {
        snprintf( line, sizeof(line),
                  "[MOUSE] the screen is %dx%d, the desktop is %dx%d, its picture at %d,%d %dx%d",
                  WINE_NX_SCREEN_W, WINE_NX_SCREEN_H, map->desktop_w, map->desktop_h,
                  map->pane_x, map->pane_y, map->pane_w, map->pane_h );
        nxdrv_say( line );
    }
    /* Said apart: a finger on the screen and the stick come through the same
     * mapping, and a log that calls both a finger says nothing about whether
     * the touchscreen was used at all. */
    snprintf( line, sizeof(line), finger
              ? "[MOUSE] the finger's %d,%d (flags %x) is %d,%d to the program"
              : "[MOUSE] the stick's %d,%d (flags %x) is %d,%d to the program",
              screen_x, screen_y, (int)flags, x, y );
    nxdrv_say( line );
}

static void wine_nx_flush_mouse(void)
{
    static const INPUT input = { .type = INPUT_MOUSE, .mi.dwFlags = MOUSEEVENTF_MOVE_NOCOALESCE };

    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/* Where the cursor really is. NtUserGetCursorPos is not it: without a recent
 * change it asks the display driver, which is this one, and maps the answer
 * through the thread's DPI. The stick works in the screen's own coordinates,
 * which is what the desktop holds. */
static BOOL wine_nx_cursor_pos( POINT *pos )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const desktop_shm_t *desktop_shm;
    NTSTATUS status;

    while ((status = get_shared_desktop( &lock, &desktop_shm )) == STATUS_PENDING)
    {
        pos->x = desktop_shm->cursor.x;
        pos->y = desktop_shm->cursor.y;
    }
    return !status;
}

/* One key, down or up, with a real scan code the way wine_nx_send_key
 * derives it - so DirectInput's keyboard state buffer and GetAsyncKeyState
 * see it, not only whatever WM_CHAR a message loop's TranslateMessage makes
 * of it. */
static void wine_nx_send_vk( HKL layout, BYTE vk, BOOL up )
{
    INPUT input = {0};
    UINT scan = NtUserMapVirtualKeyEx( vk, MAPVK_VK_TO_VSC_EX, layout );

    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan & 0xff;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if ((scan & 0xff00) == 0xe000) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
    if (&wine_nx_runtime_trace)
    {
        BYTE state[256];
        char line[80];

        /* Where the key ended up, as the desktop holds it: a key the system
         * kept reads 0x80 down or 0 up here, and a key a low-level hook took
         * before the desktop saw it reads as nothing happened at all. This is
         * what a game polling GetAsyncKeyState would be told, so a film it
         * cannot skip says here whether the key was ever there to be read. */
        if (get_async_keyboard_state( state ))
            snprintf( line, sizeof(line), "[OSK] key %02x %s, system state %02x", vk,
                      up ? "up" : "down", state[vk] );
        else
            snprintf( line, sizeof(line), "[OSK] key %02x %s, system state unknown", vk,
                      up ? "up" : "down" );
        wine_nx_runtime_trace( line );
    }
    nxdrv_note_key_sent( vk, up );
    nxdrv_trace_key_target();
}

/* The floating keyboard (horizon-wine/source/osk.c) queues the keys pressed
 * on it, each due at its time; they go to whichever window has focus, as the
 * controller's do. */
extern int wine_nx_osk_next_key( unsigned long long now_ns, unsigned short *vk, int *up ) __attribute__((weak));
extern unsigned long long wine_nx_osk_clock( void ) __attribute__((weak));
extern void wine_nx_keyboard_open( void ) __attribute__((weak));

static BOOL wine_nx_send_keyboard_keys( void )
{
    unsigned long long now;
    unsigned short vk;
    HKL layout = 0;
    BOOL sent = FALSE;
    int up;

    if (!&wine_nx_osk_next_key || !&wine_nx_osk_clock) return FALSE;
    now = wine_nx_osk_clock();
    while (wine_nx_osk_next_key( now, &vk, &up ))
    {
        if (!sent) layout = NtUserGetKeyboardLayout( 0 );
        wine_nx_send_vk( layout, vk, up );   /* and says where it went */
        sent = TRUE;
        /* One key event per look at the program's messages: a key that went
         * down is down while the program is told anything else, so a program
         * that polls the keyboard once a frame -- a game skipping an intro
         * with Escape -- sees it down, and a down and its up are never handed
         * over in the same call. */
        if (up) break;
    }
    return sent;
}

/* Whether to draw the arrow, from the cursor the program set and its show
 * count. Until a program sets a cursor there is none and the arrow is shown; a
 * program that later sets none, over a cursor it draws itself, hides it, and
 * so does a negative show count (ShowCursor). */
static BOOL wine_nx_cursor_visible_for( HCURSOR cursor, int count, BOOL *seen )
{
    if (cursor) *seen = TRUE;
    return count >= 0 && (cursor || !*seen);
}

/* Wine tells the display driver about cursor changes with WM_WINE_SETCURSOR,
 * which the Horizon server does not queue. Read the state instead. */
static void wine_nx_update_cursor( void )
{
    static BOOL seen;
    struct object_lock lock = OBJECT_LOCK_INIT;
    const input_shm_t *input_shm;
    HCURSOR cursor = 0;
    int count = 0;
    NTSTATUS status;

    while ((status = get_shared_input( 0, &lock, &input_shm )) == STATUS_PENDING)
    {
        cursor = wine_server_ptr_handle( input_shm->cursor );
        count = input_shm->cursor_count;
    }
    if (status) return;
    wine_nx_cursor_show( wine_nx_cursor_visible_for( cursor, count, &seen ) );
}

BOOL wine_nx_drv_ProcessEvents( DWORD mask )
{
    static unsigned int delivering;
    struct input_snapshot snapshot;
    const struct input_key_event *key_events = snapshot.keys;
    unsigned int key_count, key_after;
    int wheel, hwheel;
    static unsigned int last_buttons;
    unsigned int buttons, pressed, released;
    DWORD first, second;
    BOOL moved, keys, placed, stepped, map_scaled;
    struct nxdrv_screen_map map = {0};
    int x, y, dx, dy;

    (void)mask;
    if (__atomic_exchange_n( &delivering, 1, __ATOMIC_ACQUIRE )) return FALSE;
    __atomic_add_fetch( &wine_nx_input_pumps, 1, __ATOMIC_RELAXED );
    wine_nx_fb_present();
    /* Poll here too, then deliver everything the polls saw since the last
     * call, including those of the background thread. */
    wine_nx_pointer_poll( &x, &y, &buttons );
    wine_nx_input_take( &snapshot );
    x = snapshot.x;
    y = snapshot.y;
    dx = snapshot.dx;
    dy = snapshot.dy;
    buttons = snapshot.buttons;
    pressed = snapshot.pressed;
    released = snapshot.released;
    moved = snapshot.moved;
    placed = snapshot.placed;
    stepped = dx || dy;
    key_count = snapshot.key_count;
    wheel = snapshot.wheel;
    hwheel = snapshot.hwheel;
    key_after = key_count;
    while (key_after && !key_events[key_after - 1].down) key_after--;
    for (unsigned int i = 0; i < key_after; i++) wine_nx_send_key( key_events + i );
    wine_nx_pointer_flags( last_buttons, buttons, pressed, released, &first, &second );
    /* A program that asked for a display mode smaller than the screen has a
     * desktop of that size, drawn fitted and centred -- 640x480 becomes 960x720
     * at 160,0 -- so every point has to be said in the program's own pixels,
     * and the stick points rather than pushes: a whole screen pixel is not a
     * whole desktop pixel any more. Without a mode change the picture is the
     * screen and this is a plain copy. */
    map_scaled = nxdrv_screen_map_get( &map ) &&
                 (map.pane_w != map.desktop_w || map.pane_h != map.desktop_h);
    {
        int desktop_x = x, desktop_y = y;

        if (map_scaled) nxdrv_screen_point_to_desktop( &map, &desktop_x, &desktop_y );
        if (placed || first || (stepped && map_scaled))
        {
            /* The point goes with every click, even a tap that moved nothing:
             * without MOUSEEVENTF_MOVE the server takes the button's place
             * from where the cursor already was, not from the point said. */
            DWORD flags = (placed || (stepped && map_scaled) || (first & MOUSEEVENTF_LEFTDOWN)
                           ? MOUSEEVENTF_MOVE : 0) | first;

            nxdrv_trace_pointer_target( &map, x, y, desktop_x, desktop_y, flags, placed );
            wine_nx_send_mouse( desktop_x, desktop_y, flags );
            if (stepped && map_scaled) stepped = FALSE;  /* it went with the point */
        }
        if (stepped) wine_nx_send_mouse_motion( dx, dy );
        if (second) wine_nx_send_mouse( desktop_x, desktop_y, second );
    }
    wine_nx_flush_mouse();
    wine_nx_send_extra_mouse( last_buttons, buttons, pressed, released, wheel, hwheel );
    for (unsigned int i = key_after; i < key_count; i++) wine_nx_send_key( key_events + i );
    if (moved || placed || stepped)
    {
        POINT pos;

        /* The arrow belongs where the cursor is, which after movement is for
         * the server to say: it clips the cursor to the screen, and holds it
         * still for a program that has taken the mouse for itself. Nothing is
         * lost by snapping to it, the movement having been sent already. What
         * it answers is also the proof that the point arrived: the first few
         * are said whether or not it agrees, and a disagreement is followed. */
        if (wine_nx_cursor_pos( &pos ))
        {
            /* The server answers in the desktop's own pixels; the finger and
             * the arrow are on the screen, so the answer is put there first --
             * else a tap in a shrunken desktop would move the finger to the
             * same numbers read as screen pixels, and the next tap would be
             * measured from the wrong place. */
            static unsigned int told;
            static int followed;
            int cursor_x = pos.x, cursor_y = pos.y;

            if (map_scaled) nxdrv_desktop_point_to_screen( &map, &cursor_x, &cursor_y );
            if (told++ < 8 && &wine_nx_runtime_trace)
            {
                char line[144];

                snprintf( line, sizeof(line), "[MOUSE] the cursor is at %d,%d of the screen, the finger at %d,%d",
                          cursor_x, cursor_y, x, y );
                wine_nx_runtime_trace( line );
            }
            if (cursor_x != x || cursor_y != y)
            {
                /* Quiet once it is plainly working, loud enough to see that it is. */
                if (followed++ < 8 && &wine_nx_runtime_trace)
                {
                    char line[144];

                    snprintf( line, sizeof(line), "[NXINPUT] the cursor is at %d,%d, the stick moved %d,%d",
                              cursor_x, cursor_y, dx, dy );
                    wine_nx_runtime_trace( line );
                }
                wine_nx_pointer_follow( cursor_x, cursor_y );
            }
        }
    }
    if (first) nxdrv_trace( "[NXINPUT] buttons=%x flags=%x,%x x=%d", buttons, first, second, x );
    else if (moved) nxdrv_trace_hot( "[NXINPUT] move x=%d y=%d buttons=%x", x, y, buttons, 0 );
    last_buttons = buttons;
    keys = key_count || wheel || hwheel || ((pressed | released) & 0x1c);
    keys |= wine_nx_send_keyboard_keys();
    wine_nx_update_cursor();
    wine_nx_fb_present();
    __atomic_store_n( &delivering, 0, __ATOMIC_RELEASE );
    return moved || first || keys;
}

/**********************************************************************
 *           wine_nx_drv_SetCursor
 *
 * For a server that queues WM_WINE_SETCURSOR; the shared state is authoritative.
 */
void wine_nx_drv_SetCursor( HWND hwnd, HCURSOR cursor )
{
    (void)hwnd;
    (void)cursor;
    wine_nx_update_cursor();
}

/**********************************************************************
 *           wine_nx_drv_ShowSoftwareKeyboard
 *
 * Shows the floating keyboard; what is typed on it goes to whichever window
 * has focus, so hwnd is not needed.
 */
BOOL wine_nx_drv_ShowSoftwareKeyboard( HWND hwnd )
{
    (void)hwnd;
    if (!&wine_nx_keyboard_open) return FALSE;
    wine_nx_keyboard_open();
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_GetCursorPos
 *
 * Where the pointer is, for GetCursorPos().  win32u asks the driver only when
 * the server's answer is older than 100 ms, and the null driver's answer is
 * "no": a program that draws its own cursor read a position that stopped
 * moving, or a call that failed, whenever the pointer stood still.  The
 * desktop's own state is the answer, in the desktop's pixels, which is what
 * win32u hands the program unchanged.
 */
BOOL wine_nx_drv_GetCursorPos( POINT *pos )
{
    return wine_nx_cursor_pos( pos );
}

/**********************************************************************
 *           wine_nx_drv_SetCursorPos
 */
BOOL wine_nx_drv_SetCursorPos( INT x, INT y )
{
    struct nxdrv_screen_map map;

    /* The point comes in the desktop's own pixels, which is what the server
     * keeps; the arrow lives on the screen, so it goes where the desktop shows
     * that point. The same when the two are alike. */
    if (nxdrv_screen_map_get( &map ) && (map.pane_w != map.desktop_w || map.pane_h != map.desktop_h))
        nxdrv_desktop_point_to_screen( &map, &x, &y );
    wine_nx_pointer_set_pos( x, y );
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_CreateWindow
 */
BOOL wine_nx_drv_CreateWindow( HWND hwnd )
{
    nxdrv_trace( "[NXDRV] CreateWindow hwnd=%p", (int)(ULONG_PTR)hwnd, 0, 0, 0 );
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_CreateWindowSurface
 */
BOOL wine_nx_drv_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect,
                                      struct window_surface **surface )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    int width  = surface_rect->right - surface_rect->left;
    int height = surface_rect->bottom - surface_rect->top;

    if (*surface) window_surface_release( *surface );
    *surface = NULL;
    if (width <= 0 || height <= 0) return TRUE;

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size( info );
    info->bmiHeader.biCompression = BI_RGB;

    *surface = window_surface_create( &wine_nx_surface_funcs,
                                      hwnd, surface_rect, info, 0 );
    /* Made with the surface, the layer gets every flush of it. win32u fills a
     * new surface with white: a window that never paints with GDI, like a
     * Direct3D game's, would show that white whenever the compositor has the
     * screen, so it starts black, like the layer. */
    if (*surface && wine_nx_compositor_enabled())
    {
        memset( window_surface_get_color( *surface, info ), 0, info->bmiHeader.biSizeImage );
        wine_nx_surface_from_base( *surface )->layer = wine_nx_layer_create( width, height );
    }
    nxdrv_trace( "[NXDRV] CreateWindowSurface rect=%d,%d %dx%d", surface_rect->left, surface_rect->top,
                 width, height );
    nxdrv_trace( "[NXDRV] surface_create -> %d", *surface ? 1 : 0, 0, 0, 0 );
    wine_nx_start_input_thread();
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_WindowPosChanged
 *
 * Present the current surface contents after a geometry/visibility change.
 */
void wine_nx_drv_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                   const struct window_rects *new_rects, struct window_surface *surface )
{
    BOOL initial_redraw = FALSE;
    RECT old_screen_rect;
    BOOL has_old_screen_rect = wine_nx_get_cached_screen_rect( hwnd, &old_screen_rect );

    nxdrv_trace( "[NXDRV] WindowPosChanged surface=%d swp=%x", surface ? 1 : 0, swp_flags, 0, 0 );
    if (swp_flags & SWP_HIDEWINDOW)
    {
        wine_nx_note_surface_hidden( hwnd );
        /* The compositor shows what was under the window by itself. */
        if (wine_nx_hide_window_layer( hwnd )) return;
        wine_nx_restore_popup_owner( hwnd, owner_hint, has_old_screen_rect ? &old_screen_rect : NULL );
        wine_nx_fb_present();
        return;
    }
    if (surface)
    {
        if (surface->funcs == &wine_nx_surface_funcs)
        {
            struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
            RECT allocation = { 0, 0, surface->rect.right - surface->rect.left,
                                surface->rect.bottom - surface->rect.top };
            int new_width = new_rects->visible.right - new_rects->visible.left;
            int new_height = new_rects->visible.bottom - new_rects->visible.top;

            if (!nx_surface->layer && nx_surface->initial_redraw_done &&
                (nx_surface->screen_origin.x != new_rects->visible.left ||
                 nx_surface->screen_origin.y != new_rects->visible.top ||
                 nx_surface->present_rect.right != new_width ||
                 nx_surface->present_rect.bottom != new_height))
                wine_nx_restore_popup_owner( hwnd, owner_hint,
                                             has_old_screen_rect ? &old_screen_rect : NULL );

            initial_redraw = !nx_surface->initial_redraw_done;
            nx_surface->initial_redraw_done = TRUE;
            nx_surface->screen_origin.x = new_rects->visible.left;
            nx_surface->screen_origin.y = new_rects->visible.top;
            nx_surface->present_rect.left = nx_surface->present_rect.top = 0;
            nx_surface->present_rect.right = new_width;
            nx_surface->present_rect.bottom = new_height;
            intersect_rect( &nx_surface->present_rect, &nx_surface->present_rect, &allocation );
            wine_nx_note_surface_present( hwnd, &nx_surface->screen_origin, &nx_surface->present_rect );
            if (nx_surface->layer)
            {
                /* Only the visible part is drawn, never the rest of the allocation. */
                wine_nx_show_window_layer( hwnd, nx_surface->layer, &nx_surface->screen_origin,
                                           &nx_surface->present_rect );
                wine_nx_restack_layers();
            }
            nxdrv_trace( "[NXDRV] present rect=%d,%d %dx%d",
                         nx_surface->screen_origin.x, nx_surface->screen_origin.y,
                         nx_surface->present_rect.right, nx_surface->present_rect.bottom );

            wine_nx_surface_mark_full_dirty( surface );
            if (!wine_nx_surface_present_full( surface ))
                window_surface_flush( surface );

            /* Child controls can paint while the top-level still owns the dummy
             * surface during ShowWindow.  Repaint the complete hierarchy once
             * the real surface has been installed so those discarded pixels are
             * produced again on the drawable surface. */
            if (initial_redraw)
            {
                nxdrv_trace( "[NXDRV] initial redraw hwnd=%p", (int)(ULONG_PTR)hwnd, 0, 0, 0 );
                NtUserRedrawWindow( hwnd, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                                   RDW_ALLCHILDREN | RDW_UPDATENOW );
                wine_nx_surface_mark_full_dirty( surface );
                wine_nx_surface_present_full( surface );
            }
        }
        else window_surface_flush( surface );
        wine_nx_fb_present();
    }
}
