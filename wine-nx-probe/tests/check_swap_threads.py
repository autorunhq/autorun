from pathlib import Path
import os
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
source = (probe / 'source/fex_exceptions.c').read_text().replace(
    '__asm__( "mrs %0, tpidrro_el0" : "=r"(tls) );', 'tls = (uintptr_t)pthread_self();')
tests = r'''
#include <assert.h>
#include <stdio.h>
static int fail_create;
int threadTlsAlloc(void (*destroy)(void *))
{
    pthread_key_t key;
    return pthread_key_create(&key, destroy) ? -1 : (int)key;
}
void *threadTlsGet(int key) { return pthread_getspecific(key); }
void threadTlsSet(int key, void *value) { assert(!pthread_setspecific(key, value)); }
Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack, size_t size, int priority, int core)
{
    (void)stack; (void)size; (void)priority; (void)core;
    if (fail_create) return 1;
    *t = (Thread){.entry = entry, .arg = arg};
    return 0;
}
Result __real_threadClose(Thread *t) { return t->started ? pthread_join(t->native, NULL) : 0; }
static void *run(void *arg)
{
    Thread *t = arg;
    t->entry(t->arg);
    return NULL;
}
static void entry(void *arg)
{
    struct fex_exception_slot *slot = get_slot();
    (void)arg;
    assert(slot && slot->tls == (uintptr_t)pthread_self());
    assert(slot->dump && slot->stack_top);
    assert(!wine_nx_fex_exception_attach() && get_slot() == slot);
    wine_nx_fex_exception_detach();
    assert(get_slot() == slot && slot->tls);
}
static void empty(void)
{
    unsigned int i;
    assert(!exception_threads);
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++)
        assert(!wine_nx_fex_exception_slots[i].tls && !wine_nx_fex_exception_slots[i].dump);
}
int main(void)
{
    Thread threads[FEX_EXCEPTION_SLOTS + 1];
    unsigned int i, n;
    fail_create = 1;
    assert(__wrap_threadCreate(&threads[0], entry, NULL, NULL, 16384, 0, 0));
    fail_create = 0;
    empty();
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++)
        assert(!__wrap_threadCreate(&threads[i], entry, NULL, NULL, 16384, 0, 0));
    assert(__wrap_threadCreate(&threads[i], entry, NULL, NULL, 16384, 0, 0));
    for (i = 0; i < FEX_EXCEPTION_SLOTS; i++) assert(!__wrap_threadClose(&threads[i]));
    empty();
    for (n = 0; n < 128; n++)
    {
        for (i = 0; i < 4; i++)
        {
            assert(!__wrap_threadCreate(&threads[i], entry, NULL, NULL, 16384, 0, 0));
            threads[i].started = 1;
            assert(!pthread_create(&threads[i].native, NULL, run, &threads[i]));
        }
        for (i = 0; i < 4; i++) assert(!__wrap_threadClose(&threads[i]));
        empty();
    }
    puts("paging thread contexts: creation failure, slot exhaustion, unstarted cleanup and 512 thread lifetimes passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-swap-threads-') as directory:
    unit = Path(directory) / 'threads.c'
    binary = Path(directory) / 'threads'
    unit.write_text(source + tests)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-pthread', '-DWINE_NX_SWAP_POC',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-I' + str(probe / 'tests/swap_threads'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
