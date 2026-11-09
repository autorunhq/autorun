/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The floating keyboard (osk.h): a US layout in fifteen units of width and six
 * rows, F1 to F12 on top where a keyboard has them, with the selection moved
 * with the d-pad or the left stick, a key pressed with A or a finger.
 *
 * It sends what a physical keyboard sends. A key is down while it is held --
 * by a finger, or by the button that presses it -- and goes up when that ends,
 * so Shift can be held with one finger while another types, and a key held
 * repeats the way a held key does. Shift and Caps are keys like the others:
 * Caps sends VK_CAPITAL and lets the system keep the lock, and Shift let go
 * quickly stays down for the key after it (sticky keys), which is what a
 * one-button selection needs. The keys leave as virtual-key codes with the
 * scan codes the layout gives them, down and up, so a field a program draws
 * itself can be backspaced, moved through and typed into, and it only gets
 * Enter when Enter is pressed.
 *
 * Its picture is drawn here, in software, with SDL_ttf over the font
 * wine_nx_osk_font gives, and only when something on it changed. The controls
 * panel (wine_nx_osk_map_show) is the same picture made of the same pieces: a
 * list of what the controller sends, which the runtime fills in.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "osk.h"

enum { KEY_CHAR, KEY_ACTION, KEY_SHIFT, KEY_CAPS, KEY_CLOSE, KEY_ARROW };

struct osk_key
{
    const char *label, *shifted;  /* shifted: what it types with Shift, for KEY_CHAR */
    unsigned short vk;
    unsigned char quarters;       /* width in quarters of a key */
    unsigned char kind;
};

#define ROWS 6
#define ROW_UNITS 15

/* A key that types a character, and one that does something else. */
#define C( label, shifted, vk ) { label, shifted, vk, 4, KEY_CHAR }
#define K( label, vk, quarters, kind ) { label, NULL, vk, quarters, kind }

/* The function keys, above the rest as they are on a keyboard: twelve keys of
 * five quarters fill the row exactly. */
