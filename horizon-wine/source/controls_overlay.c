/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The controls overlay (controls_overlay.h): the controls editor over a
 * running program, worked with the controller or the touchscreen. It is the
 * launcher's editor in small: the controller's own inputs down the left, what
 * each does on the right, and a row opened into the thing that should do it.
 *
 * Keyboard/mouse mode picks keys and mouse actions from a grid of the ones
 * games use; controller mode captures what is pressed. A change is written to
 * the program's controls at once (the same file the launcher edits, over the
 * game's recommended layout and the global controls), so every row can be
 * tried against the game the moment it is closed.
 *
 * Its picture is drawn here, in software, with SDL_ttf over the font
 * wine_nx_osk_font gives (osk.c), and only when something on it changed.
 * osk.c hands its frame and copy calls over to this while the overlay is up,
 * so every presenter that draws the floating keyboard draws this too.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "controls_overlay.h"
#include "osk.h"

/* The runtime's copy of the profile the game runs on (runtime.c): every save
 * below hands the whole thing over at once, so a remap takes effect in the
 * game the moment it is made, not at the next launch. Weak, so the host tests
 * can link this file without the runtime. */
extern void wine_nx_input_profile_publish( const struct input_profile *profile ) __attribute__((weak));

#define LIST_ROWS 8
#define PICK_COLS 6
#define PICK_ROWS 5
/* The row under the bindings: the mouse speed, which is not a binding. */
#define SPEED_ROW INPUT_SOURCE_COUNT
#define STICK_DEAD 16000
#define REPEAT_FIRST_NS 350000000ull
#define REPEAT_NEXT_NS    70000000ull

/* Generations handed out to presenters have their top bit set: the keyboard's
 * never do, so two overlays sharing one presenter cannot share a number. */
#define GENERATION_BIT 0x80000000u

enum { MODE_LIST, MODE_PICK, MODE_CAPTURE, MODE_SPEED };

/* What the grid offers: the mouse actions, then the keys games bind. */
static const struct { unsigned short code; const char *label; } pick_targets[] =
{
    { INPUT_MOUSE_LEFT, "Left click" }, { INPUT_MOUSE_RIGHT, "Right click" },
    { INPUT_MOUSE_MIDDLE, "Middle" }, { INPUT_MOUSE_X1, "Mouse back" },
    { INPUT_MOUSE_X2, "Mouse fwd" }, { INPUT_WHEEL_UP, "Wheel up" },
    { INPUT_WHEEL_DOWN, "Wheel down" }, { INPUT_WHEEL_LEFT, "Wheel left" },
    { INPUT_WHEEL_RIGHT, "Wheel right" }, { INPUT_MOUSE_UP, "Mouse up" },
    { INPUT_MOUSE_DOWN, "Mouse down" }, { INPUT_MOUSE_MOVE_LEFT, "Mouse left" },
    { INPUT_MOUSE_MOVE_RIGHT, "Mouse right" },
    { 0x1b, "Esc" }, { 0x0d, "Enter" }, { 0x20, "Space" }, { 0x09, "Tab" },
    { 0x08, "Back" }, { 0x2e, "Del" }, { 0x10, "Shift" }, { 0x11, "Ctrl" },
    { 0x12, "Alt" }, { 0x5b, "Win" }, { 0x26, "Up" }, { 0x28, "Down" },
    { 0x25, "Left" }, { 0x27, "Right" }, { 0xc0, "`" }, { 0xbd, "-" },
    { 0xbb, "=" }, { 0xdb, "[" }, { 0xdd, "]" }, { 0xdc, "\\" },
    { 0xba, ";" }, { 0xde, "'" }, { 0xbc, "," }, { 0xbe, "." }, { 0xbf, "/" },
    { '1', "1" }, { '2', "2" }, { '3', "3" }, { '4', "4" }, { '5', "5" },
    { '6', "6" }, { '7', "7" }, { '8', "8" }, { '9', "9" }, { '0', "0" },
    { 'A', "A" }, { 'B', "B" }, { 'C', "C" }, { 'D', "D" }, { 'E', "E" },
    { 'F', "F" }, { 'G', "G" }, { 'H', "H" }, { 'I', "I" }, { 'J', "J" },
    { 'K', "K" }, { 'L', "L" }, { 'M', "M" }, { 'N', "N" }, { 'O', "O" },
    { 'P', "P" }, { 'Q', "Q" }, { 'R', "R" }, { 'S', "S" }, { 'T', "T" },
    { 'U', "U" }, { 'V', "V" }, { 'W', "W" }, { 'X', "X" }, { 'Y', "Y" },
    { 'Z', "Z" },
    { 0x70, "F1" }, { 0x71, "F2" }, { 0x72, "F3" }, { 0x73, "F4" },
    { 0x74, "F5" }, { 0x75, "F6" }, { 0x76, "F7" }, { 0x77, "F8" },
    { 0x78, "F9" }, { 0x79, "F10" }, { 0x7a, "F11" }, { 0x7b, "F12" },
};
#define PICK_COUNT ((int)(sizeof(pick_targets) / sizeof(pick_targets[0])))

