/*
 * Memory behind sections that have no file
 *
 * wineserver backs a section created without a file (CreateFileMapping with
 * INVALID_HANDLE_VALUE) with a temporary file that every view maps shared.
 * Here the section is memory, and its views show that same memory: Horizon's
 * svcMapProcessMemory maps pages that are already code-mapped at a second
 * address. Pages of the section get code-mapped once, as an anchor, while
 * views use them, and each view maps its pages from the anchors, so a write
 * through one view is in every other at once. The kernel locks the section's
 * own pages while they are anchored, so reads and writes through its
 * descriptor go through the anchors there. Anchors come and go with the view
 * ranges that use them, which leaves a 32-bit process its address space when
 * views are unmapped, as DXVK intends when it unmaps what it does not need.
 *
 * horizon.c supplies the kernel operations; host tests supply stand-ins.
 */

#ifndef __WINE_HORIZON_MEMFILE_H
#define __WINE_HORIZON_MEMFILE_H

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef HORIZON_MEMFILE_PAGE_SIZE
#define HORIZON_MEMFILE_PAGE_SIZE ((size_t)0x1000)
#endif
#ifndef HORIZON_MEMFILE_ALLOC_PAGES
#include <malloc.h>
#define HORIZON_MEMFILE_ALLOC_PAGES(size) memalign( HORIZON_MEMFILE_PAGE_SIZE, (size) )
#define HORIZON_MEMFILE_FREE_PAGES(ptr, size) free( (ptr) )
#endif

#ifndef HORIZON_SEC_IMAGE
#define HORIZON_SEC_IMAGE 0x01000000u
#endif
#define HORIZON_SEC_FILE         0x00800000u
#define HORIZON_SEC_RESERVE      0x04000000u
#define HORIZON_SEC_COMMIT       0x08000000u
#define HORIZON_SEC_WRITECOMBINE 0x40000000u
#define HORIZON_SEC_LARGE_PAGES  0x80000000u

#ifndef HORIZON_STATUS_INVALID_PARAMETER
#define HORIZON_STATUS_INVALID_PARAMETER 0xc000000du
#endif
#define HORIZON_STATUS_INVALID_FILE_FOR_SECTION 0xc0000020u

struct horizon_memfile_ops
{
    /* Code-maps size bytes at source, which the kernel then locks, at a new
     * address; returns it and a token for unanchor, or NULL with errno set. */
    void *(*anchor)( void *source, size_t size, void **token );
    int (*unanchor)( void *addr, void *source, size_t size, void *token );
    /* Maps size bytes of an anchor at dst as well, or removes them from dst;
     * 0, or -1 with errno set. */
    int (*alias)( void *dst, void *src, size_t size );
    int (*unalias)( void *dst, void *src, size_t size );
};

struct horizon_memfile_anchor
{
    struct horizon_memfile_anchor *next;  /* in page order; anchors never overlap */
    size_t first;                         /* in pages of the section */
    size_t count;
    void *addr;
    void *token;
    unsigned int users;                   /* aliased view ranges using these pages */
};

struct horizon_memfile_piece
{
    unsigned char *data;
    size_t offset, size;
};

struct horizon_memfile
{
    pthread_mutex_t mutex;
    const struct horizon_memfile_ops *ops;
    unsigned char *data;       /* the kernel locks the pages that are anchored */
    struct horizon_memfile_piece *pieces;
    size_t piece_count;
    size_t size;               /* whole pages */
    off_t pos;                 /* shared by the descriptors duplicated from one */
    unsigned int refs;         /* its descriptor, views the server records, view ranges */
    struct horizon_memfile_anchor *anchors;
    unsigned char *committed;  /* SEC_RESERVE: a byte per page, set once committed */
};

static inline void horizon_memfile_release_pages( struct horizon_memfile *file )
{
    size_t i;

    if (file->data) HORIZON_MEMFILE_FREE_PAGES( file->data, file->size );
    for (i = 0; i < file->piece_count; i++)
        if (file->pieces[i].data)
            HORIZON_MEMFILE_FREE_PAGES( file->pieces[i].data, file->pieces[i].size );
    free( file->pieces );
    file->data = NULL;
    file->pieces = NULL;
    file->piece_count = 0;
}

