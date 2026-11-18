#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile

from check_horizon_async_sockets import definition, fixture, root, source, struct

fixture = fixture.replace('int main(', 'int socket_test_main(')
fixture = fixture.replace('int type, refs, signaled, file_fd;', '''int type, refs, signaled, file_fd;
    int file_peer_fd;
    unsigned int file_sharing, pipe_flags, rootdir, name_len;
    unsigned char *name;
    void *device_file;''')
fixture = fixture.replace('struct horizon_server_object *object; };',
                          'struct horizon_server_object *object; struct horizon_server_handle_entry *next; };')
fixture = fixture.replace('static unsigned int next_handle = 1;', '''static unsigned int next_handle = 1;
static struct horizon_server_handle_entry *horizon_server_handles;''')
fixture = fixture.replace('objects[handle].file_fd = -1;', '''objects[handle].file_fd = -1;
    objects[handle].file_peer_fd = -1;
    entries[handle].next = horizon_server_handles;
    horizon_server_handles = &entries[handle];''')
fixture = fixture.replace('return horizon_server_find_sock_locked( handle, object );', '''
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked(handle);
    *object = entry ? entry->object : NULL;
    return !entry ? HORIZON_STATUS_INVALID_HANDLE :
           (*object)->type == HORIZON_SERVER_OBJECT_SOCK || (*object)->type == HORIZON_SERVER_OBJECT_NAMED_PIPE ?
           0 : HORIZON_STATUS_OBJECT_TYPE_MISMATCH;''')
fixture = fixture.replace('static struct horizon_byte_pipe_end *horizon_byte_pipe_from_fd( int fd ) { (void)fd; return NULL; }', '''
static struct horizon_byte_pipe_end native_ends[64];
static unsigned int next_fd = 1000;
static struct horizon_byte_pipe_end *horizon_byte_pipe_from_fd(int fd)
{
    return fd >= 1000 && fd < 1064 && native_ends[fd - 1000].pipe ? &native_ends[fd - 1000] : NULL;
}''')
fixture = fixture.replace('static int horizon_poll_one( struct pollfd *fd, int timeout ) { return poll( fd, 1, timeout ); }', '''
static int horizon_poll_one(struct pollfd *fd, int timeout)
{
    struct horizon_byte_pipe_end *end = horizon_byte_pipe_from_fd(fd->fd);
    if (!end) return poll(fd, 1, timeout);
    assert(!timeout);
    pthread_mutex_lock(&end->pipe->mutex);
    fd->revents = horizon_byte_pipe_ready_locked(end, fd->events);
    pthread_mutex_unlock(&end->pipe->mutex);
    return fd->revents != 0;
}''')
fixture = fixture.replace('assert( size <= sizeof(last_reply) && !data_size );', '''
    extern unsigned char pipe_output[1056];
    extern unsigned int pipe_output_size;
    assert(size <= sizeof(last_reply) && data_size <= 1056);
    if (data_size) memcpy(pipe_output, data, data_size);
    pipe_output_size = data_size;''')

