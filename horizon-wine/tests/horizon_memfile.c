/* Sections with no file (horizon_memfile.h) against a host stand-in for the
 * Horizon kernel (horizon_memfile_host.h), which really shares pages between
 * addresses: views that overlap see each other's writes at once, a view keeps
 * the memory after the descriptor is closed, split view ranges keep their
 * anchors, unused anchors go and come back with the data, refused kernel calls
 * leave nothing behind, reads and writes at an offset ignore the shared
 * position, and committed ranges follow wineserver. */
#include <pthread.h>
#include <stdio.h>
#include "horizon_memfile_host.h"
#include "../../dlls/ntdll/unix/horizon_memfile.h"

#define P HOST_PAGE

static void *test_anchor( void *source, size_t size, void **token )
{
    *token = (void *)0x5ec7;
    return host_anchor( NULL, source, size );
}

static int test_unanchor( void *addr, void *source, size_t size, void *token )
{
    assert( token == (void *)0x5ec7 );
    return host_unanchor( addr, source, size );
}

static const struct horizon_memfile_ops test_ops = { test_anchor, test_unanchor, host_alias, host_unalias };

static unsigned char pattern( size_t offset, unsigned char seed )
{
    return (unsigned char)(offset * 131 + seed);
}

static void fill( unsigned char *ptr, size_t offset, size_t size, unsigned char seed )
{
    size_t i;

    for (i = 0; i < size; i++) ptr[i] = pattern( offset + i, seed );
}

static int holds( const unsigned char *ptr, size_t offset, size_t size, unsigned char seed )
{
    size_t i;

    for (i = 0; i < size; i++) if (ptr[i] != pattern( offset + i, seed )) return 0;
    return 1;
}

static struct horizon_memfile_anchor *anchor_at( struct horizon_memfile *file, size_t page )
{
    struct horizon_memfile_anchor *anchor;

    for (anchor = file->anchors; anchor; anchor = anchor->next)
        if (anchor->first <= page && page < anchor->first + anchor->count) return anchor;
    return NULL;
}

static void check_flags(void)
{
    unsigned int status;

    status = 0;
    assert( horizon_anonymous_section_flags( HORIZON_SEC_COMMIT, &status ) == HORIZON_SEC_COMMIT && !status );
    assert( horizon_anonymous_section_flags( HORIZON_SEC_COMMIT | HORIZON_SEC_LARGE_PAGES, &status ) ==
            (HORIZON_SEC_COMMIT | HORIZON_SEC_LARGE_PAGES) && !status );
    assert( horizon_anonymous_section_flags( HORIZON_SEC_RESERVE, &status ) == HORIZON_SEC_RESERVE && !status );
    assert( !horizon_anonymous_section_flags( HORIZON_SEC_RESERVE | HORIZON_SEC_LARGE_PAGES, &status ) &&
            status == HORIZON_STATUS_INVALID_PARAMETER );
    status = 0;
    assert( !horizon_anonymous_section_flags( HORIZON_SEC_IMAGE, &status ) &&
            status == HORIZON_STATUS_INVALID_FILE_FOR_SECTION );
    status = 0;
    assert( !horizon_anonymous_section_flags( HORIZON_SEC_IMAGE | HORIZON_SEC_LARGE_PAGES, &status ) &&
            status == HORIZON_STATUS_INVALID_PARAMETER );
    status = 0;
    assert( !horizon_anonymous_section_flags( 0, &status ) && status == HORIZON_STATUS_INVALID_PARAMETER );
    status = 0;
    assert( !horizon_anonymous_section_flags( HORIZON_SEC_COMMIT | HORIZON_SEC_RESERVE, &status ) &&
            status == HORIZON_STATUS_INVALID_PARAMETER );
    status = 0;
    assert( !horizon_anonymous_section_flags( HORIZON_SEC_FILE | HORIZON_SEC_COMMIT, &status ) &&
            status == HORIZON_STATUS_INVALID_PARAMETER );
}

