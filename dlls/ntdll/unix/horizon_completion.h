/*
 * I/O completion ports for the Horizon server, after server/completion.c.
 *
 * A port queues messages: NtSetIoCompletion adds them, and so does I/O on a
 * file associated with the port. NtRemoveIoCompletion takes them. A thread
 * that finds the queue empty waits on its own completion wait object, which
 * takes the next message for it once the port has one, or abandons the wait
 * once the port's last handle is closed. This header holds the queue, those
 * rules and the requests' wire layouts; horizon.c keeps objects and handles.
 * The includer defines struct horizon_server_request_header and
 * struct horizon_server_reply_header first.
 */

#ifndef __WINE_HORIZON_COMPLETION_H
#define __WINE_HORIZON_COMPLETION_H

#include <stdlib.h>

#define HORIZON_REQ_CREATE_COMPLETION      264
#define HORIZON_REQ_OPEN_COMPLETION        265
#define HORIZON_REQ_ADD_COMPLETION         266
#define HORIZON_REQ_REMOVE_COMPLETION      267
#define HORIZON_REQ_GET_THREAD_COMPLETION  268
#define HORIZON_REQ_QUERY_COMPLETION       269
#define HORIZON_REQ_SET_COMPLETION_INFO    270
#define HORIZON_REQ_ADD_FD_COMPLETION      271
#define HORIZON_REQ_SET_FD_COMPLETION_MODE 272

#define HORIZON_FILE_SYNCHRONOUS_IO_ALERT            0x10
#define HORIZON_FILE_SYNCHRONOUS_IO_NONALERT         0x20
#define HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define HORIZON_FILE_SKIP_SET_EVENT_ON_HANDLE        0x2
#define HORIZON_FILE_SKIP_SET_USER_EVENT_ON_FAST_IO  0x4

struct horizon_completion_msg
{
    struct horizon_completion_msg *next;
    unsigned long long ckey;
    unsigned long long cvalue;
    unsigned long long information;
    unsigned int status;
};

struct horizon_completion_queue
{
    struct horizon_completion_msg *head;
    struct horizon_completion_msg *tail;
    unsigned int depth;
};

/* Queues a message at the tail. Returns 0 when out of memory, which drops the
 * message as the server does. */
static inline int horizon_completion_add( struct horizon_completion_queue *queue, unsigned long long ckey,
                                          unsigned long long cvalue, unsigned int status,
                                          unsigned long long information )
{
    struct horizon_completion_msg *msg = malloc( sizeof(*msg) );

    if (!msg) return 0;
    msg->next = NULL;
    msg->ckey = ckey;
    msg->cvalue = cvalue;
    msg->information = information;
    msg->status = status;
    if (queue->tail) queue->tail->next = msg;
    else queue->head = msg;
    queue->tail = msg;
    queue->depth++;
    return 1;
}

/* Moves the message at the head into *msg. Returns 0 when the queue is empty. */
static inline int horizon_completion_take( struct horizon_completion_queue *queue,
                                           struct horizon_completion_msg *msg )
{
    struct horizon_completion_msg *head = queue->head;

    if (!head) return 0;
    if (!(queue->head = head->next)) queue->tail = NULL;
    queue->depth--;
    *msg = *head;
    msg->next = NULL;
    free( head );
    return 1;
}

static inline void horizon_completion_clear( struct horizon_completion_queue *queue )
{
    struct horizon_completion_msg msg;

    while (horizon_completion_take( queue, &msg )) ;
}

/* A completion wait is signaled once its port holds a message, or when it has
 * no port to wait on: none yet, or the port's last handle was closed. */
static inline int horizon_completion_wait_signaled( const struct horizon_completion_queue *port, int closed )
{
    return !port || closed || port->depth;
}

/* A signaled wait takes its message. Returns 1 when the wait is abandoned. */
static inline int horizon_completion_wait_satisfy( struct horizon_completion_queue *port, int closed,
                                                   struct horizon_completion_msg *msg, int *has_msg )
{
    if (!port || closed) return 1;
    *has_msg = horizon_completion_take( port, msg );
    return 0;
}

/* server/fd.c's is_fd_overlapped: only a handle opened for asynchronous I/O
 * can be associated with a port. */
static inline int horizon_completion_file_overlapped( unsigned int options )
{
    return !(options & (HORIZON_FILE_SYNCHRONOUS_IO_ALERT | HORIZON_FILE_SYNCHRONOUS_IO_NONALERT));
}

/* add_fd_completion: an I/O result reaches the file's port when it completed
 * asynchronously, or at once without FILE_SKIP_COMPLETION_PORT_ON_SUCCESS. */
static inline int horizon_completion_file_posts( int async, unsigned int flags )
{
    return async || !(flags & HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
}

/* set_fd_completion_mode: flags are added, never removed. */
static inline unsigned int horizon_completion_file_mode( unsigned int flags, unsigned int request )
{
    return flags | (request & (HORIZON_FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
                               HORIZON_FILE_SKIP_SET_EVENT_ON_HANDLE |
                               HORIZON_FILE_SKIP_SET_USER_EVENT_ON_FAST_IO));
}

/* Wire layouts of include/wine/server_protocol.h. open_completion has
 * open_event's layout, which horizon.c's named-object open reads. */
struct horizon_create_completion_request
{
    struct horizon_server_request_header header;
    unsigned int access;
    unsigned int concurrent;
    char __pad_20[4];
};

struct horizon_create_completion_reply
{
    struct horizon_server_reply_header header;
    unsigned int handle;
    char __pad_12[4];
};

struct horizon_add_completion_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long ckey;
    unsigned long long cvalue;
    unsigned long long information;
    unsigned int reserve_handle;
    unsigned int status;
};

struct horizon_remove_completion_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    int alertable;
    char __pad_20[4];
};

struct horizon_remove_completion_reply
{
    struct horizon_server_reply_header header;
    unsigned long long ckey;
    unsigned long long cvalue;
    unsigned long long information;
    unsigned int status;
    unsigned int wait_handle;
};

struct horizon_get_thread_completion_reply
{
    struct horizon_server_reply_header header;
    unsigned long long ckey;
    unsigned long long cvalue;
    unsigned long long information;
    unsigned int status;
    char __pad_36[4];
};

struct horizon_query_completion_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
};

struct horizon_query_completion_reply
{
    struct horizon_server_reply_header header;
    unsigned int depth;
    char __pad_12[4];
};

struct horizon_set_completion_info_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long ckey;
    unsigned int chandle;
    char __pad_28[4];
};

struct horizon_add_fd_completion_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned long long cvalue;
    unsigned long long information;
    unsigned int status;
    int async;
};

struct horizon_set_fd_completion_mode_request
{
    struct horizon_server_request_header header;
    unsigned int handle;
    unsigned int flags;
    char __pad_20[4];
};

#endif /* __WINE_HORIZON_COMPLETION_H */
