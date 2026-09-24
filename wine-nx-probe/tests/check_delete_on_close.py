#!/usr/bin/env python3
"""Exercise the actual Horizon close handler with host files and a handle fixture,
and the handle table it goes through: the list and hash of
horizon_server_link_handle_locked, horizon_server_unlink_handle_locked and
horizon_server_find_handle_locked."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
a = source.index('static unsigned int horizon_server_close_object_handle(')
b = source.index('static unsigned int horizon_server_duplicate_object_handle', a)

def function(name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

hash_table = '\n'.join(re.findall(r'^(?:#define HORIZON_SERVER_HANDLE_HASH_SIZE .*|static struct horizon_server_handle_entry \*horizon_server_handle_hash\[.*;)$',
                                  source, re.M))
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define HORIZON_STATUS_SUCCESS 0
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_SERVER_OBJECT_FILE 1
#define HORIZON_SERVER_OBJECT_COMPLETION 2
#define HORIZON_SERVER_OBJECT_SOCK 3
/* Closing a socket ends what waits on it (check_horizon_async_sockets.py); no socket here. */
static struct { void *head; } horizon_asyncs;
static unsigned long long horizon_async_now(void) { return 0; }
static unsigned horizon_async_cancel(void *list, unsigned sock, unsigned long long iosb, unsigned tid,
                                     unsigned long long now, int closing) {
    (void)list; (void)sock; (void)iosb; (void)tid; (void)now; (void)closing; return 0; }
#define horizon_trace(...) ((void)0)
struct horizon_server_object { unsigned refs, type, file_options; char *file_name; int file_is_dir, file_fd, file_delete, completion_closed; void *reg_key; };
struct horizon_server_handle_entry { unsigned handle; struct horizon_server_object *object; struct horizon_server_handle_entry *next, **pprev, *hash_next; };
static struct horizon_server_handle_entry *horizon_server_handles;
''' + hash_table + r'''
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static int horizon_registry;
static void horizon_reg_handle_closed(int *reg, void *key, unsigned handle) { (void)reg; (void)key; (void)handle; }
static void horizon_sync_notify_async_locked(void) {}
static void horizon_sync_notify_object_locked(struct horizon_server_object *o, int satisfy) {
    (void)o; assert(!satisfy);
}
static unsigned horizon_server_errno_status(int e) { return 0xc0000000u | e; }
static void horizon_server_free_object(struct horizon_server_object *o) { if (o->file_fd != -1) close(o->file_fd); free(o->file_name); free(o); }
static int expected_closed_fd = -1;
static int checked_unlink(const char *path) {
    if (expected_closed_fd != -1) assert(fcntl(expected_closed_fd, F_GETFD) == -1 && errno == EBADF);
    return unlink(path);
}
#define unlink checked_unlink
'''
helpers = '\n'.join(function(name) for name in (
    'static struct horizon_server_handle_entry **horizon_server_handle_bucket(',
    'static struct horizon_server_handle_entry *horizon_server_find_handle_locked(',
    'static void horizon_server_link_handle_locked(',
    'static void horizon_server_unlink_handle_locked(',
    'static int horizon_server_object_has_handles_locked(')) + '\n'
setup = r'''
static void setup(const char *name, unsigned options, int directory, int duplicates)
{
    struct horizon_server_object *o = calloc(1, sizeof(*o));
    o->file_fd = open(name, O_RDONLY); expected_closed_fd = o->file_fd; o->refs = duplicates; o->type = HORIZON_SERVER_OBJECT_FILE;
    o->file_options = options; o->file_name = strdup(name); o->file_is_dir = directory;
    for (int i = 1; i <= duplicates; i++) {
        struct horizon_server_handle_entry *h = calloc(1, sizeof(*h));
        h->handle = i; h->object = o; horizon_server_link_handle_locked(h);
    }
}

/* Every entry is in the list once, in its bucket once, and the back links agree. */
static unsigned check_table(void)
{
    unsigned count = 0, bucketed = 0, i;
    struct horizon_server_handle_entry *e, **pprev = &horizon_server_handles;

    for (e = horizon_server_handles; e; pprev = &e->next, e = e->next, count++) {
        assert(e->pprev == pprev);
        assert(horizon_server_find_handle_locked(e->handle) == e);
    }
    for (i = 0; i < HORIZON_SERVER_HANDLE_HASH_SIZE; i++)
        for (e = horizon_server_handle_hash[i]; e; e = e->hash_next, bucketed++)
            assert((struct horizon_server_handle_entry **)&horizon_server_handle_hash[i] == horizon_server_handle_bucket(e->handle));
    assert(count == bucketed);
    return count;
}

/* Handles as the server allocates them (by 4, never reused), added and removed
 * in a random order, as a long session opens and closes them. */
static void stress_table(void)
{
    enum { SLOTS = 20000 };
    static struct horizon_server_handle_entry *live[SLOTS];
    static struct horizon_server_object object;
    unsigned next_handle = 0, rng = 1, live_count = 0, i;

    for (i = 0; i < 40 * SLOTS; i++) {
        unsigned slot;
        rng = rng * 1103515245 + 12345;
        slot = (rng >> 8) % SLOTS;
        if (live[slot]) {
            horizon_server_unlink_handle_locked(live[slot]);
            assert(!horizon_server_find_handle_locked(live[slot]->handle));
            free(live[slot]);
            live[slot] = NULL;
            live_count--;
        } else {
            live[slot] = calloc(1, sizeof(*live[slot]));
            live[slot]->handle = next_handle += 4;
            live[slot]->object = &object;
            horizon_server_link_handle_locked(live[slot]);
            live_count++;
        }
        if (i % 50000 == 0) assert(check_table() == live_count);
    }
    assert(check_table() == live_count);
    for (i = 0; i < SLOTS; i++) if (live[i]) { horizon_server_unlink_handle_locked(live[i]); free(live[i]); }
    assert(!horizon_server_handles && check_table() == 0);
}
'''
tests = r'''
int main(void)
{
    char path[] = "/tmp/wine-nx-delete-XXXXXX";
    stress_table();
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    setup(path, 0, 0, 1);
    assert(!horizon_server_close_object_handle(1) && access(path, F_OK) == 0);
    setup(path, 0x1000, 0, 2);
    assert(!horizon_server_close_object_handle(1) && access(path, F_OK) == 0);
    assert(!horizon_server_close_object_handle(2) && access(path, F_OK) == -1 && errno == ENOENT);
    assert(horizon_server_close_object_handle(2) == HORIZON_STATUS_INVALID_HANDLE);
    /* Trying file unlink on a directory must not silently succeed. */
    assert(mkdir(path, 0700) == 0);
    setup(path, 0x1000, 0, 1);
    assert(horizon_server_close_object_handle(1) != 0 && access(path, F_OK) == 0);
    setup(path, 0x1000, 1, 1);
    assert(!horizon_server_close_object_handle(1) && access(path, F_OK) == -1);
    assert(check_table() == 0);
    puts("Horizon close: handle table under random opens and closes, normal close, final duplicate deletion, error propagation and directory deletion passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-nx-delete-test-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text('#include <sys/stat.h>\n' + fixture + helpers + setup + source[a:b] + tests)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
