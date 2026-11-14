/* Carafe's orchestrator behind its switch (source/orchestrator.c with the
 * policy it ships, on the host). Off, and before a program's settings are
 * read, the runtime's balancer does all the placing and the orchestrator
 * touches nothing. On, it watches until one core is full, then places the
 * threads itself, keeps the balancer away from the busiest thread's core and
 * stops calling the balancer.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
 *      -I tests/orchestrator-shims -I source tests/orchestrator_switch.c \
 *      source/orchestrator.c source/orchestrator_policy.c -o /tmp/orchestrator_switch
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "orchestrator_policy.h"

struct wine_nx_thread_view
{
    unsigned int tid;
    char kind;
    char name[32];
    unsigned int permille;
    u64 mask;
    int movable;
    int side;
};

void __wrap_wine_nx_thread_balance( void );

/* The console: a 19.2 MHz counter, four cores, the caller on one of them. */
#define PERIOD 38400000ull  /* two seconds */
static u64 tick = 1;
static u64 idle[4];
static u32 current_core;
static s32 own_core = 1;
static u64 own_mask = 7;
static unsigned int info_calls, own_mask_sets;

u64 armGetSystemTick( void ) { return tick; }
u32 svcGetCurrentProcessorNumber( void ) { return current_core; }
void svcSleepThread( s64 ns ) { (void)ns; }

Result svcGetInfo( u64 *out, u32 type, Handle handle, u64 sub )
{
    (void)handle; (void)sub;
    info_calls++;
    *out = type == InfoType_CoreMask ? 0xf : idle[current_core];
    return 0;
}

Result svcGetThreadCoreMask( s32 *core, u64 *mask, Handle handle )
{
    (void)handle;
    *core = own_core;
    *mask = own_mask;
    return 0;
}

Result __real_svcSetThreadCoreMask( Handle handle, s32 core, u32 mask )
{
    assert( handle == CUR_THREAD_HANDLE );
    own_mask_sets++;
    if (mask == (1u << core)) current_core = (u32)core;
    else { own_core = core; own_mask = mask; }
    return 0;
}

/* The runtime: its switch, its balancer, its threads and its log. */
static int setting = -1;  /* -1: the program's settings have not been read */
static unsigned int balancer_calls, place_calls, reserve_calls, traces, on_lines;
static s32 reserved = -2;
static struct wine_nx_thread_view threads[3];
static char last_trace[640];

bool carafeOrchestrating( void ) { return setting > 0; }
void __real_wine_nx_thread_balance( void ) { balancer_calls++; }
unsigned int wine_nx_thread_side_cores( void ) { return 8; }

void wine_nx_runtime_trace( const char *msg )
{
    traces++;
    if (strstr( msg, "[ORCHESTRATOR] on:" )) on_lines++;
    snprintf( last_trace, sizeof(last_trace), "%s", msg );
}

unsigned int wine_nx_thread_views( struct wine_nx_thread_view *views, unsigned int max )
{
    assert( max >= 3 );
    memcpy( views, threads, sizeof(threads) );
    return 3;
}

int wine_nx_thread_place( unsigned int tid, s32 core, u32 mask )
{
    unsigned int i;

    place_calls++;
    assert( mask && (mask & (1u << core)) );
    for (i = 0; i < 3; i++)
        if (threads[i].tid == tid)
        {
            assert( threads[i].movable );
            threads[i].mask = mask;
            return 1;
        }
    return 0;
}

void wine_nx_thread_reserve_core( s32 core )
{
    reserve_calls++;
    reserved = core;
}

/* One period of the log's thread: the cores were this busy, then it balances. */
static void period( unsigned int busy0, unsigned int busy1, unsigned int busy2, unsigned int busy3 )
{
    unsigned int busy[4] = { busy0, busy1, busy2, busy3 }, i;

    tick += PERIOD;
    for (i = 0; i < 4; i++) idle[i] += PERIOD / 1000 * (1000 - busy[i]);
    __wrap_wine_nx_thread_balance();
}

int main( void )
{
    unsigned int i, calls;

    /* Three program threads all on core 0, one of them filling it. */
    threads[0] = (struct wine_nx_thread_view){ 11, 'w', "main", 900, 1, 1, 0 };
    threads[1] = (struct wine_nx_thread_view){ 12, 'w', "audio", 400, 1, 1, 0 };
    threads[2] = (struct wine_nx_thread_view){ 13, 'w', "loader", 350, 1, 1, 0 };

    /* While the launcher is on the screen, and for a program that has not
     * asked: only the balancer, every period, and nothing read or moved. */
    for (i = 0; i < 4; i++) period( 1000, 0, 0, 100 );
    setting = 0;
    for (i = 0; i < 4; i++) period( 1000, 0, 0, 100 );
    assert( balancer_calls == 8 );
    assert( !place_calls && !reserve_calls && !traces && !info_calls && !own_mask_sets );

    /* Asked for: it says so once, and the first period only measures. */
    setting = 1;
    period( 1000, 0, 0, 100 );
    assert( on_lines == 1 && balancer_calls == 9 && !place_calls );
    assert( info_calls == 5 );                         /* the process's cores, then each core's idle time */
    assert( own_mask_sets == 5 && own_core == 1 && own_mask == 7 );  /* and back where it was */

    /* One core stays full: after the policy's two periods it places the
     * threads itself, off the full core, and the balancer is not called. */
    calls = balancer_calls;
    for (i = 0; i < 6 && !place_calls; i++) period( 1000, 0, 0, 100 );
    assert( place_calls >= 2 && i <= ORCHESTRATOR_ENTER_PERIODS + 1 );
    assert( balancer_calls == calls + i - 1 );
    assert( reserve_calls == 1 && reserved >= 0 && reserved <= 2 );
    assert( threads[0].mask == (1u << reserved) );     /* the busiest thread has that core */
    assert( threads[1].mask != threads[0].mask && threads[2].mask != threads[0].mask );
    assert( strstr( last_trace, "[ORCHESTRATOR] " ) && strstr( last_trace, "-> cores" ) );
    calls = balancer_calls;
    period( 900, 400, 350, 100 );
    assert( balancer_calls == calls && on_lines == 1 );

    /* Turned off again: the balancer has every period back. A program's
     * setting does not change while it runs; this is the way back all the same. */
    setting = 0;
    calls = balancer_calls;
    place_calls = 0;
    for (i = 0; i < 3; i++) period( 900, 400, 350, 100 );
    assert( balancer_calls == calls + 3 && !place_calls );
    assert( reserve_calls == 2 && reserved == -1 );    /* and every core */

    printf( "orchestrator switch: all checks passed\n" );
    return 0;
}
