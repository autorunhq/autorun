/* Recommended keyboard and mouse controls for games that were made for a
 * mouse, applied over the player's global controls and under their own
 * per-game changes. A game is recognised by its executable's name; the layout
 * follows what the game reads: the right stick moves the mouse, one trigger
 * clicks, and the keys the game's manual lists sit on the buttons beside them.
 *
 * The player's own file wins: what they change in the controls editor (or
 * remap while the game runs) is kept per game and applied on top, and clearing
 * a row brings this layout back for it.
 *
 * Codes are the ones input_binding_parse reads: virtual-key codes for keys and
 * INPUT_MOUSE_* for mouse actions. Sources are input_sources' order (A, B, X,
 * Y, L, R, ZL, ZR, +, -, stick clicks, d-pad, then each stick's directions).
 * A binding this table does not mention keeps whatever the layers below gave
 * it, so only the game's own layout is spelled out here.
 */
#ifndef AUTORUN_GAME_CONTROLS_H
#define AUTORUN_GAME_CONTROLS_H

#include <string.h>
#include <strings.h>
#include "../input_profile.h"

enum game_control_source
{
    GAME_CONTROL_A, GAME_CONTROL_B, GAME_CONTROL_X, GAME_CONTROL_Y,
    GAME_CONTROL_L, GAME_CONTROL_R, GAME_CONTROL_ZL, GAME_CONTROL_ZR,
    GAME_CONTROL_PLUS, GAME_CONTROL_MINUS, GAME_CONTROL_STICKL, GAME_CONTROL_STICKR,
    GAME_CONTROL_UP, GAME_CONTROL_DOWN, GAME_CONTROL_LEFT, GAME_CONTROL_RIGHT,
    GAME_CONTROL_LUP, GAME_CONTROL_LDOWN, GAME_CONTROL_LLEFT, GAME_CONTROL_LRIGHT,
    GAME_CONTROL_RUP, GAME_CONTROL_RDOWN, GAME_CONTROL_RLEFT, GAME_CONTROL_RRIGHT,
};

struct game_control_binding
{
    unsigned char source;                     /* enum game_control_source */
    unsigned char count;
    unsigned short code[INPUT_CHORD_MAX];
};

struct game_controls
{
    const char *name;                         /* the game, for the editor's labels */
    const char *const *executables;           /* lowercase file names, NULL-terminated */
    int mode;                                 /* enum input_mode */
    int mouse_speed;                          /* percent of the built-in cursor speed */
    const struct game_control_binding *bindings;
    unsigned int count;
};

#define GAME_CONTROL( source, ... ) \
    { source, (unsigned char)(sizeof((unsigned short[]){ __VA_ARGS__ }) / sizeof(unsigned short)), \
      { __VA_ARGS__ } }

/* Septerra Core: a mouse game. Click to walk, talk and fight; Shift-click runs.
 * Tab is the map, Space the subscreen, X the options, F5 and F8 the quick save
 * and load, Q/A/Z pick a party member. The mapping follows the one the
 * Definitive Edition patcher and the Steam Deck community layouts settled on,
 * with the clicks on the buttons this console's players expect them on.
 */
static const struct game_control_binding septerra_controls[] =
{
    GAME_CONTROL( GAME_CONTROL_A, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_B, 0x1b ),                  /* Esc: back, skip */
    GAME_CONTROL( GAME_CONTROL_X, 0x0d ),                  /* Enter: item displays */
    GAME_CONTROL( GAME_CONTROL_Y, 0x20 ),                  /* Space: subscreen */
    GAME_CONTROL( GAME_CONTROL_L, 0x74 ),                  /* F5: quick save */
    GAME_CONTROL( GAME_CONTROL_R, 0x77 ),                  /* F8: quick load */
    GAME_CONTROL( GAME_CONTROL_ZL, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_ZR, 0x10, INPUT_MOUSE_LEFT ),  /* Shift-click: run */
    GAME_CONTROL( GAME_CONTROL_PLUS, 0x58 ),               /* X: options screen */
    GAME_CONTROL( GAME_CONTROL_MINUS, 0x09 ),              /* Tab: map */
    GAME_CONTROL( GAME_CONTROL_STICKR, INPUT_MOUSE_RIGHT ),
    GAME_CONTROL( GAME_CONTROL_UP, 0x51 ),                 /* Q: party member 1 */
    GAME_CONTROL( GAME_CONTROL_LEFT, 0x41 ),               /* A: party member 2 */
    GAME_CONTROL( GAME_CONTROL_DOWN, 0x5a ),               /* Z: party member 3 */
    GAME_CONTROL( GAME_CONTROL_RIGHT, 0x0d ),
    GAME_CONTROL( GAME_CONTROL_LUP, 0x26 ),
    GAME_CONTROL( GAME_CONTROL_LDOWN, 0x28 ),
    GAME_CONTROL( GAME_CONTROL_LLEFT, 0x25 ),
    GAME_CONTROL( GAME_CONTROL_LRIGHT, 0x27 ),
    GAME_CONTROL( GAME_CONTROL_RUP, INPUT_MOUSE_UP ),
    GAME_CONTROL( GAME_CONTROL_RDOWN, INPUT_MOUSE_DOWN ),
    GAME_CONTROL( GAME_CONTROL_RLEFT, INPUT_MOUSE_MOVE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_RRIGHT, INPUT_MOUSE_MOVE_RIGHT ),
};

