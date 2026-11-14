/* Host test for the sdmc device wrapper. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int fail_calloc, fail_strdup;

static void *test_calloc( size_t count, size_t size )
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return calloc( count, size );
}

static char *test_strdup( const char *str )
{
    if (fail_strdup) { fail_strdup = 0; return NULL; }
    return strdup( str );
}

#define calloc test_calloc
#define strdup test_strdup
#include "../source/sd_cache.c"
#undef calloc
#undef strdup

struct _reent test_reent;
static char heap;
char *fake_heap_start = &heap, *fake_heap_end = &heap;
const devoptab_t *devoptab_list[1];

static unsigned char data[15 * 1024 * 1024 + 17];
static size_t data_size;
static unsigned int card_reads;
static int fail_open;
static void (*during_write)(void);
struct test_file { off_t pos; int flags; };
static struct test_file reader, writer;

int FindDevice( const char *name ) { return strcmp( name, "sdmc:" ) ? -1 : 0; }
static char last_trace[512];
void wine_nx_runtime_trace( const char *msg ) { snprintf( last_trace, sizeof(last_trace), "%s", msg ); }

static int card_open( struct _reent *r, void *fd, const char *path, int flags, int mode )
{
    struct test_file *file = fd;

    (void)path;
    (void)mode;
    if (fail_open) { r->_errno = EACCES; return -1; }
    file->pos = 0;
    file->flags = flags;
    if (flags & O_TRUNC) data_size = 0;
    return 0;
}

static int card_close( struct _reent *r, void *fd ) { (void)r; (void)fd; return 0; }

static off_t card_seek( struct _reent *r, void *fd, off_t offset, int dir )
{
    struct test_file *file = fd;
    off_t pos = offset + (dir == SEEK_CUR ? file->pos : dir == SEEK_END ? (off_t)data_size : 0);

    if (pos < 0) { r->_errno = EINVAL; return -1; }
    return file->pos = pos;
}

static ssize_t card_read( struct _reent *r, void *fd, char *buf, size_t len )
{
    struct test_file *file = fd;

    card_reads++;
    if ((file->flags & O_ACCMODE) == O_WRONLY) { r->_errno = EBADF; return -1; }
    if ((size_t)file->pos >= data_size) return 0;
    if (len > data_size - file->pos) len = data_size - file->pos;
    memcpy( buf, data + file->pos, len );
    file->pos += len;
    return len;
}

static ssize_t card_write( struct _reent *r, void *fd, const char *buf, size_t len )
{
    struct test_file *file = fd;

    if ((file->flags & O_ACCMODE) == O_RDONLY) { r->_errno = EBADF; return -1; }
    if (during_write) during_write();
    if (file->flags & O_APPEND) file->pos = data_size;
    assert( file->pos >= 0 && file->pos + len <= sizeof(data) );
    memcpy( data + file->pos, buf, len );
    file->pos += len;
    if ((size_t)file->pos > data_size) data_size = file->pos;
    return len;
}

static int card_fstat( struct _reent *r, void *fd, struct stat *st )
{
    (void)r;
    (void)fd;
    memset( st, 0, sizeof(*st) );
    st->st_size = data_size;
    return 0;
}

static int card_stat( struct _reent *r, const char *path, struct stat *st )
{
    (void)path;
    return card_fstat( r, NULL, st );
}

static const devoptab_t card = {
    .open_r = card_open, .close_r = card_close, .read_r = card_read, .write_r = card_write,
    .seek_r = card_seek, .fstat_r = card_fstat, .stat_r = card_stat,
};

static void reset(void)
{
    size_t i;

    while (sd_cache_files) sd_cache_close( &test_reent, sd_cache_files->key );
    sd_stat_forget_all( &sd_stat_cache );
    memset( sd_cache_closed_tally, 0, sizeof(sd_cache_closed_tally) );
    assert( !sd_cache_pool.used && !sd_cache_pool.held && !sd_write_buffers );
    sd_cache_pool.max = SD_CACHE_POOL_MIN;
    sd_cache_off = fail_open = 0;
    card_reads = wine_nx_sd_reads = wine_nx_sd_hits = 0;
    sd_cache_settled_at = 0;
    during_write = NULL;
    data_size = sizeof(data);
    for (i = 0; i < data_size; i++) data[i] = (unsigned char)(i * 37 + (i >> 9));
}

static char read_byte( struct test_file *file )
{
    char value;

    assert( !sd_cache_seek( &test_reent, file, 0, SEEK_SET ) );
    assert( sd_cache_read_file( &test_reent, file, &value, 1 ) == 1 );
    return value;
}

static void test_open_writer(void)
{
    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/other.bin", O_RDONLY, 0 ) );
    assert( read_byte( &reader ) == data[0] && card_reads == 1 );
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", O_RDWR, 0 ) );
    assert( !sd_cache_off );
    assert( read_byte( &reader ) == data[0] && card_reads == 1 );
    assert( read_byte( &writer ) == data[0] && card_reads == 2 );
    assert( read_byte( &writer ) == data[0] && card_reads == 2 );
}

static void test_open_failure(void)
{
    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDONLY, 0 ) );
    read_byte( &reader );
    fail_open = 1;
    assert( sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", O_RDWR | O_TRUNC, 0 ) == -1 );
    assert( test_reent._errno == EACCES && !sd_cache_off );
    assert( read_byte( &reader ) == data[0] && card_reads == 1 );
}

static void test_tracking_failure(void)
{
    const int flags[] = { O_RDONLY, O_RDWR, O_RDWR | O_CREAT, O_RDWR | O_TRUNC, O_WRONLY | O_APPEND };
    unsigned int i, allocation;

    for (allocation = 0; allocation < 2; allocation++)
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
    {
        reset();
        assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDONLY, 0 ) );
        read_byte( &reader );
        if (allocation) fail_strdup = 1;
        else fail_calloc = 1;
        assert( !sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", flags[i], 0 ) );
        assert( !sd_cache_find( sd_cache_files, &writer ) );
        assert( sd_cache_off == ((flags[i] & O_ACCMODE) != O_RDONLY) );
        assert( sd_cache_pool.used == (sd_cache_off ? 0 : 1) );
    }
}

static void test_truncate(void)
{
    char byte;

    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDONLY, 0 ) );
    read_byte( &reader );
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", O_RDWR | O_TRUNC, 0 ) );
    assert( !sd_cache_off );
    assert( !sd_cache_seek( &test_reent, &reader, 0, SEEK_SET ) );
    assert( !sd_cache_read_file( &test_reent, &reader, &byte, 1 ) );
}

static void test_held_write(void)
{
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", O_RDWR | O_CREAT, 0 ) );
    read_byte( &writer );
    assert( !sd_cache_seek( &test_reent, &writer, 0, SEEK_SET ) );
    assert( sd_cache_write_file( &test_reent, &writer, "X", 1 ) == 1 );
    assert( data[0] != 'X' );
    assert( read_byte( &writer ) == 'X' && data[0] == 'X' );
    assert( read_byte( &writer ) == 'X' && card_reads == 2 );
}

static void read_while_writing(void)
{
    struct stat st;

    assert( read_byte( &reader ) == data[0] );
    assert( !sd_cache_stat( &test_reent, "sdmc:/cache.bin", &st ) );
    assert( st.st_size == (off_t)data_size );
}

static void test_shared_write(void)
{
    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDONLY, 0 ) );
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/CACHE.BIN", O_RDWR | O_CREAT, 0 ) );
    read_byte( &reader );
    during_write = read_while_writing;
    assert( sd_cache_write_file( &test_reent, &writer, "X", 1 ) == 1 );
    during_write = NULL;
    assert( data[0] == 'X' );
    assert( read_byte( &reader ) == 'X' );
}

static void test_append(void)
{
    struct stat st;
    char byte;

    data_size = 1024;
    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDONLY, 0 ) );
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/cache.bin", O_WRONLY | O_APPEND, 0 ) );
    read_byte( &reader );
    during_write = read_while_writing;
    assert( sd_cache_write_file( &test_reent, &writer, "X", 1 ) == 1 );
    during_write = NULL;
    assert( sd_cache_seek( &test_reent, &reader, 1024, SEEK_SET ) == 1024 );
    assert( sd_cache_read_file( &test_reent, &reader, &byte, 1 ) == 1 && byte == 'X' );
    assert( !sd_cache_stat( &test_reent, "sdmc:/cache.bin", &st ) && st.st_size == 1025 );
}

static void test_tiny_reads(void)
{
    const size_t sizes[] = { 1, 4, 16 };
    unsigned int requests = 0;
    size_t offset = 0;
    char buf[2313];

    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/cache.bin", O_RDWR, 0 ) );
    while (offset < data_size)
    {
        size_t size = requests % 32 == 31 ? sizeof(buf) : sizes[requests % 3];

        if (size > data_size - offset) size = data_size - offset;
        assert( sd_cache_read_file( &test_reent, &reader, buf, size ) == (ssize_t)size );
        assert( !memcmp( buf, data + offset, size ) );
        offset += size;
        requests++;
    }
    printf( "tiny reads: %u requests, %u card reads, %u KiB cached\n",
            requests, card_reads, sd_cache_pool.used * SD_CACHE_CHUNK / 1024 );
    assert( card_reads == (data_size + SD_CACHE_CHUNK - 1) / SD_CACHE_CHUNK );
    assert( sd_cache_pool.used <= SD_CACHE_LINES );
}

/* [SDFILES]: what the card gave each open file against what was asked of it,
 * scattered small reads costing whole chunks, and big reads straight through. */
