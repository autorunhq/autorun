/*
 * Operations on a socket that wait: what server/async.c keeps for them, cut to
 * what the in-process server needs.
 *
 * A program that asks for overlapped I/O is told ERROR_IO_PENDING and expects
 * to hear later -- through the event it gave, a completion routine, or a packet
 * on the completion port the socket is tied to. Asio, which is what EA's
 * launcher emulation and a good many games are written on, does nothing else:
 * it queues an AcceptEx and a WSARecv and waits in GetQueuedCompletionStatus.
 *
 * The client does every recv and send itself on the socket's fd. When one
 * would block, the server keeps it as a queued async; when the fd is ready, the
 * async is handed to the thread that started it as an APC_ASYNC_IO system APC
 * at its next wait, the client's own callback tries the operation again and
 * writes the status block, and the result comes back with that thread's next
 * select. Only then is the completion signalled, by the rules below, which are
 * async_set_result's.
 *
 * An accept is the other way round: the server accepts, into the socket the
 * program gave it, and keeps what AcceptEx writes for the client's callback to
 * fetch with get_async_result.
 *
 * Everything here is bookkeeping; the caller holds the server's object lock.
 */
#ifndef WINE_NX_HORIZON_ASYNC_H
#define WINE_NX_HORIZON_ASYNC_H

#include <stdlib.h>
#include <string.h>

#define HORIZON_ASYNC_DIRECT  0  /* the client is trying it now */
#define HORIZON_ASYNC_QUEUED  1  /* waiting for its socket to be ready */
#define HORIZON_ASYNC_READY   2  /* ready: the thread that started it runs it at its next wait */
#define HORIZON_ASYNC_RUNNING 3  /* that thread has the APC and has not answered yet */

#define HORIZON_ASYNC_READ    0  /* recv and accept, which wait for POLLIN */
#define HORIZON_ASYNC_WRITE   1  /* send, which waits for POLLOUT */

/* What kind of operation it is. */
#define HORIZON_ASYNC_IO          0  /* a recv or send, which the client does */
#define HORIZON_ASYNC_ACCEPT      1  /* accept(): the server makes a new socket */
#define HORIZON_ASYNC_ACCEPT_INTO 2  /* AcceptEx: the server accepts into the socket given */

#define HORIZON_ASYNC_STATUS_ALERTED   0x00000101u  /* the operation is to be done now */
#define HORIZON_ASYNC_STATUS_CANCELLED 0xc0000120u
#define HORIZON_ASYNC_STATUS_HANDLES_CLOSED 0x8000000au  /* a warning: the port still hears of it */

/* What the completion of one async has to do. */
#define HORIZON_ASYNC_POST    0x1  /* a packet on the socket's completion port */
#define HORIZON_ASYNC_APC     0x2  /* the program's completion routine, as a user APC */
#define HORIZON_ASYNC_EVENT   0x4  /* the event the program gave */

/* struct async_data, as the client sends it with every request that can wait;
 * the server declares it itself, beside the requests that carry it. */
#ifndef HORIZON_ASYNC_DATA_DEFINED
#define HORIZON_ASYNC_DATA_DEFINED 1
struct horizon_async_data
{
    unsigned int handle;
    unsigned int event;
    unsigned long long iosb;
    unsigned long long user;
    unsigned long long apc;
    unsigned long long apc_context;
};
#endif

struct horizon_async
{
    struct horizon_async *next;
    unsigned int id;           /* the wait handle the client knows it by */
    unsigned int apc_id;       /* the system APC running it, while it runs */
    unsigned int owner_tid;    /* the thread that started it, which runs its callback */
    unsigned int sock;         /* the socket it waits on */
    int direction;
    int state;
    int pending;               /* it went pending, or was marked so: its completion is always told */
    struct horizon_async_data data;
    unsigned int status;       /* what the APC tells the client: ALERTED to do it, or how it ended */
    unsigned long long ready_at;
    unsigned long long deadline;
    /* The socket's completion port as it was when the operation started,
     * referenced, which is where the result goes even if the socket is closed
     * before it comes. */
    void *port;
    unsigned long long port_key;
    unsigned int port_flags;
    /* An accept, which the server does. */
    int kind;
    unsigned int accept_into;  /* the socket accepted into (AcceptEx) */
    int accepted;              /* AcceptEx has its connection and waits for the first data */
    unsigned int recv_len, local_len, out_size;
    unsigned char *out;        /* what get_async_result hands back */
    unsigned int out_status;
    unsigned int out_info;     /* the status block's Information: bytes received */
};

