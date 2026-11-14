#!/usr/bin/env python3
"""SaltyNX and FPSLocker for a program Autorun runs (horizon-wine/source/
fpslocker.h, saltynx.h, saltynx.c): FPSLocker's block in SaltyNX's shared
memory, laid out as NxFpsSharedBlock is, found, or made anew as SaltyNX and
FPSLocker make it with the title's saved settings, and the frame rate written
into it; the panel rate display sync asks for; the swap interval and the
waiting that keep FPSLocker's target; the requests to SaltySD's port and its
replies, as libnx's old IPC lays them out. Then that the runtime starts and
stops the bridge, that it never writes FPSLocker's own fields, and that both
present paths, OpenGL's and Vulkan's, keep the target."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'horizon-wine/source'
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "fpslocker.h"
#include "saltynx.h"

/* NxFpsSharedBlock as SaltyNX declares it (saltysd_proc/source/useful.h). */
struct resolution_calls { uint16_t width, height, calls; };
struct nx_fps_shared_block
{
    uint32_t MAGIC;
    uint8_t FPS;
    float FPSavg;
    bool pluginActive;
    uint8_t FPSlocked;
    uint8_t FPSmode;
    uint8_t ZeroSync;
    uint8_t patchApplied;
    uint8_t API;
    uint32_t FPSticks[10];
    uint8_t Buffers;
    uint8_t SetBuffers;
    uint8_t ActiveBuffers;
    uint8_t SetActiveBuffers;
    uint8_t displaySync;
    struct resolution_calls renderCalls[8];
    struct resolution_calls viewportCalls[8];
    bool forceOriginalRefreshRate;
    bool dontForce60InDocked;
    bool forceSuspend;
    uint8_t currentRefreshRate;
    float readSpeedPerSecond;
    uint8_t FPSlockedDocked;
    uint64_t frameNumber;
    int8_t expectedSetBuffers;
} __attribute__((packed));

static uint8_t shared[FPSLOCKER_SHARED_SIZE];

static void put32( uint32_t *buffer, unsigned int byte, uint64_t value ) { memcpy( (uint8_t *)buffer + byte, &value, 8 ); }

int main(void)
{
    struct nx_fps_shared_block block;
    uint32_t buffer[0x40], handle, magic;
    uint64_t due, raw[4], extra[2];
    uint8_t *found, dat[5] = { 30, 1, 2, 7, 40 };
    unsigned int i;
    float average;

    /* The layout, field by field. */
    assert( sizeof(block) == FPSLOCKER_BLOCK_SIZE );
    assert( offsetof(struct nx_fps_shared_block, FPS) == FPSLOCKER_FPS );
    assert( offsetof(struct nx_fps_shared_block, FPSavg) == FPSLOCKER_FPS_AVG );
    assert( offsetof(struct nx_fps_shared_block, pluginActive) == FPSLOCKER_PLUGIN_ACTIVE );
    assert( offsetof(struct nx_fps_shared_block, FPSlocked) == FPSLOCKER_TARGET );
    assert( offsetof(struct nx_fps_shared_block, ZeroSync) == FPSLOCKER_ZERO_SYNC );
    assert( offsetof(struct nx_fps_shared_block, API) == FPSLOCKER_API );
    assert( offsetof(struct nx_fps_shared_block, SetBuffers) == FPSLOCKER_SET_BUFFERS );
    assert( offsetof(struct nx_fps_shared_block, displaySync) == FPSLOCKER_DISPLAY_SYNC );
    assert( offsetof(struct nx_fps_shared_block, forceOriginalRefreshRate) == FPSLOCKER_FORCE_ORIGINAL );
    assert( offsetof(struct nx_fps_shared_block, dontForce60InDocked) == FPSLOCKER_DONT_FORCE_60 );
    assert( offsetof(struct nx_fps_shared_block, forceSuspend) == FPSLOCKER_FORCE_SUSPEND );
    assert( offsetof(struct nx_fps_shared_block, FPSlockedDocked) == FPSLOCKER_TARGET_DOCKED );
    assert( offsetof(struct nx_fps_shared_block, frameNumber) == FPSLOCKER_FRAME_NUMBER );
    assert( offsetof(struct nx_fps_shared_block, expectedSetBuffers) == FPSLOCKER_EXPECTED_BUFFERS );

    /* Found by its magic on a 4-byte boundary only, never past the memory. */
    assert( !fpslocker_find_block( NULL ) && !fpslocker_find_block( shared ) );
    magic = FPSLOCKER_MAGIC;
    memcpy( shared + 6, &magic, 4 );
    assert( !fpslocker_find_block( shared ) );
    memcpy( shared + FPSLOCKER_SHARED_SIZE - FPSLOCKER_BLOCK_SIZE + 4, &magic, 4 );
    assert( !fpslocker_find_block( shared ) );
    memcpy( shared + 0x40, &magic, 4 );
    assert( fpslocker_find_block( shared ) == shared + 0x40 );
    memset( shared, 0, sizeof(shared) );

    /* Made where FPSLocker makes it, with the title's saved settings: the target
     * for both modes, then ZeroSync, SetBuffers, forceSuspend, the docked one.
     * Whatever an earlier program left is cleared, SaltyNX's four bytes kept. */
    memset( shared, 0x55, sizeof(shared) );
    memcpy( shared + 0x80, &magic, 4 );
    shared[FPSLOCKER_PANEL_RATE] = 60;
    found = fpslocker_make_block( shared, dat, sizeof(dat) );
    assert( found == shared + 4 && fpslocker_find_block( shared ) == found );
    assert( shared[0] == 0x55 && shared[FPSLOCKER_PANEL_RATE] == 60 && shared[2] == 0x55 && shared[3] == 0x55 );
    for (i = 4 + FPSLOCKER_BLOCK_SIZE; i < FPSLOCKER_SHARED_SIZE; i++) assert( !shared[i] );
    memcpy( &block, found, sizeof(block) );
    assert( block.MAGIC == FPSLOCKER_MAGIC && block.FPSlocked == 30 && block.FPSlockedDocked == 40 );
    assert( block.ZeroSync == 1 && block.SetBuffers == 2 && block.forceSuspend == 1 );
    assert( block.expectedSetBuffers == -1 && !block.pluginActive && !block.displaySync );
    assert( shared[FPSLOCKER_PANEL_RATE] == 60 );
    /* A file of one byte: one target for both modes. None: no target. */
    memset( shared, 0, sizeof(shared) );
    found = fpslocker_make_block( shared, dat, 1 );
    assert( found[FPSLOCKER_TARGET] == 30 && found[FPSLOCKER_TARGET_DOCKED] == 30 && !found[FPSLOCKER_ZERO_SYNC] );
    memset( shared, 0xaa, sizeof(shared) );
    found = fpslocker_make_block( shared, NULL, 0 );
    memcpy( &block, found, sizeof(block) );
    assert( block.MAGIC == FPSLOCKER_MAGIC && !block.FPSlocked && !block.FPSlockedDocked && !block.FPS );
    assert( block.expectedSetBuffers == -1 && shared[0] == 0xaa && !shared[4 + FPSLOCKER_BLOCK_SIZE] );

    /* The frame rate: active while frames come, held at 255, the API kept when
     * none drew this time. */
    fpslocker_publish( found, 59, 59.4f, FPSLOCKER_API_EGL, 1234 );
    memcpy( &block, found, sizeof(block) );
    average = block.FPSavg;
    assert( block.FPS == 59 && average == 59.4f && block.pluginActive && block.API == FPSLOCKER_API_EGL );
    assert( block.frameNumber == 1234 );
    fpslocker_publish( found, 300, 300.0f, 0, 1300 );
    assert( found[FPSLOCKER_FPS] == 255 && found[FPSLOCKER_API] == FPSLOCKER_API_EGL );
    fpslocker_publish( found, 0, 0.0f, FPSLOCKER_API_VULKAN, 1300 );
    assert( !found[FPSLOCKER_PLUGIN_ACTIVE] && found[FPSLOCKER_API] == FPSLOCKER_API_VULKAN );

    /* Display sync's panel rate: the mode's target, 60 with none or with the
     * game's own rate forced (unless docked and told not to); nothing with
     * display sync off. */
    memset( shared, 0, sizeof(shared) );
    found = fpslocker_make_block( shared, dat, sizeof(dat) );
    assert( fpslocker_sync_rate( found, 0, 1 ) == 30 && fpslocker_sync_rate( found, 1, 1 ) == 40 );
    assert( fpslocker_sync_rate( found, 0, 0 ) == 0 && fpslocker_sync_rate( found, 1, 0 ) == 0 );
    found[FPSLOCKER_FORCE_ORIGINAL] = 1;
    assert( fpslocker_sync_rate( found, 0, 1 ) == 60 && fpslocker_sync_rate( found, 1, 1 ) == 60 );
    found[FPSLOCKER_DONT_FORCE_60] = 1;
    assert( fpslocker_sync_rate( found, 0, 1 ) == 60 && fpslocker_sync_rate( found, 1, 1 ) == 40 );
    found[FPSLOCKER_TARGET] = found[FPSLOCKER_TARGET_DOCKED] = 0;
    assert( fpslocker_sync_rate( found, 0, 1 ) == 60 && fpslocker_sync_rate( found, 1, 1 ) == 60 );

    /* The swap interval: the panel at the target or a multiple of it up to 4x. */
    assert( fpslocker_swap_interval( 30, 60 ) == 2 && fpslocker_swap_interval( 40, 40 ) == 1 );
    assert( fpslocker_swap_interval( 60, 60 ) == 1 && fpslocker_swap_interval( 15, 60 ) == 4 );
    assert( fpslocker_swap_interval( 45, 90 ) == 2 && fpslocker_swap_interval( 12, 60 ) == 0 );
    assert( fpslocker_swap_interval( 40, 60 ) == 0 && fpslocker_swap_interval( 72, 60 ) == 0 );
    assert( fpslocker_swap_interval( 60, 0 ) == 0 && fpslocker_swap_interval( 0, 60 ) == 0 );
    assert( fpslocker_swap_interval( -1, 60 ) == 0 );

    /* Waiting: a frame early waits for its time, a frame on time or a little
     * late goes at once and keeps the count, one more than a period late starts
     * it again. */
    due = 0;
    assert( fpslocker_pace( &due, 1000, 100 ) == 0 && due == 1100 );
    assert( fpslocker_pace( &due, 1040, 100 ) == 60 && due == 1200 );
    assert( fpslocker_pace( &due, 1230, 100 ) == 0 && due == 1300 );
    assert( fpslocker_pace( &due, 1500, 100 ) == 0 && due == 1600 );
    assert( fpslocker_pace( &due, 1500, 0 ) == 0 && due == 1600 );

    /* A request: plain, the PID sent, SFCI's magic, the command and its two
     * arguments 16-byte aligned after it, the size counting libnx's padding. */
    memset( buffer, 0xee, sizeof(buffer) );
    saltysd_request( buffer, SALTYSD_SET_REFRESH_RATE, 60, 0 );
    assert( buffer[0] == 4 && buffer[1] == 0x8000000c && buffer[2] == 1 && !buffer[3] && !buffer[4] );
    memcpy( raw, (uint8_t *)buffer + 0x20, sizeof(raw) );
    assert( raw[0] == SALTYSD_IN_MAGIC && raw[1] == SALTYSD_SET_REFRESH_RATE && raw[2] == 60 && raw[3] == 0 );
    assert( buffer[0x40 - 1] == 0 );

    /* GetSharedMemoryHandle's reply, as SaltyNX makes it: one handle copied, the
     * raw data 16-byte aligned after it. */
    memset( buffer, 0, sizeof(buffer) );
    buffer[0] = 4;
    buffer[1] = 0x80000000u | 8;
    buffer[2] = 1 << 1;
    buffer[3] = 0x1234;
    put32( buffer, 16, SALTYSD_OUT_MAGIC );
    put32( buffer, 24, 0 );
    assert( saltysd_reply( buffer, &handle, NULL, 0 ) == 0 && handle == 0x1234 );
    /* Not SFCO's magic, or too little raw data: no reply. */
    put32( buffer, 16, SALTYSD_IN_MAGIC );
    assert( saltysd_reply( buffer, &handle, NULL, 0 ) == ~0ull && handle == 0x1234 );
    put32( buffer, 16, SALTYSD_OUT_MAGIC );
    buffer[1] = 0x80000000u | 3;
    assert( saltysd_reply( buffer, &handle, NULL, 0 ) == ~0ull );
    /* With a PID too: the handle after it, the raw data at 32. */
    memset( buffer, 0, sizeof(buffer) );
    buffer[0] = 4;
    buffer[1] = 0x80000000u | 8;
    buffer[2] = 1 | (1 << 1);
    buffer[5] = 0x5678;
    put32( buffer, 32, SALTYSD_OUT_MAGIC );
    put32( buffer, 40, 0xe01 );
    assert( saltysd_reply( buffer, &handle, NULL, 0 ) == 0xe01 && handle == 0x5678 );
    /* GetDisplayRefreshRate's: no handle, the rate after the result. */
    memset( buffer, 0, sizeof(buffer) );
    buffer[0] = 4;
    buffer[1] = 10;
    put32( buffer, 16, SALTYSD_OUT_MAGIC );
    put32( buffer, 24, 1 );
    put32( buffer, 32, 40 );
    assert( saltysd_reply( buffer, &handle, extra, 1 ) == 1 && !handle && extra[0] == 40 );
    assert( saltysd_reply( buffer, NULL, extra, 7 ) == ~0ull );

    printf( "FPSLocker: the block (%u bytes), display sync's rate, the swap interval, waiting, SaltySD's "
            "requests and replies\n",
            FPSLOCKER_BLOCK_SIZE );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-fpslocker-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(source), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def body(text, start):
    """The function that starts with start, to its closing brace."""
    text = text[text.index(start):]
    return text[:text.index('\n}\n')]


def line(text, start):
    """The line that starts with start."""
    text = text[text.index('\n' + start) + 1:]
    return text[:text.index('\n') + 1]


# The present paths' own code, run with a clock of the test's: OpenGL's
# nx_fps_lock_frame and Vulkan's nx_pace_present, as winnx_opengl.c and
# vulkan.c have them.
gl = (root / 'dlls/win32u/winnx_opengl.c').read_text()
vk = (root / 'dlls/win32u/vulkan.c').read_text()
paths = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "fpslocker.h"

typedef int EGLint;
typedef enum { VK_PRESENT_MODE_IMMEDIATE_KHR = 0, VK_PRESENT_MODE_FIFO_KHR = 2 } VkPresentModeKHR;
struct opengl_drawable { int interval; };
struct egl_platform { void *display; };
struct opengl_funcs { void (*p_eglSwapInterval)( void *, EGLint ); };

/* As the present paths declare them; bridge.c defines them, as saltynx.c does,
 * unless the test runs without it. */
extern int wine_nx_fps_lock_target __attribute__((weak));
extern int wine_nx_fps_lock_panel __attribute__((weak));

static unsigned int interval_calls, sleeps;
static EGLint last_interval;
static uint64_t now_ns, slept_ns;

static void mock_interval( void *display, EGLint value ) { (void)display; interval_calls++; last_interval = value; }
static const struct egl_platform platform;
static const struct egl_platform *egl = &platform;
static const struct opengl_funcs functions = { mock_interval };
static const struct opengl_funcs *funcs = &functions;
static unsigned long long horizon_interrupt_time( void ) { return now_ns / 100; }
static int mock_nanosleep( const struct timespec *request, struct timespec *remaining )
{
    const uint64_t ns = (uint64_t)request->tv_sec * 1000000000ull + request->tv_nsec;

    (void)remaining;
    assert( request->tv_nsec >= 0 && request->tv_nsec < 1000000000 );
    sleeps++;
    slept_ns = ns;
    now_ns += ns;
    return 0;
}
static int mock_clock_gettime( clockid_t clock, struct timespec *time )
{
    assert( clock == CLOCK_MONOTONIC );
    time->tv_sec = now_ns / 1000000000ull;
    time->tv_nsec = now_ns % 1000000000ull;
    return 0;
}
#define nanosleep mock_nanosleep
#define clock_gettime mock_clock_gettime
static uint64_t nx_frame_interval;
''' + line(gl, 'static EGLint nx_swap_interval_set') + line(gl, 'static uint64_t nx_fps_lock_due;') + \
    body(gl, 'static void nx_fps_lock_frame( struct opengl_drawable *base )') + '\n}\n' + \
    body(vk, 'static void nx_pace_present( uint64_t *previous, VkPresentModeKHR mode )') + '\n}\n' + r'''
static void lock( int target, int panel )
{
    wine_nx_fps_lock_target = target;
    wine_nx_fps_lock_panel = panel;
}

int main(void)
{
    struct opengl_drawable base = { 1 };
    uint64_t previous = 0;

    /* Without the runtime's bridge, no target: the program's interval, and
     * neither path waits. */
    if (!&wine_nx_fps_lock_target)
    {
        nx_fps_lock_frame( &base );
        nx_pace_present( &previous, VK_PRESENT_MODE_IMMEDIATE_KHR );
        assert( interval_calls == 1 && last_interval == 1 && !sleeps && !previous );
        printf( "FPSLocker's target: nothing to keep without the runtime's bridge\n" );
        return 0;
    }

    /* OpenGL, no target: the program's interval, set once, and no waiting. */
    nx_fps_lock_frame( &base );
    assert( interval_calls == 1 && last_interval == 1 && !sleeps );
    nx_fps_lock_frame( &base );
    assert( interval_calls == 1 && !sleeps );
    /* A new surface before Wine set an interval is left alone; Wine's -1
     * (adaptive) is 1. */
    nx_swap_interval_set = -1;
    base.interval = INT_MIN;
    nx_fps_lock_frame( &base );
    assert( interval_calls == 1 );
    base.interval = -1;
    nx_fps_lock_frame( &base );
    assert( interval_calls == 2 && last_interval == 1 );
    /* The panel at twice the target: interval 2, no waiting. */
    lock( 30, 60 );
    nx_fps_lock_frame( &base );
    nx_fps_lock_frame( &base );
    assert( interval_calls == 3 && last_interval == 2 && !sleeps );
    /* 40 on a 60 Hz panel: the program's interval back, and each frame waits
     * for its 25 ms. */
    lock( 40, 60 );
    now_ns = 1000000000ull;
    nx_fps_lock_frame( &base );
    assert( interval_calls == 4 && last_interval == 1 && !sleeps );
    now_ns += 10000000;
    nx_fps_lock_frame( &base );
    assert( sleeps == 1 && slept_ns == 15000000 && now_ns == 1025000000ull );
    now_ns += 30000000;  /* a slow frame: 5 ms late, the next one not held back */
    nx_fps_lock_frame( &base );
    assert( sleeps == 1 );
    now_ns += 5000000;
    nx_fps_lock_frame( &base );
    assert( sleeps == 2 && slept_ns == 15000000 );
    /* 30 on a 50 Hz panel (none SaltyNX makes, but a TV's) waits too. */
    lock( 30, 50 );
    now_ns += 200000000;
    nx_fps_lock_frame( &base );
    now_ns += 3333300;
    nx_fps_lock_frame( &base );
    assert( sleeps == 3 && slept_ns == 30000000 && interval_calls == 4 );
    /* The target gone: no more waiting. */
    lock( 0, 60 );
    now_ns += 1000000;
    nx_fps_lock_frame( &base );
    assert( sleeps == 3 && interval_calls == 4 && !nx_fps_lock_due );

    /* Vulkan: a FIFO swapchain on a panel at the target waits for nothing;
     * any other present mode waits for each frame. */
    sleeps = 0;
    lock( 30, 30 );
    nx_pace_present( &previous, VK_PRESENT_MODE_FIFO_KHR );
    assert( !previous && !sleeps );
    nx_pace_present( &previous, VK_PRESENT_MODE_IMMEDIATE_KHR );
    assert( previous == now_ns && !sleeps );
    now_ns += 10000000;
    nx_pace_present( &previous, VK_PRESENT_MODE_IMMEDIATE_KHR );
    assert( sleeps == 1 && slept_ns == 23333333 && previous == now_ns );
    /* FIFO with the panel at twice the target waits as well. */
    lock( 30, 60 );
    now_ns += 1000000;
    nx_pace_present( &previous, VK_PRESENT_MODE_FIFO_KHR );
    assert( sleeps == 2 && slept_ns == 32333333 );
    /* A frame limit of the game's own: the lower of the two holds. */
    nx_frame_interval = 1000000000ull / 20;
    now_ns += 1000000;
    nx_pace_present( &previous, VK_PRESENT_MODE_FIFO_KHR );
    assert( sleeps == 3 && slept_ns == 49000000 );
    nx_frame_interval = 1000000000ull / 60;
    now_ns += 1000000;
    nx_pace_present( &previous, VK_PRESENT_MODE_FIFO_KHR );
    assert( sleeps == 4 && slept_ns == 32333333 );
    lock( 0, 60 );
    now_ns += 1000000;
    nx_pace_present( &previous, VK_PRESENT_MODE_FIFO_KHR );
    assert( sleeps == 5 && slept_ns == 15666666 );

    printf( "FPSLocker's target: OpenGL's swap interval and waiting, Vulkan's waiting, as the present paths have them\n" );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-fpslocker-paths-') as tmp:
    c, bridge_c = Path(tmp) / 'paths.c', Path(tmp) / 'bridge.c'
    c.write_text(paths)
    bridge_c.write_text('int wine_nx_fps_lock_target, wine_nx_fps_lock_panel;\n')
    for name, extra in (('paths', [str(bridge_c)]), ('alone', [])):
        exe = Path(tmp) / name
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I', str(source), str(c), *extra, '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


# The bridge: SaltySD asked for its shared memory, for the panel rate display
# sync needs and, when the program ends, for 60 Hz back, nothing else;
# FPSLocker's own fields read, never written; the block made anew on the first
# look, with the title's settings, and later only when it is gone.
bridge = (source / 'saltynx.c').read_text()
commands = set(re.findall(r'saltysd_call\( (SALTYSD_\w+),', bridge))
assert commands == {'SALTYSD_GET_SHARED_MEMORY', 'SALTYSD_SET_REFRESH_RATE'}, commands
for field in ('FPSLOCKER_TARGET', 'FPSLOCKER_TARGET_DOCKED', 'FPSLOCKER_DISPLAY_SYNC', 'FPSLOCKER_PANEL_RATE'):
    assert not re.search(r'\[' + field + r'\]\s*=[^=]', bridge), field
make = body(bridge, 'static uint8_t *find_or_make_block(')
assert make.index('if (block && !first) return block;') < make.index('fpslocker_make_block(')
assert '"sdmc:/SaltySD/plugins/FPSLocker/%016lX.dat"' in make
main = body(bridge, 'static void *bridge_main(')
assert 'block = find_or_make_block( base, title, !looks );' in main
assert 'target = block[docked ? FPSLOCKER_TARGET_DOCKED : FPSLOCKER_TARGET];' in main
# The console's mode asked of am each time (ICommonStateGetter 5), not
# appletMainLoop's copy, which no one updates while a game runs.
docked = body(bridge, 'static int console_docked( void )')
assert 'serviceDispatchOut( state, 5, mode )' in docked and 'const int docked = console_docked();' in main
# Display sync: SaltyNX's flags, read once a second; the rate asked when it
# changes and the panel is not there, again for a new mode, retried while the
# one session is taken.
assert '#define SYNC_FLAG        "sdmc:/SaltySD/flags/displaysync.flag"' in bridge
assert '#define SYNC_FLAG_DOCKED "sdmc:/SaltySD/flags/displaysyncdocked.flag"' in bridge
assert re.search(r'if \(!\(looks\+\+ % 10\)\)\s*\{\s*sync_flags\[0\] = file_exists\( SYNC_FLAG \);\s*'
                 r'sync_flags\[1\] = file_exists\( SYNC_FLAG_DOCKED \);', main)
assert 'sync = sync_flags[docked];' in main
assert 'want = fpslocker_sync_rate( block, docked, sync );' in main
assert 'if (docked != last_docked || !want) asked = 0;' in main
assert 'if (want && want != asked && want == panel) asked = want;' in main
asking = main[main.index('if (want && want != asked)\n'):]
assert 'saltysd_call( SALTYSD_SET_REFRESH_RATE, want, NULL, NULL, 0, &connect );' in asking
assert 'if (R_SUCCEEDED( connect ) || ++refused >= 10)' in asking
assert 'if (want != 60 && R_SUCCEEDED( connect ) && !result) panel_moved = 1;' in asking
assert 'if (panel && panel != 60 && (sync || ((block[FPSLOCKER_DISPLAY_SYNC] >> docked) & 1))) panel_moved = 1;' in main
assert main.index('__atomic_store_n( &wine_nx_fps_lock_panel') < main.index('__atomic_store_n( &wine_nx_fps_lock_target')
assert 'svcSleepThread( BRIDGE_PERIOD_NS );' in main and '#define BRIDGE_PERIOD_NS 100000000ll' in bridge
assert 'next_try = now + second * (tries < 10 ? 1 : 10);' in main
start = body(bridge, 'void wine_nx_saltynx_start( void )')
assert 'appletGetAppletType() != AppletType_Application' in start
stop = body(bridge, 'void wine_nx_saltynx_stop( void )')
assert stop.index('pthread_join( bridge_thread, NULL );') < stop.index('fpslocker_publish( block, 0, 0.0f, 0,')
assert 'if (panel_moved && base[FPSLOCKER_PANEL_RATE] != 60)' in stop
assert 'saltysd_call( SALTYSD_SET_REFRESH_RATE, 60,' in stop and 'shmemClose( &shared );' in stop
call = body(bridge, 'static uint64_t saltysd_call(')
assert call.index('svcSendSyncRequest( session )') < call.index('svcCloseHandle( session );')
# Vampire's log said "SaltyNX does not answer (0x10801)": the kernel's
# LimitReached, the process out of sessions. The limit is raised above what is
# in use, only where the loader hints SetResourceLimitLimitValue, and set back
# when the program ends.
assert '#define KERNEL_LIMIT_REACHED 0x10801' in bridge
assert re.search(r'if \(\*connect == KERNEL_LIMIT_REACHED && more_sessions\(\)\)\s*'
                 r'\*connect = svcConnectToNamedPort\( &session, SALTYSD_PORT \);', call)
more = body(bridge, 'static int more_sessions( void )')
assert more.index('if (!envIsSyscallHinted( 0x7e ))') < more.index('svcSetResourceLimitLimitValue(')
assert 'used + 8 > limit ? svcSetResourceLimitLimitValue( (Handle)limits, LimitableResource_Sessions, used + 8 ) : 0' in more
assert stop.count('sessions_back();') == 2

# The runtime: on unless fpslocker says otherwise, for the whole card or one
# program; started once the program is known, for a game and not for the
# runtime's own components, stopped on every way out.
runtime = (source / 'runtime.c').read_text()
assert '#include "saltynx.h"' in runtime and 'int wine_nx_fpslocker = 1;' in runtime
assert 'wine_nx_fpslocker = config_bool( "fpslocker", 1, "fpslocker.txt", 0 );' in runtime
assert 'int fpslocker = launcher_setting_state( &kv, "fpslocker" );' in runtime
assert 'if (fpslocker >= 0) wine_nx_fpslocker = fpslocker;' in runtime
started = runtime.index('if (wine_nx_fpslocker && !runtime_components_run) wine_nx_saltynx_start();')
assert runtime.index('log_line( "[TARGET] %s", target );') < started
leave = body(runtime, 'static int leave_cleanly( void )')
assert leave.index('wine_nx_saltynx_stop();') < leave.index('wine_nx_runtime_log_flush();')
# A program that ends goes back through return_to_launcher, not leave_cleanly:
# the bridge stops there too, before the screen's presenter.
back = body(runtime, 'static int return_to_launcher( void )')
assert back.index('wine_nx_saltynx_stop();') < back.index('wine_nx_compositor_stop();')
assert 'unsigned int frames = wine_nx_frames_shown();' in runtime
cmake = (root / 'horizon-wine/CMakeLists.txt').read_text()
assert cmake.count('source/saltynx.c') == 2

# OpenGL: before each frame of the screen, the swap interval display sync asks
# for, or the wait; the program's interval back when there is no target.
assert '#include "../../horizon-wine/source/fpslocker.h"' in gl
for name in ('target', 'panel'):
    assert f'extern int wine_nx_fps_lock_{name} __attribute__((weak));' in gl
    assert f'extern int wine_nx_fps_lock_{name} __attribute__((weak));' in vk
assert 'wine_nx_fps_lock_sync' not in gl + vk
swap = body(gl, 'static BOOL nx_drawable_swap( struct opengl_drawable *base )')
assert swap.index('nx_osk_draw( base );') < swap.index('nx_fps_lock_frame( base );') < \
    swap.index('ret = funcs->p_eglSwapBuffers( egl->display, base->surface );')
frame = body(gl, 'static void nx_fps_lock_frame( struct opengl_drawable *base )')
assert 'int multiple = fpslocker_swap_interval( target, panel );' in frame
assert 'base->interval != INT_MIN ? abs( base->interval ) : nx_swap_interval_set' in frame
assert 'fpslocker_pace( &nx_fps_lock_due, horizon_interrupt_time(), 10000000ull / target )' in frame
assert 'nx_swap_interval_set = abs( base->interval );' in body(gl, 'static void nx_drawable_flush(')
assert 'nx_swap_interval_set = -1;' in body(gl, 'static BOOL nx_surface_create(')

# Vulkan: the swapchain's own present mode decides; FIFO with display sync at
# the target waits for nothing more.
assert '#include "../../horizon-wine/source/fpslocker.h"' in vk
pace = body(vk, 'static void nx_pace_present( uint64_t *previous, VkPresentModeKHR mode )')
assert '!(mode == VK_PRESENT_MODE_FIFO_KHR && fpslocker_swap_interval( target, panel ) == 1)' in pace
assert 'interval = 1000000000ull / target;' in pace
assert re.search(r'nx_pace_present\( &swapchain_from_handle\( client_swapchains\[0\] \)->nx_last_present,\s*'
                 r'swapchain_from_handle\( client_swapchains\[0\] \)->nx_present_mode \);', vk)
print('FPSLocker: the bridge asks SaltySD for its memory, display sync\'s rate and 60 Hz back only, never writes '
      'FPSLocker\'s fields, starts with a game and stops on every way out; OpenGL and Vulkan keep the target')
