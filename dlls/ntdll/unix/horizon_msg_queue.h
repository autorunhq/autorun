/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#ifndef WINE_HORIZON_MSG_QUEUE_H
#define WINE_HORIZON_MSG_QUEUE_H

#include <stdlib.h>
#include <string.h>

/* Per-thread message queues for the in-process Horizon server, following
 * server/queue.c: the wake and changed bits that GetQueueStatus and
 * MsgWaitForMultipleObjects see, WM_QUIT, and messages sent by other threads
 * (SendMessage, SendMessageTimeout, SendNotifyMessage, SendMessageCallback)
 * with the results their senders wait for. Posted messages, input, paints and
 * timers are kept elsewhere; the server reports whether any are pending with
 * horizon_msgq_update. Callers hold the server lock; nothing here performs
 * I/O, so the host tests include this header directly. */

#define HORIZON_MSGQ_QS_KEY            0x0001
#define HORIZON_MSGQ_QS_MOUSEMOVE      0x0002
#define HORIZON_MSGQ_QS_MOUSEBUTTON    0x0004
#define HORIZON_MSGQ_QS_POSTMESSAGE    0x0008
#define HORIZON_MSGQ_QS_TIMER          0x0010
#define HORIZON_MSGQ_QS_PAINT          0x0020
#define HORIZON_MSGQ_QS_SENDMESSAGE    0x0040
#define HORIZON_MSGQ_QS_HOTKEY         0x0080
#define HORIZON_MSGQ_QS_ALLPOSTMESSAGE 0x0100
#define HORIZON_MSGQ_QS_RAWINPUT       0x0400
#define HORIZON_MSGQ_QS_INPUT          (HORIZON_MSGQ_QS_KEY | HORIZON_MSGQ_QS_MOUSEMOVE | \
                                        HORIZON_MSGQ_QS_MOUSEBUTTON | HORIZON_MSGQ_QS_RAWINPUT)
#define HORIZON_MSGQ_QS_SMRESULT       0x8000

#define HORIZON_MSGQ_MSG_ASCII           0
#define HORIZON_MSGQ_MSG_UNICODE         1
#define HORIZON_MSGQ_MSG_NOTIFY          2
#define HORIZON_MSGQ_MSG_CALLBACK        3
#define HORIZON_MSGQ_MSG_CALLBACK_RESULT 4
#define HORIZON_MSGQ_MSG_OTHER_PROCESS   5

#define HORIZON_MSGQ_STATUS_SUCCESS           0x00000000u
#define HORIZON_MSGQ_STATUS_TIMEOUT           0x00000102u
#define HORIZON_MSGQ_STATUS_PENDING           0x00000103u
#define HORIZON_MSGQ_STATUS_INVALID_PARAMETER 0xc000000du
#define HORIZON_MSGQ_STATUS_NO_MEMORY         0xc0000017u
#define HORIZON_MSGQ_STATUS_ACCESS_DENIED     0xc0000022u

/* Offset of the result in struct callback_msg_data (callback, data, result). */
#define HORIZON_MSGQ_CALLBACK_RESULT_OFFSET 16

struct horizon_msgq;
struct horizon_msgq_result;

struct horizon_msgq_sent
{
    int type;
    unsigned int win;
    unsigned int msg;
    unsigned long long wparam;
    unsigned long long lparam;
    int x;
    int y;
    unsigned int time;
    unsigned char *data;
    unsigned int data_size;
    struct horizon_msgq_result *result;
    struct horizon_msgq_sent *next;
};

struct horizon_msgq_result
{
    struct horizon_msgq_result *sender_next;  /* in the sender's send or callback list */
    struct horizon_msgq_result *recv_next;    /* on the receiver's stack */
    struct horizon_msgq_sent *msg;            /* the message while it is not received */
    struct horizon_msgq *sender;              /* NULL once the sender stopped waiting */
    struct horizon_msgq *receiver;            /* NULL once the receiver is done with it */
    struct horizon_msgq_sent *callback_msg;   /* queued to the sender when replied */
    int replied;
    unsigned int error;
    unsigned long long result;
    unsigned char *data;
    unsigned int data_size;
    int has_deadline;
    unsigned long long deadline;
};