struct horizon_async_list
{
    struct horizon_async *head;
    unsigned int next_id;      /* ids the client sees, never 0 */
    int ready;                 /* how many are ready and not yet handed to a thread */
};

static inline unsigned int horizon_async_new_id( struct horizon_async_list *list )
{
    if (!++list->next_id) list->next_id = 1;
    return list->next_id;
}

/* Appended, so that asyncs on one socket and in one direction complete in the
 * order they were started, as a stream socket's bytes have to. */
static inline void horizon_async_add( struct horizon_async_list *list, struct horizon_async *async )
{
    struct horizon_async **ptr = &list->head;

    while (*ptr) ptr = &(*ptr)->next;
    async->next = NULL;
    *ptr = async;
}

static inline void horizon_async_remove( struct horizon_async_list *list, struct horizon_async *async )
{
    struct horizon_async **ptr;

    for (ptr = &list->head; *ptr; ptr = &(*ptr)->next)
    {
        if (*ptr != async) continue;
        *ptr = async->next;
        async->next = NULL;
        return;
    }
}

static inline void horizon_async_free( struct horizon_async *async )
{
    if (!async) return;
    free( async->out );
    free( async );
}

static inline struct horizon_async *horizon_async_find_id( const struct horizon_async_list *list, unsigned int id )
{
    struct horizon_async *async;

    if (!id) return NULL;
    for (async = list->head; async; async = async->next) if (async->id == id) return async;
    return NULL;
}

static inline struct horizon_async *horizon_async_find_apc( const struct horizon_async_list *list,
                                                            unsigned int apc_id )
{
    struct horizon_async *async;

    if (!apc_id) return NULL;
    for (async = list->head; async; async = async->next)
        if (async->state == HORIZON_ASYNC_RUNNING && async->apc_id == apc_id) return async;
    return NULL;
}

/* get_async_result names an async by the client's own pointer to it. */
static inline struct horizon_async *horizon_async_find_user( const struct horizon_async_list *list,
                                                             unsigned long long user )
{
    struct horizon_async *async;

    for (async = list->head; async; async = async->next) if (async->data.user == user) return async;
    return NULL;
}

static inline void horizon_async_ready( struct horizon_async_list *list, struct horizon_async *async,
                                        unsigned int status, unsigned long long now )
{
    if (async->state != HORIZON_ASYNC_READY) __atomic_add_fetch( &list->ready, 1, __ATOMIC_RELAXED );
    async->state = HORIZON_ASYNC_READY;
    async->status = status;
    async->ready_at = now;
}

/* A ready one handed to a thread as a system APC. */
static inline void horizon_async_run( struct horizon_async_list *list, struct horizon_async *async,
                                      unsigned int apc_id )
{
    if (async->state == HORIZON_ASYNC_READY) __atomic_sub_fetch( &list->ready, 1, __ATOMIC_RELAXED );
    async->state = HORIZON_ASYNC_RUNNING;
    async->apc_id = apc_id;
}

/* The one to run next on this thread: one the thread started, or one that has
 * waited stale ticks for a thread that has not waited since. Windows does the
 * I/O in no thread of the program's; Wine interrupts the thread that started
 * it with a signal, which Horizon has no way to do, and any thread can run the
 * client's side of it -- it reads into the program's buffers and writes the
 * status block, which is the same from every thread. */
static inline struct horizon_async *horizon_async_ready_for( const struct horizon_async_list *list,
                                                             unsigned int tid, unsigned long long now,
                                                             unsigned long long stale )
{
    struct horizon_async *async;

    for (async = list->head; async; async = async->next)
        if (async->state == HORIZON_ASYNC_READY && async->owner_tid == tid) return async;
    for (async = list->head; async; async = async->next)
        if (async->state == HORIZON_ASYNC_READY && now - async->ready_at >= stale) return async;
    return NULL;
}

/* Whether one is ready and has waited that long, for which every waiting
 * thread is woken to look. */
static inline int horizon_async_any_stale( const struct horizon_async_list *list, unsigned long long now,
                                           unsigned long long stale )
{
    struct horizon_async *async;

    for (async = list->head; async; async = async->next)
        if (async->state == HORIZON_ASYNC_READY && now - async->ready_at >= stale) return 1;
    return 0;
}

/* Whether any is waiting for its socket, which the poller then watches
 * closely. */
static inline int horizon_async_any_queued( const struct horizon_async_list *list )
{
    struct horizon_async *async;

    for (async = list->head; async; async = async->next)
        if (async->state == HORIZON_ASYNC_QUEUED || async->state == HORIZON_ASYNC_READY) return 1;
    return 0;
}