static void test_report_files(void)
{
    struct sd_cache_file *file;
    char byte, *big = malloc( SD_CACHE_DIRECT );
    unsigned int i;

    assert( big );
    assert( !sd_cache_open( &test_reent, &reader, "sdmc:/Roms/Doom 3/base/pak002.pk4", O_RDONLY, 0 ) );
    /* A byte in each of 100 chunks: 100 bytes asked, 100 chunks from the card. */
    for (i = 0; i < 100; i++)
    {
        assert( sd_cache_seek( &test_reent, &reader, (off_t)i * SD_CACHE_CHUNK, SEEK_SET ) == (off_t)i * SD_CACHE_CHUNK );
        assert( sd_cache_read_file( &test_reent, &reader, &byte, 1 ) == 1 );
    }
    assert( (file = sd_cache_find( sd_cache_files, &reader )) );
    assert( file->asked == 100 && file->card == 100ull * SD_CACHE_CHUNK );
    /* Read again: the last chunks are held, nothing more from the card. */
    assert( sd_cache_seek( &test_reent, &reader, 99 * SD_CACHE_CHUNK, SEEK_SET ) == 99 * SD_CACHE_CHUNK );
    assert( sd_cache_read_file( &test_reent, &reader, &byte, 1 ) == 1 );
    assert( file->asked == 101 && file->card == 100ull * SD_CACHE_CHUNK );
    /* Large reads go straight to the card and count as they are. */
    assert( !sd_cache_seek( &test_reent, &reader, 0, SEEK_SET ) );
    for (i = 0; i < 64; i++) assert( sd_cache_read_file( &test_reent, &reader, big, SD_CACHE_DIRECT ) == SD_CACHE_DIRECT );
    assert( file->asked == 101 + 64ull * SD_CACHE_DIRECT && file->card == 100ull * SD_CACHE_CHUNK + 64ull * SD_CACHE_DIRECT );

    last_trace[0] = 0;
    wine_nx_sd_cache_report_files();
    assert( !strcmp( last_trace, "[SDFILES] the card gave files 16 MB: pak002.pk4 16 MB for 4 MB asked" ) );
    /* Told once: nothing new, nothing said. */
    last_trace[0] = 0;
    wine_nx_sd_cache_report_files();
    assert( !last_trace[0] );

    /* Doom 3 opens its archive for each read: closed files count too, the
     * opens of one name together, beside one still open. */
    for (i = 0; i < 3; i++)
    {
        unsigned int k;

        if (i) assert( !sd_cache_open( &test_reent, &reader, "sdmc:/Roms/Doom 3/base/pak002.pk4", O_RDONLY, 0 ) );
        assert( !sd_cache_seek( &test_reent, &reader, 0, SEEK_SET ) );
        for (k = 0; k < 96; k++) assert( sd_cache_read_file( &test_reent, &reader, big, SD_CACHE_DIRECT ) == SD_CACHE_DIRECT );
        assert( !sd_cache_close( &test_reent, &reader ) );
    }
    assert( !sd_cache_open( &test_reent, &writer, "sdmc:/Roms/Doom 3/base/pak000.pk4", O_RDONLY, 0 ) );
    assert( !sd_cache_seek( &test_reent, &writer, 0, SEEK_SET ) );
    for (i = 0; i < 16; i++) assert( sd_cache_read_file( &test_reent, &writer, big, SD_CACHE_DIRECT ) == SD_CACHE_DIRECT );
    last_trace[0] = 0;
    wine_nx_sd_cache_report_files();
    assert( !strcmp( last_trace, "[SDFILES] the card gave files 19 MB: pak002.pk4 18 MB for 18 MB asked; "
                                 "pak000.pk4 1 MB for 1 MB asked" ) );
    last_trace[0] = 0;
    wine_nx_sd_cache_report_files();
    assert( !last_trace[0] );
    assert( !sd_cache_close( &test_reent, &writer ) );
    free( big );
}

int main( int argc, char **argv )
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        { "open-writer", test_open_writer }, { "open-failure", test_open_failure },
        { "tracking-failure", test_tracking_failure }, { "truncate", test_truncate },
        { "held-write", test_held_write }, { "shared-write", test_shared_write },
        { "append", test_append }, { "tiny-reads", test_tiny_reads },
        { "report-files", test_report_files },
    };
    unsigned int i;

    devoptab_list[0] = &card;
    assert( wine_nx_sd_cache_install() );
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        if (argc > 1 && strcmp( argv[1], tests[i].name )) continue;
        reset();
        tests[i].run();
        printf( "%s: passed\n", tests[i].name );
    }
    reset();
    return 0;
}
