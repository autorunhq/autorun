#include "../source/swap_pager.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fixture
{
    struct swap_pager pager;
    uint64_t disk[32], ram[4], expected[32];
    int mapped[4];
    int fail_map, fail_unmap, fail_read, fail_write;
    pthread_mutex_t lock;
};

static int fail( int *count )
{
    if (!*count) return 0;
    --*count;
    errno = EIO;
    return -1;
}

static int map_page( void *context, unsigned int page, unsigned int slot )
{
    struct fixture *f = context;
    assert( f->mapped[slot] == -1 );
    if (fail( &f->fail_map )) return -1;
    f->mapped[slot] = page;
    return 0;
}

static int unmap_page( void *context, unsigned int page, unsigned int slot )
{
    struct fixture *f = context;
    assert( f->mapped[slot] == (int)page );
    if (fail( &f->fail_unmap )) return -1;
    f->mapped[slot] = -1;
    return 0;
}

static int read_page( void *context, unsigned int page, unsigned int slot )
{
    struct fixture *f = context;
    assert( f->mapped[slot] == -1 );
    if (fail( &f->fail_read )) return -1;
    f->ram[slot] = f->disk[page];
    return 0;
}

static int write_page( void *context, unsigned int page, unsigned int slot )
{
    struct fixture *f = context;
    assert( f->mapped[slot] == -1 );
    if (fail( &f->fail_write )) return -1;
    f->disk[page] = f->ram[slot];
    return 0;
}

static void zero_page( void *context, unsigned int slot )
{
    struct fixture *f = context;
    assert( f->mapped[slot] == -1 );
    f->ram[slot] = 0;
}

static void init( struct fixture *f, unsigned int slots )
{
    static const struct swap_pager_ops ops = { map_page, unmap_page, read_page, write_page, zero_page };
    unsigned int i;
    memset( f, 0, sizeof(*f) );
    for (i = 0; i < 4; i++) f->mapped[i] = -1;
    assert( !swap_pager_init( &f->pager, 32, slots, &ops, f ) );
    assert( !pthread_mutex_init( &f->lock, NULL ) );
}

static void close_fixture( struct fixture *f )
{
    assert( !swap_pager_close( &f->pager ) );
    assert( !pthread_mutex_destroy( &f->lock ) );
}

static void access_page( struct fixture *f, unsigned int page )
{
    int slot;
    assert( !swap_pager_fault( &f->pager, page ) );
    slot = f->pager.pages[page].slot;
    assert( slot >= 0 && slot < 4 && f->mapped[slot] == (int)page );
    assert( f->ram[slot] == f->expected[page] );
    f->ram[slot] = ++f->expected[page];
}

static void *stress_worker( void *context )
{
    struct fixture *f = context;
    uint32_t state = 0x1a72d0f;
    unsigned int i;
    for (i = 0; i < 10000; i++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        pthread_mutex_lock( &f->lock );
        access_page( f, state % 32 );
        pthread_mutex_unlock( &f->lock );
    }
    return NULL;
}

int main(void)
{
    struct fixture f;
    pthread_t threads[4];
    unsigned int i;
    int slot;

    init( &f, 4 );
    for (i = 0; i < 4; i++) assert( !pthread_create( &threads[i], NULL, stress_worker, &f ) );
    for (i = 0; i < 4; i++) assert( !pthread_join( threads[i], NULL ) );
    for (i = 0; i < 32; i++) access_page( &f, i );
    assert( f.pager.reads > 1000 && f.pager.writes > 1000 );
    close_fixture( &f );

    init( &f, 1 );
    assert( !swap_pager_pin( &f.pager, 0 ) );
    slot = f.pager.pages[0].slot;
    assert( swap_pager_fault( &f.pager, 1 ) == -1 && errno == EBUSY );
    assert( f.pager.pages[0].slot == slot );
    assert( swap_pager_close( &f.pager ) == -1 && errno == EBUSY );
    assert( !swap_pager_unpin( &f.pager, 0 ) );
    assert( swap_pager_unpin( &f.pager, 0 ) == -1 );
    assert( swap_pager_fault( &f.pager, 32 ) == -1 );
    f.ram[slot] = f.expected[0] = 0xcafebeef;
    f.fail_write = 1;
    assert( swap_pager_fault( &f.pager, 1 ) == -1 );
    assert( f.pager.pages[0].slot == slot && f.mapped[slot] == 0 );
    assert( f.ram[slot] == f.expected[0] );
    f.fail_unmap = 1;
    assert( swap_pager_fault( &f.pager, 1 ) == -1 );
    access_page( &f, 1 );
    f.fail_read = 1;
    assert( swap_pager_fault( &f.pager, 0 ) == -1 );
    assert( f.pager.owners[0] == -1 );
    f.fail_map = 1;
    assert( swap_pager_fault( &f.pager, 0 ) == -1 );
    assert( f.pager.owners[0] == -1 );
    access_page( &f, 0 );
    access_page( &f, 1 );
    f.fail_unmap = 1;
    assert( swap_pager_close( &f.pager ) == -1 );
    assert( f.pager.pages && f.pager.owners );
    close_fixture( &f );

    init( &f, 1 );
    access_page( &f, 0 );
    f.fail_write = f.fail_map = 1;
    assert( swap_pager_fault( &f.pager, 1 ) == -1 && f.pager.poisoned );
    assert( swap_pager_fault( &f.pager, 0 ) == -1 );
    /* Simulate recovery by the platform, never free a still-mapped frame. */
    f.mapped[0] = 0;
    f.pager.poisoned = 0;
    close_fixture( &f );
    puts( "swap pager: eviction, pinning, 40000 serialized accesses and I/O rollback passed" );
    return 0;
}
