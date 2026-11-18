/* Carafe's reports on the host: what a thread waited for ([WAITS]), the
 * priority a thread waits for a lock at, and FEX's counters ([FEX]).
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
 *      -I tests/carafe-shims -I source tests/carafe_reports.c -o /tmp/carafe_reports
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

u64 test_tick = 1000;
Handle test_thread = 0x51;
s32 test_priority = 59;
unsigned int test_priority_sets;

static char traced[8][1024];
static unsigned int traces;

/* The profiler's registry: thread 0x51 is Windows thread 7, 0x52 is unknown. */
unsigned int wine_nx_thread_tid( Handle handle, char *kind, char *name, size_t size )
{
    if (handle != 0x51) return 0;
    *kind = 'w';
    snprintf( name, size, "render" );
    return 7;
}

/* What the wrapped calls do while the wrappers time them. */
static u64 wait_takes;
static s32 priority_inside = -1;
static unsigned int futex_calls, server_calls, lock_calls;

int __real_horizon_futex_wait( const int *addr, int value, long long timeout_ns )
{
    (void)addr; (void)value; (void)timeout_ns;
    futex_calls++;
    test_tick += wait_takes;
    return 3;
}

int __real_horizon_server_sync_call( unsigned int tid, const void *message, const void *data, unsigned int data_size,
                                     void *reply, void *reply_data )
{
    (void)tid; (void)message; (void)data; (void)data_size; (void)reply; (void)reply_data;
    server_calls++;
    test_tick += wait_takes;
    return 4;
}

Result __real_svcArbitrateLock( u32 wait_tag, u32 *tag_location, u32 self_tag )
{
    (void)wait_tag; (void)tag_location; (void)self_tag;
    lock_calls++;
    priority_inside = test_priority;
    test_tick += wait_takes;
    return 5;
}

#include "carafe_waits.c"
#include "carafe_lock_boost.c"
#include "carafe_fex_stats.c"

/* The reports look for these as the runtime's weak symbols, so they are
 * defined after the code that asks whether they are there. */
void wine_nx_runtime_trace( const char *msg )
{
    assert( traces < 8 );
    snprintf( traced[traces++], sizeof(traced[0]), "%s", msg );
}

void *wine_nx_fex_stats_base;
u32 wine_nx_fex_stats_size;

static void waits( void )
{
    /* Off: the calls go through untimed, and nothing is said. */
    wait_takes = 100;
    assert( __wrap_horizon_futex_wait( NULL, 0, -1 ) == 3 && futex_calls == 1 );
    assert( __wrap_horizon_server_sync_call( 1, NULL, NULL, 0, NULL, NULL ) == 4 && server_calls == 1 );
    carafeWaitsReport();
    assert( !traces );

    /* On: the first report only sets the start of the interval. */
    carafeWaitsEnable();
    carafeWaitsReport();
    assert( !traces );

    /* 1000 ticks pass: 250 in two sync waits and 100 in one server call on
     * the Windows thread, 500 in one sync wait on a thread nobody registered. */
    wait_takes = 125;
    assert( __wrap_horizon_futex_wait( NULL, 0, -1 ) == 3 );
    assert( __wrap_horizon_futex_wait( NULL, 0, -1 ) == 3 );
    wait_takes = 100;
    assert( __wrap_horizon_server_sync_call( 1, NULL, NULL, 0, NULL, NULL ) == 4 );
    test_thread = 0x52;
    wait_takes = 500;
    assert( __wrap_horizon_futex_wait( NULL, 0, -1 ) == 3 );
    test_thread = 0x51;
    test_tick += 150;
    carafeWaitsReport();
    assert( traces == 1 );
    /* The first sight of a thread counts nothing, so that a report never
     * covers more than its interval: both threads were first seen now. */
    assert( !strcmp( traced[0], "[WAITS] share of each thread's time blocked, by kind:" ) );

    wait_takes = 125;
    __wrap_horizon_futex_wait( NULL, 0, -1 );
    __wrap_horizon_futex_wait( NULL, 0, -1 );
    wait_takes = 100;
    __wrap_horizon_server_sync_call( 1, NULL, NULL, 0, NULL, NULL );
    test_thread = 0x52;
    wait_takes = 500;
    __wrap_horizon_futex_wait( NULL, 0, -1 );
    test_thread = 0x51;
    test_tick += 150;
    carafeWaitsReport();
    assert( traces == 2 );
    assert( !strcmp( traced[1], "[WAITS] share of each thread's time blocked, by kind:"
                                " native 0x52 sync 50.0% (1); 7w:render sync 25.0% (2) server 10.0% (1);" ) );
    traces = 0;
}