/* Two overlapping views, the descriptor closed before they are unmapped, a
 * view range split around a PROT_NONE page and mapped again, and the anchors
 * released with the last user and made again with the data intact. */
static void check_shared_views(void)
{
    struct horizon_memfile *file = horizon_memfile_alloc( 8 * P, 0, &test_ops );
    unsigned char *a = host_reserve( 4 * P ), *b = host_reserve( 4 * P ), *c = host_reserve( 2 * P );
    unsigned char buffer[64];

    assert( file && host_pages_live == 1 );
    assert( horizon_memfile_pwrite( file, (const char *)"early", 5, 2 * P ) == 5 );

    /* View a: pages 1-4. */
    horizon_memfile_ref( file );
    assert( !horizon_memfile_map( file, a, 1 * P, 4 * P ) );
    assert( host_live_anchors() == 1 && anchor_at( file, 1 )->users == 1 );
    assert( !memcmp( a + P, "early", 5 ) );
    fill( a, 1 * P, 4 * P, 1 );

    /* The section's own pages are locked: reading goes through the anchor. */
    assert( horizon_memfile_pread( file, (char *)buffer, sizeof(buffer), 3 * P ) == sizeof(buffer) );
    assert( holds( buffer, 3 * P, sizeof(buffer), 1 ) );

    /* View b: pages 3-6, over a's pages 3-4 and two new ones. */
    horizon_memfile_ref( file );
    assert( !horizon_memfile_map( file, b, 3 * P, 4 * P ) );
    assert( host_live_anchors() == 2 && anchor_at( file, 3 )->users == 2 && anchor_at( file, 5 )->users == 1 );
    assert( holds( b, 3 * P, 2 * P, 1 ) );
    b[7] = 0xa5;
    assert( a[2 * P + 7] == 0xa5 );
    a[3 * P + 9] = 0x5a;
    assert( b[P + 9] == 0x5a );
    assert( horizon_memfile_pwrite( file, (const char *)"through the descriptor", 22, 5 * P ) == 22 );
    assert( !memcmp( b + 2 * P, "through the descriptor", 22 ) );

    /* The descriptor goes first; the views keep the memory. */
    horizon_memfile_unref( file );
    b[P + 1] = 0x77;
    assert( a[3 * P + 1] == 0x77 );

    /* Page 2 of a becomes PROT_NONE, as horizon.c splits a range: the pieces
     * count first, then the middle is unmapped, then the whole stops counting. */
    horizon_memfile_use( file, 1 * P, 1 * P, 1 );
    horizon_memfile_use( file, 3 * P, 2 * P, 1 );
    assert( !horizon_memfile_alias( file, a + P, 2 * P, 1 * P, 0 ) );
    horizon_memfile_use( file, 1 * P, 4 * P, -1 );
    /* Pages 1-4 are one anchor: the left and right pieces and b use it. */
    assert( host_live_anchors() == 2 && anchor_at( file, 2 ) == anchor_at( file, 3 ) &&
            anchor_at( file, 2 )->users == 3 && anchor_at( file, 5 )->users == 1 );
    a[2 * P] = 0x42;
    assert( b[0] == 0x42 );

    /* ... and mapped again, still the same memory. */
    assert( !horizon_memfile_map( file, a + P, 2 * P, 1 * P ) );
    assert( anchor_at( file, 2 )->users == 4 );
    assert( holds( a + P, 2 * P, P, 1 ) );
    a[P + 3] = 0x33;
    assert( horizon_memfile_pread( file, (char *)buffer, 1, 2 * P + 3 ) == 1 && buffer[0] == 0x33 );

    /* b goes: pages 5-6 lose their last user and their anchor; the data stays. */
    assert( !horizon_memfile_alias( file, b, 3 * P, 4 * P, 0 ) );
    horizon_memfile_use( file, 3 * P, 4 * P, -1 );
    horizon_memfile_unref( file );
    assert( host_live_anchors() == 1 && !anchor_at( file, 5 ) );
    assert( horizon_memfile_pread( file, (char *)buffer, 22, 5 * P ) == 22 );
    assert( !memcmp( buffer, "through the descriptor", 22 ) );

    /* A new view over those pages anchors them again with the data. */
    horizon_memfile_ref( file );
    assert( !horizon_memfile_map( file, c, 5 * P, 2 * P ) );
    assert( !memcmp( c, "through the descriptor", 22 ) );
    assert( !horizon_memfile_alias( file, c, 5 * P, 2 * P, 0 ) );
    horizon_memfile_use( file, 5 * P, 2 * P, -1 );
    horizon_memfile_unref( file );

    /* a's three pieces go; the last reference frees the memory. */
    assert( !horizon_memfile_alias( file, a, 1 * P, 1 * P, 0 ) );
    horizon_memfile_use( file, 1 * P, 1 * P, -1 );
    assert( !horizon_memfile_alias( file, a + P, 2 * P, 1 * P, 0 ) );
    horizon_memfile_use( file, 2 * P, 1 * P, -1 );
    assert( !horizon_memfile_alias( file, a + 2 * P, 3 * P, 2 * P, 0 ) );
    horizon_memfile_use( file, 3 * P, 2 * P, -1 );
    assert( !host_live_anchors() && !host_alias_count && host_pages_live == 1 );
    horizon_memfile_unref( file );
    assert( host_pages_live == 0 );

    munmap( a, 4 * P );
    munmap( b, 4 * P );
    munmap( c, 2 * P );
}