struct horizon_msgq
{
    unsigned int tid;
    unsigned int wake_bits;
    unsigned int changed_bits;
    unsigned int wake_mask;
    unsigned int changed_mask;
    int quit_message;
    int exit_code;
    struct horizon_msgq_sent *sent;
    struct horizon_msgq_sent **sent_tail;
    struct horizon_msgq_result *send_results;      /* the head is the one being waited for */
    struct horizon_msgq_result *callback_results;
    struct horizon_msgq_result *recv_result;
    unsigned long long shm_id;                     /* for the server */
    unsigned long long shm_offset;
    void *sync;
    int hook_chains[17];
    struct horizon_msgq *next;
};

struct horizon_msgqs
{
    struct horizon_msgq *head;
};

static inline struct horizon_msgq *horizon_msgq_find( struct horizon_msgqs *queues, unsigned int tid )
{
    struct horizon_msgq *queue;

    for (queue = queues->head; queue; queue = queue->next)
        if (queue->tid == tid) return queue;
    return NULL;
}

/* A newly created queue is returned with *created set. */
static inline struct horizon_msgq *horizon_msgq_get( struct horizon_msgqs *queues, unsigned int tid,
                                                     int *created )
{
    struct horizon_msgq *queue = horizon_msgq_find( queues, tid );

    if (created) *created = 0;
    if (queue || !tid) return queue;
    if (!(queue = calloc( 1, sizeof(*queue) ))) return NULL;
    queue->tid = tid;
    queue->sent_tail = &queue->sent;
    queue->next = queues->head;
    queues->head = queue;
    if (created) *created = 1;
    return queue;
}

/* Something new arrived (set_queue_bits): it counts as changed even if the
 * bit was already set. */
static inline void horizon_msgq_touch( struct horizon_msgq *queue, unsigned int bits )
{
    queue->wake_bits |= bits;
    queue->changed_bits |= bits;
}

/* The bits for what this header tracks itself. */
static inline unsigned int horizon_msgq_own_bits( const struct horizon_msgq *queue )
{
    unsigned int bits = 0;

    if (queue->sent) bits |= HORIZON_MSGQ_QS_SENDMESSAGE;
    if (queue->quit_message) bits |= HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_ALLPOSTMESSAGE;
    if (queue->send_results && queue->send_results->replied) bits |= HORIZON_MSGQ_QS_SMRESULT;
    return bits;
}

/* Set the wake bits from "external" (posted messages, input, paints, expired
 * timers) plus this header's own. Bits that appear also become changed; bits
 * with nothing left pending are cleared from both (clear_queue_bits), so a
 * wait for changes does not wake for a message already processed. */
static inline void horizon_msgq_update( struct horizon_msgq *queue, unsigned int external )
{
    unsigned int wake = external | horizon_msgq_own_bits( queue );

    queue->changed_bits &= ~(queue->wake_bits & ~wake);
    queue->changed_bits |= wake & ~queue->wake_bits;
    queue->wake_bits = wake;
}

static inline int horizon_msgq_signaled( const struct horizon_msgq *queue )
{
    return (queue->wake_bits & queue->wake_mask) || (queue->changed_bits & queue->changed_mask);
}

/* get_message clears the changed bits it is about to look at, so a later
 * wait only wakes for something new. */
static inline void horizon_msgq_begin_get( struct horizon_msgq *queue, unsigned int filter,
                                           unsigned int first, unsigned int last )
{
    if (filter & HORIZON_MSGQ_QS_POSTMESSAGE)
    {
        queue->changed_bits &= ~(HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_HOTKEY | HORIZON_MSGQ_QS_TIMER);
        if (first == 0 && last == ~0u) queue->changed_bits &= ~HORIZON_MSGQ_QS_ALLPOSTMESSAGE;
    }
    if (filter & HORIZON_MSGQ_QS_INPUT) queue->changed_bits &= ~HORIZON_MSGQ_QS_INPUT;
    if (filter & HORIZON_MSGQ_QS_PAINT) queue->changed_bits &= ~HORIZON_MSGQ_QS_PAINT;
}