/* A pressed button's row (input_sources' order), by the OSK_* bit it sets:
 * capture reads "the button you pressed" as the target it should become. */
static const unsigned char button_source[16] =
{
    12, 13, 14, 15,  /* D-pad up, down, left, right */
    0, 1, 2, 3,      /* A, B, X, Y */
    4, 5, 6, 7,      /* L, R, ZL, ZR */
    8, 9, 10, 11,    /* +, -, left stick click, right stick click */
};

static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static int visible, mode;
static struct input_profile profile;
static struct launcher_kv kv;
static char root_dir[256], program[768], save_path[768];
static int selected, list_top, pick, pick_top;
static struct input_binding draft;
static unsigned int ignored, previous, repeating;
static uint64_t repeat_at;
static int capture_last = -1;
static unsigned int generation = 1;
static int touch_down;

/* The picture last drawn, for which screen and which generation. */
static uint32_t *picture;
static int picture_screen_w, picture_screen_h, picture_w, picture_h;
static unsigned int picture_generation;

struct geometry
{
    int pad, hint, header, row_h, gap, width, height, x, y;
};

static void layout( int screen_w, int screen_h, struct geometry *g )
{
    float s = screen_h / 720.0f;

    if (screen_w * 720 < screen_h * 1280) s = screen_w / 1280.0f;
    g->pad = (int)floorf( 10 * s ); if (g->pad < 8) g->pad = 8;
    g->hint = (int)floorf( 32 * s );
    g->header = (int)floorf( 38 * s );
    g->row_h = (int)floorf( 40 * s );
    g->gap = (int)floorf( 5 * s ); if (g->gap < 2) g->gap = 2;
    g->width = (int)floorf( 800 * s );
    g->height = g->header + LIST_ROWS * g->row_h + (LIST_ROWS - 1) * g->gap + g->hint + 2 * g->pad;
    g->x = (screen_w - g->width) / 2;
    g->y = (screen_h - g->height) / 2;
    if (g->y < 0) g->y = 0;
}

static int controller_mapping( void )
{
    return profile.mode != INPUT_KEYBOARD_MOUSE;
}

static struct input_binding *row_binding( int source )
{
    return controller_mapping() ? profile.pad + source : profile.keys + source;
}

/* Shift, Control and Alt in their left-shoes: the grid knows one of each, and
 * a chord the launcher saved with a side's code is the same chord here. */
static unsigned short normalize_code( unsigned short code )
{
    if (code >= 0xa0 && code <= 0xa5) return (unsigned short)(0x10 + (code - 0xa0) / 2);
    return code;
}

static int has_code( const struct input_binding *binding, unsigned short code )
{
    for (unsigned int i = 0; i < binding->count; i++)
        if (normalize_code( binding->code[i] ) == normalize_code( code )) return 1;
    return 0;
}

/* Reload every layer over the file just written: the game's layout and the
 * global controls come back under the change, and a cleared row falls back to
 * whatever they say. */
