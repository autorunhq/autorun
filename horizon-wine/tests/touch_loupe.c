/* Host test for the finger's magnifier (source/touch_loupe.h). */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "../source/touch_loupe.h"

#define MS 1000000ull
#define DELAY (TOUCH_LOUPE_DELAY_DEFAULT_MS * MS)

static void test_layout_above_the_finger(void)
{
    struct touch_loupe_rect r;

    assert( touch_loupe_layout( 1280, 720, 640, 500, 3, &r ) );
    assert( r.src_x == 560 && r.src_y == 450 && r.src_w == 160 && r.src_h == 100 );
    assert( r.dst_w == 480 && r.dst_h == 300 && r.dst_x == 400 );
    assert( r.dst_y + r.dst_h == 500 - TOUCH_LOUPE_GAP );
    assert( r.cross_x == r.dst_x + r.dst_w / 2 && r.cross_y == r.dst_y + r.dst_h / 2 );
}

static void test_layout_below_when_there_is_no_room_above(void)
{
    struct touch_loupe_rect r;

    assert( touch_loupe_layout( 1280, 720, 640, 100, 3, &r ) );
    assert( r.dst_y == 100 + TOUCH_LOUPE_GAP );
}

static void test_layout_stays_on_the_screen_in_every_corner(void)
{
    static const int xs[] = { 0, 5, 640, 1274, 1279 }, ys[] = { 0, 5, 360, 714, 719 };
    struct touch_loupe_rect r;
    int i, j;

    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
        {
            assert( touch_loupe_layout( 1280, 720, xs[i], ys[j], 3, &r ) );
            assert( r.src_x >= 0 && r.src_y >= 0 && r.src_x + r.src_w <= 1280 && r.src_y + r.src_h <= 720 );
            assert( r.dst_x >= 0 && r.dst_y >= 0 && r.dst_x + r.dst_w <= 1280 && r.dst_y + r.dst_h <= 720 );
            /* the crosshair is on the finger's place, enlarged */
            assert( r.cross_x == r.dst_x + (xs[i] - r.src_x) * r.zoom );
            assert( r.cross_y == r.dst_y + (ys[j] - r.src_y) * r.zoom );
            assert( r.cross_x >= r.dst_x && r.cross_x < r.dst_x + r.dst_w );
            assert( r.cross_y >= r.dst_y && r.cross_y < r.dst_y + r.dst_h );
        }
}

static void test_layout_follows_the_zoom(void)
{
    struct touch_loupe_rect r;
    int zoom;

    for (zoom = TOUCH_LOUPE_ZOOM_MIN; zoom <= TOUCH_LOUPE_ZOOM_MAX; zoom++)
    {
        assert( touch_loupe_layout( 1280, 720, 640, 500, zoom, &r ) );
        assert( r.zoom == zoom && r.dst_w == TOUCH_LOUPE_DST_W && r.dst_h == TOUCH_LOUPE_DST_H );  /* one size */
        assert( r.src_w * zoom == r.dst_w && r.src_h * zoom == r.dst_h );  /* a whole number of times */
        assert( r.src_x == 640 - r.src_w / 2 && r.src_y == 500 - r.src_h / 2 );
        assert( r.cross_x == r.dst_x + (640 - r.src_x) * zoom && r.cross_y == r.dst_y + (500 - r.src_y) * zoom );
    }
    assert( touch_loupe_layout( 1280, 720, 640, 500, 2, &r ) && r.src_w == 240 && r.src_h == 150 );
    assert( touch_loupe_layout( 1280, 720, 640, 500, 4, &r ) && r.src_w == 120 && r.src_h == 75 );
    assert( !touch_loupe_layout( 1280, 720, 640, 500, TOUCH_LOUPE_ZOOM_MIN - 1, &r ) );
    assert( !touch_loupe_layout( 1280, 720, 640, 500, TOUCH_LOUPE_ZOOM_MAX + 1, &r ) );
}

static void test_layout_needs_a_screen_that_holds_the_magnifier(void)
{
    struct touch_loupe_rect r;

    assert( !touch_loupe_layout( 400, 720, 100, 100, 3, &r ) );
    assert( !touch_loupe_layout( 1280, 250, 100, 100, 3, &r ) );
}

