#!/usr/bin/env python3
"""The WM_INPUT payload for a raw mouse registration (horizon_mouse.h and the
mouse path of horizon_server_handle_send_hardware_message in
dlls/ntdll/unix/horizon.c).

DirectInput 8 registers a raw mouse device rather than watching a window's
mouse messages, so a program that turns a view -- Halo -- reads lLastX and
lLastY and nothing else. The stick moves a cursor, which the edges of the
screen stop; the movement it asked for does not stop there, and that is what
the payload has to carry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'dlls/ntdll/unix/horizon_mouse.h').read_text()
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()

def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

send = function(horizon, 'static int horizon_server_handle_send_hardware_message')
# The movement, taken before the screen's edges are applied to it: what the
# mouse did for a stick, and the step from the last place pointed at for a
# touch, which owes nothing to where the cursor is.
assert send.index('dx = mouse->x;') < send.index('if (x < desktop->cursor.clip.left)')
assert send.index('mouse->x - horizon_pointed_at_x') < send.index('if (x < desktop->cursor.clip.left)')
assert 'if (flags & HORIZON_MOUSEEVENTF_LEFTDOWN) dx = dy = 0;' in send
# Raw input first, and a program that asked for the mouse alone gets nothing else.
assert send.index('horizon_server_queue_raw_mouse_locked') < send.index('HORIZON_WM_MOUSEMOVE')
assert 'legacy = !raw_device || !(raw_device->flags & HORIZON_RIDEV_NOLEGACY)' in send
assert 'if (legacy && (flags & HORIZON_MOUSEEVENTF_MOVE)' in send
assert 'if (!legacy) continue;' in send
# And the cursor stays where it is for one.
cursor = send.index('desktop->cursor.x = x;')
assert send.rindex('if (legacy)', 0, cursor) > send.index('mouse->x - horizon_pointed_at_x')

message = function(horizon, 'static int horizon_server_handle_get_message(')
assert 'HORIZON_RIM_TYPEMOUSE' in message and 'queued->raw_m' in message

buttons = function(horizon, 'horizon_mouse_button_events[]')
assert 'HORIZON_WM_MBUTTONDOWN' in buttons and 'HORIZON_WM_MBUTTONUP' in buttons

fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

@HEADER@

/* RAWMOUSE, from the SDK: what win32u copies the payload into. */
typedef struct
{
    unsigned short usFlags;
    union
    {
        unsigned int ulButtons;
        struct { unsigned short usButtonFlags, usButtonData; };
    };
    unsigned int ulRawButtons;
    int lLastX, lLastY;
    unsigned int ulExtraInformation;
} RAWMOUSE;

#define SAME(field, member) \
    _Static_assert( offsetof(struct horizon_raw_mouse, field) == offsetof(RAWMOUSE, member), #field )
SAME( flags, usFlags );
SAME( button_flags, usButtonFlags );
SAME( button_data, usButtonData );
SAME( raw_buttons, ulRawButtons );
SAME( last_x, lLastX );
SAME( last_y, lLastY );
SAME( extra_information, ulExtraInformation );
_Static_assert( sizeof(struct horizon_raw_mouse) == sizeof(RAWMOUSE), "size" );

int main( void )
{
    struct horizon_raw_mouse raw;

    /* Movement, which a view being turned reads as it is. */
    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_MOVE | HORIZON_MOUSEEVENTF_ABSOLUTE, 37, -11, 0, &raw );
    assert( raw.flags == HORIZON_MOUSE_MOVE_RELATIVE );  /* never the cursor's place */
    assert( raw.last_x == 37 && raw.last_y == -11 );
    assert( !raw.button_flags );

    /* A click, with no movement in it. */
    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_LEFTDOWN, 9, 9, 0, &raw );
    assert( raw.button_flags == HORIZON_RI_MOUSE_LEFT_DOWN );
    assert( !raw.last_x && !raw.last_y );

    /* Both buttons let go at once, both said. */
    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_LEFTUP | HORIZON_MOUSEEVENTF_RIGHTUP, 0, 0, 0, &raw );
    assert( raw.button_flags == (HORIZON_RI_MOUSE_LEFT_UP | HORIZON_RI_MOUSE_RIGHT_UP) );

    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_MOVE | HORIZON_MOUSEEVENTF_RIGHTDOWN, -1, 2, 0x1234, &raw );
    assert( raw.last_x == -1 && raw.last_y == 2 );
    assert( raw.button_flags == HORIZON_RI_MOUSE_RIGHT_DOWN );
    assert( raw.extra_information == 0x1234 );

    /* The middle button, which the presets bind to a third mouse button. */
    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, &raw );
    assert( raw.button_flags == HORIZON_RI_MOUSE_MIDDLE_DOWN );
    horizon_raw_mouse_event( HORIZON_MOUSEEVENTF_MIDDLEUP, 0, 0, 0, &raw );
    assert( raw.button_flags == HORIZON_RI_MOUSE_MIDDLE_UP );

    printf( "raw mouse: the movement asked for, the buttons that changed, and the SDK's layout\n" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'raw_mouse.c'
    source.write_text(fixture.replace('@HEADER@', header))
    binary = Path(tmp) / 'raw_mouse'
    subprocess.run(['cc', '-o', str(binary), str(source)], check=True)
    print(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip())