/* The kernel refuses an anchor, then an alias halfway through a view. */
static void check_refusals(void)
{
    struct horizon_memfile *file = horizon_memfile_alloc( 4 * P, 0, &test_ops );
    unsigned char *small = host_reserve( P ), *big = host_reserve( 3 * P );

    assert( file );
    host_fail_anchor = 1;
    errno = 0;
    assert( horizon_memfile_map( file, big, 0, 3 * P ) == -1 && errno == ENOMEM );
    assert( !host_live_anchors() && !host_alias_count && !file->anchors );
    host_fail_anchor = 0;

    /* Page 1 is anchored and used by a small view; a view of pages 0-2 needs
     * a new anchor on each side of it and fails on its second piece. */
    assert( !horizon_memfile_map( file, small, 1 * P, 1 * P ) );
    host_fail_alias_countdown = 2;
    errno = 0;
    assert( horizon_memfile_map( file, big, 0, 3 * P ) == -1 && errno == ENOMEM );
    host_fail_alias_countdown = 0;
    assert( host_live_anchors() == 1 && anchor_at( file, 1 )->users == 1 && host_alias_count == 1 );

    /* A range outside the section. */
    errno = 0;
    assert( horizon_memfile_map( file, big, 2 * P, 3 * P ) == -1 && errno == EINVAL );
    assert( horizon_memfile_map( file, big, 1, P ) == -1 );

    /* The kernel keeps an anchor: its pages stay locked and are never freed,
     * but still read and write through it. */
    host_fail_unanchor = 1;
    assert( !horizon_memfile_alias( file, small, 1 * P, 1 * P, 0 ) );
    horizon_memfile_use( file, 1 * P, 1 * P, -1 );
    assert( host_live_anchors() == 1 && anchor_at( file, 1 ) && !anchor_at( file, 1 )->users );
    assert( horizon_memfile_pwrite( file, (const char *)"kept", 4, P ) == 4 );
    host_fail_unanchor = 0;
    horizon_memfile_use( file, 1 * P, 1 * P, -1 );  /* trims it now */
    assert( !host_live_anchors() );
    horizon_memfile_unref( file );
    assert( host_pages_live == 0 );

    munmap( small, P );
    munmap( big, 3 * P );
}