static void test_no_finger_says_nothing(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    assert( !touch_loupe_step( &t, 0, 0, 0, 0, DELAY, &o ) );
}

static void test_a_tap_clicks_where_it_started_when_it_ends(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    assert( touch_loupe_step( &t, 1, 100, 200, 0, DELAY, &o ) && !o.left && !o.loupe && o.x == 100 && o.y == 200 );
    assert( touch_loupe_step( &t, 1, 104, 203, 50 * MS, DELAY, &o ) && !o.left && !o.loupe );  /* within the slop */
    assert( touch_loupe_step( &t, 0, 0, 0, 100 * MS, DELAY, &o ) );
    assert( o.left && o.x == 100 && o.y == 200 && !o.loupe );
    assert( !touch_loupe_step( &t, 0, 0, 0, 120 * MS, DELAY, &o ) );  /* and then nothing */
}

static void test_a_move_is_a_drag_that_starts_where_the_finger_went_down(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    touch_loupe_step( &t, 1, 100, 200, 0, DELAY, &o );
    assert( touch_loupe_step( &t, 1, 100 + TOUCH_LOUPE_SLOP + 1, 200, 30 * MS, DELAY, &o ) );
    assert( o.left && o.x == 100 && o.y == 200 );  /* the button goes down at the start */
    assert( touch_loupe_step( &t, 1, 150, 210, 46 * MS, DELAY, &o ) && o.left && o.x == 150 && o.y == 210 );
    assert( touch_loupe_step( &t, 1, 150, 210, 900 * MS, DELAY, &o ) && o.left && !o.loupe );  /* no magnifier in a drag */
    assert( !touch_loupe_step( &t, 0, 0, 0, 1000 * MS, DELAY, &o ) );  /* the release shows in the button */
}

static void test_a_move_of_exactly_the_slop_is_not_a_drag(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    touch_loupe_step( &t, 1, 100, 200, 0, DELAY, &o );
    assert( touch_loupe_step( &t, 1, 100 + TOUCH_LOUPE_SLOP, 200, 30 * MS, DELAY, &o ) && !o.left );
}

static void test_waiting_shows_the_magnifier_and_the_release_clicks_where_the_finger_is(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    touch_loupe_step( &t, 1, 100, 200, 0, DELAY, &o );
    assert( touch_loupe_step( &t, 1, 101, 200, DELAY - 1, DELAY, &o ) && !o.loupe );
    assert( touch_loupe_step( &t, 1, 101, 200, DELAY, DELAY, &o ) && o.loupe && !o.left );
    /* the finger moves the place, as far as it likes */
    assert( touch_loupe_step( &t, 1, 160, 240, DELAY + 40 * MS, DELAY, &o ) && o.loupe && !o.left );
    assert( o.x == 160 && o.y == 240 );
    assert( touch_loupe_step( &t, 0, 0, 0, DELAY + 80 * MS, DELAY, &o ) );
    assert( o.left && !o.loupe && o.x == 160 && o.y == 240 );
    assert( !touch_loupe_step( &t, 0, 0, 0, DELAY + 100 * MS, DELAY, &o ) );
}

static void test_the_delay_is_the_one_given(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    touch_loupe_step( &t, 1, 100, 200, 0, 1000 * MS, &o );
    assert( touch_loupe_step( &t, 1, 100, 200, 999 * MS, 1000 * MS, &o ) && !o.loupe );
    assert( touch_loupe_step( &t, 1, 100, 200, 1000 * MS, 1000 * MS, &o ) && o.loupe );
    t = (struct touch_loupe){0};
    touch_loupe_step( &t, 1, 100, 200, 0, 200 * MS, &o );
    assert( touch_loupe_step( &t, 1, 100, 200, 200 * MS, 200 * MS, &o ) && o.loupe );
}

