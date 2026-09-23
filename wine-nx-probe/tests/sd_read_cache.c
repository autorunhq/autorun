/* Host test for the SD card read cache (source/sd_read_cache.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/sd_read_cache.h"

struct fake_file
{
    char *data;
    long long size;
    unsigned int requests;
    int fail;
    unsigned long long bytes;  /* returned by all requests */
};

static long long fake_fill( void *ctx, long long offset, char *buf, size_t size )
{
    struct fake_file *f = ctx;

    f->requests++;
    if (f->fail) return -1;
    if (offset >= f->size) return 0;
    if ((long long)size > f->size - offset) size = (size_t)(f->size - offset);
    memcpy( buf, f->data + offset, size );
    f->bytes += size;
    return (long long)size;
}

static struct fake_file make_file( long long size )
{
    struct fake_file f = { malloc( (size_t)size ), size, 0, 0, 0 };
    long long i;

    assert( f.data );
    for (i = 0; i < size; i++) f.data[i] = (char)((unsigned long long)i * 2654435761u >> 11);
    return f;
}

static unsigned int holding( const struct sd_cache_file *file )
{
    unsigned int i, count = 0;

    for (i = 0; i < SD_CACHE_LINES; i++) count += !!file->lines[i].data;
    return count;
}

static long long cached_read( struct sd_cache_file *file, struct sd_cache_pool *pool, struct fake_file *f,
                              long long offset, char *buf, size_t size )
{
    unsigned int fills = 0, before = f->requests;
    long long got = sd_cache_read( file, pool, offset, buf, size, fake_fill, f, &fills );

    assert( fills == f->requests - before );
    return got;
}

/* OpenTTD's pattern: a seek and a 4 KB read per sprite, mostly forward. */
static void test_sprite_reads(void)
{
    struct sd_cache_pool pool = { .max = 64 };
    struct fake_file f = make_file( 3 * SD_CACHE_CHUNK + 12345 );
    struct sd_cache_file file = { .cacheable = 1 };
    char buf[4096];
    unsigned int reads = 0;
    long long pos;

    for (pos = 0; pos + 4096 <= f.size; pos += 1500, reads++)
    {
        long long at = (reads % 7 == 3 && pos > 50000) ? pos - 50000 : pos;

        assert( cached_read( &file, &pool, &f, at, buf, sizeof(buf) ) == (long long)sizeof(buf) );
        assert( !memcmp( buf, f.data + at, sizeof(buf) ) );
    }
    assert( reads > 250 && f.requests == 4 );  /* one request per chunk */
    assert( pool.used == 4 );
    sd_cache_drop( &file, &pool );
    assert( !pool.used && !file.lines[0].data );
    free( f.data );
}

static void test_end_of_file(void)
{
    struct sd_cache_pool pool = { .max = 64 };
    struct fake_file f = make_file( 1000 );
    struct sd_cache_file file = { .cacheable = 1 };
    char buf[2000];

    assert( cached_read( &file, &pool, &f, 990, buf, 100 ) == 10 && !memcmp( buf, f.data + 990, 10 ) );
    assert( cached_read( &file, &pool, &f, 1000, buf, 100 ) == 0 );
    assert( cached_read( &file, &pool, &f, 5000, buf, 100 ) == 0 );
    assert( cached_read( &file, &pool, &f, 0, buf, sizeof(buf) ) == 1000 && !memcmp( buf, f.data, 1000 ) );
    assert( f.requests == 1 );  /* the short chunk marks the end */
    sd_cache_drop( &file, &pool );
    free( f.data );

    /* A file that ends exactly at a chunk boundary asks once past it. */
    f = make_file( SD_CACHE_CHUNK );
    assert( cached_read( &file, &pool, &f, SD_CACHE_CHUNK - 4, buf, 100 ) == 4 );
    assert( cached_read( &file, &pool, &f, SD_CACHE_CHUNK, buf, 100 ) == 0 );
    assert( cached_read( &file, &pool, &f, SD_CACHE_CHUNK + 50, buf, 100 ) == 0 );
    assert( f.requests == 2 );
    sd_cache_drop( &file, &pool );
    free( f.data );
}

