#include <switch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/server.h"
#include "unix_private.h"
#include "horizon_private.h"

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 256 * 1024 * 1024;
unsigned char __attribute__((aligned(16))) __nx_exception_stack[0x10000];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);

static FILE *log_file;
static int failures;

static void log_line( const char *fmt, ... )
{
    va_list args;

    va_start( args, fmt );
    vprintf( fmt, args );
    va_end( args );
    putchar( '\n' );

    if (log_file)
    {
        va_start( args, fmt );
        vfprintf( log_file, fmt, args );
        va_end( args );
        fputc( '\n', log_file );
        fflush( log_file );
    }
    consoleUpdate( NULL );
}

static int check_status( const char *name, unsigned int status )
{
    if (status == STATUS_SUCCESS)
    {
        log_line( "[OK] %s", name );
        return 1;
    }

    log_line( "[FAIL] %s status=%08x", name, status );
    failures++;
    return 0;
}

static int check_bool( const char *name, int ok )
{
    if (ok)
    {
        log_line( "[OK] %s", name );
        return 1;
    }

    log_line( "[FAIL] %s", name );
    failures++;
    return 0;
}

static void park_forever(void)
{
    log_line( "[EXIT] parked after summary; close the application from HOME" );
    for (;;) svcSleepThread( 1000000000LL );
}

static int smoke_init_process(void)
{
    struct thread_data *data = virtual_alloc_first_thread_data();
    server_init_process( data );

    log_line( "[INFO] server_init_process pid=%lu tid=%lu session=%u", pid, data->tid, session_id );
    return check_bool( "server_init_process machine",
                       supported_machines_count && native_machine == IMAGE_FILE_MACHINE_ARM64 );
}

static unsigned int smoke_init_process_done(void)
{
    unsigned int status;

    SERVER_START_REQ( init_process_done )
    {
        req->teb = wine_server_client_ptr( NtCurrentTeb() );
        req->peb = wine_server_client_ptr( NtCurrentTeb()->Peb );
        status = wine_server_call( req );
        if (!status) log_line( "[INFO] init_process_done suspend=%d", reply->suspend );
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int smoke_open_unix_file( HANDLE *handle )
{
    OBJECT_ATTRIBUTES attr;

    memset( &attr, 0, sizeof(attr) );
    attr.Length = sizeof(attr);
    return open_unix_file( handle, "sdmc:/switch/wine-nx-probe/real-file.txt",
                           FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                           &attr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
}

static unsigned int close_handle_object( HANDLE handle )
{
    unsigned int status;

    SERVER_START_REQ( close_handle )
    {
        req->handle = wine_server_obj_handle( handle );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    return status;
}

int main(int argc, char **argv)
{
    static const char payload[] = "wine-horizon real file path\n";
    TEB *teb;
    HANDLE handle = 0;
    enum server_fd_type type = FD_TYPE_INVALID;
    unsigned int options = 0;
    unsigned int status;
    int needs_close = 0;
    int fd = -1;
    char buffer[sizeof(payload)] = { 0 };
    ssize_t ret;

    (void)argc;
    (void)argv;

    consoleInit( NULL );
    mkdir( "sdmc:/switch", 0777 );
    mkdir( "sdmc:/switch/wine-nx-probe", 0777 );
    log_file = fopen( "sdmc:/switch/wine-nx-probe/ntdll-file-smoke.log", "w" );

    log_line( "wine-nx-ntdll-file-smoke: real Wine virtual/thread/server/file path" );

    log_line( "[STEP] virtual_init" );
    virtual_init();
    log_line( "[OK] virtual_init" );
    smoke_init_process();
    main_image_info.Machine = IMAGE_FILE_MACHINE_ARM64;
    log_line( "[STEP] virtual_alloc_first_teb" );
    virtual_alloc_first_teb();
    teb = NtCurrentTeb();
    if (!check_bool( "virtual_alloc_first_teb", teb && NtCurrentTeb() == teb && teb->Peb ))
    {
        log_line( "SUMMARY failures=%d overall=FAIL", failures );
        park_forever();
    }

    check_status( "init_process_done", smoke_init_process_done() );

    status = smoke_open_unix_file( &handle );
    if (check_status( "open_unix_file", status ))
        check_bool( "open_unix_file handle", handle != 0 );

    if (handle)
    {
        status = server_get_unix_fd( handle, FILE_WRITE_DATA, &fd, &needs_close, &type, &options );
        if (check_status( "server_get_unix_fd write", status ))
        {
            check_bool( "server_get_unix_fd type", type == FD_TYPE_FILE );
            ret = write( fd, payload, sizeof(payload) - 1 );
            check_bool( "write through passed fd", ret == (ssize_t)sizeof(payload) - 1 );
            if (needs_close) close( fd );
            fd = -1;
            needs_close = 0;
        }

        status = server_get_unix_fd( handle, FILE_READ_DATA, &fd, &needs_close, &type, &options );
        if (check_status( "server_get_unix_fd read", status ))
        {
            check_bool( "lseek passed fd", lseek( fd, 0, SEEK_SET ) == 0 );
            ret = read( fd, buffer, sizeof(buffer) - 1 );
            check_bool( "read through passed fd", ret == (ssize_t)sizeof(payload) - 1 );
            check_bool( "read content", !memcmp( buffer, payload, sizeof(payload) - 1 ) );
            if (needs_close) close( fd );
            fd = -1;
            needs_close = 0;
        }

        check_status( "close_handle file", close_handle_object( handle ) );
    }

    log_line( "SUMMARY failures=%d overall=%s", failures, failures ? "FAIL" : "OK" );
    park_forever();
    return failures ? 1 : 0;
}