/* Harbinger: a Diablo in space. Left click attacks, picks up and talks, right
 * click walks there, the middle button drinks a health item. Shift stands
 * still (and fires a ranged weapon without stepping), Alt shows the items on
 * the floor. Both buttons can be held down to keep attacking; the run-and-shoot
 * of holding Shift is on the other trigger.
 */
static const struct game_control_binding harbinger_controls[] =
{
    GAME_CONTROL( GAME_CONTROL_A, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_B, 0x1b ),                  /* Esc: menus, back */
    GAME_CONTROL( GAME_CONTROL_X, 0x12 ),                  /* Alt: show items */
    GAME_CONTROL( GAME_CONTROL_Y, INPUT_MOUSE_MIDDLE ),    /* drink a health item */
    GAME_CONTROL( GAME_CONTROL_L, 0x10 ),                  /* Shift: stand and fire */
    GAME_CONTROL( GAME_CONTROL_R, 0x12 ),                  /* Alt: show items */
    GAME_CONTROL( GAME_CONTROL_ZL, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_ZR, 0x10, INPUT_MOUSE_LEFT ),  /* stand and shoot */
    GAME_CONTROL( GAME_CONTROL_PLUS, 0x1b ),
    GAME_CONTROL( GAME_CONTROL_STICKR, INPUT_MOUSE_RIGHT ),
    GAME_CONTROL( GAME_CONTROL_RUP, INPUT_MOUSE_UP ),
    GAME_CONTROL( GAME_CONTROL_RDOWN, INPUT_MOUSE_DOWN ),
    GAME_CONTROL( GAME_CONTROL_RLEFT, INPUT_MOUSE_MOVE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_RRIGHT, INPUT_MOUSE_MOVE_RIGHT ),
};

/* Vampire: The Masquerade - Redemption (and its Ages of Redemption builds):
 * a mouse game. Left click walks, talks, attacks and picks things up; right
 * click uses the selected Discipline or item; Shift-click is the secondary
 * attack and the force run; Ctrl-click throws the whole coterie at a target.
 * Tab switches the character, the panes hang on the d-pad, the quick
 * Discipline slots sit on the left stick, and Pause pauses combat. The keys
 * are the ones the manual and the 1.1 patch readme list. */
