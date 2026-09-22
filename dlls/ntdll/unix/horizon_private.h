/*
 * ntdll Horizon private interface
 *
 * Copyright 2026 Diogo Silva
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NTDLL_UNIX_HORIZON_PRIVATE_H
#define __NTDLL_UNIX_HORIZON_PRIVATE_H

#ifdef __SWITCH__
#include <stddef.h>

extern unsigned int horizon_set_process_machine( unsigned short machine );
extern ULONG_PTR horizon_get_system_affinity_mask(void);
extern unsigned int horizon_get_processor_count(void);
/* The runtime's thread profiler (wine-nx-probe/source/thread_profile.c); weak,
 * since the ntdll test executables link without the runtime. */
extern void wine_nx_thread_register( char kind, unsigned int tid, void *teb ) __attribute__((weak));
extern void wine_nx_thread_unregister( void ) __attribute__((weak));
extern void wine_nx_thread_affinity_fixed( void ) __attribute__((weak));
extern __thread int horizon_suspend_pending;
extern void horizon_wait_suspend_arm64ec(void);
extern void horizon_get_memory_info( unsigned long long *total, unsigned long long *used );
extern void horizon_get_address_space_limits( void **start, void **limit );
extern void *virtual_alloc_horizon_native( size_t size, void **token );
extern void virtual_free_horizon_native( void *token );
extern void *wine_nx_arm64ec_dispatch_ret;
extern int horizon_get_kernel_regions( void **starts, size_t *sizes, int max );
/* Logs the kernel's view of the low 4 GB once: megabytes per memory type and the largest free ranges. */
extern void horizon_log_low_address_space( void );
extern BOOL horizon_get_stack_region( void **start, void **limit );
extern BOOL horizon_is_native_stack_range( const void *address, size_t size );
/* Around re-protecting every view: the server's session views stay writable. */
extern void horizon_lock_session_views( void );
extern void horizon_unlock_session_views( void );
extern void horizon_trace( const char *fmt, ... );
extern void horizon_pin_current_thread( ULONG_PTR requested_mask );
extern void *horizon_anon_mmap_fixed( void *start, size_t size, int prot, int flags );
extern void *horizon_anon_mmap_alloc( size_t size, int prot );
extern int horizon_pipe( int fd[2] );
extern void horizon_server_queue_fd( int fd, unsigned int handle );
extern int horizon_server_take_client_fd( unsigned int *handle );
extern unsigned int horizon_server_protocol_version(void);
extern int horizon_server_connect(void);
extern void horizon_server_send_fd( int fd );
extern int horizon_server_receive_fd( unsigned int *handle );
extern int horizon_fast_sync_enabled;
extern int horizon_server_sync_call( unsigned int tid, const void *request, const void *data,
                                     unsigned int data_size, void *reply, void *reply_data );

/* Horizon address arbitration used as a futex; timeout_ns < 0 waits forever. */
extern int horizon_futex_wait( const int *addr, int value, long long timeout_ns );
extern void horizon_futex_wake( const int *addr, int count );
extern unsigned long long horizon_interrupt_time(void);
extern void wine_nx_start_user_shared_data_clock(void);
extern void horizon_mark_std_stream( HANDLE handle, int stream );
extern void horizon_echo_std_write( HANDLE handle, const void *data, size_t size );
struct stat;
extern int horizon_stat_open_file( const char *path, struct stat *st );

/* Live resources that each Wine thread owns; updated atomically. */
struct horizon_lifecycle_counters
{
    LONG connections;      /* server connections (one per live client thread) */
    LONG thread_objects;   /* server thread objects, including terminated ones */
    LONG pipes;            /* in-process pipes */
    LONG tebs;             /* TEBs handed out by virtual_alloc_teb */
    LONG worker_pthreads;  /* NtCreateThreadEx pthreads not yet joined */
    LONG thread_exits;
};
extern struct horizon_lifecycle_counters horizon_lifecycle;
extern void horizon_lifecycle_baseline(void);
extern void horizon_lifecycle_report( const char *tag, unsigned int tid, int code );
#endif

#endif /* __NTDLL_UNIX_HORIZON_PRIVATE_H */
