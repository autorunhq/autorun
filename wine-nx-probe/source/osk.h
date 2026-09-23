/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The floating keyboard: a keyboard drawn over the program, worked with the
 * controller or the touchscreen, that sends one key at a time the way a real
 * keyboard does. Horizon's own keyboard applet can only hand back a finished
 * string, so a field a game draws itself could be added to but never edited:
 * no Backspace, no arrows, and Enter sent whether or not it was wanted.
 *
 * The runtime feeds it the controller while it is up (wine_nx_osk_input) and
 * the display driver sends what it queues (wine_nx_osk_next_key); each of the
 * ways a frame reaches the screen (Vulkan, OpenGL, the window compositor)
 * copies its picture over the frame (wine_nx_osk_frame, wine_nx_osk_copy).
 * Every call may come from any thread.
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

/* The font the labels are drawn with, a TrueType file in memory that stays
 * there. The runtime gives it the console's shared font. */
int wine_nx_osk_font( const void **data, size_t *size );

int wine_nx_osk_visible( void );
/* A number that changes whenever the keyboard does, shown or hidden: cheap,
 * for a caller that only has to know something should be drawn again. */
unsigned int wine_nx_osk_generation( void );
/* Shows or hides it. Buttons held when it opens are ignored until released,
 * so the combination that opened it does not also press a key. */
void wine_nx_osk_show( int show, unsigned int held );

/* One reading of the controller while it is up: the buttons held, the left
 * stick (-32768 to 32767, up positive), and a finger on a 1280x720 screen
 * when touching is set. Returns whether the finger is on the keyboard, which
 * is then not the program's. now_ns only has to go up. */
int wine_nx_osk_input( unsigned int held, int stick_x, int stick_y, int touching, int touch_x, int touch_y,
                       uint64_t now_ns );

/* The next key due by now_ns (the clock wine_nx_osk_input is given), oldest
 * first: its virtual-key code and whether it is going up. Returns 0 when there
 * is none yet. A key stays down some tens of milliseconds, Shift around it. */
int wine_nx_osk_next_key( uint64_t now_ns, unsigned short *vk, int *up );
/* That clock, in the runtime: the system tick in nanoseconds. */
uint64_t wine_nx_osk_clock( void );

/* Where the keyboard goes on a screen of that size, and a number that changes
 * whenever its picture does. Returns 0 while it is hidden. */
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