static const struct game_control_binding vampire_controls[] =
{
    GAME_CONTROL( GAME_CONTROL_A, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_B, 0x1b ),                  /* Esc: menus, back */
    GAME_CONTROL( GAME_CONTROL_X, 0x20 ),                  /* Space: stop action, hold to stand */
    GAME_CONTROL( GAME_CONTROL_Y, 0x09 ),                  /* Tab: next coterie member */
    GAME_CONTROL( GAME_CONTROL_L, 0x11, INPUT_MOUSE_LEFT ),  /* Ctrl-click: party attack */
    GAME_CONTROL( GAME_CONTROL_R, 0x52 ),                  /* R: regroup the coterie */
    GAME_CONTROL( GAME_CONTROL_ZL, INPUT_MOUSE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_ZR, 0x10, INPUT_MOUSE_LEFT ),  /* Shift-click: heavy attack, run */
    GAME_CONTROL( GAME_CONTROL_PLUS, 0x13 ),               /* Pause: pause combat */
    GAME_CONTROL( GAME_CONTROL_MINUS, 0x08 ),              /* Backspace: close the panes */
    GAME_CONTROL( GAME_CONTROL_STICKL, 0x46 ),             /* F: Feed */
    GAME_CONTROL( GAME_CONTROL_STICKR, INPUT_MOUSE_RIGHT ), /* use the selected Discipline */
    GAME_CONTROL( GAME_CONTROL_UP, 0x4d ),                 /* M: map */
    GAME_CONTROL( GAME_CONTROL_DOWN, 0x49 ),               /* I: inventory */
    GAME_CONTROL( GAME_CONTROL_LEFT, 0x51 ),               /* Q: quest log */
    GAME_CONTROL( GAME_CONTROL_RIGHT, 0x43 ),              /* C: character pane */
    GAME_CONTROL( GAME_CONTROL_LUP, 0x31 ),                /* 1: quick Discipline 1 */
    GAME_CONTROL( GAME_CONTROL_LRIGHT, 0x32 ),             /* 2: quick Discipline 2 */
    GAME_CONTROL( GAME_CONTROL_LDOWN, 0x33 ),              /* 3: quick Discipline 3 */
    GAME_CONTROL( GAME_CONTROL_LLEFT, 0x34 ),              /* 4: quick Discipline 4 */
    GAME_CONTROL( GAME_CONTROL_RUP, INPUT_MOUSE_UP ),
    GAME_CONTROL( GAME_CONTROL_RDOWN, INPUT_MOUSE_DOWN ),
    GAME_CONTROL( GAME_CONTROL_RLEFT, INPUT_MOUSE_MOVE_LEFT ),
    GAME_CONTROL( GAME_CONTROL_RRIGHT, INPUT_MOUSE_MOVE_RIGHT ),
};

static const char *const septerra_executables[] =
    { "septerra.exe", "sr-septerra.exe", NULL };
static const char *const harbinger_executables[] =
    { "harbinger.exe", NULL };
static const char *const vampire_executables[] =
    { "vampire.exe", "vampire_aor.exe", "vampire_aor_portable.exe", NULL };

static const struct game_controls game_controls_list[] =
{
    { "Septerra Core", septerra_executables, INPUT_KEYBOARD_MOUSE, 150,
      septerra_controls, sizeof(septerra_controls) / sizeof(septerra_controls[0]) },
    { "Harbinger", harbinger_executables, INPUT_KEYBOARD_MOUSE, 150,
      harbinger_controls, sizeof(harbinger_controls) / sizeof(harbinger_controls[0]) },
    { "Vampire: The Masquerade - Redemption", vampire_executables, INPUT_KEYBOARD_MOUSE, 150,
      vampire_controls, sizeof(vampire_controls) / sizeof(vampire_controls[0]) },
};

/* The layout a program is recognised as, or NULL. The name matched is the
 * executable's, with either kind of directory separator before it. */
static inline const struct game_controls *game_controls_find( const char *target )
{
    const char *name = target;

    if (!target) return NULL;
    for (const char *p = target; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    for (unsigned int i = 0; i < sizeof(game_controls_list) / sizeof(game_controls_list[0]); i++)
    {
        const struct game_controls *game = &game_controls_list[i];
        for (const char *const *exe = game->executables; *exe; exe++)
            if (!strcasecmp( name, *exe )) return game;
    }
    return NULL;
}

/* Lay the game's bindings over the profile. Its mouse speed and input mode
 * come with them; anything the table leaves out keeps its earlier layers. */
static inline void game_controls_apply( struct input_profile *profile, const struct game_controls *game )
{
    if (!profile || !game) return;
    for (unsigned int i = 0; i < game->count; i++)
    {
        const struct game_control_binding *binding = &game->bindings[i];

        if (binding->source >= INPUT_SOURCE_COUNT || binding->count > INPUT_CHORD_MAX) continue;
        profile->keys[binding->source].count = binding->count;
        memcpy( profile->keys[binding->source].code, binding->code, sizeof(binding->code) );
        if (binding->source >= 16 && binding->source < 20) profile->left_inherit &= ~(1u << (binding->source - 16));
        if ((binding->source == GAME_CONTROL_A || binding->source == GAME_CONTROL_B) && !binding->count)
            profile->keys[binding->source] = (struct input_binding){ { INPUT_MOUSE_LEFT + binding->source }, 1 };
    }
    profile->mode = game->mode;
    /* The speed the game plays best at, unless the player chose one: a
     * personal speed is not the layout to be corrected. */
    if (!profile->mouse_speed_explicit) profile->mouse_speed = game->mouse_speed;
}

#endif
