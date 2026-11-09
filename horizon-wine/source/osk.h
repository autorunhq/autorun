/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The floating keyboard: a keyboard drawn over the program, worked with the
 * controller or the touchscreen, that sends keys the way a physical keyboard
 * does: a key is down while it is held (by a finger, or by the button that
 * presses it) and up when that ends, so Shift can be held with one finger
 * while another types, and a held key repeats. Horizon's own keyboard applet
 * can only hand back a finished string, so a field a game draws itself could
 * be added to but never edited: no Backspace, no arrows, and Enter sent
 * whether or not it was wanted.
 *
 * The runtime feeds it the controller and the fingers while it is up
 * (wine_nx_osk_input) and the display driver sends what it queues
 * (wine_nx_osk_next_key); each of the ways a frame reaches the screen
 * (Vulkan, OpenGL, the window compositor) copies its picture over the frame
 * (wine_nx_osk_frame, wine_nx_osk_copy). Every call may come from any thread.
 */
#ifndef WINE_NX_OSK_H
#define WINE_NX_OSK_H

#include <stddef.h>
#include <stdint.h>

/* The controller's buttons, as the keyboard reads them. */
enum
{
    OSK_UP = 1 << 0, OSK_DOWN = 1 << 1, OSK_LEFT = 1 << 2, OSK_RIGHT = 1 << 3,
    OSK_A = 1 << 4, OSK_B = 1 << 5, OSK_X = 1 << 6, OSK_Y = 1 << 7,
    OSK_L = 1 << 8, OSK_R = 1 << 9, OSK_ZL = 1 << 10, OSK_ZR = 1 << 11,
    OSK_PLUS = 1 << 12, OSK_MINUS = 1 << 13, OSK_STICKL = 1 << 14, OSK_STICKR = 1 << 15,
};

/* The controls panel: what the controller sends, drawn over the program while
 * it is up, in two columns of name and what it sends. The runtime builds the
 * rows from the profile in effect -- what the program's controller sees, or the
 * keys and mouse actions it gets -- and shows and hides it; the keyboard and
 * the panel are never up at once. */
struct wine_nx_osk_map_row
{
    const char *name, *value;
};
void wine_nx_osk_map_show( int show, const char *title, const struct wine_nx_osk_map_row *rows, int count );
/* The tasks panel: the same surface, another view of it -- what the program is
 * running, with the core each thread is on and the share of a core it took.
 * The runtime builds the rows and puts it up again while it is open, so what
 * it shows is the program as it is rather than as it was. */
void wine_nx_osk_tasks_show( int show, const char *title, const struct wine_nx_osk_map_row *rows, int count );
int wine_nx_osk_map_visible( void );

/* The font the labels are drawn with, a TrueType file in memory that stays
 * there. The runtime gives it the console's shared font. */
int wine_nx_osk_font( const void **data, size_t *size );

int wine_nx_osk_visible( void );
/* A number that changes whenever the keyboard does, shown or hidden: cheap,
 * for a caller that only has to know something should be drawn again. */
unsigned int wine_nx_osk_generation( void );
/* Shows or hides it. Buttons held when it opens are ignored until released,
 * so the combination that opened it does not also press a key. Hiding it lets
 * go of every key that is down, so nothing is left held behind it. */
void wine_nx_osk_show( int show, unsigned int held );

/* One point of a finger on a 1280x720 screen. */
struct wine_nx_osk_point
{
    int x, y;
};

/* One reading of the controller while it is up: the buttons held, the left
 * stick (-32768 to 32767, up positive), and the fingers down, each a point on
 * a 1280x720 screen. A key is down while a finger is on it, and several
 * fingers hold several keys, so Shift can be held while another types.
 * Returns whether any finger is on the keyboard, which is then not the
 * program's. now_ns only has to go up. */
int wine_nx_osk_input( unsigned int held, int stick_x, int stick_y,
                       const struct wine_nx_osk_point *fingers, int count, uint64_t now_ns );

/* The next key event due by now_ns (the clock wine_nx_osk_input is given),
 * oldest first: its virtual-key code and whether it is going up. Returns 0
 * when there is none yet. What the keyboard queues is what a physical one
 * sends: a down when a key goes down, repeats while it is held, and an up when
 * it is let go -- no sooner than some tens of milliseconds after its down, so
 * a program that looks at the keyboard once a frame still sees it. */
int wine_nx_osk_next_key( uint64_t now_ns, unsigned short *vk, int *up );
/* That clock, in the runtime: the system tick in nanoseconds. */
uint64_t wine_nx_osk_clock( void );

/* Where the keyboard, or the controls panel, goes on a screen of that size, and
 * a number that changes whenever its picture does. Returns 0 while both are
 * hidden. */
struct wine_nx_osk_frame
{
    int x, y, width, height;
    unsigned int generation;
};
int wine_nx_osk_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame );
/* The picture for that screen, frame.height rows of frame.width pixels, into
 * rows stride bytes apart: B, G, R, A bytes, or R, G, B, A with rgba. Returns
 * the generation copied, 0 when hidden. */
unsigned int wine_nx_osk_copy( int screen_width, int screen_height, void *pixels, int stride, int rgba );

/* Forget everything, for tests. */
void wine_nx_osk_reset( void );

#endif