static inline void horizon_msgq_post_quit( struct horizon_msgq *queue, int exit_code )
{
    queue->quit_message = 1;
    queue->exit_code = exit_code;
    horizon_msgq_touch( queue, HORIZON_MSGQ_QS_POSTMESSAGE | HORIZON_MSGQ_QS_ALLPOSTMESSAGE );
}

static inline void horizon_msgq_free_result( struct horizon_msgq_result *result )
{
    if (result->callback_msg)
    {
        free( result->callback_msg->data );
        free( result->callback_msg );
    }
    free( result->data );
    free( result );
}

static inline void horizon_msgq_unlink_result( struct horizon_msgq_result **list, struct horizon_msgq_result *result )
{
    for (; *list; list = &(*list)->sender_next)
    {
        if (*list != result) continue;
        *list = result->sender_next;
        result->sender_next = NULL;
        return;
    }
}

static inline void horizon_msgq_remove_result_from_sender( struct horizon_msgq_result *result )
{
    horizon_msgq_unlink_result( &result->sender->send_results, result );
    horizon_msgq_unlink_result( &result->sender->callback_results, result );
    result->sender = NULL;
    if (!result->receiver) horizon_msgq_free_result( result );
}

static inline void horizon_msgq_store_result( struct horizon_msgq_result *result, unsigned long long value,
                                              unsigned int error )
{
    struct horizon_msgq *sender = result->sender;

    result->result = value;
    result->error = error;
    result->replied = 1;
    result->has_deadline = 0;
    if (sender)
    {
        if (result->callback_msg)
        {
            struct horizon_msgq_sent *callback = result->callback_msg;

            if (callback->data_size >= HORIZON_MSGQ_CALLBACK_RESULT_OFFSET + sizeof(value))
                memcpy( callback->data + HORIZON_MSGQ_CALLBACK_RESULT_OFFSET, &value, sizeof(value) );
            *sender->sent_tail = callback;
            sender->sent_tail = &callback->next;
            horizon_msgq_touch( sender, HORIZON_MSGQ_QS_SENDMESSAGE );
            result->callback_msg = NULL;
            horizon_msgq_remove_result_from_sender( result );
        }
        else if (sender->send_results == result)
            horizon_msgq_touch( sender, HORIZON_MSGQ_QS_SMRESULT );
    }
    else if (!result->receiver) horizon_msgq_free_result( result );
}

/* free_message in server/queue.c: a sender still waiting learns the message is gone. */
static inline void horizon_msgq_free_sent( struct horizon_msgq_sent *msg )
{
    struct horizon_msgq_result *result = msg->result;

    if (result)
    {
        result->msg = NULL;
        result->receiver = NULL;
        horizon_msgq_store_result( result, 0, HORIZON_MSGQ_STATUS_ACCESS_DENIED );
    }
    free( msg->data );
    free( msg );
}

static inline void horizon_msgq_unlink_sent( struct horizon_msgq *queue, struct horizon_msgq_sent *msg )
{
    struct horizon_msgq_sent **link;

    for (link = &queue->sent; *link; link = &(*link)->next)
    {
        if (*link != msg) continue;
        *link = msg->next;
        if (queue->sent_tail == &msg->next) queue->sent_tail = link;
        msg->next = NULL;
        return;
    }
}

/* send_message for every type but MSG_POSTED. "fields" supplies type, win,
 * msg, wparam, lparam, x, y and time; the data is copied. A sender without a
 * queue (NULL) cannot wait for a result. */
