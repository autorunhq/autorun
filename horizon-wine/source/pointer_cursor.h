/*
 * A mouse cursor driven by an analog stick, and the arrow drawn for it.
 *
 * The stick sets a velocity rather than a position. Past a radial dead zone
 * that hides resting drift, the speed grows with the square of the remaining
 * deflection: a slight tilt places the cursor precisely and a full tilt
 * crosses the 1280-pixel screen in about a second. Win32u polls at an
 * irregular rate, so each step is scaled by the time since the previous one,
 * capped so that a stalled message loop cannot fling the cursor away.
 *
 * Horizon has no hardware cursor. The arrow is painted into the back buffer
 * just before a frame is queued and the covered pixels are put back right
 * after, so window blits never pick it up.
 */
#ifndef WINE_NX_POINTER_CURSOR_H
#define WINE_NX_POINTER_CURSOR_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define POINTER_CURSOR_STICK_MAX   32767.0 /* libnx JOYSTICK_MAX */
#define POINTER_CURSOR_DEAD_ZONE   4000.0  /* about 12% of the stick range */
#define POINTER_CURSOR_SPEED       1000.0  /* pixels per second at full tilt */
#define POINTER_CURSOR_MAX_STEP_NS 50000000ull

#define POINTER_CURSOR_W 12
#define POINTER_CURSOR_H 19

struct pointer_cursor
{
    double x, y;       /* sub-pixel position; the hot spot is the arrow tip */
    double motion_x, motion_y; /* movement not yet handed over, sub-pixel */
    int width, height; /* screen size */
    double speed;      /* pixels per second at full tilt; 0 means the default */
    uint32_t under[POINTER_CURSOR_W * POINTER_CURSOR_H];
};

/* The movement the stick has made since this was last asked, in whole pixels,
 * keeping the rest for next time. A mouse reports what it did, not where it
 * is: the screen's edges stop the cursor and a program turning a view still
 * has to be told, and a program that has taken the mouse for itself holds the
 * cursor still and is told nothing else. Returns nonzero when there is any. */
static inline int pointer_cursor_take_motion( struct pointer_cursor *c, int *dx, int *dy )
{
    *dx = (int)c->motion_x;
    *dy = (int)c->motion_y;
    c->motion_x -= *dx;
    c->motion_y -= *dy;
    return *dx || *dy;
}

static inline void pointer_cursor_place( struct pointer_cursor *c, double x, double y )
{
    c->x = x < 0 ? 0 : x > c->width - 1 ? c->width - 1 : x;
    c->y = y < 0 ? 0 : y > c->height - 1 ? c->height - 1 : y;
}

/* Move by the stick deflection (libnx axes: y up) held for elapsed_ns.
 * Returns nonzero when the whole-pixel position changed. */
static inline int pointer_cursor_step( struct pointer_cursor *c, int stick_x, int stick_y,
                                       unsigned long long elapsed_ns )
{
    int old_x = (int)c->x, old_y = (int)c->y;
    double dx = stick_x, dy = -(double)stick_y;
    double deflection = sqrt( dx * dx + dy * dy ), tilt, distance, speed;

    if (deflection <= POINTER_CURSOR_DEAD_ZONE) return 0;
    if (elapsed_ns > POINTER_CURSOR_MAX_STEP_NS) elapsed_ns = POINTER_CURSOR_MAX_STEP_NS;
    tilt = (deflection - POINTER_CURSOR_DEAD_ZONE) / (POINTER_CURSOR_STICK_MAX - POINTER_CURSOR_DEAD_ZONE);
    if (tilt > 1) tilt = 1;
    /* The caller's speed, or the built-in one when it left it at zero. */
    speed = c->speed > 0 ? c->speed : POINTER_CURSOR_SPEED;
    distance = speed * tilt * tilt * (double)elapsed_ns / 1e9;
    dx = dx / deflection * distance;
    dy = dy / deflection * distance;
    c->motion_x += dx;
    c->motion_y += dy;
    pointer_cursor_place( c, c->x + dx, c->y + dy );
    return (int)c->x != old_x || (int)c->y != old_y;
}

/* The program moved the cursor to x, y (SetCursorPos) after Wine was last
 * handed sent_x, sent_y. Stick motion since then, its sub-pixel part included,
 * carries on from the new position, as mouse motion Windows has not applied yet
 * does: Quake III's mouse look warps the cursor to its window's centre every
 * frame, and dropping that motion made the view move, stop and move again.
 * Returns nonzero when the whole-pixel position is not x, y, so it still has
 * to be sent. */
static inline int pointer_cursor_warp( struct pointer_cursor *c, int sent_x, int sent_y, int x, int y )
{
    pointer_cursor_place( c, x + (c->x - sent_x), y + (c->y - sent_y) );
    return (int)c->x != x || (int)c->y != y;
}

/* Buttons seen by the polls since the last take: the latest state, and every
 * button that went down or up in between. Polls can outpace the message loop
 * that takes them, and a click shorter than that gap must not be lost. */
struct pointer_buttons
{
    unsigned int held, pressed, released;
};

static inline void pointer_buttons_update( struct pointer_buttons *b, unsigned int held )
{
    b->pressed |= held & ~b->held;
    b->released |= b->held & ~held;
    b->held = held;
}

static inline struct pointer_buttons pointer_buttons_take( struct pointer_buttons *b )
{
    struct pointer_buttons taken = *b;

    b->pressed = b->released = 0;
    return taken;
}

/* The classic Windows arrow: B outline, W fill, '.' transparent. */
static const char pointer_cursor_shape[POINTER_CURSOR_H][POINTER_CURSOR_W + 1] =
{
    "B...........",
    "BB..........",
    "BWB.........",
    "BWWB........",
    "BWWWB.......",
    "BWWWWB......",
    "BWWWWWB.....",
    "BWWWWWWB....",
    "BWWWWWWWB...",
    "BWWWWWWWWB..",
    "BWWWWWWWWWB.",
    "BWWWWWWBBBBB",
    "BWWWBWWB....",
    "BWWBBWWB....",
    "BWB..BWWB...",
    "BB...BWWB...",
    "B.....BWWB..",
    "......BWWB..",
    ".......BB...",
};

/* Draw the arrow into RGBA8888 pixels (draw != 0), saving what it covers, or
 * put the saved pixels back. The position must not change in between. */
static inline void pointer_cursor_paint( struct pointer_cursor *c, uint32_t *bits, int stride, int draw )
{
    int left = (int)c->x, top = (int)c->y, i, j;

    for (j = 0; j < POINTER_CURSOR_H && top + j < c->height; j++)
    {
        for (i = 0; i < POINTER_CURSOR_W && left + i < c->width; i++)
        {
            char shape = pointer_cursor_shape[j][i];
            uint32_t *pixel = bits + (size_t)(top + j) * stride + left + i;
            uint32_t *saved = c->under + j * POINTER_CURSOR_W + i;

            if (shape == '.') continue;
            if (!draw) *pixel = *saved;
            else
            {
                *saved = *pixel;
                *pixel = shape == 'W' ? 0xffffffffu : 0xff000000u;
            }
        }
    }
}

#endif
