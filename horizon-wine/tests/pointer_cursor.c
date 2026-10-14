/* Host test for the analog-stick cursor (source/pointer_cursor.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../source/pointer_cursor.h"

#define MS 1000000ull

static struct pointer_cursor cursor_at( double x, double y )
{
    struct pointer_cursor c = { .width = 1280, .height = 720 };

    pointer_cursor_place( &c, x, y );
    return c;
}

static void test_dead_zone_ignores_drift(void)
{
    struct pointer_cursor c = cursor_at( 640, 360 );

    assert( !pointer_cursor_step( &c, 3000, -2500, 50 * MS ) );
    assert( !pointer_cursor_step( &c, 0, 4000, 50 * MS ) );
    assert( c.x == 640 && c.y == 360 );
}

static void test_full_tilt_speed_and_axes(void)
{
    struct pointer_cursor c = cursor_at( 100, 360 );
    int i;

    /* One second of full right tilt in 20 ms polls. */
    for (i = 0; i < 50; i++) assert( pointer_cursor_step( &c, 32767, 0, 20 * MS ) );
    assert( fabs( c.x - 1100 ) < 0.001 && c.y == 360 );
    /* Stick up is positive on libnx and moves the cursor toward row 0. */
    assert( pointer_cursor_step( &c, 0, 32767, 100 * MS / 2 ) );
    assert( fabs( c.y - 310 ) < 0.001 );
    assert( pointer_cursor_step( &c, -32767, -32767, 50 * MS ) );
    assert( c.x < 1100 && c.y > 310 );
}

static void test_diagonal_is_not_faster(void)
{
    struct pointer_cursor c = cursor_at( 400, 400 );

    /* A square gate can report both axes at maximum. */
    pointer_cursor_step( &c, 32767, 32767, 50 * MS );
    assert( fabs( hypot( c.x - 400, c.y - 400 ) - 50 ) < 0.001 );
}

static void test_quadratic_response(void)
{
    struct pointer_cursor c = cursor_at( 0, 0 );
    int half = (int)(POINTER_CURSOR_DEAD_ZONE + (POINTER_CURSOR_STICK_MAX - POINTER_CURSOR_DEAD_ZONE) / 2);

    pointer_cursor_step( &c, half, 0, 40 * MS );
    assert( fabs( c.x - 10 ) < 0.01 ); /* a quarter of 1000 px/s for 40 ms */
}

static void test_slow_tilt_accumulates_subpixels(void)
{
    struct pointer_cursor c = cursor_at( 200, 200 );
    int moved = 0, polls = 0;

    /* About 11 px/s: a pixel takes several 10 ms polls. */
    while (!moved)
    {
        moved = pointer_cursor_step( &c, 7000, 0, 10 * MS );
        polls++;
    }
    assert( polls > 3 && (int)c.x == 201 && c.y == 200 );
}

static void test_stall_and_edges(void)
{
    struct pointer_cursor c = cursor_at( 640, 360 );

    /* Two seconds without a poll still moves only one capped step. */
    pointer_cursor_step( &c, 32767, 0, 2000 * MS );
    assert( fabs( c.x - 690 ) < 0.001 );
    c = cursor_at( 1279, 0 );
    assert( !pointer_cursor_step( &c, 32767, 32767, 50 * MS ) );
    assert( c.x == 1279 && c.y == 0 );
    c = cursor_at( -20, 9999 );
    assert( c.x == 0 && c.y == 719 );
}

static void test_paint_restores_pixels(void)
{
    enum { W = 64, H = 40, STRIDE = 70 };
    static uint32_t bits[STRIDE * H], before[STRIDE * H];
    struct pointer_cursor c = { .width = W, .height = H };
    unsigned int i, black = 0, white = 0;

    for (i = 0; i < STRIDE * H; i++) bits[i] = 0xff000000u | (i * 2654435761u >> 8);
    memcpy( before, bits, sizeof(bits) );

    pointer_cursor_place( &c, 10, 5 );
    pointer_cursor_paint( &c, bits, STRIDE, 1 );
    assert( bits[5 * STRIDE + 10] == 0xff000000u );            /* the tip */
    assert( bits[7 * STRIDE + 11] == 0xffffffffu );            /* fill */
    assert( bits[5 * STRIDE + 11] == before[5 * STRIDE + 11] ); /* transparent */
    for (i = 0; i < STRIDE * H; i++)
    {
        if (bits[i] == before[i]) continue;
        if (bits[i] == 0xff000000u) black++;
        else if (bits[i] == 0xffffffffu) white++;
        else assert( 0 );
    }
    assert( black > 30 && white > 40 );
    pointer_cursor_paint( &c, bits, STRIDE, 0 );
    assert( !memcmp( bits, before, sizeof(bits) ) );

    /* At the bottom-right corner only the tip is on screen. */
    pointer_cursor_place( &c, W - 1, H - 1 );
    pointer_cursor_paint( &c, bits, STRIDE, 1 );
    assert( bits[(H - 1) * STRIDE + W - 1] == 0xff000000u );
    assert( bits[(H - 1) * STRIDE + W] == before[(H - 1) * STRIDE + W] ); /* stride padding */
    pointer_cursor_paint( &c, bits, STRIDE, 0 );
    assert( !memcmp( bits, before, sizeof(bits) ) );
}

