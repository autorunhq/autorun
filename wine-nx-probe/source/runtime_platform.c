#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <switch.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "horizon_private.h"
#include "unix_private.h"

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

const char wine_build[] __attribute__((weak)) = "wine-11.0-nx";

extern unixlib_handle_t __wine_unixlib_handle;
extern unixlib_handle_t wine_nx_get_ntdll_unixlib_handle(void);
extern void wine_nx_init_paths(void);
extern void init_environment(void);
extern void __wine_syscall_dispatcher(void);
extern NTSTATUS WINAPI wine_nx_pe_unix_call_dispatcher( unixlib_handle_t handle,
                                                         unsigned int code, void *args );
extern NTSTATUS (WINAPI *wine_nx_unix_call_dispatcher_ptr)( unixlib_handle_t handle,
                                                            unsigned int code, void *args );
extern void *wine_nx_syscall_dispatcher_ptr;

extern void wine_nx_runtime_trace( const char *msg );

static void wine_nx_init_sockets(void)
{
    /* libnx's BSD socket API requires explicit init via socketInitializeDefault
     * (uses BsdServiceType_Auto: bsd:s -> bsd:u fallback). Without this, every
     * call to socket() returns -1 with ENOSYS. Done once at startup so all PE
     * code calling through ntdll/unix/socket.c sees a functioning POSIX socket
     * layer that maps to FreeBSD-derived bsd: service on Horizon. */
    static int sockets_initialized;
    if (sockets_initialized) return;

    {
        Result rc = socketInitializeDefault();
        char buf[96];
        if (R_SUCCEEDED(rc))
        {
            wine_nx_runtime_trace( "[NET] socketInitializeDefault OK" );
            sockets_initialized = 1;
        }
        else
        {
            snprintf( buf, sizeof(buf), "[NET] socketInitializeDefault FAILED rc=0x%08x", (unsigned)rc );
            wine_nx_runtime_trace( buf );
        }
    }
}

/* The SDL launcher performs HTTPS requests before Wine's loader and virtual
 * memory are initialized, so its socket service must be available separately. */
void wine_nx_runtime_network_init(void)
{
    wine_nx_init_sockets();
}

void wine_nx_runtime_platform_init(void)
{
    static int paths_initialized;

    if (!paths_initialized)
    {
        wine_nx_init_paths();
        paths_initialized = 1;
    }
    if (!__wine_unixlib_handle)
        __wine_unixlib_handle = wine_nx_get_ntdll_unixlib_handle();
    if (!wine_nx_unix_call_dispatcher_ptr)
        wine_nx_unix_call_dispatcher_ptr = wine_nx_pe_unix_call_dispatcher;
    if (!wine_nx_syscall_dispatcher_ptr)
        wine_nx_syscall_dispatcher_ptr = __wine_syscall_dispatcher;
    wine_nx_init_sockets();
}

void wine_nx_runtime_environment_init(void)
{
    static int environment_initialized;

    if (environment_initialized) return;
    setenv( "WINEDLLOVERRIDES", "unicows=d", 0 );
    init_environment();
    environment_initialized = 1;
}

struct fd_path_entry
{
    int fd;
    char *path;
    dev_t dev;
    ino_t ino;
    mode_t mode;
    int has_stat;
    struct fd_path_entry *next;
};

static pthread_mutex_t fd_path_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct fd_path_entry *fd_paths;

static int path_is_absolute( const char *path )
{
    return path[0] == '/' || strchr( path, ':' );
}

static char *path_dup( const char *path )
{
    size_t len = strlen( path ) + 1;
    char *copy = malloc( len );

    if (copy) memcpy( copy, path, len );
    return copy;
}

static char *path_join( const char *base, const char *name )
{
    size_t base_len = strlen( base );
    size_t name_len = strlen( name );
    int need_slash = base_len && base[base_len - 1] != '/';
    char *path = malloc( base_len + need_slash + name_len + 1 );

    if (!path) return NULL;
    memcpy( path, base, base_len );
    if (need_slash) path[base_len++] = '/';
    memcpy( path + base_len, name, name_len + 1 );
    return path;
}

static void fd_path_remove_locked( int fd )
{
    struct fd_path_entry **entry = &fd_paths;

    while (*entry)
    {
        if ((*entry)->fd == fd)
        {
            struct fd_path_entry *old = *entry;

            *entry = old->next;
            free( old->path );
            free( old );
            return;
        }
        entry = &(*entry)->next;
    }
}

static void fd_path_register( int fd, const char *path )
{
    struct fd_path_entry *entry;
    struct stat st;

    if (fd < 0 || !path) return;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return;
    if (!(entry->path = path_dup( path )))
    {
        free( entry );
        return;
    }
    if (!fstat( fd, &st ))
    {
        entry->dev = st.st_dev;
        entry->ino = st.st_ino;
        entry->mode = st.st_mode;
        entry->has_stat = 1;
    }

    pthread_mutex_lock( &fd_path_mutex );
    fd_path_remove_locked( fd );
    entry->fd = fd;
    entry->next = fd_paths;
    fd_paths = entry;
    pthread_mutex_unlock( &fd_path_mutex );
}

