/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Yields on Horizon: Sleep(0), SwitchToThread, and Wine's own yields after a
 * wait that timed out or a message poll that found nothing all come to
 * NtYieldExecution. Windows returns from a yield at once when no other thread
 * is ready to run, and so does svcSleepThread(-1): a thread that waits in a
 * loop of them keeps its core busy the whole time, through a system call each
 * turn that FEX, WoW64 and Wine make costly. Vampire: The Masquerade -
 * Redemption's main thread called Sleep(0) up to a million times a second, with
 * timeGetTime or nothing else between the calls, and held core 0 at 100%.
 *
 * Once a thread has been yielding for HORIZON_YIELD_BACKOFF_AFTER without a
 * break (less than HORIZON_YIELD_BACKOFF_GAP between one yield's return and
 * the next), each further yield sleeps HORIZON_YIELD_BACKOFF_SLEEP instead. A
 * yield that comes after the thread did something else for longer starts the
 * run again with plain yields.
 *
 * The run is measured in time, not in calls: a first version slept after 64
 * quick yields, 50 us doubling to 250 us, and Vampire's 3D scenes fell from
 * 45-60 frames a second to 11-23. Its main thread waited in some 120 runs of
 * Sleep(0) a frame, each well under a millisecond (wined3d waits for its
 * command stream that way, wined3d_pause), and every one of them came back
 * late. A wait shorter than 2 ms is now never slowed, and a longer one by at
 * most 100 us, while a frame limiter or an idle loop that waits longer leaves
 * its core idle.
 */
#ifndef HORIZON_YIELD_BACKOFF_H
#define HORIZON_YIELD_BACKOFF_H

#define HORIZON_YIELD_BACKOFF_GAP     200ull     /* 100 ns units: 20 us */
#define HORIZON_YIELD_BACKOFF_AFTER   20000ull   /* 100 ns units: 2 ms */
#define HORIZON_YIELD_BACKOFF_SLEEP   100000ll   /* ns: 100 us */

struct horizon_yield_backoff
{
    unsigned long long last;   /* when the thread's previous yield returned, 100 ns units; 0 before the first */
    unsigned long long start;  /* when the run of quick yields it belongs to began */
};

/* How long the thread's yield at now (100 ns units) sleeps, in nanoseconds;
 * 0 is a plain yield. The caller sets state->last when the yield returns. */
static inline long long horizon_yield_backoff_ns( struct horizon_yield_backoff *state, unsigned long long now )
{
    if (!state->last || now < state->last || now - state->last >= HORIZON_YIELD_BACKOFF_GAP)
        state->start = now;
    return now - state->start >= HORIZON_YIELD_BACKOFF_AFTER ? HORIZON_YIELD_BACKOFF_SLEEP : 0;
}

#endif /* HORIZON_YIELD_BACKOFF_H */