static void test_least_recently_used(void)
{
    struct sd_cache_pool pool = { .max = 64 };
    struct fake_file f = make_file( 20LL * SD_CACHE_CHUNK );
    struct sd_cache_file file = { .cacheable = 1 };
    char buf[100];
    unsigned int i, seed = 1;

    for (i = 0; i < SD_CACHE_LINES; i++)
        assert( cached_read( &file, &pool, &f, (long long)i * SD_CACHE_CHUNK + 7, buf, 10 ) == 10 );
    assert( f.requests == SD_CACHE_LINES );
    assert( cached_read( &file, &pool, &f, 3, buf, 10 ) == 10 && f.requests == SD_CACHE_LINES );
    /* A ninth chunk replaces chunk 1, the least recently used, not chunk 0. */
    assert( cached_read( &file, &pool, &f, 8LL * SD_CACHE_CHUNK, buf, 10 ) == 10 );
    assert( f.requests == SD_CACHE_LINES + 1 && pool.used == SD_CACHE_LINES );
    assert( cached_read( &file, &pool, &f, 11, buf, 10 ) == 10 && f.requests == SD_CACHE_LINES + 1 );
    assert( cached_read( &file, &pool, &f, SD_CACHE_CHUNK, buf, 10 ) == 10 && f.requests == SD_CACHE_LINES + 2 );

    /* Random reads across and between chunks always return the file's bytes. */
    for (i = 0; i < 20000; i++)
    {
        long long at;
        size_t size;

        seed = seed * 1103515245 + 12345;
        at = (long long)(seed % (unsigned int)f.size);
        size = 1 + (seed >> 7) % sizeof(buf);
        if (at + (long long)size > f.size) size = (size_t)(f.size - at);
        assert( cached_read( &file, &pool, &f, at, buf, size ) == (long long)size );
        assert( !memcmp( buf, f.data + at, size ) );
    }
    assert( pool.used == SD_CACHE_LINES );
    sd_cache_drop( &file, &pool );
    free( f.data );
}

static void test_failures_and_memory(void)
{
    struct sd_cache_pool pool = { .max = 64 }, small = { .max = 1 };
    struct fake_file f = make_file( 3LL * SD_CACHE_CHUNK );
    struct sd_cache_file a = { .cacheable = 1 }, b = { .cacheable = 1 };
    char buf[300];

    /* A failed request returns -1, and the next one reuses its buffer. */
    f.fail = 1;
    assert( cached_read( &a, &pool, &f, 5, buf, 10 ) == -1 && pool.used == 1 );
    f.fail = 0;
    assert( cached_read( &a, &pool, &f, 5, buf, 10 ) == 10 && !memcmp( buf, f.data + 5, 10 ) );
    assert( pool.used == 1 );

    /* A request failing after data was copied returns that data. */
    f.fail = 1;
    assert( cached_read( &a, &pool, &f, SD_CACHE_CHUNK - 100, buf, sizeof(buf) ) == 100 );
    assert( !memcmp( buf, f.data + SD_CACHE_CHUNK - 100, 100 ) );
    f.fail = 0;
    sd_cache_drop( &a, &pool );
    assert( !pool.used );

    /* With the pool used up, another file takes the chunk read longest ago
     * rather than reading around the cache. */
    assert( cached_read( &a, &small, &f, 5, buf, 10 ) == 10 && small.used == 1 );
    assert( cached_read( &b, &small, &f, 0, buf, 10 ) == 10 && !memcmp( buf, f.data, 10 ) );
    assert( small.used == 1 && holding( &a ) == 0 && holding( &b ) == 1 );
    /* A read across two chunks gets both: the second takes the first's place. */
    assert( cached_read( &a, &small, &f, SD_CACHE_CHUNK - 100, buf, sizeof(buf) ) == (long long)sizeof(buf) );
    assert( !memcmp( buf, f.data + SD_CACHE_CHUNK - 100, sizeof(buf) ) );
    assert( small.used == 1 && holding( &a ) == 1 && holding( &b ) == 0 );
    sd_cache_drop( &a, &small );
    sd_cache_drop( &b, &small );
    assert( !small.used && !small.held );
    free( f.data );
}