static int reload( void )
{
    struct input_profile loaded;
    struct launcher_kv loaded_kv;
    char loaded_path[sizeof(save_path)];

    if (program[0] && program[1] != ':')
    {
        if (!input_profile_program( root_dir, program, profile.keyboard_auto, &loaded, &loaded_kv,
                                    loaded_path, sizeof(loaded_path) )) return 0;
    }
    else if (!input_profile_global( root_dir, profile.keyboard_auto, &loaded, &loaded_kv,
                                    loaded_path, sizeof(loaded_path) )) return 0;
    profile = loaded;
    kv = loaded_kv;
    memcpy( save_path, loaded_path, sizeof(save_path) );
    generation++;
    if (wine_nx_input_profile_publish) wine_nx_input_profile_publish( &profile );
    return 1;
}

static void save_row( int source, const struct input_binding *binding )
{
    if (!input_binding_save( &kv, source, controller_mapping(), binding ) ||
        !input_profile_save( &kv, save_path )) return;
    reload();
}

/* The mouse speed, by its stop in input_mouse_speed_values, or -1 for the
 * game's own (the key gone, whatever is under it stands). */
static void save_speed( int index )
{
    char value[16];

    if (index < 0) launcher_kv_set( &kv, "mouse-speed", NULL );
    else
    {
        snprintf( value, sizeof(value), "%d", input_mouse_speed_values[index] );
        if (!launcher_kv_set( &kv, "mouse-speed", value )) return;
    }
    if (input_profile_save( &kv, save_path )) reload();
}

/* Which stop the profile's speed sits on, nearest wins. */
static int speed_choice( int speed )
{
    int best = 0, best_gap = speed > input_mouse_speed_values[0] ?
        speed - input_mouse_speed_values[0] : input_mouse_speed_values[0] - speed;

    for (int i = 1; i < INPUT_MOUSE_SPEED_COUNT; i++)
    {
        int gap = speed > input_mouse_speed_values[i] ?
            speed - input_mouse_speed_values[i] : input_mouse_speed_values[i] - speed;
        if (gap < best_gap) { best_gap = gap; best = i; }
    }
    return best;
}

static void close_overlay( void )
{
    visible = 0;
    mode = MODE_LIST;
    capture_last = -1;
    generation++;
}

int wine_nx_controls_overlay_show( int show, unsigned int held, const char *root, const char *program_path )
{
    int ok = 1;

    if (show)
    {
        /* The keyboard is hidden outside this lock: it may be drawing, with
         * the picture's lock in its hands, the other way round. */
        wine_nx_osk_show( 0, held );
        pthread_mutex_lock( &overlay_lock );
        if (!visible)
        {
            snprintf( root_dir, sizeof(root_dir), "%s", root ? root : "" );
            snprintf( program, sizeof(program), "%s", program_path ? program_path : "" );
            save_path[0] = 0;
            input_profile_defaults( &profile, 1 );
            if (!(ok = reload())) {}
            else
            {
                visible = 1;
                mode = MODE_LIST;
                selected = list_top = 0;
                capture_last = -1;
                ignored = held;
                previous = repeating = 0;
                touch_down = 0;
                generation++;
            }
        }
        pthread_mutex_unlock( &overlay_lock );
    }
    else
    {
        pthread_mutex_lock( &overlay_lock );
        if (visible) close_overlay();
        pthread_mutex_unlock( &overlay_lock );
    }
    return ok;
}

int wine_nx_controls_overlay_visible( void )
{
    return __atomic_load_n( &visible, __ATOMIC_RELAXED );
}

unsigned int wine_nx_controls_overlay_generation( void )
{
    return __atomic_load_n( &generation, __ATOMIC_RELAXED ) | GENERATION_BIT;
}

int wine_nx_controls_overlay_selected( void )
{
    return __atomic_load_n( &selected, __ATOMIC_RELAXED );
}

void wine_nx_controls_overlay_reset( void )
{
    pthread_mutex_lock( &overlay_lock );
    visible = 0;
    mode = MODE_LIST;
    selected = list_top = pick = pick_top = 0;
    ignored = previous = repeating = 0;
    capture_last = -1;
    touch_down = 0;
    generation++;
    pthread_mutex_unlock( &overlay_lock );
}

