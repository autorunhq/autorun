from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
nx = (root / "dlls/win32u/winnx_opengl.c").read_text()
core = (root / "dlls/win32u/opengl.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;]*?\n\{", source, re.M)
    assert match, name
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


fixture = r'''
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define __SWITCH__ 1
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned int UINT;
typedef void *HWND, *HDC, *EGLNativeWindowType;
typedef int EGLint, EGLConfig;
typedef struct { int cx, cy; } SIZE, POINT;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct mock_surface { int window, width, height; } *EGLSurface;
#define TRUE 1
#define FALSE 0
#define GWL_STYLE 0
#define WS_VISIBLE 1
#define EGL_NONE 0
#define EGL_WIDTH 1
#define EGL_HEIGHT 2
#define EGL_VENDOR 3
#define EGL_VERSION 4
#define GL_FRONT_LEFT 0x400
#define GL_FRONT 0x404
#define GL_BACK 0x405
#define GL_BACK_LEFT 0x402
#define GL_BACK_RIGHT 0x403
#define GL_FRONT_AND_BACK 0x408
#define GL_FLUSH_INTERVAL 2
#define WND_DESKTOP ((WND *)1)
#define WND_OTHER_PROCESS ((WND *)2)
#define CONTAINING_RECORD(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
static LONG InterlockedExchange(LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static LONG InterlockedIncrement(LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG InterlockedCompareExchange(LONG *p, LONG v, LONG expected)
{ __atomic_compare_exchange_n(p, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return expected; }
struct client_surface;
struct client_surface_funcs {
    UINT size;
    void (*destroy)(struct client_surface *);
    void (*detach)(struct client_surface *);
    void (*update)(struct client_surface *);
    void (*present)(struct client_surface *, HDC);
};
struct client_surface {
    const struct client_surface_funcs *funcs;
    LONG ref;
    HWND hwnd, toplevel;
    int format;
    LONG offscreen;
    RECT virtual_rect, monitor_rect;
    BOOL raw;
};
struct opengl_drawable;
struct opengl_drawable_funcs {
    UINT size;
    void (*destroy)(struct opengl_drawable *);
    void (*flush)(struct opengl_drawable *, UINT);
    BOOL (*swap)(struct opengl_drawable *);
};
struct opengl_drawable {
    const struct opengl_drawable_funcs *funcs;
    LONG ref;
    struct client_surface *client;
    HDC owner_hdc;
    int format, interval;
    EGLSurface surface;
    SIZE virtual_size, monitor_size;
    int buffer_map[64];
};
typedef struct mock_window {
    BOOL visible;
    struct mock_window *parent;
    RECT rect;
    struct opengl_drawable *current_drawable, *unused_drawable, *dc_drawable;
} WND;
struct framebuffer_surface { struct opengl_drawable base; struct opengl_drawable *target; };
static const struct opengl_drawable_funcs framebuffer_surface_funcs = {0};
static struct framebuffer_surface *framebuffer_from_opengl_drawable(struct opengl_drawable *d)
{ return CONTAINING_RECORD(d, struct framebuffer_surface, base); }
static const struct opengl_drawable_funcs nx_drawable_funcs;
static struct client_surface *clients[64];
static unsigned int live_drawables, live_surfaces, acquires, releases, swaps, overlays, intervals, style_reads;
static BOOL screen_busy, fail_pbuffer, fail_window, fail_drawable, fail_client;
static int get_window_long(HWND hwnd, int offset) { style_reads++; return ((WND *)hwnd)->visible ? WS_VISIBLE : 0; }
static BOOL is_window_visible(HWND hwnd) {
    for (WND *win = hwnd; win; win = win->parent) if (!win->visible) return FALSE;
    return TRUE;
}
static struct client_surface *client_surface_create(const struct client_surface_funcs *f, HWND hwnd, int format, BOOL raw) {
    if (fail_client) return NULL;
    struct client_surface *c = calloc(1, f->size);
    c->funcs = f; c->ref = 1; c->hwnd = hwnd; c->toplevel = hwnd; c->format = format; c->raw = raw;
    while (((WND *)c->toplevel)->parent) c->toplevel = ((WND *)c->toplevel)->parent;
    c->virtual_rect = c->monitor_rect = ((WND *)hwnd)->rect;
    for (unsigned int i = 0; i < 64; i++) if (!clients[i]) { clients[i] = c; return c; }
    abort();
}
static void client_surface_release(struct client_surface *c) {
    if (--c->ref) return;
    c->funcs->destroy(c);
    for (unsigned int i = 0; i < 64; i++) if (clients[i] == c) clients[i] = NULL;
    free(c);
}
static void *opengl_drawable_create(const struct opengl_drawable_funcs *f, int format, struct client_surface *c, const SIZE *size) {
    if (fail_drawable) return NULL;
    struct opengl_drawable *d = calloc(1, f->size);
    d->funcs = f; d->ref = 1; d->format = format; d->client = c; c->ref++;
    d->virtual_size = (SIZE){ max(1, c->virtual_rect.right - c->virtual_rect.left), max(1, c->virtual_rect.bottom - c->virtual_rect.top) };
    d->monitor_size = (SIZE){ max(1, c->monitor_rect.right - c->monitor_rect.left), max(1, c->monitor_rect.bottom - c->monitor_rect.top) };
    live_drawables++;
    return d;
}
static void opengl_drawable_add_ref(struct opengl_drawable *d) { d->ref++; }
static void mock_destroy_surface(void *display, EGLSurface s) { live_surfaces--; free(s); }
static void opengl_drawable_release(struct opengl_drawable *d) {
    if (--d->ref) return;
    d->funcs->destroy(d);
    if (d->surface) mock_destroy_surface(NULL, d->surface);
    client_surface_release(d->client);
    live_drawables--; free(d);
}
static void *wine_nx_gl_acquire_window(void) { acquires++; if (screen_busy) return NULL; screen_busy = TRUE; return (void *)1; }
static void wine_nx_gl_release_window(void) { assert(screen_busy); screen_busy = FALSE; releases++; }
static void nx_log(const char *format, ...) { }
static EGLSurface mock_create_pbuffer(void *display, EGLConfig config, const EGLint *attributes) {
    if (fail_pbuffer) return NULL;
    assert(attributes[0] == EGL_WIDTH && attributes[2] == EGL_HEIGHT && attributes[4] == EGL_NONE);
    EGLSurface s = calloc(1, sizeof(*s)); s->width = attributes[1]; s->height = attributes[3]; live_surfaces++;
    assert(s->width > 0 && s->height > 0); return s;
}
static EGLSurface mock_create_window(void *display, EGLConfig config, EGLNativeWindowType window, const EGLint *attributes) {
    if (fail_window) return NULL;
    EGLSurface s = calloc(1, sizeof(*s)); s->window = TRUE; live_surfaces++; return s;
}
static BOOL mock_swap(void *display, EGLSurface surface) { assert(surface); swaps++; return TRUE; }
static void mock_interval(void *display, int value) { intervals++; }
static int mock_error(void) { return 0; }
static const char *mock_string(void *display, int name) { return "mock"; }
static void nx_osk_draw(struct opengl_drawable *base) { overlays++; }
static unsigned long long horizon_interrupt_time(void) { static unsigned int ticks; return ++ticks; }
static unsigned int wine_nx_gl_swaps;
static unsigned long long wine_nx_gl_swap_time;
struct egl_platform { void *display; EGLConfig configs[2]; unsigned int config_count; };
struct opengl_funcs {
    EGLSurface (*p_eglCreatePbufferSurface)(void *, EGLConfig, const EGLint *);
    EGLSurface (*p_eglCreateWindowSurface)(void *, EGLConfig, EGLNativeWindowType, const EGLint *);
    void (*p_eglDestroySurface)(void *, EGLSurface);
    int (*p_eglGetError)(void);
    const char *(*p_eglQueryString)(void *, int);
    BOOL (*p_eglSwapBuffers)(void *, EGLSurface);
    void (*p_eglSwapInterval)(void *, int);
};
static const struct egl_platform platform = { .configs = {1, 2}, .config_count = 2 };
static const struct egl_platform *egl = &platform;
static const struct opengl_funcs functions = {mock_create_pbuffer, mock_create_window, mock_destroy_surface, mock_error, mock_string, mock_swap, mock_interval};
static const struct opengl_funcs *funcs = &functions;
'''

