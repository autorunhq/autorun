/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * Raw mouse input: the WM_INPUT payload for a program that reads the mouse
 * through DirectInput 8, which registers a raw mouse device rather than
 * watching the window's mouse messages. Mouse look wants the motion the mouse
 * made, not where the cursor ended up, so the payload carries the movement
 * asked for before the screen's edges are applied to it.
 */
#ifndef WINE_NX_HORIZON_MOUSE_H
#define WINE_NX_HORIZON_MOUSE_H

#include <string.h>

/* MOUSEEVENTF_*, as a hardware message carries them. */
#define HORIZON_MOUSEEVENTF_MOVE        0x0001
#define HORIZON_MOUSEEVENTF_LEFTDOWN    0x0002
#define HORIZON_MOUSEEVENTF_LEFTUP      0x0004
#define HORIZON_MOUSEEVENTF_RIGHTDOWN   0x0008
#define HORIZON_MOUSEEVENTF_RIGHTUP     0x0010
#define HORIZON_MOUSEEVENTF_MIDDLEDOWN  0x0020
#define HORIZON_MOUSEEVENTF_MIDDLEUP    0x0040
#define HORIZON_MOUSEEVENTF_XDOWN       0x0080
#define HORIZON_MOUSEEVENTF_XUP         0x0100
#define HORIZON_MOUSEEVENTF_WHEEL       0x0800
#define HORIZON_MOUSEEVENTF_HWHEEL      0x1000
#define HORIZON_MOUSEEVENTF_ABSOLUTE    0x8000

#define HORIZON_RIM_TYPEMOUSE           0
#define HORIZON_WINE_MOUSE_HANDLE       1
/* struct rawinput_device.usage: MAKELONG(HID_USAGE_GENERIC_MOUSE, HID_USAGE_PAGE_GENERIC) */
#define HORIZON_RAWINPUT_USAGE_MOUSE    0x00010002

#define HORIZON_MOUSE_MOVE_RELATIVE     0x0000
#define HORIZON_MOUSE_MOVE_ABSOLUTE     0x0001

#define HORIZON_RI_MOUSE_LEFT_DOWN      0x0001
#define HORIZON_RI_MOUSE_LEFT_UP        0x0002
#define HORIZON_RI_MOUSE_RIGHT_DOWN     0x0004
#define HORIZON_RI_MOUSE_RIGHT_UP       0x0008
#define HORIZON_RI_MOUSE_MIDDLE_DOWN    0x0010
#define HORIZON_RI_MOUSE_MIDDLE_UP      0x0020
#define HORIZON_RI_MOUSE_BUTTON_4_DOWN  0x0040
#define HORIZON_RI_MOUSE_BUTTON_4_UP    0x0080
#define HORIZON_RI_MOUSE_BUTTON_5_DOWN  0x0100
#define HORIZON_RI_MOUSE_BUTTON_5_UP    0x0200
#define HORIZON_RI_MOUSE_WHEEL          0x0400
#define HORIZON_RI_MOUSE_HWHEEL         0x0800

/* RAWMOUSE, which win32u copies out of the message whole. */
struct horizon_raw_mouse
{
    unsigned short flags;              /* usFlags: relative or absolute motion */
    unsigned short pad;
    unsigned short button_flags;       /* usButtonFlags: the buttons that changed */
    unsigned short button_data;        /* usButtonData: wheel notches */
    unsigned int raw_buttons;
    int last_x;
    int last_y;
    unsigned int extra_information;
};

/* One mouse event as a raw payload. dx and dy are the movement asked for, which
 * is what a program turning a view wants: at the edge of the screen the cursor
 * stops and the mouse does not. */
static inline void horizon_raw_mouse_event_data( unsigned int flags, unsigned int data, int dx, int dy,
                                                 unsigned long long info, struct horizon_raw_mouse *raw )
{
    static const struct { unsigned int event; unsigned short button; } buttons[] =
    {
        { HORIZON_MOUSEEVENTF_LEFTDOWN,   HORIZON_RI_MOUSE_LEFT_DOWN },
        { HORIZON_MOUSEEVENTF_LEFTUP,     HORIZON_RI_MOUSE_LEFT_UP },
        { HORIZON_MOUSEEVENTF_RIGHTDOWN,  HORIZON_RI_MOUSE_RIGHT_DOWN },
        { HORIZON_MOUSEEVENTF_RIGHTUP,    HORIZON_RI_MOUSE_RIGHT_UP },
        { HORIZON_MOUSEEVENTF_MIDDLEDOWN, HORIZON_RI_MOUSE_MIDDLE_DOWN },
        { HORIZON_MOUSEEVENTF_MIDDLEUP,   HORIZON_RI_MOUSE_MIDDLE_UP },
    };
    unsigned int i;

    memset( raw, 0, sizeof(*raw) );
    /* Always relative: a program asks for raw input to be told the movement,
     * and an absolute payload would make it ask for the cursor instead. */
    raw->flags = HORIZON_MOUSE_MOVE_RELATIVE;
    if (flags & HORIZON_MOUSEEVENTF_MOVE)
    {
        raw->last_x = dx;
        raw->last_y = dy;
    }
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (flags & buttons[i].event) raw->button_flags |= buttons[i].button;
    if ((flags & HORIZON_MOUSEEVENTF_XDOWN) && (data & 1)) raw->button_flags |= HORIZON_RI_MOUSE_BUTTON_4_DOWN;
    if ((flags & HORIZON_MOUSEEVENTF_XDOWN) && (data & 2)) raw->button_flags |= HORIZON_RI_MOUSE_BUTTON_5_DOWN;
    if ((flags & HORIZON_MOUSEEVENTF_XUP) && (data & 1)) raw->button_flags |= HORIZON_RI_MOUSE_BUTTON_4_UP;
    if ((flags & HORIZON_MOUSEEVENTF_XUP) && (data & 2)) raw->button_flags |= HORIZON_RI_MOUSE_BUTTON_5_UP;
    if (flags & (HORIZON_MOUSEEVENTF_WHEEL | HORIZON_MOUSEEVENTF_HWHEEL))
    {
        raw->button_flags |= (flags & HORIZON_MOUSEEVENTF_WHEEL) ? HORIZON_RI_MOUSE_WHEEL : HORIZON_RI_MOUSE_HWHEEL;
        raw->button_data = (unsigned short)data;
    }
    raw->extra_information = (unsigned int)info;
}

static inline void horizon_raw_mouse_event( unsigned int flags, int dx, int dy,
                                            unsigned long long info, struct horizon_raw_mouse *raw )
{
    horizon_raw_mouse_event_data( flags, 0, dx, dy, info, raw );
}

#endif /* WINE_NX_HORIZON_MOUSE_H */
