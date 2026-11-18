/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * FPSLocker and SaltyNX, for a game Autorun runs: what the runtime's bridge
 * (saltynx.c) and the present paths (dlls/win32u/winnx_opengl.c, vulkan.c)
 * share. Pure, so tests/check_fpslocker.py runs it on a computer.
 *
 * SaltyNX keeps one block of shared memory, 0x1000 bytes, which its NX-FPS
 * plugin fills for a game it injected and FPSLocker's overlay reads and writes:
 * NxFpsSharedBlock (saltysd_proc/source/useful.h), packed, found by its magic.
 * SaltyNX injects no homebrew title (its title ID above 0x01FF..., or the
 * process named hbloader, as every Autorun forwarder is), so the runtime does
 * the plugin's part itself, as NetherSX2_nx does: it writes the frame rate and
 * reads FPSLocker's target. SaltyNX's own part for a game it injected, clearing
 * the memory when the game starts and display sync, the runtime does too.
 * FPSLocker owns the target and the display sync bits; the runtime never
 * writes them.
 */
#ifndef WINE_NX_FPSLOCKER_H
#define WINE_NX_FPSLOCKER_H

#include <stdint.h>
#include <string.h>

#define FPSLOCKER_SHARED_SIZE      0x1000u
#define FPSLOCKER_MAGIC            0x00465053u  /* "SPF\0": NX-FPS's block */
#define FPSLOCKER_BLOCK_SIZE       174u
/* Byte 1 of the shared memory is the panel's refresh rate, which SaltyNX keeps
 * (GetDisplayRefreshRate without its internal flag reads it there). */
#define FPSLOCKER_PANEL_RATE       1u
/* Fields of NxFpsSharedBlock, from its start. */
#define FPSLOCKER_FPS              4u   /* uint8_t */
#define FPSLOCKER_FPS_AVG          5u   /* float, unaligned */
#define FPSLOCKER_PLUGIN_ACTIVE    9u   /* bool */
#define FPSLOCKER_TARGET           10u  /* uint8_t FPSlocked, handheld; FPSLocker's */
#define FPSLOCKER_ZERO_SYNC        12u  /* uint8_t */
#define FPSLOCKER_API              14u  /* uint8_t: 1 NVN, 2 EGL, 3 Vulkan */
#define FPSLOCKER_SET_BUFFERS      56u  /* uint8_t */
#define FPSLOCKER_DISPLAY_SYNC     59u  /* bit 0 handheld, bit 1 docked; FPSLocker's */
#define FPSLOCKER_FORCE_ORIGINAL   156u /* bool forceOriginalRefreshRate */
#define FPSLOCKER_DONT_FORCE_60    157u /* bool dontForce60InDocked */
#define FPSLOCKER_FORCE_SUSPEND    158u /* bool */
#define FPSLOCKER_TARGET_DOCKED    164u /* uint8_t FPSlockedDocked; FPSLocker's */
#define FPSLOCKER_FRAME_NUMBER     165u /* uint64_t, unaligned */
#define FPSLOCKER_EXPECTED_BUFFERS 173u /* int8_t expectedSetBuffers */

#define FPSLOCKER_API_EGL          2u
#define FPSLOCKER_API_VULKAN       3u

/* The block in SaltyNX's memory, as SaltyNX and FPSLocker look for it: the
 * first magic on a 4-byte boundary; NULL when there is none. */
static inline uint8_t *fpslocker_find_block( uint8_t *base )
{
    uint32_t offset, magic;

    if (!base) return NULL;
    for (offset = 0; offset + FPSLOCKER_BLOCK_SIZE <= FPSLOCKER_SHARED_SIZE; offset += 4)
    {
        memcpy( &magic, base + offset, sizeof(magic) );
        if (magic == FPSLOCKER_MAGIC) return base + offset;
    }
    return NULL;
}

/* The block for a game that starts: the memory after SaltyNX's own four bytes
 * cleared, as SaltyNX clears it for each game it starts but never for a
 * homebrew title (a block one Autorun game left would otherwise be the next
 * one's, with its target); then a block where FPSLocker makes one when it
 * finds none (base + 4, its magic, expectedSetBuffers -1), with the settings
 * FPSLocker saved for the title, as its loadSettingsToShared reads them: the
 * target (for both modes), ZeroSync, SetBuffers, forceSuspend, then the docked
 * target. dat may be NULL. */
