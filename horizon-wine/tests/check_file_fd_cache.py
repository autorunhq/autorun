#!/usr/bin/env python3
"""Run the Horizon server's get_handle_fd handler
(horizon_server_handle_get_handle_fd in dlls/ntdll/unix/horizon.c) on file
handles opened with the access masks programs use, and check which of them the
client may keep in its fd cache.

A cached copy keeps the file open, and Horizon can neither rename nor delete an
open file, so only a handle that can do neither is cached. Without the cache,
every seek and read on a file is a round trip to the server thread: The Sims
seeks in its .far archives over two million times while it loads."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
horizon = (root / 'dlls/ntdll/unix/horizon.c').read_text()
access = (root / 'dlls/ntdll/unix/horizon_file_access.h').read_text()


def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_STATUS_OBJECT_TYPE_MISMATCH 0xc0000024u
#define HORIZON_STATUS_BAD_DEVICE_TYPE 0xc00000cbu
#define HORIZON_FD_TYPE_FILE 1
#define HORIZON_FD_TYPE_SOCKET 3
#define HORIZON_FD_TYPE_CHAR 5
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
enum { HORIZON_SERVER_OBJECT_FILE = 1, HORIZON_SERVER_OBJECT_MAPPING, HORIZON_SERVER_OBJECT_SOCK,
       HORIZON_SERVER_OBJECT_NAMED_PIPE };

struct horizon_server_request_header { unsigned int req, request_size, reply_size; };
struct horizon_server_reply_header { unsigned int error, reply_size; };
struct horizon_get_handle_fd_request { struct horizon_server_request_header header; unsigned int handle; };
struct horizon_get_handle_fd_reply
{
    struct horizon_server_reply_header header;
    int type;
    int cacheable;
    unsigned int access;
    unsigned int options;
};
struct horizon_server_connection { int reply_fd; };
struct horizon_server_object { int type, file_fd, file_is_dir; unsigned int file_access, file_options; };
struct horizon_server_handle_entry { struct horizon_server_object *object; };

static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_server_handle_entry entry;
static struct horizon_get_handle_fd_reply last_reply;
static int queued_fd = -1;

static struct horizon_server_handle_entry *horizon_server_find_handle_locked( unsigned int handle )
{
    return handle == 0x40 ? &entry : NULL;
}

static void horizon_server_queue_fd( int fd, unsigned int handle )
{
    (void)handle;
    queued_fd = fd;
}

static int horizon_server_write_reply( int fd, const void *reply, unsigned int size,
                                       const void *data, unsigned int data_size )
{
    (void)fd; (void)data; (void)data_size;
    assert( size == sizeof(last_reply) );
    memcpy( &last_reply, reply, size );
    return 0;
}

@BODY@

/* The reply to get_handle_fd for a file opened with this access and these options. */
static struct horizon_get_handle_fd_reply file_fd( unsigned int access, unsigned int options )
{
    struct horizon_server_object object = { HORIZON_SERVER_OBJECT_FILE, 7, 0,
                                            horizon_file_map_access( access ), options };
    struct horizon_get_handle_fd_request request = { { 0, 0, 0 }, 0x40 };
    struct horizon_server_connection connection = { 3 };

    entry.object = &object;
    queued_fd = -1;
    assert( !horizon_server_handle_get_handle_fd( &connection, (const unsigned char *)&request ) );
    assert( !last_reply.header.error && last_reply.type == HORIZON_FD_TYPE_FILE );
    assert( queued_fd == 7 && last_reply.access == object.file_access );
    return last_reply;
}

int main( void )
{
    /* CreateFileW( GENERIC_READ ): The Sims's .far archives. Cached. */
    assert( file_fd( 0x80100080, 0x60 ).cacheable );
    /* FILE_READ_DATA | SYNCHRONIZE, and an attributes query. Cached. */
    assert( file_fd( 0x00100001, 0x60 ).cacheable );
    assert( file_fd( 0x00100080, 0x60 ).cacheable );

    /* GENERIC_WRITE, read and write, appending: a write handle stays uncached. */
    assert( !file_fd( 0x40100080, 0x60 ).cacheable );
    assert( !file_fd( 0xc0100080, 0x60 ).cacheable );
    assert( !file_fd( 0x00100004, 0x60 ).cacheable );
    /* DELETE can rename or delete the file through the handle; so can GENERIC_ALL. */
    assert( !file_fd( 0x80110080, 0x60 ).cacheable );
    assert( !file_fd( 0x10000000, 0x60 ).cacheable );
    /* FILE_DELETE_ON_CLOSE deletes it when the last handle closes. */
    assert( !file_fd( 0x80100080, 0x60 | 0x1000 ).cacheable );

    printf( "read-only file handles cached; write, delete and delete-on-close handles not\n" );
    return 0;
}
'''

body = '\n\n'.join([
    function(access, 'static inline unsigned int horizon_file_map_access'),
    function(horizon, 'static int horizon_server_handle_get_handle_fd'),
])

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    source = tmp / 'fd_cache.c'
    source.write_text(fixture.replace('@BODY@', body))
    binary = tmp / 'fd_cache'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-o', str(binary), str(source)], check=True)
    out = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    print(out.stdout.strip())