static inline int horizon_memfile_allocate_pages( struct horizon_memfile *file )
{
    const size_t minimum = HORIZON_MEMFILE_PAGE_SIZE;
    size_t piece_size = 2 * 1048576, offset = 0, capacity = 0;

    if ((file->data = HORIZON_MEMFILE_ALLOC_PAGES( file->size )))
    {
        memset( file->data, 0, file->size );
        return 1;
    }
    if (errno != ENOMEM || file->size <= minimum) return 0;
    if (piece_size >= file->size) piece_size = (file->size / 2) & ~(minimum - 1);
    if (piece_size < minimum) piece_size = minimum;
    while (offset < file->size)
    {
        unsigned char *data;
        size_t bytes = file->size - offset;
        if (bytes > piece_size) bytes = piece_size;
        if (file->piece_count == capacity)
        {
            size_t next_capacity = capacity ? capacity * 2 : 8;
            struct horizon_memfile_piece *pieces;
            if (next_capacity > file->size / minimum) next_capacity = file->size / minimum;
            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*pieces)) goto failed;
            if (!(pieces = realloc( file->pieces, next_capacity * sizeof(*pieces) ))) goto failed;
            file->pieces = pieces;
            capacity = next_capacity;
        }
        while (!(data = HORIZON_MEMFILE_ALLOC_PAGES( bytes )))
        {
            if (errno != ENOMEM || bytes <= minimum) goto failed;
            piece_size = (bytes / 2) & ~(minimum - 1);
            if (piece_size < minimum) piece_size = minimum;
            bytes = file->size - offset;
            if (bytes > piece_size) bytes = piece_size;
        }
        file->pieces[file->piece_count++] = (struct horizon_memfile_piece){ data, offset, bytes };
        memset( data, 0, bytes );
        offset += bytes;
    }
    return 1;

failed:
    horizon_memfile_release_pages( file );
    errno = ENOMEM;
    return 0;
}

static inline unsigned char *horizon_memfile_source( struct horizon_memfile *file, size_t offset, size_t *limit )
{
    size_t first = 0, end = file->piece_count;
    const struct horizon_memfile_piece *piece;

    if (file->data)
    {
        if (limit) *limit = file->size;
        return file->data + offset;
    }
    while (first + 1 < end)
    {
        size_t middle = first + (end - first) / 2;
        if (offset < file->pieces[middle].offset) end = middle;
        else first = middle;
    }
    piece = &file->pieces[first];
    if (limit) *limit = piece->offset + piece->size;
    return piece->data + offset - piece->offset;
}

/* The flags wineserver keeps for a section without a file (get_mapping_flags in
 * server/mapping.c), or 0 with *status set to the error it reports. */
static inline unsigned int horizon_anonymous_section_flags( unsigned int flags, unsigned int *status )
{
    switch (flags & (HORIZON_SEC_IMAGE | HORIZON_SEC_RESERVE | HORIZON_SEC_COMMIT | HORIZON_SEC_FILE))
    {
    case HORIZON_SEC_IMAGE:
        if (flags & (HORIZON_SEC_WRITECOMBINE | HORIZON_SEC_LARGE_PAGES)) break;
        *status = HORIZON_STATUS_INVALID_FILE_FOR_SECTION;
        return 0;
    case HORIZON_SEC_COMMIT:
        return flags;
    case HORIZON_SEC_RESERVE:
        if (flags & HORIZON_SEC_LARGE_PAGES) break;
        return flags;
    }
    *status = HORIZON_STATUS_INVALID_PARAMETER;
    return 0;
}

/* Zeroed whole pages, like a new temporary file grown to the section's size;
 * a SEC_RESERVE section starts with nothing committed. The caller holds the
 * one reference. */
static inline struct horizon_memfile *horizon_memfile_alloc( unsigned long long size, int reserve,
                                                             const struct horizon_memfile_ops *ops )
{
    struct horizon_memfile *file;

    if (!size || size % HORIZON_MEMFILE_PAGE_SIZE || size != (size_t)size || (off_t)size < 0)
    {
        errno = EINVAL;
        return NULL;
    }
    if (!(file = calloc( 1, sizeof(*file) )))
    {
        errno = ENOMEM;
        return NULL;
    }
    file->size = size;
    if (!horizon_memfile_allocate_pages( file ) ||
        (reserve && !(file->committed = calloc( size / HORIZON_MEMFILE_PAGE_SIZE, 1 ))))
    {
        horizon_memfile_release_pages( file );
        free( file );
        errno = ENOMEM;
        return NULL;
    }
    pthread_mutex_init( &file->mutex, NULL );
    file->ops = ops;
    file->size = size;
    file->refs = 1;
    return file;
}

static inline void horizon_memfile_ref( struct horizon_memfile *file )
{
    pthread_mutex_lock( &file->mutex );
    file->refs++;
    pthread_mutex_unlock( &file->mutex );
}