extra = r'''
#define min(a,b) ((a) < (b) ? (a) : (b))
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define HORIZON_NAMED_PIPE_MESSAGE_STREAM_WRITE 1
#define HORIZON_NAMED_PIPE_MESSAGE_STREAM_READ 2
#define HORIZON_NAMED_PIPE_NONBLOCKING_MODE 4
#define HORIZON_NAMED_PIPE_SERVER_END 0x8000
#define HORIZON_SERVER_OBJECT_NSI 99
#define HORIZON_SERVER_OBJECT_DEVICE 100
static void *(*carafeHidOpen)(const void *, unsigned int);
static void (*carafeHidClose)(void *);
unsigned char pipe_output[1056];
unsigned int pipe_output_size;

@STRUCTS@
@FILEACCESS@

static int byte_close(int fd)
{
    struct horizon_byte_pipe_end *end = horizon_byte_pipe_from_fd(fd);
    if (!end) return close(fd);
    horizon_byte_pipe_close(end);
    end->pipe = NULL;
    return 0;
}

static int horizon_byte_pipe_pair(int fd[2], unsigned int in, unsigned int out)
{
    struct horizon_byte_pipe *pipe = horizon_byte_pipe_alloc(in, out);
    assert(next_fd < 1063);
    if (!pipe) { errno = ENOMEM; return -1; }
    fd[0] = next_fd++;
    fd[1] = next_fd++;
    native_ends[fd[0] - 1000] = (struct horizon_byte_pipe_end){pipe, 0};
    native_ends[fd[1] - 1000] = (struct horizon_byte_pipe_end){pipe, 1};
    return 0;
}

static unsigned int horizon_server_errno_status(int error)
{ return error == ENOMEM ? HORIZON_STATUS_NO_MEMORY : HORIZON_STATUS_UNSUCCESSFUL; }
static unsigned int horizon_server_name_root_locked(unsigned int rootdir) { return rootdir; }
static unsigned int horizon_server_find_typed_object_locked(unsigned int handle, int type, struct horizon_server_object **object)
{
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked(handle);
    *object = entry ? entry->object : NULL;
    return !entry ? HORIZON_STATUS_INVALID_HANDLE : (*object)->type != type ? HORIZON_STATUS_OBJECT_TYPE_MISMATCH : 0;
}
static void horizon_server_unlink_handle_locked(struct horizon_server_handle_entry *entry) { (void)entry; }
static int horizon_server_write_status(int fd, unsigned int status)
{
    struct horizon_server_reply_header reply = {status, 0};
    return horizon_server_write_reply(fd, &reply, sizeof(reply), NULL, 0);
}
static int horizon_nsi_device_name(const unsigned char *name, unsigned int size) { (void)name; (void)size; return 0; }

#define close byte_close
@FUNCTIONS@
#undef close

static unsigned int pipe_create_instances(const char *name, unsigned int sharing, unsigned int options,
                                          unsigned int flags, unsigned int instances)
{
    struct horizon_create_named_pipe_request request = {0};
    struct horizon_object_attributes attr = {0};
    unsigned char data[256];
    struct horizon_server_connection owner = {4, 0};
    const struct horizon_create_named_pipe_reply *reply = (void *)last_reply;
    size_t i;
    attr.name_len = strlen(name) * 2;
    memcpy(data, &attr, sizeof(attr));
    for (i = 0; i < strlen(name); ++i) { data[sizeof(attr) + 2 * i] = name[i]; data[sizeof(attr) + 2 * i + 1] = 0; }
    request.access = sharing == 3 ? 0xc0000000u : sharing == 2 ? 0x80000000u : 0x40000000u;
    request.sharing = sharing;
    request.options = options;
    request.disposition = FILE_OPEN_IF;
    request.maxinstances = instances;
    request.insize = request.outsize = 4096;
    request.flags = flags;
    assert(!horizon_server_handle_create_named_pipe(&owner, (void *)&request, data, sizeof(attr) + attr.name_len));
    return reply->handle;
}

static unsigned int pipe_create(const char *name, unsigned int sharing, unsigned int options, unsigned int flags)
{
    return pipe_create_instances(name, sharing, options, flags, 1);
}

static unsigned int pipe_open(const char *name, unsigned int access)
{
    struct horizon_open_file_object_request request = {0};
    struct horizon_server_connection owner = {4, 0};
    const struct horizon_open_file_object_reply *reply = (void *)last_reply;
    unsigned char data[256];
    size_t i;
    for (i = 0; i < strlen(name); ++i) { data[2*i] = name[i]; data[2*i+1] = 0; }
    request.access = access;
    assert(!horizon_server_handle_open_file_object(&owner, (void *)&request, data, 2 * strlen(name)));
    return reply->handle;
}

static unsigned int pipe_read_mode(unsigned int handle, unsigned int flags)
{
    struct horizon_set_named_pipe_info_request request = {0};
    struct horizon_server_connection owner = {4, 0};
    request.handle = handle;
    request.flags = flags;
    assert(!horizon_server_handle_set_named_pipe_info(&owner, (void *)&request));
    return u32(last_reply);
}

static void drop_pipes(void)
{
    unsigned int i;
    for (i = 1; i < next_handle; i++)
    {
        struct horizon_server_object *o = entries[i].object;
        if (!o || o->type != HORIZON_SERVER_OBJECT_NAMED_PIPE) continue;
        byte_close(o->file_fd);
        if (o->file_peer_fd != -1) byte_close(o->file_peer_fd);
        free(o->name);
    }
}

int main(void)
{
    struct horizon_server_connection owner = {4, 0};
    struct horizon_server_object thread = {0};
    struct horizon_register_async_request request = {0};
    struct horizon_ioctl_request ioctl = {0};
    struct horizon_server_object *port, *event;
    struct horizon_server_handle_entry alias = {0}, **link;
    struct horizon_byte_pipe_end *a, *b;
    unsigned int server, client, server2, client2, port_h, event_h, apc, i;
    int more;
    unsigned char call[64], data[8192], result[8192];
    struct pollfd pfd;

    thread.thread.tid = owner.tid;
    horizon_server_threads = &thread;
    server = pipe_create("\\??\\pipe\\Mojo.Test", 3, 0, 0);
    assert(server && !u32(last_reply));
    assert(!pipe_create("\\??\\pipe\\mojo.test", 3, 0, 0));
    assert(u32(last_reply) == HORIZON_STATUS_INSTANCE_NOT_AVAILABLE);
    client = pipe_open("\\Device\\NamedPipe\\mojo.test", 0xc0000000);
    assert(client && !u32(last_reply));
    assert(!pipe_open("\\??\\pipe\\mojo.test", 0xc0000000));
    assert(u32(last_reply) == HORIZON_STATUS_PIPE_NOT_AVAILABLE);
    a = horizon_byte_pipe_from_fd(entries[server].object->file_fd);
    b = horizon_byte_pipe_from_fd(entries[client].object->file_fd);
    assert(a->pipe == b->pipe && a->side == 0 && b->side == 1);

    ioctl.async.handle = server;
    ioctl.code = 0x110008;
    assert(!horizon_server_handle_pipe_ioctl(&owner, &ioctl));
    assert(u32(last_reply) == HORIZON_STATUS_PIPE_CONNECTED && !horizon_asyncs.head);
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == -1 && errno == EAGAIN);
    assert(horizon_byte_pipe_write(b, "abc", 3) == 3);
    ioctl.code = 0x11400c;
    ioctl.header.reply_size = 32;
    assert(!horizon_server_handle_pipe_ioctl(&owner, &ioctl));
    assert(!u32(last_reply) && pipe_output_size == 19 && u32(pipe_output + 4) == 3);
    assert(!memcmp(pipe_output + 16, "abc", 3));
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == 3 && !memcmp(result, "abc", 3));

    for (i = 0; i < sizeof(data); ++i) data[i] = i;
    assert(horizon_byte_pipe_write(a, data, sizeof(data)) == 4096);
    assert(horizon_byte_pipe_write(a, data, 1) == -1 && errno == EAGAIN);
    assert(horizon_byte_pipe_read(b, result, 3071) == 3071 && !memcmp(result, data, 3071));
    assert(horizon_byte_pipe_write(a, data + 4096, 3071) == 3071);
    assert(horizon_byte_pipe_read(b, result, sizeof(result)) == 4096);
    assert(!memcmp(result, data + 3071, 4096));

    port_h = horizon_server_create_handle_locked(HORIZON_SERVER_OBJECT_COMPLETION)->handle;
    event_h = horizon_server_create_handle_locked(HORIZON_SERVER_OBJECT_EVENT)->handle;
    port = entries[port_h].object;
    event = entries[event_h].object;
    entries[server].object->file_completion = port;
    entries[server].object->file_completion_key = 123;
    request.async.handle = server;
    request.async.event = event_h;
    request.async.user = 0x1234;
    request.async.apc_context = 456;
    request.type = 1;
    request.count = 64;
    event->signaled = 1;
    assert(!horizon_server_handle_register_async(&owner, (void *)&request));
    assert(u32(last_reply) == HORIZON_STATUS_PENDING && !event->signaled && port->refs == 2);
    assert(!horizon_sock_poll_all_asyncs_locked());
    assert(horizon_byte_pipe_write(b, "hello", 5) == 5);
    assert(horizon_sock_poll_all_asyncs_locked());
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && u32(call + 4) == HORIZON_STATUS_ALERTED);
    assert(horizon_byte_pipe_read(a, result, 64) == 5);
    send_result(apc, HORIZON_STATUS_SUCCESS, 5);
    assert(!horizon_asyncs.head && event->signaled && port->refs == 1 && nposts == 1);
    assert(posts[0].key == 123 && posts[0].value == 456 && posts[0].info == 5);

    assert(horizon_byte_pipe_write(a, data, 4096) == 4096);
    request.type = 2;
    assert(!horizon_server_handle_register_async(&owner, (void *)&request));
    assert(!horizon_sock_poll_all_asyncs_locked());
    assert(horizon_byte_pipe_read(b, result, 4096) == 4096);
    assert(horizon_sock_poll_all_asyncs_locked());
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && horizon_byte_pipe_write(a, "x", 1) == 1);
    send_result(apc, HORIZON_STATUS_SUCCESS, 1);
    assert(nposts == 2 && !horizon_asyncs.head);

    request.type = 1;
    assert(!horizon_server_handle_register_async(&owner, (void *)&request));
    assert(horizon_async_cancel(&horizon_asyncs, server, 0, 0, fake_now, 1));
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && u32(call + 4) == HORIZON_ASYNC_STATUS_HANDLES_CLOSED);
    send_result(apc, HORIZON_ASYNC_STATUS_HANDLES_CLOSED, 0);
    assert(nposts == 3 && !horizon_asyncs.head && event->signaled);

    assert(horizon_byte_pipe_write(b, "end", 3) == 3);
    byte_close(entries[client].object->file_fd);
    entries[client].object->file_fd = -1;
    pfd = (struct pollfd){entries[server].object->file_fd, POLLIN, 0};
    assert(horizon_poll_one(&pfd, 0) && (pfd.revents & POLLHUP));
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == 3 && !memcmp(result, "end", 3));
    assert(horizon_byte_pipe_read(a, result, 1) == -1 && errno == EPIPE);
    assert(horizon_byte_pipe_write(a, data, 1) == -1 && errno == EPIPE);

    server = pipe_create("\\??\\pipe\\listen.test", 3, 0, 0);
    ioctl.code = 0x110008;
    ioctl.async.handle = server;
    ioctl.async.user = 0x5678;
    ioctl.async.event = event_h;
    assert(!horizon_server_handle_pipe_ioctl(&owner, &ioctl));
    assert(u32(last_reply) == HORIZON_STATUS_PENDING);
    assert(!horizon_sock_poll_all_asyncs_locked());
    client = pipe_open("\\??\\pipe\\listen.test", 0xc0000000);
    assert(client && horizon_sock_poll_all_asyncs_locked());
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && u32(call + 4) == HORIZON_STATUS_SUCCESS);
    send_result(apc, HORIZON_STATUS_SUCCESS, 0);
    assert(!horizon_asyncs.head && event->signaled);

    assert(!pipe_create("\\??\\pipe\\invalid.mode", 3, 0, 2));
    assert(u32(last_reply) == HORIZON_STATUS_INVALID_PARAMETER);
    server = pipe_create("\\??\\pipe\\inbound", 2, 0x20, 0);
    assert(server && !pipe_open("\\??\\pipe\\inbound", 0x80000000));
    assert(u32(last_reply) == HORIZON_STATUS_ACCESS_DENIED);
    assert(pipe_open("\\??\\pipe\\inbound", 0x40000000));

    server = pipe_create_instances("\\??\\pipe\\crashpad", 3, 0x20, 3, 2);
    assert(server && !u32(last_reply));
    assert(u32(last_reply + 12) == 1);
    assert(!pipe_create_instances("\\??\\pipe\\crashpad", 3, 0x20, 3, 3));
    assert(u32(last_reply) == HORIZON_STATUS_ACCESS_DENIED);
    alias.object = entries[server].object;
    alias.next = horizon_server_handles;
    horizon_server_handles = &alias;
    server2 = pipe_create_instances("\\Device\\NamedPipe\\CRASHPAD", 3, 0, 1, 2);
    assert(server2 && !u32(last_reply) && !u32(last_reply + 12));
    for (link = &horizon_server_handles; *link != &alias; link = &(*link)->next) {}
    *link = alias.next;
    assert(!pipe_create_instances("\\??\\pipe\\crashpad", 3, 0, 3, 2));
    assert(u32(last_reply) == HORIZON_STATUS_INSTANCE_NOT_AVAILABLE);
    client = pipe_open("\\??\\pipe\\crashpad", 0xc0000000);
    client2 = pipe_open("\\??\\pipe\\crashpad", 0xc0000000);
    assert(client && client2 && client != client2);
    assert(!pipe_open("\\??\\pipe\\crashpad", 0xc0000000));
    assert(u32(last_reply) == HORIZON_STATUS_PIPE_NOT_AVAILABLE);
    a = horizon_byte_pipe_from_fd(entries[server].object->file_fd);
    b = horizon_byte_pipe_from_fd(entries[client2].object->file_fd);
    assert(a->pipe == b->pipe && !a->pipe->read_mode[1]);
    assert(horizon_byte_pipe_from_fd(entries[server2].object->file_fd)->pipe != a->pipe);
    assert(horizon_byte_pipe_write(b, "hello", 5) == 5);
    assert(horizon_byte_pipe_write(b, "next", 4) == 4);
    assert(horizon_byte_pipe_read_ex(a, result, 2, &more) == 2 && more && !memcmp(result, "he", 2));
    ioctl.async.handle = server;
    ioctl.code = 0x11400c;
    ioctl.header.reply_size = 18;
    assert(!horizon_server_handle_pipe_ioctl(&owner, &ioctl));
    assert(u32(last_reply) == HORIZON_STATUS_BUFFER_OVERFLOW && pipe_output_size == 18);
    assert(u32(pipe_output + 4) == 7 && u32(pipe_output + 8) == 2 && u32(pipe_output + 12) == 3);
    assert(!memcmp(pipe_output + 16, "ll", 2));
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == 3 && !more);
    assert(!memcmp(result, "llo", 3));
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == 4 && !more);
    assert(!memcmp(result, "next", 4));
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == -1 && errno == EAGAIN);
    assert(horizon_byte_pipe_write(a, "one", 3) == 3);
    assert(horizon_byte_pipe_write(a, "two", 3) == 3);
    assert(horizon_byte_pipe_read_ex(b, result, sizeof(result), &more) == 6 && !more);
    assert(!memcmp(result, "onetwo", 6));
    assert(!pipe_read_mode(client2, 2));
    assert(horizon_byte_pipe_write(a, "message", 7) == 7);
    assert(horizon_byte_pipe_read_ex(b, result, 3, &more) == 3 && more);
    assert(horizon_byte_pipe_read_ex(b, result, sizeof(result), &more) == 4 && !more);
    assert(!pipe_read_mode(server, 0));
    assert(a->pipe->message_mode && !a->pipe->read_mode[0]);
    assert(!pipe_read_mode(server, 2));
    assert(horizon_byte_pipe_write(b, "", 0) == 0);
    pfd = (struct pollfd){entries[server].object->file_fd, POLLIN, 0};
    assert(horizon_poll_one(&pfd, 0));
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == 0 && !more);
    assert(!horizon_poll_one(&pfd, 0));
    assert(horizon_byte_pipe_write(b, data, sizeof(data)) == sizeof(data));
    assert(horizon_byte_pipe_write(b, "later", 5) == -1 && errno == EAGAIN);
    assert(horizon_byte_pipe_read_ex(a, result, 100, &more) == 100 && more);
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == sizeof(data) - 100 && !more);
    assert(horizon_byte_pipe_write(b, "after", 5) == 5);
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == 5);

    entries[server].object->file_completion = port;
    entries[server].object->file_completion_key = 123;
    request.async.handle = server;
    request.type = 1;
    assert(!horizon_server_handle_register_async(&owner, (void *)&request));
    assert(u32(last_reply) == HORIZON_STATUS_PENDING);
    assert(!horizon_sock_poll_all_asyncs_locked());
    assert(horizon_byte_pipe_write(b, "overlapped", 10) == 10);
    assert(horizon_sock_poll_all_asyncs_locked());
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && u32(call + 4) == HORIZON_STATUS_ALERTED);
    assert(horizon_byte_pipe_read_ex(a, result, 4, &more) == 4 && more);
    send_result(apc, HORIZON_STATUS_BUFFER_OVERFLOW, 4);
    assert(!horizon_asyncs.head && event->signaled && port->refs == 1);
    assert(posts[nposts - 1].status == HORIZON_STATUS_BUFFER_OVERFLOW && posts[nposts - 1].info == 4);
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == 6 && !more);

    assert(horizon_byte_pipe_write(a, "space", 5) == 5);
    request.type = 2;
    request.count = sizeof(data);
    assert(!horizon_server_handle_register_async(&owner, (void *)&request));
    assert(u32(last_reply) == HORIZON_STATUS_PENDING);
    assert(!horizon_sock_poll_all_asyncs_locked());
    assert(horizon_byte_pipe_read(b, result, sizeof(result)) == 5);
    assert(horizon_sock_poll_all_asyncs_locked());
    apc = horizon_server_async_apc_locked(&owner, call);
    assert(apc && u32(call + 4) == HORIZON_STATUS_ALERTED);
    assert(horizon_byte_pipe_write(a, data, sizeof(data)) == sizeof(data));
    send_result(apc, HORIZON_STATUS_SUCCESS, sizeof(data));
    assert(!horizon_asyncs.head && event->signaled && port->refs == 1);
    assert(horizon_byte_pipe_read_ex(b, result, sizeof(result), &more) == sizeof(data) && !more);
    assert(horizon_byte_pipe_write(b, "remaining", 9) == 9);
    byte_close(entries[client2].object->file_fd);
    entries[client2].object->file_fd = -1;
    assert(horizon_byte_pipe_read_ex(a, result, sizeof(result), &more) == 9 && !more);
    assert(horizon_byte_pipe_read(a, result, sizeof(result)) == -1 && errno == EPIPE);

    drop_pipes();
    puts("named pipes: byte/message reads, partial messages, peek, quotas, instances, pending I/O, IOCP and listen passed");
    return 0;
}
'''