static void test_open_files(void)
{
    static const char grf[] = "sdmc:/switch/wine/drive_c/openttd/baseset/OPENTTD.GRF";
    struct sd_cache_pool pool = { .max = 64 };
    struct fake_file f = make_file( 1000 );
    struct sd_cache_file *list = NULL, *reader, *writer, *late, *after, *lang;
    unsigned int before;
    char buf[10];

    assert( sd_cache_same_path( grf, "/switch//wine/drive_c/openttd/baseset/openttd.grf/" ) );
    assert( sd_cache_same_path( "sdmc:/a/B", "sdmc:/A/b" ) && !sd_cache_same_path( "sdmc:/a/b", "sdmc:/a/bc" ) );
    assert( !sd_cache_same_path( "sdmc:/a/b", "sdmc:/a" ) );

    reader = sd_cache_opened( &list, &pool, (void *)1, grf, 0 );
    assert( reader && reader->cacheable && !reader->writable );
    assert( cached_read( reader, &pool, &f, 0, buf, sizeof(buf) ) == 10 && pool.used == 1 );

    /* A file opened for writing is cached too: The Sims 2 opens its packages
     * that way and reads them. Opening it drops what a writer left. */
    writer = sd_cache_opened( &list, &pool, (void *)2, "/switch/wine/drive_c/openttd/baseset/openttd.grf", 1 );
    assert( writer && writer->cacheable && writer->writable );
    assert( reader->cacheable && reader->lines[0].data && pool.used == 1 );
    assert( cached_read( writer, &pool, &f, 0, buf, sizeof(buf) ) == 10 && pool.used == 2 );

    /* Writing to the path throws away what every open file with it holds, and
     * the next read goes back to the card for the bytes that were written. */
    sd_cache_written( list, &pool, grf );
    assert( !pool.used && reader->cacheable && writer->cacheable );
    memset( f.data, 0x77, 16 );
    before = f.requests;
    assert( cached_read( reader, &pool, &f, 0, buf, sizeof(buf) ) == 10 );
    assert( f.requests == before + 1 && buf[0] == 0x77 );

    /* A reader opened while the writer is open is cached as well. */
    late = sd_cache_opened( &list, &pool, (void *)3, grf, 0 );
    assert( late && late->cacheable );
    sd_cache_closed( &list, &pool, (void *)2 );
    assert( !sd_cache_find( list, (void *)2 ) && sd_cache_find( list, (void *)3 ) == late );

    after = sd_cache_opened( &list, &pool, (void *)4, grf, 0 );
    assert( after && after->cacheable );

    /* Renaming or removing a path forgets it. */
    lang = sd_cache_opened( &list, &pool, (void *)5, "sdmc:/switch/wine/drive_c/openttd/lang/english.lng", 0 );
    before = pool.used;
    assert( cached_read( lang, &pool, &f, 0, buf, sizeof(buf) ) == 10 && pool.used == before + 1 );
    sd_cache_forget_path( list, &pool, "sdmc:/switch/wine/drive_c/openttd/lang/ENGLISH.LNG" );
    assert( !lang->cacheable && pool.used == before && after->cacheable );

    sd_cache_closed( &list, &pool, (void *)1 );
    sd_cache_closed( &list, &pool, (void *)3 );
    sd_cache_closed( &list, &pool, (void *)4 );
    sd_cache_closed( &list, &pool, (void *)5 );
    sd_cache_closed( &list, &pool, (void *)6 );  /* opened before the cache: unknown */
    assert( !list && !pool.used );
    free( f.data );
}