static inline unsigned int horizon_msgq_send( struct horizon_msgq *sender, struct horizon_msgq *receiver,
                                              const struct horizon_msgq_sent *fields, const void *data,
                                              unsigned int data_size, int has_deadline,
                                              unsigned long long deadline )
{
    struct horizon_msgq_sent *msg;
    struct horizon_msgq_result *result = NULL;

    switch (fields->type)
    {
    case HORIZON_MSGQ_MSG_ASCII:
    case HORIZON_MSGQ_MSG_UNICODE:
    case HORIZON_MSGQ_MSG_NOTIFY:
    case HORIZON_MSGQ_MSG_CALLBACK:
    case HORIZON_MSGQ_MSG_OTHER_PROCESS:
        break;
    default:
        return HORIZON_MSGQ_STATUS_INVALID_PARAMETER;
    }
    if (!(msg = calloc( 1, sizeof(*msg) ))) return HORIZON_MSGQ_STATUS_NO_MEMORY;
    *msg = *fields;
    msg->data = NULL;
    msg->data_size = 0;
    msg->result = NULL;
    msg->next = NULL;
    if (data_size)
    {
        if (!(msg->data = malloc( data_size )))
        {
            free( msg );
            return HORIZON_MSGQ_STATUS_NO_MEMORY;
        }
        memcpy( msg->data, data, data_size );
        msg->data_size = data_size;
    }

    if (fields->type != HORIZON_MSGQ_MSG_NOTIFY)
    {
        if (!(result = calloc( 1, sizeof(*result) )))
        {
            free( msg->data );
            free( msg );
            return HORIZON_MSGQ_STATUS_NO_MEMORY;
        }
        result->msg = msg;
        result->sender = sender;
        result->receiver = receiver;
        result->has_deadline = has_deadline;
        result->deadline = deadline;
        if (fields->type == HORIZON_MSGQ_MSG_CALLBACK)
        {
            struct horizon_msgq_sent *callback = calloc( 1, sizeof(*callback) );

            if (!callback)
            {
                free( result );
                free( msg->data );
                free( msg );
                return HORIZON_MSGQ_STATUS_NO_MEMORY;
            }
            callback->type = HORIZON_MSGQ_MSG_CALLBACK_RESULT;
            callback->win = msg->win;
            callback->msg = msg->msg;
            callback->x = msg->x;
            callback->y = msg->y;
            callback->time = msg->time;
            /* The receiver does not need the callback address; the sender does. */
            callback->data = msg->data;
            callback->data_size = msg->data_size;
            msg->data = NULL;
            msg->data_size = 0;
            result->callback_msg = callback;
            if (sender)
            {
                result->sender_next = sender->callback_results;
                sender->callback_results = result;
            }
        }
        else if (sender)
        {
            result->sender_next = sender->send_results;
            sender->send_results = result;
        }
        msg->result = result;
    }

    *receiver->sent_tail = msg;
    receiver->sent_tail = &msg->next;
    horizon_msgq_touch( receiver, HORIZON_MSGQ_QS_SENDMESSAGE );
    return HORIZON_MSGQ_STATUS_SUCCESS;
}

/* receive_message: take the oldest sent message; a result moves onto the
 * receiver's stack for reply_message. The caller frees the returned message
 * with free() of its data and itself once the reply is written. */
static inline struct horizon_msgq_sent *horizon_msgq_receive( struct horizon_msgq *queue )
{
    struct horizon_msgq_sent *msg = queue->sent;
    struct horizon_msgq_result *result;

    if (!msg) return NULL;
    horizon_msgq_unlink_sent( queue, msg );
    if ((result = msg->result))
    {
        result->msg = NULL;
        result->recv_next = queue->recv_result;
        queue->recv_result = result;
        msg->result = NULL;
    }
    return msg;
}

/* reply_message: answer the message being processed; "remove" when its window
 * procedure returned (ReplyMessage answers early without removing). */
