/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * SaltyNX and FPSLocker for a program Autorun runs (fpslocker.h says why the
 * runtime does the NX-FPS plugin's part, and SaltyNX's). A thread of its own,
 * ten times a second:
 * - SaltySD's shared memory (command 7), until it is mapped: once a second,
 *   then every ten once SaltyNX has not answered ten times;
 * - once mapped, FPSLocker's block in it made anew, with the settings
 *   FPSLocker saved for the title, as SaltyNX and NX-FPS make it for a game
 *   they start;
 * - the frame rate of the last second written to the block;
 * - FPSLocker's target for the console's mode and the panel's rate read for
 *   the present paths (wine_nx_fps_lock_*), which keep the target by the swap
 *   interval while the panel runs at the target or a multiple of it, and by
 *   waiting for each frame otherwise;
 * - with FPSLocker's display sync on for the mode (SaltyNX's flag files),
 *   SaltyNX asked for the panel rate the target needs (command 11) whenever
 *   that changes, from the game's start: FPSLocker asks it only when its
 *   target is changed, and SaltyNX itself only for a game it injected.
 * When the program ends, the panel goes back to 60 Hz if display sync moved it
 * off 60: SaltyNX does that itself only for a game it injected.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "fpslocker.h"
#include "saltynx.h"

extern void wine_nx_runtime_trace( const char *line );
extern unsigned int wine_nx_frames_shown( void );                 /* runtime.c */
extern unsigned int wine_nx_gl_swaps __attribute__((weak));       /* winnx_opengl.c */
extern unsigned int wine_nx_vk_presents __attribute__((weak));    /* vulkan.c */

/* For the present paths: FPSLocker's target for the console's mode (0 none)
 * and the panel's refresh rate (0 not known). */
int wine_nx_fps_lock_target;
int wine_nx_fps_lock_panel;

#define BRIDGE_PERIOD_NS 100000000ll  /* a tenth of a second */
#define BRIDGE_WINDOW    10           /* periods the frame rate is counted over */
#define SYNC_FLAG        "sdmc:/SaltySD/flags/displaysync.flag"
#define SYNC_FLAG_DOCKED "sdmc:/SaltySD/flags/displaysyncdocked.flag"

static pthread_t bridge_thread;
static int bridge_running, bridge_stop;
static SharedMemory shared;
static int shared_mapped;
static int panel_moved;  /* display sync was on with the panel off 60 Hz */

static void bridge_trace( const char *format, ... )
{
    char line[256];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
}

/* One request to SaltySD on a session of its own, closed at once: its port
 * takes one session at a time, and FPSLocker needs it too. The result, or ~0
 * when there was no answer. */
static uint64_t saltysd_call( uint64_t command, uint64_t argument, uint32_t *handle, uint64_t *extra,
                              unsigned int extra_count, Result *connect )
{
    uint32_t *buffer = (uint32_t *)armGetTls();
    uint64_t result = ~0ull;
    Handle session;

    *connect = svcConnectToNamedPort( &session, SALTYSD_PORT );
    if (R_FAILED( *connect )) return ~0ull;
    saltysd_request( buffer, command, argument, 0 );
    if (R_SUCCEEDED( svcSendSyncRequest( session ) )) result = saltysd_reply( buffer, handle, extra, extra_count );
    svcCloseHandle( session );
    return result;
}

static int map_shared_memory( Result *connect )
{
    uint32_t handle = 0;

    if (saltysd_call( SALTYSD_GET_SHARED_MEMORY, 0, &handle, NULL, 0, connect ) || !handle) return 0;
    shmemLoadRemote( &shared, handle, FPSLOCKER_SHARED_SIZE, Perm_Rw );
    if (R_FAILED( shmemMap( &shared ) ))
    {
        shmemClose( &shared );
        return 0;
    }
    shared_mapped = 1;
    return 1;
}

/* The block: on the first look made anew with the settings FPSLocker saved for
 * this title (fpslocker_make_block), later the one there, or a new one when it
 * is gone. */