/* A full pool gives a new chunk the one read longest ago, whichever file read
 * it, so a chunk still being read stays where it is. */
static void test_oldest_across_files(void)
{
    struct sd_cache_pool pool = { .max = 2 };
    struct fake_file f = make_file( 4LL * SD_CACHE_CHUNK );
    struct sd_cache_file a = { .cacheable = 1 }, b = { .cacheable = 1 }, c = { .cacheable = 1 };
    unsigned int before;
    char buf[10];

    assert( cached_read( &a, &pool, &f, 0, buf, 10 ) == 10 );
    assert( cached_read( &b, &pool, &f, SD_CACHE_CHUNK, buf, 10 ) == 10 );
    assert( cached_read( &a, &pool, &f, 20, buf, 10 ) == 10 );              /* a is read again */
    assert( cached_read( &c, &pool, &f, 2 * SD_CACHE_CHUNK, buf, 10 ) == 10 );
    assert( holding( &a ) == 1 && holding( &b ) == 0 && holding( &c ) == 1 ); /* b's was oldest */
    before = f.requests;
    assert( cached_read( &a, &pool, &f, 40, buf, 10 ) == 10 && !memcmp( buf, f.data + 40, 10 ) );
    assert( f.requests == before );                                           /* still a hit */
    assert( cached_read( &b, &pool, &f, SD_CACHE_CHUNK + 5, buf, 10 ) == 10 );
    assert( !memcmp( buf, f.data + SD_CACHE_CHUNK + 5, 10 ) && f.requests == before + 1 );
    sd_cache_drop( &a, &pool );
    sd_cache_drop( &b, &pool );
    sd_cache_drop( &c, &pool );
    assert( !pool.used && !pool.held );
    free( f.data );
}

/* The Sims 2's pattern: hundreds of packages held open all at once, each read
 * in clusters. The runtime's 256 chunks were taken by the first packages it
 * opened and every later package read around the cache -- 149,000 card reads
 * in five minutes. Each package now costs one card read for its cluster. */
static void test_many_open_files(void)
{
    enum { FILES = 300, READS = 30 };
    struct sd_cache_pool pool = { .max = 256 };
    struct fake_file f = make_file( 2LL * SD_CACHE_CHUNK );
    static struct sd_cache_file files[FILES];
    unsigned int i, r;
    char buf[64];

    for (i = 0; i < FILES; i++) files[i].cacheable = 1;
    for (i = 0; i < FILES; i++)
        for (r = 0; r < READS; r++)
        {
            long long at = (long long)(r * 997 % (SD_CACHE_CHUNK - sizeof(buf)));

            assert( cached_read( &files[i], &pool, &f, at, buf, sizeof(buf) ) == (long long)sizeof(buf) );
            assert( !memcmp( buf, f.data + at, sizeof(buf) ) );
        }
    assert( f.requests == FILES );             /* one card read a package, not one a read */
    assert( pool.used == pool.max );
    for (i = 0; i < FILES; i++) sd_cache_drop( &files[i], &pool );
    assert( !pool.used && !pool.held );
    free( f.data );
}

/* The pool is sized from the heap the game leaves free, so max moves while
 * files are open: growing must let new chunks in without the held list moving,
 * and shrinking must give chunks back, the ones read longest ago first. */
