#!/usr/bin/env python3
"""Exercise Horizon AFD poll and event-select with both guest layouts."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


defines = '\n'.join(line for line in source.splitlines() if re.match(
    r'#define HORIZON_(STATUS_|AFD_POLL_|IMAGE_FILE_MACHINE_I386\b|IOCTL_AFD_EVENT_SELECT\b)', line))
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define horizon_trace(...) ((void)0)
''' + defines + r'''
static unsigned int horizon_process_machine;
static int sockets[2], poller_starts;
struct horizon_server_object {
    unsigned int sock_event_handle;
    int sock_event_mask, sock_pending_events, sock_connect_status, sock_nonblocking;
};
static struct horizon_server_object test_object;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int horizon_server_get_sock_fd(unsigned int handle, int *fd, void *unused)
{
    (void)unused;
    if (handle != 0x81000001 && handle != 0x81000002) return HORIZON_STATUS_INVALID_HANDLE;
    *fd = sockets[handle - 0x81000001];
    return 0;
}
static unsigned int horizon_server_find_sock_locked(unsigned int handle, struct horizon_server_object **object)
{
    if (handle != 0x81000001) return HORIZON_STATUS_INVALID_HANDLE;
    *object = &test_object;
    return 0;
}
static unsigned int horizon_sock_errno_status(int err) { return err ? HORIZON_STATUS_INVALID_PARAMETER : 0; }
static void horizon_sock_poller_start(void) { poller_starts++; }
'''
for marker in ('static void horizon_sock_poll_entry_read(', 'static void horizon_sock_poll_entry_write(',
               'static unsigned int horizon_sock_ioctl_poll('):
    fixture += block(marker)
fixture += r'''
static unsigned int event_select(const unsigned char *data, unsigned int data_size)
{
    unsigned int status = 0, handle = 0x81000001;
    switch (HORIZON_IOCTL_AFD_EVENT_SELECT) {
''' + block('case HORIZON_IOCTL_AFD_EVENT_SELECT:') + r'''
    }
    return status;
}
static void request(unsigned char *data, unsigned int stride, unsigned int count)
{
    memset(data, 0, 64);
    memcpy(data + 8, &count, 4);
    for (unsigned int i = 0; i < count; i++) {
        uint64_t socket = 0x81000001 + i;
        int flags = HORIZON_AFD_POLL_READ;
        memcpy(data + 16 + i * stride, &socket, stride - 8);
        memcpy(data + 16 + i * stride + stride - 8, &flags, 4);
    }
}
int main(void)
{
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    assert(write(sockets[0], "x", 1) == 1);
    for (unsigned int stride = 12; stride <= 16; stride += 4) {
        unsigned char input[65], output[66];
        unsigned char *data = input + 1, *out = output + 1;
        unsigned int size, count, bytes = 16 + 2 * stride;
        unsigned long long handle;
        int flags, status;
        horizon_process_machine = stride == 12 ? HORIZON_IMAGE_FILE_MACHINE_I386 : 0x8664;
        request(data, stride, 2);
        memset(output, 0xcc, sizeof(output));
        assert(!horizon_sock_ioctl_poll(0, data, bytes, out, bytes, &size));
        memcpy(&count, out + 8, 4);
        assert(count == 1 && size == 16 + stride);
        horizon_sock_poll_entry_read(&handle, &flags, &status, out + 16, stride);
        assert(handle == 0x81000002 && flags == HORIZON_AFD_POLL_READ && !status);
        for (unsigned int i = size + 1; i < sizeof(output); i++) assert(output[i] == 0xcc);
        assert(output[0] == 0xcc);
        assert(horizon_sock_ioctl_poll(0, data, bytes - 1, out, bytes, &size) == HORIZON_STATUS_INVALID_PARAMETER);
        assert(!size);
        assert(horizon_sock_ioctl_poll(0, data, bytes, out, bytes - 1, &size) == HORIZON_STATUS_BUFFER_TOO_SMALL);
        request(data, stride, 0);
        assert(horizon_sock_ioctl_poll(0, data, 16, out, bytes, &size) == HORIZON_STATUS_INVALID_PARAMETER);
        count = 65; memcpy(data + 8, &count, 4);
        assert(horizon_sock_ioctl_poll(0, data, 16, out, bytes, &size) == HORIZON_STATUS_INVALID_PARAMETER);
        request(data, stride, 1);
        assert(!horizon_sock_ioctl_poll(0, data, 16 + stride, out, 16 + stride, &size));
        memcpy(&count, out + 8, 4);
        assert(!count && size == 16);
        request(data, stride, 2);
        assert(!horizon_sock_ioctl_poll(0, data, bytes, data, bytes, &size));
        horizon_sock_poll_entry_read(&handle, &flags, &status, data + 16, stride);
        assert(handle == 0x81000002 && flags == HORIZON_AFD_POLL_READ);
        handle = stride == 12 ? 0xf1234567 : UINT64_C(0x12345678f1234567);
        horizon_sock_poll_entry_write(out, stride, handle, 0x1234, -3);
        unsigned long long decoded;
        horizon_sock_poll_entry_read(&decoded, &flags, &status, out, stride);
        assert(decoded == handle && flags == 0x1234 && status == -3);
        memset(data, 0, 64);
        memcpy(data, &handle, stride - 8);
        flags = HORIZON_AFD_POLL_READ | HORIZON_AFD_POLL_WRITE;
        memcpy(data + stride - 8, &flags, 4);
        int starts = poller_starts;
        assert(event_select(data, stride - 5) == HORIZON_STATUS_INVALID_PARAMETER);
        assert(poller_starts == starts);
        assert(!event_select(data, stride - 4));
        assert(test_object.sock_event_handle == (unsigned int)handle && test_object.sock_event_mask == flags);
        assert(test_object.sock_nonblocking && poller_starts == starts + 1);
        flags = 0; memcpy(data + stride - 8, &flags, 4);
        assert(!event_select(data, stride - 4) && !test_object.sock_event_mask);
        assert(poller_starts == starts + 1);
    }
    close(sockets[0]); close(sockets[1]);
    puts("AFD layouts: 32/64-bit polling, reply compaction, bounds and event selection passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'afd.c'
    exe = Path(tmp) / 'afd'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