static uint8_t *find_or_make_block( uint8_t *base, uint64_t title, int first )
{
    uint8_t *block = fpslocker_find_block( base ), dat[8];
    unsigned int size = 0;
    char path[96];
    FILE *file;

    if (block && !first) return block;
    snprintf( path, sizeof(path), "sdmc:/SaltySD/plugins/FPSLocker/%016lX.dat", (unsigned long)title );
    if ((file = fopen( path, "rb" )))
    {
        size = fread( dat, 1, sizeof(dat), file );
        fclose( file );
    }
    bridge_trace( "[FPSLOCKER] FPSLocker's block made in SaltyNX's memory%s%s", size ? ", with the title's saved settings" : "",
                  block ? ", in place of the one there" : "" );
    return fpslocker_make_block( base, size ? dat : NULL, size );
}

static unsigned int counter( unsigned int *value )
{
    return value ? __atomic_load_n( value, __ATOMIC_RELAXED ) : 0;
}

/* The console's mode as it is now: appletGetOperationMode keeps what
 * appletMainLoop last heard, and nothing runs that while a game does. */
static int console_docked( void )
{
    Service *state = appletGetServiceSession_CommonStateGetter();
    u8 mode = 0;

    if (state && R_SUCCEEDED( serviceDispatchOut( state, 5, mode ) )) return mode == AppletOperationMode_Console;
    return appletGetOperationMode() == AppletOperationMode_Console;
}

static int file_exists( const char *path )
{
    FILE *file = fopen( path, "rb" );

    if (!file) return 0;
    fclose( file );
    return 1;
}

static void *bridge_main( void *arg )
{
    unsigned int frames[BRIDGE_WINDOW + 1], gl[BRIDGE_WINDOW + 1], vk[BRIDGE_WINDOW + 1];
    uint64_t ticks[BRIDGE_WINDOW + 1], title = 0, next_try = 0;
    unsigned int filled = 0, slot = 0, tries = 0, api = 0, looks = 0, refused = 0;
    int last_target = -1, last_panel = -1, last_sync = -1, last_docked = -1, asked = 0;
    int sync_flags[2] = {0, 0};

    (void)arg;
    svcGetInfo( &title, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0 );
    while (!__atomic_load_n( &bridge_stop, __ATOMIC_ACQUIRE ))
    {
        const uint64_t now = armGetSystemTick(), second = armGetSystemTickFreq();
        unsigned int oldest;
        uint8_t *base, *block;

        frames[slot] = wine_nx_frames_shown();
        gl[slot] = counter( &wine_nx_gl_swaps ? &wine_nx_gl_swaps : NULL );
        vk[slot] = counter( &wine_nx_vk_presents ? &wine_nx_vk_presents : NULL );
        ticks[slot] = now;
        oldest = filled < BRIDGE_WINDOW ? 0 : (slot + 1) % (BRIDGE_WINDOW + 1);
        if (filled < BRIDGE_WINDOW) filled++;

        if (!shared_mapped && now >= next_try)
        {
            Result connect = 0;

            if (map_shared_memory( &connect ))
                bridge_trace( "[FPSLOCKER] SaltyNX's shared memory mapped for title %016lx", (unsigned long)title );
            else
            {
                if (!tries) bridge_trace( "[FPSLOCKER] SaltyNX does not answer (%#x); asked again every second, "
                                          "then every ten", connect );
                tries++;
                next_try = now + second * (tries < 10 ? 1 : 10);
            }
        }
        if (shared_mapped && (base = shmemGetAddr( &shared )))
        {
            const int docked = console_docked();
            const unsigned int counted = frames[slot] - frames[oldest];
            const uint64_t span = ticks[slot] - ticks[oldest];
            const float rate = span ? (float)counted * (float)second / (float)span : 0.0f;
            int target, sync, panel, want;

            block = find_or_make_block( base, title, !looks );
            if (gl[slot] != gl[oldest]) api = FPSLOCKER_API_EGL;
            else if (vk[slot] != vk[oldest]) api = FPSLOCKER_API_VULKAN;
            fpslocker_publish( block, counted ? (unsigned int)(rate + 0.5f) : 0, counted ? rate : 0.0f, api,
                               frames[slot] );

            /* SaltyNX's flags for FPSLocker's display sync, once a second. */
            if (!(looks++ % 10))
            {
                sync_flags[0] = file_exists( SYNC_FLAG );
                sync_flags[1] = file_exists( SYNC_FLAG_DOCKED );
            }
            target = block[docked ? FPSLOCKER_TARGET_DOCKED : FPSLOCKER_TARGET];
            sync = sync_flags[docked];
            panel = base[FPSLOCKER_PANEL_RATE];
            __atomic_store_n( &wine_nx_fps_lock_panel, panel, __ATOMIC_RELAXED );
            __atomic_store_n( &wine_nx_fps_lock_target, target, __ATOMIC_RELEASE );
            if (panel && panel != 60 && (sync || ((block[FPSLOCKER_DISPLAY_SYNC] >> docked) & 1))) panel_moved = 1;

            /* Display sync: the panel rate the target needs, asked of SaltyNX
             * when that changes and the panel is not there already (FPSLocker
             * asks when its target is changed); asked again a tenth of a second
             * later while FPSLocker holds SaltySD's one session, up to ten
             * times. Each mode's panel is asked for its own. */
            want = fpslocker_sync_rate( block, docked, sync );
            if (docked != last_docked || !want) asked = 0;
            if (want && want != asked && want == panel) asked = want;
            if (want && want != asked)
            {
                Result connect = 0;
                const uint64_t result = saltysd_call( SALTYSD_SET_REFRESH_RATE, want, NULL, NULL, 0, &connect );

                if (R_SUCCEEDED( connect ) || ++refused >= 10)
                {
                    bridge_trace( "[FPSLOCKER] display sync %s: the panel asked for %d Hz, for a target of %d FPS: %s",
                                  docked ? "docked" : "handheld", want, target,
                                  R_FAILED( connect ) ? "no session" : result ? "SaltyNX refused" : "done" );
                    if (want != 60 && R_SUCCEEDED( connect ) && !result) panel_moved = 1;
                    asked = want;
                    refused = 0;
                }
            }
            last_docked = docked;

            if (target != last_target || sync != last_sync || panel != last_panel)
            {
                const int interval = fpslocker_swap_interval( target, panel );

                bridge_trace( "[FPSLOCKER] target %d FPS %s, display sync %s, panel %d Hz: %s", target,
                              docked ? "docked" : "handheld", sync ? "on" : "off", panel,
                              !target ? "no limit" : interval ? "kept by the swap interval" : "kept by waiting" );
                last_target = target;
                last_sync = sync;
                last_panel = panel;
            }
        }
        slot = (slot + 1) % (BRIDGE_WINDOW + 1);
        svcSleepThread( BRIDGE_PERIOD_NS );
    }
    return NULL;
}

