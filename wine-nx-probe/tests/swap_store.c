#define _XOPEN_SOURCE 700
#include "../source/swap_store.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_flush, fail_rename;
static unsigned int renames;
int __real_rename( const char *old_path, const char *new_path );
int __real_fsync( int fd );

int __wrap_rename( const char *old_path, const char *new_path )
{
    struct stat source, open_file;
    int fd, error = errno;
    assert( !stat( old_path, &source ) );
    /* Horizon does not allow renaming an open file. */
    for (fd = 0; fd < 256; fd++)
        if (!fstat( fd, &open_file ))
            assert( open_file.st_dev != source.st_dev || open_file.st_ino != source.st_ino );
    renames++;
    if (fail_rename) { errno = EIO; return -1; }
    errno = error;
    return __real_rename( old_path, new_path );
}

int __wrap_fsync( int fd )
{
    if (fail_flush) { errno = EIO; return -1; }
    return __real_fsync( fd );
}

static int cancel( void *context, const char *phase, uint64_t done, uint64_t total )
{
    (void)context;
    (void)phase;
    (void)total;
    return !done;
}

int main(void)
{
    char directory[512], path[768];
    unsigned char out[8192], in[8192];
    struct swap_store store;
    struct stat st;
    unsigned int i;
    int fd;

    snprintf( directory, sizeof(directory), "%s/autorun-swap-test.XXXXXX",
              getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" );
    assert( mkdtemp( directory ) );
    assert( !swap_store_open( &store, directory, 4, NULL, NULL ) );
    assert( renames == 1 && !store.operation );
    assert( !fstat( store.fd[0], &st ) );
    assert( st.st_size == 4 * 1048576 + 4096 );
#ifndef __CYGWIN__
    assert( (uint64_t)st.st_blocks * 512 >= (uint64_t)st.st_size );
#endif
    for (i = 0; i < sizeof(out); i++) out[i] = (i * 19) ^ (i >> 3);
    assert( !swap_store_write( &store, store.size - sizeof(out), out, sizeof(out) ) );
    assert( !swap_store_read( &store, store.size - sizeof(in), in, sizeof(in) ) );
    assert( !memcmp( in, out, sizeof(in) ) );
    assert( swap_store_write( &store, store.size - 1, out, sizeof(out) ) == -1 );
    assert( swap_store_read( &store, UINT64_MAX, in, sizeof(in) ) == -1 );
    swap_store_close( &store );
    assert( !swap_store_open( &store, directory, 4, NULL, NULL ) );
    assert( !swap_store_read( &store, store.size - sizeof(in), in, sizeof(in) ) );
    assert( !memcmp( in, out, sizeof(in) ) );
    swap_store_close( &store );
    assert( swap_store_open( &store, directory, 8, NULL, NULL ) == -1 && errno == EINVAL );
    assert( !swap_store_remove( directory ) );
    assert( swap_store_open( &store, directory, 4, cancel, NULL ) == -1 && errno == ECANCELED );
    snprintf( path, sizeof(path), "%s/part-0.swap.creating", directory );
    assert( access( path, F_OK ) == -1 && errno == ENOENT );
    snprintf( path, sizeof(path), "%s/part-0.swap", directory );
    fd = open( path, O_CREAT | O_EXCL | O_WRONLY, 0600 );
    assert( fd >= 0 && write( fd, "not swap", 8 ) == 8 && !close( fd ) );
    assert( swap_store_remove( directory ) == -1 && errno == EINVAL );
    assert( !stat( path, &st ) && st.st_size == 8 );
    assert( !unlink( path ) );

    fail_flush = 1;
    assert( swap_store_open( &store, directory, 4, NULL, NULL ) == -1 && errno == EIO );
    assert( !strcmp( store.operation, "flush segment" ) && store.offset == 4 * 1048576 );
    assert( !store.fs_error && store.fd[0] == -1 && !store.size );
    fail_flush = 0;
    fail_rename = 1;
    assert( swap_store_open( &store, directory, 4, NULL, NULL ) == -1 && errno == EIO );
    assert( !strcmp( store.operation, "rename segment" ) && store.fd[0] == -1 );
    fail_rename = 0;
    assert( access( path, F_OK ) == -1 && errno == ENOENT );
    snprintf( path, sizeof(path), "%s/part-0.swap.creating", directory );
    assert( access( path, F_OK ) == -1 && errno == ENOENT );
    fd = open( path, O_CREAT | O_EXCL | O_WRONLY, 0600 );
    assert( fd >= 0 && write( fd, "keep", 4 ) == 4 && !close( fd ) );
    assert( swap_store_open( &store, directory, 4, NULL, NULL ) == -1 && errno == EEXIST );
    assert( !strcmp( store.operation, "create segment" ) );
    assert( !stat( path, &st ) && st.st_size == 4 && !unlink( path ) );
    assert( !swap_store_open( &store, directory, 4, NULL, NULL ) );
    swap_store_close( &store );
    assert( !swap_store_remove( directory ) );

    /* Sparse fixtures exercise the FAT32-safe segment boundary without a 2 GiB test write. */
    memset( &store, 0xff, sizeof(store) );
    store.size = SWAP_STORE_SEGMENT * 2;
    for (i = 0; i < 2; i++)
    {
        snprintf( path, sizeof(path), "%s/boundary-%u", directory, i );
        store.fd[i] = open( path, O_RDWR | O_CREAT | O_EXCL, 0600 );
        assert( store.fd[i] >= 0 && !unlink( path ) );
        assert( !ftruncate( store.fd[i], SWAP_STORE_SEGMENT + 4096 ) );
    }
    assert( !swap_store_write( &store, SWAP_STORE_SEGMENT - 4096, out, sizeof(out) ) );
    assert( !swap_store_read( &store, SWAP_STORE_SEGMENT - 4096, in, sizeof(in) ) );
    assert( !memcmp( in, out, sizeof(in) ) );
    assert( !ftruncate( store.fd[1], 4096 ) );
    assert( swap_store_read( &store, SWAP_STORE_SEGMENT, in, sizeof(in) ) == -1 && errno == EIO );
    swap_store_close( &store );
    assert( !rmdir( directory ) );
    puts( "swap store: preallocation, closed-file rename, failure cleanup, reuse, cancellation, ownership and segment boundaries passed" );
    return 0;
}