static void keep_in_view( void )
{
    if (selected < list_top) list_top = selected;
    if (selected >= list_top + LIST_ROWS) list_top = selected - LIST_ROWS + 1;
    if (list_top < 0) list_top = 0;
    if (list_top > SPEED_ROW + 1 - LIST_ROWS) list_top = SPEED_ROW + 1 - LIST_ROWS;
    if (pick < pick_top) pick_top = pick;
    if (pick >= pick_top + PICK_COLS * PICK_ROWS) pick_top = pick - PICK_COLS * PICK_ROWS + 1;
    if (pick_top < 0) pick_top = 0;
    if (pick_top > PICK_COUNT - PICK_COLS * PICK_ROWS) pick_top = PICK_COUNT - PICK_COLS * PICK_ROWS;
    if (pick_top < 0) pick_top = 0;
}

static void act_list( unsigned int button )
{
    switch (button)
    {
    case OSK_UP: selected--; break;
    case OSK_DOWN: selected++; break;
    case OSK_LEFT: case OSK_L: selected -= LIST_ROWS; break;
    case OSK_RIGHT: case OSK_R: selected += LIST_ROWS; break;
    case OSK_A:
        if (selected == SPEED_ROW)
        {
            pick = speed_choice( profile.mouse_speed );
            pick_top = 0;
            mode = MODE_SPEED;
            generation++;
            break;
        }
        draft = *row_binding( selected );
        for (unsigned int i = 0; i < draft.count; i++) draft.code[i] = normalize_code( draft.code[i] );
        pick = 0;
        pick_top = 0;
        capture_last = -1;
        mode = controller_mapping() ? MODE_CAPTURE : MODE_PICK;
        generation++;
        break;
    case OSK_X:
        if (selected != SPEED_ROW) save_row( selected, &(struct input_binding){0} );
        break;
    case OSK_Y:
        if (selected == SPEED_ROW) save_speed( -1 );
        else save_row( selected, NULL );
        break;
    case OSK_B: case OSK_MINUS: case OSK_PLUS: close_overlay(); break;
    }
    if (selected < 0) selected = 0;
    if (selected > SPEED_ROW) selected = SPEED_ROW;
}

static void act_speed( unsigned int button )
{
    switch (button)
    {
    case OSK_UP: pick -= PICK_COLS; break;
    case OSK_DOWN: pick += PICK_COLS; break;
    case OSK_LEFT: pick--; break;
    case OSK_RIGHT: pick++; break;
    case OSK_L: pick -= PICK_COLS * PICK_ROWS; break;
    case OSK_R: pick += PICK_COLS * PICK_ROWS; break;
    case OSK_A: save_speed( pick ); mode = MODE_LIST; generation++; break;
    case OSK_X: case OSK_Y: save_speed( -1 ); mode = MODE_LIST; generation++; break;
    case OSK_B: mode = MODE_LIST; generation++; break;
    case OSK_MINUS: case OSK_PLUS: close_overlay(); break;
    }
    if (pick < 0) pick = 0;
    if (pick >= INPUT_MOUSE_SPEED_COUNT) pick = INPUT_MOUSE_SPEED_COUNT - 1;
}

static void act_pick( unsigned int button )
{
    switch (button)
    {
    case OSK_UP: pick -= PICK_COLS; break;
    case OSK_DOWN: pick += PICK_COLS; break;
    case OSK_LEFT: pick--; break;
    case OSK_RIGHT: pick++; break;
    case OSK_L: pick -= PICK_COLS * PICK_ROWS; break;
    case OSK_R: pick += PICK_COLS * PICK_ROWS; break;
    case OSK_A:
    {
        unsigned short code = pick_targets[pick].code;
        unsigned int i;

        for (i = 0; i < draft.count; i++) if (normalize_code( draft.code[i] ) == code) break;
        if (i < draft.count)
            memmove( draft.code + i, draft.code + i + 1, (--draft.count - i) * sizeof(draft.code[0]) );
        else if (draft.count < INPUT_CHORD_MAX) draft.code[draft.count++] = code;
        generation++;
        break;
    }
    case OSK_X: draft.count = 0; generation++; break;
    case OSK_B: save_row( selected, &draft ); mode = MODE_LIST; generation++; break;
    case OSK_MINUS: case OSK_PLUS: close_overlay(); break;
    }
    if (pick < 0) pick = 0;
    if (pick >= PICK_COUNT) pick = PICK_COUNT - 1;
}