void wine_nx_saltynx_start( void )
{
    if (bridge_running) return;
    if (appletGetAppletType() != AppletType_Application)
    {
        bridge_trace( "[FPSLOCKER] not started: FPSLocker sees only a program started as an application, "
                      "from a forwarder" );
        return;
    }
    __atomic_store_n( &bridge_stop, 0, __ATOMIC_RELEASE );
    if (pthread_create( &bridge_thread, NULL, bridge_main, NULL ))
    {
        bridge_trace( "[FPSLOCKER] not started: no thread for it" );
        return;
    }
    bridge_running = 1;
}

void wine_nx_saltynx_stop( void )
{
    uint8_t *base, *block;

    if (!bridge_running) return;
    __atomic_store_n( &bridge_stop, 1, __ATOMIC_RELEASE );
    pthread_join( bridge_thread, NULL );
    bridge_running = 0;
    __atomic_store_n( &wine_nx_fps_lock_target, 0, __ATOMIC_RELEASE );
    if (!shared_mapped) return;
    if ((base = shmemGetAddr( &shared )))
    {
        if ((block = fpslocker_find_block( base ))) fpslocker_publish( block, 0, 0.0f, 0, wine_nx_frames_shown() );
        if (panel_moved && base[FPSLOCKER_PANEL_RATE] != 60)
        {
            Result connect = 0;
            const uint64_t result = saltysd_call( SALTYSD_SET_REFRESH_RATE, 60, NULL, NULL, 0, &connect );

            bridge_trace( "[FPSLOCKER] the panel back to 60 Hz from %u: %s", base[FPSLOCKER_PANEL_RATE],
                          R_FAILED( connect ) ? "no session" : result ? "SaltyNX refused" : "done" );
        }
    }
    shmemClose( &shared );
    shared_mapped = 0;
    panel_moved = 0;
}