static void lock_boost( void )
{
    u32 tag = 0;

    /* Off, as it starts: the wait is timed and the priority is left alone. */
    assert( !carafe_lock_boost_enabled );
    test_priority_sets = 0;
    wait_takes = 40;
    assert( __wrap_svcArbitrateLock( 1, &tag, 2 ) == 5 && lock_calls == 1 );
    assert( priority_inside == 59 && test_priority == 59 && !test_priority_sets && !carafe_lock_boosts );

    /* On: a program thread waits one step higher and comes back. */
    carafe_lock_boost_enabled = 1;
    assert( __wrap_svcArbitrateLock( 1, &tag, 2 ) == 5 && lock_calls == 2 );
    assert( priority_inside == 58 && test_priority == 59 && test_priority_sets == 2 && carafe_lock_boosts == 1 );

    /* A thread at any other priority (audio, graphics workers) keeps it. */
    test_priority = 44;
    assert( __wrap_svcArbitrateLock( 1, &tag, 2 ) == 5 && lock_calls == 3 );
    assert( priority_inside == 44 && test_priority == 44 && test_priority_sets == 2 && carafe_lock_boosts == 1 );
    test_priority = 59;
    carafe_lock_boost_enabled = 0;

    /* The three waits show as lock time in the next report. */
    test_tick += 880;
    carafeWaitsReport();
    assert( traces == 1 );
    assert( !strcmp( traced[0], "[WAITS] share of each thread's time blocked, by kind:"
                                " 7w:render lock 12.0% (3);" ) );
    traces = 0;
}

static void fex_stats( void )
{
    static union { u8 bytes[4096]; u64 align; } area;
    FexStatsHeader *header = (FexStatsHeader *)area.bytes;
    FexThreadStats *first = (FexThreadStats *)(area.bytes + 64), *second = (FexThreadStats *)(area.bytes + 256);

    /* No area yet (stock FEX with the counters off): nothing is said. */
    carafeFexStatsReport();
    assert( !traces );

    header->version = 2;
    header->thread_stats_size = sizeof(*first);
    header->head = 64;
    header->size = 4096;
    first->next = 256;
    first->tid = 9;
    first->jit_count = 12;
    first->jit_time = 192;                       /* 10 us at 19.2 MHz */
    first->cache_miss_count = 3;
    second->tid = 11;
    second->jit_count = 400;
    second->jit_time = 19200;                    /* 1 ms */
    second->return_miss_count = 5;
    second->smc_count = 1;
    wine_nx_fex_stats_size = 4096;
    wine_nx_fex_stats_base = area.bytes;

    carafeFexStatsReport();
    assert( traces == 1 );
    assert( strstr( traced[0], "[FEX] since the last report, by thread: 11w compiled 400 blocks in 1000 us, "
                               "L1 lookup misses 0, returns off the call-return stack 5," ) == traced[0] );
    assert( strstr( traced[0], "code changes 1, disk cache 0 hits 0 misses in 0 us; 9w compiled 12 blocks in 10 us, "
                               "L1 lookup misses 3," ) );

    /* Only what was added since: thread 9 compiled 8 more, thread 11 nothing. */
    first->jit_count = 20;
    first->jit_time = 384;
    carafeFexStatsReport();
    assert( traces == 2 );
    assert( strstr( traced[1], " 9w compiled 8 blocks in 10 us," ) && !strstr( traced[1], " 11w " ) );

    carafeFexStatsReport();
    assert( traces == 3 && !strcmp( traced[2], "[FEX] since the last report, by thread: nothing compiled or looked up" ) );

    /* A record that would end past the area is not read. */
    first->jit_count = 30;
    header->head = 4096 - 8;
    carafeFexStatsReport();
    assert( traces == 4 && strstr( traced[3], "nothing compiled or looked up" ) );
    header->head = 64;

    /* A FEX whose record has another size: its counters are not guessed at. */
    header->thread_stats_size = 104;
    carafeFexStatsReport();
    assert( traces == 4 );
    traces = 0;
}

int main( void )
{
    waits();
    lock_boost();
    fex_stats();
    puts( "Carafe reports: waits by kind, the lock wait's priority, FEX's counters passed" );
    return 0;
}