/* The row a capture would assign: the first button pressed, or the stick
 * direction pushed furthest. Minus is the overlay's own (it opens and closes
 * it) and B and X are kept for cancelling and clearing, as the strip says;
 * nothing is taken while Minus is held. */
static int captured_source( unsigned int held, int left_x, int left_y, int right_x, int right_y )
{
    const struct { int value; int source; } directions[] =
    {
        { left_y, 16 }, { -left_y, 17 }, { -left_x, 18 }, { left_x, 19 },
        { right_y, 20 }, { -right_y, 21 }, { -right_x, 22 }, { right_x, 23 },
    };
    int best = -1, best_value = STICK_DEAD;

    if (held & (OSK_MINUS | OSK_B | OSK_X)) return -1;
    for (unsigned int bit = 0; bit < 16; bit++)
        if (held & (1u << bit)) return button_source[bit];
    for (unsigned int i = 0; i < sizeof(directions) / sizeof(directions[0]); i++)
        if (directions[i].value > best_value) { best_value = directions[i].value; best = directions[i].source; }
    return best;
}

static void act_capture( unsigned int held, unsigned int pressed, int left_x, int left_y,
                         int right_x, int right_y )
{
    int source = captured_source( held, left_x, left_y, right_x, right_y );

    if (pressed & OSK_B) { mode = MODE_LIST; generation++; return; }
    if (pressed & OSK_X) { save_row( selected, &(struct input_binding){0} ); mode = MODE_LIST; generation++; return; }
    if (source >= 0 && source != capture_last)
    {
        /* The captured button is the pad output the row should make: the
         * same button, which is its own target code in the pad table. */
        save_row( selected, &(struct input_binding){ { (unsigned short)(source + 1) }, 1 } );
        mode = MODE_LIST;
        generation++;
    }
    capture_last = source;
}

static int repeats( unsigned int button )
{
    return button & (OSK_UP | OSK_DOWN | OSK_LEFT | OSK_RIGHT | OSK_L | OSK_R);
}