static void test_buttons_between_takes(void)
{
    enum { L = 1, R = 2 };
    struct pointer_buttons b = {0}, t;

    /* Nothing happens: nothing to deliver. */
    pointer_buttons_update( &b, 0 );
    t = pointer_buttons_take( &b );
    assert( !t.held && !t.pressed && !t.released );

    /* A held across several polls is one press. */
    pointer_buttons_update( &b, L );
    pointer_buttons_update( &b, L );
    t = pointer_buttons_take( &b );
    assert( t.held == L && t.pressed == L && !t.released );
    pointer_buttons_update( &b, L );
    t = pointer_buttons_take( &b );
    assert( t.held == L && !t.pressed && !t.released );

    /* A released and pressed again before the take: both edges survive. */
    pointer_buttons_update( &b, 0 );
    pointer_buttons_update( &b, L );
    t = pointer_buttons_take( &b );
    assert( t.held == L && t.pressed == L && t.released == L );

    /* A quick tap of B between two takes: pressed and released, not held. */
    pointer_buttons_update( &b, L | R );
    pointer_buttons_update( &b, L );
    t = pointer_buttons_take( &b );
    assert( t.held == L && t.pressed == R && t.released == R );

    /* Take clears the edges but keeps the state. */
    t = pointer_buttons_take( &b );
    assert( t.held == L && !t.pressed && !t.released );
}

/* A Quake III style loop: each frame the message pump polls and hands the
 * position to Wine, then, later in the frame, the game reads it, turns by its
 * offset from the centre and warps back there; a background thread polls
 * every 16 ms in between. Returns the motion the game saw, and the smallest
 * and largest per-frame turn after the first frame. */
static double run_warp_loop( int keep_motion, int stick_x, int seconds, int *min_turn, int *max_turn )
{
    enum { CX = 640, CY = 360, FRAME_MS = 27, READ_MS = 12, POLL_MS = 16 };
    struct pointer_cursor c = cursor_at( CX, CY );
    unsigned long long last_poll = 0;
    int sent_x = CX, sent_y = CY, t, seen = 0, frame = 0;

    *min_turn = 1 << 30;
    *max_turn = 0;
    for (t = 1; t <= seconds * 1000; t++)
    {
        if (t % POLL_MS == 5)  /* the background thread */
        {
            pointer_cursor_step( &c, stick_x, 0, (t - last_poll) * MS );
            last_poll = t;
        }
        if (t % FRAME_MS == 0)  /* the pump: poll, then take */
        {
            pointer_cursor_step( &c, stick_x, 0, (t - last_poll) * MS );
            last_poll = t;
            sent_x = (int)c.x;
            sent_y = (int)c.y;
        }
        if (t % FRAME_MS == READ_MS)  /* GetCursorPos, then SetCursorPos to the centre */
        {
            int turn = sent_x - CX;

            seen += turn;
            if (frame++ && t > FRAME_MS * 2)
            {
                if (turn < *min_turn) *min_turn = turn;
                if (turn > *max_turn) *max_turn = turn;
            }
            if (keep_motion) pointer_cursor_warp( &c, sent_x, sent_y, CX, CY );
            else pointer_cursor_place( &c, CX, CY );
            sent_x = CX;
            sent_y = CY;
        }
    }
    return seen;
}

