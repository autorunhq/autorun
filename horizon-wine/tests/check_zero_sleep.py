#!/usr/bin/env python3
"""Sleep(0) on Horizon (dlls/ntdll/unix/horizon_zero_sleep.h): a long run of
calls in a row sleeps a little each, so a thread waiting in a loop of them
leaves its core idle; a call after other work, and the first calls of a run,
only yield."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
unix = root / 'dlls/ntdll/unix'
test = r'''
#include <assert.h>
#include <stdio.h>
#include "horizon_zero_sleep.h"

/* One call at now; the caller's part: last is when the call returned. */
static long long call( struct horizon_zero_sleep *s, unsigned long long *now, unsigned long long gap )
{
    long long ns;

    *now += gap;
    ns = horizon_zero_sleep_ns( s, *now );
    *now += 10 + ns / 100;  /* the call itself: 1 us, and the sleep */
    s->last = *now;
    return ns;
}

int main(void)
{
    struct horizon_zero_sleep s = {0};
    unsigned long long now = 123456789;
    unsigned int i;

    /* A loop with next to nothing between its calls: yields first, ... */
    for (i = 0; i < HORIZON_ZERO_SLEEP_YIELDS; i++) assert( call( &s, &now, 20 ) == 0 );
    /* ... then sleeps, doubling every 16 calls, never past the cap. */
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 50000 );
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 100000 );
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 200000 );
    for (i = 0; i < 100000; i++) assert( call( &s, &now, 20 ) == HORIZON_ZERO_SLEEP_MAX );

    /* Other work between two calls starts the count again. */
    assert( call( &s, &now, HORIZON_ZERO_SLEEP_GAP ) == 0 );
    for (i = 1; i < HORIZON_ZERO_SLEEP_YIELDS; i++) assert( call( &s, &now, 20 ) == 0 );
    assert( call( &s, &now, 20 ) == 50000 );

    /* A thread that yields between pieces of work longer than the gap is
     * never slowed. */
    for (i = 0; i < 10000; i++) assert( call( &s, &now, HORIZON_ZERO_SLEEP_GAP + 50 ) == 0 );

    /* A wait that ends within the first calls of its run is not slowed. */
    for (i = 0; i < 50; i++) assert( call( &s, &now, 15 ) == 0 );
    assert( call( &s, &now, 5000 ) == 0 );

    /* The first call, and a clock that went back, only yield. */
    {
        struct horizon_zero_sleep fresh = {0};
        assert( horizon_zero_sleep_ns( &fresh, 5 ) == 0 && fresh.run == 0 );
        fresh.last = 1000; fresh.run = 500;
        assert( horizon_zero_sleep_ns( &fresh, 900 ) == 0 && fresh.run == 0 );
    }
    printf( "zero sleep: %u calls in a row yield, then 50-%lld us\n", HORIZON_ZERO_SLEEP_YIELDS,
            HORIZON_ZERO_SLEEP_MAX / 1000 );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-zero-sleep-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(unix), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# NtDelayExecution's zero timeout on the Switch: the schedule decides between
# a yield and an interruptible sleep, and the return time is kept per thread.
sync = (unix / 'sync.c').read_text()
assert '#include "horizon_zero_sleep.h"' in sync
delay = sync[sync.index('NTSTATUS WINAPI NtDelayExecution( BOOLEAN alertable'):]
delay = delay[:delay.index('\n}\n')]
zero = delay[delay.index('#ifdef __SWITCH__\n        /* Sleep(0)'):]
zero = zero[:zero.index('#endif')]
assert 'static __thread struct horizon_zero_sleep zero_sleep;' in zero
assert 'horizon_zero_sleep_ns( &zero_sleep, horizon_interrupt_time() )' in zero
assert re.search(r'if \(!ns\) status = NtYieldExecution\(\);', zero)
assert 'wine_nx_stop_sleep( ns )' in zero and 'wine_nx_stop_point()' in zero
assert zero.index('return status;') > zero.index('zero_sleep.last = horizon_interrupt_time();')
# An alertable Sleep(0) and a wait for ready socket operations still go to the
# server first, and longer sleeps are not changed.
assert delay.index('if (alertable)') < delay.index('horizon_async_any_ready()') < delay.index(zero)
assert delay.index(zero) < delay.index('status = NtYieldExecution();\n        if (!when) return status;')
print('zero sleep: a loop of Sleep(0) sleeps after a run of yields; the rest of NtDelayExecution unchanged')
