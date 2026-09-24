/*
 * Drawing and input for the launcher, on SDL2's renderer: the fixed Wine-NX
 * visual system, text, panels, the header and the footer of button hints, a list
 * of settings rows, dialogs, and controller, keyboard and touch input.
 */
#ifndef WINE_NX_LAUNCHER_UI_H
#define WINE_NX_LAUNCHER_UI_H

#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher_audio.h"

/* SDL names controller buttons by position: Nintendo's A, on the right, is SDL's B. */
enum ui_button
{
    UI_NONE = -1,
    UI_A = SDL_CONTROLLER_BUTTON_B,
    UI_B = SDL_CONTROLLER_BUTTON_A,
    UI_X = SDL_CONTROLLER_BUTTON_Y,
    UI_Y = SDL_CONTROLLER_BUTTON_X,
    UI_MINUS = SDL_CONTROLLER_BUTTON_BACK,
    UI_PLUS = SDL_CONTROLLER_BUTTON_START,
    UI_L = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    UI_R = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    UI_UP = SDL_CONTROLLER_BUTTON_DPAD_UP,
    UI_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    UI_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    UI_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    UI_ZL = SDL_CONTROLLER_BUTTON_MAX,
    UI_ZR,
};

enum ui_touch
{
    UI_TOUCH_NONE,
    UI_TOUCH_TAP,
    UI_TOUCH_SCROLL_UP,    /* the finger moved up: show later rows */
    UI_TOUCH_SCROLL_DOWN,
    UI_TOUCH_SWIPE_LEFT,
    UI_TOUCH_SWIPE_RIGHT,
};

/* One input: a button (controller, keyboard or a tapped footer hint), or a touch. */
struct ui_input
{
    int button;          /* enum ui_button */
    enum ui_touch touch;
    int x, y;            /* where a touch ended, in screen pixels */
    int steps;           /* rows a scroll moves */
};

#define UI_TEXT_CACHE    192
#define UI_TEXT_KEY      160
#define UI_FOOTER_HINTS  10
#define UI_HEADER_HEIGHT 80
/* Every header keeps these, so the clock and the battery do not move from one
 * screen to the next. */
#define UI_HEADER_MARGIN 84     /* from the left and right edges */
#define UI_HEADER_CENTRE 56     /* the centre line of what a header draws */
#define UI_FOOTER_Y      (720 - 26)

struct ui_hint
{
    int button;         /* enum ui_button, or UI_NONE for a label only */
    const char *label;
};

struct ui_text_entry
{
    TTF_Font *font;
    SDL_Color color;
    char text[UI_TEXT_KEY];
    SDL_Texture *texture;
    int width, height;
    unsigned int use;
};

struct ui
{
    SDL_Window *window;
    struct launcher_audio *audio;
    SDL_Renderer *renderer;
    int width, height;
    TTF_Font *small, *normal, *large;
    SDL_Texture *glow, *sheen;
    /* The frame is drawn into screen and copied out at the end of it, so that a
     * modal can keep the screen it opened over in snapshot and dim it. Either
     * may be NULL, and then a modal simply has no screen behind it. */
    SDL_Texture *screen, *snapshot;
    SDL_Texture *glyphs[16];

    int animations;
    SDL_Color background, text, dim, value, selection, panel, card, focus, success, danger;

    struct ui_text_entry cache[UI_TEXT_CACHE];
    unsigned int cache_use;

    /* The clock and the battery belong to the launcher, which knows how to read
     * them; any screen's header can ask for them through this. */
    void (*header_status)( void *data, int right, int y );
    void *header_status_data;
    /* The mark the launcher puts in the corner, for the screens it does not
     * draw itself. Given header_status_data. */
    void (*footer_mark)( void *data );
    void (*background_tick)( void *data );
    void *background_data;
    /* Where the left of the clock and the battery came out, so that nothing a
     * screen draws on the right stands underneath them. */
    int status_left;
    /* Whether the arrow back has the focus, for the header to frame it. */
    int back_focused;
    /* How many modals are open: only the first dims what is behind it. */
    int modal_depth;
    int hide_overlays;

    SDL_GameController *controller;
    int held;
    Uint32 held_since, held_last;
    int stick_x, stick_y;
    Sint16 axes[4];
    int trigger_left, trigger_right;
    struct
    {
        int active, vertical;
        SDL_FingerID finger;
        float start_x, start_y, last_y;
        Uint32 started;
    } touch;

