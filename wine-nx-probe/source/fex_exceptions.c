#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

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
static pthread_key_t slot_key;
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
    key_error = pthread_key_create( &slot_key, detach );
}

int wine_nx_fex_exception_attach(void)
{
    uintptr_t tls;
    unsigned int i;
    int error = ENOMEM;

    pthread_once( &slot_once, make_key );
    if (key_error) return key_error;
    if (pthread_getspecific( slot_key )) return 0;
    __asm__( "mrs %0, tpidrro_el0" : "=r"(tls) );
    pthread_mutex_lock( &slots_lock );
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++)
    {
        struct fex_exception_slot *slot = &wine_nx_fex_exception_slots[i];
        if (__atomic_load_n( &slot->tls, __ATOMIC_RELAXED )) continue;
        if (!(slot->dump = malloc( FEX_EXCEPTION_STACK + 0x400 ))) break;
        slot->stack_top = (char *)slot->dump + FEX_EXCEPTION_STACK + 0x400;
        if ((error = pthread_setspecific( slot_key, slot )))
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
    void *slot;
    pthread_once( &slot_once, make_key );
    if (key_error || !(slot = pthread_getspecific( slot_key ))) return;
    pthread_setspecific( slot_key, NULL );
    detach( slot );
}

_Static_assert( sizeof(struct fex_exception_slot) == 24, "exception slot ABI" );