int wine_nx_controls_overlay_input( unsigned int held, int left_x, int left_y, int right_x, int right_y,
                                    int touching, int touch_x, int touch_y, uint64_t now_ns )
{
    struct geometry g;
    unsigned int pressed, bit;
    int on_panel = 0;

    pthread_mutex_lock( &overlay_lock );
    if (!visible)
    {
        pthread_mutex_unlock( &overlay_lock );
        return 0;
    }
    if (mode != MODE_CAPTURE)
    {
        if (left_y > STICK_DEAD) held |= OSK_UP;
        if (left_y < -STICK_DEAD) held |= OSK_DOWN;
        if (left_x < -STICK_DEAD) held |= OSK_LEFT;
        if (left_x > STICK_DEAD) held |= OSK_RIGHT;
    }
    ignored &= held;
    held &= ~ignored;
    pressed = held & ~previous;
    previous = held;

    if (mode == MODE_CAPTURE)
        act_capture( held, pressed, left_x, left_y, right_x, right_y );
    else
    {
        for (bit = 1; bit <= OSK_STICKR && visible; bit <<= 1)
        {
            if (!(pressed & bit)) continue;
            if (mode == MODE_LIST) act_list( bit );
            else if (mode == MODE_PICK) act_pick( bit );
            else act_speed( bit );
            if (visible && mode != MODE_CAPTURE && repeats( bit ))
            {
                repeating = bit;
                repeat_at = now_ns + REPEAT_FIRST_NS;
            }
        }
        if (repeating && !(held & repeating)) repeating = 0;
        if (visible && mode != MODE_CAPTURE && repeating && now_ns >= repeat_at)
        {
            if (mode == MODE_LIST) act_list( repeating );
            else if (mode == MODE_PICK) act_pick( repeating );
            else act_speed( repeating );
            repeat_at = now_ns + REPEAT_NEXT_NS;
        }
    }

    /* A tap is a press where the finger comes up, on the row or cell it went
     * down on: a row opens, a cell is taken into the chord or taken back out,
     * and the strip at the bottom is Back. The screen is the 1280x720 one the
     * keyboard reads its taps against. */
    layout( 1280, 720, &g );
    if (visible && touching)
    {
        int px = touch_x - g.x, py = touch_y - g.y;

        on_panel = px >= 0 && py >= 0 && px < g.width && py < g.height;
        if (!touch_down)
        {
            int area_y = g.pad + g.header, area_h = LIST_ROWS * g.row_h + (LIST_ROWS - 1) * g.gap;

            touch_down = 1;
            if (mode == MODE_LIST && on_panel && py >= area_y && py < area_y + area_h)
            {
                int row = (py - area_y) / (g.row_h + g.gap);

                if (row >= 0 && row < LIST_ROWS && list_top + row <= SPEED_ROW)
                {
                    selected = list_top + row;
                    act_list( OSK_A );
                }
            }
            else if ((mode == MODE_PICK || mode == MODE_SPEED) && on_panel &&
                     py >= area_y && py < area_y + area_h)
            {
                int cell_w = (g.width - 2 * g.pad - (PICK_COLS - 1) * g.gap) / PICK_COLS;
                int cell_h = (area_h - (PICK_ROWS - 1) * g.gap) / PICK_ROWS;
                int col = cell_w > 0 ? (px - g.pad) / (cell_w + g.gap) : -1;
                int row = cell_h > 0 ? (py - area_y) / (cell_h + g.gap) : -1;
                int index = pick_top + row * PICK_COLS + col;
                int count = mode == MODE_PICK ? PICK_COUNT : INPUT_MOUSE_SPEED_COUNT;

                if (col >= 0 && col < PICK_COLS && row >= 0 && row < PICK_ROWS &&
                    index >= 0 && index < count)
                {
                    pick = index;
                    if (mode == MODE_PICK) act_pick( OSK_A );
                    else act_speed( OSK_A );
                }
            }
            else if (on_panel && py >= g.height - g.pad - g.hint)
            {
                if (mode == MODE_LIST) act_list( OSK_B );
                else if (mode == MODE_PICK) act_pick( OSK_B );
                else if (mode == MODE_SPEED) act_speed( OSK_B );
            }
        }
    }
    else touch_down = 0;
    keep_in_view();
    pthread_mutex_unlock( &overlay_lock );
    return on_panel;
}

/* Drawing. */

#define PANEL    0xff15171cu
#define BORDER   0xff3a3f4bu
#define CARD     0xff2e323bu
#define SELECTED 0xff1e88e5u
#define TEXT     0xfff2f4f8u
#define DIM      0xff9aa3b2u

static TTF_Font *fonts[2];
static int font_sizes[2], font_failed;
static struct { char text[16]; int size; SDL_Surface *surface; } labels[192];
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
            else if (d < radius + 0.5f) picture[y * picture_w + x] = blend( picture[y * picture_w + x], color,
                                                                            (unsigned int)((radius + 0.5f - d) * 255) );
        }
}

/* Text, baseline-centred on y: to the left of x (align 0), around x (1), or
 * ending at x (2). */
static void text_at( int which, int size, const char *text, int x, int y, uint32_t color, int align )
{
    SDL_Surface *s = label_surface( which, size, text );
    int px, py, x0, y0;

    if (!s) return;
    x0 = align == 1 ? x - s->w / 2 : align == 2 ? x - s->w : x;
    y0 = y - s->h / 2;
    for (py = 0; py < s->h; py++)
        for (px = 0; px < s->w; px++)
        {
            uint32_t p = ((uint32_t *)((uint8_t *)s->pixels + py * s->pitch))[px];
            int x1 = x0 + px, y1 = y0 + py;

            if ((p >> 24) && x1 >= 0 && y1 >= 0 && x1 < picture_w && y1 < picture_h)
                picture[y1 * picture_w + x1] = blend( picture[y1 * picture_w + x1], color, p >> 24 );
        }
}