static const struct osk_key row_f[] =
{
    K( "F1", 0x70, 5, KEY_ACTION ), K( "F2", 0x71, 5, KEY_ACTION ), K( "F3", 0x72, 5, KEY_ACTION ),
    K( "F4", 0x73, 5, KEY_ACTION ), K( "F5", 0x74, 5, KEY_ACTION ), K( "F6", 0x75, 5, KEY_ACTION ),
    K( "F7", 0x76, 5, KEY_ACTION ), K( "F8", 0x77, 5, KEY_ACTION ), K( "F9", 0x78, 5, KEY_ACTION ),
    K( "F10", 0x79, 5, KEY_ACTION ), K( "F11", 0x7a, 5, KEY_ACTION ), K( "F12", 0x7b, 5, KEY_ACTION ),
    { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key row0[] =
{
    C( "`", "~", 0xc0 ), C( "1", "!", '1' ), C( "2", "@", '2' ), C( "3", "#", '3' ),
    C( "4", "$", '4' ), C( "5", "%", '5' ), C( "6", "^", '6' ), C( "7", "&", '7' ),
    C( "8", "*", '8' ), C( "9", "(", '9' ), C( "0", ")", '0' ), C( "-", "_", 0xbd ),
    C( "=", "+", 0xbb ), K( "Back", 0x08, 8, KEY_ACTION ), { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key row1[] =
{
    K( "Tab", 0x09, 6, KEY_ACTION ),
    C( "q", "Q", 'Q' ), C( "w", "W", 'W' ), C( "e", "E", 'E' ), C( "r", "R", 'R' ),
    C( "t", "T", 'T' ), C( "y", "Y", 'Y' ), C( "u", "U", 'U' ), C( "i", "I", 'I' ),
    C( "o", "O", 'O' ), C( "p", "P", 'P' ), C( "[", "{", 0xdb ), C( "]", "}", 0xdd ),
    { "\\", "|", 0xdc, 6, KEY_CHAR }, { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key row2[] =
{
    K( "Caps", 0, 7, KEY_CAPS ),
    C( "a", "A", 'A' ), C( "s", "S", 'S' ), C( "d", "D", 'D' ), C( "f", "F", 'F' ),
    C( "g", "G", 'G' ), C( "h", "H", 'H' ), C( "j", "J", 'J' ), C( "k", "K", 'K' ),
    C( "l", "L", 'L' ), C( ";", ":", 0xba ), C( "'", "\"", 0xde ),
    K( "Enter", 0x0d, 9, KEY_ACTION ), { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key row3[] =
{
    K( "Shift", 0, 9, KEY_SHIFT ),
    C( "z", "Z", 'Z' ), C( "x", "X", 'X' ), C( "c", "C", 'C' ), C( "v", "V", 'V' ),
    C( "b", "B", 'B' ), C( "n", "N", 'N' ), C( "m", "M", 'M' ), C( ",", "<", 0xbc ),
    C( ".", ">", 0xbe ), C( "/", "?", 0xbf ), K( "Shift", 0, 11, KEY_SHIFT ), { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key row4[] =
{
    K( "Esc", 0x1b, 6, KEY_ACTION ), K( "Space", 0x20, 24, KEY_ACTION ),
    K( "<", 0x25, 4, KEY_ARROW ), K( "v", 0x28, 4, KEY_ARROW ), K( "^", 0x26, 4, KEY_ARROW ),
    K( ">", 0x27, 4, KEY_ARROW ), K( "Del", 0x2e, 6, KEY_ACTION ),
    K( "Close", 0, 8, KEY_CLOSE ), { NULL, NULL, 0, 0, 0 }
};
static const struct osk_key *const rows[ROWS] = { row_f, row0, row1, row2, row3, row4 };

#define VK_SHIFT   0x10
#define VK_CAPITAL 0x14
#define ROW_CELLS  16             /* the longest row's keys, for the tables below */

/* Where the keys the keyboard itself works with are: the two Shifts, and
 * Close, which is not a key. */
#define SHIFT_ROW       4
#define SHIFT_LEFT_COL  0
#define SHIFT_RIGHT_COL 11
#define CLOSE_ROW       5
#define CLOSE_COL       7

#define MIN_HOLD_NS    40000000ull  /* a key stays down at least this long: a program that looks at the keyboard once a frame (DirectInput, GetAsyncKeyState) still sees it */
#define REPEAT_FIRST_NS 500000000ull /* a key held this long is repeated, as Windows repeats its own */
#define REPEAT_NEXT_NS  60000000ull
#define MOVE_FIRST_NS   350000000ull /* the selection, which is quicker than a key */
#define MOVE_NEXT_NS    70000000ull
#define SHIFT_TAP_NS   300000000ull /* Shift let go this quickly stays down for the key after it */
#define STICK_DEAD 16000

static pthread_mutex_t osk_lock = PTHREAD_MUTEX_INITIALIZER;
static int visible, at_top, caps;
static int sel_row = 3, sel_col = 5;   /* g */
static unsigned int ignored, previous, repeating;
static uint64_t repeat_at;
static unsigned int generation = 1;

/* Keys to send, each with when it is due: a key stays down as long as it is
 * held, and a key let go at once stays down MIN_HOLD_NS, so a program that
 * looks at the keyboard once a frame still sees it down. */
static struct { unsigned short vk; unsigned char up; uint64_t due; } queue[256];
static unsigned int queue_head, queue_tail;
static uint64_t input_now, last_due;

/* What holds a key down: the fingers on the screen, the controller's buttons,
 * and Shift and Caps, which stay down once let go for what comes next. */
static int ctrl_a_row = -1, ctrl_a_col = -1;   /* A, on the key the selection is on when it goes down */
static int ctrl_shift;                         /* X, which is Shift */
static unsigned short ctrl_vk[8];              /* B, Y, L, R and +, by the key they hold */
static int ctrl_count;
static int shift_sticky, sticky_row, sticky_col;  /* Shift down for the next key */
static int shift_used;                            /* a key went down while Shift was */
static int shift_keep;     /* this press of Shift lets go of what a tap left down */
static int ignore_touch;   /* fingers that were down when it opened press nothing */
static unsigned char touch_cell[ROWS][ROW_CELLS];  /* cells a finger is on */
static unsigned char cell_down[ROWS][ROW_CELLS];   /* cells the system has down */
static unsigned char cell_repeat[ROWS][ROW_CELLS];
static uint64_t cell_down_at[ROWS][ROW_CELLS], cell_repeat_at[ROWS][ROW_CELLS];

/* The picture last drawn, for which screen and which generation: of the
 * keyboard or of the controls panel. */
static uint32_t *picture;
static int picture_screen_w, picture_screen_h, picture_w, picture_h;
static unsigned int picture_generation;
static int picture_map;

/* The controls panel: up or not, and its rows, copied in so the caller's
 * strings need not outlive wine_nx_osk_map_show. */
#define MAP_ROWS_MAX  24
#define MAP_NAME_MAX  28
#define MAP_VALUE_MAX 56
#define MAP_TITLE_MAX 64
static int map_visible, map_count;
static char map_title[MAP_TITLE_MAX];
static char map_names[MAP_ROWS_MAX][MAP_NAME_MAX], map_values[MAP_ROWS_MAX][MAP_VALUE_MAX];

static int row_length( int row )
{
    int n = 0;
    while (rows[row][n].label) n++;
    return n;
}

static int is_letter( const struct osk_key *key )
{
    return key->kind == KEY_CHAR && key->vk >= 'A' && key->vk <= 'Z';
}

/* Sizes, in pixels, for a screen that tall: everything scales with 720. */
struct geometry
{
    float unit, key_h, gap, pad, hint;
    int width, height, x, y;
};

static void layout( int screen_w, int screen_h, int top, struct geometry *g )
{
    float s = screen_h / 720.0f;

    if (screen_w * 720 < screen_h * 1000) s = screen_w / 1000.0f;  /* narrower than the panel */
    g->unit = floorf( 64 * s );
    g->key_h = floorf( 50 * s );
    g->gap = floorf( 6 * s ) < 2 ? 2 : floorf( 6 * s );
    g->pad = floorf( 12 * s );
    g->hint = floorf( 30 * s );
    g->width = (int)(ROW_UNITS * g->unit + 2 * g->pad);
    g->height = (int)(g->hint + ROWS * g->key_h + (ROWS - 1) * g->gap + 2 * g->pad);
    g->x = (screen_w - g->width) / 2;
    g->y = top ? (int)g->pad : screen_h - g->height - (int)g->pad;
    if (g->y < 0) g->y = 0;
}

/* A key's rectangle inside the panel. */
static void key_rect( const struct geometry *g, int row, int col, int *x, int *y, int *w, int *h )
{
    int quarters = 0, i;

    for (i = 0; i < col; i++) quarters += rows[row][i].quarters;
    *x = (int)(g->pad + quarters * g->unit / 4 + g->gap / 2);
    *w = (int)(rows[row][col].quarters * g->unit / 4 - g->gap);
    *y = (int)(g->pad + g->hint + row * (g->key_h + g->gap));
    *h = (int)g->key_h;
}

static float key_center( int row, int col )
{
    int quarters = 0, i;

    for (i = 0; i < col; i++) quarters += rows[row][i].quarters;
    return quarters + rows[row][col].quarters / 2.0f;
}

/* Which key the system is sent for a key of the picture: Shift and Caps have
 * their own codes, and Close is not a key at all. */
static unsigned short key_vk( const struct osk_key *key )
{
    switch (key->kind)
    {
    case KEY_SHIFT: return VK_SHIFT;
    case KEY_CAPS:  return VK_CAPITAL;
    default:        return key->vk;
    }
}

/* Keys Windows repeats while they are held: Shift and Caps are not. */
static int repeatable( const struct osk_key *key )
{
    return key->kind == KEY_CHAR || key->kind == KEY_ACTION || key->kind == KEY_ARROW;
}

/* delay: after the event before it, or now when the queue has gone quiet.
 * Returns when the event is due, which the repeats count from. */
static uint64_t push( unsigned short vk, int up, uint64_t delay )
{
    unsigned int slot = queue_tail % (sizeof(queue) / sizeof(queue[0]));

    last_due = last_due + delay > input_now ? last_due + delay : input_now;
    if (queue_tail - queue_head >= sizeof(queue) / sizeof(queue[0])) return last_due;
    queue[slot].vk = vk;
    queue[slot].up = !!up;
    queue[slot].due = last_due;
    queue_tail++;
    return last_due;
}

static int shift_cell( int row, int col )
{
    return rows[row][col].kind == KEY_SHIFT;
}

/* Shift and Caps, which change the keys after them. */
static int modifier_cell( int row, int col )
{
    return rows[row][col].kind == KEY_SHIFT || rows[row][col].kind == KEY_CAPS;
}

/* Shift is down: held by a finger or the controller, or left down by a tap. */
static int shift_down( void )
{
    return cell_down[SHIFT_ROW][SHIFT_LEFT_COL] || cell_down[SHIFT_ROW][SHIFT_RIGHT_COL];
}

/* A key goes down, and stays down while its button or finger does. */
static void cell_press( int row, int col );
/* Every key up, for the keyboard closing. */
static void release_all( void );

/* A key let go: it goes up, but not before MIN_HOLD_NS, so a program that
 * looks at the keyboard once a frame still sees it down. Shift let go quickly
 * and with nothing typed stays down for the key after it. */
static void cell_release( int row, int col )
{
    const struct osk_key *key = &rows[row][col];
    int shift = shift_cell( row, col );

    if (!cell_down[row][col]) return;
    if (shift)
    {
        int tap = !shift_used && !shift_keep && input_now - cell_down_at[row][col] < SHIFT_TAP_NS;

        shift_keep = 0;
        if (tap)
        {
            shift_sticky = 1;
            sticky_row = row;
            sticky_col = col;
            generation++;
            return;
        }
    }
    cell_down[row][col] = 0;
    cell_repeat[row][col] = 0;
    push( key_vk( key ), 1, MIN_HOLD_NS );
    /* Shift that was down for this key goes up after it. */
    if (shift_sticky && repeatable( key )) shift_sticky = 0;
    generation++;
}

static void cell_press( int row, int col )
{
    const struct osk_key *key = &rows[row][col];
    uint64_t due;

    if (cell_down[row][col]) return;
    cell_down[row][col] = 1;
    if (shift_cell( row, col )) shift_used = 0;
    else if (repeatable( key )) shift_used = 1;
    if (key->kind == KEY_CAPS) caps = !caps;
    due = push( key_vk( key ), 0, 0 );
    cell_down_at[row][col] = input_now;   /* when it was held, not when the event is due */
    cell_repeat[row][col] = repeatable( key );
    cell_repeat_at[row][col] = due + REPEAT_FIRST_NS;
    generation++;
}

/* Whether a key should be down: a finger on it, the controller's button, or
 * Shift left down by a tap. */
static int cell_wanted( int row, int col )
{
    const struct osk_key *key = &rows[row][col];
    int i;

    if (touch_cell[row][col]) return 1;
    if (row == ctrl_a_row && col == ctrl_a_col) return 1;
    if (key->kind == KEY_CLOSE) return 0;
    if (key->kind == KEY_SHIFT)
        return (ctrl_shift && row == SHIFT_ROW && col == SHIFT_LEFT_COL) ||
               (shift_sticky && row == sticky_row && col == sticky_col);
    for (i = 0; i < ctrl_count; i++)
        if (ctrl_vk[i] == key->vk) return 1;
    return 0;
}

/* Brings the keys in line with what holds them, and repeats those held: the
 * key stays down and another down goes out, which the system marks as a
 * repeat the way it marks one from a keyboard. */
static void sync_cells( void )
{
    int row, col;

    /* Close is not a key: pressing it closes the keyboard, with everything
     * held let go first. */
    if (touch_cell[CLOSE_ROW][CLOSE_COL] || (ctrl_a_row == CLOSE_ROW && ctrl_a_col == CLOSE_COL))
    {
        release_all();
        visible = 0;
        return;
    }

    /* Shift was left down by a tap and is pressed again: that lets it go. */
    if (shift_sticky && (touch_cell[sticky_row][sticky_col] ||
                         (ctrl_shift && sticky_row == SHIFT_ROW && sticky_col == SHIFT_LEFT_COL)))
    {
        shift_sticky = 0;   /* pressed again: it is let go, and stays let go */
        shift_keep = 1;
    }

    /* Shift and Caps go down before the keys they change, and up after them,
     * as a hand holds them. */
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (cell_wanted( row, col ) && !cell_down[row][col] && modifier_cell( row, col ))
                cell_press( row, col );
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (cell_wanted( row, col ) && !cell_down[row][col]) cell_press( row, col );
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (!cell_wanted( row, col ) && cell_down[row][col] && !modifier_cell( row, col ))
                cell_release( row, col );
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (!cell_wanted( row, col ) && cell_down[row][col]) cell_release( row, col );
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (cell_down[row][col] && cell_repeat[row][col] && input_now >= cell_repeat_at[row][col])
            {
                push( key_vk( &rows[row][col] ), 0, 0 );
                cell_repeat_at[row][col] = input_now + REPEAT_NEXT_NS;
            }
}

/* Every key up, for the keyboard closing: nothing is left down behind it. */
static void release_all( void )
{
    int row, col;

    for (row = 0; row < ROWS; row++)
        for (col = 0; col < row_length( row ); col++)
            if (cell_down[row][col])
            {
                cell_down[row][col] = 0;
                cell_repeat[row][col] = 0;
                push( key_vk( &rows[row][col] ), 1, MIN_HOLD_NS );
            }
    memset( touch_cell, 0, sizeof(touch_cell) );
    ctrl_count = 0;
    ctrl_a_row = ctrl_a_col = -1;
    ctrl_shift = 0;
    shift_sticky = 0;
    shift_used = 0;
    generation++;
}

/* Up and down go to the key nearest above or below, left and right wrap. */
static void move( unsigned int direction )
{
    if (direction & (OSK_UP | OSK_DOWN))
    {
        float center = key_center( sel_row, sel_col ), best = 1e9f;
        int row = (sel_row + (direction & OSK_UP ? ROWS - 1 : 1)) % ROWS, col, n = row_length( row );

        for (col = 0; col < n; col++)
        {
            float d = fabsf( key_center( row, col ) - center );
            if (d < best) { best = d; sel_col = col; }
        }
        sel_row = row;
    }
    else
    {
        int n = row_length( sel_row );
        sel_col = (sel_col + (direction & OSK_LEFT ? n - 1 : 1)) % n;
    }
    generation++;
}

int wine_nx_osk_visible( void )
{
    return __atomic_load_n( &visible, __ATOMIC_RELAXED );
}

unsigned int wine_nx_osk_generation( void )
{
    return __atomic_load_n( &generation, __ATOMIC_RELAXED );
}

void wine_nx_osk_map_show( int show, const char *title, const struct wine_nx_osk_map_row *rows, int count )
{
    int i;

    pthread_mutex_lock( &osk_lock );
    if (show)
    {
        if (count < 0) count = 0;
        if (count > MAP_ROWS_MAX) count = MAP_ROWS_MAX;
        snprintf( map_title, sizeof(map_title), "%s", title ? title : "" );
        for (i = 0; i < count; i++)
        {
            snprintf( map_names[i], sizeof(map_names[0]), "%s", rows[i].name ? rows[i].name : "" );
            snprintf( map_values[i], sizeof(map_values[0]), "%s", rows[i].value ? rows[i].value : "" );
        }
        map_count = count;
    }
    if (map_visible != !!show)
    {
        map_visible = !!show;
        generation++;
    }
    pthread_mutex_unlock( &osk_lock );
}

int wine_nx_osk_map_visible( void )
{
    return __atomic_load_n( &map_visible, __ATOMIC_RELAXED );
}

void wine_nx_osk_show( int show, unsigned int held )
{
    pthread_mutex_lock( &osk_lock );
    /* One of them at a time: the keyboard is what is worked with, the panel
     * what is read. */
    if (show && map_visible)
    {
        map_visible = 0;
        generation++;
    }
    if (visible != !!show)
    {
        if (!show) release_all();
        visible = !!show;
        ignored = held;
        ignore_touch = !!show;
        previous = 0;
        repeating = 0;
        memset( touch_cell, 0, sizeof(touch_cell) );
        generation++;
    }
    pthread_mutex_unlock( &osk_lock );
}

/* The cells under the fingers, and whether any finger is on the keyboard: the
 * rest of the screen is then the program's, as before. */
static int touch_update( const struct wine_nx_osk_point *points, int count )
{
    struct geometry g;
    unsigned char now_cell[ROWS][ROW_CELLS];
    int i, row, col, n, x, y, w, h, on_panel = 0;

    memset( now_cell, 0, sizeof(now_cell) );
    layout( 1280, 720, at_top, &g );
    for (i = 0; i < count; i++)
    {
        int px = points[i].x - g.x, py = points[i].y - g.y;

        if (px < 0 || py < 0 || px >= g.width || py >= g.height) continue;
        on_panel = 1;
        for (row = 0; row < ROWS; row++)
            for (col = 0, n = row_length( row ); col < n; col++)
            {
                key_rect( &g, row, col, &x, &y, &w, &h );
                if (px >= x - g.gap / 2 && px < x + w + g.gap / 2 && py >= y - g.gap / 2 &&
                    py < y + h + g.gap / 2)
                {
                    if (!touch_cell[row][col])
                    {
                        sel_row = row;
                        sel_col = col;
                    }
                    now_cell[row][col] = 1;
                }
            }
    }
    /* A finger that was down when it opened presses nothing until it lifts,
     * as a button held when it opened does. */
    if (ignore_touch)
    {
        if (!count) ignore_touch = 0;
        memset( now_cell, 0, sizeof(now_cell) );
    }
    memcpy( touch_cell, now_cell, sizeof(touch_cell) );
    return on_panel;
}

/* The controller's buttons that hold a key, by that key's code. */
static const struct { unsigned int button; unsigned short vk; } holds[] =
{
    { OSK_B, 0x08 }, { OSK_Y, 0x20 }, { OSK_L, 0x25 }, { OSK_R, 0x27 }, { OSK_PLUS, 0x0d },
};

int wine_nx_osk_input( unsigned int held, int stick_x, int stick_y,
                       const struct wine_nx_osk_point *points, int count, uint64_t now_ns )
{
    unsigned int pressed, bit;
    int i, on_panel;

    pthread_mutex_lock( &osk_lock );
    if (!visible)
    {
        pthread_mutex_unlock( &osk_lock );
        return 0;
    }
    input_now = now_ns;
    if (stick_y > STICK_DEAD) held |= OSK_UP;
    if (stick_y < -STICK_DEAD) held |= OSK_DOWN;
    if (stick_x < -STICK_DEAD) held |= OSK_LEFT;
    if (stick_x > STICK_DEAD) held |= OSK_RIGHT;
    ignored &= held;
    held &= ~ignored;
    pressed = held & ~previous;
    previous = held;

    /* The selection moves with the d-pad and the stick, and repeats held. */
    for (bit = OSK_UP; bit <= OSK_RIGHT; bit <<= 1)
        if (pressed & bit)
        {
            move( bit );
            repeating = bit;
            repeat_at = now_ns + MOVE_FIRST_NS;
        }
    if (repeating && !(held & repeating)) repeating = 0;
    if (visible && repeating && now_ns >= repeat_at)
    {
        move( repeating );
        repeat_at = now_ns + MOVE_NEXT_NS;
    }

    /* A holds the key the selection was on when it went down; X is Shift,
     * held while it is, and the rest hold their key while pressed. */
    if (pressed & OSK_A)
    {
        ctrl_a_row = sel_row;
        ctrl_a_col = sel_col;
    }
    if (!(held & OSK_A)) ctrl_a_row = ctrl_a_col = -1;
    ctrl_shift = (held & OSK_X) != 0;
    for (i = ctrl_count = 0; i < (int)(sizeof(holds) / sizeof(holds[0])); i++)
        if (held & holds[i].button) ctrl_vk[ctrl_count++] = holds[i].vk;

    on_panel = touch_update( points, count );
    sync_cells();

    if (visible && (pressed & OSK_MINUS))
    {
        release_all();
        visible = 0;
    }
    if (visible && (pressed & OSK_ZL) && !at_top) { at_top = 1; generation++; }
    if (visible && (pressed & OSK_ZR) && at_top) { at_top = 0; generation++; }
    if (!visible) on_panel = 0;
    pthread_mutex_unlock( &osk_lock );
    return on_panel;
}

int wine_nx_osk_next_key( uint64_t now_ns, unsigned short *vk, int *up )
{
    int found = 0;

    pthread_mutex_lock( &osk_lock );
    if (queue_head != queue_tail && queue[queue_head % (sizeof(queue) / sizeof(queue[0]))].due <= now_ns)
    {
        *vk = queue[queue_head % (sizeof(queue) / sizeof(queue[0]))].vk;
        *up = queue[queue_head % (sizeof(queue) / sizeof(queue[0]))].up;
        queue_head++;
        found = 1;
    }
    pthread_mutex_unlock( &osk_lock );
    return found;
}

/* Drawing. */

#define PANEL    0xff15171cu
#define BORDER   0xff3a3f4bu
#define KEY      0xff2e323bu
#define SPECIAL  0xff23262du
#define SELECTED 0xff1e88e5u
#define ACTIVE   0xff3d5a80u
#define TEXT     0xfff2f4f8u
#define DIM      0xff9aa3b2u

static TTF_Font *fonts[2];
static int font_sizes[2], font_failed;
static struct { char text[56]; int size; SDL_Surface *surface; } labels[256];
static int label_count;

static TTF_Font *font_at( int which, int size )
{
    const void *data;
    size_t bytes;

    if (fonts[which] && font_sizes[which] == size) return fonts[which];
    if (font_failed) return NULL;
    if (!TTF_WasInit() && TTF_Init()) { font_failed = 1; return NULL; }
    if (!wine_nx_osk_font( &data, &bytes )) { font_failed = 1; return NULL; }
    if (fonts[which]) TTF_CloseFont( fonts[which] );
    fonts[which] = TTF_OpenFontRW( SDL_RWFromConstMem( data, (int)bytes ), 1, size );
    font_sizes[which] = size;
    return fonts[which];
}

/* White text, drawn once for each size it is asked for. */
static SDL_Surface *label_surface( int which, int size, const char *text )
{
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *drawn, *converted;
    TTF_Font *font;
    int i;

    for (i = 0; i < label_count; i++)
        if (labels[i].size == size * 2 + which && !strcmp( labels[i].text, text )) return labels[i].surface;
    if (!(font = font_at( which, size )) || !(drawn = TTF_RenderUTF8_Blended( font, text, white ))) return NULL;
    converted = SDL_ConvertSurfaceFormat( drawn, SDL_PIXELFORMAT_ARGB8888, 0 );
    SDL_FreeSurface( drawn );
    if (!converted) return NULL;
    if (label_count == sizeof(labels) / sizeof(labels[0]))
    {
        for (i = 0; i < label_count; i++) SDL_FreeSurface( labels[i].surface );
        label_count = 0;
    }
    snprintf( labels[label_count].text, sizeof(labels[0].text), "%s", text );
    labels[label_count].size = size * 2 + which;
    labels[label_count].surface = converted;
    label_count++;
    return converted;
}

static uint32_t blend( uint32_t under, uint32_t color, unsigned int alpha )
{
    uint32_t r = ((under >> 16 & 0xff) * (255 - alpha) + (color >> 16 & 0xff) * alpha) / 255;
    uint32_t g = ((under >> 8 & 0xff) * (255 - alpha) + (color >> 8 & 0xff) * alpha) / 255;
    uint32_t b = ((under & 0xff) * (255 - alpha) + (color & 0xff) * alpha) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static void fill_rounded( int x0, int y0, int w, int h, int radius, uint32_t color )
{
    int x, y;

    for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < picture_h; y++)
        for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < picture_w; x++)
        {
            int dx = x < x0 + radius ? x0 + radius - x : x >= x0 + w - radius ? x - (x0 + w - radius - 1) : 0;
            int dy = y < y0 + radius ? y0 + radius - y : y >= y0 + h - radius ? y - (y0 + h - radius - 1) : 0;
            float d = sqrtf( (float)(dx * dx + dy * dy) );

            if (d <= radius - 0.5f) picture[y * picture_w + x] = color;
            else if (d < radius + 0.5f)
                picture[y * picture_w + x] = blend( picture[y * picture_w + x], color,
                                                    (unsigned int)((radius + 0.5f - d) * 255) );
        }
}

/* Text at x -- its left edge, its middle or its right edge, by align -- and
 * centred on cy, in the picture. */
static void text_at( int which, int size, const char *text, int x, int cy, int align, uint32_t color )
{
    SDL_Surface *s = label_surface( which, size, text );
    int dx, dy, px, py;

    if (!s) return;
    dx = x - (align < 0 ? 0 : align > 0 ? s->w : s->w / 2);
    dy = cy - s->h / 2;
    for (py = 0; py < s->h; py++)
        for (px = 0; px < s->w; px++)
        {
            uint32_t p = ((uint32_t *)((uint8_t *)s->pixels + py * s->pitch))[px];
            int to_x = dx + px, to_y = dy + py;

            if ((p >> 24) && to_x >= 0 && to_y >= 0 && to_x < picture_w && to_y < picture_h)
                picture[to_y * picture_w + to_x] =
                    blend( picture[to_y * picture_w + to_x], color, p >> 24 );
        }
}

static void text_centered( int which, int size, const char *text, int cx, int cy, uint32_t color )
{
    text_at( which, size, text, cx, cy, 0, color );
}

/* A triangle pointing the way an arrow key moves, filled with its edges soft. */
static void arrow( int cx, int cy, int size, unsigned short vk, uint32_t color )
{
    int x, y;

    for (y = -size; y <= size; y++)
        for (x = -size; x <= size; x++)
        {
            /* along: how far toward the point, across: distance from the axis */
            float along = vk == 0x25 ? -x : vk == 0x27 ? x : vk == 0x26 ? -y : y;
            float across = fabsf( (float)(vk == 0x25 || vk == 0x27 ? y : x) );
            float inside = (size - along) * 0.6f - across;  /* > 0 within the triangle */

            if (along < -size * 0.6f || inside <= -0.5f) continue;
            if (cx + x < 0 || cy + y < 0 || cx + x >= picture_w || cy + y >= picture_h) continue;
            picture[(cy + y) * picture_w + cx + x] =
                inside >= 0.5f ? color : blend( picture[(cy + y) * picture_w + cx + x], color,
                                                (unsigned int)((inside + 0.5f) * 255) );
        }
}

static void draw( int screen_w, int screen_h )
{
    struct geometry g;
    int row, col, n, x, y, w, h, size = 0, small;

    layout( screen_w, screen_h, at_top, &g );
    if (picture_w != g.width || picture_h != g.height || !picture)
    {
        free( picture );
        picture_w = g.width;
        picture_h = g.height;
        picture = malloc( (size_t)picture_w * picture_h * 4 );
    }
    if (!picture) return;
    for (x = 0; x < picture_w * picture_h; x++) picture[x] = BORDER;
    for (y = 1; y < picture_h - 1; y++)
        for (x = 1; x < picture_w - 1; x++) picture[y * picture_w + x] = PANEL;

    size = (int)(g.key_h * 0.42f);
    small = (int)(g.hint * 0.5f);
    text_centered( 1, small, "A type   B back   Y space   X shift   L R cursor   + enter   ZL ZR move   - close",
                   picture_w / 2, (int)(g.pad + g.hint / 2 - 2), DIM );
    for (row = 0; row < ROWS; row++)
        for (col = 0, n = row_length( row ); col < n; col++)
        {
            const struct osk_key *key = &rows[row][col];
            uint32_t color = key->kind == KEY_CHAR ? KEY : SPECIAL;
            const char *label = key->label;

            /* Held keys are lit, as Shift and Caps are while they stay down. */
            if (cell_down[row][col] || (key->kind == KEY_CAPS && caps)) color = ACTIVE;
            if (row == sel_row && col == sel_col) color = SELECTED;
            key_rect( &g, row, col, &x, &y, &w, &h );
            fill_rounded( x, y, w, h, (int)(g.key_h / 8), color );
            if (key->kind == KEY_CHAR && (is_letter( key ) ? shift_down() ^ caps : shift_down())) label = key->shifted;
            if (key->kind == KEY_ARROW) arrow( x + w / 2, y + h / 2, (int)(g.key_h / 5), key->vk, TEXT );
            else text_centered( 0, key->kind == KEY_CHAR ? size : (int)(size * 0.8f), label, x + w / 2, y + h / 2, TEXT );
        }
}

/* Where the controls panel goes on a screen that size: two columns of rows, a
 * title and the line that says what closes it, everything scaling with 720. */
struct map_geometry
{
    float s, pad, row_h, hint;
    int width, height, x, y, columns, rows_per_column, column_width;
};

static void map_layout( int screen_w, int screen_h, struct map_geometry *g )
{
    float s = screen_h / 720.0f;
    int title_h, margin;

    if (screen_w * 720 < screen_h * 1000) s = screen_w / 1000.0f;
    g->s = s;
    g->pad = floorf( 18 * s );
    g->row_h = floorf( 32 * s );
    g->hint = floorf( 26 * s );
    title_h = (int)(g->row_h * 1.4f);
    margin = screen_w / 20;
    g->columns = map_count > 12 ? 2 : 1;
    g->rows_per_column = (map_count + g->columns - 1) / g->columns;
    if (g->rows_per_column < 1) g->rows_per_column = 1;
    g->width = (int)(g->columns * 470 * s) + (int)(2 * g->pad);
    if (g->width > screen_w - 2 * margin) g->width = screen_w - 2 * margin;
    g->column_width = (g->width - (int)(2 * g->pad)) / g->columns;
    g->height = (int)(title_h + g->rows_per_column * g->row_h + g->hint + 2 * g->pad);
    if (g->height > screen_h) g->height = screen_h;
    g->x = (screen_w - g->width) / 2;
    g->y = (int)g->pad;
    if (g->y < 0) g->y = 0;
}

static void draw_map( int screen_w, int screen_h )
{
    struct map_geometry g;
    int i, x, y, size, small, title;

    map_layout( screen_w, screen_h, &g );
    if (picture_w != g.width || picture_h != g.height || !picture)
    {
        free( picture );
        picture_w = g.width;
        picture_h = g.height;
        picture = malloc( (size_t)picture_w * picture_h * 4 );
    }
    if (!picture) return;
    for (x = 0; x < picture_w * picture_h; x++) picture[x] = BORDER;
    for (y = 1; y < picture_h - 1; y++)
        for (x = 1; x < picture_w - 1; x++) picture[y * picture_w + x] = PANEL;

    size = (int)(g.row_h * 0.52f);
    small = (int)(g.hint * 0.55f);
    title = (int)(g.row_h * 0.72f);
    text_at( 1, title, map_title, (int)g.pad, (int)(g.pad + g.row_h * 0.75f), -1, TEXT );
    for (i = 0; i < map_count; i++)
    {
        int column = i / g.rows_per_column, row = i % g.rows_per_column;
        int left = (int)g.pad + column * g.column_width;
        int cy = (int)(g.pad + g.row_h * 1.75f + row * g.row_h);

        text_at( 0, size, map_names[i], left, cy, -1, DIM );
        text_at( 0, size, map_values[i], left + g.column_width - (int)g.pad, cy, 1, TEXT );
    }
    text_at( 1, small, "- + A closes   - + right stick click: the keyboard   + and - held: close the program",
             picture_w / 2, picture_h - (int)(g.pad / 2 + g.hint / 2), 0, DIM );
}

/* Draws the picture for that screen if what is there is not it: the keyboard,
 * or the controls panel. */
static void ensure_picture( int screen_w, int screen_h )
{
    if (picture && picture_generation == generation && picture_map == map_visible &&
        picture_screen_w == screen_w && picture_screen_h == screen_h)
        return;
    if (picture_screen_w != screen_w || picture_screen_h != screen_h)
    {
        /* Another screen size is another picture: the presenters that copied
         * the old one see a new generation. */
        if (picture_screen_w) generation++;
        picture_screen_w = screen_w;
        picture_screen_h = screen_h;
    }
    if (map_visible) draw_map( screen_w, screen_h );
    else draw( screen_w, screen_h );
    picture_map = map_visible;
    picture_generation = generation;
}

int wine_nx_osk_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame )
{
    struct geometry g;
    struct map_geometry map;

    pthread_mutex_lock( &osk_lock );
    if (!visible && !map_visible)
    {
        pthread_mutex_unlock( &osk_lock );
        return 0;
    }
    ensure_picture( screen_width, screen_height );
    if (map_visible)
    {
        map_layout( screen_width, screen_height, &map );
        frame->x = map.x;
        frame->y = map.y;
        frame->width = map.width;
        frame->height = map.height;
    }
    else
    {
        layout( screen_width, screen_height, at_top, &g );
        frame->x = g.x;
        frame->y = g.y;
        frame->width = g.width;
        frame->height = g.height;
    }
    frame->generation = generation;
    pthread_mutex_unlock( &osk_lock );
    return 1;
}

unsigned int wine_nx_osk_copy( int screen_width, int screen_height, void *pixels, int stride, int rgba )
{
    unsigned int copied = 0;
    int x, y;

    pthread_mutex_lock( &osk_lock );
    if (visible || map_visible)
    {
        ensure_picture( screen_width, screen_height );
        for (y = 0; y < picture_h; y++)
        {
            uint32_t *out = (uint32_t *)((uint8_t *)pixels + (size_t)y * stride);
            const uint32_t *in = picture + (size_t)y * picture_w;

            if (!rgba) memcpy( out, in, picture_w * 4 );
            else
                for (x = 0; x < picture_w; x++)
                    out[x] = (in[x] & 0xff00ff00u) | (in[x] >> 16 & 0xff) | (in[x] & 0xff) << 16;
        }
        copied = generation;
    }
    pthread_mutex_unlock( &osk_lock );
    return copied;
}

void wine_nx_osk_reset( void )
{
    pthread_mutex_lock( &osk_lock );
    visible = at_top = map_visible = map_count = 0;
    release_all();
    caps = 0;
    shift_keep = 0;
    ignore_touch = 0;
    sel_row = 3;
    sel_col = 5;
    ignored = previous = repeating = 0;
    repeat_at = 0;
    memset( cell_down_at, 0, sizeof(cell_down_at) );
    memset( cell_repeat_at, 0, sizeof(cell_repeat_at) );
    queue_head = queue_tail = 0;
    input_now = last_due = 0;
    generation++;
    pthread_mutex_unlock( &osk_lock );
}