/* The oldest one waiting on this socket in this direction, and only if none
 * before it is still being run: the next recv must not overtake the one whose
 * callback is still reading. */
static inline struct horizon_async *horizon_async_next_queued( const struct horizon_async_list *list,
                                                               unsigned int sock, int direction )
{
    struct horizon_async *async;

    for (async = list->head; async; async = async->next)
    {
        if (async->sock != sock || async->direction != direction) continue;
        if (async->state == HORIZON_ASYNC_QUEUED) return async;
        if (async->state == HORIZON_ASYNC_READY || async->state == HORIZON_ASYNC_RUNNING) return NULL;
    }
    return NULL;
}

/* Whether anything on this socket is waiting for it to be ready, in either
 * direction; the poller only watches sockets something is waiting on. */
static inline int horizon_async_waiting_on( const struct horizon_async_list *list, unsigned int sock,
                                            int *read, int *write )
{
    struct horizon_async *async;

    *read = *write = 0;
    for (async = list->head; async; async = async->next)
    {
        if (async->sock != sock || async->state != HORIZON_ASYNC_QUEUED) continue;
        if (async->direction == HORIZON_ASYNC_READ) *read = 1;
        else *write = 1;
    }
    return *read || *write;
}

/* What finishing an async with this status has to do, which is
 * async_set_result's rule: nothing for an operation that failed synchronously,
 * since the program was told then; otherwise its completion routine if it gave
 * one, or else a packet on the port -- unless it finished directly on a socket
 * that asked to skip that -- and its event either way. */
static inline unsigned int horizon_async_completion( const struct horizon_async *async, int failed,
                                                     int has_port, int skip_on_success )
{
    unsigned int actions = 0;

    if (!async->pending && failed) return 0;
    if (async->data.apc) actions |= HORIZON_ASYNC_APC;
    else if (async->data.apc_context && has_port && (async->pending || !skip_on_success))
        actions |= HORIZON_ASYNC_POST;
    if (async->data.event) actions |= HORIZON_ASYNC_EVENT;
    return actions;
}

/* CancelIo, CancelIoEx and closing the socket: every operation on it that is
 * waiting, or a given one, or those one thread started, ends. They end through
 * the client like any other -- its callback lets go of its side and writes the
 * status block -- so they are made ready with how they ended. One being tried
 * or run now ends by itself.
 *
 * Cancelled, they end STATUS_CANCELLED. When the socket is closed, a recv or
 * send ends as server/async.c's free_async_queue ends it, STATUS_HANDLES_CLOSED,
 * and an accept as server/sock.c's sock_close_handle ends it, cancelled. */
static inline unsigned int horizon_async_cancel( struct horizon_async_list *list, unsigned int sock,
                                                 unsigned long long iosb, unsigned int tid,
                                                 unsigned long long now, int closing )
{
    struct horizon_async *async;
    unsigned int count = 0;

    for (async = list->head; async; async = async->next)
    {
        if (async->sock != sock && !(async->kind == HORIZON_ASYNC_ACCEPT_INTO && async->accept_into == sock))
            continue;
        if (iosb && async->data.iosb != iosb) continue;
        if (tid && async->owner_tid != tid) continue;
        if (async->state != HORIZON_ASYNC_QUEUED && async->state != HORIZON_ASYNC_READY) continue;
        horizon_async_ready( list, async, closing && async->kind == HORIZON_ASYNC_IO ?
                             HORIZON_ASYNC_STATUS_HANDLES_CLOSED : HORIZON_ASYNC_STATUS_CANCELLED, now );
        count++;
    }
    return count;
}

/* The buffer AcceptEx fills, as server/sock.c's fill_accept_output lays it out
 * for ws2_32 to read back: the first data received, then the local and the
 * remote address, each as an int with its length and the address after it. */
static inline int horizon_async_accept_layout( unsigned int out_size, unsigned int recv_len, unsigned int local_len,
                                               unsigned int *local_at, unsigned int *remote_at,
                                               unsigned int *remote_len )
{
    if (recv_len > out_size || local_len > out_size - recv_len) return 0;
    if (local_len && local_len < sizeof(int)) return 0;
    *local_at = recv_len;
    *remote_at = recv_len + local_len;
    *remote_len = out_size - recv_len - local_len;
    return *remote_len >= sizeof(int);
}

#endif /* WINE_NX_HORIZON_ASYNC_H */