implementation = nx[nx.index("struct nx_gl_drawable\n"):nx.index("static void nx_log(")]
for name in ("nx_config_for_format", "nx_drawable_destroy", "nx_drawable_flush", "nx_drawable_swap"):
    implementation += function(nx, name)
start = nx.index("static const struct opengl_drawable_funcs nx_drawable_funcs =")
implementation += nx[start:nx.index("\n};", start) + 3]
implementation += function(nx, "nx_surface_create")
implementation += function(core, "get_target") + function(core, "drawable_matches_window")
implementation += r'''
static WND *get_win_ptr(HWND hwnd) { return hwnd; }
static void release_win_ptr(WND *win) { }
static BOOL is_client_surface_window(struct client_surface *client, HWND hwnd) { return client && client->hwnd == hwnd; }
static HWND NtUserWindowFromDC(HDC hdc) { return hdc; }
static void use_window_client_surface(struct client_surface *client, BOOL use) { }
static struct client_surface *get_unused_client_surface(HWND hwnd, int format, BOOL raw) {
    return wine_nx_drv_CreateClientSurface(hwnd, format, raw);
}
static BOOL emulate_modeset;
static struct opengl_drawable *framebuffer_surface_create(int format, struct client_surface *client, struct opengl_drawable *target) { abort(); }
static const struct { BOOL (*p_surface_create)(struct client_surface *, int, struct opengl_drawable **); } drivers = {nx_surface_create};
static const typeof(drivers) *driver_funcs = &drivers;
static struct opengl_drawable *get_dc_opengl_drawable(HDC hdc) {
    struct opengl_drawable *d = ((WND *)hdc)->dc_drawable;
    if (d) opengl_drawable_add_ref(d);
    return d;
}
static void set_dc_opengl_drawable(HDC hdc, struct opengl_drawable *d) {
    struct opengl_drawable *old = ((WND *)hdc)->dc_drawable;
    ((WND *)hdc)->dc_drawable = d;
    if (d) { d->owner_hdc = hdc; opengl_drawable_add_ref(d); }
    if (old) { old->owner_hdc = NULL; opengl_drawable_release(old); }
}
'''
for name in ("set_window_opengl_drawable", "get_window_unused_drawable", "get_updated_drawable"):
    implementation += function(core, name)