static inline unsigned int horizon_msgq_reply( struct horizon_msgq *queue, unsigned long long value,
                                               unsigned int error, int remove, const void *data,
                                               unsigned int data_size )
{
    struct horizon_msgq_result *result = queue->recv_result;

    if (!result) return HORIZON_MSGQ_STATUS_SUCCESS;
    if (remove)
    {
        queue->recv_result = result->recv_next;
        result->recv_next = NULL;
        result->receiver = NULL;
        if (!result->sender)
        {
            horizon_msgq_free_result( result );
            return HORIZON_MSGQ_STATUS_SUCCESS;
        }
    }
    if (!result->replied)
    {
        if (data_size && (result->data = malloc( data_size )))
        {
            memcpy( result->data, data, data_size );
            result->data_size = data_size;
        }
        horizon_msgq_store_result( result, value, error );
    }
    return HORIZON_MSGQ_STATUS_SUCCESS;
}

/* get_message_reply: the status is the result's error, or PENDING while the
 * awaited result has no reply and "cancel" is not set. Reply data moves to
 * *data for the caller to free. */
static inline unsigned int horizon_msgq_get_reply( struct horizon_msgq *queue, int cancel,
                                                   unsigned long long *value, unsigned char **data,
                                                   unsigned int *data_size )
{
    struct horizon_msgq_result *result = queue->send_results;
    unsigned int status = HORIZON_MSGQ_STATUS_PENDING;

    *value = 0;
    *data = NULL;
    *data_size = 0;
    if (!result || (!result->replied && !cancel)) return status;
    if (result->replied)
    {
        *value = result->result;
        status = result->error;
        *data = result->data;
        *data_size = result->data_size;
        result->data = NULL;
        result->data_size = 0;
    }
    horizon_msgq_remove_result_from_sender( result );
    if (queue->send_results && queue->send_results->replied)
        horizon_msgq_touch( queue, HORIZON_MSGQ_QS_SMRESULT );
    return status;
}

/* result_timeout: a sender stops waiting once the deadline passes. Returns 1
 * when something timed out, so the caller refreshes every queue. */
static inline int horizon_msgq_expire( struct horizon_msgqs *queues, unsigned long long now )
{
    struct horizon_msgq *queue;
    struct horizon_msgq_result *result;
    int expired = 0;

again:
    for (queue = queues->head; queue; queue = queue->next)
    {
        for (int list = 0; list < 2; list++)
        {
            for (result = list ? queue->callback_results : queue->send_results; result; result = result->sender_next)
            {
                if (result->replied || !result->has_deadline || result->deadline > now) continue;
                result->has_deadline = 0;
                if (result->msg && result->receiver)
                {
                    struct horizon_msgq_sent *msg = result->msg;

                    horizon_msgq_unlink_sent( result->receiver, msg );
                    msg->result = NULL;
                    free( msg->data );
                    free( msg );
                    result->msg = NULL;
                    result->receiver = NULL;
                }
                horizon_msgq_store_result( result, 0, HORIZON_MSGQ_STATUS_TIMEOUT );
                expired = 1;
                goto again;  /* storing may have unlinked or freed results */
            }
        }
    }
    return expired;
}

/* The thread is gone: its senders get ACCESS_DENIED, results it was waiting
 * for are abandoned, and the queue is freed. */
static inline void horizon_msgq_destroy( struct horizon_msgqs *queues, struct horizon_msgq *queue )
{
    struct horizon_msgq **link;
    struct horizon_msgq_sent *msg;

    while (queue->send_results) horizon_msgq_remove_result_from_sender( queue->send_results );
    while (queue->callback_results) horizon_msgq_remove_result_from_sender( queue->callback_results );
    while (queue->recv_result)
        horizon_msgq_reply( queue, 0, HORIZON_MSGQ_STATUS_ACCESS_DENIED, 1, NULL, 0 );
    while ((msg = queue->sent))
    {
        horizon_msgq_unlink_sent( queue, msg );
        horizon_msgq_free_sent( msg );
    }
    for (link = &queues->head; *link; link = &(*link)->next)
    {
        if (*link != queue) continue;
        *link = queue->next;
        break;
    }
    free( queue );
}

#endif
