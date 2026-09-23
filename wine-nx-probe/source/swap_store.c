#include "swap_store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch/runtime/devices/fs_dev.h>
#endif

#define HEADER_SIZE 4096
#define PREALLOC_CHUNK (1024 * 1024)

static int transfer( int fd, void *buffer, size_t size, off_t offset, int writing )
{
    char *data = buffer;
    if (lseek( fd, offset, SEEK_SET ) != offset) return -1;
    while (size)
    {
        ssize_t count = writing ? write( fd, data, size ) : read( fd, data, size );
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        data += count;
        size -= count;
    }
    return 0;
}

static void header( unsigned char *data, unsigned int index, uint64_t size )
{
    memset( data, 0, HEADER_SIZE );
    snprintf( (char *)data, HEADER_SIZE, "Autorun swap POC 1\n%u\n%llu\n", index,
              (unsigned long long)size );
}

static void save_error( struct swap_store *store )
{
    store->fs_error = 0;
#ifdef __SWITCH__
    if (errno == EIO) store->fs_error = fsdevGetLastResult();
#endif
}

static int open_segment( struct swap_store *store, const char *directory, unsigned int index, uint64_t size,
                         swap_progress progress, void *context, uint64_t before, uint64_t total, int create )
{
    char path[768], temp[780];
    unsigned char expected[HEADER_SIZE], found[HEADER_SIZE];
    struct stat st;
    struct statvfs space;
    void *zeros = NULL;
    uint64_t done;
    int fd = -1, error, created = 0;

    store->operation = "open segment";
    store->offset = before;
    if (snprintf( path, sizeof(path), "%s/part-%u.swap", directory, index ) >= (int)sizeof(path))
    { errno = ENAMETOOLONG; goto failed; }
    header( expected, index, size );
    fd = open( path, O_RDWR );
    if (fd >= 0)
    {
        store->operation = "validate segment";
        if (fstat( fd, &st )) goto failed;
        if ((uint64_t)st.st_size != size + HEADER_SIZE) { errno = EINVAL; goto failed; }
        if (transfer( fd, found, sizeof(found), 0, 0 )) goto failed;
        if (memcmp( found, expected, sizeof(found) )) { errno = EINVAL; goto failed; }
        return fd;
    }
    if (errno != ENOENT || !create) goto failed;
    store->operation = "check free space";
    if (statvfs( directory, &space )) goto failed;
    if ((uint64_t)space.f_bavail * space.f_frsize < size + HEADER_SIZE + 64 * UINT64_C(1048576))
    { errno = ENOSPC; goto failed; }
    snprintf( temp, sizeof(temp), "%s.creating", path );
    store->operation = "allocate write buffer";
    if (!(zeros = calloc( 1, PREALLOC_CHUNK ))) { errno = ENOMEM; goto failed; }
    store->operation = "create segment";
    fd = open( temp, O_RDWR | O_CREAT | O_EXCL, 0600 );
    if (fd < 0) goto failed;
    created = 1;
    store->operation = "write header";
    if (transfer( fd, expected, sizeof(expected), 0, 1 )) goto failed;
    store->operation = "allocate segment";
    for (done = 0; done < size; )
    {
        size_t chunk = size - done < PREALLOC_CHUNK ? (size_t)(size - done) : PREALLOC_CHUNK;
        store->offset = before + done;
        if (progress && !progress( context, "Allocating SD swap", before + done, total ))
        { errno = ECANCELED; goto failed; }
        if (transfer( fd, zeros, chunk, HEADER_SIZE + done, 1 )) goto failed;
        done += chunk;
    }
    store->offset = before + size;
    store->operation = "flush segment";
    if (fsync( fd )) goto failed;
    store->operation = "close segment";
    error = close( fd );
    fd = -1;
    if (error) goto failed;
    store->operation = "rename segment";
    if (rename( temp, path )) goto failed;
    created = 0;
    store->operation = "reopen segment";
    fd = open( path, O_RDWR );
    if (fd < 0) goto failed;
    free( zeros );
    return fd;
failed:
    error = errno;
    save_error( store );
    if (fd >= 0) close( fd );
    if (created) unlink( temp );
    free( zeros );
    errno = error;
    return -1;
}

