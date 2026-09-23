/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The floating keyboard (osk.h): a US layout, five rows fifteen units wide,
 * the selection moved with the d-pad or the left stick, a key pressed with A
 * or a tap. Keys go out as virtual-key codes, down and up, with Shift held
 * around the ones that need it, so a program sees what a keyboard would send
 * for them: a field it draws itself can be backspaced, moved through and
 * typed into, and it only gets Enter when Enter is pressed.
 *
 * Its picture is drawn here, in software, with SDL_ttf over the font
 * wine_nx_osk_font gives, and only when something on it changed.
 */
#include <math.h>
#include <pthread.h>
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

#define ROWS 5
#define ROW_UNITS 15

/* A key that types a character, and one that does something else. */
#define C( label, shifted, vk ) { label, shifted, vk, 4, KEY_CHAR }
#define K( label, vk, quarters, kind ) { label, NULL, vk, quarters, kind }

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
static const struct osk_key *const rows[ROWS] = { row0, row1, row2, row3, row4 };

#define VK_SHIFT 0x10
#define REPEAT_FIRST_NS 350000000ull
#define REPEAT_NEXT_NS   70000000ull
#define STICK_DEAD 16000

static pthread_mutex_t osk_lock = PTHREAD_MUTEX_INITIALIZER;
static int visible, shift, caps, at_top;
static int sel_row = 2, sel_col = 5;
static unsigned int ignored, previous, repeating;
static uint64_t repeat_at;
static int touch_row = -1, touch_col = -1, touch_was_down;
static unsigned int generation = 1;

/* Keys to send, each with when it is due: a key is held down for a while, as
 * a finger holds one, so a program that looks at the keyboard once a frame
 * (DirectInput, GetAsyncKeyState) still sees it down, and sees Shift down
 * with it. */
static struct { unsigned short vk; unsigned char up; uint64_t due; } queue[256];
static unsigned int queue_head, queue_tail;
static uint64_t input_now, last_due;

#define KEY_GAP_NS   10000000ull  /* between one key and the next */
#define KEY_HOLD_NS  40000000ull  /* how long a key stays down */
#define SHIFT_LEAD_NS 20000000ull /* Shift down before the key, and up after it */

/* The picture last drawn, for which screen and which generation. */
static uint32_t *picture;
static int picture_screen_w, picture_screen_h, picture_w, picture_h;
static unsigned int picture_generation;

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

/* delay: after the event before it, or now when the queue has gone quiet. */
static void push( unsigned short vk, int up, uint64_t delay )
{
    unsigned int slot = queue_tail % (sizeof(queue) / sizeof(queue[0]));

    if (queue_tail - queue_head >= sizeof(queue) / sizeof(queue[0])) return;
    last_due = last_due + delay > input_now ? last_due + delay : input_now;
    queue[slot].vk = vk;
    queue[slot].up = !!up;
    queue[slot].due = last_due;
    queue_tail++;
}

static void tap( unsigned short vk )
{
    push( vk, 0, KEY_GAP_NS );
    push( vk, 1, KEY_HOLD_NS );
}

