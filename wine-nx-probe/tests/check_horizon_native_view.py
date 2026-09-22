#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/virtual.c').read_text()


def function(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
typedef uintptr_t ULONG_PTR;
#define VPROT_SYSTEM 0x200
#define SEC_RESERVE 0x4000000
#define MEM_TOP_DOWN 0x100000
struct file_view { void *base; size_t size; unsigned protect; } view;
static void *host_addr_space_limit = (void *)0x100000000ULL;
static pthread_mutex_t virtual_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned live, failed;
static void server_enter_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *set)
{ (void)set; assert(!pthread_mutex_lock(mutex)); }
static void server_leave_uninterrupted_section(pthread_mutex_t *mutex, sigset_t *set)
{ (void)set; assert(!pthread_mutex_unlock(mutex)); }
static unsigned map_view(struct file_view **ret, void *base, size_t size, unsigned type,
                         unsigned protect, uintptr_t low, uintptr_t high, size_t align)
{
    assert(pthread_mutex_trylock(&virtual_mutex) != 0);
    assert(!base && type == MEM_TOP_DOWN && protect == (VPROT_SYSTEM | SEC_RESERVE));
    assert(low == ((uintptr_t)host_addr_space_limit > 0x100000000ULL ? 0x100000000ULL : 0));
    assert(high == (uintptr_t)host_addr_space_limit - 1 && !align && !live);
    if (failed) return 1;
    *ret = &view;
    view = (struct file_view){(void *)(low ? low + 0xc8000000 : 0xc8000000), size, protect};
    live = 1;
    return 0;
}
static void delete_view(struct file_view *v)
{
    assert(pthread_mutex_trylock(&virtual_mutex) != 0);
    assert(live && v == &view && v->protect == SEC_RESERVE);
    live = 0;
}
'''
fixture += function('void *virtual_alloc_horizon_native(')
fixture += function('void virtual_free_horizon_native(')
fixture += r'''
int main(void)
{
    void *token;
    assert(virtual_alloc_horizon_native(0x4002000, &token) == (void *)0xc8000000);
    assert(live && token == &view && view.size == 0x4002000);
    virtual_free_horizon_native(token);
    assert(!live);
    host_addr_space_limit = (void *)0x8000000000ULL;
    assert(virtual_alloc_horizon_native(0x4002000, &token) == (void *)0x1c8000000ULL);
    virtual_free_horizon_native(token);
    assert(!live);
    failed = 1;
    assert(!virtual_alloc_horizon_native(0x4002000, &token) && !token && !live);
    assert(!pthread_mutex_trylock(&virtual_mutex));
    pthread_mutex_unlock(&virtual_mutex);
    puts("Horizon native views: Wine reservation, no-access system view, release and failure cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='horizon-native-view-') as directory:
    path = Path(directory)
    (path / 'view.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-pthread', str(path / 'view.c'),
                    '-o', str(path / 'view')], check=True)
    subprocess.run([str(path / 'view')], check=True)
