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

static unsigned int smoke_init_first_thread(void)
{
    NtCurrentTeb();
    struct thread_data *thread_data = get_thread_data();
    USHORT machines[8] = { 0 };
    obj_handle_t version = 0;
    int control_fd;
    int reply_pipe[2] = {-1, -1};
    int wait_pipe[2] = {-1, -1};
    unsigned int status;

    control_fd = horizon_server_connect();
    thread_data->request_fd = wine_server_receive_fd( &version );
    if (version != SERVER_PROTOCOL_VERSION)
    {
        log_line( "[FAIL] protocol version got=%u expected=%u", version, SERVER_PROTOCOL_VERSION );
        failures++;
        return STATUS_REVISION_MISMATCH;
    }

    if (horizon_pipe( reply_pipe ) == -1 || horizon_pipe( wait_pipe ) == -1)
    {
        log_line( "[FAIL] bootstrap pipe errno=%d", errno );
        failures++;
        return STATUS_UNSUCCESSFUL;
    }

    thread_data->reply_fd = reply_pipe[0];
    thread_data->wait_fd[0] = wait_pipe[0];
    thread_data->wait_fd[1] = wait_pipe[1];

    wine_server_send_fd( reply_pipe[1] );
    wine_server_send_fd( wait_pipe[1] );

    SERVER_START_REQ( init_first_thread )
    {
        req->unix_pid = 1;
        req->unix_tid = 1;
        req->reply_fd = reply_pipe[1];
        req->wait_fd = wait_pipe[1];
        req->debug_level = 0;
        req->page_size = 4096;
        wine_server_set_reply( req, machines, sizeof(machines) );
        status = wine_server_call( req );
        if (!status)
        {
            check_bool( "init_first_thread machine", wine_server_reply_size( reply ) >= sizeof(USHORT) &&
                        machines[0] == IMAGE_FILE_MACHINE_ARM64 );
            log_line( "[INFO] init pid=%u tid=%u session=%u", reply->pid, reply->tid, reply->session_id );
        }
    }
    SERVER_END_REQ;

    close( reply_pipe[1] );
    close( control_fd );
    return status;
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

static unsigned int smoke_reinit_thread(void)
{
    struct thread_data *thread_data = get_thread_data();
    int reply_pipe[2] = {-1, -1};
    int wait_pipe[2] = {-1, -1};
    unsigned int status;

    if (horizon_pipe( reply_pipe ) == -1 || horizon_pipe( wait_pipe ) == -1)
    {
        log_line( "[FAIL] init_thread pipe errno=%d", errno );
        failures++;
        return STATUS_UNSUCCESSFUL;
    }

    close( thread_data->reply_fd );
    close( thread_data->wait_fd[0] );
    close( thread_data->wait_fd[1] );

    thread_data->reply_fd = reply_pipe[0];
    thread_data->wait_fd[0] = wait_pipe[0];
    thread_data->wait_fd[1] = wait_pipe[1];

    wine_server_send_fd( reply_pipe[1] );
    wine_server_send_fd( wait_pipe[1] );

    SERVER_START_REQ( init_thread )
    {
        req->unix_tid = 2;
        req->reply_fd = reply_pipe[1];
        req->wait_fd = wait_pipe[1];
        req->teb = wine_server_client_ptr( NtCurrentTeb() );
        req->entry = 0;
        status = wine_server_call( req );
        if (!status) log_line( "[INFO] init_thread suspend=%d", reply->suspend );
    }
    SERVER_END_REQ;

    close( reply_pipe[1] );
    return status;
}

static void make_named_attr( void *buffer, size_t buffer_size, const WCHAR *name,
                             data_size_t name_size, data_size_t *ret_size )
{
    struct object_attributes *attr = buffer;
    unsigned char *ptr = (unsigned char *)(attr + 1);

    memset( buffer, 0, buffer_size );
    attr->name_len = name_size;
    memcpy( ptr, name, name_size );
    *ret_size = (sizeof(*attr) + name_size + 3) & ~3;
}

static unsigned int create_named_event( obj_handle_t *handle )
{
    static const WCHAR name[] = {
        '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s',
        '\\','C','o','d','e','x','S','m','o','k','e'
    };
    unsigned char attr_buffer[sizeof(struct object_attributes) + sizeof(name) + 4];
    data_size_t attr_size;
    unsigned int status;

    make_named_attr( attr_buffer, sizeof(attr_buffer), name, sizeof(name), &attr_size );

    SERVER_START_REQ( create_event )
    {
        req->access = EVENT_ALL_ACCESS;
        req->manual_reset = 1;
        req->initial_state = 0;
        wine_server_add_data( req, attr_buffer, attr_size );
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int open_named_event( obj_handle_t *handle )
{
    static const WCHAR name[] = {
        '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s',
        '\\','C','o','d','e','x','S','m','o','k','e'
    };
    unsigned int status;

    SERVER_START_REQ( open_event )
    {
        req->access = EVENT_ALL_ACCESS;
        req->attributes = 0;
        req->rootdir = 0;
        wine_server_add_data( req, name, sizeof(name) );
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int create_event_object( obj_handle_t *handle )
{
    unsigned int status;

    SERVER_START_REQ( create_event )
    {
        req->access = EVENT_ALL_ACCESS;
        req->manual_reset = 0;
        req->initial_state = 0;
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int set_event_object( obj_handle_t handle, int *state )
{
    unsigned int status;

    SERVER_START_REQ( event_op )
    {
        req->handle = handle;
        req->op = SET_EVENT;
        status = wine_server_call( req );
        *state = reply->state;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int query_event_object( obj_handle_t handle, int *manual, int *state )
{
    unsigned int status;

    SERVER_START_REQ( query_event )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *manual = reply->manual_reset;
        *state = reply->state;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int select_one( obj_handle_t handle, int *signaled )
{
    union apc_result apc_result;
    union select_op select_op;
    data_size_t select_size = offsetof( union select_op, wait.handles[1] );
    int cookie = 0;
    unsigned int status;

    memset( &apc_result, 0, sizeof(apc_result) );
    memset( &select_op, 0, sizeof(select_op) );
    select_op.wait.op = SELECT_WAIT;
    select_op.wait.handles[0] = handle;

    SERVER_START_REQ( select )
    {
        req->flags = 0;
        req->cookie = wine_server_client_ptr( &cookie );
        req->timeout = 0;
        req->size = select_size;
        req->prev_apc = 0;
        wine_server_add_data( req, &apc_result, sizeof(apc_result) );
        wine_server_add_data( req, &select_op, select_size );
        status = wine_server_call( req );
        *signaled = reply->signaled;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int dup_handle_object( obj_handle_t handle, obj_handle_t *dup )
{
    unsigned int status;

    SERVER_START_REQ( dup_handle )
    {
        req->src_process = 0;
        req->src_handle = handle;
        req->dst_process = 0;
        req->access = 0;
        req->attributes = 0;
        req->options = 0;
        status = wine_server_call( req );
        *dup = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int compare_handles( obj_handle_t first, obj_handle_t second )
{
    unsigned int status;

    SERVER_START_REQ( compare_objects )
    {
        req->first = first;
        req->second = second;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int close_handle_object( obj_handle_t handle )
{
    unsigned int status;

    SERVER_START_REQ( close_handle )
    {
        req->handle = handle;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int create_mutex_object( obj_handle_t *handle )
{
    unsigned int status;

    SERVER_START_REQ( create_mutex )
    {
        req->access = MUTANT_ALL_ACCESS;
        req->owned = 1;
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int query_mutex_object( obj_handle_t handle, unsigned int *count, int *owned )
{
    unsigned int status;

    SERVER_START_REQ( query_mutex )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *count = reply->count;
        *owned = reply->owned;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int release_mutex_object( obj_handle_t handle, unsigned int *prev_count )
{
    unsigned int status;

    SERVER_START_REQ( release_mutex )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *prev_count = reply->prev_count;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int create_semaphore_object( obj_handle_t *handle )
{
    unsigned int status;

    SERVER_START_REQ( create_semaphore )
    {
        req->access = SEMAPHORE_ALL_ACCESS;
        req->initial = 1;
        req->max = 3;
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int query_semaphore_object( obj_handle_t handle, unsigned int *current, unsigned int *max )
{
    unsigned int status;

    SERVER_START_REQ( query_semaphore )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *current = reply->current;
        *max = reply->max;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int release_semaphore_object( obj_handle_t handle, unsigned int count, unsigned int *prev_count )
{
    unsigned int status;

    SERVER_START_REQ( release_semaphore )
    {
        req->handle = handle;
        req->count = count;
        status = wine_server_call( req );
        *prev_count = reply->prev_count;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int create_timer_object( obj_handle_t *handle )
{
    unsigned int status;

    SERVER_START_REQ( create_timer )
    {
        req->access = TIMER_ALL_ACCESS;
        req->manual = 0;
        status = wine_server_call( req );
        *handle = reply->handle;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int set_timer_object( obj_handle_t handle, timeout_t when, int *signaled )
{
    unsigned int status;

    SERVER_START_REQ( set_timer )
    {
        req->handle = handle;
        req->expire = when;
        req->callback = 0;
        req->arg = 0;
        req->period = 0;
        status = wine_server_call( req );
        *signaled = reply->signaled;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int query_timer_object( obj_handle_t handle, timeout_t *when, int *signaled )
{
    unsigned int status;

    SERVER_START_REQ( get_timer_info )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *when = reply->when;
        *signaled = reply->signaled;
    }
    SERVER_END_REQ;

    return status;
}

static unsigned int cancel_timer_object( obj_handle_t handle, int *signaled )
{
    unsigned int status;

    SERVER_START_REQ( cancel_timer )
    {
        req->handle = handle;
        status = wine_server_call( req );
        *signaled = reply->signaled;
    }
    SERVER_END_REQ;

    return status;
}

int main(int argc, char **argv)
{
    obj_handle_t event_handle = 0;
    obj_handle_t named_event = 0;
    obj_handle_t named_event_again = 0;
    obj_handle_t opened_event = 0;
    obj_handle_t dup_event = 0;
    obj_handle_t mutex_handle = 0;
    obj_handle_t semaphore_handle = 0;
    obj_handle_t timer_handle = 0;
    unsigned int status;
    unsigned int count;
    unsigned int max_count;
    unsigned int prev_count;
    timeout_t timer_when = 0x123456789LL;
    timeout_t queried_when;
    int state;
    int manual;
    int owned;
    int signaled;

    (void)argc;
    (void)argv;

    consoleInit( NULL );
    mkdir( "sdmc:/switch", 0777 );
    mkdir( "sdmc:/switch/wine-nx-probe", 0777 );
    log_file = fopen( "sdmc:/switch/wine-nx-probe/ntdll-smoke.log", "w" );

    log_line( "wine-nx-ntdll-smoke: real server.h + server.c vertical smoke" );

    status = smoke_init_first_thread();
    if (check_status( "init_first_thread", status ))
        check_status( "init_process_done", smoke_init_process_done() );
    check_status( "init_thread", smoke_reinit_thread() );

    status = create_event_object( &event_handle );
    if (check_status( "create_event", status ))
    {
        check_bool( "create_event handle", event_handle != 0 );
        check_status( "query_event initial", query_event_object( event_handle, &manual, &state ) );
        check_bool( "query_event initial values", manual == 0 && state == 0 );
        check_status( "set_event", set_event_object( event_handle, &state ) );
        check_bool( "set_event previous state", state == 0 );
        check_status( "query_event set", query_event_object( event_handle, &manual, &state ) );
        check_bool( "query_event set values", manual == 0 && state == 1 );
        check_status( "select event", select_one( event_handle, &signaled ) );
        check_bool( "select event signaled", signaled == 1 );
        check_status( "query_event consumed", query_event_object( event_handle, &manual, &state ) );
        check_bool( "query_event consumed values", manual == 0 && state == 0 );
    }

    status = create_named_event( &named_event );
    if (check_status( "create_named_event", status ))
    {
        status = create_named_event( &named_event_again );
        check_bool( "create_named_event duplicate status", status == STATUS_OBJECT_NAME_EXISTS );
        check_bool( "create_named_event duplicate handle", named_event_again != 0 );
        check_status( "open_named_event", open_named_event( &opened_event ) );
        check_status( "compare named/opened", compare_handles( named_event, opened_event ) );
    }

    check_status( "dup_handle event", dup_handle_object( event_handle, &dup_event ) );
    check_status( "compare event/dup", compare_handles( event_handle, dup_event ) );
    check_bool( "compare event/named differs",
                compare_handles( event_handle, named_event ) == (unsigned int)STATUS_NOT_SAME_OBJECT );

    status = create_mutex_object( &mutex_handle );
    if (check_status( "create_mutex", status ))
    {
        check_status( "query_mutex owned", query_mutex_object( mutex_handle, &count, &owned ) );
        check_bool( "query_mutex owned values", count == 1 && owned == 1 );
        check_status( "release_mutex", release_mutex_object( mutex_handle, &prev_count ) );
        check_bool( "release_mutex prev_count", prev_count == 1 );
        check_status( "query_mutex free", query_mutex_object( mutex_handle, &count, &owned ) );
        check_bool( "query_mutex free values", count == 0 && owned == 0 );
    }

    status = create_semaphore_object( &semaphore_handle );
    if (check_status( "create_semaphore", status ))
    {
        check_status( "query_semaphore", query_semaphore_object( semaphore_handle, &count, &max_count ) );
        check_bool( "query_semaphore values", count == 1 && max_count == 3 );
        check_status( "release_semaphore", release_semaphore_object( semaphore_handle, 2, &prev_count ) );
        check_bool( "release_semaphore prev_count", prev_count == 1 );
        check_bool( "release_semaphore limit",
                    release_semaphore_object( semaphore_handle, 2, &prev_count ) ==
                    (unsigned int)STATUS_SEMAPHORE_LIMIT_EXCEEDED );
    }

    status = create_timer_object( &timer_handle );
    if (check_status( "create_timer", status ))
    {
        check_status( "set_timer", set_timer_object( timer_handle, timer_when, &signaled ) );
        check_bool( "set_timer previous signal", signaled == 0 );
        check_status( "query_timer", query_timer_object( timer_handle, &queried_when, &signaled ) );
        check_bool( "query_timer values", queried_when == timer_when && signaled == 1 );
        check_status( "select timer", select_one( timer_handle, &signaled ) );
        check_bool( "select timer signaled", signaled == 1 );
        check_status( "query_timer consumed", query_timer_object( timer_handle, &queried_when, &signaled ) );
        check_bool( "query_timer consumed values", signaled == 0 );
        check_status( "cancel_timer", cancel_timer_object( timer_handle, &signaled ) );
    }

    if (timer_handle) check_status( "close timer", close_handle_object( timer_handle ) );
    if (semaphore_handle) check_status( "close semaphore", close_handle_object( semaphore_handle ) );
    if (mutex_handle) check_status( "close mutex", close_handle_object( mutex_handle ) );
    if (dup_event) check_status( "close dup event", close_handle_object( dup_event ) );
    if (opened_event) check_status( "close opened event", close_handle_object( opened_event ) );
    if (named_event_again) check_status( "close duplicate named event", close_handle_object( named_event_again ) );
    if (named_event) check_status( "close named event", close_handle_object( named_event ) );
    if (event_handle) check_status( "close event", close_handle_object( event_handle ) );

    log_line( "SUMMARY failures=%d overall=%s", failures, failures ? "FAIL" : "OK" );
    park_forever();
    return failures ? 1 : 0;
}
