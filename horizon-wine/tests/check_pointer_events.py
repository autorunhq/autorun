#!/usr/bin/env python3
"""Run the NX driver's pointer event translation against scripted controller states."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/win32u/winnx_drv.c').read_text()
defines = source[source.index('/* Buttons reported by wine_nx_pointer_poll(). */'):source.index('struct wine_nx_surface\n')]
events = source[source.index('/* The button flags that take the delivered buttons'):source.index('/**********************************************************************\n *           wine_nx_drv_CreateWindow')]
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include "input_events.h"
typedef int BOOL, INT;
typedef unsigned int UINT, DWORD;
typedef unsigned char BYTE;
typedef void *HWND, *HCURSOR, *HKL;
typedef int NTSTATUS;
typedef unsigned int user_handle_t;
#define STATUS_PENDING 0x103
struct object_lock { int id; };
#define OBJECT_LOCK_INIT {0}
typedef struct { user_handle_t cursor; int cursor_count; } input_shm_t;
typedef struct { int x, y; } POINT;
typedef struct { POINT cursor; } desktop_shm_t;
#define wine_server_ptr_handle(h) ((void *)(unsigned long)(h))
/* The input state the program's SetCursor and ShowCursor calls leave. */
static input_shm_t shared_input;
static NTSTATUS shared_input_status;
static NTSTATUS get_shared_input(unsigned int tid, struct object_lock *lock, const input_shm_t **input_shm) {
    assert(!tid);
    if (shared_input_status) return shared_input_status;
    if (!lock->id) { lock->id = 1; *input_shm = &shared_input; return STATUS_PENDING; }
    return 0;
}
static int cursor_shown = 1;
static void wine_nx_cursor_show(int visible) { cursor_shown = visible; }
static NTSTATUS get_shared_desktop(struct object_lock *lock, const desktop_shm_t **desktop) { return -1; }
static void wine_nx_pointer_follow(int x, int y) { assert(0); }
extern void wine_nx_runtime_trace(const char *line) __attribute__((weak));
typedef long LPARAM;
#define TRUE 1
#define FALSE 0
#define INPUT_MOUSE 0
#define INPUT_KEYBOARD 1
#define VK_RETURN 0x0d
#define MAPVK_VK_TO_VSC_EX 4
#define KEYEVENTF_EXTENDEDKEY 0x0001
#define KEYEVENTF_KEYUP 0x0002
#define MOUSEEVENTF_MOVE 0x0001
#define MOUSEEVENTF_LEFTDOWN 0x0002
#define MOUSEEVENTF_LEFTUP 0x0004
#define MOUSEEVENTF_RIGHTDOWN 0x0008
#define MOUSEEVENTF_RIGHTUP 0x0010
#define MOUSEEVENTF_MIDDLEDOWN 0x0020
#define MOUSEEVENTF_MIDDLEUP 0x0040
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP 0x0100
#define MOUSEEVENTF_WHEEL 0x0800
#define MOUSEEVENTF_HWHEEL 0x1000
#define MOUSEEVENTF_ABSOLUTE 0x8000
#define XBUTTON1 1
#define XBUTTON2 2
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef struct { int dx, dy; DWORD mouseData, dwFlags, time; unsigned long dwExtraInfo; } MOUSEINPUT;
typedef struct { unsigned short wVk, wScan; DWORD dwFlags; } KEYBDINPUT;
typedef struct { DWORD type; union { MOUSEINPUT mi; KEYBDINPUT ki; }; } INPUT;
/* What the runtime's polls saw since the last take (the background thread's included). */
static struct input_snapshot state;
static INPUT sent[128];
static int sent_count, presents, polls, set_x = -1, set_y = -1;
static int reenter;
BOOL wine_nx_drv_ProcessEvents(DWORD mask);
static int wine_nx_pointer_poll(int *x, int *y, unsigned int *buttons) {
    polls++; *x = state.x; *y = state.y; *buttons = state.buttons; return state.moved;
}
static void wine_nx_input_take(struct input_snapshot *out) { *out = state; }
static void wine_nx_pointer_set_pos(int x, int y) { set_x = x; set_y = y; }
static void wine_nx_fb_present(void) { presents++; }
static void nxdrv_trace(const char *fmt, int a, int b, int c, int d) {}
static void nxdrv_trace_hot(const char *fmt, int a, int b, int c, int d) {}
static HKL NtUserGetKeyboardLayout(DWORD id) { return (HKL)1; }
static UINT NtUserMapVirtualKeyEx(UINT vk, UINT type, HKL layout) {
    assert(type == MAPVK_VK_TO_VSC_EX && layout == (HKL)1);
    if (vk == 0xa3) return 0xe01d;
    if (vk == VK_RETURN) return 0x1c;
    return 0x1e;
}
static UINT NtUserSendHardwareInput(HWND hwnd, UINT flags, const INPUT *input, LPARAM lparam) {
    assert(!hwnd && !flags && !lparam && sent_count < 128);
    if (reenter) { reenter = 0; assert(!wine_nx_drv_ProcessEvents(0)); }
    sent[sent_count++] = *input; return TRUE;
}
'''
tests = r'''
/* Call ProcessEvents once with what the polls saw; return the flags of the
 * events sent (second << 32 | first), or -1 for none. */
static long long take(int moved, int x, int y, unsigned int held, unsigned int pressed, unsigned int released) {
    long long flags = 0;
    int before = sent_count, i;
    BOOL ret;
    state = (struct input_snapshot){ .moved = moved, .placed = moved, .x = x, .y = y,
        .buttons = held, .pressed = pressed, .released = released };
    ret = wine_nx_drv_ProcessEvents(0);
    assert(sent_count - before <= 2);
    assert(ret == (sent_count != before));
    if (sent_count == before) return -1;
    for (i = before; i < sent_count; i++) {
        assert(sent[i].mi.dx == x && sent[i].mi.dy == y);
        flags |= (long long)sent[i].mi.dwFlags << (32 * (i - before));
    }
    return flags;
}
/* One poll between calls: the edges follow from the previous buttons. */
static unsigned int previous;
static long long poll(int moved, int x, int y, unsigned int buttons) {
    long long flags = take(moved, x, y, buttons, buttons & ~previous, previous & ~buttons);
    previous = buttons;
    return flags;
}
int main(void) {
    enum { L = WINE_NX_POINTER_LEFT, R = WINE_NX_POINTER_RIGHT };
    const long long ABS = MOUSEEVENTF_ABSOLUTE;
    assert(poll(0, 640, 360, 0) == -1);
    assert(presents == 2 && polls == 1);  /* presents before and after; polls even with the background thread */
    assert(poll(1, 700, 360, 0) == (ABS | MOUSEEVENTF_MOVE));
    /* A clicks where the cursor is, without a spurious move. */
    assert(poll(0, 700, 360, L) == (ABS | MOUSEEVENTF_LEFTDOWN));
    assert(poll(0, 700, 360, L) == -1);
    /* Holding A while the stick moves drags. */
    assert(poll(1, 720, 380, L) == (ABS | MOUSEEVENTF_MOVE));
    assert(poll(0, 720, 380, L | R) == (ABS | MOUSEEVENTF_RIGHTDOWN));
    assert(poll(0, 720, 380, R) == (ABS | MOUSEEVENTF_LEFTUP));
    assert(poll(0, 720, 380, 0) == (ABS | MOUSEEVENTF_RIGHTUP));
    /* B alone is a right click. */
    assert(poll(0, 720, 380, R) == (ABS | MOUSEEVENTF_RIGHTDOWN));
    assert(poll(0, 720, 380, 0) == (ABS | MOUSEEVENTF_RIGHTUP));
    /* A touch elsewhere moves and presses in one event; lifting releases. */
    assert(poll(1, 100, 50, L) == (ABS | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN));
    assert(poll(0, 100, 50, 0) == (ABS | MOUSEEVENTF_LEFTUP));
    /* A tapped between two calls (seen only by the background thread) still clicks. */
    assert(take(0, 100, 50, 0, L, L) == (ABS | MOUSEEVENTF_LEFTDOWN | (ABS | MOUSEEVENTF_LEFTUP) << 32));
    /* ...also while the stick moves, and for B. */
    assert(take(1, 110, 50, 0, R, R) == (ABS | MOUSEEVENTF_MOVE | MOUSEEVENTF_RIGHTDOWN | (ABS | MOUSEEVENTF_RIGHTUP) << 32));
    /* A held, released and pressed again between calls: up, then down. */
    assert(take(0, 110, 50, L, L, 0) == (ABS | MOUSEEVENTF_LEFTDOWN));
    assert(take(0, 110, 50, L, L, L) == (ABS | MOUSEEVENTF_LEFTUP | (ABS | MOUSEEVENTF_LEFTDOWN) << 32));
    /* Nothing new: nothing sent. */
    assert(take(0, 110, 50, L, 0, 0) == -1);
    /* Another thread took the release: the state still catches up. */
    assert(take(0, 110, 50, 0, 0, 0) == (ABS | MOUSEEVENTF_LEFTUP));
    /* The arrow follows the cursor the program set: shown until a program sets
     * one, hidden when it sets none over its own cursor or hides it. */
    assert(cursor_shown);
    shared_input.cursor = 0x10010;           /* a class cursor */
    take(0, 110, 50, 0, 0, 0);
    assert(cursor_shown);
    shared_input.cursor = 0;                 /* SetCursor(NULL), as OpenTTD over its own */
    take(0, 110, 50, 0, 0, 0);
    assert(!cursor_shown);
    shared_input.cursor = 0x10010;
    shared_input.cursor_count = -1;          /* ShowCursor(FALSE) */
    take(0, 110, 50, 0, 0, 0);
    assert(!cursor_shown);
    shared_input.cursor_count = 0;
    wine_nx_drv_SetCursor(0, (HCURSOR)0x10010);
    assert(cursor_shown);
    shared_input_status = (NTSTATUS)0xc0000034;  /* unreadable: leave it as it is */
    shared_input.cursor = 0;
    take(0, 110, 50, 0, 0, 0);
    assert(cursor_shown);
    shared_input_status = 0;
    assert(wine_nx_drv_SetCursorPos(12, 34) && set_x == 12 && set_y == 34);

    /* Relative motion must stay relative, including when the game clips its cursor. */
    sent_count = 0;
    state = (struct input_snapshot){ .moved = 1, .dx = 7, .dy = -3 };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 1);
    assert(sent[0].mi.dwFlags == MOUSEEVENTF_MOVE && sent[0].mi.dx == 7 && sent[0].mi.dy == -3);

    /* Modifier-click chords enclose the mouse events and cannot interleave on reentry. */
    sent_count = 0;
    reenter = 1;
    state = (struct input_snapshot){ .pressed = L, .released = L, .key_count = 2,
        .keys = {{0xa3, 1}, {0xa3, 0}} };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 4 && !reenter);
    assert(sent[0].type == INPUT_KEYBOARD && sent[0].ki.wVk == 0xa3 && sent[0].ki.wScan == 0x1d);
    assert(sent[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY);
    assert(sent[1].type == INPUT_MOUSE && sent[1].mi.dwFlags == (ABS | MOUSEEVENTF_LEFTDOWN));
    assert(sent[2].type == INPUT_MOUSE && sent[2].mi.dwFlags == (ABS | MOUSEEVENTF_LEFTUP));
    assert(sent[3].type == INPUT_KEYBOARD && sent[3].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP));

    sent_count = 0;
    state = (struct input_snapshot){ .key_count = 2, .keys = {{INPUT_NUMPAD_ENTER, 1}, {INPUT_NUMPAD_ENTER, 0}} };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 2);
    assert(sent[0].ki.wVk == VK_RETURN && sent[0].ki.wScan == 0x1c && sent[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY);
    assert(sent[1].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP));

    sent_count = 0;
    state = (struct input_snapshot){ .pressed = 4 | 8 | 16, .released = 4 | 8 | 16, .wheel = 120, .hwheel = -120 };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 8);
    assert(sent[0].mi.dwFlags == (ABS | MOUSEEVENTF_MIDDLEDOWN));
    assert(sent[1].mi.dwFlags == (ABS | MOUSEEVENTF_MIDDLEUP));
    assert(sent[2].mi.dwFlags == MOUSEEVENTF_XDOWN && sent[2].mi.mouseData == XBUTTON1);
    assert(sent[3].mi.dwFlags == MOUSEEVENTF_XUP && sent[3].mi.mouseData == XBUTTON1);
    assert(sent[4].mi.dwFlags == MOUSEEVENTF_XDOWN && sent[4].mi.mouseData == XBUTTON2);
    assert(sent[5].mi.dwFlags == MOUSEEVENTF_XUP && sent[5].mi.mouseData == XBUTTON2);
    assert(sent[6].mi.dwFlags == MOUSEEVENTF_WHEEL && sent[6].mi.mouseData == 120);
    assert(sent[7].mi.dwFlags == MOUSEEVENTF_HWHEEL && (int)sent[7].mi.mouseData == -120);
    puts("PASS: stick moves, A left and B right clicks, drag, touch press, clicks between calls, cursor visibility "
         "and SetCursorPos");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-pointer-') as temp:
    src = Path(temp) / 'test.c'
    exe = Path(temp) / 'test'
    src.write_text(fixture + defines + events + tests)
    subprocess.run(['cc', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-I', str(root / 'horizon-wine/source'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