static char *fd_path_lookup( int fd )
{
    struct fd_path_entry *entry;
    struct stat st;
    char *path = NULL;

    pthread_mutex_lock( &fd_path_mutex );
    for (entry = fd_paths; entry; entry = entry->next)
    {
        if (entry->fd != fd) continue;
        if (entry->has_stat)
        {
            int ret = fstat( fd, &st );

            if ((ret == -1 && errno == EBADF) ||
                (ret == 0 && (st.st_dev != entry->dev || st.st_ino != entry->ino ||
                st.st_mode != entry->mode)
                ))
            {
                fd_path_remove_locked( fd );
                break;
            }
        }
        path = path_dup( entry->path );
        break;
    }
    pthread_mutex_unlock( &fd_path_mutex );
    return path;
}

static char *resolve_at_path( int dirfd, const char *path )
{
    char *base, *resolved;

    if (!path)
    {
        errno = EFAULT;
        return NULL;
    }
    if (dirfd == AT_FDCWD || path_is_absolute( path )) return path_dup( path );
    if (!(base = fd_path_lookup( dirfd )))
    {
        errno = ENOSYS;
        return NULL;
    }
    resolved = path_join( base, path );
    free( base );
    if (!resolved) errno = ENOMEM;
    return resolved;
}

SECTION_IMAGE_INFORMATION main_image_info __attribute__((weak)) =
{
    .MaximumStackSize = 1024 * 1024,
    .CommittedStackSize = 64 * 1024,
    .SubSystemType = IMAGE_SUBSYSTEM_WINDOWS_CUI,
    .Machine = IMAGE_FILE_MACHINE_ARM64,
    .ImageContainsCode = TRUE,
};

SIZE_T kernel_stack_size __attribute__((weak)) = 1024 * 1024;
BOOL simulate_writecopy __attribute__((weak)) = FALSE;
SYSTEM_SERVICE_TABLE KeServiceDescriptorTable[4] __attribute__((weak));

/* Serialize seek-based positioned I/O and preserve the caller's cursor.
 * Ordinary read/write on a shared descriptor still require caller coordination. */
static pthread_mutex_t positioned_io_mutex = PTHREAD_MUTEX_INITIALIZER;

ssize_t pread( int fd, void *buffer, size_t size, off_t offset ) __attribute__((weak));
ssize_t pread( int fd, void *buffer, size_t size, off_t offset )
{
    ssize_t total = 0;
    off_t saved;
    int saved_errno;

    pthread_mutex_lock( &positioned_io_mutex );
    saved = lseek( fd, 0, SEEK_CUR );
    if (saved == (off_t)-1 || lseek( fd, offset, SEEK_SET ) == (off_t)-1)
    {
        pthread_mutex_unlock( &positioned_io_mutex );
        return -1;
    }

    while (size)
    {
        ssize_t ret = read( fd, (char *)buffer + total, size );

        if (ret > 0)
        {
            total += ret;
            size -= ret;
            continue;
        }
        if (!ret) break;
        if (errno == EINTR) continue;
        total = total ? total : -1;
        break;
    }

    saved_errno = errno;
    if (lseek( fd, saved, SEEK_SET ) == (off_t)-1)
        total = -1;
    else
        errno = saved_errno;
    pthread_mutex_unlock( &positioned_io_mutex );
    return total;
}

ssize_t pwrite( int fd, const void *buffer, size_t size, off_t offset ) __attribute__((weak));
ssize_t pwrite( int fd, const void *buffer, size_t size, off_t offset )
{
    ssize_t total = 0;
    off_t saved;
    int saved_errno;

    pthread_mutex_lock( &positioned_io_mutex );
    saved = lseek( fd, 0, SEEK_CUR );
    if (saved == (off_t)-1 || lseek( fd, offset, SEEK_SET ) == (off_t)-1)
    {
        pthread_mutex_unlock( &positioned_io_mutex );
        return -1;
    }

    while (size)
    {
        ssize_t ret = write( fd, (const char *)buffer + total, size );

        if (ret > 0)
        {
            total += ret;
            size -= ret;
            continue;
        }
        if (!ret)
        {
            errno = EIO;
            total = total ? total : -1;
            break;
        }
        if (errno == EINTR) continue;
        total = total ? total : -1;
        break;
    }

    saved_errno = errno;
    if (lseek( fd, saved, SEEK_SET ) == (off_t)-1)
        total = -1;
    else
        errno = saved_errno;
    pthread_mutex_unlock( &positioned_io_mutex );
    return total;
}