extra = (extra.replace('@STRUCTS@', '\n'.join(struct(name) for name in (
    'horizon_object_attributes', 'horizon_object_name', 'horizon_create_named_pipe_request',
    'horizon_create_named_pipe_reply', 'horizon_set_named_pipe_info_request',
    'horizon_open_file_object_request', 'horizon_open_file_object_reply',
    'horizon_register_async_request', 'horizon_ioctl_request', 'horizon_ioctl_reply')))
    .replace('@FILEACCESS@', (root / 'dlls/ntdll/unix/horizon_file_access.h').read_text())
    .replace('@FUNCTIONS@', '\n\n'.join(definition(name) for name in (
        'horizon_server_parse_object_attributes', 'horizon_server_set_object_name',
        'horizon_named_pipe_has_prefix', 'horizon_named_pipe_name_tail', 'horizon_named_pipe_name_equal',
        'horizon_server_find_named_pipe_locked', 'horizon_named_pipe_instances_locked',
        'horizon_server_handle_create_named_pipe', 'horizon_server_handle_set_named_pipe_info',
        'horizon_server_handle_open_file_object', 'horizon_server_handle_pipe_ioctl',
        'horizon_server_handle_register_async'))))

fixture += extra
with tempfile.TemporaryDirectory(prefix='wine-nx-byte-pipes-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(fixture, errors='surrogateescape')
    subprocess.run([os.environ.get('CC', 'cc'), '-g', '-Wall', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    print(subprocess.run([str(tmp / 'test')], check=True, capture_output=True, text=True).stdout.strip())