static void press( int row, int col )
{
    const struct osk_key *key = &rows[row][col];
    int shifted;

    switch (key->kind)
    {
    case KEY_CHAR:
        shifted = is_letter( key ) ? shift ^ caps : shift;
        if (shifted)
        {
            push( VK_SHIFT, 0, KEY_GAP_NS );
            push( key->vk, 0, SHIFT_LEAD_NS );
            push( key->vk, 1, KEY_HOLD_NS );
            push( VK_SHIFT, 1, SHIFT_LEAD_NS );
        }
        else tap( key->vk );
        shift = 0;  /* one character, as on a phone */
        break;
    case KEY_ACTION:
    case KEY_ARROW:
        tap( key->vk );
        break;
    case KEY_SHIFT:
        shift = !shift;
        break;
    case KEY_CAPS:
        caps = !caps;
        break;
    case KEY_CLOSE:
        visible = 0;
        break;
    }
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

/* What a button does, pressed or repeated. */
static void act( unsigned int button )
{
    switch (button)
    {
    case OSK_UP: case OSK_DOWN: case OSK_LEFT: case OSK_RIGHT: move( button ); break;
    case OSK_A: press( sel_row, sel_col ); break;
    case OSK_B: tap( 0x08 ); break;
    case OSK_Y: tap( 0x20 ); break;
    case OSK_X: shift = !shift; generation++; break;
    case OSK_L: tap( 0x25 ); break;
    case OSK_R: tap( 0x27 ); break;
    case OSK_PLUS: tap( 0x0d ); break;
    case OSK_ZL: if (!at_top) { at_top = 1; generation++; } break;
    case OSK_ZR: if (at_top) { at_top = 0; generation++; } break;
    case OSK_MINUS: visible = 0; generation++; break;
    }
}

static int repeats( unsigned int button )
{
    return button & (OSK_UP | OSK_DOWN | OSK_LEFT | OSK_RIGHT | OSK_B | OSK_L | OSK_R);
}

int wine_nx_osk_visible( void )
{
    return __atomic_load_n( &visible, __ATOMIC_RELAXED );
}

unsigned int wine_nx_osk_generation( void )
{
    return __atomic_load_n( &generation, __ATOMIC_RELAXED );
}

void wine_nx_osk_show( int show, unsigned int held )
{
    pthread_mutex_lock( &osk_lock );
    if (visible != !!show)
    {
        visible = !!show;
        ignored = held;
        previous = 0;
        repeating = 0;
        touch_row = touch_col = -1;
        touch_was_down = 0;
        generation++;
    }
    pthread_mutex_unlock( &osk_lock );
}

int wine_nx_osk_input( unsigned int held, int stick_x, int stick_y, int touching, int touch_x, int touch_y,
                       uint64_t now_ns )
{
    unsigned int pressed, bit;
    struct geometry g;
    int on_panel = 0;

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

    for (bit = 1; bit <= OSK_STICKR && visible; bit <<= 1)
    {
        if (!(pressed & bit)) continue;
        act( bit );
        if (repeats( bit ))
        {
            repeating = bit;
            repeat_at = now_ns + REPEAT_FIRST_NS;
        }
    }
    if (repeating && !(held & repeating)) repeating = 0;
    if (visible && repeating && now_ns >= repeat_at)
    {
        act( repeating );
        repeat_at = now_ns + REPEAT_NEXT_NS;
    }

    /* A tap is a press where the finger comes up, on the key it went down on. */
    layout( 1280, 720, at_top, &g );
    if (visible && touching)
    {
        int px = touch_x - g.x, py = touch_y - g.y, row, col, n, x, y, w, h;

        on_panel = px >= 0 && py >= 0 && px < g.width && py < g.height;
        if (!touch_was_down)
        {
            touch_row = touch_col = -1;
            for (row = 0; on_panel && row < ROWS; row++)
                for (col = 0, n = row_length( row ); col < n; col++)
                {
                    key_rect( &g, row, col, &x, &y, &w, &h );
                    if (px >= x - g.gap / 2 && px < x + w + g.gap / 2 && py >= y - g.gap / 2 &&
                        py < y + h + g.gap / 2)
                    {
                        touch_row = row;
                        touch_col = col;
                    }
                }
            if (touch_row >= 0)
            {
                sel_row = touch_row;
                sel_col = touch_col;
                generation++;
            }
        }
        touch_was_down = 1;
    }
    else if (touch_was_down)
    {
        touch_was_down = 0;
        if (visible && touch_row >= 0) press( touch_row, touch_col );
        touch_row = touch_col = -1;
    }
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
            else if (d < radius + 0.5f)
                picture[y * picture_w + x] = blend( picture[y * picture_w + x], color,
                                                    (unsigned int)((radius + 0.5f - d) * 255) );
        }
}

static void text_centered( int which, int size, const char *text, int cx, int cy, uint32_t color )
{
    SDL_Surface *s = label_surface( which, size, text );
    int x, y, x0, y0;

    if (!s) return;
    x0 = cx - s->w / 2;
    y0 = cy - s->h / 2;
    for (y = 0; y < s->h; y++)
        for (x = 0; x < s->w; x++)
        {
            uint32_t p = ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x];
            int px = x0 + x, py = y0 + y;

            if ((p >> 24) && px >= 0 && py >= 0 && px < picture_w && py < picture_h)
                picture[py * picture_w + px] = blend( picture[py * picture_w + px], color, p >> 24 );
        }
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

            if ((key->kind == KEY_SHIFT && shift) || (key->kind == KEY_CAPS && caps)) color = ACTIVE;
            if (row == sel_row && col == sel_col) color = SELECTED;
            key_rect( &g, row, col, &x, &y, &w, &h );
            fill_rounded( x, y, w, h, (int)(g.key_h / 8), color );
            if (key->kind == KEY_CHAR && (is_letter( key ) ? shift ^ caps : shift)) label = key->shifted;
            if (key->kind == KEY_ARROW) arrow( x + w / 2, y + h / 2, (int)(g.key_h / 5), key->vk, TEXT );
            else text_centered( 0, key->kind == KEY_CHAR ? size : (int)(size * 0.8f), label, x + w / 2, y + h / 2, TEXT );
        }
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

int wine_nx_osk_frame( int screen_width, int screen_height, struct wine_nx_osk_frame *frame )
{
    struct geometry g;

    pthread_mutex_lock( &osk_lock );
    if (!visible)
    {
        pthread_mutex_unlock( &osk_lock );
        return 0;
    }
    ensure_picture( screen_width, screen_height );
    layout( screen_width, screen_height, at_top, &g );
    frame->x = g.x;
    frame->y = g.y;
    frame->width = g.width;
    frame->height = g.height;
    frame->generation = generation;
    pthread_mutex_unlock( &osk_lock );
    return 1;
}

unsigned int wine_nx_osk_copy( int screen_width, int screen_height, void *pixels, int stride, int rgba )
{
    unsigned int copied = 0;
    int x, y;

    pthread_mutex_lock( &osk_lock );
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
        copied = generation;
    }
    pthread_mutex_unlock( &osk_lock );
    return copied;
}

void wine_nx_osk_reset( void )
{
    pthread_mutex_lock( &osk_lock );
    visible = shift = caps = at_top = 0;
    sel_row = 2;
    sel_col = 5;
    ignored = previous = repeating = 0;
    touch_row = touch_col = -1;
    touch_was_down = 0;
    queue_head = queue_tail = 0;
    input_now = last_due = 0;
    generation++;
    pthread_mutex_unlock( &osk_lock );
}