static inline void horizon_memfile_unref( struct horizon_memfile *file )
{
    unsigned int refs;

    pthread_mutex_lock( &file->mutex );
    refs = --file->refs;
    pthread_mutex_unlock( &file->mutex );
    if (refs) return;

    /* An anchor the kernel would not remove still locks its pages, so they
     * are never handed back to the heap. */
    if (!file->anchors) horizon_memfile_release_pages( file );
    free( file->pieces );
    pthread_mutex_destroy( &file->mutex );
    free( file->committed );
    free( file );
}

static inline int horizon_memfile_range_valid( const struct horizon_memfile *file, size_t offset, size_t size )
{
    return size && !(offset % HORIZON_MEMFILE_PAGE_SIZE) && !(size % HORIZON_MEMFILE_PAGE_SIZE) &&
           offset <= file->size && size <= file->size - offset;
}

/* Removes the anchors no view range uses, giving their pages back. */
static inline void horizon_memfile_trim_locked( struct horizon_memfile *file )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    struct horizon_memfile_anchor **next = &file->anchors, *anchor;

    while ((anchor = *next))
    {
        if (anchor->users ||
            file->ops->unanchor( anchor->addr, horizon_memfile_source( file, anchor->first * page_size, NULL ),
                                 anchor->count * page_size, anchor->token ))
        {
            next = &anchor->next;
            continue;
        }
        *next = anchor->next;
        free( anchor );
    }
}

/* Anchors the pages [first, end) that have none. Anchors made before a failure
 * stay, unused, for the caller to trim. */
static inline int horizon_memfile_anchor_locked( struct horizon_memfile *file, size_t first, size_t end )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    struct horizon_memfile_anchor **next = &file->anchors, *anchor;
    size_t page = first;

    while (page < end)
    {
        size_t stop, limit;
        unsigned char *source;

        while (*next && (*next)->first + (*next)->count <= page) next = &(*next)->next;
        if (*next && (*next)->first <= page)
        {
            page = (*next)->first + (*next)->count;
            continue;
        }
        stop = *next && (*next)->first < end ? (*next)->first : end;
        source = horizon_memfile_source( file, page * page_size, &limit );
        if (stop > limit / page_size) stop = limit / page_size;
        if (!(anchor = calloc( 1, sizeof(*anchor) )))
        {
            errno = ENOMEM;
            return -1;
        }
        anchor->first = page;
        anchor->count = stop - page;
        if (!(anchor->addr = file->ops->anchor( source, anchor->count * page_size,
                                                &anchor->token )))
        {
            free( anchor );
            return -1;
        }
        anchor->next = *next;
        *next = anchor;
        next = &anchor->next;
        page = stop;
    }
    return 0;
}

/* Maps (or unmaps) the section's pages [first, end) at dst, one piece per
 * anchor; *done is the first page not handled. */
static inline int horizon_memfile_alias_range_locked( struct horizon_memfile *file, void *dst, size_t first,
                                                      size_t end, int map, size_t *done )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    struct horizon_memfile_anchor *anchor = file->anchors;
    size_t page = first;

    while (page < end)
    {
        size_t stop;
        void *src, *at;
        int ret;

        while (anchor && anchor->first + anchor->count <= page) anchor = anchor->next;
        if (!anchor || anchor->first > page)
        {
            errno = EINVAL;  /* a view range over pages nothing anchors */
            break;
        }
        stop = anchor->first + anchor->count < end ? anchor->first + anchor->count : end;
        src = (char *)anchor->addr + (page - anchor->first) * page_size;
        at = (char *)dst + (page - first) * page_size;
        ret = map ? file->ops->alias( at, src, (stop - page) * page_size )
                  : file->ops->unalias( at, src, (stop - page) * page_size );
        if (ret) break;
        page = stop;
    }
    *done = page;
    return page == end ? 0 : -1;
}

/* As above, putting back what was done if the kernel refuses a piece. */
static inline int horizon_memfile_alias_locked( struct horizon_memfile *file, void *dst, size_t first,
                                                size_t end, int map )
{
    size_t done, undone;
    int saved_errno;

    if (!horizon_memfile_alias_range_locked( file, dst, first, end, map, &done )) return 0;
    saved_errno = errno;
    horizon_memfile_alias_range_locked( file, dst, first, done, !map, &undone );
    errno = saved_errno;
    return -1;
}

static inline void horizon_memfile_use_locked( struct horizon_memfile *file, size_t first, size_t end, int delta )
{
    struct horizon_memfile_anchor *anchor;

    for (anchor = file->anchors; anchor && anchor->first < end; anchor = anchor->next)
    {
        if (anchor->first + anchor->count <= first) continue;
        if (delta > 0) anchor->users++;
        else if (anchor->users) anchor->users--;
    }
}