static void test_pool_resized(void)
{
    struct sd_cache_pool pool = { .max = 2, .cap = 8 };
    struct fake_file f = make_file( 16LL * SD_CACHE_CHUNK );
    struct sd_cache_file files[6];
    struct sd_cache_line **held;
    unsigned int i, before;
    char buf[10];

    memset( files, 0, sizeof(files) );
    for (i = 0; i < 6; i++) files[i].cacheable = 1;
    /* Two chunks is the whole pool: the third file takes the oldest chunk. */
    for (i = 0; i < 3; i++) assert( cached_read( &files[i], &pool, &f, i * SD_CACHE_CHUNK, buf, 10 ) == 10 );
    assert( pool.used == 2 && !holding( &files[0] ) );
    held = pool.held;

    pool.max = 6;  /* the game gave memory back */
    for (i = 3; i < 6; i++) assert( cached_read( &files[i], &pool, &f, i * SD_CACHE_CHUNK, buf, 10 ) == 10 );
    assert( pool.used == 5 && pool.held == held );  /* room for all five, same list */
    before = f.requests;
    assert( cached_read( &files[5], &pool, &f, 5 * SD_CACHE_CHUNK + 4, buf, 10 ) == 10 );
    assert( f.requests == before && !memcmp( buf, f.data + 5 * SD_CACHE_CHUNK + 4, 10 ) );

    pool.max = 2;  /* the game took it back */
    sd_cache_trim( &pool );
    assert( pool.used == 2 );
    /* files[4] and files[5] were read last, so they keep their chunks. */
    assert( holding( &files[4] ) == 1 && holding( &files[5] ) == 1 );
    assert( !holding( &files[1] ) && !holding( &files[2] ) && !holding( &files[3] ) );
    before = f.requests;
    assert( cached_read( &files[5], &pool, &f, 5 * SD_CACHE_CHUNK + 8, buf, 10 ) == 10 );
    assert( f.requests == before && !memcmp( buf, f.data + 5 * SD_CACHE_CHUNK + 8, 10 ) );
    /* A file whose chunk went reads it again, and gets the right bytes. */
    assert( cached_read( &files[1], &pool, &f, SD_CACHE_CHUNK + 12, buf, 10 ) == 10 );
    assert( f.requests == before + 1 && !memcmp( buf, f.data + SD_CACHE_CHUNK + 12, 10 ) );

    pool.max = 0;  /* nothing at all: every chunk goes back */
    sd_cache_trim( &pool );
    assert( !pool.used && !pool.held );
    for (i = 0; i < 6; i++) sd_cache_drop( &files[i], &pool );
    free( f.data );
}

/* Where the last read of the random mix in test_readahead ended. */
static long long file_next;
static long long at_next( const struct sd_cache_file *file )
{
    (void)file;
    return file_next;
}

/* Readahead: scattered small reads each cost fill_min from the card, not a
 * whole chunk, and a file read in order still ends up in chunk-sized fills. */
