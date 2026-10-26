/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The controls overlay: the controls editor that opens over a running program
 * (Minus + the right stick), so a mapping can be tried and fixed where the
 * game is, instead of back in the launcher. It works on the same per-game
 * controls the launcher edits -- the program's settings file, over the game's
 * recommended layout and the global controls -- and saves each change as it is
 * made, so closing it is only ever closing it.
 *
 * One list of the controller's own inputs (what A does, what ZL does), a row
 * opened into what it should do: keys and mouse actions picked from a grid in
 * keyboard/mouse mode, or captured from the controller in controller mode.
 * Rows cleared or reset fall back through the layers below.
 *
 * Like the floating keyboard (osk.h) it is drawn here in software and copied
 * over each frame by the presenters; osk.c hands its frame and copy calls to
 * this while the overlay is up. Every call may come from any thread.
 */
#ifndef WINE_NX_CONTROLS_OVERLAY_H
#define WINE_NX_CONTROLS_OVERLAY_H

#include <stddef.h>
#include <stdint.h>
#include "input_profile.h"

/* Opens or closes it over a program (root and program as input_profile_program
 * takes them; the global controls when program is NULL). Buttons held when it
 * opens are ignored until released. Opening it hides the floating keyboard.
 * Returns 0 when the controls could not be read. */
int wine_nx_controls_overlay_show( int show, unsigned int held, const char *root, const char *program );
int wine_nx_controls_overlay_visible( void );
/* A number that changes whenever its picture should be drawn again. Its top
 * bit is always set, so a presenter that keeps one generation for whichever
 * overlay is up never sees the keyboard's and this one collide. */
unsigned int wine_nx_controls_overlay_generation( void );

/* One reading of the controller while it is up: the buttons held as the
 * keyboard reads them (OSK_* in osk.h), both sticks (-32768 to 32767, up and
 * right positive), and a finger on a 1280x720 screen. Returns whether the
 * finger is on the overlay, which is then not the program's. */
int wine_nx_controls_overlay_input( unsigned int held, int left_x, int left_y, int right_x, int right_y,
                                    int touching, int touch_x, int touch_y, uint64_t now_ns );

/* Where the overlay goes on a screen of that size, and its generation.
 * Returns 0 while it is hidden. The struct is osk.h's. */
struct wine_nx_osk_frame;
int wine_nx_controls_overlay_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame );
/* The picture, frame.height rows of frame.width pixels into rows stride bytes
 * apart: B, G, R, A bytes, or R, G, B, A with rgba. 0 when hidden. */
unsigned int wine_nx_controls_overlay_copy( int screen_width, int screen_height, void *pixels,
                                            int stride, int rgba );

/* The row the selection is on (input_sources' order), for tests. */
int wine_nx_controls_overlay_selected( void );
/* Forget everything, for tests. */
void wine_nx_controls_overlay_reset( void );

#endif