static int open_store( struct swap_store *store, const char *directory, unsigned int megabytes,
                       swap_progress progress, void *context, int create )
{
    uint64_t left, before = 0, needed = 64 * UINT64_C(1048576);
    struct statvfs space;
    char path[768];
    unsigned int i;
    int error;

    memset( store, 0, sizeof(*store) );
    for (i = 0; i < SWAP_STORE_FILES; i++) store->fd[i] = -1;
    store->operation = "check size";
    if (!megabytes || megabytes > 8192) { errno = EINVAL; goto failed; }
    store->operation = "create directory";
    if (create && mkdir( directory, 0700 ) && errno != EEXIST) goto failed;
    store->size = (uint64_t)megabytes * 1048576;
    for (i = 0, left = store->size; create && left; i++)
    {
        struct stat st;
        uint64_t size = left < SWAP_STORE_SEGMENT ? left : SWAP_STORE_SEGMENT;
        store->operation = "stat segment";
        store->offset = store->size - left;
        if (snprintf( path, sizeof(path), "%s/part-%u.swap", directory, i ) >= (int)sizeof(path))
        { errno = ENAMETOOLONG; goto failed; }
        if (stat( path, &st ))
        {
            if (errno != ENOENT) goto failed;
            needed += size + HEADER_SIZE;
        }
        left -= size;
    }
    store->operation = "check free space";
    store->offset = 0;
    if (create && statvfs( directory, &space )) goto failed;
    if (create && (uint64_t)space.f_bavail * space.f_frsize < needed) { errno = ENOSPC; goto failed; }
    for (i = 0, left = store->size; left; i++)
    {
        uint64_t size = left < SWAP_STORE_SEGMENT ? left : SWAP_STORE_SEGMENT;
        store->fd[i] = open_segment( store, directory, i, size, progress, context, before, store->size, create );
        if (store->fd[i] < 0) goto close_store;
        before += size;
        left -= size;
    }
    store->operation = NULL;
    return 0;
failed:
    save_error( store );
close_store:
    error = errno;
    swap_store_close( store );
    errno = error;
    return -1;
}

int swap_store_open( struct swap_store *store, const char *directory, unsigned int megabytes,
                     swap_progress progress, void *context )
{
    return open_store( store, directory, megabytes, progress, context, 1 );
}

int swap_store_open_existing( struct swap_store *store, const char *directory, unsigned int megabytes )
{
#ifdef __SWITCH__
    unsigned char expected[HEADER_SIZE], found[HEADER_SIZE];
    char path[768], translated[FS_MAX_PATH];
    FsFileSystem *device, isolated;
    uint64_t left, got;
    unsigned int i;
    int64_t length;
    Result rc;
    memset( store, 0, sizeof(*store) );
    for (i = 0; i < SWAP_STORE_FILES; i++) store->fd[i] = -1;
    store->operation = "check size";
    if (!megabytes || megabytes > 8192) { errno = EINVAL; return -1; }
    store->size = (uint64_t)megabytes * 1048576;
    store->operation = "clone filesystem session";
    if (R_FAILED(rc = serviceClone( fsGetServiceSession(), &store->io_session ))) goto failed;
    for (i = 0, left = store->size; left; i++)
    {
        uint64_t size = left < SWAP_STORE_SEGMENT ? left : SWAP_STORE_SEGMENT;
        store->operation = "open native segment";
        if (snprintf( path, sizeof(path), "%s/part-%u.swap", directory, i ) >= (int)sizeof(path))
        { errno = ENAMETOOLONG; goto close_store; }
        if (fsdevTranslatePath( path, &device, translated )) goto close_store;
        if (device->s.session != fsGetServiceSession()->session || !device->s.object_id)
        { errno = ENOTSUP; goto close_store; }
        isolated = *device;
        isolated.s.session = store->io_session.session;
        isolated.s.own_handle = 0;
        if (R_FAILED(rc = fsFsOpenFile( &isolated, translated, FsOpenMode_Read | FsOpenMode_Write,
                                        &store->files[i] ))) goto failed;
        store->operation = "validate native segment";
        if (R_FAILED(rc = fsFileGetSize( &store->files[i], &length ))) goto failed;
        if ((uint64_t)length != size + HEADER_SIZE) { errno = EINVAL; goto close_store; }
        if (R_FAILED(rc = fsFileRead( &store->files[i], 0, found, sizeof(found), 0, &got ))) goto failed;
        header( expected, i, size );
        if (got != sizeof(found) || memcmp( found, expected, sizeof(found) ))
        { errno = EINVAL; goto close_store; }
        left -= size;
    }
    store->operation = NULL;
    return 0;
failed:
    store->fs_error = rc;
    errno = EIO;
close_store:
    {
        int error = errno;
        swap_store_close( store );
        errno = error;
    }
    return -1;
#else
    return open_store( store, directory, megabytes, NULL, NULL, 0 );
#endif
}

