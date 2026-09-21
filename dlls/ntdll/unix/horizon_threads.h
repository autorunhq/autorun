/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#ifndef WINE_HORIZON_THREADS_H
#define WINE_HORIZON_THREADS_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Thread, mutex and keyed-event state for the in-process Horizon server and
 * the Switch ntdll. Callers hold the lock that protects the state; nothing here
 * performs I/O, so the host tests include this header directly. */

#define HORIZON_THREAD_STILL_ACTIVE          0x00000103u /* STATUS_PENDING */
#define HORIZON_THREAD_INFO_TERMINATED       0x02u       /* GET_THREAD_INFO_FLAG_TERMINATED */
#define HORIZON_THREAD_INFO_LAST             0x04u       /* GET_THREAD_INFO_FLAG_LAST */
#define HORIZON_THREAD_MAX_SUSPEND           127         /* MAXIMUM_SUSPEND_COUNT */
#define HORIZON_THREAD_ALL_ACCESS            0x001fffffu
#define HORIZON_THREADS_STATUS_ACCESS_DENIED     0xc0000022u
#define HORIZON_THREADS_STATUS_MUTANT_NOT_OWNED  0xc0000046u
#define HORIZON_THREADS_STATUS_SUSPEND_EXCEEDED  0xc000004au

/* Generic mapping and implied query/set rights from server/thread.c. */
static inline unsigned int horizon_thread_map_access( unsigned int access )
{
    if (access & 0x80000000u) access |= 0x00020048u;
    if (access & 0x40000000u) access |= 0x00020437u;
    if (access & 0x20000000u) access |= 0x00121800u;
    if (access & 0x12000000u) access |= HORIZON_THREAD_ALL_ACCESS;
    access &= ~0xf2000000u;
    if (access & 0x0040u) access |= 0x0800u;
    if (access & 0x0020u) access |= 0x0400u;
    return access;
}

struct horizon_thread_state
{
    unsigned int tid;
    unsigned int pid;
    unsigned long long teb;
    unsigned long long entry;
    unsigned long long affinity;
    int priority;
    int base_priority;
    int exit_code;
    int started;     /* init_thread released it into Windows code */
    int terminated;  /* its request pipe closed; no Windows code can still run */
    int suspend;
    long long creation_time;
    long long exit_time;
};

static inline void horizon_thread_init( struct horizon_thread_state *thread, unsigned int tid,
                                        unsigned int pid, unsigned long long affinity, long long now )
{
    memset( thread, 0, sizeof(*thread) );
    thread->tid = tid;
    thread->pid = pid;
    thread->affinity = affinity;
    thread->creation_time = now;
}

static inline int horizon_thread_exit_status( const struct horizon_thread_state *thread )
{
    return thread->terminated ? thread->exit_code : (int)HORIZON_THREAD_STILL_ACTIVE;
}

/* Like the Wine server, LAST describes the process, not the queried thread. */
static inline unsigned int horizon_thread_info_flags( const struct horizon_thread_state *thread,
                                                      unsigned int running )
{
    unsigned int flags = thread->terminated ? HORIZON_THREAD_INFO_TERMINATED : 0;
    if (running == 1) flags |= HORIZON_THREAD_INFO_LAST;
    return flags;
}

/* terminate_thread on the caller only records the code. The thread remains
 * running until its request pipe closes, so a join never observes a thread that
 * is still executing Windows code (DLL detach, TLS callbacks, stack frames). */
static inline void horizon_thread_set_exit_code( struct horizon_thread_state *thread, int code )
{
    if (!thread->terminated) thread->exit_code = code;
}

/* Returns 1 when this call ended the thread and the running count must drop. */
static inline int horizon_thread_mark_terminated( struct horizon_thread_state *thread, long long now )
{
    if (thread->terminated) return 0;
    thread->terminated = 1;
    thread->suspend = 0;
    thread->exit_time = now;
    return 1;
}

/* CREATE_SUSPENDED is honoured at the start gate, before any Windows code. */
static inline int horizon_thread_may_start( const struct horizon_thread_state *thread )
{
    return !thread->suspend || thread->terminated;
}

static inline unsigned int horizon_thread_resume( struct horizon_thread_state *thread, int *previous )
{
    *previous = thread->suspend;
    if (thread->suspend) thread->suspend--;
    return 0;
}

static inline unsigned int horizon_thread_suspend( struct horizon_thread_state *thread, int *previous )
{
    *previous = thread->suspend;
    if (thread->terminated) return HORIZON_THREADS_STATUS_ACCESS_DENIED;
    if (thread->suspend >= HORIZON_THREAD_MAX_SUSPEND) return HORIZON_THREADS_STATUS_SUSPEND_EXCEEDED;
    thread->suspend++;
    return 0;
}

struct horizon_mutex_state
{
    unsigned int owner;  /* owning tid, 0 when free */
    unsigned int count;  /* recursion depth */
    int abandoned;       /* owner exited while holding it; reported to the next owner */
};

static inline int horizon_mutex_signaled( const struct horizon_mutex_state *mutex, unsigned int tid )
{
    return !mutex->count || mutex->owner == tid;
}

/* Returns 1 when this acquisition inherits an abandoned mutex. */
static inline int horizon_mutex_acquire( struct horizon_mutex_state *mutex, unsigned int tid )
{
    int abandoned = mutex->abandoned;

    mutex->abandoned = 0;
    if (mutex->count && mutex->owner == tid) mutex->count++;
    else
    {
        mutex->owner = tid;
        mutex->count = 1;
    }
    return abandoned;
}

