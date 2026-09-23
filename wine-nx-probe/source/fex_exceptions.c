#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef WINE_NX_SWAP_POC
#include <switch.h>
#endif

#define FEX_EXCEPTION_SLOTS 256
#define FEX_EXCEPTION_STACK 0x10000

struct fex_exception_slot
{
    uintptr_t tls;
    void *dump;
    void *stack_top;
};

struct fex_exception_slot wine_nx_fex_exception_slots[FEX_EXCEPTION_SLOTS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef WINE_NX_SWAP_POC
static int slot_key = -1;
#define get_slot() threadTlsGet( slot_key )
static int set_slot( void *slot ) { threadTlsSet( slot_key, slot ); return 0; }
#else
static pthread_key_t slot_key;
#define get_slot() pthread_getspecific( slot_key )
static int set_slot( void *slot ) { return pthread_setspecific( slot_key, slot ); }
#endif
static pthread_once_t slot_once = PTHREAD_ONCE_INIT;
static int key_error;

static void detach( void *pointer )
{
    struct fex_exception_slot *slot = pointer;
    pthread_mutex_lock( &slots_lock );
    __atomic_store_n( &slot->tls, 0, __ATOMIC_RELEASE );
    free( slot->dump );
    slot->dump = slot->stack_top = NULL;
    pthread_mutex_unlock( &slots_lock );
}

static void make_key(void)
{
#ifdef WINE_NX_SWAP_POC
    if ((slot_key = threadTlsAlloc( detach )) < 0) key_error = ENOMEM;
#else
    key_error = pthread_key_create( &slot_key, detach );
#endif
}

int wine_nx_fex_exception_attach(void)
{
    uintptr_t tls;
    unsigned int i;
    int error = ENOMEM;

    pthread_once( &slot_once, make_key );
    if (key_error) return key_error;
    if (get_slot()) return 0;
    __asm__( "mrs %0, tpidrro_el0" : "=r"(tls) );
    pthread_mutex_lock( &slots_lock );
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++)
    {
        struct fex_exception_slot *slot = &wine_nx_fex_exception_slots[i];
        if (__atomic_load_n( &slot->tls, __ATOMIC_RELAXED )) continue;
        if (!(slot->dump = malloc( FEX_EXCEPTION_STACK + 0x400 ))) break;
        slot->stack_top = (char *)slot->dump + FEX_EXCEPTION_STACK + 0x400;
        if ((error = set_slot( slot )))
        {
            free( slot->dump );
            slot->dump = slot->stack_top = NULL;
            break;
        }
        __atomic_store_n( &slot->tls, tls, __ATOMIC_RELEASE );
        error = 0;
        break;
    }
    pthread_mutex_unlock( &slots_lock );
    return error;
}

void wine_nx_fex_exception_detach(void)
{
    /* The paging build keeps the context until libnx's thread TLS teardown. */
#ifndef WINE_NX_SWAP_POC
    void *slot;
    pthread_once( &slot_once, make_key );
    if (key_error || !(slot = get_slot())) return;
    set_slot( NULL );
    detach( slot );
#endif
}

_Static_assert( sizeof(struct fex_exception_slot) == 24, "exception slot ABI" );

#ifdef WINE_NX_SWAP_POC
struct exception_thread
{
    Thread *thread;
    ThreadFunc entry;
    void *arg;
    struct fex_exception_slot *slot;
    struct exception_thread *next;
    int entered;
};
static struct exception_thread *exception_threads;

static void exception_thread_entry( void *arg )
{
    struct exception_thread *start = arg;
    uintptr_t tls;
    start->entered = 1;
    set_slot( start->slot );
    __asm__( "mrs %0, tpidrro_el0" : "=r"(tls) );
    __atomic_store_n( &start->slot->tls, tls, __ATOMIC_RELEASE );
    start->entry( start->arg );
}

extern Result __real_threadCreate( Thread *, ThreadFunc, void *, void *, size_t, int, int );
extern Result __real_threadClose( Thread * );

Result __wrap_threadCreate( Thread *thread, ThreadFunc entry, void *arg, void *stack,
                            size_t size, int priority, int core )
{
    struct exception_thread *start;
    unsigned int i;
    Result rc;
    pthread_once( &slot_once, make_key );
    if (key_error || !(start = calloc( 1, sizeof(*start) )))
        return MAKERESULT( Module_Libnx, LibnxError_OutOfMemory );
    pthread_mutex_lock( &slots_lock );
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++)
    {
        struct fex_exception_slot *slot = &wine_nx_fex_exception_slots[i];
        if (__atomic_load_n( &slot->tls, __ATOMIC_RELAXED )) continue;
        if (!(slot->dump = malloc( FEX_EXCEPTION_STACK + 0x400 ))) break;
        slot->stack_top = (char *)slot->dump + FEX_EXCEPTION_STACK + 0x400;
        __atomic_store_n( &slot->tls, UINTPTR_MAX, __ATOMIC_RELEASE );
        start->slot = slot;
        break;
    }
    pthread_mutex_unlock( &slots_lock );
    if (!start->slot) { free( start ); return MAKERESULT( Module_Libnx, LibnxError_OutOfMemory ); }
    start->thread = thread;
    start->entry = entry;
    start->arg = arg;
    rc = __real_threadCreate( thread, exception_thread_entry, start, stack, size, priority, core );
    if (R_FAILED(rc)) { detach( start->slot ); free( start ); return rc; }
    pthread_mutex_lock( &slots_lock );
    start->next = exception_threads;
    exception_threads = start;
    pthread_mutex_unlock( &slots_lock );
    return rc;
}

Result __wrap_threadClose( Thread *thread )
{
    struct exception_thread **link, *start = NULL;
    Result rc = __real_threadClose( thread );
    if (R_FAILED(rc)) return rc;
    pthread_mutex_lock( &slots_lock );
    for (link = &exception_threads; *link; link = &(*link)->next)
        if ((*link)->thread == thread) { start = *link; *link = start->next; break; }
    pthread_mutex_unlock( &slots_lock );
    if (start)
    {
        if (!start->entered) detach( start->slot );
        free( start );
    }
    return rc;
}
#endif