#ifdef __SWITCH__
extern __thread unsigned int wine_nx_swap_native_io;

static int native_transfer( struct swap_store *store, unsigned int index, uint64_t offset,
                            void *data, size_t size, int writing )
{
    unsigned char ipc[0x100];
    Result rc = 0;
    /* A page fault may interrupt construction of another IPC request. */
    memcpy( ipc, armGetTls(), sizeof(ipc) );
    /* Only resident pager buffers use this session; avoid recursive IPC pinning. */
    wine_nx_swap_native_io++;
    while (size)
    {
        uint64_t count = size;
        rc = writing ? fsFileWrite( &store->files[index], offset, data, size, 0 ) :
                       fsFileRead( &store->files[index], offset, data, size, 0, &count );
        if (R_FAILED(rc) || !count) break;
        offset += count;
        data = (char *)data + count;
        size -= count;
    }
    wine_nx_swap_native_io--;
    memcpy( armGetTls(), ipc, sizeof(ipc) );
    if (!size) return 0;
    store->fs_error = rc;
    errno = EIO;
    return -1;
}
#endif

static int store_transfer( struct swap_store *store, uint64_t offset, void *data, size_t size, int writing )
{
    if (offset > store->size || size > store->size - offset) { errno = EINVAL; return -1; }
    while (size)
    {
        unsigned int index = offset / SWAP_STORE_SEGMENT;
        uint64_t at = offset % SWAP_STORE_SEGMENT;
        size_t chunk = size < SWAP_STORE_SEGMENT - at ? size : (size_t)(SWAP_STORE_SEGMENT - at);
#ifdef __SWITCH__
        if (serviceIsActive( &store->io_session ))
        {
            if (native_transfer( store, index, HEADER_SIZE + at, data, chunk, writing )) return -1;
        }
        else
#endif
        if (transfer( store->fd[index], data, chunk, HEADER_SIZE + at, writing )) return -1;
        offset += chunk;
        data = (char *)data + chunk;
        size -= chunk;
    }
    return 0;
}

int swap_store_read( struct swap_store *store, uint64_t offset, void *data, size_t size )
{
    return store_transfer( store, offset, data, size, 0 );
}

int swap_store_write( struct swap_store *store, uint64_t offset, const void *data, size_t size )
{
    return store_transfer( store, offset, (void *)data, size, 1 );
}

void swap_store_close( struct swap_store *store )
{
    unsigned int i;
    for (i = 0; i < SWAP_STORE_FILES; i++)
    {
        if (store->fd[i] >= 0) close( store->fd[i] );
        store->fd[i] = -1;
#ifdef __SWITCH__
        if (serviceIsActive( &store->files[i].s )) fsFileClose( &store->files[i] );
#endif
    }
#ifdef __SWITCH__
    if (serviceIsActive( &store->io_session )) serviceClose( &store->io_session );
#endif
    store->size = 0;
}

int swap_store_remove( const char *directory )
{
    char paths[SWAP_STORE_FILES * 2][780];
    unsigned int i, count = 0;
    for (i = 0; i < SWAP_STORE_FILES * 2; i++)
    {
        unsigned char found[HEADER_SIZE + 1], expected[HEADER_SIZE];
        unsigned int index;
        unsigned long long size;
        struct stat st;
        int fd, valid;
        if (snprintf( paths[count], sizeof(paths[count]), "%s/part-%u.swap%s", directory,
                      i / 2, (i & 1) ? ".creating" : "" ) >= (int)sizeof(paths[count]))
        { errno = ENAMETOOLONG; return -1; }
        fd = open( paths[count], O_RDONLY );
        if (fd < 0) { if (errno == ENOENT) continue; return -1; }
        valid = !fstat( fd, &st ) && st.st_size >= HEADER_SIZE &&
                !transfer( fd, found, HEADER_SIZE, 0, 0 );
        found[HEADER_SIZE] = 0;
        valid = valid && sscanf( (char *)found, "Autorun swap POC 1\n%u\n%llu\n", &index, &size ) == 2 &&
                index == i / 2 && size && size <= SWAP_STORE_SEGMENT;
        if (valid)
        {
            header( expected, index, size );
            valid = !memcmp( found, expected, HEADER_SIZE ) &&
                    (uint64_t)st.st_size <= size + HEADER_SIZE;
        }
        close( fd );
        if (!valid) { errno = EINVAL; return -1; }
        count++;
    }
    for (i = 0; i < count; i++) if (unlink( paths[i] )) return -1;
    return 0;
}