/* A new view range: anchors its pages, maps them at dst and counts the range
 * as a user of those anchors. */
static inline int horizon_memfile_map( struct horizon_memfile *file, void *dst, size_t offset, size_t size )
{
    size_t first, end;
    int ret;

    if (!horizon_memfile_range_valid( file, offset, size ))
    {
        errno = EINVAL;
        return -1;
    }
    first = offset / HORIZON_MEMFILE_PAGE_SIZE;
    end = first + size / HORIZON_MEMFILE_PAGE_SIZE;

    pthread_mutex_lock( &file->mutex );
    if (!(ret = horizon_memfile_anchor_locked( file, first, end )) &&
        !(ret = horizon_memfile_alias_locked( file, dst, first, end, 1 )))
    {
        horizon_memfile_use_locked( file, first, end, 1 );
    }
    else
    {
        int saved_errno = errno;

        horizon_memfile_trim_locked( file );
        errno = saved_errno;
    }
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

/* The kernel side of a view range alone, for splitting ranges: their pages
 * must already be anchored and used, and use counts stay as they are. */
static inline int horizon_memfile_alias( struct horizon_memfile *file, void *dst, size_t offset, size_t size, int map )
{
    size_t first;
    int ret;

    if (!horizon_memfile_range_valid( file, offset, size ))
    {
        errno = EINVAL;
        return -1;
    }
    first = offset / HORIZON_MEMFILE_PAGE_SIZE;
    pthread_mutex_lock( &file->mutex );
    ret = horizon_memfile_alias_locked( file, dst, first, first + size / HORIZON_MEMFILE_PAGE_SIZE, map );
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

/* Counts a view range as a user of its pages' anchors, or stops counting it;
 * anchors no range uses any more are removed. */
static inline void horizon_memfile_use( struct horizon_memfile *file, size_t offset, size_t size, int delta )
{
    size_t first;

    if (!horizon_memfile_range_valid( file, offset, size )) return;
    first = offset / HORIZON_MEMFILE_PAGE_SIZE;
    pthread_mutex_lock( &file->mutex );
    horizon_memfile_use_locked( file, first, first + size / HORIZON_MEMFILE_PAGE_SIZE, delta );
    if (delta < 0) horizon_memfile_trim_locked( file );
    pthread_mutex_unlock( &file->mutex );
}

/* Copies len bytes at offset, through the anchors where pages are anchored. */
static inline void horizon_memfile_copy_locked( struct horizon_memfile *file, size_t offset, void *buffer,
                                                size_t len, int write )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    struct horizon_memfile_anchor *anchor = file->anchors;

    while (len)
    {
        const size_t page = offset / page_size;
        unsigned char *ptr;
        size_t stop, chunk;

        while (anchor && anchor->first + anchor->count <= page) anchor = anchor->next;
        if (anchor && anchor->first <= page)
        {
            stop = (anchor->first + anchor->count) * page_size;
            ptr = (unsigned char *)anchor->addr + (offset - anchor->first * page_size);
        }
        else
        {
            size_t limit;
            stop = anchor ? anchor->first * page_size : file->size;
            ptr = horizon_memfile_source( file, offset, &limit );
            if (stop > limit) stop = limit;
        }
        chunk = stop - offset < len ? stop - offset : len;
        if (write) memcpy( ptr, buffer, chunk );
        else memcpy( buffer, ptr, chunk );
        buffer = (char *)buffer + chunk;
        offset += chunk;
        len -= chunk;
    }
}

/* Reads and writes at an offset return a count or a negative errno value, and
 * leave the descriptor position alone. A section keeps the size it was
 * created with: a write past its end is cut short, and one that starts there
 * fails, as on a full disk. */
static inline ssize_t horizon_memfile_pread_locked( struct horizon_memfile *file, char *ptr, size_t len, off_t offset )
{
    size_t count;

    if (offset < 0) return -EINVAL;
    if ((unsigned long long)offset >= file->size) return 0;
    count = file->size - offset;
    if (count > len) count = len;
    horizon_memfile_copy_locked( file, offset, ptr, count, 0 );
    return count;
}

static inline ssize_t horizon_memfile_pwrite_locked( struct horizon_memfile *file, const char *ptr, size_t len,
                                                     off_t offset )
{
    size_t count;

    if (offset < 0) return -EINVAL;
    if (!len) return 0;
    if ((unsigned long long)offset >= file->size) return -ENOSPC;
    count = file->size - offset;
    if (count > len) count = len;
    horizon_memfile_copy_locked( file, offset, (char *)ptr, count, 1 );
    return count;
}

static inline ssize_t horizon_memfile_pread( struct horizon_memfile *file, char *ptr, size_t len, off_t offset )
{
    ssize_t ret;

    pthread_mutex_lock( &file->mutex );
    ret = horizon_memfile_pread_locked( file, ptr, len, offset );
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

static inline ssize_t horizon_memfile_pwrite( struct horizon_memfile *file, const char *ptr, size_t len, off_t offset )
{
    ssize_t ret;

    pthread_mutex_lock( &file->mutex );
    ret = horizon_memfile_pwrite_locked( file, ptr, len, offset );
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

/* read and write at the shared position, each one step under the lock. */
static inline ssize_t horizon_memfile_read( struct horizon_memfile *file, char *ptr, size_t len )
{
    ssize_t ret;

    pthread_mutex_lock( &file->mutex );
    if ((ret = horizon_memfile_pread_locked( file, ptr, len, file->pos )) > 0) file->pos += ret;
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

static inline ssize_t horizon_memfile_write( struct horizon_memfile *file, const char *ptr, size_t len )
{
    ssize_t ret;

    pthread_mutex_lock( &file->mutex );
    if ((ret = horizon_memfile_pwrite_locked( file, ptr, len, file->pos )) > 0) file->pos += ret;
    pthread_mutex_unlock( &file->mutex );
    return ret;
}

static inline off_t horizon_memfile_seek( struct horizon_memfile *file, off_t offset, int whence )
{
    const off_t max_off = (off_t)(~0ull >> 1);
    off_t base;

    pthread_mutex_lock( &file->mutex );
    switch (whence)
    {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = file->pos; break;
    case SEEK_END: base = file->size; break;
    default:
        pthread_mutex_unlock( &file->mutex );
        return -EINVAL;
    }
    if ((offset > 0 && base > max_off - offset) || base + offset < 0)
    {
        pthread_mutex_unlock( &file->mutex );
        return -EINVAL;
    }
    file->pos = base + offset;
    pthread_mutex_unlock( &file->mutex );
    return base + offset;
}

/* Views and anchors depend on the size, so it never changes. */
static inline int horizon_memfile_truncate( struct horizon_memfile *file, off_t length )
{
    return length == (off_t)file->size ? 0 : -EINVAL;
}

static inline void horizon_memfile_stat( struct horizon_memfile *file, struct stat *st )
{
    memset( st, 0, sizeof(*st) );
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_size = file->size;
}

/* wineserver's find_committed_range for a view of the section's bytes
 * [start, start + view_size): whether the pages from offset in the view are
 * committed, and for how many bytes that stays so. */
static inline unsigned int horizon_memfile_committed_range( struct horizon_memfile *file, unsigned long long start,
                                                            unsigned long long view_size, unsigned long long offset,
                                                            unsigned long long *size, int *committed )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    unsigned long long pos, end;

    if ((offset % page_size) || offset >= view_size) return HORIZON_STATUS_INVALID_PARAMETER;
    *committed = 1;
    *size = view_size - offset;
    if (!file->committed) return 0;  /* SEC_COMMIT: everything is */

    pthread_mutex_lock( &file->mutex );
    pos = start + offset;
    end = start + view_size;
    *committed = file->committed[pos / page_size];
    for (pos += page_size; pos < end && file->committed[pos / page_size] == *committed; pos += page_size) ;
    *size = (pos < end ? pos : end) - (start + offset);
    pthread_mutex_unlock( &file->mutex );
    return 0;
}

/* wineserver's add_committed_range: commits [offset, offset + size) of the view. */
static inline unsigned int horizon_memfile_add_committed( struct horizon_memfile *file, unsigned long long start,
                                                          unsigned long long view_size, unsigned long long offset,
                                                          unsigned long long size )
{
    const size_t page_size = HORIZON_MEMFILE_PAGE_SIZE;
    const unsigned long long end = offset + size;
    unsigned long long pos;

    if ((offset % page_size) || (end % page_size) || offset >= view_size || end > view_size || offset >= end)
        return HORIZON_STATUS_INVALID_PARAMETER;
    if (!file->committed) return 0;

    pthread_mutex_lock( &file->mutex );
    for (pos = start + offset; pos < start + end; pos += page_size) file->committed[pos / page_size] = 1;
    pthread_mutex_unlock( &file->mutex );
    return 0;
}

#endif  /* __WINE_HORIZON_MEMFILE_H */