struct racer { struct horizon_memfile *file; unsigned char seed; off_t offset; int failures; };

static void *race_offsets( void *arg )
{
    struct racer *racer = arg;
    unsigned char out[128], in[128];
    int i;

    for (i = 0; i < 20000; i++)
    {
        fill( out, racer->offset + i, sizeof(out), racer->seed );
        if (horizon_memfile_pwrite( racer->file, (const char *)out, sizeof(out), racer->offset ) != sizeof(out) ||
            horizon_memfile_pread( racer->file, (char *)in, sizeof(in), racer->offset ) != sizeof(in) ||
            memcmp( in, out, sizeof(in) ))
            racer->failures++;
    }
    return NULL;
}

static void *race_cursor( void *arg )
{
    struct racer *racer = arg;
    char buffer[16];
    int i;

    for (i = 0; i < 20000; i++)
    {
        horizon_memfile_seek( racer->file, (i * 7919) % (4 * P), SEEK_SET );
        horizon_memfile_read( racer->file, buffer, sizeof(buffer) );
    }
    return NULL;
}

/* Offsets, the shared position, the end of the section, and the size. */
static void check_descriptor(void)
{
    struct horizon_memfile *file = horizon_memfile_alloc( 4 * P, 0, &test_ops );
    struct racer one = { file, 1, 0, 0 }, two = { file, 2, 2 * P, 0 }, cursor = { file, 0, 0, 0 };
    pthread_t threads[3];
    unsigned char tail[64];
    struct stat st;

    assert( file );
    pthread_create( &threads[0], NULL, race_offsets, &one );
    pthread_create( &threads[1], NULL, race_offsets, &two );
    pthread_create( &threads[2], NULL, race_cursor, &cursor );
    for (int i = 0; i < 3; i++) pthread_join( threads[i], NULL );
    assert( !one.failures && !two.failures );

    assert( horizon_memfile_seek( file, 0, SEEK_SET ) == 0 );
    assert( horizon_memfile_write( file, "abc", 3 ) == 3 && file->pos == 3 );
    assert( horizon_memfile_seek( file, -3, SEEK_CUR ) == 0 );
    assert( horizon_memfile_read( file, (char *)tail, 3 ) == 3 && !memcmp( tail, "abc", 3 ) && file->pos == 3 );
    assert( horizon_memfile_pread( file, (char *)tail, 3, 0 ) == 3 && file->pos == 3 );

    assert( horizon_memfile_seek( file, -10, SEEK_END ) == (off_t)(4 * P - 10) );
    assert( horizon_memfile_read( file, (char *)tail, sizeof(tail) ) == 10 );
    assert( !horizon_memfile_read( file, (char *)tail, sizeof(tail) ) );
    assert( horizon_memfile_write( file, (const char *)tail, 1 ) == -ENOSPC );
    assert( horizon_memfile_pwrite( file, (const char *)tail, sizeof(tail), 4 * P - 10 ) == 10 );
    assert( horizon_memfile_pread( file, (char *)tail, 1, -1 ) == -EINVAL );
    assert( horizon_memfile_seek( file, -1, SEEK_SET ) == -EINVAL );
    assert( horizon_memfile_seek( file, 0, 42 ) == -EINVAL );
    assert( horizon_memfile_seek( file, (off_t)(~0ull >> 1), SEEK_END ) == -EINVAL );

    assert( !horizon_memfile_truncate( file, 4 * P ) && horizon_memfile_truncate( file, P ) == -EINVAL );
    horizon_memfile_stat( file, &st );
    assert( S_ISREG( st.st_mode ) && st.st_size == (off_t)(4 * P) );
    horizon_memfile_unref( file );

    /* A copy of a file ends where the file did, inside its last page. */
    assert( (file = horizon_memfile_alloc( 2 * P, 0, &test_ops )) );
    file->length = P + 5;
    assert( horizon_memfile_pread( file, (char *)tail, sizeof(tail), P ) == 5 );
    assert( !horizon_memfile_pread( file, (char *)tail, sizeof(tail), P + 5 ) );
    assert( horizon_memfile_seek( file, 0, SEEK_END ) == (off_t)(P + 5) );
    assert( !horizon_memfile_read( file, (char *)tail, sizeof(tail) ) );
    horizon_memfile_stat( file, &st );
    assert( st.st_size == (off_t)(P + 5) );
    horizon_memfile_unref( file );

    errno = 0;
    assert( !horizon_memfile_alloc( 0, 0, &test_ops ) && errno == EINVAL );
    assert( !horizon_memfile_alloc( P + 1, 0, &test_ops ) );
    assert( host_pages_live == 0 );
}