static void test_the_first_finger_is_the_one_followed(void)
{
    struct touch_loupe t = {0};
    const unsigned int ids[2] = { 7, 9 };
    const int xs[2] = { 100, 600 }, ys[2] = { 100, 400 };
    int x, y;

    assert( touch_loupe_track( &t, ids, xs, ys, 1, &x, &y ) && x == 100 && y == 100 );
    t.phase = TOUCH_LOUPE_WAIT;  /* a touch is going on */
    assert( touch_loupe_track( &t, ids, xs, ys, 2, &x, &y ) && x == 100 && y == 100 );  /* a second finger changes nothing */
    assert( !touch_loupe_track( &t, ids + 1, xs + 1, ys + 1, 1, &x, &y ) );  /* the first one lifted: not down */
    assert( !touch_loupe_track( &t, ids, xs, ys, 0, &x, &y ) );
    t.phase = TOUCH_LOUPE_IDLE;  /* a new touch takes the first finger that is down */
    assert( touch_loupe_track( &t, ids + 1, xs + 1, ys + 1, 1, &x, &y ) && x == 600 && y == 400 && t.finger == 9 );
}

static void test_the_second_finger_does_not_turn_a_wait_into_a_drag(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;
    const unsigned int ids[2] = { 7, 9 };
    const int xs[2] = { 100, 600 }, ys[2] = { 100, 400 };
    int x = 0, y = 0, down;

    down = touch_loupe_track( &t, ids, xs, ys, 1, &x, &y );
    touch_loupe_step( &t, down, x, y, 0, DELAY, &o );
    down = touch_loupe_track( &t, ids + 1, xs + 1, ys + 1, 1, &x, &y );  /* the first finger lifted, the second is down */
    assert( !down );
    assert( touch_loupe_step( &t, down, x, y, 30 * MS, DELAY, &o ) && o.left && o.x == 100 && o.y == 100 );  /* a tap */
}

static void test_plain_touch_leaves_a_touch_alone_that_the_magnifier_had(void)
{
    int ignore = 0;

    assert( touch_loupe_plain( &ignore, 0, 1 ) && touch_loupe_plain( &ignore, 0, 1 ) );  /* a press while down */
    assert( !touch_loupe_plain( &ignore, 0, 0 ) );
    assert( !touch_loupe_plain( &ignore, 1, 1 ) && !touch_loupe_plain( &ignore, 0, 1 ) );  /* switched off mid-touch */
    assert( !touch_loupe_plain( &ignore, 0, 0 ) );  /* lifted */
    assert( touch_loupe_plain( &ignore, 0, 1 ) );  /* the next touch presses */
}

static void test_a_new_touch_after_each_kind_starts_again(void)
{
    struct touch_loupe t = {0};
    struct touch_loupe_output o;

    touch_loupe_step( &t, 1, 10, 10, 0, DELAY, &o );
    touch_loupe_step( &t, 1, 90, 90, 10 * MS, DELAY, &o );  /* a drag */
    touch_loupe_step( &t, 0, 0, 0, 20 * MS, DELAY, &o );
    assert( touch_loupe_step( &t, 1, 500, 300, 30 * MS, DELAY, &o ) && !o.left && o.x == 500 && o.y == 300 );
    assert( touch_loupe_step( &t, 0, 0, 0, 40 * MS, DELAY, &o ) && o.left && o.x == 500 && o.y == 300 );
}

int main(void)
{
    test_layout_above_the_finger();
    test_layout_below_when_there_is_no_room_above();
    test_layout_stays_on_the_screen_in_every_corner();
    test_layout_follows_the_zoom();
    test_layout_needs_a_screen_that_holds_the_magnifier();
    test_no_finger_says_nothing();
    test_a_tap_clicks_where_it_started_when_it_ends();
    test_a_move_is_a_drag_that_starts_where_the_finger_went_down();
    test_a_move_of_exactly_the_slop_is_not_a_drag();
    test_waiting_shows_the_magnifier_and_the_release_clicks_where_the_finger_is();
    test_the_delay_is_the_one_given();
    test_the_first_finger_is_the_one_followed();
    test_the_second_finger_does_not_turn_a_wait_into_a_drag();
    test_plain_touch_leaves_a_touch_alone_that_the_magnifier_had();
    test_a_new_touch_after_each_kind_starts_again();
    puts( "touch loupe: layout, zooms, corners, small screens, taps, drags, the wait for the magnifier, its delay, the followed finger, the plain touch and new touches passed" );
    return 0;
}