static void draw_row( const struct geometry *g, int row, int source, int active )
{
    int y = g->y + g->pad + g->header + row * (g->row_h + g->gap);
    int x = g->x + g->pad;
    int w = g->width - 2 * g->pad;
    char label[192];
    int size = (int)(g->row_h * 0.48f);

    fill_rounded( x, y, w, g->row_h, 12, active ? CARD : PANEL );
    if (active) fill_rounded( x, y, 6, g->row_h, 3, SELECTED );
    if (source == SPEED_ROW)
    {
        text_at( 0, size, "Mouse speed", x + 28, y + g->row_h / 2, TEXT, 0 );
        if (profile.mouse_speed_explicit) snprintf( label, sizeof(label), "%d%%", profile.mouse_speed );
        else snprintf( label, sizeof(label), "%d%% (default)", profile.mouse_speed );
        text_at( 1, size, label, x + w - 28, y + g->row_h / 2, active ? TEXT : DIM, 2 );
        return;
    }
    text_at( 0, size, input_sources[source].label, x + 28, y + g->row_h / 2, TEXT, 0 );
    input_binding_label( row_binding( source ), controller_mapping(), label, sizeof(label) );
    text_at( 1, size, label, x + w - 28, y + g->row_h / 2, active ? TEXT : DIM, 2 );
}

static void draw_list( const struct geometry *g, int size )
{
    for (int row = 0; row < LIST_ROWS; row++)
    {
        int source = list_top + row;

        if (source <= SPEED_ROW) draw_row( g, row, source, source == selected );
    }
    text_at( 1, size, "A change    X clear    Y reset    B close", g->x + g->width / 2,
             g->y + g->height - g->pad - g->hint / 2, DIM, 1 );
}

static void draw_pick( const struct geometry *g, int size )
{
    int area_y = g->y + g->pad + g->header;
    int area_h = LIST_ROWS * g->row_h + (LIST_ROWS - 1) * g->gap;
    int cell_w = (g->width - 2 * g->pad - (PICK_COLS - 1) * g->gap) / PICK_COLS;
    int cell_h = (area_h - (PICK_ROWS - 1) * g->gap) / PICK_ROWS;
    char title[128], label[192];

    snprintf( title, sizeof(title), "Map %s", input_sources[selected].label );
    text_at( 0, size, title, g->x + g->pad + 28, g->y + g->pad + g->header / 2, TEXT, 0 );
    input_binding_label( &draft, controller_mapping(), label, sizeof(label) );
    text_at( 1, size, label, g->x + g->width - g->pad - 28, g->y + g->pad + g->header / 2, DIM, 2 );
    for (int row = 0; row < PICK_ROWS; row++)
        for (int col = 0; col < PICK_COLS; col++)
        {
            int index = pick_top + row * PICK_COLS + col;
            int x, y;

            if (index >= PICK_COUNT) continue;
            x = g->x + g->pad + col * (cell_w + g->gap);
            y = area_y + row * (cell_h + g->gap);
            if (index == pick) fill_rounded( x - 3, y - 3, cell_w + 6, cell_h + 6, 12, DIM );
            fill_rounded( x, y, cell_w, cell_h, 10, has_code( &draft, pick_targets[index].code ) ? SELECTED : CARD );
            text_at( 0, size, pick_targets[index].label, x + cell_w / 2, y + cell_h / 2,
                     index == pick ? TEXT : DIM, 1 );
        }
    text_at( 1, size, "A add / remove    X clear    B done", g->x + g->width / 2,
             g->y + g->height - g->pad - g->hint / 2, DIM, 1 );
}

