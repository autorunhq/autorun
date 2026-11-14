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
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef unsigned long ULONG_PTR;
typedef void *HWND, *HCURSOR, *HKL;
typedef int NTSTATUS;
typedef unsigned int user_handle_t;
#define STATUS_PENDING 0x103
struct object_lock { int id; };
#define OBJECT_LOCK_INIT {0}
typedef struct { user_handle_t cursor; int cursor_count; } input_shm_t;
typedef struct { int x, y; } POINT;
typedef struct { POINT cursor; } desktop_shm_t;
typedef struct { DWORD cbSize; DWORD flags; HWND hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; int rcCaret[4]; } GUITHREADINFO;
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
static desktop_shm_t shared_desktop = { .cursor = { 640, 360 } };
static NTSTATUS shared_desktop_status = -1;
static NTSTATUS get_shared_desktop(struct object_lock *lock, const desktop_shm_t **desktop) {
    if (shared_desktop_status) return shared_desktop_status;
    if (!lock->id) { lock->id = 1; *desktop = &shared_desktop; return STATUS_PENDING; }
    return 0;
}
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
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
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
static int mouse_flushes;
static int reenter;
static DWORD mock_tick;
BOOL wine_nx_drv_ProcessEvents(DWORD mask);
static int wine_nx_pointer_poll(int *x, int *y, unsigned int *buttons) {
    polls++; *x = state.x; *y = state.y; *buttons = state.buttons; return state.moved;
}
static void wine_nx_input_take(struct input_snapshot *out) { *out = state; }
static void wine_nx_pointer_set_pos(int x, int y) { set_x = x; set_y = y; }
static void wine_nx_fb_present(void) { presents++; }
static void nxdrv_trace(const char *fmt, int a, int b, int c, int d) {}
static void nxdrv_trace_hot(const char *fmt, int a, int b, int c, int d) {}
static void nxdrv_say(const char *line) { (void)line; }
static void nxdrv_note_key_sent(WORD vk, BOOL up) { (void)vk; (void)up; }
static void wine_nx_fit_screen_to_game(void) {}
static void nxdrv_check_key_taken(void) {}
static void nxdrv_trace_key_target(void) {}
static DWORD NtGetTickCount(void) { return mock_tick; }
static DWORD GetCurrentThreadId(void) { return 1; }
static BOOL NtUserGetGUIThreadInfo(DWORD tid, GUITHREADINFO *info) { (void)tid; (void)info; return FALSE; }
static HWND NtUserGetForegroundWindow(void) { return NULL; }
static BOOL get_async_keyboard_state(BYTE s[256]) { (void)s; return TRUE; }
static struct { unsigned short vk; int up; } osk_queue[8];
static int osk_head, osk_tail;
int test_osk_next_key(unsigned long long now_ns, unsigned short *vk, int *up) {
    (void)now_ns;
    if (osk_head >= osk_tail) return 0;
    *vk = osk_queue[osk_head].vk; *up = osk_queue[osk_head].up; osk_head++;
    return 1;
}
static HKL NtUserGetKeyboardLayout(DWORD id) { return (HKL)1; }
static UINT NtUserMapVirtualKeyEx(UINT vk, UINT type, HKL layout) {
    assert(type == MAPVK_VK_TO_VSC_EX && layout == (HKL)1);
    if (vk == 0xa3) return 0xe01d;
    if (vk == VK_RETURN) return 0x1c;
    return 0x1e;
}
static UINT NtUserSendHardwareInput(HWND hwnd, UINT flags, const INPUT *input, LPARAM lparam) {
    assert(!hwnd && !flags && !lparam && sent_count < 128);
    if (input->type == INPUT_MOUSE && input->mi.dwFlags == MOUSEEVENTF_MOVE_NOCOALESCE)
    {
        mouse_flushes++;
        return TRUE;
    }
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
    int flushes_before;
    assert(poll(0, 640, 360, 0) == -1);
    assert(presents == 2 && polls == 1);  /* presents before and after; polls even with the background thread */
    flushes_before = mouse_flushes;
    assert(poll(1, 700, 360, 0) == (ABS | MOUSEEVENTF_MOVE));
    assert(mouse_flushes == flushes_before + 1);  /* otherwise win32u can leave the move coalesced */
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
    /* A touch elsewhere moves and presses in one event; lifting releases. Both
     * touch events must flush too, not just emit MOUSEEVENTF_ABSOLUTE. */
    flushes_before = mouse_flushes;
    assert(poll(1, 100, 50, L) == (ABS | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN));
    assert(mouse_flushes == flushes_before + 1);
    flushes_before = mouse_flushes;
    assert(poll(0, 100, 50, 0) == (ABS | MOUSEEVENTF_LEFTUP));
    assert(mouse_flushes == flushes_before + 1);
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

    /* OSK stops after releasing a key so a polling loop sees the press first. */
    sent_count = 0;
    state = (struct input_snapshot){0};
    osk_head = 0; osk_tail = 4;
    osk_queue[0] = (typeof(osk_queue[0])){ 'A', 0 };
    osk_queue[1] = (typeof(osk_queue[1])){ 'A', 1 };
    osk_queue[2] = (typeof(osk_queue[2])){ 'B', 0 };
    osk_queue[3] = (typeof(osk_queue[3])){ 'B', 1 };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 2);
    assert(sent[0].ki.wVk == 'A' && !(sent[0].ki.dwFlags & KEYEVENTF_KEYUP));
    assert(sent[1].ki.wVk == 'A' && (sent[1].ki.dwFlags & KEYEVENTF_KEYUP));
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 4);
    assert(sent[2].ki.wVk == 'B' && !(sent[2].ki.dwFlags & KEYEVENTF_KEYUP));
    assert(sent[3].ki.wVk == 'B' && (sent[3].ki.dwFlags & KEYEVENTF_KEYUP));

    POINT gp = {0};
    shared_desktop_status = 0;
    assert(wine_nx_drv_GetCursorPos(&gp) && gp.x == 640 && gp.y == 360);
    shared_desktop_status = -1;

    /* Relative delta motion without placed=1 delivers relative MOUSEEVENTF_MOVE (no ABSOLUTE) and returns TRUE,
     * even if the visual cursor sat at the screen edge (moved=0). */
    state = (struct input_snapshot){ .x = 1279, .y = 360, .dx = 25, .dy = -10, .moved = 0 };
    int before_rel = sent_count;
    assert(wine_nx_drv_ProcessEvents(0));
    assert(sent_count == before_rel + 1);
    assert(sent[before_rel].mi.dwFlags == MOUSEEVENTF_MOVE);
    assert(sent[before_rel].mi.dx == 25 && sent[before_rel].mi.dy == -10);

    /* A pressed after the stick moved, both seen since the last call: the
     * movement goes first, so the press is where the arrow is drawn. */
    sent_count = 0;
    state = (struct input_snapshot){ .x = 300, .y = 200, .dx = 40, .dy = 12, .moved = 1, .buttons = L, .pressed = L };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 2);
    assert(sent[0].mi.dwFlags == MOUSEEVENTF_MOVE && sent[0].mi.dx == 40 && sent[0].mi.dy == 12);
    assert(sent[1].mi.dwFlags == (ABS | MOUSEEVENTF_LEFTDOWN));
    /* ...and a tap during it is a click there, not a drag across what the stick covered. */
    sent_count = 0;
    state = (struct input_snapshot){ .x = 320, .y = 200, .dx = 20, .moved = 1, .pressed = R, .released = R };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 3);
    assert(sent[0].mi.dwFlags == MOUSEEVENTF_MOVE && sent[0].mi.dx == 20 && !sent[0].mi.dy);
    assert(sent[1].mi.dwFlags == (ABS | MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTDOWN));  /* A let go where the drag ended */
    assert(sent[2].mi.dwFlags == (ABS | MOUSEEVENTF_RIGHTUP));
    /* A finger places the cursor; what the stick did is added after it. */
    sent_count = 0;
    state = (struct input_snapshot){ .x = 500, .y = 400, .dx = 3, .dy = 4, .moved = 1, .placed = 1, .buttons = L, .pressed = L };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 2);
    assert(sent[0].mi.dwFlags == (ABS | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN) && sent[0].mi.dx == 500 && sent[0].mi.dy == 400);
    assert(sent[1].mi.dwFlags == MOUSEEVENTF_MOVE && sent[1].mi.dx == 3 && sent[1].mi.dy == 4);
    sent_count = 0;
    state = (struct input_snapshot){ .x = 500, .y = 400 };
    assert(wine_nx_drv_ProcessEvents(0) && sent_count == 1 && sent[0].mi.dwFlags == (ABS | MOUSEEVENTF_LEFTUP));

    puts("PASS: stick moves, A left and B right clicks, drag, touch press, clicks between calls, cursor visibility, "
         "relative dock mouse motion and SetCursorPos");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-pointer-') as temp:
    src = Path(temp) / 'test.c'
    osk = Path(temp) / 'osk_stub.c'
    exe = Path(temp) / 'test'
    src.write_text(fixture + defines + events + tests)
    osk.write_text(
        'int test_osk_next_key(unsigned long long now_ns, unsigned short *vk, int *up);\n'
        'int wine_nx_osk_next_key(unsigned long long now_ns, unsigned short *vk, int *up) {\n'
        '    return test_osk_next_key(now_ns, vk, up);\n'
        '}\n'
        'unsigned long long wine_nx_osk_clock(void) { return 1000; }\n'
    )
    subprocess.run(['cc', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-I', str(root / 'horizon-wine/source'), str(src), str(osk), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

runtime_src = (root / 'horizon-wine/source/runtime.c').read_text()
dock_start = runtime_src.index('/* A mouse plugged into the dock')
dock_end = runtime_src.index('/* Two fingers scroll the picture under them', dock_start)
dock_block = runtime_src[dock_start:dock_end]

dock_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pointer_cursor.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

enum { HidMouseAttribute_IsConnected = 1 };
enum { HidMouseButton_Left = 1, HidMouseButton_Right = 2, HidMouseButton_Middle = 4 };
enum { WINE_NX_POINTER_LEFT = 1, WINE_NX_POINTER_RIGHT = 2, WINE_NX_POINTER_MIDDLE = 4 };
typedef struct {
    u64 sampling_number;
    s32 x, y;
    s32 delta_x, delta_y;
    s32 wheel_delta_x, wheel_delta_y;
    u32 buttons;
    u32 attributes;
} HidMouseState;

static HidMouseState mock_states[17];
static size_t mock_count;
static size_t hidGetMouseStates(HidMouseState *out, size_t max_count) {
    size_t n = mock_count < max_count ? mock_count : max_count;
    if (n) memcpy(out, mock_states, n * sizeof(*out));
    return n;
}
static void log_line(const char *fmt, ...) { (void)fmt; }

static struct pointer_cursor wine_nx_pointer = { .width = 1280, .height = 720, .x = 640, .y = 360 };
static int wine_nx_pointer_placed;
static int runtime_wheel[2];

static int run_dock_mouse_poll(int on_keyboard, int controls_open, int tasks_open, int finger_down,
                               int *moved_out, int *buttons_out)
{
    int moved = 0;
    struct { unsigned int buttons; } output = { 0 };
''' + dock_block + r'''
    *moved_out = moved;
    *buttons_out = output.buttons;
    return 0;
}

int main(void) {
    int moved = 0, buttons = 0, dx = 0, dy = 0;

    /* Initial connection places cursor once at OS coordinates and marks placed. */
    mock_count = 1;
    mock_states[0] = (HidMouseState){ .sampling_number = 10, .x = 500, .y = 300, .delta_x = 12, .delta_y = -4,
                                      .attributes = HidMouseAttribute_IsConnected };
    run_dock_mouse_poll(0, 0, 0, 0, &moved, &buttons);
    assert(wine_nx_pointer.x == 500 && wine_nx_pointer.y == 300);
    assert(wine_nx_pointer_placed == 1 && moved == 1);
    wine_nx_pointer_placed = 0;

    /* Game warps cursor to center via SetCursorPos; idle poll must NOT snap back to mouse.x/y. */
    pointer_cursor_warp(&wine_nx_pointer, 500, 300, 640, 360);
    run_dock_mouse_poll(0, 0, 0, 0, &moved, &buttons);
    assert(wine_nx_pointer.x == 640 && wine_nx_pointer.y == 360);
    assert(!wine_nx_pointer_placed && !moved);

    /* Multiple unseen LIFO samples (newest at index 0) sum delta_x/y and wheel deltas without placed=1. */
    mock_count = 4;
    mock_states[0] = (HidMouseState){ .sampling_number = 13, .x = 1279, .y = 0, .delta_x = 15, .delta_y = 2,
                                      .wheel_delta_y = 1, .buttons = HidMouseButton_Left,
                                      .attributes = HidMouseAttribute_IsConnected };
    mock_states[1] = (HidMouseState){ .sampling_number = 12, .x = 1279, .y = 0, .delta_x = 10, .delta_y = -5,
                                      .wheel_delta_x = -2, .attributes = HidMouseAttribute_IsConnected };
    mock_states[2] = (HidMouseState){ .sampling_number = 11, .x = 1279, .y = 0, .delta_x = 5, .delta_y = -1,
                                      .attributes = HidMouseAttribute_IsConnected };
    mock_states[3] = (HidMouseState){ .sampling_number = 10, .x = 500, .y = 300, .delta_x = 999, .delta_y = 999,
                                      .attributes = HidMouseAttribute_IsConnected };
    run_dock_mouse_poll(0, 0, 0, 0, &moved, &buttons);
    assert(moved == 1 && !wine_nx_pointer_placed);
    assert(buttons == WINE_NX_POINTER_LEFT);
    assert(runtime_wheel[0] == 120 && runtime_wheel[1] == -240);
    assert(pointer_cursor_take_motion(&wine_nx_pointer, &dx, &dy) && dx == 30 && dy == -4);
    assert(wine_nx_pointer.x == 670 && wine_nx_pointer.y == 356);

    /* Samples arriving while OSK is open are drained, not replayed as a jump when OSK closes. */
    mock_count = 1;
    mock_states[0] = (HidMouseState){ .sampling_number = 14, .x = 0, .y = 0, .delta_x = 100, .delta_y = 100,
                                      .wheel_delta_y = 2, .attributes = HidMouseAttribute_IsConnected };
    run_dock_mouse_poll(1, 0, 0, 0, &moved, &buttons);
    assert(!moved && !buttons && runtime_wheel[0] == 120);
    run_dock_mouse_poll(0, 0, 0, 0, &moved, &buttons);
    assert(!moved && !pointer_cursor_take_motion(&wine_nx_pointer, &dx, &dy));

    /* Even pinned at right screen edge (1279), physical delta keeps turning mouse-look view. */
    pointer_cursor_place(&wine_nx_pointer, 1279, 360);
    mock_states[0] = (HidMouseState){ .sampling_number = 15, .x = 1279, .y = 360, .delta_x = 40, .delta_y = 0,
                                      .attributes = HidMouseAttribute_IsConnected };
    run_dock_mouse_poll(0, 0, 0, 0, &moved, &buttons);
    assert(moved == 1 && !wine_nx_pointer_placed);
    assert(pointer_cursor_take_motion(&wine_nx_pointer, &dx, &dy) && dx == 40 && dy == 0);

    puts("PASS: physical dock mouse drains LIFO deltas, keeps SetCursorPos warps, and works at screen edges");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-dock-mouse-') as temp:
    src = Path(temp) / 'dock.c'
    exe = Path(temp) / 'dock'
    src.write_text(dock_fixture)
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-I', str(root / 'horizon-wine/source'), str(src), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
