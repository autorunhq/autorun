/* Host tests for the Horizon thread lifecycle and synchronization state. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../../dlls/ntdll/unix/horizon_threads.h"

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Futex emulation with the kernel semantics the Switch build relies on:
 * the value check and the sleep are atomic with respect to wake. */
static pthread_mutex_t futex_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t futex_cond = PTHREAD_COND_INITIALIZER;
static int futex_wait( const int *addr, int value, long long ns )
{
    int ret = 0;
    pthread_mutex_lock( &futex_lock );
    if (__atomic_load_n( addr, __ATOMIC_ACQUIRE ) == value)
    {
        if (ns < 0) pthread_cond_wait( &futex_cond, &futex_lock );
        else
        {
            struct timespec ts;
            clock_gettime( CLOCK_REALTIME, &ts );
            ts.tv_sec += ns / 1000000000LL;
            ts.tv_nsec += ns % 1000000000LL;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait( &futex_cond, &futex_lock, &ts ) == ETIMEDOUT) ret = -1;
        }
    }
    pthread_mutex_unlock( &futex_lock );
    return ret;
}
static void futex_wake( const int *addr, int count )
{
    (void)addr; (void)count;
    pthread_mutex_lock( &futex_lock );
    pthread_cond_broadcast( &futex_cond );
    pthread_mutex_unlock( &futex_lock );
}

static pthread_mutex_t keyed_lock = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_keyed_waiter *keyed_queue;

static int keyed( unsigned long long object, unsigned long long key, int release, long long timeout_ns )
{
    struct horizon_keyed_waiter self = { NULL, object, key, release, 0 };
    return horizon_keyed_rendezvous( &keyed_lock, &keyed_queue, &self, timeout_ns, now_ns,
                                     futex_wait, futex_wake );
}

struct keyed_job { unsigned long long key; int release; int result; long long done_at; };
static void *keyed_thread( void *arg )
{
    struct keyed_job *job = arg;
    job->result = keyed( 1, job->key, job->release, 5000000000LL );
    job->done_at = now_ns();
    return NULL;
}

static void test_thread_state(void)
{
    struct horizon_thread_state t;
    int previous;

    horizon_thread_init( &t, 8, 1, 0x7, 123 );
    assert( horizon_thread_exit_status( &t ) == (int)HORIZON_THREAD_STILL_ACTIVE );
    assert( horizon_thread_info_flags( &t, 2 ) == 0 );
    assert( horizon_thread_info_flags( &t, 1 ) == HORIZON_THREAD_INFO_LAST );
    horizon_thread_set_exit_code( &t, 0x1234 );
    /* terminate_thread(self) only records the code; the thread is still live. */
    assert( horizon_thread_exit_status( &t ) == (int)HORIZON_THREAD_STILL_ACTIVE );
    assert( horizon_thread_mark_terminated( &t, 456 ) == 1 );
    assert( horizon_thread_mark_terminated( &t, 789 ) == 0 && t.exit_time == 456 );
    assert( horizon_thread_exit_status( &t ) == 0x1234 );
    horizon_thread_set_exit_code( &t, 0x5678 );
    assert( horizon_thread_exit_status( &t ) == 0x1234 );
    assert( horizon_thread_info_flags( &t, 1 ) == (HORIZON_THREAD_INFO_TERMINATED | HORIZON_THREAD_INFO_LAST) );

    /* Start gate and nested suspend counts. */
    horizon_thread_init( &t, 12, 1, 0x7, 0 );
    t.suspend = 1;
    assert( !horizon_thread_may_start( &t ) );
    assert( horizon_thread_suspend( &t, &previous ) == 0 && previous == 1 && t.suspend == 2 );
    assert( horizon_thread_resume( &t, &previous ) == 0 && previous == 2 && !horizon_thread_may_start( &t ) );
    assert( horizon_thread_resume( &t, &previous ) == 0 && previous == 1 && horizon_thread_may_start( &t ) );
    assert( horizon_thread_resume( &t, &previous ) == 0 && previous == 0 && t.suspend == 0 );
    t.started = 1;
    assert( horizon_thread_suspend( &t, &previous ) == 0 && previous == 0 && t.suspend == 1 );
    assert( horizon_thread_suspend( &t, &previous ) == 0 && previous == 1 && t.suspend == 2 );
    assert( horizon_thread_resume( &t, &previous ) == 0 && previous == 2 && t.suspend == 1 );
    assert( horizon_thread_resume( &t, &previous ) == 0 && previous == 1 && t.suspend == 0 );
    horizon_thread_mark_terminated( &t, 1 );
    assert( horizon_thread_suspend( &t, &previous ) == HORIZON_THREADS_STATUS_ACCESS_DENIED );
    horizon_thread_init( &t, 16, 1, 0x7, 0 );
    t.suspend = HORIZON_THREAD_MAX_SUSPEND;
    assert( horizon_thread_suspend( &t, &previous ) == HORIZON_THREADS_STATUS_SUSPEND_EXCEEDED );
    /* A terminated suspended thread must not keep its start gate closed. */
    horizon_thread_mark_terminated( &t, 1 );
    assert( horizon_thread_may_start( &t ) );
}

