/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Yields on Horizon: Sleep(0), SwitchToThread, and Wine's own yields after a
 * wait that timed out or a message poll that found nothing all come to
 * NtYieldExecution. Windows returns from a yield at once when no other thread
 * is ready to run, and so does svcSleepThread(-1): a thread that waits in a
 * loop of them keeps its core busy the whole time, through a system call each
 * turn that FEX, WoW64 and Wine make costly. Vampire: The Masquerade -
 * Redemption's main thread called Sleep(0) up to a million times a second, with
 * timeGetTime or nothing else between the calls, and held core 0 at 100%;
 * wined3d waits for its command stream the same way (wined3d_pause).
 *
 * Once a thread has yielded HORIZON_YIELD_BACKOFF_YIELDS times in a row with
 * less than HORIZON_YIELD_BACKOFF_GAP between one yield's return and the next,
 * each further yield sleeps instead: 50 us, doubling every 16 calls up to
 * 250 us. A yield that comes after the thread did something else for longer
 * starts again with plain yields, so a thread that yields between pieces of
 * work is not slowed, and a wait that ends within the first yields is not
 * either.
 */
#ifndef HORIZON_YIELD_BACKOFF_H
#define HORIZON_YIELD_BACKOFF_H

#define HORIZON_YIELD_BACKOFF_GAP     200ull    /* 100 ns units: 20 us */
#define HORIZON_YIELD_BACKOFF_YIELDS  64u       /* yields in a row that stay yields */
#define HORIZON_YIELD_BACKOFF_STEP    16u       /* calls between doublings */
#define HORIZON_YIELD_BACKOFF_FIRST   50000ll   /* ns */
#define HORIZON_YIELD_BACKOFF_MAX     250000ll  /* ns */

struct horizon_yield_backoff
{
    unsigned long long last;  /* when the thread's previous yield returned, 100 ns units; 0 before the first */
    unsigned int run;         /* yields in a row that each came within the gap */
};

/* How long the thread's yield at now (100 ns units) sleeps, in nanoseconds;
 * 0 is a plain yield. The caller sets state->last when the yield returns. */
static inline long long horizon_yield_backoff_ns( struct horizon_yield_backoff *state, unsigned long long now )
{
    unsigned int doublings;
    long long ns;

    if (state->last && now >= state->last && now - state->last < HORIZON_YIELD_BACKOFF_GAP)
    {
        if (state->run < 0x10000000u) state->run++;
    }
    else state->run = 0;

    if (state->run < HORIZON_YIELD_BACKOFF_YIELDS) return 0;
    doublings = (state->run - HORIZON_YIELD_BACKOFF_YIELDS) / HORIZON_YIELD_BACKOFF_STEP;
    if (doublings > 3) doublings = 3;
    ns = HORIZON_YIELD_BACKOFF_FIRST << doublings;
    return ns < HORIZON_YIELD_BACKOFF_MAX ? ns : HORIZON_YIELD_BACKOFF_MAX;
}

#endif /* HORIZON_YIELD_BACKOFF_H */