    SDL_Rect footer_hits[UI_FOOTER_HINTS];
    int footer_buttons[UI_FOOTER_HINTS];
    int footer_count;

    Uint32 fx_start, busy_until, deadline;
    float highlight, last_highlight;
    int scrolling_text;
    SDL_Event queued[32];
    int queued_count;
    int running;

    char toast[160];
    Uint32 toast_since, toast_until;
    int toast_notice;
};

int  ui_init( struct ui *ui, const void *font_data, size_t font_size, int animations );
void ui_quit( struct ui *ui );
void ui_sound( struct ui *ui, enum launcher_sound sound );
/* Why ui_init failed. */
const char *ui_error(void);
/* Whether SDL got as far as a window, so the screen was in EGL's hands. */
int  ui_screen_used(void);
/* Whether transitions should keep scheduling frames. */
int  ui_animated( const struct ui *ui );

/* A frame: ui_begin_frame, ui_poll until it returns 0, draw, ui_present, ui_wait. */
int  ui_begin_frame( struct ui *ui );
int  ui_poll( struct ui *ui, struct ui_input *input );
void ui_present( struct ui *ui );
/* Called with each finished frame before it is shown; the host test takes screenshots here. */
extern void (*ui_present_hook)( SDL_Renderer *renderer );
void ui_wait( struct ui *ui );
void ui_start_screen( struct ui *ui );

void ui_fill( struct ui *ui, int x, int y, int w, int h, SDL_Color color );
void ui_border( struct ui *ui, int x, int y, int w, int h, int thickness, SDL_Color color );
/* What has the focus: an outline the light travels around, dim over most of it
 * and lit in the stretch the light is in. */
void ui_animated_border( struct ui *ui, int x, int y, int w, int h, int radius, int thickness,
                         SDL_Color dim, SDL_Color lit );
/* The same outline with nothing travelling round it. */
void ui_outline( struct ui *ui, int x, int y, int w, int h, int radius, int thickness, SDL_Color color );
void ui_fill_circle( struct ui *ui, float cx, float cy, float radius, SDL_Color color );
void ui_rounded( struct ui *ui, int x, int y, int w, int h, int radius, SDL_Color color );
void ui_panel( struct ui *ui, int x, int y, int w, int h );
void ui_background( struct ui *ui );
/* A rectangle shading from one colour at its top (or, when horizontal, its left) to another. */
void ui_gradient( struct ui *ui, int x, int y, int w, int h, SDL_Color from, SDL_Color to, int horizontal );
/* A texture inside rounded corners, tinted by mod: covers, and the light over them.
 * src is the part of the texture to use, or NULL for all of it. */
void ui_rounded_texture( struct ui *ui, SDL_Texture *texture, const SDL_Rect *src, SDL_Rect rect, int radius,
                         SDL_Color mod );
/* White fading to nothing downwards, to stretch over a shape as a sheen. */
SDL_Texture *ui_sheen( struct ui *ui );
/* A white icon from an SVG path (launcher_svg.h), size pixels square, to tint with
 * SDL_SetTextureColorMod and SDL_SetTextureAlphaMod; NULL when it cannot be made. */
SDL_Texture *ui_svg_texture( struct ui *ui, const char *d, float view_x, float view_y, float view_w, float view_h,
                             int size );

int  ui_text_width( struct ui *ui, TTF_Font *font, const char *text );
void ui_text( struct ui *ui, TTF_Font *font, int x, int y, const char *text, SDL_Color color );
void ui_text_centered( struct ui *ui, TTF_Font *font, int cx, int y, const char *text, SDL_Color color );
/* Text on its way in or out: at open 1 all of it, at 0 none, and between the
 * two revealed from the left and faded, for a label that comes and goes. */
void ui_text_opening( struct ui *ui, TTF_Font *font, int x, int y, const char *text, SDL_Color color, float open );
void ui_text_right( struct ui *ui, TTF_Font *font, int right, int y, const char *text, SDL_Color color );
/* Text cut to max_width with an ellipsis, or, when scroll is set, moving back and forth. */
void ui_text_fit( struct ui *ui, TTF_Font *font, int x, int y, int max_width, const char *text,
                  SDL_Color color, int scroll );
int  ui_text_wrapped( struct ui *ui, TTF_Font *font, int x, int y, int max_width, int max_lines,
                      const char *text, SDL_Color color, int centered );

void ui_header( struct ui *ui, const char *title, const char *context );
/* A header for a screen one goes back from: an arrow, the screen's name beside
 * it, and what the launcher puts at the right. */