static void test_warp_keeps_unsent_motion(void)
{
    int slow = (int)(POINTER_CURSOR_DEAD_ZONE + (POINTER_CURSOR_STICK_MAX - POINTER_CURSOR_DEAD_ZONE) * 0.15);
    int min_turn, max_turn;
    double seen;

    /* Full tilt: 1000 px/s for 3 s, a steady 27 px each 27 ms frame. */
    seen = run_warp_loop( 1, 32767, 3, &min_turn, &max_turn );
    assert( fabs( seen - 3000 ) < 40 );
    assert( min_turn >= 26 && max_turn <= 28 );
    /* Dropping it at the warp loses the polls that land between the take and
     * the warp, and the turn stops and goes: what the Switch showed. */
    seen = run_warp_loop( 0, 32767, 3, &min_turn, &max_turn );
    assert( seen < 2500 && max_turn - min_turn > 10 );

    /* A slight tilt, 22.5 px/s, well under a pixel a frame, still turns. */
    seen = run_warp_loop( 1, slow, 4, &min_turn, &max_turn );
    assert( fabs( seen - 90 ) < 3 && max_turn <= 1 );
    assert( run_warp_loop( 0, slow, 4, &min_turn, &max_turn ) < 10 );
}

/* A program that took the mouse for itself: the server holds the cursor still
 * and tells the program the movement instead, and the driver puts the arrow
 * back on the cursor after each poll. Returns the movement sent, which is the
 * stick's own count and owes nothing to where the cursor is. */
static double run_held_cursor_loop( int held_x, int stick_x, int seconds )
{
    enum { CY = 360, FRAME_MS = 27, POLL_MS = 16 };
    struct pointer_cursor c = cursor_at( held_x, CY );
    unsigned long long last_poll = 0;
    double told = 0;
    int t, dx, dy;

    for (t = 1; t <= seconds * 1000; t++)
    {
        if (t % POLL_MS == 5)
        {
            pointer_cursor_step( &c, stick_x, 0, (t - last_poll) * MS );
            last_poll = t;
        }
        if (t % FRAME_MS == 0)
        {
            pointer_cursor_step( &c, stick_x, 0, (t - last_poll) * MS );
            last_poll = t;
            pointer_cursor_take_motion( &c, &dx, &dy );
            told += dx;
            pointer_cursor_place( &c, held_x, CY );  /* the arrow follows the cursor */
        }
    }
    return told;
}

static void test_held_cursor_still_moves(void)
{
    /* Both ways, at the same rate: 1000 px/s for two seconds. */
    assert( fabs( run_held_cursor_loop( 640, 32767, 2 ) - 2000 ) < 60 );
    assert( fabs( run_held_cursor_loop( 640, -32767, 2 ) + 2000 ) < 60 );
    /* And wherever the cursor was when the program took the mouse, the corner
     * of the screen included: Halo holds it at 0,0, and a view turning left
     * from there is the whole of what the stick did, not nothing. */
    assert( fabs( run_held_cursor_loop( 0, -32767, 2 ) + 2000 ) < 60 );
    assert( fabs( run_held_cursor_loop( 1279, 32767, 2 ) - 2000 ) < 60 );
    /* A slight tilt, well under a pixel a frame, still adds up. */
    {
        int slow = (int)(POINTER_CURSOR_DEAD_ZONE +
                         (POINTER_CURSOR_STICK_MAX - POINTER_CURSOR_DEAD_ZONE) * 0.15);

        assert( fabs( run_held_cursor_loop( 0, -slow, 4 ) + 90 ) < 4 );
    }
}

static void test_speed_scales_the_stick(void)
{
    struct pointer_cursor a = cursor_at( 0, 360 );
    struct pointer_cursor b = cursor_at( 0, 360 );
    struct pointer_cursor c = cursor_at( 0, 360 );

    /* Half the speed, half the distance over the same second. */
    b.speed = POINTER_CURSOR_SPEED * 0.5;
    for (int i = 0; i < 50; i++) { pointer_cursor_step( &a, 32767, 0, 20 * MS ); pointer_cursor_step( &b, 32767, 0, 20 * MS ); }
    assert( fabs( a.x - 1000 ) < 0.01 && fabs( b.x - 500 ) < 0.01 );
    /* A speed of zero (a cursor no one configured) keeps the built-in one. */
    for (int i = 0; i < 50; i++) pointer_cursor_step( &c, 32767, 0, 20 * MS );
    assert( fabs( c.x - a.x ) < 0.01 );
}

int main(void)
{
    test_buttons_between_takes();
    test_dead_zone_ignores_drift();
    test_full_tilt_speed_and_axes();
    test_diagonal_is_not_faster();
    test_quadratic_response();
    test_slow_tilt_accumulates_subpixels();
    test_stall_and_edges();
    test_paint_restores_pixels();
    test_warp_keeps_unsent_motion();
    test_held_cursor_still_moves();
    test_speed_scales_the_stick();
    puts( "pointer cursor: buttons between takes, dead zone, speed curve, time scaling, edges, sprite "
          "restore, motion kept across warps, a held cursor still moving and the speed scale passed" );
    return 0;
}
