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
extern int   wine_nx_screen_wants_pictures( void ) __attribute__((weak));
extern void  wine_nx_screen_picture_drawn( DWORD thread, int redrawn ) __attribute__((weak));
extern int   wine_nx_layer_redrawn( struct wine_nx_layer *layer, const uint32_t *pixels, int stride,
                                    int left, int top, int right, int bottom ) __attribute__((weak));

/* Buttons reported by wine_nx_pointer_poll(). */
#define WINE_NX_POINTER_LEFT   0x1
#define WINE_NX_POINTER_RIGHT  0x2
#define WINE_NX_POINTER_MIDDLE 0x4

struct wine_nx_surface
{
    struct window_surface surface;
    BOOL initial_redraw_done;
    RECT present_rect;
    POINT screen_origin;
    SIZE dst_size;
    RECT clip_rect;
    BOOL has_clip;
    struct wine_nx_layer *layer;  /* the compositor's copy of the pixels, when it presents */
    RECT client_rect;
    LONG drawn;
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

/* Moves the drawn cursor while the program is too busy to pump messages, such
 * as OpenTTD loading its sprites. This thread has no TEB, so it must not call
 * into Wine: the first server call dereferences a NULL TEB. It only polls the
 * controller and presents; ProcessEvents sends the input from a Wine thread. */
static void *wine_nx_input_thread( void *arg )
{
    (void)arg;
    while (!__atomic_load_n( &wine_nx_input_thread_quit, __ATOMIC_ACQUIRE ))
    {
        unsigned int buttons;
        int x, y;

        wine_nx_pointer_poll( &x, &y, &buttons );
        wine_nx_fb_present();
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

static void nxdrv_say( const char *line )
{
    if (&wine_nx_runtime_trace) wine_nx_runtime_trace( line );
}

extern unsigned int wine_nx_keys_taken;

static pthread_mutex_t nxdrv_key_watch_lock = PTHREAD_MUTEX_INITIALIZER;

static struct
{
    BOOL pending;
    DWORD tick;
    unsigned int taken;
    WORD vk;
    BOOL up;
    unsigned int reported;
} nxdrv_key_watch;

static void nxdrv_note_key_sent( WORD vk, BOOL up )
{
    pthread_mutex_lock( &nxdrv_key_watch_lock );
    if (!nxdrv_key_watch.pending && nxdrv_key_watch.reported < 24)
    {
        nxdrv_key_watch.pending = TRUE;
        nxdrv_key_watch.tick = NtGetTickCount();
        nxdrv_key_watch.taken = __atomic_load_n( &wine_nx_keys_taken, __ATOMIC_RELAXED );
        nxdrv_key_watch.vk = vk;
        nxdrv_key_watch.up = up;
    }
    pthread_mutex_unlock( &nxdrv_key_watch_lock );
}

static void nxdrv_check_key_taken( void )
{
    unsigned int taken;
    DWORD elapsed;
    WORD vk;
    BOOL up, report = FALSE;
    char line[160];

    pthread_mutex_lock( &nxdrv_key_watch_lock );
    if (nxdrv_key_watch.pending)
    {
        elapsed = NtGetTickCount() - nxdrv_key_watch.tick;
        taken = __atomic_load_n( &wine_nx_keys_taken, __ATOMIC_RELAXED );
        if (taken != nxdrv_key_watch.taken)
        {
            nxdrv_key_watch.pending = FALSE;
        }
        else if (elapsed >= 400)
        {
            nxdrv_key_watch.pending = FALSE;
            nxdrv_key_watch.reported++;
            vk = nxdrv_key_watch.vk;
            up = nxdrv_key_watch.up;
            report = TRUE;
        }
    }
    pthread_mutex_unlock( &nxdrv_key_watch_lock );
    if (!report) return;
    snprintf( line, sizeof(line), "[KEY] the key %x %s sent %u ms ago was not taken from the queue yet",
              vk, up ? "up" : "down", (unsigned int)elapsed );
    nxdrv_say( line );
}

static void nxdrv_trace_key_target( void )
{
    static HWND last_focus, last_active, last_fg;
    static unsigned int told;
    GUITHREADINFO info = { .cbSize = sizeof(info) };
    HWND fg;
    char line[160];

    if (told >= 24) return;
    NtUserGetGUIThreadInfo( GetCurrentThreadId(), &info );
    fg = NtUserGetForegroundWindow();
    if (info.hwndFocus == last_focus && info.hwndActive == last_active && fg == last_fg) return;
    last_focus = info.hwndFocus;
    last_active = info.hwndActive;
    last_fg = fg;
    told++;
    snprintf( line, sizeof(line), "[KEY] target focus=%x active=%x fg=%x tid=%u",
              (int)(ULONG_PTR)info.hwndFocus, (int)(ULONG_PTR)info.hwndActive,
              (int)(ULONG_PTR)fg, (unsigned int)GetCurrentThreadId() );
    nxdrv_say( line );
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

void wine_nx_driver_surface_drawn( struct window_surface *surface )
{
    if (surface && surface->funcs == &wine_nx_surface_funcs)
        InterlockedExchange( &wine_nx_surface_from_base( surface )->drawn, 1 );
}

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

#define WINE_NX_SCREEN_W 1280
#define WINE_NX_SCREEN_H 720

/* With the compositor, each window shows through the layer of its current
 * surface. A surface forgets its layer here before destroying it, so a layer
 * found under the entries mutex stays alive while the mutex is held. */
static void wine_nx_show_window_layer( HWND hwnd, struct wine_nx_layer *layer, const POINT *origin,
                                       int dst_w, int dst_h, const RECT *present )
{
    struct wine_nx_surface_entry *entry;
    static POINT last_origin;
    static SIZE last_dst;
    static RECT last_present;
    static unsigned int told;
    int src_w = present->right - present->left;
    int src_h = present->bottom - present->top;

    if (told < 24 && (origin->x != last_origin.x || origin->y != last_origin.y ||
                      dst_w != last_dst.cx || dst_h != last_dst.cy ||
                      src_w != last_present.right - last_present.left ||
                      src_h != last_present.bottom - last_present.top))
    {
        char line[160];

        last_origin = *origin;
        last_dst.cx = dst_w;
        last_dst.cy = dst_h;
        last_present = *present;
        told++;
        snprintf( line, sizeof(line), "[NXCOMP] hwnd %x picture at %d,%d %dx%d from %dx%d of the %dx%d screen",
                  (int)(ULONG_PTR)hwnd, (int)origin->x, (int)origin->y, dst_w, dst_h,
                  src_w, src_h, WINE_NX_SCREEN_W, WINE_NX_SCREEN_H );
        nxdrv_say( line );
    }

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    if ((entry = find_surface_entry_locked( hwnd, TRUE )))
    {
        /* A resized window gets a new surface; the old one can outlive this. */
        if (entry->layer && entry->layer != layer) wine_nx_layer_place( entry->layer, 0, 0, 0, 0 );
        entry->layer = layer;
    }
    wine_nx_layer_place_scaled( layer, origin->x, origin->y, dst_w, dst_h,
                                present->left, present->top, src_w, src_h );
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

static void wine_nx_note_surface_present( HWND hwnd, const POINT *origin, int dst_w, int dst_h )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, TRUE );

    if (!entry) return;
    entry->screen_rect.left = origin->x;
    entry->screen_rect.top = origin->y;
    entry->screen_rect.right = origin->x + dst_w;
    entry->screen_rect.bottom = origin->y + dst_h;
    entry->visible = dst_w > 0 && dst_h > 0;
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

static BOOL wine_nx_client_lit( struct wine_nx_surface *nx_surface, const RECT *dirty,
                                const void *color_bits, int stride, int sh )
{
    const DWORD *bits = color_bits;
    RECT area, bounds = { 0, 0, stride, sh };
    int x, y;

    if (!bits || stride <= 0 || sh <= 0) return FALSE;
    if (!intersect_rect( &area, dirty, &nx_surface->client_rect )) return FALSE;
    if (!intersect_rect( &area, &area, &nx_surface->present_rect )) return FALSE;
    if (!intersect_rect( &area, &area, &bounds )) return FALSE;

    for (y = area.top; y < area.bottom; y++)
        for (x = area.left; x < area.right; x++)
            if (bits[(size_t)y * stride + x] & 0xffffff) return TRUE;
    return FALSE;
}

/* Blit the dirty region of the (BGRX, top-down) DIB to the linear RGBA8888
 * framebuffer at the surface's screen position, clipped to the screen. */
static BOOL wine_nx_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( surface->hwnd, FALSE );
    BOOL drawn = InterlockedExchange( &nx_surface->drawn, 0 );
    RECT blit = *dirty;
    int sw = color_info->bmiHeader.biWidth;
    int sh = abs( color_info->bmiHeader.biHeight );
    int fbw = 0, fbh = 0, fbstride = 0;
    DWORD *fb;
    int sy, sx;

    (void)rect;

    if (drawn && entry && entry->visible && wine_nx_screen_wants_pictures && wine_nx_screen_wants_pictures() &&
        wine_nx_screen_picture_drawn && wine_nx_client_lit( nx_surface, dirty, color_bits, sw, sh ))
        wine_nx_screen_picture_drawn( NtUserGetWindowThread( surface->hwnd, NULL ),
                                      wine_nx_layer_redrawn ? wine_nx_layer_redrawn( nx_surface->layer, color_bits, sw,
                                                                                     dirty->left, dirty->top,
                                                                                     dirty->right, dirty->bottom ) : 0 );

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

    {
        int pw = nx_surface->present_rect.right - nx_surface->present_rect.left;
        int ph = nx_surface->present_rect.bottom - nx_surface->present_rect.top;
        int dw = nx_surface->dst_size.cx > 0 ? nx_surface->dst_size.cx : pw;
        int dh = nx_surface->dst_size.cy > 0 ? nx_surface->dst_size.cy : ph;

        if (pw > 0 && ph > 0 && dw > 0 && dh > 0)
        {
            if (dw == pw && dh == ph)
            {
                for (sy = blit.top; sy < blit.bottom; sy++)
                {
                    int v = sy;
                    int dy = nx_surface->screen_origin.y + (sy - nx_surface->present_rect.top);
                    const DWORD *src;
                    DWORD *dst;

                    if (v < 0 || v >= sh || dy < 0 || dy >= fbh) continue;
                    src = (const DWORD *)color_bits + (size_t)v * sw;
                    dst = fb + (size_t)dy * fbstride;
                    for (sx = blit.left; sx < blit.right; sx++)
                    {
                        int u = sx;
                        int dx = nx_surface->screen_origin.x + (sx - nx_surface->present_rect.left);
                        DWORD p;
                        if (u < 0 || u >= sw || dx < 0 || dx >= fbw) continue;
                        p = src[u];
                        dst[dx] = 0xff000000u | (p & 0x0000ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
                    }
                }
            }
            else
            {
                int ox = nx_surface->screen_origin.x;
                int oy = nx_surface->screen_origin.y;
                int x0 = ox + (blit.left - nx_surface->present_rect.left) * dw / pw;
                int y0 = oy + (blit.top - nx_surface->present_rect.top) * dh / ph;
                int x1 = ox + ((blit.right - nx_surface->present_rect.left) * dw + pw - 1) / pw;
                int y1 = oy + ((blit.bottom - nx_surface->present_rect.top) * dh + ph - 1) / ph;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > fbw) x1 = fbw;
                if (y1 > fbh) y1 = fbh;
                for (int dy = y0; dy < y1; dy++)
                {
                    int v = nx_surface->present_rect.top + (dy - oy) * ph / dh;
                    const DWORD *src;
                    DWORD *dst;
                    if (v < 0 || v >= sh) continue;
                    src = (const DWORD *)color_bits + (size_t)v * sw;
                    dst = fb + (size_t)dy * fbstride;
                    for (int dx = x0; dx < x1; dx++)
                    {
                        int u = nx_surface->present_rect.left + (dx - ox) * pw / dw;
                        DWORD p;
                        if (u < 0 || u >= sw) continue;
                        p = src[u];
                        dst[dx] = 0xff000000u | (p & 0x0000ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
                    }
                }
            }
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

static LONG wine_nx_fit_pending;
static DWORD wine_nx_fit_thread;

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
static void wine_nx_fit_screen_to_game( void );

/* The button flags that take the delivered buttons (last) to the held ones,
 * in two steps: a button pressed and released since the last delivery clicks
 * (down, then up), and one released and pressed again goes up, then down. */
static void wine_nx_pointer_flags( unsigned int last, unsigned int held, unsigned int pressed,
                                   unsigned int released, DWORD *first, DWORD *second )
{
    static const struct { unsigned int button; DWORD down, up; } map[] =
    {
        { WINE_NX_POINTER_LEFT,   MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP },
        { WINE_NX_POINTER_RIGHT,  MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP },
        { WINE_NX_POINTER_MIDDLE, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
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
}

/* The floating keyboard (horizon-wine/source/osk.c) queues the keys pressed
 * on it, each due at its time; they go to whichever window has focus, as the
 * controller's do. */
extern int wine_nx_osk_next_key( unsigned long long now_ns, unsigned short *vk, int *up ) __attribute__((weak));
extern unsigned long long wine_nx_osk_clock( void ) __attribute__((weak));
extern void wine_nx_keyboard_open( void ) __attribute__((weak));

static BOOL wine_nx_send_keyboard_keys( void )
{
    static unsigned int osk_logged;
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
        wine_nx_send_vk( layout, vk, up );
        nxdrv_note_key_sent( vk, up );
        if (osk_logged++ < 40)
        {
            BYTE state[256] = {0};
            char line[96];

            get_async_keyboard_state( state );
            snprintf( line, sizeof(line), "[OSK] sent vk=%02x %s -> async=%02x",
                      vk, up ? "up" : "down", state[vk & 0xff] );
            nxdrv_say( line );
        }
        sent = TRUE;
        /* One tap per frame at a time: a program polling GetAsyncKeyState gets
         * to see the key down before the release clears it. */
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
    BOOL moved, keys, placed, stepped;
    int x, y, dx, dy;

    (void)mask;
    wine_nx_fit_screen_to_game();
    if (__atomic_exchange_n( &delivering, 1, __ATOMIC_ACQUIRE )) return FALSE;
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
    if (placed || first) wine_nx_send_mouse( x, y, (placed ? MOUSEEVENTF_MOVE : 0) | first );
    if (stepped) wine_nx_send_mouse_motion( dx, dy );
    if (second) wine_nx_send_mouse( x, y, second );
    if (placed || first || stepped || second) wine_nx_flush_mouse();
    wine_nx_send_extra_mouse( last_buttons, buttons, pressed, released, wheel, hwheel );
    for (unsigned int i = key_after; i < key_count; i++) wine_nx_send_key( key_events + i );
    if (moved || placed || stepped)
    {
        POINT pos;

        /* The arrow belongs where the cursor is, which after movement is for
         * the server to say: it clips the cursor to the screen, and holds it
         * still for a program that has taken the mouse for itself. Nothing is
         * lost by snapping to it, the movement having been sent already. */
        if (wine_nx_cursor_pos( &pos ) && (pos.x != x || pos.y != y))
        {
            static unsigned int followed;

            /* Quiet once it is plainly working, loud enough to see that it is. */
            if (followed++ < 8 && &wine_nx_runtime_trace)
            {
                char line[128];

                snprintf( line, sizeof(line), "[NXINPUT] the cursor is at %d,%d, the stick moved %d,%d",
                          pos.x, pos.y, dx, dy );
                wine_nx_runtime_trace( line );
            }
            wine_nx_pointer_follow( pos.x, pos.y );
        }
    }
    if (first) nxdrv_trace( "[NXINPUT] buttons=%x flags=%x,%x x=%d", buttons, first, second, x );
    else if (moved) nxdrv_trace_hot( "[NXINPUT] move x=%d y=%d buttons=%x", x, y, buttons, 0 );
    last_buttons = buttons;
    keys = key_count || wheel || hwheel || ((pressed | released) & 0x1c);
    keys |= wine_nx_send_keyboard_keys();
    nxdrv_check_key_taken();
    wine_nx_update_cursor();
    wine_nx_fb_present();
    __atomic_store_n( &delivering, 0, __ATOMIC_RELEASE );
    return moved || stepped || first || keys;
}

/**********************************************************************
 *           wine_nx_drv_GetCursorPos
 *
 * Where the pointer is, for GetCursorPos(). win32u asks the driver only when
 * the server's answer is older than 100 ms, and the null driver's answer is
 * "no": a program that draws its own cursor read a position that stopped
 * moving, or a call that failed, whenever the pointer stood still.
 */
BOOL wine_nx_drv_GetCursorPos( POINT *pos )
{
    return wine_nx_cursor_pos( pos );
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
 *           wine_nx_drv_SetCursorPos
 */
BOOL wine_nx_drv_SetCursorPos( INT x, INT y )
{
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

#define WINE_NX_GAME_WINDOW_MIN_W 640
#define WINE_NX_GAME_WINDOW_MIN_H 400

extern LONG wine_nx_fit_screen( UINT width, UINT height );
extern __thread int wine_nx_reply_waits;

static BOOL wine_nx_game_window( HWND hwnd, RECT *client )
{
    struct ratio dpi = get_thread_dpi();
    DWORD style = get_window_long( hwnd, GWL_STYLE );
    DWORD pid = 0;

    if (!(style & WS_VISIBLE) || (style & (WS_CHILD | WS_MINIMIZE))) return FALSE;
    if (get_window_long( hwnd, GWL_EXSTYLE ) & WS_EX_TOOLWINDOW) return FALSE;
    if (get_window_relative( hwnd, GW_OWNER )) return FALSE;
    if (!NtUserGetWindowThread( hwnd, &pid ) || pid != GetCurrentProcessId()) return FALSE;
    if (!get_client_rect( hwnd, client, dpi )) return FALSE;
    map_window_points( hwnd, 0, (POINT *)client, 2, dpi );
    return client->right - client->left >= WINE_NX_GAME_WINDOW_MIN_W &&
           client->bottom - client->top >= WINE_NX_GAME_WINDOW_MIN_H;
}

static void wine_nx_log_window( HWND hwnd, BOOL chosen )
{
    static unsigned int logged;
    struct ratio dpi = get_thread_dpi();
    RECT client = {0};
    DWORD pid = 0;
    char line[200];

    if (!&wine_nx_runtime_trace || logged >= 40) return;
    logged++;
    NtUserGetWindowThread( hwnd, &pid );
    if (get_client_rect( hwnd, &client, dpi )) map_window_points( hwnd, 0, (POINT *)&client, 2, dpi );
    snprintf( line, sizeof(line), "[NXSCREEN] window %p style %08x exstyle %08x owner %p pid %u client %d,%d %dx%d: %s",
              hwnd, (unsigned int)get_window_long( hwnd, GWL_STYLE ), (unsigned int)get_window_long( hwnd, GWL_EXSTYLE ),
              get_window_relative( hwnd, GW_OWNER ), (unsigned int)pid, (int)client.left, (int)client.top,
              (int)(client.right - client.left), (int)(client.bottom - client.top), chosen ? "a candidate" : "skipped" );
    wine_nx_runtime_trace( line );
}

static HWND wine_nx_find_game_window( RECT *client )
{
    HWND *list, game = 0;
    RECT rect;
    LONGLONG area, best = 0;
    unsigned int i;

    if (!(list = list_window_children( NtUserGetDesktopWindow() ))) return 0;
    for (i = 0; list[i]; i++)
    {
        BOOL candidate = wine_nx_game_window( list[i], &rect );

        if (get_window_long( list[i], GWL_STYLE ) & WS_VISIBLE) wine_nx_log_window( list[i], candidate );
        if (!candidate) continue;
        area = (LONGLONG)(rect.right - rect.left) * (rect.bottom - rect.top);
        if (area <= best) continue;
        best = area;
        game = list[i];
        *client = rect;
    }
    free( list );
    return game;
}

static void wine_nx_fit_screen_to_game( void )
{
    struct ratio dpi = get_thread_dpi();
    int width, height, screen_width, screen_height;
    RECT client, window;
    HWND game;
    UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    LONG ret;
    char line[160];

    if (__atomic_load_n( &wine_nx_fit_thread, __ATOMIC_ACQUIRE ) != GetCurrentThreadId()) return;
    if (wine_nx_reply_waits) return;
    if (!InterlockedExchange( &wine_nx_fit_pending, 0 )) return;
    if (!(game = wine_nx_find_game_window( &client ))) return;
    if (NtUserGetWindowThread( game, NULL ) != GetCurrentThreadId())
    {
        __atomic_store_n( &wine_nx_fit_thread, NtUserGetWindowThread( game, NULL ), __ATOMIC_RELEASE );
        InterlockedExchange( &wine_nx_fit_pending, 1 );
        return;
    }
    width = client.right - client.left;
    height = client.bottom - client.top;
    screen_width = get_system_metrics( SM_CXSCREEN );
    screen_height = get_system_metrics( SM_CYSCREEN );
    if (width != screen_width || height != screen_height)
    {
        ret = wine_nx_fit_screen( width, height );
        if (&wine_nx_runtime_trace)
        {
            snprintf( line, sizeof(line), "[NXSCREEN] the screen follows the game's window: %dx%d, was %dx%d, result %d",
                      width, height, screen_width, screen_height, (int)ret );
            wine_nx_runtime_trace( line );
        }
        if (ret != DISP_CHANGE_SUCCESSFUL || !wine_nx_game_window( game, &client )) return;
        flags |= SWP_FRAMECHANGED;
    }
    if (!client.left && !client.top) flags |= SWP_NOMOVE;
    if ((flags & SWP_NOMOVE) && !(flags & SWP_FRAMECHANGED)) return;
    if (!get_window_rect( game, &window, dpi )) return;
    NtUserSetWindowPos( game, 0, window.left - client.left, window.top - client.top, 0, 0, flags );
    NtUserRedrawWindow( game, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
    if (!(flags & SWP_NOMOVE) && &wine_nx_runtime_trace)
    {
        snprintf( line, sizeof(line), "[NXSCREEN] the game's window moved by %d,%d to the screen's corner",
                  (int)-client.left, (int)-client.top );
        wine_nx_runtime_trace( line );
    }
}

static RECT wine_nx_shown_rect( const struct window_rects *rects )
{
    RECT shown;

    if (!intersect_rect( &shown, &rects->visible, &rects->window )) shown = rects->visible;
    return shown;
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
    if (NtUserGetAncestor( hwnd, GA_PARENT ) == NtUserGetDesktopWindow())
    {
        __atomic_store_n( &wine_nx_fit_thread, NtUserGetWindowThread( hwnd, NULL ), __ATOMIC_RELEASE );
        InterlockedExchange( &wine_nx_fit_pending, 1 );
    }
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
            RECT shown = wine_nx_shown_rect( new_rects );
            int new_width = shown.right - shown.left;
            int new_height = shown.bottom - shown.top;
            struct ratio dpi = get_dpi_for_window( hwnd ), raw_dpi;
            RECT src_rect;

            if (!nx_surface->layer && nx_surface->initial_redraw_done &&
                (nx_surface->screen_origin.x != shown.left ||
                 nx_surface->screen_origin.y != shown.top ||
                 nx_surface->dst_size.cx != new_width ||
                 nx_surface->dst_size.cy != new_height))
                wine_nx_restore_popup_owner( hwnd, owner_hint,
                                             has_old_screen_rect ? &old_screen_rect : NULL );

            get_win_monitor_dpi( hwnd, &raw_dpi );
            if (dpi.num && raw_dpi.num)
            {
                RECT vis_virt = map_rect_raw_to_virt( new_rects->visible, dpi );
                RECT shown_virt = map_rect_raw_to_virt( shown, dpi );
                OffsetRect( &shown_virt, -vis_virt.left, -vis_virt.top );
                src_rect = map_dpi_rect( shown_virt, dpi, raw_dpi );
            }
            else
            {
                SetRect( &src_rect, 0, 0, new_width, new_height );
            }
            intersect_rect( &src_rect, &src_rect, &allocation );

            initial_redraw = !nx_surface->initial_redraw_done;
            nx_surface->initial_redraw_done = TRUE;
            nx_surface->screen_origin.x = shown.left;
            nx_surface->screen_origin.y = shown.top;
            nx_surface->dst_size.cx = new_width;
            nx_surface->dst_size.cy = new_height;
            nx_surface->present_rect = src_rect;
            if (dpi.num && raw_dpi.num)
            {
                RECT shown_virt = map_rect_raw_to_virt( shown, dpi );
                RECT client_virt = map_rect_raw_to_virt( new_rects->client, dpi );
                OffsetRect( &client_virt, -shown_virt.left, -shown_virt.top );
                nx_surface->client_rect = map_dpi_rect( client_virt, dpi, raw_dpi );
            }
            else
            {
                nx_surface->client_rect = new_rects->client;
                OffsetRect( &nx_surface->client_rect, -shown.left, -shown.top );
            }
            wine_nx_note_surface_present( hwnd, &nx_surface->screen_origin, new_width, new_height );
            if (nx_surface->layer)
            {
                /* Only the visible part is drawn, never the rest of the allocation. */
                wine_nx_show_window_layer( hwnd, nx_surface->layer, &nx_surface->screen_origin,
                                           new_width, new_height, &nx_surface->present_rect );
                wine_nx_restack_layers();
            }
            nxdrv_trace( "[NXDRV] present rect=%d,%d %dx%d",
                         nx_surface->screen_origin.x, nx_surface->screen_origin.y,
                         new_width, new_height );

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