static void test_readahead(void)
{
    struct sd_cache_pool pool = { .max = 64, .fill_min = SD_CACHE_FILL_MIN };
    struct fake_file f = make_file( 4LL * 1024 * 1024 );
    struct sd_cache_file file = { .cacheable = 1 };
    char buf[4096];
    unsigned int i, seed = 7;
    long long pos;

    /* Far apart: one fill_min request apiece. */
    for (i = 0; i < 16; i++)
    {
        long long at = (long long)i * 256 * 1024 + 5000;

        assert( cached_read( &file, &pool, &f, at, buf, 1000 ) == 1000 && !memcmp( buf, f.data + at, 1000 ) );
    }
    assert( f.requests == 16 && f.bytes == 16ull * SD_CACHE_FILL_MIN );
    /* A read next to the last of them is already there (a file keeps eight). */
    assert( cached_read( &file, &pool, &f, 15ll * 256 * 1024 + 9000, buf, 2000 ) == 2000 && f.requests == 16 );
    sd_cache_drop( &file, &pool );

    /* In order from the start: fill_min, twice that, and so on up to a chunk
     * at a time. The last fill runs on past the reader, as readahead does. */
    f.requests = 0;
    f.bytes = 0;
    for (pos = 0; pos < 1024 * 1024; pos += sizeof(buf))
    {
        assert( cached_read( &file, &pool, &f, pos, buf, sizeof(buf) ) == (long long)sizeof(buf) );
        assert( !memcmp( buf, f.data + pos, sizeof(buf) ) );
    }
    {
        unsigned long long covered = 0, window = SD_CACHE_FILL_MIN;
        unsigned int requests = 0;

        for (; covered < 1024 * 1024; requests++)
        {
            covered += window;
            window = window * 2 > SD_CACHE_CHUNK ? SD_CACHE_CHUNK : window * 2;
        }
        assert( f.requests == requests && f.bytes == covered );
    }
    sd_cache_drop( &file, &pool );

    /* The end of the file: found once, then no request past it. */
    free( f.data );
    f = make_file( 1000 );
    assert( cached_read( &file, &pool, &f, 990, buf, 100 ) == 10 && !memcmp( buf, f.data + 990, 10 ) );
    assert( cached_read( &file, &pool, &f, 1000, buf, 100 ) == 0 && cached_read( &file, &pool, &f, 9000, buf, 1 ) == 0 );
    assert( cached_read( &file, &pool, &f, 0, buf, 2000 ) == 1000 && !memcmp( buf, f.data, 1000 ) );
    assert( f.requests == 1 );
    sd_cache_drop( &file, &pool );
    free( f.data );

    /* Anything, anywhere, with fills of every size: always the file's bytes. */
    f = make_file( 3LL * 1024 * 1024 + 777 );
    for (i = 0; i < 30000; i++)
    {
        long long at;
        size_t size;

        seed = seed * 1103515245 + 12345;
        at = (i % 5 == 0) ? (long long)(seed % (unsigned int)f.size) : at_next( &file );
        size = 1 + (seed >> 9) % sizeof(buf);
        if (at >= f.size) at = f.size - 1;
        if (at + (long long)size > f.size) size = (size_t)(f.size - at);
        assert( cached_read( &file, &pool, &f, at, buf, size ) == (long long)size );
        assert( !memcmp( buf, f.data + at, size ) );
        file_next = at + (long long)size;
    }
    sd_cache_drop( &file, &pool );
    assert( !pool.used );
    free( f.data );
}

/* A replay of the two policies on the same reads, costed as the card costs
 * them on the hardware: 0.83 ms a request and 42 MB/s, fitted from two runs of
 * The Sims 2 (10,795 requests for 1,390 MB in 41.7 s with whole chunks, 28,552
 * for 665 MB in 39.3 s from 16 KB). The replay's reads are more scattered than
 * the game's, so it favours small fills more than the card did: it tests that
 * readahead works, not which policy the runtime should use. The files are
 * generated rather than stored, so they can be as large as packages. */
struct virtual_file { unsigned int id; unsigned long long bytes; unsigned int requests; };

static char virtual_byte( unsigned int id, long long offset )
{
    return (char)(((unsigned long long)offset + id * 0x9e3779b9ull) * 2654435761u >> 11);
}

static long long virtual_fill( void *ctx, long long offset, char *buf, size_t size )
{
    struct virtual_file *f = ctx;
    const long long file_size = 96ll * 1024 * 1024;
    size_t i;

    f->requests++;
    if (offset >= file_size) return 0;
    if ((long long)size > file_size - offset) size = (size_t)(file_size - offset);
    for (i = 0; i < size; i++) buf[i] = virtual_byte( f->id, offset + (long long)i );
    f->bytes += size;
    return (long long)size;
}

struct replay { double ms; unsigned long long asked, fetched; unsigned int requests; };

