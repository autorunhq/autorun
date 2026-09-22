#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../source/fex_jit.h"

static void exercise(void)
{
    void *rx, *rw;
    assert(!wine_nx_fex_jit_create( 4096, &rx, &rw ));
    assert(rx && rw && rx != rw);
    assert(wine_nx_fex_jit_size( rx ) == 4096);
    assert(!wine_nx_fex_jit_size( (char *)rx + 1 ));
    assert(!wine_nx_fex_jit_size( rw ));
    assert(wine_nx_fex_jit_flush( rx, 4097 ) == EINVAL);
    assert(wine_nx_fex_jit_flush( (char *)rx + 1, SIZE_MAX ) == EINVAL);
    assert(wine_nx_fex_jit_close( (char *)rx + 1 ) == EINVAL);
#ifdef __aarch64__
    const uint32_t code[] = { 0x528000e0, 0xd65f03c0 }; /* mov w0, #7; ret */
    memcpy( (char *)rw + 16, code, sizeof(code) );
    assert(!wine_nx_fex_jit_flush( rx, 16 + sizeof(code) ));
    assert(((int (*)(void))((char *)rx + 16))() == 7);
    *(uint32_t *)((char *)rw + 16) = 0x52800540; /* mov w0, #42 */
    assert(!wine_nx_fex_jit_flush( (char *)rx + 16, 4 ));
    assert(((int (*)(void))((char *)rx + 16))() == 42);
#else
    memset( rw, 0xa5, 4096 );
    assert(!memcmp( rx, rw, 4096 ));
#endif
    assert(!wine_nx_fex_jit_close( rx ));
}

static void *worker( void *arg )
{
    (void)arg;
    for (unsigned i = 0; i < 200; i++) exercise();
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    void *rx[96], *rw, *extra;
    assert(wine_nx_fex_jit_create( 0, &extra, &rw ) == EINVAL);
    assert(wine_nx_fex_jit_create( SIZE_MAX, &extra, &rw ) == EINVAL);
    exercise();
    for (unsigned i = 0; i < 96; i++)
    {
        assert(!wine_nx_fex_jit_create( 4096, &rx[i], &rw ));
        memset(rw, i, 4096);
    }
    for (unsigned i = 0; i < 96; i++) assert(*(unsigned char *)rx[i] == i);
    for (unsigned i = 0; i < 96; i += 2) assert(!wine_nx_fex_jit_close(rx[i]));
    for (unsigned i = 0; i < 96; i += 2) assert(!wine_nx_fex_jit_create(4096, &rx[i], &rw));
    for (unsigned i = 1; i < 96; i += 2) assert(*(unsigned char *)rx[i] == i);
    assert(!wine_nx_fex_jit_release());
    for (unsigned i = 0; i < 96; i++) assert(!wine_nx_fex_jit_size( rx[i] ));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create( &threads[i], NULL, worker, NULL ));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join( threads[i], NULL ));
    assert(!wine_nx_fex_jit_release());
    puts("FEX JIT: split aliases, execution, patching, bounds, 96 live buffers and concurrent lifetime passed");
}