checks = r'''
static void update(WND *win, BOOL visible, int width, int height) {
    win->visible = visible; win->rect = (RECT){0, 0, width, height};
    for (unsigned int i = 0; i < 64; i++) {
        struct client_surface *c = clients[i];
        if (!c || (c->hwnd != win && c->toplevel != win)) continue;
        c->virtual_rect = c->monitor_rect = ((WND *)c->hwnd)->rect;
        c->funcs->update(c);
    }
}
static void clear(WND *win, struct opengl_drawable *d) {
    set_window_opengl_drawable(win, NULL, TRUE);
    set_window_opengl_drawable(win, NULL, FALSE);
    set_dc_opengl_drawable(win, NULL);
    if (d) opengl_drawable_release(d);
}
int main(void) {
    WND probe = {.rect = {0, 0, 1280, 720}}, game = {.visible = TRUE, .rect = {0, 0, 1280, 720}};
    struct opengl_drawable *hidden = get_window_unused_drawable(&probe, 1);
    assert(hidden && !hidden->surface->window && hidden->surface->width == 1280 && hidden->surface->height == 720);
    assert(!screen_busy && !acquires);
    hidden->funcs->flush(hidden, GL_FLUSH_INTERVAL);
    assert(hidden->funcs->swap(hidden) && !overlays && !intervals && !wine_nx_gl_swaps);
    struct opengl_drawable *visible = get_window_unused_drawable(&game, 1);
    assert(visible && visible->surface->window && screen_busy && acquires == 1);
    assert(visible->funcs->swap(visible) && overlays == 1 && wine_nx_gl_swaps == 1);
    struct opengl_drawable *shared = get_window_unused_drawable(&game, 1);
    assert(shared && shared->surface == visible->surface && nx_screen_refs == 2 && acquires == 1);
    fail_pbuffer = TRUE;
    assert(!get_window_unused_drawable(&probe, 2) && screen_busy && acquires == 1 && !releases);
    fail_pbuffer = FALSE;
    struct opengl_drawable *another_hidden = get_window_unused_drawable(&probe, 1);
    assert(another_hidden && another_hidden->surface != visible->surface && acquires == 1);
    clear(&probe, another_hidden); clear(&game, shared); clear(&game, visible);
    assert(!screen_busy && releases == 1);

    set_window_opengl_drawable(&probe, hidden, TRUE);
    set_window_opengl_drawable(&probe, hidden, FALSE);
    update(&probe, FALSE, 640, 480);
    assert(!drawable_matches_window(hidden));
    struct opengl_drawable *resized = get_updated_drawable(NULL, 1, hidden);
    assert(resized && resized != hidden && resized->surface->width == 640 && resized->surface->height == 480);
    assert(!probe.current_drawable && !probe.unused_drawable);
    opengl_drawable_release(hidden); hidden = resized;
    set_window_opengl_drawable(&probe, hidden, TRUE);
    set_window_opengl_drawable(&probe, hidden, FALSE);
    update(&probe, TRUE, 640, 480);
    visible = get_updated_drawable(NULL, 1, hidden);
    assert(visible && visible->surface->window && screen_busy);
    opengl_drawable_release(hidden);
    assert(screen_busy);
    set_window_opengl_drawable(&probe, visible, TRUE);
    set_window_opengl_drawable(&probe, visible, FALSE);
    update(&probe, TRUE, 1280, 720);
    assert(drawable_matches_window(visible));
    update(&probe, FALSE, 1280, 720);
    hidden = get_updated_drawable(NULL, 1, visible);
    assert(hidden && !hidden->surface->window && screen_busy);
    set_window_opengl_drawable(&probe, visible, FALSE);
    assert(!probe.unused_drawable);
    opengl_drawable_release(visible);
    assert(!screen_busy);
    clear(&probe, hidden);

    fail_pbuffer = TRUE;
    assert(!get_window_unused_drawable(&probe, 1) && !screen_busy);
    fail_pbuffer = FALSE;
    fail_window = TRUE;
    assert(!get_window_unused_drawable(&game, 1) && !screen_busy);
    fail_window = FALSE;
    fail_drawable = TRUE;
    assert(!get_window_unused_drawable(&probe, 1));
    fail_drawable = FALSE;
    fail_client = TRUE;
    assert(!get_window_unused_drawable(&probe, 1));
    fail_client = FALSE;

    struct client_surface *vk = wine_nx_drv_CreateClientSurface(&game, 0, TRUE);
    unsigned int reads = style_reads;
    vk->funcs->update(vk);
    assert(reads == style_reads && !vk->offscreen);
    client_surface_release(vk);
    WND parent = {.rect = {0, 0, 800, 600}}, child = {.parent = &parent, .visible = TRUE, .rect = {0, 0, 320, 240}};
    hidden = get_window_unused_drawable(&child, 1);
    assert(hidden && !hidden->surface->window);
    update(&parent, TRUE, 800, 600);
    assert(!drawable_matches_window(hidden));
    visible = get_updated_drawable(NULL, 1, hidden);
    assert(visible && visible->surface->window);
    opengl_drawable_release(hidden); clear(&child, visible);

    hidden = get_window_unused_drawable(&probe, 1);
    set_dc_opengl_drawable(&probe, hidden);
    update(&probe, FALSE, 200, 100);
    resized = get_updated_drawable(NULL, 1, hidden);
    assert(resized && resized->surface->width == 200 && !probe.dc_drawable);
    opengl_drawable_release(hidden); clear(&probe, resized);
    WND empty = {0};
    hidden = get_window_unused_drawable(&empty, 1);
    assert(hidden && hidden->surface->width == 1 && hidden->surface->height == 1);
    clear(&empty, hidden);
    struct client_surface *raw = wine_nx_drv_CreateClientSurface(&probe, 1, TRUE);
    raw->monitor_rect = (RECT){0, 0, 400, 200};
    raw->funcs->update(raw);
    hidden = NULL;
    assert(nx_surface_create(raw, 1, &hidden));
    assert(hidden->surface->width == 400 && hidden->surface->height == 200);
    clear(&probe, hidden); client_surface_release(raw);
    assert(!live_drawables && !live_surfaces && !screen_busy && !nx_screen_refs);
    for (unsigned int i = 0; i < 64; i++) assert(!clients[i]);
    puts("OpenGL surfaces: offscreen probes, sharing, visibility, resizing, cached references, failures and Vulkan isolation passed");
}
'''

with tempfile.TemporaryDirectory(prefix="autorun-gl-surfaces-") as directory:
    path = Path(directory)
    source = path / "gl_surfaces.c"
    source.write_text(fixture + implementation + checks)
    executable = path / "gl_surfaces"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-variable", "-fsanitize=address,undefined", "-pthread",
                    str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
