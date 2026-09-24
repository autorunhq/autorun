#!/usr/bin/env python3
"""Run the Horizon server's overlapped socket path (dlls/ntdll/unix/horizon.c)
against real host sockets: listen, AcceptEx with and without the first data,
accept(), a recv that goes pending and completes when data comes, a completion
routine, cancelling by closing, and an ioctl done at once.

The Sims 2 Legacy's launcher emulation (anadius) and the game's own LSX client
are written on Asio, which on Windows is nothing but overlapped sockets on an
I/O completion port: it ties each socket to the port, queues AcceptEx and
WSARecv, and waits in GetQueuedCompletionStatus. Every case here is one where a
missing or extra packet leaves it waiting for good."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text(errors='surrogateescape')
header = (root / 'dlls/ntdll/unix/horizon_async.h').read_text()
sockaddr_header = (root / 'dlls/ntdll/unix/horizon_sockaddr.h').read_text()


def definition(name):
    """The body of a static function, skipping its forward declaration."""
    for match in re.finditer(r'^static [^;{]*?\b' + name + r'\(', source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end]
    raise SystemExit(f'{name} not found')


def struct(name):
    start = source.index(f'struct {name}\n{{')
    return source[start:source.index('};', start) + 2]


defines = '\n'.join(line for line in source.splitlines()
                    if re.match(r'#define HORIZON_(STATUS_\w+ |APC_|ASYNC_STALE|NT_ERROR|WS_AF_INET6?\b|'
                                r'IOCTL_AFD_WINE_(GET_INFO|[GS]ET_IPV6_V6ONLY)\b)', line))

# The select handler takes a result before anything else, and hands out a
# system APC before it would wait, but not before a signal-and-wait signals.
select = definition('horizon_server_handle_select')
assert select.index('horizon_server_async_result_locked') < select.index('for (int initial')
assert select.index('horizon_server_async_apc_locked') < select.index('HORIZON_SELECT_ALERTABLE')
assert select.index('horizon_server_signal_object_locked') < select.index('horizon_server_async_apc_locked')
# Closing a socket cancels what waits on it; the ioctl handler tells what ends at once.
assert 'horizon_async_cancel( &horizon_asyncs, handle' in definition('horizon_server_close_object_handle')
ioctl = definition('horizon_server_handle_ioctl')
assert 'horizon_server_ioctl_start' in ioctl and 'horizon_server_ioctl_done' in ioctl
for code in ('LISTEN', 'WINE_ACCEPT', 'WINE_ACCEPT_INTO', 'WINE_SET_SO_REUSEADDR', 'WINE_SET_TCP_NODELAY'):
    assert f'case HORIZON_IOCTL_AFD_{code}:' in ioctl, code
# Ports can be tied to sockets.
assert 'HORIZON_SERVER_OBJECT_SOCK' in definition('horizon_server_find_io_object_locked')
assert 'horizon_server_find_io_object_locked' in definition('horizon_server_handle_set_completion_info')

functions = '\n\n'.join(definition(name) for name in (
    'horizon_sock_errno_status', 'horizon_ws_sockaddr_to_unix_for', 'horizon_ws_sockaddr_from_unix_as',
    'horizon_server_get_sock_fd', 'horizon_sock_ioctl_create',
    'horizon_sock_ioctl_connect',
    'horizon_sock_ioctl_family',
    'horizon_server_async_create_locked', 'horizon_server_async_free_locked',
    'horizon_server_accepted_sock_locked', 'horizon_sock_accept_output_locked',
    'horizon_sock_accept_async_locked', 'horizon_sock_poll_asyncs_locked',
    'horizon_sock_ioctl_listen', 'horizon_sock_ioctl_accept', 'horizon_sock_ioctl_accept_into',
    'horizon_server_ioctl_start', 'horizon_server_ioctl_done', 'horizon_server_select_signals',
    'horizon_async_finish_locked', 'horizon_server_async_result_locked', 'horizon_server_async_apc_locked'))

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__linux__) || defined(__CYGWIN__)
#define sin_len sin_zero[0]
#endif

@DEFINES@
#define HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define LONG int

@HEADER@
@SOCKADDR@

enum { HORIZON_SERVER_OBJECT_EVENT = 1, HORIZON_SERVER_OBJECT_COMPLETION, HORIZON_SERVER_OBJECT_SOCK };
enum { HORIZON_SELECT_NONE, HORIZON_SELECT_WAIT, HORIZON_SELECT_WAIT_ALL, HORIZON_SELECT_SIGNAL_AND_WAIT };

struct horizon_server_request_header { int req; unsigned int request_size; unsigned int reply_size; };
@SELECT@

struct horizon_server_object
{
    int type, refs, signaled, file_fd;
    unsigned int file_access, file_options;
    int sock_bound, sock_nonblocking;
    int sock_family, sock_type, sock_protocol, sock_v6only;
    unsigned int sock_event_handle;
    int sock_event_mask, sock_pending_events;
    struct horizon_server_object *file_completion;
    unsigned long long file_completion_key;
    unsigned int file_completion_flags;
    struct { unsigned int tid; } thread;
    struct horizon_server_object *thread_next;
};
struct horizon_server_handle_entry { unsigned int handle; struct horizon_server_object *object; };
struct horizon_server_connection { unsigned int tid; };

static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_async_list horizon_asyncs;
static unsigned int horizon_async_apc_ids;
static struct horizon_server_object *horizon_server_threads;
static struct horizon_server_object objects[32];
static struct horizon_server_handle_entry entries[32];
static unsigned int next_handle = 1;
static unsigned long long fake_now = 1000000;
static int woken, freed, forgotten[8], nforgotten;
static struct { void *port; unsigned long long key, value; unsigned int status; unsigned long long info; } posts[16];
static int nposts;
static struct { unsigned int tid; unsigned char call[64]; } user_apcs[4];
static int nuser_apcs;

static void horizon_trace( const char *format, ... ) { (void)format; }
static void wine_nx_runtime_trace( const char *message ) { (void)message; }
static void horizon_report_async( const char *what, const struct horizon_async *async, unsigned int status )
{ (void)what; (void)async; (void)status; }
static unsigned long long horizon_async_now( void ) { return fake_now; }
static void horizon_sock_poller_start( void ) {}
static void horizon_sync_notify_object_locked(struct horizon_server_object *o, int satisfy) { (void)o; (void)satisfy; woken++; }
static void horizon_server_free_object( struct horizon_server_object *object ) { (void)object; freed++; }
static void horizon_client_forget_fd( unsigned int handle ) { forgotten[nforgotten++] = handle; }

static struct horizon_server_handle_entry *horizon_server_find_handle_locked( unsigned int handle )
{
    if (!handle || handle >= next_handle || !entries[handle].object) return NULL;
    return &entries[handle];
}

static struct horizon_server_handle_entry *horizon_server_create_handle_locked( int type )
{
    unsigned int handle = next_handle++;

    entries[handle].handle = handle;
    entries[handle].object = &objects[handle];
    memset( &objects[handle], 0, sizeof(objects[handle]) );
    objects[handle].type = type;
    objects[handle].refs = 1;
    objects[handle].file_fd = -1;
    return &entries[handle];
}

static unsigned int horizon_server_find_sock_locked( unsigned int handle, struct horizon_server_object **object )
{
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked( handle );

    *object = NULL;
    if (!entry) return HORIZON_STATUS_INVALID_HANDLE;
    if (entry->object->type != HORIZON_SERVER_OBJECT_SOCK) return HORIZON_STATUS_OBJECT_TYPE_MISMATCH;
    *object = entry->object;
    return HORIZON_STATUS_SUCCESS;
}

static void horizon_server_post_completion_locked( struct horizon_server_object *port, unsigned long long ckey,
                                                   unsigned long long cvalue, unsigned int status,
                                                   unsigned long long information )
{
    posts[nposts].port = port;
    posts[nposts].key = ckey;
    posts[nposts].value = cvalue;
    posts[nposts].status = status;
    posts[nposts].info = information;
    nposts++;
}

static unsigned int horizon_server_queue_user_apc_locked( struct horizon_server_object *thread,
                                                          const unsigned char *call, unsigned int size )
{
    assert( size == 64 );
    user_apcs[nuser_apcs].tid = thread->thread.tid;
    memcpy( user_apcs[nuser_apcs].call, call, size );
    nuser_apcs++;
    return HORIZON_STATUS_SUCCESS;
}

static void horizon_async_finish_locked( struct horizon_async *async, unsigned int status,
                                         unsigned long long total );

@FUNCTIONS@

static unsigned int u32( const unsigned char *p ) { unsigned int v; memcpy( &v, p, 4 ); return v; }
static unsigned long long u64( const unsigned char *p ) { unsigned long long v; memcpy( &v, p, 8 ); return v; }
static unsigned int ws_port( const unsigned char *ws ) { unsigned short v; memcpy( &v, ws + 2, 2 ); return ntohs( v ); }

static unsigned int new_sock( int bind_it, struct sockaddr_in *addr )
{
    struct horizon_server_handle_entry *entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_SOCK );
    int fd = socket( AF_INET, SOCK_STREAM, 0 );

    fcntl( fd, F_SETFL, O_NONBLOCK );
    entry->object->file_fd = fd;
    if (bind_it)
    {
        socklen_t len = sizeof(*addr);

        memset( addr, 0, sizeof(*addr) );
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        assert( !bind( fd, (struct sockaddr *)addr, sizeof(*addr) ) );
        getsockname( fd, (struct sockaddr *)addr, &len );
    }
    return entry->handle;
}

static int connect_to( const struct sockaddr_in *addr, unsigned int *local_port )
{
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    int fd = socket( AF_INET, SOCK_STREAM, 0 );

    assert( !connect( fd, (const struct sockaddr *)addr, sizeof(*addr) ) );
    getsockname( fd, (struct sockaddr *)&local, &len );
    *local_port = ntohs( local.sin_port );
    return fd;
}

/* What server_select sends after running an APC: the result, then the wait. */
static void send_result( unsigned int apc, unsigned int status, unsigned int total )
{
    struct horizon_select_request request;
    unsigned char data[HORIZON_APC_RESULT_SIZE];
    unsigned int type = HORIZON_APC_ASYNC_IO;

    memset( &request, 0, sizeof(request) );
    request.prev_apc = apc;
    memset( data, 0, sizeof(data) );
    memcpy( data, &type, 4 );
    memcpy( data + 4, &status, 4 );
    memcpy( data + 8, &total, 4 );
    horizon_server_async_result_locked( &request, data, sizeof(data) );
}

/* Poll until the socket's operation is ready, as the poller does every 5 ms. */
static int poll_ready( unsigned int handle )
{
    int i;

    for (i = 0; i < 200; i++)
    {
        if (horizon_sock_poll_asyncs_locked( handle, entries[handle].object )) return 1;
        usleep( 1000 );
    }
    return 0;
}

int main( void )
{
    struct horizon_server_connection owner = { 0x20 }, other = { 0x30 };
    struct horizon_server_object owner_thread;
    struct horizon_server_object *port, *listener;
    struct horizon_async_data data;
    struct horizon_async *async;
    struct sockaddr_in listen_addr;
    unsigned char call[64], out[16], buffer[16];
    unsigned int listener_h, target_h, target2_h, event_h, params[3], apc, client_port, out_size;
    int client, client2, client3, old_fd, backlog[3] = { 0, 5, 0 };

    memset( &owner_thread, 0, sizeof(owner_thread) );
    owner_thread.thread.tid = owner.tid;
    horizon_server_threads = &owner_thread;
    port = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_COMPLETION )->object;
    event_h = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_EVENT )->handle;

    /* listen: refused before bind, as server/sock.c does. */
    listener_h = new_sock( 0, &listen_addr );
    listener = entries[listener_h].object;
    assert( horizon_sock_ioctl_listen( listener_h, (unsigned char *)backlog, sizeof(backlog) ) ==
            HORIZON_STATUS_INVALID_PARAMETER );
    close( listener->file_fd );
    listener_h = new_sock( 1, &listen_addr );
    listener = entries[listener_h].object;
    listener->sock_bound = 1;
    assert( !horizon_sock_ioctl_listen( listener_h, (unsigned char *)backlog, sizeof(backlog) ) );
    /* CreateIoCompletionPort( listener, port, 0x77 ). */
    listener->file_completion = port;
    listener->file_completion_key = 0x77;
    port->refs++;

    /* AcceptEx with no data asked for: pending until someone connects. */
    target_h = new_sock( 0, NULL );
    old_fd = entries[target_h].object->file_fd;
    memset( &data, 0, sizeof(data) );
    data.handle = listener_h; data.iosb = 0x1000; data.user = 0xa1; data.apc_context = 0x2000;
    params[0] = target_h; params[1] = 0; params[2] = 32;
    assert( horizon_sock_ioctl_accept_into( &owner, &data, (unsigned char *)params, sizeof(params), 16 ) ==
            HORIZON_STATUS_BUFFER_TOO_SMALL );
    assert( horizon_sock_ioctl_accept_into( &owner, &data, (unsigned char *)params, sizeof(params), 64 ) ==
            HORIZON_STATUS_PENDING );
    async = horizon_async_find_user( &horizon_asyncs, 0xa1 );
    assert( async && async->state == HORIZON_ASYNC_QUEUED && async->pending && async->port == port );
    assert( port->refs == 3 );
    assert( !horizon_sock_poll_asyncs_locked( listener_h, listener ) );
    client = connect_to( &listen_addr, &client_port );
    assert( poll_ready( listener_h ) );
    assert( async->state == HORIZON_ASYNC_READY && async->status == HORIZON_STATUS_ALERTED );
    /* The connection went into the socket given, whose old descriptor the client forgets. */
    assert( entries[target_h].object->file_fd != -1 && nforgotten == 1 && forgotten[0] == target_h );
    assert( entries[target_h].object->sock_bound );
    (void)old_fd;
    /* What GetAcceptExSockaddrs reads: [len][local] [len][remote]. */
    assert( async->out && async->out_size == 64 && async->out_info == 0 );
    assert( u32( async->out ) == 16 && ws_port( async->out + 4 ) == ntohs( listen_addr.sin_port ) );
    assert( u32( async->out + 32 ) == 16 && ws_port( async->out + 36 ) == client_port );
    /* Another thread waits for its turn; the one that started it runs it. */
    assert( !horizon_server_async_apc_locked( &other, call ) );
    apc = horizon_server_async_apc_locked( &owner, call );
    assert( apc && async->state == HORIZON_ASYNC_RUNNING );
    assert( u32( call ) == HORIZON_APC_ASYNC_IO && u32( call + 4 ) == HORIZON_STATUS_ALERTED );
    assert( u64( call + 8 ) == 0xa1 && u64( call + 16 ) == 0x1000 && u32( call + 24 ) == 0 );
    send_result( apc, HORIZON_STATUS_SUCCESS, 0 );
    assert( !horizon_asyncs.head && nposts == 1 );
    assert( posts[0].port == port && posts[0].key == 0x77 && posts[0].value == 0x2000 && !posts[0].status );
    assert( port->refs == 2 );
    /* The accepted socket carries the connection. */
    assert( send( client, "x", 1, 0 ) == 1 );
    usleep( 20000 );
    assert( recv( entries[target_h].object->file_fd, buffer, 1, 0 ) == 1 && buffer[0] == 'x' );

    /* WSARecv with nothing there: the client tried, got EAGAIN, went pending. */
    entries[target_h].object->file_completion = port;
    entries[target_h].object->file_completion_key = 0x88;
    port->refs++;
    entries[event_h].object->signaled = 1;
    memset( &data, 0, sizeof(data) );
    data.handle = target_h; data.iosb = 0x3000; data.user = 0xb1; data.apc_context = 0x4000; data.event = event_h;
    async = horizon_server_async_create_locked( &owner, &data, HORIZON_ASYNC_READ, HORIZON_ASYNC_IO );
    assert( async->id && async->state == HORIZON_ASYNC_DIRECT && !entries[event_h].object->signaled );
    async->state = HORIZON_ASYNC_QUEUED;   /* set_async_direct_result( STATUS_PENDING ) */
    async->pending = 1;
    assert( !horizon_sock_poll_asyncs_locked( target_h, entries[target_h].object ) );
    assert( send( client, "hello", 5, 0 ) == 5 );
    assert( poll_ready( target_h ) );
    apc = horizon_server_async_apc_locked( &owner, call );
    assert( apc && u32( call + 4 ) == HORIZON_STATUS_ALERTED && u64( call + 8 ) == 0xb1 );
    /* Not ready after all: it waits for the socket again. */
    send_result( apc, HORIZON_STATUS_PENDING, 0 );
    assert( async->state == HORIZON_ASYNC_QUEUED && nposts == 1 );
    assert( poll_ready( target_h ) );
    apc = horizon_server_async_apc_locked( &owner, call );
    send_result( apc, HORIZON_STATUS_SUCCESS, 5 );
    assert( nposts == 2 && posts[1].key == 0x88 && posts[1].value == 0x4000 && posts[1].info == 5 );
    assert( entries[event_h].object->signaled );
    recv( entries[target_h].object->file_fd, buffer, sizeof(buffer), 0 );

    /* A ready operation whose thread never waits goes to one that does. */
    data.user = 0xb2; data.event = 0;
    async = horizon_server_async_create_locked( &owner, &data, HORIZON_ASYNC_READ, HORIZON_ASYNC_IO );
    async->state = HORIZON_ASYNC_QUEUED;
    async->pending = 1;
    assert( send( client, "later", 5, 0 ) == 5 );
    assert( poll_ready( target_h ) );
    assert( !horizon_server_async_apc_locked( &other, call ) );
    fake_now += HORIZON_ASYNC_STALE;
    apc = horizon_server_async_apc_locked( &other, call );
    assert( apc && u64( call + 8 ) == 0xb2 );
    send_result( apc, HORIZON_STATUS_SUCCESS, 5 );
    assert( nposts == 3 && posts[2].info == 5 );
    recv( entries[target_h].object->file_fd, buffer, sizeof(buffer), 0 );

    /* A completion routine instead of the port, on the thread that started it. */
    data.user = 0xc1; data.apc = 0x5000; data.apc_context = 0x6000; data.iosb = 0x7000;
    async = horizon_server_async_create_locked( &owner, &data, HORIZON_ASYNC_READ, HORIZON_ASYNC_IO );
    async->state = HORIZON_ASYNC_QUEUED;
    async->pending = 1;
    assert( send( client, "apc", 3, 0 ) == 3 );
    assert( poll_ready( target_h ) );
    apc = horizon_server_async_apc_locked( &owner, call );
    send_result( apc, HORIZON_STATUS_SUCCESS, 3 );
    assert( nposts == 3 && nuser_apcs == 1 && user_apcs[0].tid == owner.tid );
    assert( u32( user_apcs[0].call ) == HORIZON_APC_USER && u64( user_apcs[0].call + 8 ) == 0x5000 );
    assert( u64( user_apcs[0].call + 16 ) == 0x6000 && u64( user_apcs[0].call + 24 ) == 0x7000 );
    recv( entries[target_h].object->file_fd, buffer, sizeof(buffer), 0 );

    /* Closing the socket: what waits ends, through the client, as wineserver ends it. */
    data.user = 0xd1; data.apc = 0; data.apc_context = 0x8000;
    async = horizon_server_async_create_locked( &owner, &data, HORIZON_ASYNC_READ, HORIZON_ASYNC_IO );
    async->state = HORIZON_ASYNC_QUEUED;
    async->pending = 1;
    assert( horizon_async_cancel( &horizon_asyncs, target_h, 0, 0, fake_now, 1 ) == 1 );
    apc = horizon_server_async_apc_locked( &owner, call );
    assert( u32( call + 4 ) == HORIZON_ASYNC_STATUS_HANDLES_CLOSED && u32( call + 24 ) == 0 );
    send_result( apc, HORIZON_ASYNC_STATUS_HANDLES_CLOSED, 0 );
    /* A warning, not an error: GetQueuedCompletionStatus says 676, as desktop Wine does. */
    assert( nposts == 4 && posts[3].status == HORIZON_ASYNC_STATUS_HANDLES_CLOSED && posts[3].value == 0x8000 );

    /* AcceptEx asking for the first 8 bytes: it has its connection, then waits for them. */
    target2_h = new_sock( 0, NULL );
    memset( &data, 0, sizeof(data) );
    data.handle = listener_h; data.user = 0xe1; data.apc_context = 0x9000;
    params[0] = target2_h; params[1] = 8; params[2] = 32;
    assert( horizon_sock_ioctl_accept_into( &owner, &data, (unsigned char *)params, sizeof(params), 8 + 64 ) ==
            HORIZON_STATUS_PENDING );
    async = horizon_async_find_user( &horizon_asyncs, 0xe1 );
    client2 = connect_to( &listen_addr, &client_port );
    usleep( 20000 );
    assert( !horizon_sock_poll_asyncs_locked( listener_h, listener ) );
    assert( async->accepted && async->state == HORIZON_ASYNC_QUEUED && nforgotten == 2 );
    assert( send( client2, "ABCDEFGHIJ", 10, 0 ) == 10 );
    assert( poll_ready( listener_h ) );
    assert( async->out_info == 8 && !memcmp( async->out, "ABCDEFGH", 8 ) );
    assert( ws_port( async->out + 8 + 4 ) == ntohs( listen_addr.sin_port ) );
    assert( ws_port( async->out + 8 + 32 + 4 ) == client_port );
    apc = horizon_server_async_apc_locked( &owner, call );
    assert( u32( call + 24 ) == 8 );   /* the status block's Information */
    send_result( apc, HORIZON_STATUS_SUCCESS, 8 );
    assert( nposts == 5 && posts[4].info == 8 && posts[4].key == 0x77 );

    /* accept(): nonblocking says so, blocking waits, and a waiting connection is taken at once. */
    listener->sock_event_mask = 0x10;
    memset( &data, 0, sizeof(data) );
    data.handle = listener_h; data.user = 0xf1; data.event = event_h;
    listener->sock_nonblocking = 1;
    out_size = 0;
    assert( horizon_sock_ioctl_accept( &owner, &data, out, sizeof(out), &out_size ) ==
            HORIZON_STATUS_DEVICE_NOT_READY );
    listener->sock_nonblocking = 0;
    assert( horizon_sock_ioctl_accept( &owner, &data, out, sizeof(out), &out_size ) == HORIZON_STATUS_PENDING );
    async = horizon_async_find_user( &horizon_asyncs, 0xf1 );
    client3 = connect_to( &listen_addr, &client_port );
    assert( poll_ready( listener_h ) );
    assert( async->out_size == 4 && async->out_status == HORIZON_STATUS_SUCCESS );
    {
        unsigned int accepted = u32( async->out );

        assert( entries[accepted].object->type == HORIZON_SERVER_OBJECT_SOCK );
        assert( entries[accepted].object->file_fd != -1 && entries[accepted].object->sock_bound );
        assert( entries[accepted].object->sock_event_mask == 0x10 );
    }
    entries[event_h].object->signaled = 0;
    apc = horizon_server_async_apc_locked( &owner, call );
    send_result( apc, HORIZON_STATUS_SUCCESS, 4 );
    assert( entries[event_h].object->signaled );   /* what ws2_32's accept waits on */
    close( client3 );
    client3 = connect_to( &listen_addr, &client_port );
    usleep( 20000 );
    out_size = 0;
    assert( !horizon_sock_ioctl_accept( &owner, &data, out, sizeof(out), &out_size ) && out_size == 4 );

    /* ConnectEx that connects at once: its packet still goes to the port. */
    memset( &data, 0, sizeof(data) );
    data.handle = listener_h; data.apc_context = 0xa000; data.event = event_h;
    entries[event_h].object->signaled = 1;
    horizon_server_ioctl_start( &data );
    assert( !entries[event_h].object->signaled );
    horizon_server_ioctl_done( owner.tid, &data, HORIZON_STATUS_SUCCESS, 0 );
    assert( nposts == 6 && posts[5].value == 0xa000 && entries[event_h].object->signaled );
    horizon_server_ioctl_done( owner.tid, &data, HORIZON_STATUS_DEVICE_NOT_READY, 0 );
    listener->file_completion_flags = HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    horizon_server_ioctl_done( owner.tid, &data, HORIZON_STATUS_SUCCESS, 0 );
    assert( nposts == 6 );
    /* ws2_32's own calls pass no context: nothing to post. */
    listener->file_completion_flags = 0;
    data.apc_context = 0; data.event = 0;
    horizon_server_ioctl_done( owner.tid, &data, HORIZON_STATUS_SUCCESS, 0 );
    assert( nposts == 6 );

    /* A signal-and-wait is recognised with or without the APC result before it. */
    {
        struct horizon_select_request request;
        unsigned char select_data[HORIZON_APC_RESULT_SIZE + 16];
        int op = HORIZON_SELECT_SIGNAL_AND_WAIT;

        memset( &request, 0, sizeof(request) );
        memset( select_data, 0, sizeof(select_data) );
        request.size = 16;
        memcpy( select_data + HORIZON_APC_RESULT_SIZE, &op, sizeof(op) );
        assert( horizon_server_select_signals( &request, select_data, sizeof(select_data) ) );
        op = HORIZON_SELECT_WAIT;
        memcpy( select_data + HORIZON_APC_RESULT_SIZE, &op, sizeof(op) );
        assert( !horizon_server_select_signals( &request, select_data, sizeof(select_data) ) );
    }

    /* DirtySock, which The Sims 2 Legacy reaches its launcher with: an AF_INET6
     * stream socket, IPv4 underneath, V6ONLY cleared, connected to
     * ::ffff:127.0.0.1 -- anadius's IPv4 listener. */
    {
        unsigned int v6_h = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_SOCK )->handle;
        unsigned int into_h, got, value;
        int create[4] = { HORIZON_WS_AF_INET6, 1, 0, 0 }, info[3];
        unsigned char connect6[8 + 28];
        struct horizon_async *accept6;

        assert( !horizon_sock_ioctl_create( v6_h, (unsigned char *)create, sizeof(create) ) );
        assert( entries[v6_h].object->sock_family == HORIZON_WS_AF_INET6 && entries[v6_h].object->sock_v6only );
        got = 0;
        assert( !horizon_sock_ioctl_family( HORIZON_IOCTL_AFD_WINE_GET_INFO, v6_h, NULL, 0,
                                            (unsigned char *)info, sizeof(info), &got ) );
        assert( got == 12 && info[0] == HORIZON_WS_AF_INET6 && info[1] == 1 && info[2] == 0 );

        memset( connect6, 0, sizeof(connect6) );
        connect6[0] = 28;                                   /* afd_connect_params.addr_len */
        connect6[8] = HORIZON_WS_AF_INET6;
        memcpy( connect6 + 8 + 2, &listen_addr.sin_port, 2 );
        connect6[8 + 18] = connect6[8 + 19] = 0xff;
        connect6[8 + 20] = 127; connect6[8 + 23] = 1;
        /* IPv6 only, as Windows makes one: IPv4 written the IPv6 way is not reached. */
        assert( horizon_sock_ioctl_connect( v6_h, connect6, sizeof(connect6) ) == HORIZON_STATUS_NETWORK_UNREACHABLE );
        value = 0;
        got = 0;
        assert( !horizon_sock_ioctl_family( HORIZON_IOCTL_AFD_WINE_SET_IPV6_V6ONLY, v6_h,
                                            (unsigned char *)&value, sizeof(value), NULL, 0, &got ) );
        value = 1;
        assert( !horizon_sock_ioctl_family( HORIZON_IOCTL_AFD_WINE_GET_IPV6_V6ONLY, v6_h, NULL, 0,
                                            (unsigned char *)&value, sizeof(value), &got ) && got == 4 && !value );
        /* An IPv4 socket has no V6ONLY to read. */
        assert( horizon_sock_ioctl_family( HORIZON_IOCTL_AFD_WINE_GET_IPV6_V6ONLY, target_h, NULL, 0,
                                           (unsigned char *)&value, sizeof(value), &got ) ==
                HORIZON_STATUS_INVALID_PARAMETER );

        /* The listener's AcceptEx, into a socket made the same way as the listener: IPv6. */
        into_h = new_sock( 0, NULL );
        entries[into_h].object->sock_family = HORIZON_WS_AF_INET6;
        memset( &data, 0, sizeof(data) );
        data.handle = listener_h; data.user = 0x61; data.apc_context = 0xb000;
        params[0] = into_h; params[1] = 0; params[2] = 28 + 16;
        assert( horizon_sock_ioctl_accept_into( &owner, &data, (unsigned char *)params, sizeof(params),
                                                2 * (28 + 16) ) == HORIZON_STATUS_PENDING );
        accept6 = horizon_async_find_user( &horizon_asyncs, 0x61 );
        assert( !horizon_sock_ioctl_connect( v6_h, connect6, sizeof(connect6) ) );
        assert( poll_ready( listener_h ) );
        /* [len][sockaddr_in6 ::ffff:127.0.0.1:listener] [len][sockaddr_in6 ::ffff:127.0.0.1:client] */
        assert( u32( accept6->out ) == 28 && accept6->out[4] == HORIZON_WS_AF_INET6 );
        assert( ws_port( accept6->out + 4 ) == ntohs( listen_addr.sin_port ) );
        assert( accept6->out[4 + 8 + 10] == 0xff && accept6->out[4 + 8 + 12] == 127 );
        assert( u32( accept6->out + 44 ) == 28 && accept6->out[48] == HORIZON_WS_AF_INET6 );
        apc = horizon_server_async_apc_locked( &owner, call );
        send_result( apc, HORIZON_STATUS_SUCCESS, 0 );
        assert( nposts == 7 && posts[6].value == 0xb000 );
        /* Too late to change once bound. */
        entries[v6_h].object->sock_bound = 1;
        assert( horizon_sock_ioctl_family( HORIZON_IOCTL_AFD_WINE_SET_IPV6_V6ONLY, v6_h,
                                           (unsigned char *)&value, sizeof(value), NULL, 0, &got ) ==
                HORIZON_STATUS_INVALID_PARAMETER );
        close( entries[v6_h].object->file_fd );
        close( entries[into_h].object->file_fd );
    }

    /* A nonblocking connect must return to ws2_32 while it is in progress. */
    {
        struct sockaddr_in connect_addr;
        unsigned char connect4[8 + 16] = {0};
        unsigned int connect_h, connect_listener_h;
        int addr_len = 16;

        connect_listener_h = new_sock( 1, &connect_addr );
        entries[connect_listener_h].object->sock_bound = 1;
        assert( !listen( entries[connect_listener_h].object->file_fd, 1 ) );
        connect_h = new_sock( 0, NULL );
        entries[connect_h].object->sock_nonblocking = 1;
        memcpy( connect4, &addr_len, sizeof(addr_len) );
        connect4[8] = HORIZON_WS_AF_INET;
        memcpy( connect4 + 10, &connect_addr.sin_port, sizeof(connect_addr.sin_port) );
        connect4[12] = 127;
        connect4[15] = 1;
        assert( horizon_sock_ioctl_connect( connect_h, connect4, sizeof(connect4) ) ==
                HORIZON_STATUS_DEVICE_NOT_READY );
        close( entries[connect_h].object->file_fd );
        close( entries[connect_listener_h].object->file_fd );
    }

    assert( !horizon_asyncs.head && port->refs == 3 );
    close( client ); close( client2 ); close( client3 );
    printf( "overlapped sockets: AcceptEx, accept, pending recv, completion routine, cancel and ConnectEx "
            "end on the port as Windows ends them; IPv6 and nonblocking connect semantics passed\n" );
    return 0;
}
'''

fixture = (fixture.replace('@DEFINES@', defines)
                  .replace('@HEADER@', header)
                  .replace('@SOCKADDR@', sockaddr_header)
                  .replace('@SELECT@', struct('horizon_select_request') + '\n' + struct('horizon_select_signal_and_wait_op'))
                  .replace('@FUNCTIONS@', functions))

with tempfile.TemporaryDirectory(prefix='wine-nx-async-sockets-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(fixture, errors='surrogateescape')
    subprocess.run(['cc', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    print(subprocess.run([str(tmp / 'test')], check=True, capture_output=True, text=True).stdout.strip())
