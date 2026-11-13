#!/usr/bin/env python3
"""Yields on Horizon (dlls/ntdll/unix/horizon_yield_backoff.h): a long quick
run of them (Sleep(0), SwitchToThread, Wine's own yields after a wait that timed
out or an empty message poll) sleeps a little each, so a thread waiting in such
a loop leaves its core idle; a yield after other work, and the first yields of a
run, stay yields. yield-backoff turns it off."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
unix = root / 'dlls/ntdll/unix'
test = r'''
#include <assert.h>
#include <stdio.h>
#include "horizon_yield_backoff.h"

/* One yield gap after the previous one returned; the caller's part: last is
 * when the yield returned. */
static long long call( struct horizon_yield_backoff *s, unsigned long long *now, unsigned long long gap )
{
    long long ns;

    *now += gap;
    ns = horizon_yield_backoff_ns( s, *now );
    *now += 10 + ns / 100;  /* the call itself: 1 us, and the sleep */
    s->last = *now;
    return ns;
}

int main(void)
{
    struct horizon_yield_backoff s = {0};
    unsigned long long now = 123456789;
    unsigned int i;

    /* A loop with next to nothing between its yields: yields first, ... */
    for (i = 0; i < HORIZON_YIELD_BACKOFF_YIELDS; i++) assert( call( &s, &now, 20 ) == 0 );
    /* ... then sleeps, doubling every 16 calls, never past the cap. */
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 50000 );
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 100000 );
    for (i = 0; i < 16; i++) assert( call( &s, &now, 20 ) == 200000 );
    for (i = 0; i < 100000; i++) assert( call( &s, &now, 20 ) == HORIZON_YIELD_BACKOFF_MAX );

    /* Other work between two yields starts the count again. */
    assert( call( &s, &now, HORIZON_YIELD_BACKOFF_GAP ) == 0 );
    for (i = 1; i < HORIZON_YIELD_BACKOFF_YIELDS; i++) assert( call( &s, &now, 20 ) == 0 );
    assert( call( &s, &now, 20 ) == 50000 );

    /* A thread that yields between pieces of work longer than the gap is
     * never slowed. */
    for (i = 0; i < 10000; i++) assert( call( &s, &now, HORIZON_YIELD_BACKOFF_GAP + 50 ) == 0 );

    /* A wait that ends within the first yields of its run is not slowed. */
    for (i = 0; i < 50; i++) assert( call( &s, &now, 15 ) == 0 );
    assert( call( &s, &now, 5000 ) == 0 );

    /* The first yield, and a clock that went back, stay yields. */
    {
        struct horizon_yield_backoff fresh = {0};
        assert( horizon_yield_backoff_ns( &fresh, 5 ) == 0 && fresh.run == 0 );
        fresh.last = 1000; fresh.run = 500;
        assert( horizon_yield_backoff_ns( &fresh, 900 ) == 0 && fresh.run == 0 );
    }
    printf( "yield backoff: %u yields in a row stay yields, then 50-%lld us\n", HORIZON_YIELD_BACKOFF_YIELDS,
            HORIZON_YIELD_BACKOFF_MAX / 1000 );
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-yield-backoff-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(unix), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# NtYieldExecution on the Switch: the schedule decides between a yield and a
# short sleep, behind yield-backoff, and the return time is kept per thread.
sync = (unix / 'sync.c').read_text()
assert '#include "horizon_yield_backoff.h"' in sync
yield_ = sync[sync.index('NTSTATUS WINAPI NtYieldExecution(void)'):]
yield_ = yield_[:yield_.index('#elif defined(HAVE_SCHED_YIELD)')]
assert re.search(r'\(!&wine_nx_yield_backoff \|\| wine_nx_yield_backoff\)\s*\?\s*'
                 r'horizon_yield_backoff_ns\( &yield_backoff, horizon_interrupt_time\(\) \) : 0;', yield_)
assert 'if (!ns) svcSleepThread( -1 );' in yield_ and 'svcSleepThread( ns );' in yield_
assert '__atomic_add_fetch( &wine_nx_yield_sleeps, 1, __ATOMIC_RELAXED );' in yield_
assert yield_.index('return STATUS_SUCCESS;') > yield_.index('yield_backoff.last = horizon_interrupt_time();')
# No stop point in it: Wine's own yields come from places (a thread taking its
# address wait out of the table as it ends) where a suspension must not start.
assert 'wine_nx_stop_point' not in yield_
assert 'static __thread struct horizon_yield_backoff yield_backoff;' in sync

# Sleep(0) comes to it through NtDelayExecution, unchanged from Wine's, after
# an alertable Sleep(0) and a wait for ready socket operations went to the
# server first.
delay = sync[sync.index('NTSTATUS WINAPI NtDelayExecution( BOOLEAN alertable'):]
delay = delay[:delay.index('\n}\n')]
assert 'horizon_yield_backoff' not in delay
assert delay.index('if (alertable)') < delay.index('horizon_async_any_ready()') < \
    delay.index('status = NtYieldExecution();\n        if (!when) return status;')

# yield-backoff: settings.json's, a program's yield-backoff=0 over it, the
# [INIT] line, and the count of yields that slept on the [PROGRESS] line.
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
assert 'int wine_nx_yield_backoff = 1;' in runtime
assert 'wine_nx_yield_backoff = config_bool( "yield-backoff", 1, "yield-backoff.txt", 0 );' in runtime
assert re.search(r'int backoff = launcher_setting_state\( &kv, "yield-backoff" \);\s*'
                 r'if \(backoff >= 0\) wine_nx_yield_backoff = backoff;', runtime)
assert runtime.index('config_bool( "yield-backoff"') < runtime.index('launcher_setting_state( &kv, "yield-backoff" )') \
    < runtime.index('log_line( "[INIT] yield backoff %s (yield-backoff)%s"')
assert '" yield_sleeps=%u"' in runtime and 'systop, yields, native, gl, audio, usb );' in runtime
print('yield backoff: Sleep(0), SwitchToThread and Wine\'s own yields sleep after a run of quick yields; '
      'yield-backoff turns it off')
