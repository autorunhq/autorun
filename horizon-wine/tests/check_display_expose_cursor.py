#!/usr/bin/env python3
"""Exercise child paint expose on set_window_pos (0090), cursor clip clamping
(0100/0101), and emulated-mode display mapping & OpenGL letterboxing."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()
input_c = (root / 'dlls/win32u/input.c').read_text()
opengl_c = (root / 'dlls/win32u/opengl.c').read_text()
sysparams_c = (root / 'dlls/win32u/sysparams.c').read_text()
winnx_c = (root / 'dlls/win32u/winnx_drv.c').read_text()


def extract_function(source, name):
    match = re.search(rf'^static [^\n;]*\b{re.escape(name)}\([^;]*?\)\n\{{', source, re.M)
    if match is None:
        raise RuntimeError(f'function not found: {name}')
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


mon_start = horizon.index('struct horizon_monitor_info')
mon_end = horizon.index('static int horizon_server_handle_enum_winstation')
mon_block = horizon[mon_start:mon_end]

funcs = '\n\n'.join([
    extract_function(horizon, 'horizon_server_shallow_window_from_point_locked'),
    extract_function(horizon, 'horizon_server_handle_set_window_pos'),
    extract_function(horizon, 'horizon_server_handle_set_cursor'),
])
shown_rect_fn = extract_function(winnx_c, 'wine_nx_shown_rect')

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HORIZON_STATUS_SUCCESS 0u
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_STATUS_INVALID_PARAMETER 0xc000000du
#define HORIZON_SERVER_OBJECT_WINSTATION 10
#define HORIZON_WS_VISIBLE 0x10000000u
#define HORIZON_WS_CHILD   0x40000000u
#define HORIZON_WS_EX_LAYERED 0x00080000u
#define HORIZON_WS_EX_TRANSPARENT 0x00000020u
#define HORIZON_SWP_NOSIZE     0x0001u
#define HORIZON_SWP_NOMOVE     0x0002u
#define HORIZON_SWP_NOZORDER   0x0004u
#define HORIZON_SWP_NOREDRAW   0x0008u
#define HORIZON_SWP_SHOWWINDOW 0x0040u
#define HORIZON_SWP_HIDEWINDOW 0x0080u
#define HORIZON_RDW_ALLCHILDREN 0x0080u
#define HORIZON_SET_WINPOS_PAINT_SURFACE 0x0001u
#define HORIZON_SET_CURSOR_HANDLE 0x01u
#define HORIZON_SET_CURSOR_COUNT  0x02u
#define HORIZON_SET_CURSOR_POS    0x04u
#define HORIZON_SET_CURSOR_CLIP   0x08u
#define HORIZON_SET_CURSOR_NOCLIP 0x10u

struct horizon_server_request_header { int req; unsigned int request_size, reply_size; };
struct horizon_server_reply_header { unsigned int error, reply_size; };
struct horizon_obj_locator { unsigned long long id, offset; };
struct horizon_rectangle { int left, top, right, bottom; };
struct horizon_shared_cursor { int x, y; unsigned int last_change; struct horizon_rectangle clip; };
struct horizon_desktop_shm { unsigned int flags; struct horizon_shared_cursor cursor; unsigned char keystate[256]; unsigned long long monitor_serial, keystate_serial; };
struct horizon_shared_object { long long seq; unsigned long long id; union { struct horizon_desktop_shm desktop; } shm; };
struct horizon_input_shm { int foreground; unsigned int active, focus, capture, menu_owner, move_size, caret; struct horizon_rectangle caret_rect; unsigned int cursor; int cursor_count; unsigned char keystate[256]; int keystate_lock; unsigned long long keystate_serial; };
struct horizon_server_object { unsigned int handle, count; int type; };
struct horizon_server_connection { int reply_fd; unsigned int pid, tid; };
struct horizon_user_window {
    unsigned int handle, parent, tid, style, ex_style, paint_flags;
    unsigned int has_update_rect, needs_erase, needs_nonclient, is_layered;
    struct horizon_rectangle window_rect, client_rect, visible_rect, surface_rect, update_rect;
    unsigned int win_region_count;
    struct horizon_user_window *next;
};

struct horizon_set_winstation_monitors_request { struct horizon_server_request_header header; int increment; char __pad[4]; };
struct horizon_set_winstation_monitors_reply { struct horizon_server_reply_header header; unsigned int serial; char __pad[4]; };
struct horizon_set_window_pos_request {
    struct horizon_server_request_header header;
    unsigned short swp_flags, paint_flags; unsigned int handle, previous;
    struct horizon_rectangle window, client;
};
struct horizon_set_window_pos_reply { struct horizon_server_reply_header header; unsigned int new_style, new_ex_style, surface_win; char __pad_20[4]; };
struct horizon_set_cursor_request {
    struct horizon_server_request_header header;
    unsigned int flags, handle; int show_count, x, y; struct horizon_rectangle clip;
};
struct horizon_set_cursor_reply {
    struct horizon_server_reply_header header;
    unsigned int prev_handle; int prev_count, prev_x, prev_y, new_x, new_y;
    struct horizon_rectangle new_clip; unsigned int last_change; char __pad_52[4];
};

static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int horizon_process_winstation = 0x10;
static struct horizon_server_object mock_winstation = { 0x10, 1, HORIZON_SERVER_OBJECT_WINSTATION };
static struct horizon_user_window *horizon_windows;
static struct horizon_desktop_shm mock_desktop;
static struct horizon_input_shm mock_input;
static unsigned int mock_input_time = 100;
static unsigned char last_reply[256];
static unsigned int redraw_children_calls;
static unsigned int redraw_children_flags;
static struct horizon_rectangle redraw_children_rect;

static void horizon_trace( const char *fmt, ... ) { (void)fmt; }
static unsigned int horizon_server_input_time( void ) { return ++mock_input_time; }
static struct horizon_desktop_shm *horizon_server_desktop_shared_locked( struct horizon_obj_locator *loc )
{
    if (loc) { loc->id = 1; loc->offset = 0; }
    return &mock_desktop;
}
static struct horizon_input_shm *horizon_server_input_shared_locked( unsigned int tid ) { (void)tid; return &mock_input; }
static void horizon_server_flush_input_locked( struct horizon_input_shm *in ) { (void)in; }
static void horizon_server_flush_session_range_locked( unsigned long long off, unsigned long long sz ) { (void)off; (void)sz; }

static struct horizon_server_object *horizon_server_find_handle_object_locked( unsigned int h, int type )
{
    return (h == mock_winstation.handle && type == mock_winstation.type) ? &mock_winstation : NULL;
}

static struct horizon_user_window *horizon_server_find_window_locked( unsigned int h )
{
    struct horizon_user_window *w;
    for (w = horizon_windows; w; w = w->next) if (w->handle == h) return w;
    return NULL;
}

static void horizon_server_link_window_locked( struct horizon_user_window *w, unsigned int prev )
{
    (void)w; (void)prev;
}

static int horizon_server_rect_equal( const struct horizon_rectangle *a, const struct horizon_rectangle *b )
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static int horizon_server_rect_empty( const struct horizon_rectangle *r )
{
    return r->left >= r->right || r->top >= r->bottom;
}

static int horizon_server_point_in_rect( const struct horizon_rectangle *r, int x, int y )
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

static unsigned int horizon_server_window_depth_locked( const struct horizon_user_window *w )
{
    unsigned int depth = 0;
    while (w && w->parent) { depth++; w = horizon_server_find_window_locked( w->parent ); }
    return depth;
}

static void horizon_server_window_screen_rects_locked( const struct horizon_user_window *w,
                                                       struct horizon_rectangle *win,
                                                       struct horizon_rectangle *client )
{
    *win = w->window_rect;
    *client = w->client_rect;
}

static void horizon_server_mark_window_update_locked( struct horizon_user_window *w, struct horizon_rectangle r, int erase )
{
    w->has_update_rect = 1;
    w->update_rect = r;
    w->needs_erase |= (unsigned int)erase;
}

static void horizon_server_redraw_children_locked( struct horizon_user_window *parent,
                                                   struct horizon_rectangle dirty, unsigned int flags )
{
    (void)parent;
    redraw_children_calls++;
    redraw_children_flags = flags;
    redraw_children_rect = dirty;
}

static int horizon_server_window_hit_locked( const struct horizon_user_window *w, int x, int y )
{
    return (w->style & HORIZON_WS_VISIBLE) && x >= w->window_rect.left && x < w->window_rect.right &&
           y >= w->window_rect.top && y < w->window_rect.bottom;
}

static int horizon_server_window_point_in_client_locked( const struct horizon_user_window *w, int x, int y )
{
    return x >= w->client_rect.left && x < w->client_rect.right &&
           y >= w->client_rect.top && y < w->client_rect.bottom;
}

static int horizon_server_write_reply( int fd, const void *rep, size_t sz, const void *d, size_t dsz )
{
    (void)fd; (void)d; (void)dsz;
    assert( sz <= sizeof(last_reply) );
    memcpy( last_reply, rep, sz );
    return 0;
}

typedef struct { int left, top, right, bottom; } RECT;
struct window_rects { RECT window, client, visible; };
static int intersect_rect( RECT *dst, const RECT *a, const RECT *b )
{
    dst->left = a->left > b->left ? a->left : b->left;
    dst->top = a->top > b->top ? a->top : b->top;
    dst->right = a->right < b->right ? a->right : b->right;
    dst->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    return dst->left < dst->right && dst->top < dst->bottom;
}

''' + mon_block + '\n\n' + funcs + '\n\n' + shown_rect_fn + r'''

int main( void )
{
    struct horizon_server_connection conn = { 1, 1, 1 };
    struct horizon_monitor_info mon = { { 280, 0, 1000, 720 }, { 0, 0, 640, 480 }, 0, 96, 96, 96, 96 };
    struct horizon_set_winstation_monitors_request mon_req;
    const struct horizon_set_winstation_monitors_reply *mon_rep = (const void *)last_reply;
    struct horizon_user_window desktop = {0}, wnd = {0};
    struct horizon_set_window_pos_request swp_req;
    struct horizon_set_cursor_request cur_req;
    const struct horizon_set_cursor_reply *cur_rep = (const void *)last_reply;

    /* 1. Monitor cache & raw->virt point mapping (emulated-modes-fit-the-screen). */
    memset( &mon_req, 0, sizeof(mon_req) );
    mon_req.increment = 1;
    horizon_server_handle_set_winstation_monitors( &conn, (const void *)&mon_req, &mon, sizeof(mon) );
    assert( !mon_rep->header.error && mon_rep->serial == 2 && horizon_monitor_count == 1 );

    desktop.handle = 0x1000;
    desktop.style = HORIZON_WS_VISIBLE;
    desktop.window_rect = desktop.client_rect = (struct horizon_rectangle){ 0, 0, 640, 480 };
    desktop.next = &wnd;

    wnd.handle = 0x1001;
    wnd.parent = desktop.handle;
    wnd.style = HORIZON_WS_VISIBLE;
    wnd.window_rect = (struct horizon_rectangle){ 0, 0, 640, 480 };
    wnd.client_rect = wnd.window_rect;
    horizon_windows = &desktop;

    /* Center of raw 1280x720 (640, 360) maps to center of virtual 640x480 (320, 240). */
    int px = 640, py = 360;
    horizon_server_map_point_raw_to_virt_locked( &px, &py );
    assert( px == 320 && py == 240 );
    /* Left/top edge inside letterboxed screen (280, 0) maps to (0, 0) and hits virtual window. */
    assert( horizon_server_shallow_window_from_point_locked( 280, 0 ) == &wnd );

    /* 2. set_window_pos showing hidden window and moving visible window exposes children (0090). */
    wnd.style = 0; /* initially hidden */
    memset( &swp_req, 0, sizeof(swp_req) );
    swp_req.handle = wnd.handle;
    swp_req.swp_flags = HORIZON_SWP_NOZORDER | HORIZON_SWP_SHOWWINDOW;
    swp_req.window = swp_req.client = (struct horizon_rectangle){ 10, 20, 310, 220 };
    redraw_children_calls = 0;
    horizon_server_handle_set_window_pos( &conn, (const void *)&swp_req, NULL, 0 );
    assert( (wnd.style & HORIZON_WS_VISIBLE) && wnd.has_update_rect );
    assert( redraw_children_calls == 1 && redraw_children_flags == HORIZON_RDW_ALLCHILDREN );

    /* With SWP_NOREDRAW set, no child expose is scheduled. */
    swp_req.swp_flags = HORIZON_SWP_NOZORDER | HORIZON_SWP_NOREDRAW;
    swp_req.window = swp_req.client = (struct horizon_rectangle){ 20, 30, 320, 230 };
    redraw_children_calls = 0;
    horizon_server_handle_set_window_pos( &conn, (const void *)&swp_req, NULL, 0 );
    assert( redraw_children_calls == 0 );

    /* Moving while already visible exposes children again. */
    swp_req.swp_flags = HORIZON_SWP_NOZORDER;
    swp_req.window = swp_req.client = (struct horizon_rectangle){ 30, 40, 330, 240 };
    horizon_server_handle_set_window_pos( &conn, (const void *)&swp_req, NULL, 0 );
    assert( redraw_children_calls == 1 );

    /* 3. set_cursor clamps out-of-bounds cursor inside clip rectangle (0101). */
    mock_desktop.cursor.x = 50;
    mock_desktop.cursor.y = 700;
    memset( &cur_req, 0, sizeof(cur_req) );
    cur_req.flags = HORIZON_SET_CURSOR_CLIP;
    cur_req.clip = (struct horizon_rectangle){ 280, 0, 1000, 720 };
    horizon_server_handle_set_cursor( &conn, (const void *)&cur_req );
    assert( cur_rep->new_x == 280 && cur_rep->new_y == 700 );

    mock_desktop.cursor.x = 1200;
    mock_desktop.cursor.y = 900;
    cur_req.flags = HORIZON_SET_CURSOR_NOCLIP;
    horizon_server_handle_set_cursor( &conn, (const void *)&cur_req );
    assert( cur_rep->new_x == 1199 || cur_rep->new_x == 1200 ); /* wait: clip is 0..1280 -> 1200 stays, y 900 -> 719! */
    assert( cur_rep->new_x == 1200 && cur_rep->new_y == 719 );

    /* 4. wine_nx_shown_rect keeps scaled fullscreen surface between side bars (0111). */
    struct window_rects scaled_rects = {
        .window = { 160, 0, 1120, 720 },
        .client = { 160, 0, 1120, 720 },
        .visible = { 0, 0, 1280, 720 },
    };
    RECT shown = wine_nx_shown_rect( &scaled_rects );
    assert( shown.left == 160 && shown.top == 0 && shown.right == 1120 && shown.bottom == 720 );

    puts( "Display mode raw->virt mapping, scaled surface bounds (0111), set_window_pos child expose (0090), and cursor clip clamp (0101) passed" );
    return 0;
}
'''

assert 'map_rect_virt_to_raw( get_virtual_screen_rect( dpi, MDT_DEFAULT ), dpi )' in input_c
assert '#ifdef __SWITCH__\nBOOL emulate_modeset = TRUE;' in sysparams_c
assert 'client_surface_screen_rect' in opengl_c and 'glClearColor( 0.0f, 0.0f, 0.0f, 1.0f )' in opengl_c
assert 'RECT shown = wine_nx_shown_rect( new_rects );' in winnx_c

cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'disp_test.c'
    exe = Path(tmp) / 'disp_test'
    src.write_text(fixture)
    subprocess.run([
        cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-variable',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
        str(src), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