static void test_mutex(void)
{
    struct horizon_mutex_state m = { 0 };
    unsigned int previous;

    assert( horizon_mutex_signaled( &m, 8 ) && horizon_mutex_signaled( &m, 12 ) );
    assert( horizon_mutex_acquire( &m, 8 ) == 0 && m.owner == 8 && m.count == 1 );
    /* Recursive for the owner, not signaled for others. */
    assert( horizon_mutex_signaled( &m, 8 ) && !horizon_mutex_signaled( &m, 12 ) );
    assert( horizon_mutex_acquire( &m, 8 ) == 0 && m.count == 2 );
    assert( horizon_mutex_release( &m, 12, &previous ) == HORIZON_THREADS_STATUS_MUTANT_NOT_OWNED );
    assert( horizon_mutex_release( &m, 8, &previous ) == 0 && previous == 2 && m.owner == 8 );
    assert( horizon_mutex_release( &m, 8, &previous ) == 0 && previous == 1 && m.owner == 0 );
    assert( horizon_mutex_release( &m, 8, &previous ) == HORIZON_THREADS_STATUS_MUTANT_NOT_OWNED );
    /* Owner exits while holding it: the next owner is told exactly once. */
    horizon_mutex_acquire( &m, 8 );
    horizon_mutex_acquire( &m, 8 );
    assert( horizon_mutex_abandon( &m, 12 ) == 0 && m.count == 2 );
    assert( horizon_mutex_abandon( &m, 8 ) == 1 && m.count == 0 && m.abandoned );
    assert( horizon_mutex_signaled( &m, 12 ) );
    assert( horizon_mutex_acquire( &m, 12 ) == 1 && m.owner == 12 && !m.abandoned );
    assert( horizon_mutex_release( &m, 12, &previous ) == 0 );
    assert( horizon_mutex_acquire( &m, 16 ) == 0 );
}

static void test_keyed_events(void)
{
    struct keyed_job jobs[16];
    pthread_t threads[16];
    long long start;
    int i;

    /* Zero timeout without a peer, and odd timeout expiry, both leave no entry. */
    assert( keyed( 1, 0x10, 0, 0 ) == 1 && !keyed_queue );
    start = now_ns();
    assert( keyed( 1, 0x10, 1, 20000000LL ) == 1 && !keyed_queue );
    assert( now_ns() - start >= 20000000LL );

    /* Waiter first: it must stay blocked until the release arrives. */
    jobs[0] = (struct keyed_job){ 0x20, 0, -1, 0 };
    pthread_create( &threads[0], NULL, keyed_thread, &jobs[0] );
    usleep( 50000 );
    assert( jobs[0].result == -1 );
    start = now_ns();
    assert( keyed( 1, 0x20, 1, -1 ) == 0 );
    pthread_join( threads[0], NULL );
    assert( jobs[0].result == 0 && jobs[0].done_at >= start && !keyed_queue );

    /* Release first: it blocks until a waiter arrives (RtlRunOnce ordering). */
    jobs[0] = (struct keyed_job){ 0x30, 1, -1, 0 };
    pthread_create( &threads[0], NULL, keyed_thread, &jobs[0] );
    usleep( 50000 );
    assert( jobs[0].result == -1 );
    assert( keyed( 1, 0x30, 0, -1 ) == 0 );
    pthread_join( threads[0], NULL );
    assert( jobs[0].result == 0 && !keyed_queue );

    /* Other keys and other keyed-event objects never pair. */
    jobs[0] = (struct keyed_job){ 0x40, 0, -1, 0 };
    pthread_create( &threads[0], NULL, keyed_thread, &jobs[0] );
    usleep( 20000 );
    assert( keyed( 1, 0x44, 1, 30000000LL ) == 1 );
    assert( keyed( 2, 0x40, 1, 30000000LL ) == 1 );
    assert( jobs[0].result == -1 );
    assert( keyed( 1, 0x40, 1, -1 ) == 0 );
    pthread_join( threads[0], NULL );

    /* Many waiters and releases on one key pair one-to-one. */
    for (i = 0; i < 16; i++)
    {
        jobs[i] = (struct keyed_job){ 0x50, i & 1, -1, 0 };
        pthread_create( &threads[i], NULL, keyed_thread, &jobs[i] );
    }
    for (i = 0; i < 16; i++)
    {
        pthread_join( threads[i], NULL );
        assert( jobs[i].result == 0 );
    }
    assert( !keyed_queue );
}

static struct horizon_zombie_list zombies = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };
static int exited;

static void *zombie_thread( void *arg )
{
    struct horizon_zombie *node = malloc( sizeof(*node) );
    /* A zombie reaping must never join itself. */
    if (arg) horizon_zombie_reap( &zombies );
    __atomic_add_fetch( &exited, 1, __ATOMIC_SEQ_CST );
    horizon_zombie_push( &zombies, node, pthread_self() );
    return NULL;
}

static void test_reaper(void)
{
    pthread_t thread;
    unsigned int joined = 0;
    int i;

    for (i = 0; i < 200; i++)
    {
        assert( !pthread_create( &thread, NULL, zombie_thread, (void *)(long)(i % 3 == 0) ) );
        if (i % 7 == 0) joined += horizon_zombie_reap( &zombies );
    }
    while (__atomic_load_n( &exited, __ATOMIC_SEQ_CST ) < 200) usleep( 1000 );
    usleep( 20000 );
    joined += horizon_zombie_reap( &zombies );
    assert( zombies.reaped == 200 && zombies.pending == 0 && !zombies.head );
    assert( joined <= 200 );
}

int main(void)
{
    test_thread_state();
    test_mutex();
    test_keyed_events();
    test_reaper();
    puts( "Horizon threads: exit codes, start gate, mutex ownership/abandonment, keyed rendezvous, reaper passed" );
    return 0;
}
