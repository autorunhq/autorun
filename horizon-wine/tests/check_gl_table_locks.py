#!/usr/bin/env python3
"""opengl32's object tables without their lock until the lists are shared.

A context's display lists are used by its one thread until another context
shares them, and the SRW lock around every name lookup cost Doom 3 8 to 10% of
its main thread (x86 ntdll, translated). dlls/opengl32/wgl.c marks the lists
busy instead and takes the lock once they are shared. The functions, as they
are in the source, with pthread locks and C atomics under them: one thread
uses a table while another shares the lists and then writes to it; the writer
must never be in the table with the first thread, under ThreadSanitizer and
by a count of who is inside."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
wgl = (root / 'dlls/opengl32/wgl.c').read_text()
code = wgl[wgl.index('static BOOL lock_object_table('):wgl.index('static GLuint *find_object_id(')]
acquire = re.search(r'static struct display_lists \*display_lists_acquire\(.*?\n}\n', wgl, re.S).group(0)
assert 'InterlockedExchange( &lists->shared, 1 );' in acquire
assert acquire.index('InterlockedExchange( &lists->shared, 1 );') < acquire.index('ReadAcquire( &lists->busy )')
# Every object table lock goes through the pair; the handle tables keep theirs.
tail = wgl[wgl.index('static struct object_table *get_object_table('):]
assert '&table->lock' not in tail
assert tail.count('LOCK_OBJECT_TABLE( table,') == 2 * tail.count('UNLOCK_OBJECT_TABLE( table,') == 20
for block in re.findall(r'[^N]LOCK_OBJECT_TABLE\( table, \w+ \)(.*?)UNLOCK_OBJECT_TABLE', tail, re.S):
    assert not re.search(r'\breturn\b|\bgoto\b', block), block
assert 'lists->tables[i].shared = &lists->shared;' in wgl and 'lists->tables[i].busy = &lists->busy;' in wgl

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int BOOL; typedef atomic_long LONG;
#define TRUE 1
#define FALSE 0
typedef pthread_rwlock_t SRWLOCK;
#define AcquireSRWLockExclusive(l) pthread_rwlock_wrlock(l)
#define ReleaseSRWLockExclusive(l) pthread_rwlock_unlock(l)
#define AcquireSRWLockShared(l) pthread_rwlock_rdlock(l)
#define ReleaseSRWLockShared(l) pthread_rwlock_unlock(l)
#define InterlockedExchange(p, v) atomic_exchange(p, v)
#define InterlockedIncrement(p) (atomic_fetch_add(p, 1) + 1)
#define ReadNoFence(p) atomic_load_explicit(p, memory_order_relaxed)
#define ReadAcquire(p) atomic_load(p)
#define NtYieldExecution() sched_yield()
struct object_table { SRWLOCK lock; LONG *shared, *busy; };
struct display_lists { LONG refcount, shared, busy; struct object_table tables[1]; };
'''
test = r'''
static struct display_lists lists = { .refcount = 1 };
static long value;           /* what the table holds: plain, for ThreadSanitizer */
static atomic_int writers, stop;
static long unlocked, locked;

static void *user(void *arg)
{
    struct object_table *table = &lists.tables[0];
    while (!atomic_load(&stop))
    {
        LOCK_OBJECT_TABLE( table, TRUE )
        if (table_locked) locked++; else unlocked++;
        assert(atomic_fetch_add(&writers, 1) == 0);
        value++;
        atomic_fetch_sub(&writers, 1);
        UNLOCK_OBJECT_TABLE( table, TRUE )
    }
    return arg;
}

int main(void)
{
    struct object_table *table = &lists.tables[0];
    pthread_t thread;
    long mine = 0;

    pthread_rwlock_init(&table->lock, NULL);
    table->shared = &lists.shared;
    table->busy = &lists.busy;
    assert(!pthread_create(&thread, NULL, user, NULL));
    for (int i = 0; i < 2000; i++) sched_yield();   /* the one context on its own */
    display_lists_acquire(&lists);                  /* a second context shares the lists */
    for (int i = 0; i < 200000; i++)
    {
        LOCK_OBJECT_TABLE( table, TRUE )
        assert(table_locked);
        assert(atomic_fetch_add(&writers, 1) == 0);
        value++; mine++;
        atomic_fetch_sub(&writers, 1);
        UNLOCK_OBJECT_TABLE( table, TRUE )
    }
    atomic_store(&stop, 1);
    pthread_join(thread, NULL);
    assert(unlocked > 0 && locked > 0 && value == unlocked + locked + mine && !atomic_load(&lists.busy));
    printf("%ld uses without the lock, then %ld with it beside %ld of a sharer\n", unlocked, locked, mine);
    return 0;
}
'''
cc = os.environ.get('WINE_NX_HOST_CC') or shutil.which('clang') or 'cc'
with tempfile.TemporaryDirectory(prefix='wine-nx-gl-locks-') as directory:
    path = pathlib.Path(directory)
    (path / 'test.c').write_text(fixture + code + acquire + test)
    for sanitizer in ('thread', 'address,undefined'):
        subprocess.run([cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Werror', '-pthread', '-fsanitize=' + sanitizer,
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        out = subprocess.run([str(path / 'test')], check=True, timeout=300, capture_output=True, text=True).stdout
print('opengl32 object tables: ' + out.strip() + '; no two in a table at once')

# get_handle_ptr: the current context is found on every object call, without
# the table's lock. One thread allocates and frees handles all the while; the
# other looks up one handle that stays live and handles it has seen freed.
handles = wgl[wgl.index('struct handle_entry\n{'):wgl.index('BOOL get_pbuffer_from_handle(')]
lookup = handles[handles.index('static struct handle_entry *get_handle_ptr('):]
assert 'SRWLock' not in lookup and 'ReadAcquire' in lookup
alloc = handles[handles.index('static struct handle_entry *alloc_handle('):handles.index('static void free_handle(')]
assert alloc.index('ptr->user_data = user_data;') < alloc.index('WriteRelease( (LONG *)&ptr->handle')

handle_fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
typedef int BOOL; typedef unsigned int UINT; typedef unsigned short WORD; typedef int32_t LONG;
typedef void *HANDLE; typedef uintptr_t UINT_PTR;
typedef pthread_rwlock_t SRWLOCK;
struct opengl_client_context; struct opengl_client_pbuffer;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LOWORD(l) ((WORD)((UINT)(l) & 0xffff))
#define HIWORD(l) ((WORD)((UINT)(l) >> 16))
#define MAKELONG(lo, hi) ((LONG)(((WORD)(lo)) | ((UINT)((WORD)(hi))) << 16))
#define ULongToHandle(u) ((HANDLE)(UINT_PTR)(u))
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define AcquireSRWLockExclusive(l) pthread_rwlock_wrlock(l)
#define ReleaseSRWLockExclusive(l) pthread_rwlock_unlock(l)
#define ReadAcquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define WriteRelease(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
static _Thread_local int last_error;
#define RtlSetLastWin32Error(e) (last_error = (e))
'''
handle_test = r'''
static struct handle_table table;
static atomic_int stop;
static int markers[4096];

static void *churn(void *arg)
{
    for (int round = 0; round < 20000; round++)
    {
        struct handle_entry *got[8];
        for (int i = 0; i < 8; i++) assert((got[i] = alloc_handle(&table, &markers[(round * 8 + i) % 4096])));
        for (int i = 0; i < 8; i++) free_handle(&table, got[i]);
    }
    atomic_store(&stop, 1);
    return arg;
}

int main(void)
{
    pthread_t thread;
    struct handle_entry *live, *gone;
    HANDLE live_handle, gone_handle;
    long found = 0, refused = 0;
    int mine;

    pthread_rwlock_init(&table.lock, NULL);
    assert((live = alloc_handle(&table, &mine)));
    live_handle = ULongToHandle(live->handle);
    assert((gone = alloc_handle(&table, &markers[0])));
    gone_handle = ULongToHandle(gone->handle);
    free_handle(&table, gone);
    assert(!get_handle_ptr(&table, gone_handle) && last_error == ERROR_INVALID_HANDLE);
    assert(!get_handle_ptr(&table, ULongToHandle(0)) && !get_handle_ptr(&table, ULongToHandle(0x7fff)));

    assert(!pthread_create(&thread, NULL, churn, NULL));
    while (!atomic_load(&stop))
    {
        struct handle_entry *ptr = get_handle_ptr(&table, live_handle);
        assert(ptr == live && ptr->user_data == &mine);
        found++;
        /* The freed handle's slot is taken again and again: never by it. */
        if (get_handle_ptr(&table, gone_handle)) assert(0);
        refused++;
    }
    pthread_join(thread, NULL);
    printf("%ld lookups of a live context and %ld of a freed one while 160000 came and went", found, refused);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-gl-handles-') as directory:
    path = pathlib.Path(directory)
    (path / 'test.c').write_text(handle_fixture + handles[:handles.index('static struct handle_table pbuffers;')] +
                                 alloc + handles[handles.index('static void free_handle('):] + handle_test)
    for sanitizer in ('thread', 'address,undefined'):
        subprocess.run([cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-pthread',
                        '-fsanitize=' + sanitizer, str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        out = subprocess.run([str(path / 'test')], check=True, timeout=300, capture_output=True, text=True).stdout
print('opengl32 context handles without the lock: ' + out.strip())