int openat( int dirfd, const char *path, int flags, ... ) __attribute__((weak));
int openat( int dirfd, const char *path, int flags, ... )
{
    char *resolved;
    mode_t mode = 0;
    int fd;

    if (flags & O_CREAT)
    {
        va_list args;

        va_start( args, flags );
        mode = (mode_t)va_arg( args, int );
        va_end( args );
    }

    if (!(resolved = resolve_at_path( dirfd, path ))) return -1;
    fd = (flags & O_CREAT) ? open( resolved, flags, mode ) : open( resolved, flags );
    if (fd != -1) fd_path_register( fd, resolved );
    free( resolved );
    return fd;
}

int fstatat( int dirfd, const char *path, struct stat *st, int flags ) __attribute__((weak));
int fstatat( int dirfd, const char *path, struct stat *st, int flags )
{
    char *resolved;
    int ret;

    (void)flags;
    if (!(resolved = resolve_at_path( dirfd, path ))) return -1;
    ret = stat( resolved, st );
    /* A file this process has open for writing cannot be reopened to stat it. */
    if (ret == -1 && errno == EIO) ret = horizon_stat_open_file( resolved, st );
    free( resolved );
    return ret;
}

DIR *fdopendir( int fd ) __attribute__((weak));
DIR *fdopendir( int fd )
{
    char *path = fd_path_lookup( fd );
    DIR *dir;

    if (!path)
    {
        errno = ENOSYS;
        return NULL;
    }

    dir = opendir( path );
    free( path );
    if (dir)
    {
        pthread_mutex_lock( &fd_path_mutex );
        fd_path_remove_locked( fd );
        pthread_mutex_unlock( &fd_path_mutex );
        close( fd );
    }
    return dir;
}

int fchdir( int fd ) __attribute__((weak));
int fchdir( int fd )
{
    char *path = fd_path_lookup( fd );
    int ret;

    if (!path)
    {
        errno = ENOSYS;
        return -1;
    }
    ret = chdir( path );
    free( path );
    return ret;
}

int pipe( int fd[2] ) __attribute__((weak));
int pipe( int fd[2] )
{
    return horizon_pipe( fd );
}

int execv( const char *path, char *const argv[] ) __attribute__((weak));
int execv( const char *path, char *const argv[] )
{
    (void)path;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

int execvp( const char *path, char *const argv[] ) __attribute__((weak));
int execvp( const char *path, char *const argv[] )
{
    (void)path;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

pid_t waitpid( pid_t pid, int *status, int options ) __attribute__((weak));
pid_t waitpid( pid_t pid, int *status, int options )
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ECHILD;
    return -1;
}

pid_t setsid(void) __attribute__((weak));
pid_t setsid(void)
{
    errno = ENOSYS;
    return -1;
}

long sysconf( int name ) __attribute__((weak));
long sysconf( int name )
{
#ifdef _SC_NPROCESSORS_ONLN
    if (name == _SC_NPROCESSORS_ONLN) return 4;
#endif
#ifdef _SC_PAGESIZE
    if (name == _SC_PAGESIZE) return 0x1000;
#endif
    return -1;
}

NTSTATUS WINAPI NtQueryInformationProcess( HANDLE process, PROCESSINFOCLASS class,
                                           void *info, ULONG size, ULONG *ret_len ) __attribute__((weak));
NTSTATUS WINAPI NtQueryInformationProcess( HANDLE process, PROCESSINFOCLASS class,
                                           void *info, ULONG size, ULONG *ret_len )
{
    (void)process;
    if (ret_len) *ret_len = 0;

    switch (class)
    {
    case ProcessBasicInformation:
        if (size < sizeof(PROCESS_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        memset( info, 0, sizeof(PROCESS_BASIC_INFORMATION) );
        if (ret_len) *ret_len = sizeof(PROCESS_BASIC_INFORMATION);
        return STATUS_SUCCESS;
    case ProcessDebugPort:
        if (size < sizeof(HANDLE)) return STATUS_INFO_LENGTH_MISMATCH;
        *(HANDLE *)info = 0;
        if (ret_len) *ret_len = sizeof(HANDLE);
        return STATUS_SUCCESS;
    case ProcessWow64Information:  /* 26 */
        if (size < sizeof(ULONG_PTR)) return STATUS_INFO_LENGTH_MISMATCH;
        *(ULONG_PTR *)info = 0;  /* not WoW64 */
        if (ret_len) *ret_len = sizeof(ULONG_PTR);
        return STATUS_SUCCESS;
    case ProcessDefaultHardErrorMode:  /* 12 */
        if (size < sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
        *(ULONG *)info = 0;
        if (ret_len) *ret_len = sizeof(ULONG);
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}

ULONG_PTR redirect_arm64ec_rva( void *module, ULONG_PTR rva,
                                const IMAGE_ARM64EC_METADATA *metadata ) __attribute__((weak));
ULONG_PTR redirect_arm64ec_rva( void *module, ULONG_PTR rva,
                                const IMAGE_ARM64EC_METADATA *metadata )
{
    (void)module;
    (void)metadata;
    return rva;
}