/* wineserver's committed ranges, per view of a SEC_RESERVE section. */
static void check_committed(void)
{
    struct horizon_memfile *file = horizon_memfile_alloc( 8 * P, 1, &test_ops );
    unsigned long long size;
    int committed;

    assert( file );
    /* A view of pages 2-5. */
    assert( !horizon_memfile_committed_range( file, 2 * P, 4 * P, 0, &size, &committed ) );
    assert( !committed && size == 4 * P );
    assert( !horizon_memfile_add_committed( file, 2 * P, 4 * P, 1 * P, 1 * P ) );
    assert( !horizon_memfile_committed_range( file, 2 * P, 4 * P, 0, &size, &committed ) && !committed && size == P );
    assert( !horizon_memfile_committed_range( file, 2 * P, 4 * P, P, &size, &committed ) && committed && size == P );
    assert( !horizon_memfile_committed_range( file, 2 * P, 4 * P, 2 * P, &size, &committed ) &&
            !committed && size == 2 * P );
    assert( horizon_memfile_committed_range( file, 2 * P, 4 * P, 1, &size, &committed ) ==
            HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_memfile_committed_range( file, 2 * P, 4 * P, 4 * P, &size, &committed ) ==
            HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_memfile_add_committed( file, 2 * P, 4 * P, 3 * P, 2 * P ) == HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_memfile_add_committed( file, 2 * P, 4 * P, P, 0 ) == HORIZON_STATUS_INVALID_PARAMETER );
    assert( horizon_memfile_add_committed( file, 2 * P, 4 * P, 1, P ) == HORIZON_STATUS_INVALID_PARAMETER );

    /* Another view of the whole section sees the same commitment. */
    assert( !horizon_memfile_committed_range( file, 0, 8 * P, 0, &size, &committed ) && !committed && size == 3 * P );
    assert( !horizon_memfile_committed_range( file, 0, 8 * P, 3 * P, &size, &committed ) && committed && size == P );
    assert( !horizon_memfile_add_committed( file, 0, 8 * P, 4 * P, 4 * P ) );
    assert( !horizon_memfile_committed_range( file, 2 * P, 4 * P, P, &size, &committed ) && committed && size == 3 * P );
    horizon_memfile_unref( file );

    /* SEC_COMMIT: all of it, to the end of the view. */
    assert( (file = horizon_memfile_alloc( 4 * P, 0, &test_ops )) );
    assert( !horizon_memfile_committed_range( file, P, 2 * P, P, &size, &committed ) && committed && size == P );
    assert( !horizon_memfile_add_committed( file, P, 2 * P, 0, P ) );
    horizon_memfile_unref( file );
    assert( host_pages_live == 0 );
}

int main(void)
{
    check_flags();
    check_shared_views();
    check_refusals();
    check_descriptor();
    check_committed();
    printf( "Section memory (%zu KiB pages): flags, overlapping views, close before unmap, split ranges, anchor release, "
            "refused kernel calls, offsets against the shared position and committed ranges passed\n", P / 1024 );
    return 0;
}