static void replay( unsigned int fill_min, int scattered, struct replay *out )
{
    enum { FILES = 24, LOADS = 20000 };
    struct sd_cache_pool pool = { .max = 512, .fill_min = fill_min };
    static struct sd_cache_file files[FILES];
    static struct virtual_file data[FILES];
    static long long recent[2048];
    static char buf[64 * 1024];
    unsigned int i, seed = 99, fills, recent_count = 0;

    memset( files, 0, sizeof(files) );
    memset( data, 0, sizeof(data) );
    memset( out, 0, sizeof(*out) );
    for (i = 0; i < FILES; i++) { files[i].cacheable = 1; data[i].id = i; }
    for (i = 0; i < LOADS; i++)
    {
        unsigned int which, before_requests;
        unsigned long long before_bytes;
        long long at, size, done;

        seed = seed * 1103515245 + 12345;
        if (scattered)
        {
            /* The Sims 2: a record's header, then its body in 4 KB reads, from
             * a few packages most of the time; now and then one read again. */
            which = (seed >> 8) % 100 < 70 ? (seed >> 16) % 6 : (seed >> 16) % FILES;
            if (recent_count && (seed >> 4) % 4 == 0)
                at = recent[(seed >> 12) % recent_count];
            else
                at = (long long)((seed * 2654435761u) % (96u * 1024 * 1024 - 65536)) & ~15ll;
            if (recent_count < 2048) recent[recent_count++] = at;
            size = 256ll << ((seed >> 20) % 8);   /* 256 bytes to 32 KB */
            size += (seed >> 3) % size;
        }
        else
        {
            /* OpenTTD: sprites read in order from one file, 4 KB at a time. */
            which = 0;
            at = (long long)i * 4096;
            size = 4096;
        }
        for (done = 0; done < size; )
        {
            long long piece = size - done > 4096 ? 4096 : size - done;
            long long got;

            before_requests = data[which].requests;
            before_bytes = data[which].bytes;
            fills = 0;
            got = sd_cache_read( &files[which], &pool, at + done, buf, (size_t)piece, virtual_fill, &data[which], &fills );
            assert( got == piece );
            assert( buf[0] == virtual_byte( which, at + done ) && buf[piece - 1] == virtual_byte( which, at + done + piece - 1 ) );
            out->ms += (data[which].requests - before_requests) * 0.83 +
                       (double)(data[which].bytes - before_bytes) / (42.0 * 1024 * 1024) * 1000;
            done += got;
        }
        out->asked += (unsigned long long)size;
    }
    for (i = 0; i < FILES; i++)
    {
        out->fetched += data[i].bytes;
        out->requests += data[i].requests;
        sd_cache_drop( &files[i], &pool );
    }
    free( pool.held );
}

static void test_replay(void)
{
    struct replay whole, ahead;

    replay( 0, 1, &whole );
    replay( SD_CACHE_FILL_MIN, 1, &ahead );
    printf( "scattered records: whole chunks %u requests, %.1fx the bytes asked, %.1f s; "
            "readahead %u requests, %.1fx, %.1f s\n",
            whole.requests, (double)whole.fetched / whole.asked, whole.ms / 1000,
            ahead.requests, (double)ahead.fetched / ahead.asked, ahead.ms / 1000 );
    assert( ahead.ms < whole.ms * 0.75 );

    replay( 0, 0, &whole );
    replay( SD_CACHE_FILL_MIN, 0, &ahead );
    printf( "in order: whole chunks %u requests, %.1f s; readahead %u requests, %.1f s\n",
            whole.requests, whole.ms / 1000, ahead.requests, ahead.ms / 1000 );
    assert( ahead.ms <= whole.ms * 1.05 );
}

int main(void)
{
    test_sprite_reads();
    test_end_of_file();
    test_least_recently_used();
    test_failures_and_memory();
    test_oldest_across_files();
    test_many_open_files();
    test_pool_resized();
    test_readahead();
    test_replay();
    test_open_files();
    puts( "SD read cache: sprite reads, end of file, least recently used, failures, a full pool taking the "
          "oldest chunk of any file, 300 open files, a pool that grows and shrinks with the free heap, "
          "readahead, and open files a program writes to passed" );
    return 0;
}