static inline unsigned int horizon_mutex_release( struct horizon_mutex_state *mutex, unsigned int tid,
                                                  unsigned int *previous )
{
    *previous = mutex->count;
    if (!mutex->count || mutex->owner != tid) return HORIZON_THREADS_STATUS_MUTANT_NOT_OWNED;
    if (!--mutex->count) mutex->owner = 0;
    return 0;
}

/* Called when thread tid exits. Returns 1 if it held this mutex. */
static inline int horizon_mutex_abandon( struct horizon_mutex_state *mutex, unsigned int tid )
{
    if (!mutex->count || mutex->owner != tid) return 0;
    mutex->owner = 0;
    mutex->count = 0;
    mutex->abandoned = 1;
    return 1;
}

/* Keyed events pair one wait with one release of the same key on the same
 * object; each side blocks until its peer arrives. The queue is FIFO. */
struct horizon_keyed_waiter
{
    struct horizon_keyed_waiter *next;
    unsigned long long object;
    unsigned long long key;
    int release;
    int done;  /* published by the peer, then woken */
};

static inline struct horizon_keyed_waiter *horizon_keyed_match_locked( struct horizon_keyed_waiter **queue,
                                                                       const struct horizon_keyed_waiter *self )
{
    struct horizon_keyed_waiter **ptr, *peer;

    for (ptr = queue; (peer = *ptr); ptr = &peer->next)
    {
        if (peer->release == self->release || peer->object != self->object || peer->key != self->key)
            continue;
        *ptr = peer->next;
        peer->next = NULL;
        return peer;
    }
    return NULL;
}

static inline void horizon_keyed_enqueue_locked( struct horizon_keyed_waiter **queue,
                                                 struct horizon_keyed_waiter *self )
{
    while (*queue) queue = &(*queue)->next;
    self->next = NULL;
    *queue = self;
}

/* Returns 1 if self was still queued (a timeout); 0 if a peer already took it. */
static inline int horizon_keyed_cancel_locked( struct horizon_keyed_waiter **queue,
                                               struct horizon_keyed_waiter *self )
{
    for (; *queue; queue = &(*queue)->next)
    {
        if (*queue != self) continue;
        *queue = self->next;
        self->next = NULL;
        return 1;
    }
    return 0;
}

/* Blocks until a peer operation arrives. timeout_ns < 0 waits forever. Returns
 * 0 after pairing, 1 on timeout. A peer publishes done and wakes under the
 * lock, so a failed cancel means the pairing already completed. */
static inline int horizon_keyed_rendezvous( pthread_mutex_t *lock, struct horizon_keyed_waiter **queue,
                                            struct horizon_keyed_waiter *self, long long timeout_ns,
                                            long long (*now_ns)(void),
                                            int (*wait)( const int *addr, int value, long long ns ),
                                            void (*wake)( const int *addr, int count ) )
{
    struct horizon_keyed_waiter *peer;
    long long end = 0;

    self->done = 0;
    pthread_mutex_lock( lock );
    if ((peer = horizon_keyed_match_locked( queue, self )))
    {
        __atomic_store_n( &peer->done, 1, __ATOMIC_RELEASE );
        wake( &peer->done, 1 );
        pthread_mutex_unlock( lock );
        return 0;
    }
    if (!timeout_ns)
    {
        pthread_mutex_unlock( lock );
        return 1;
    }
    horizon_keyed_enqueue_locked( queue, self );
    pthread_mutex_unlock( lock );

    if (timeout_ns > 0) end = now_ns() + timeout_ns;
    while (!__atomic_load_n( &self->done, __ATOMIC_ACQUIRE ))
    {
        long long left = -1;

        if (end && (left = end - now_ns()) <= 0)
        {
            int cancelled;

            pthread_mutex_lock( lock );
            cancelled = horizon_keyed_cancel_locked( queue, self );
            pthread_mutex_unlock( lock );
            if (cancelled) return 1;
            continue;
        }
        wait( &self->done, 0, left );
    }
    return 0;
}

/* libnx pthreads cannot be detached (pthread_detach returns ENOSYS), and the
 * kernel thread, stack and stack mapping persist until pthread_join. A thread
 * that nobody joins queues itself as its final action; later callers join it. */
struct horizon_zombie
{
    pthread_t thread;
    struct horizon_zombie *next;
};

struct horizon_zombie_list
{
    pthread_mutex_t lock;
    struct horizon_zombie *head;
    unsigned int pending;
    unsigned long long reaped;
};

static inline void horizon_zombie_push( struct horizon_zombie_list *list, struct horizon_zombie *node,
                                        pthread_t self )
{
    node->thread = self;
    pthread_mutex_lock( &list->lock );
    node->next = list->head;
    list->head = node;
    list->pending++;
    pthread_mutex_unlock( &list->lock );
}

/* Joins every queued thread except the caller; returns the number joined. */
static inline unsigned int horizon_zombie_reap( struct horizon_zombie_list *list )
{
    struct horizon_zombie *node, *next, *head, *keep = NULL;
    pthread_t self = pthread_self();
    unsigned int joined = 0;

    pthread_mutex_lock( &list->lock );
    head = list->head;
    list->head = NULL;
    pthread_mutex_unlock( &list->lock );

    for (node = head; node; node = next)
    {
        next = node->next;
        if (pthread_equal( node->thread, self ))
        {
            node->next = keep;
            keep = node;
            continue;
        }
        pthread_join( node->thread, NULL );
        free( node );
        joined++;
    }

    pthread_mutex_lock( &list->lock );
    while (keep)
    {
        next = keep->next;
        keep->next = list->head;
        list->head = keep;
        keep = next;
    }
    list->pending -= joined;
    list->reaped += joined;
    pthread_mutex_unlock( &list->lock );
    return joined;
}

#endif