static void draw_speed( const struct geometry *g, int size )
{
    int area_y = g->y + g->pad + g->header;
    int area_h = LIST_ROWS * g->row_h + (LIST_ROWS - 1) * g->gap;
    int cell_w = (g->width - 2 * g->pad - (PICK_COLS - 1) * g->gap) / PICK_COLS;
    int cell_h = (area_h - (PICK_ROWS - 1) * g->gap) / PICK_ROWS;
    char label[32];

    text_at( 0, size, "Mouse speed", g->x + g->pad + 28, g->y + g->pad + g->header / 2, TEXT, 0 );
    if (profile.mouse_speed_explicit) snprintf( label, sizeof(label), "%d%%", profile.mouse_speed );
    else snprintf( label, sizeof(label), "%d%% (default)", profile.mouse_speed );
    text_at( 1, size, label, g->x + g->width - g->pad - 28, g->y + g->pad + g->header / 2, DIM, 2 );
    for (int row = 0; row < PICK_ROWS; row++)
        for (int col = 0; col < PICK_COLS; col++)
        {
            int index = pick_top + row * PICK_COLS + col;
            int x, y;

            if (index >= INPUT_MOUSE_SPEED_COUNT) continue;
            x = g->x + g->pad + col * (cell_w + g->gap);
            y = area_y + row * (cell_h + g->gap);
            if (index == pick) fill_rounded( x - 3, y - 3, cell_w + 6, cell_h + 6, 12, DIM );
            fill_rounded( x, y, cell_w, cell_h, 10,
                          profile.mouse_speed == input_mouse_speed_values[index] ? SELECTED : CARD );
            snprintf( label, sizeof(label), "%d%%", input_mouse_speed_values[index] );
            text_at( 0, size, label, x + cell_w / 2, y + cell_h / 2, index == pick ? TEXT : DIM, 1 );
        }
    text_at( 1, size, "A set    Y default    B back", g->x + g->width / 2,
             g->y + g->height - g->pad - g->hint / 2, DIM, 1 );
}

static void draw_capture( const struct geometry *g, int size )
{
    char title[128];

    snprintf( title, sizeof(title), "Map %s", input_sources[selected].label );
    text_at( 0, size, title, g->x + g->width / 2, g->y + g->pad + g->header / 2, TEXT, 1 );
    text_at( 0, size * 3 / 2, "Press the button to assign", g->x + g->width / 2,
             g->y + g->pad + g->header + (LIST_ROWS * g->row_h) / 2, TEXT, 1 );
    text_at( 1, size, "X clear    B cancel", g->x + g->width / 2,
             g->y + g->height - g->pad - g->hint / 2, DIM, 1 );
}

static void draw( int screen_w, int screen_h )
{
    struct geometry g;
    int x, y, size, small;

    layout( screen_w, screen_h, &g );
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

    size = (int)(g.row_h * 0.50f);
    small = size * 3 / 4;
    if (mode == MODE_LIST)
    {
        const char *kind = controller_mapping() ? "Controller remapping" : "Keyboard / mouse mapping";

        text_at( 0, size, "Controls", g.x + g.width / 2, g.y + g.pad + g.header / 2, TEXT, 1 );
        text_at( 1, small, kind, g.x + g.width - g.pad - 28, g.y + g.pad + g.header / 2, DIM, 2 );
        draw_list( &g, small );
    }
    else if (mode == MODE_PICK) draw_pick( &g, small );
    else if (mode == MODE_SPEED) draw_speed( &g, small );
    else draw_capture( &g, small );
}

/* Draws the picture for that screen if what is there is not it. */
static void ensure_picture( int screen_w, int screen_h )
{
    if (picture && picture_generation == generation && picture_screen_w == screen_w && picture_screen_h == screen_h)
        return;
    if (picture_screen_w != screen_w || picture_screen_h != screen_h)
    {
        /* Another screen size is another picture: the presenters that copied
         * the old one see a new generation. */
        if (picture_screen_w) generation++;
        picture_screen_w = screen_w;
        picture_screen_h = screen_h;
    }
    draw( screen_w, screen_h );
    picture_generation = generation;
}

int wine_nx_controls_overlay_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame )
{
    struct geometry g;

    pthread_mutex_lock( &overlay_lock );
    if (!visible)
    {
        pthread_mutex_unlock( &overlay_lock );
        return 0;
    }
    ensure_picture( screen_width, screen_height );
    layout( screen_width, screen_height, &g );
    frame->x = g.x;
    frame->y = g.y;
    frame->width = g.width;
    frame->height = g.height;
    frame->generation = generation | GENERATION_BIT;
    pthread_mutex_unlock( &overlay_lock );
    return 1;
}

unsigned int wine_nx_controls_overlay_copy( int screen_width, int screen_height, void *pixels,
                                            int stride, int rgba )
{
    unsigned int copied = 0;
    int x, y;

    pthread_mutex_lock( &overlay_lock );
    if (visible)
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
        copied = generation | GENERATION_BIT;
    }
    pthread_mutex_unlock( &overlay_lock );
    return copied;
}
