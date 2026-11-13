/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Sleep(0) on Horizon. Windows returns from it at once when no other thread is
 * ready to run, and so does the yield it becomes here (svcSleepThread(-1)): a
 * thread that waits in a loop of Sleep(0) keeps its core busy the whole time,
 * through a system call each turn that FEX, WoW64 and Wine make costly.
 * Vampire: The Masquerade - Redemption's main thread called it up to a million
 * times a second, with timeGetTime or nothing else between the calls, and held
 * core 0 at 100%; wined3d waits for its command stream the same way.
 *
 * Once a thread has called it HORIZON_ZERO_SLEEP_YIELDS times in a row with
 * less than HORIZON_ZERO_SLEEP_GAP between one call's return and the next, each
 * further call sleeps instead of yielding: 50 us, doubling every 16 calls up to
 * 250 us. A call that comes after the thread did something else for longer
 * starts again with plain yields, so a thread that yields between pieces of
 * work is not slowed, and a wait that ends within the first calls is not either.
 */
#ifndef HORIZON_ZERO_SLEEP_H
#define HORIZON_ZERO_SLEEP_H

#define HORIZON_ZERO_SLEEP_GAP     200ull    /* 100 ns units: 20 us */
#define HORIZON_ZERO_SLEEP_YIELDS  64u       /* calls in a row that only yield */
#define HORIZON_ZERO_SLEEP_STEP    16u       /* calls between doublings */
#define HORIZON_ZERO_SLEEP_FIRST   50000ll   /* ns */
#define HORIZON_ZERO_SLEEP_MAX     250000ll  /* ns */

struct horizon_zero_sleep
{
    unsigned long long last;  /* when the thread's previous Sleep(0) returned, 100 ns units; 0 before the first */
    unsigned int run;         /* calls in a row that each came within the gap */
};

/* How long the thread's Sleep(0) at now (100 ns units) sleeps, in nanoseconds;
 * 0 is a plain yield. The caller sets state->last when the call returns. */
static inline long long horizon_zero_sleep_ns( struct horizon_zero_sleep *state, unsigned long long now )
{
    unsigned int doublings;
    long long ns;

    if (state->last && now >= state->last && now - state->last < HORIZON_ZERO_SLEEP_GAP)
    {
        if (state->run < 0x10000000u) state->run++;
    }
    else state->run = 0;

    if (state->run < HORIZON_ZERO_SLEEP_YIELDS) return 0;
    doublings = (state->run - HORIZON_ZERO_SLEEP_YIELDS) / HORIZON_ZERO_SLEEP_STEP;
    if (doublings > 3) doublings = 3;
    ns = HORIZON_ZERO_SLEEP_FIRST << doublings;
    return ns < HORIZON_ZERO_SLEEP_MAX ? ns : HORIZON_ZERO_SLEEP_MAX;
}

#endif /* HORIZON_ZERO_SLEEP_H */