static inline uint8_t *fpslocker_make_block( uint8_t *base, const uint8_t *dat, unsigned int dat_size )
{
    uint8_t *block = base + 4;
    const uint32_t magic = FPSLOCKER_MAGIC;

    memset( block, 0, FPSLOCKER_SHARED_SIZE - 4 );
    block[FPSLOCKER_EXPECTED_BUFFERS] = 0xff;
    if (dat && dat_size >= 1) block[FPSLOCKER_TARGET] = block[FPSLOCKER_TARGET_DOCKED] = dat[0];
    if (dat && dat_size >= 2) block[FPSLOCKER_ZERO_SYNC] = dat[1];
    if (dat && dat_size >= 3) block[FPSLOCKER_SET_BUFFERS] = dat[2];
    if (dat && dat_size >= 4) block[FPSLOCKER_FORCE_SUSPEND] = !!dat[3];
    if (dat && dat_size >= 5) block[FPSLOCKER_TARGET_DOCKED] = dat[4];
    memcpy( block, &magic, sizeof(magic) );  /* last: a reader sees it whole */
    return block;
}

/* The frame rate written for FPSLocker: whole frames a second, the average,
 * which API drew them, and the frame count; active only while frames come. */
static inline void fpslocker_publish( uint8_t *block, unsigned int fps, float average, unsigned int api,
                                      uint64_t frames )
{
    const uint8_t whole = fps > 255 ? 255 : (uint8_t)fps, active = fps > 0;

    memcpy( block + FPSLOCKER_FPS_AVG, &average, sizeof(average) );
    memcpy( block + FPSLOCKER_FRAME_NUMBER, &frames, sizeof(frames) );
    block[FPSLOCKER_FPS] = whole;
    if (api) block[FPSLOCKER_API] = (uint8_t)api;
    block[FPSLOCKER_PLUGIN_ACTIVE] = active;  /* last, as NX-FPS: never active without a rate */
}

/* The refresh rate display sync asks of SaltyNX for the console's mode, as
 * SaltyNX asks it for a game it injected (updateRefreshRateForApp): FPSLocker's
 * target, or 60 with none or with FPSLocker keeping the game's own rate; 0 with
 * display sync off, when the panel is left as it is. SaltyNX makes of it what
 * the panel can do: 30 is 60 in handheld, as anything under 40 without a
 * multiple from 40 to 60. */
static inline int fpslocker_sync_rate( const uint8_t *block, int docked, int sync )
{
    const int target = block[docked ? FPSLOCKER_TARGET_DOCKED : FPSLOCKER_TARGET];

    if (!sync) return 0;
    if (!target) return 60;
    if (block[FPSLOCKER_FORCE_ORIGINAL] && (!docked || !block[FPSLOCKER_DONT_FORCE_60])) return 60;
    return target;
}

/* The swap interval that keeps a game at FPSLocker's target by the panel's own
 * pace: the panel at the target, or at a multiple of it (2x to 4x, as 30 on
 * 60 Hz), and a frame then waits that many refreshes, as NX-FPS has a game's
 * own interval do. 0 when the target has to be kept by waiting instead
 * (fpslocker_pace). */
static inline int fpslocker_swap_interval( int target, int panel )
{
    if (target <= 0 || panel < target || panel % target) return 0;
    return panel / target <= 4 ? panel / target : 0;
}

/* Keeping the target by waiting: due is when the next frame may go (in the
 * clock's units, period of them a frame), now the clock. Returns how long to
 * wait first. A frame later than a whole period starts the count again rather
 * than letting the next ones go back to back. */
static inline uint64_t fpslocker_pace( uint64_t *due, uint64_t now, uint64_t period )
{
    uint64_t wait = 0;

    if (!period) return 0;
    if (!*due || now > *due + period) *due = now;
    else if (now < *due) wait = *due - now;
    *due += period;
    return wait;
}

#endif /* WINE_NX_FPSLOCKER_H */