void ui_header_back( struct ui *ui, const char *title, const char *context );
void ui_footer( struct ui *ui, const struct ui_hint *hints, int count );
/* The same hints, ending at right on the line through y, and tappable like the footer's. */
void ui_hints_right( struct ui *ui, const struct ui_hint *hints, int count, int right, int y );
void ui_fade( struct ui *ui );
void ui_toast( struct ui *ui, const char *text, int milliseconds );
void ui_notice( struct ui *ui, const char *text );
void ui_draw_toast( struct ui *ui );
/* Move the highlight toward target_y; returns where to draw it. */
float ui_highlight( struct ui *ui, float target_y );

/* A card of text over the current screen, closed by A or B. */
void ui_message( struct ui *ui, const char *title, const char *text );
/* A question answered with A (returns 1) or B (returns 0). */
int  ui_confirm( struct ui *ui, const char *title, const char *text, const char *yes );
/* A question with more than two answers: returns the button that was pressed,
 * or UI_B for the way out. */
int  ui_ask( struct ui *ui, const char *title, const char *text, const struct ui_hint *hints, int count );
/* A short list inside a modal. Returns the item chosen, or -1 for the way out. */
int  ui_menu( struct ui *ui, const char *title, const char *const *items, int count, int selection );
void ui_progress_begin( struct ui *ui );
void ui_progress_update( struct ui *ui, const char *title, const char *status,
                         unsigned long long current, unsigned long long total );
void ui_progress_end( struct ui *ui );

/* A list of settings rows. ui_list_run draws it and handles input until the
 * user acts on a row, then returns the action for the row at list->selection;
 * the caller changes what it must and calls it again. */
/* How a settings row draws what it carries on the right: an arrow into a screen
 * of its own, a value the row itself changes, a switch, or read-only status. */
enum ui_row_kind { UI_ROW_ACTION, UI_ROW_VALUE, UI_ROW_SWITCH, UI_ROW_DROPDOWN, UI_ROW_INFO };
enum ui_value_tone { UI_VALUE_NORMAL, UI_VALUE_SUCCESS, UI_VALUE_DANGER };

struct ui_row
{
    char label[96];
    char value[192];
    int disabled;
    int adjustable;     /* Left and Right change the value */
    int destructive;
    int download;
    const char *help;   /* the line under the label, and what X shows */
    unsigned char kind; /* enum ui_row_kind, for ui_settings_run */
    unsigned char value_tone;
    unsigned char on;   /* UI_ROW_SWITCH: which way it is set */
    unsigned char choices; /* dropdown rows; zero for a live catalog */
    unsigned char group;/* which section it belongs to */
};

struct ui_list
{
    int selection, top;
    int started;
    float scroll, scroll_from;
    int scroll_target, started_scroll;
    Uint32 scroll_since;
    /* ui_settings_run: whether the sections or the rows have the focus, and
     * whether the row in focus is being changed rather than moved between. */
    int in_rows;
    int editing;
    /* Whether the focus has gone up to the arrow back in the header. */
    int in_header;
};

enum ui_action
{
    UI_ACTION_BACK,
    UI_ACTION_CHOOSE,
    UI_ACTION_LEFT,
    UI_ACTION_RIGHT,
    UI_ACTION_RESET,
    UI_ACTION_QUIT,     /* the system asked the launcher to close */
};

enum ui_action ui_list_run( struct ui *ui, struct ui_list *list, const char *title, const char *context,
                            const struct ui_row *rows, int count, int can_reset );

/* Settings, as a section list beside the rows of the section in focus: each row
 * carries its own description, and shows a switch, a value or an arrow.
 *
 * One thing is in focus at a time and the arrows reach all of it: up and down
 * within the sections or the rows, left back to the sections, right into them.
 * A acts -- a switch turns over, a row that opens something opens it, and a row
 * with a value of its own is taken hold of, after which left and right change it
 * and A or B lets go. group selects the section on entry and is left on the one
 * the user ends in. */
enum ui_action ui_settings_run( struct ui *ui, struct ui_list *list, const char *title, const char *context,
                                const char *const *groups, int group_count,
                                const struct ui_row *rows, int count, int can_reset, int *group );
int ui_settings_dropdown( struct ui *ui, const struct ui_list *anchor,
                          const struct ui_row *rows, int count, int selection );
int ui_settings_dropdown_live( struct ui *ui, const struct ui_list *anchor, const struct ui_row *rows,
                               int selection, int (*update)( void *data, int *selection ), void *data );

#endif
