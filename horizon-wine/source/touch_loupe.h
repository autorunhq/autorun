/*
 * A magnifier for a finger on the touchscreen. The controls of a program made
 * for a mouse are smaller than a fingertip, and the finger hides what it
 * touches. A touch that stays still for a moment shows the part of the screen
 * around it, enlarged, above the finger with a crosshair on the place that a
 * release clicks, so the finger can be moved until the crosshair is on the
 * control.
 *
 * A touch does not press the mouse button when it goes down, because it is not
 * known yet whether it is a tap, a drag or a wait for the magnifier:
 *  - a release before anything else is a tap: a click at the place it started;
 *  - a move of more than TOUCH_LOUPE_SLOP pixels is a drag: the button goes
 *    down at the place it started and stays down while the finger is down;
 *  - the delay (a setting, TOUCH_LOUPE_DELAY_DEFAULT_MS by default) without such
 *    a move shows the magnifier, and the click comes at the release, where the
 *    finger is then.
 *
 * The picture is scaled in dlls/win32u/vulkan.c (nx_loupe_present).
 */
#ifndef WINE_NX_TOUCH_LOUPE_H
#define WINE_NX_TOUCH_LOUPE_H

#include <stdint.h>

#define TOUCH_LOUPE_DELAY_DEFAULT_MS 350   /* how long a finger stays still before the magnifier comes up */
#define TOUCH_LOUPE_DELAY_MIN_MS     100
#define TOUCH_LOUPE_DELAY_MAX_MS     2000
#define TOUCH_LOUPE_SLOP     12  /* pixels; a finger is never quite still */

/* The magnifier has one size. The zoom decides how much of the screen goes into
 * it: 480 and 300 are whole multiples of every zoom, so the enlargement stays a
 * whole number of times and the program's pixels stay sharp. */
#define TOUCH_LOUPE_DST_W 480
#define TOUCH_LOUPE_DST_H 300
#define TOUCH_LOUPE_ZOOM_DEFAULT 3
#define TOUCH_LOUPE_ZOOM_MIN     2
#define TOUCH_LOUPE_ZOOM_MAX     4
#define TOUCH_LOUPE_GAP   40  /* between the finger's place and the magnifier */

struct touch_loupe_rect
{
    int zoom;
    int src_x, src_y, src_w, src_h;  /* the part of the screen that is enlarged */
    int dst_x, dst_y, dst_w, dst_h;  /* where it is shown */
    int cross_x, cross_y;            /* the place of the finger, in the magnifier */
};

static inline int touch_loupe_clamp( int value, int low, int high )
{
    return value < low ? low : value > high ? high : value;
}

/* The rectangles for a finger at x, y; zero when the screen is too small or the
 * zoom is not one the magnifier has. */
static inline int touch_loupe_layout( int screen_w, int screen_h, int x, int y, int zoom,
                                      struct touch_loupe_rect *r )
{
    if (screen_w < TOUCH_LOUPE_DST_W || screen_h < TOUCH_LOUPE_DST_H ||
        zoom < TOUCH_LOUPE_ZOOM_MIN || zoom > TOUCH_LOUPE_ZOOM_MAX) return 0;
    x = touch_loupe_clamp( x, 0, screen_w - 1 );
    y = touch_loupe_clamp( y, 0, screen_h - 1 );
    r->zoom = zoom;
    r->src_w = TOUCH_LOUPE_DST_W / zoom;
    r->src_h = TOUCH_LOUPE_DST_H / zoom;
    r->src_x = touch_loupe_clamp( x - r->src_w / 2, 0, screen_w - r->src_w );
    r->src_y = touch_loupe_clamp( y - r->src_h / 2, 0, screen_h - r->src_h );
    r->dst_w = TOUCH_LOUPE_DST_W;
    r->dst_h = TOUCH_LOUPE_DST_H;
    r->dst_x = touch_loupe_clamp( x - r->dst_w / 2, 0, screen_w - r->dst_w );
    r->dst_y = y - TOUCH_LOUPE_GAP - r->dst_h;
    if (r->dst_y < 0) r->dst_y = y + TOUCH_LOUPE_GAP;  /* no room above the finger */
    r->dst_y = touch_loupe_clamp( r->dst_y, 0, screen_h - r->dst_h );
    r->cross_x = r->dst_x + (x - r->src_x) * zoom;
    r->cross_y = r->dst_y + (y - r->src_y) * zoom;
    return 1;
}

enum touch_loupe_phase
{
    TOUCH_LOUPE_IDLE,    /* no finger */
    TOUCH_LOUPE_WAIT,    /* a finger is down and has not decided yet */
    TOUCH_LOUPE_SHOWN,   /* the magnifier is up */
    TOUCH_LOUPE_DRAG,    /* the button is down */
};

struct touch_loupe
{
    int phase;
    int start_x, start_y;  /* where the finger went down */
    int x, y;              /* where it was last */
    uint64_t since;        /* when it went down, in nanoseconds */
};

struct touch_loupe_output
{
    int x, y;      /* the place the program is given */
    int left;      /* whether the left button is down */
    int loupe;     /* whether the magnifier is up */
};

/* One look at the screen: whether a finger is down, and where, and for how many
 * nanoseconds it waits before it shows the magnifier. Returns zero
 * when the touch has nothing to say (no finger, or the finger of a drag has
 * just gone up, which the button's release already shows). */
static inline int touch_loupe_step( struct touch_loupe *t, int down, int x, int y, uint64_t now,
                                    uint64_t delay, struct touch_loupe_output *out )
{
    int dx, dy;

    out->left = out->loupe = 0;
    switch (t->phase)
    {
    case TOUCH_LOUPE_IDLE:
        if (!down) return 0;
        t->phase = TOUCH_LOUPE_WAIT;
        t->start_x = t->x = x;
        t->start_y = t->y = y;
        t->since = now;
        out->x = x;
        out->y = y;
        return 1;
    case TOUCH_LOUPE_WAIT:
        if (!down)  /* a tap */
        {
            t->phase = TOUCH_LOUPE_IDLE;
            out->x = t->start_x;
            out->y = t->start_y;
            out->left = 1;
            return 1;
        }
        t->x = x;
        t->y = y;
        dx = x - t->start_x;
        dy = y - t->start_y;
        if (dx * dx + dy * dy > TOUCH_LOUPE_SLOP * TOUCH_LOUPE_SLOP)
        {
            t->phase = TOUCH_LOUPE_DRAG;
            out->x = t->start_x;
            out->y = t->start_y;
            out->left = 1;
            return 1;
        }
        if (now - t->since >= delay) t->phase = TOUCH_LOUPE_SHOWN;
        out->x = x;
        out->y = y;
        out->loupe = t->phase == TOUCH_LOUPE_SHOWN;
        return 1;
    case TOUCH_LOUPE_SHOWN:
        if (!down)  /* the click, where the finger was last */
        {
            t->phase = TOUCH_LOUPE_IDLE;
            out->x = t->x;
            out->y = t->y;
            out->left = 1;
            return 1;
        }
        t->x = out->x = x;
        t->y = out->y = y;
        out->loupe = 1;
        return 1;
    default:  /* TOUCH_LOUPE_DRAG */
        if (!down)
        {
            t->phase = TOUCH_LOUPE_IDLE;
            return 0;
        }
        t->x = out->x = x;
        t->y = out->y = y;
        out->left = 1;
        return 1;
    }
}

#endif
