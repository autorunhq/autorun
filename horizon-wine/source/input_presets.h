/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * Ready-made keyboard / mouse mappings for games made for a keyboard and a
 * mouse, chosen in the controls menu (launcher_input.c). They follow what
 * controller layouts for such games mostly do (Steam Input's "Keyboard (WASD)
 * and Mouse", handheld PCs): the mouse buttons on the triggers, W A S D on the
 * left stick and the mouse on the right. A preset writes every key.* binding
 * and keyboard / mouse mode into the profile it is chosen for; each binding
 * can still be changed afterwards. The first entry, Autorun's own mapping,
 * clears them instead (input_profile_defaults).
 */
#ifndef AUTORUN_INPUT_PRESETS_H
#define AUTORUN_INPUT_PRESETS_H

#include "input_profile.h"

#define INPUT_PRESET_COUNT 4

struct input_preset
{
    const char *name, *help;
    /* In input_sources' order; all zero for Autorun's own mapping. */
    uint16_t keys[INPUT_SOURCE_COUNT];
};

/* Windows virtual keys. */
enum
{
    PRESET_TAB = 0x09, PRESET_SHIFT = 0x10, PRESET_CTRL = 0x11, PRESET_ESC = 0x1b, PRESET_SPACE = 0x20,
    PRESET_LEFT = 0x25, PRESET_UP = 0x26, PRESET_RIGHT = 0x27, PRESET_DOWN = 0x28,
    PRESET_F5 = 0x74, PRESET_F9 = 0x78,
};

static const struct input_preset input_presets[INPUT_PRESET_COUNT] =
{
    { "Autorun default", "Autorun's own mapping: mouse buttons on A and B, arrow keys on the d-pad.", {0} },
    {
        "First-person shooter",
        "ZR fires (left click), ZL aims (right click), A jumps, B crouches, X reloads, Y uses, "
        "L and R change weapon, d-pad 1 to 4. Doom 3, Half-Life, Quake.",
        {
            /* A B X Y */ PRESET_SPACE, PRESET_CTRL, 'R', 'E',
            /* L R */ INPUT_WHEEL_UP, INPUT_WHEEL_DOWN,
            /* ZL ZR */ INPUT_MOUSE_RIGHT, INPUT_MOUSE_LEFT,
            /* + - */ PRESET_ESC, PRESET_TAB,
            /* stick clicks */ PRESET_SHIFT, 'F',
            /* d-pad up down left right */ '1', '3', '4', '2',
            /* left stick */ 'W', 'S', 'A', 'D',
            /* right stick */ INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
        }
    },
    {
        "Action RPG",
        "ZR attacks (left click), ZL blocks or casts (right click), A uses (E), B jumps, X readies, "
        "Y opens the menu, L sneaks, R sprints, d-pad inventory, map, quick save and load. "
        "Skyrim, Oblivion, Morrowind.",
        {
            /* A B X Y */ 'E', PRESET_SPACE, 'R', PRESET_TAB,
            /* L R */ PRESET_CTRL, PRESET_SHIFT,
            /* ZL ZR */ INPUT_MOUSE_RIGHT, INPUT_MOUSE_LEFT,
            /* + - */ PRESET_ESC, PRESET_TAB,
            /* stick clicks */ PRESET_SHIFT, 'Q',
            /* d-pad up down left right */ 'I', 'M', PRESET_F9, PRESET_F5,
            /* left stick */ 'W', 'S', 'A', 'D',
            /* right stick */ INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
        }
    },
    {
        "Point-and-click / strategy",
        "The left stick moves the pointer; A and ZR click, B and ZL right-click, the right stick "
        "scrolls and turns, X pauses (Space), the d-pad moves the view. Diablo, Baldur's Gate, "
        "adventures, strategy games.",
        {
            /* A B X Y */ INPUT_MOUSE_LEFT, INPUT_MOUSE_RIGHT, PRESET_SPACE, 'I',
            /* L R */ PRESET_SHIFT, PRESET_CTRL,
            /* ZL ZR */ INPUT_MOUSE_RIGHT, INPUT_MOUSE_LEFT,
            /* + - */ PRESET_ESC, PRESET_TAB,
            /* stick clicks */ INPUT_MOUSE_MIDDLE, PRESET_TAB,
            /* d-pad up down left right */ PRESET_UP, PRESET_DOWN, PRESET_LEFT, PRESET_RIGHT,
            /* left stick */ INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
            /* right stick */ INPUT_WHEEL_UP, INPUT_WHEEL_DOWN, PRESET_LEFT, PRESET_RIGHT,
        }
    },
};

/* Writes preset's bindings and keyboard / mouse mode into kv, a profile's
 * settings; 0 when they do not fit. Autorun's own mapping takes the bindings
 * out, so input_profile_defaults' come back, and leaves the mode as it is. */
static inline int input_preset_write( struct launcher_kv *kv, int preset )
{
    const struct input_preset *p;

    if (preset < 0 || preset >= INPUT_PRESET_COUNT) return 0;
    p = &input_presets[preset];
    for (int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        struct input_binding binding = { { p->keys[i] }, 1 };

        /* The plain names (A, ZL, ...) an older file may hold go too. */
        if (!launcher_kv_set( kv, input_sources[i].name, NULL ) ||
            !input_binding_save( kv, i, 0, preset ? &binding : NULL ))
            return 0;
    }
    return !preset || launcher_kv_set( kv, "input-mode", "2" );
}

#endif
