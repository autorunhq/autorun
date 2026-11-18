#!/usr/bin/env python3
"""The server thread pool (dlls/ntdll/unix/horizon.c, Carafe's patch 0023):
with Horizon synchronization and the program's setting, the clients' request
pipes share a few threads. The pool's own functions run here with real
threads: every request is served once and in its client's order, no client is
served by two threads at a time, a thread that waits inside a request counts
as blocked and another takes its place, the extra threads go again, and a
client that ends is finished once."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
settings = (root / 'horizon-wine/source/launcher_settings.h').read_text()
launcher = (root / 'horizon-wine/source/launcher.c').read_text()


def definition(name):
    match = re.search(r'^static (?:int|void|void \*|ssize_t)\s*' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


# A pipe tells the pool when it holds data or its writer has gone, under the
# pipe's lock, and a missing worker is started after the lock is released.
write = definition('horizon_pipe_write_r')
assert write.index('need_worker = pipe->on_data ? pipe->on_data( pipe->on_data_context ) : 0;') < \
       write.index('pthread_mutex_unlock( &pipe->mutex );', write.index('need_worker = pipe->on_data')) < \
       write.index('if (need_worker) horizon_server_pool_start_worker();')
close = definition('horizon_pipe_close_r')
assert close.index('if (pipe->on_data) need_worker = pipe->on_data( pipe->on_data_context );') < \
       close.index('pthread_mutex_unlock( &pipe->mutex );') < \
       close.index('if (need_worker) horizon_server_pool_start_worker();')

# A pool thread looks at a pipe's count and its writer's end without the pipe's
# lock, so both are stored atomically. (The thread sanitizer run below found
# the plain stores.)
assert '__atomic_store_n( &pipe->used, pipe->used + chunk, __ATOMIC_RELEASE );' in write
assert '__atomic_store_n( &pipe->used, pipe->used - chunk, __ATOMIC_RELEASE );' in definition('horizon_pipe_read_r')
assert '__atomic_store_n( &pipe->write_open, 0, __ATOMIC_RELEASE );' in close
assert not re.search(r'pipe->used\s*[-+]?=[^=]|pipe->write_open\s*=\s*0', source)
thread = definition('horizon_server_pool_thread')
assert '__atomic_load_n( &connection->request_pipe->used, __ATOMIC_ACQUIRE )' in thread
assert '__atomic_load_n( &connection->request_pipe->write_open, __ATOMIC_ACQUIRE )' in thread

# The three places where the server waits inside a request say so first.
for name, wait in (('horizon_server_sleep_locked', 'horizon_server_sleepers++;'),
                   ('horizon_sync_sleep_locked', 'for (unsigned int i = 0; i < waiter->count; i++)'),
                   ('horizon_server_wait_thread_locked', 'wine_nx_thread_wait_change( teb,')):
    body = definition(name)
    mark = 'if (horizon_server_pool_worker && !horizon_server_pool_blocked) horizon_server_pool_blocking_locked();'
    assert body.count(mark) == 1 and body.index(mark) < body.index(wait), name

# A client ends: the pipe forgets the pool before the connection is freed, and
# only a client with a thread of its own takes one off the count of threads.
finish = definition('horizon_server_finish_connection')
assert finish.index('connection->request_pipe->on_data = NULL;') < finish.index('free( connection );')
assert 'if (!connection->pooled) CARAFE_SERVER_STOPPED();' in finish
new_thread = definition('horizon_server_handle_new_thread')
assert 'if (horizon_server_pool_enabled())\n        errno = horizon_server_pool_add( thread_connection ) ? 0 : EBADF;' in new_thread

# A program's setting, off unless turned on, and only with Horizon synchronization.
assert 'int horizon_server_pool_wanted;' in source
assert 'settings->server_pool = launcher_setting_state( kv, "server-pool" ) == 1;' in settings
assert 'launcher_kv_set( kv, "server-pool", settings->server_pool ? "1" : NULL )' in settings
block = runtime[runtime.index("/* The program's own settings, written by the launcher next to it"):]
assert block.index('horizon_server_pool_wanted = 0;') < block.index('horizon_server_pool_wanted = settings.server_pool;')
assert 'ADD_ROW( ROW_SERVER_POOL, SECTION_EMULATION, "Shared server threads",' in launcher
assert 'row->disabled = !p->settings.fast_sync && !p->settings.server_pool;' in launcher
handler = launcher[launcher.index('case ROW_SERVER_POOL:'):]
assert 'p->settings.server_pool = action == UI_ACTION_RESET ? 0 : !p->settings.server_pool;' in handler[:handler.index('break;')]
assert 'pool=%u/%u/%u' in source

pool = re.search(r'^#define HORIZON_SERVER_POOL_SPARE \d+\n\nstatic struct\n\{.*?\} horizon_server_pool = \{[^;]*\};\n',
                 source, re.M | re.S)
assert pool
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "horizon_threads.h"

#define WARN(...) ((void)0)
#define CARAFE_DIAG_EVENT( kind, actor, target, value ) ((void)0)
#define CUR_THREAD_HANDLE 0
#define CLIENTS 8

/* What the pool uses of a request pipe and of a client's connection. */
struct horizon_pipe
{
    pthread_mutex_t mutex;
    size_t used;
    int write_open;
    int (*on_data)( void *context );
    void *on_data_context;
};

struct horizon_server_connection
{
    int request_fd;
    unsigned int tid;
    struct horizon_pipe *request_pipe;
    struct horizon_server_connection *pool_next;
    int pooled;
    int pool_queued;
    unsigned int pool_served;
    void *pool_diag;
    /* the test's */
    int serving, finished, blocks;
    unsigned int sent, next;
    unsigned int order[4096];
};

static struct horizon_pipe pipes[CLIENTS];
static struct horizon_server_connection clients[CLIENTS];
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t release_cond = PTHREAD_COND_INITIALIZER;
static struct horizon_zombie_list horizon_server_zombies = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };
static __thread struct horizon_server_connection *horizon_server_current;
static __thread int horizon_server_pool_worker, horizon_server_pool_blocked;
static int horizon_fast_sync_enabled, horizon_server_pool_wanted;
static int release_blocked, waiting_inside, overlaps, empty_serves, finishes, live_workers, started_workers;
static int refuse_threads;
/* As the runtime's quit_joinable: a thread that ends at a quit leaves itself
 * here to be joined. One that ends because the pool has too many leaves itself
 * on horizon_server_zombies, as in horizon.c. Each is joined once. */
static pthread_t quit_joinable[64];
static int quit_joinable_count;

static void horizon_server_pool_blocking_locked( void );

static struct horizon_pipe *horizon_pipe_from_fd( int fd )
{
    return fd >= 0 && fd < CLIENTS ? &pipes[fd] : NULL;
}

static void *carafe_diag_track( struct horizon_server_connection *connection ) { return connection; }
static int svcSetThreadPriority( int handle, int priority ) { (void)handle; (void)priority; return 0; }

/* The pool calls these two through weak references, as the runtime's. */
void wine_nx_thread_register( char kind, unsigned int tid, const char *name ) __attribute__((weak));
void wine_nx_thread_unregister( void ) __attribute__((weak));

/* A thread the pool asks for may be refused. */
static int test_pthread_create( pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg )
{
    if (__atomic_load_n( &refuse_threads, __ATOMIC_SEQ_CST )) return EAGAIN;
    return pthread_create( thread, attr, entry, arg );
}
#define pthread_create test_pthread_create

/* The pool's idle wait is the real horizon_cond_wait_quit, which looks for a
 * quit every quarter of a second. Here its timed wait has no time limit, so a
 * thread that is not told of work is seen as a request never served instead
 * of one served a little late. */
static int test_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline );
#define pthread_cond_timedwait test_cond_timedwait

/* One request of one client: read it, and for a client that blocks, wait in
 * the server as horizon_server_sleep_locked does. Returns 0 at the pipe's end. */
static int horizon_server_serve_one( struct horizon_server_connection *connection, void *diag,
                                     unsigned int *served )
{
    struct horizon_pipe *pipe = connection->request_pipe;
    unsigned int number;

    assert( horizon_server_pool_worker && horizon_server_current == connection && diag == connection );
    if (__atomic_exchange_n( &connection->serving, 1, __ATOMIC_SEQ_CST ))
        __atomic_add_fetch( &overlaps, 1, __ATOMIC_SEQ_CST );
    pthread_mutex_lock( &pipe->mutex );
    if (!pipe->used)
    {
        int open = pipe->write_open;

        pthread_mutex_unlock( &pipe->mutex );
        /* A thread sent here with nothing to read would sit in the read. */
        if (open) __atomic_add_fetch( &empty_serves, 1, __ATOMIC_SEQ_CST );
        __atomic_store_n( &connection->serving, 0, __ATOMIC_SEQ_CST );
        return 0;
    }
    __atomic_store_n( &pipe->used, pipe->used - 1, __ATOMIC_RELEASE );
    number = connection->next++;
    pthread_mutex_unlock( &pipe->mutex );
    connection->order[*served % 4096] = number;
    if (connection->blocks)
    {
        pthread_mutex_lock( &horizon_server_objects_mutex );
        if (horizon_server_pool_worker && !horizon_server_pool_blocked) horizon_server_pool_blocking_locked();
        waiting_inside++;
        while (!release_blocked) pthread_cond_wait( &release_cond, &horizon_server_objects_mutex );
        waiting_inside--;
        pthread_mutex_unlock( &horizon_server_objects_mutex );
    }
    (*served)++;
    __atomic_store_n( &connection->serving, 0, __ATOMIC_SEQ_CST );
    return 1;
}

static void horizon_server_finish_connection( struct horizon_server_connection *connection, void *diag,
                                              unsigned int served )
{
    assert( diag == connection && served == connection->pool_served && connection->pooled );
    assert( !__atomic_exchange_n( &connection->finished, 1, __ATOMIC_SEQ_CST ) );
    pthread_mutex_lock( &connection->request_pipe->mutex );
    connection->request_pipe->on_data = NULL;
    connection->request_pipe->on_data_context = NULL;
    pthread_mutex_unlock( &connection->request_pipe->mutex );
    __atomic_add_fetch( &finishes, 1, __ATOMIC_SEQ_CST );
}
'''
tests = r'''
#undef pthread_cond_timedwait
#undef pthread_create
/* The runtime's two, which horizon_cond_wait_quit reaches through weak
 * references: a thread that is told to quit ends where it stands. */
volatile int wine_nx_quit_requested;

void wine_nx_quit_point( void )
{
    int slot = __atomic_fetch_add( &quit_joinable_count, 1, __ATOMIC_SEQ_CST );

    assert( slot < 64 );
    quit_joinable[slot] = pthread_self();
    wine_nx_thread_unregister();
    pthread_exit( NULL );
}

static int test_cond_timedwait( pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline )
{
    (void)deadline;
    if (wine_nx_quit_requested) return ETIMEDOUT;
    return pthread_cond_wait( cond, mutex );
}

/* Defined after the pool's code, which tests them for NULL. */
void wine_nx_thread_register( char kind, unsigned int tid, const char *name )
{
    (void)name;
    assert( kind == 's' && !tid );
    __atomic_add_fetch( &live_workers, 1, __ATOMIC_SEQ_CST );
    __atomic_add_fetch( &started_workers, 1, __ATOMIC_SEQ_CST );
}

void wine_nx_thread_unregister( void ) { __atomic_sub_fetch( &live_workers, 1, __ATOMIC_SEQ_CST ); }

/* A client's side of its pipe, as horizon_pipe_write_r and horizon_pipe_close_r. */
static void client_write( unsigned int index )
{
    struct horizon_pipe *pipe = &pipes[index];
    int need_worker;

    pthread_mutex_lock( &pipe->mutex );
    __atomic_store_n( &pipe->used, pipe->used + 1, __ATOMIC_RELEASE );
    clients[index].sent++;
    need_worker = pipe->on_data ? pipe->on_data( pipe->on_data_context ) : 0;
    pthread_mutex_unlock( &pipe->mutex );
    if (need_worker) horizon_server_pool_start_worker();
}

static void client_close( unsigned int index )
{
    struct horizon_pipe *pipe = &pipes[index];
    int need_worker = 0;

    pthread_mutex_lock( &pipe->mutex );
    __atomic_store_n( &pipe->write_open, 0, __ATOMIC_RELEASE );
    if (pipe->on_data) need_worker = pipe->on_data( pipe->on_data_context );
    pthread_mutex_unlock( &pipe->mutex );
    if (need_worker) horizon_server_pool_start_worker();
}

static void pool_counts( unsigned int *workers, unsigned int *idle, unsigned int *blocked )
{
    pthread_mutex_lock( &horizon_server_pool.mutex );
    *workers = horizon_server_pool.workers;
    *idle = horizon_server_pool.idle;
    *blocked = horizon_server_pool.blocked;
    pthread_mutex_unlock( &horizon_server_pool.mutex );
}

static unsigned int served_of( unsigned int index )
{
    unsigned int served;

    pthread_mutex_lock( &horizon_server_pool.mutex );
    served = clients[index].pool_queued ? ~0u : clients[index].pool_served;
    pthread_mutex_unlock( &horizon_server_pool.mutex );
    return served;
}

#define WAIT_FOR( condition ) do { \
        int tries_; \
        for (tries_ = 0; tries_ < 20000 && !(condition); tries_++) usleep( 500 ); \
        if (!(condition)) { fprintf( stderr, "line %d: never true: %s\n", __LINE__, #condition ); abort(); } \
    } while (0)

static void *writer( void *arg )
{
    unsigned int first = (unsigned int)(size_t)arg, i;

    /* Two clients each, some requests sent before the last one was answered. */
    for (i = 0; i < 3000; i++)
    {
        client_write( first + (i & 1) );
        if (!(i % 7)) sched_yield();
    }
    return NULL;
}

int main( void )
{
    unsigned int workers, idle, blocked, i, j;
    pthread_t writers[3];

    /* The setting alone, or Horizon synchronization alone, is not the pool. */
    assert( !horizon_server_pool_enabled() );
    horizon_server_pool_wanted = 1;
    assert( !horizon_server_pool_enabled() );
    horizon_fast_sync_enabled = 1;
    assert( horizon_server_pool_enabled() );
    horizon_server_pool_wanted = 0;
    assert( !horizon_server_pool_enabled() );
    horizon_server_pool_wanted = 1;

    for (i = 0; i < CLIENTS; i++)
    {
        pthread_mutex_init( &pipes[i].mutex, NULL );
        pipes[i].write_open = 1;
        clients[i].request_fd = (int)i;
        clients[i].tid = 100 + i;
    }
    clients[0].request_fd = 99;
    assert( !horizon_server_pool_add( &clients[0] ) && !clients[0].pooled );  /* not a pipe */
    clients[0].request_fd = 0;

    /* The first client brings the spare threads; the others bring none. A
     * client that has already written is queued as it is added. */
    pipes[1].used = 2;
    clients[1].sent = 2;
    for (i = 0; i < CLIENTS; i++) assert( horizon_server_pool_add( &clients[i] ) && clients[i].pooled );
    WAIT_FOR( served_of( 1 ) == 2 );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == HORIZON_SERVER_POOL_SPARE && !blocked );
    WAIT_FOR( (pool_counts( &workers, &idle, &blocked ), idle == HORIZON_SERVER_POOL_SPARE) );
    assert( __atomic_load_n( &started_workers, __ATOMIC_SEQ_CST ) == HORIZON_SERVER_POOL_SPARE );

    /* Six clients write from three threads at once: every request is served,
     * once, in order, never by two threads at a time, by the same two threads. */
    for (i = 0; i < 3; i++) pthread_create( &writers[i], NULL, writer, (void *)(size_t)(2 + 2 * i) );
    for (i = 0; i < 3; i++) pthread_join( writers[i], NULL );
    for (i = 2; i < CLIENTS; i++) WAIT_FOR( served_of( i ) == 1500 );
    for (i = 2; i < CLIENTS; i++)
    {
        assert( clients[i].sent == 1500 && clients[i].next == 1500 );
        for (j = 0; j < 1500; j++) assert( clients[i].order[j] == j );
    }
    assert( !__atomic_load_n( &overlaps, __ATOMIC_SEQ_CST ) && !__atomic_load_n( &empty_serves, __ATOMIC_SEQ_CST ) );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == HORIZON_SERVER_POOL_SPARE && !blocked );
    assert( __atomic_load_n( &started_workers, __ATOMIC_SEQ_CST ) == HORIZON_SERVER_POOL_SPARE );

    /* Two requests wait inside the server, which is every thread there is: a
     * third thread starts and the other clients are served meanwhile. */
    clients[2].blocks = clients[3].blocks = 1;
    client_write( 2 );
    client_write( 3 );
    WAIT_FOR( (pool_counts( &workers, &idle, &blocked ), blocked == 2 && workers == 3) );
    client_write( 2 );  /* behind the one that waits: its client's order holds */
    for (i = 0; i < 50; i++) client_write( 4 + (i & 3) );
    for (i = 4; i < CLIENTS; i++) WAIT_FOR( served_of( i ) == 1500 + (50 + 3 - (i - 4)) / 4 );
    assert( clients[2].pool_served == 1500 && clients[3].pool_served == 1500 );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == 3 && blocked == 2 );

    /* The third waits too, and no thread can be had: the request that comes
     * then stays in its pipe, and is served when a thread is free again. */
    clients[4].blocks = 1;
    client_write( 4 );
    WAIT_FOR( (pool_counts( &workers, &idle, &blocked ), blocked == 3 && workers == 4) );
    clients[5].blocks = 1;
    __atomic_store_n( &refuse_threads, 1, __ATOMIC_SEQ_CST );
    client_write( 5 );
    WAIT_FOR( (pool_counts( &workers, &idle, &blocked ), blocked == 4) );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == 4 );  /* the refused one is not counted */
    client_write( 6 );
    usleep( 20000 );
    assert( clients[6].pool_served == 1512 );
    /* A thread can be had again: the next request that arrives starts it, and
     * it serves the one that was left as well. */
    __atomic_store_n( &refuse_threads, 0, __ATOMIC_SEQ_CST );
    client_write( 7 );
    WAIT_FOR( served_of( 6 ) == 1513 && served_of( 7 ) == 1513 );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == 5 && blocked == 4 );

    /* They stop waiting: nothing is blocked, what was behind them is served,
     * and a thread that comes back for work while the spare ones are idle ends. */
    clients[2].blocks = clients[3].blocks = clients[4].blocks = clients[5].blocks = 0;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    release_blocked = 1;
    pthread_cond_broadcast( &release_cond );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    WAIT_FOR( served_of( 2 ) == 1502 && served_of( 3 ) == 1501 && served_of( 4 ) == 1514 && served_of( 5 ) == 1514 );
    WAIT_FOR( (pool_counts( &workers, &idle, &blocked ), workers == HORIZON_SERVER_POOL_SPARE && !blocked) );
    WAIT_FOR( __atomic_load_n( &live_workers, __ATOMIC_SEQ_CST ) == HORIZON_SERVER_POOL_SPARE );
    assert( !__atomic_load_n( &overlaps, __ATOMIC_SEQ_CST ) && !__atomic_load_n( &empty_serves, __ATOMIC_SEQ_CST ) );

    /* The clients end, one with a request still unread: each is finished
     * once, after its last request, and its pipe no longer calls the pool. */
    client_write( 3 );
    for (i = 0; i < CLIENTS; i++) client_close( i );
    WAIT_FOR( __atomic_load_n( &finishes, __ATOMIC_SEQ_CST ) == CLIENTS );
    for (i = 0; i < CLIENTS; i++)
    {
        assert( clients[i].finished && clients[i].pool_served == clients[i].sent && !pipes[i].on_data );
        client_close( i );
    }
    assert( __atomic_load_n( &finishes, __ATOMIC_SEQ_CST ) == CLIENTS );
    assert( !__atomic_load_n( &overlaps, __ATOMIC_SEQ_CST ) && !__atomic_load_n( &empty_serves, __ATOMIC_SEQ_CST ) );
    pool_counts( &workers, &idle, &blocked );
    assert( workers == HORIZON_SERVER_POOL_SPARE && !blocked );

    /* The program quits: the idle threads end where they wait. */
    pthread_mutex_lock( &horizon_server_pool.mutex );
    wine_nx_quit_requested = 1;
    pthread_cond_broadcast( &horizon_server_pool.cond );
    pthread_mutex_unlock( &horizon_server_pool.mutex );
    WAIT_FOR( !__atomic_load_n( &live_workers, __ATOMIC_SEQ_CST ) );
    assert( __atomic_load_n( &quit_joinable_count, __ATOMIC_SEQ_CST ) == HORIZON_SERVER_POOL_SPARE );
    for (i = 0; i < HORIZON_SERVER_POOL_SPARE; i++) pthread_join( quit_joinable[i], NULL );
    /* The ones that ended earlier are joined by whoever reaps next, each once:
     * every thread the pool started has now been joined. */
    horizon_zombie_reap( &horizon_server_zombies );
    assert( !horizon_server_zombies.head && !horizon_server_zombies.pending );
    assert( horizon_server_zombies.reaped + HORIZON_SERVER_POOL_SPARE ==
            (unsigned int)__atomic_load_n( &started_workers, __ATOMIC_SEQ_CST ) );
    assert( __atomic_load_n( &started_workers, __ATOMIC_SEQ_CST ) == 5 );
    puts( "server pool: shared threads, order, blocked threads, retiring, ending clients and quit passed" );
    return 0;
}
'''
functions = ''.join(definition('horizon_server_pool_' + name) for name in (
    'enabled', 'start_worker', 'append_locked', 'ready', 'blocking_locked', 'add', 'thread'))
functions = definition('horizon_cond_wait_quit') + functions
functions = functions.replace('#ifndef HORIZON_STANDALONE_SYNTAX\n', '').replace('#endif\n', '')
program = fixture + pool.group(0) + 'static void *horizon_server_pool_thread( void *param );\n' + functions + tests
cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory(prefix='wine-nx-server-pool-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(program)
    for sanitizer in ('address,undefined', 'thread'):
        subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
                        '-fsanitize=' + sanitizer, '-I' + str(root / 'dlls/ntdll/unix'), str(c), '-o', str(exe)],
                       check=True)
        run = subprocess.run([str(exe)], timeout=120, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if run.returncode and 'unexpected memory mapping' in run.stdout and shutil.which('setarch'):
            # The thread sanitizer of some compilers cannot place its shadow
            # memory where the kernel randomizes addresses widely.
            run = subprocess.run(['setarch', os.uname().machine, '-R', str(exe)], timeout=120,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        sys.stdout.write(run.stdout)
        if run.returncode:
            sys.exit('server pool: the test built with -fsanitize=%s failed (%d)' % (sanitizer, run.returncode))
print('server pool: ok')
