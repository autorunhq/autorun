#!/usr/bin/env python3
"""Exercise thread destruction with a live communication pipe at descriptor zero."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


allocate = function('static struct horizon_server_object *horizon_server_alloc_thread_locked(')
release = function('static void horizon_server_free_object(')
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "horizon_threads.h"
struct horizon_server_request_header { int req; unsigned request_size, reply_size; };
struct horizon_server_reply_header { unsigned error, reply_size; };
#include "horizon_completion.h"
enum { HORIZON_SERVER_OBJECT_THREAD, HORIZON_SERVER_OBJECT_FILE, HORIZON_SERVER_OBJECT_TIMER };
struct horizon_user_apc { struct horizon_user_apc *next; };
struct horizon_server_object {
    unsigned id, refs;
    int type, file_fd, file_peer_fd;
    struct horizon_thread_state thread;
    struct horizon_server_object *thread_next, *wait_port, *file_completion, *mapping_shared_file;
    struct horizon_user_apc *apc_first, *apc_last;
    struct horizon_completion_queue completion;
    void *reg_key;
    char *file_name, *dir_mask, *name;
};
static struct horizon_server_object *horizon_server_threads;
static unsigned horizon_server_running_threads;
static struct { unsigned thread_objects; } horizon_lifecycle;
static int horizon_registry;
static void horizon_server_unlink_timer_locked(struct horizon_server_object *o) { (void)o; assert(0); }
static unsigned long long horizon_get_system_affinity_mask(void) { return 7; }
static long long horizon_server_now(void) { return 12345; }
static void horizon_reg_release(int *registry, void *key) { (void)registry; (void)key; assert(0); }
'''
tests = r'''
int main(void) {
    int pipefd[2];
    char byte;
    assert(!pipe(pipefd));
    if (pipefd[0] != 0) {
        assert(dup2(pipefd[0], 0) == 0);
        assert(!close(pipefd[0]));
    }
    for (unsigned i = 0; i < 128; ++i) {
        struct horizon_server_object *thread = horizon_server_alloc_thread_locked(4 + i * 4, 8);
        assert(thread && thread->file_fd == -1 && thread->thread.tid == 4 + i * 4);
        assert(horizon_server_threads == thread && horizon_lifecycle.thread_objects == 1);
        horizon_server_free_object(thread);
        assert(!horizon_server_threads && !horizon_lifecycle.thread_objects);
        if (fcntl(0, F_GETFD) == -1) {
            assert(errno == EBADF);
            assert(!close(pipefd[1]));
            puts("Reproduced: thread destruction closed another thread's descriptor");
            return 1;
        }
        assert(write(pipefd[1], "x", 1) == 1);
        assert(read(0, &byte, 1) == 1 && byte == 'x');
    }
    struct horizon_server_object *file = calloc(1, sizeof(*file));
    assert(file);
    file->type = HORIZON_SERVER_OBJECT_FILE;
    file->file_fd = 0;
    file->file_peer_fd = pipefd[1];
    horizon_server_free_object(file);
    assert(fcntl(0, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(pipefd[1], F_GETFD) == -1 && errno == EBADF);
    puts("Horizon thread destruction: unrelated descriptor survives; owned descriptors close");
}
'''
with tempfile.TemporaryDirectory(prefix='horizon-thread-fds-') as directory:
    build = Path(directory)
    for name, body, expected in (
        ('before', allocate.replace('    object->file_peer_fd = -1;\n', ''), 1),
        ('fixed', allocate, 0),
    ):
        path = build / (name + '.c')
        path.write_text(fixture + body + '\n' + release + tests)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(root / 'dlls/ntdll/unix'),
                        str(path), '-o', str(build / name)], check=True)
        result = subprocess.run([str(build / name)])
        assert result.returncode == expected, (name, result.returncode)
