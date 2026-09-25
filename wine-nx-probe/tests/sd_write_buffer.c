/* Host test for the SD card write buffer (source/sd_write_buffer.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/sd_write_buffer.h"

/* A file on the card: what the FS service would hold, and the requests it got. */
struct fake_card
{
    char data[1 << 20];
    long long size;
    long long pos;           /* the file position the card keeps */
    unsigned int requests;
    long long max_chunk;     /* the card takes at most this much per request, 0 for all */
    int fail;
};

static long long card_write_at( struct fake_card *card, long long offset, const char *buf, size_t size )
{
    card->requests++;
    if (card->fail) return -1;
    if (card->max_chunk && (long long)size > card->max_chunk) size = (size_t)card->max_chunk;
    assert( offset + (long long)size <= (long long)sizeof(card->data) );
    memcpy( card->data + offset, buf, size );
    if (offset + (long long)size > card->size) card->size = offset + (long long)size;
    card->pos = offset + (long long)size;
    return (long long)size;
}

static long long out( void *ctx, void *key, long long offset, const char *buf, size_t size )
{
    (void)key;
    return card_write_at( ctx, offset, buf, size );
}

/* What sd_cache.c does with a write: hold it, or send it at the file position. */
static void program_write( struct sd_write_buffer **list, struct fake_card *card, void *key,
                           const char *ptr, size_t len )
{
    long long at = sd_write_position( sd_write_find( *list, key ), card->pos );
    int held = sd_write_take( list, key, at, ptr, len, out, card );

    assert( held >= 0 );
    if (!held) card_write_at( card, at, ptr, len );
}

/* What sd_cache.c does with a seek from the start. */
static void program_seek( struct sd_write_buffer **list, struct fake_card *card, void *key, long long pos )
{
    struct sd_write_buffer *wb = sd_write_find( *list, key );

    if (wb && wb->len) sd_write_seek( wb, pos );
    else card->pos = pos;
}

/* What sd_cache.c does before a seek, a read or a close. */
static void program_flush( struct sd_write_buffer **list, struct fake_card *card, void *key )
{
    assert( !sd_write_flush( sd_write_find( *list, key ), out, card ) );
}

static struct fake_card *new_card(void)
{
    struct fake_card *card = calloc( 1, sizeof(*card) );

    assert( card );
    return card;
}

static void fill( char *buf, size_t size, unsigned int seed )
{
    size_t i;

    for (i = 0; i < size; i++) buf[i] = (char)((i + seed) * 2654435761u >> 11);
}

/* A LithTech save: 262 KB, four bytes at a time. */
static void test_value_at_a_time(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card(), *plain = new_card();
    static char save[262144];
    size_t i;
    int key;

    fill( save, sizeof(save), 1 );
    for (i = 0; i < sizeof(save); i += 4)
    {
        program_write( &list, card, &key, save + i, 4 );
        card_write_at( plain, plain->pos, save + i, 4 );
    }
    program_flush( &list, card, &key );

    assert( plain->requests == sizeof(save) / 4 );
    assert( card->requests == (sizeof(save) + SD_WRITE_BUFFER_SIZE - 1) / SD_WRITE_BUFFER_SIZE );
    assert( card->size == plain->size && !memcmp( card->data, plain->data, (size_t)plain->size ) );
    sd_write_forget( &list, &key );
    assert( !list );
    printf( "value at a time: %u requests instead of %u\n", card->requests, plain->requests );
    free( card );
    free( plain );
}

/* LithTech's save: a length left blank, the values, then back to fill in the
 * length and on again after them. */
static void test_fill_in_lengths(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card(), *plain = new_card();
    unsigned int block, i, blank = 0;
    long long at, plain_at;
    char value[4];
    int key;

    for (block = 0; block < 2000; block++)
    {
        at = sd_write_position( sd_write_find( list, &key ), card->pos );
        plain_at = plain->pos;
        program_write( &list, card, &key, (const char *)&blank, 4 );
        card_write_at( plain, plain->pos, (const char *)&blank, 4 );
        for (i = 0; i < 1 + block % 40; i++)
        {
            fill( value, sizeof(value), block * 64 + i );
            program_write( &list, card, &key, value, sizeof(value) );
            card_write_at( plain, plain->pos, value, sizeof(value) );
        }
        {
            long long end = sd_write_position( sd_write_find( list, &key ), card->pos );
            long long plain_end = plain->pos;

            program_seek( &list, card, &key, at );
            program_write( &list, card, &key, (const char *)&i, 4 );
            program_seek( &list, card, &key, end );
            plain->pos = plain_at;
            card_write_at( plain, plain->pos, (const char *)&i, 4 );
            plain->pos = plain_end;
        }
    }
    program_flush( &list, card, &key );
    assert( card->size == plain->size && !memcmp( card->data, plain->data, (size_t)plain->size ) );
    /* A length that falls just before a new buffer sends the old one out, so
     * a few more than size / 64 KB. */
    assert( card->requests < plain->size / SD_WRITE_BUFFER_SIZE * 3 + 3 );
    printf( "filled-in lengths: %u requests instead of %u\n", card->requests, plain->requests );
    sd_write_forget( &list, &key );
    free( card );
    free( plain );
}

/* The runtime sends held bytes out every 200 ms, in the middle of a save. The
 * window stays, so a length filled in before the cut is one more small part
 * of the next send and not a new window. */
static void test_periodic_flush_mid_save(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card(), *plain = new_card();
    unsigned int block, i, blank = 0, sends = 0;
    char value[4];
    int key;

    for (block = 0; block < 3000; block++)
    {
        long long at = sd_write_position( sd_write_find( list, &key ), card->pos ), end;

        program_write( &list, card, &key, (const char *)&blank, 4 );
        card_write_at( plain, at, (const char *)&blank, 4 );
        for (i = 0; i < 1 + block % 30; i++)
        {
            fill( value, sizeof(value), block * 64 + i );
            program_write( &list, card, &key, value, sizeof(value) );
            card_write_at( plain, plain->pos, value, sizeof(value) );
        }
        /* The timer, between a block's values and its length. */
        if (block % 50 == 49)
        {
            program_flush( &list, card, &key );
            sends++;
        }
        end = sd_write_position( sd_write_find( list, &key ), card->pos );
        program_seek( &list, card, &key, at );
        program_write( &list, card, &key, (const char *)&i, 4 );
        program_seek( &list, card, &key, end );
        card_write_at( plain, at, (const char *)&i, 4 );
        plain->pos = end;
    }
    program_flush( &list, card, &key );
    assert( card->size == plain->size && !memcmp( card->data, plain->data, (size_t)plain->size ) );
    assert( card->requests == sends + 1 );
    printf( "periodic flush mid-save: %u requests instead of %u\n", card->requests, plain->requests );
    sd_write_forget( &list, &key );
    free( card );
    free( plain );
}

/* A zero-length write is nothing to hold and nothing to send. */
static void test_nothing_written(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card();
    int key;

    assert( sd_write_take( &list, &key, 0, "", 0, out, card ) == 0 );
    assert( !list && card->requests == 0 );
    free( card );
}

/* Bytes reach the card in the order they were written, whatever their sizes. */
static void test_mixed_sizes_keep_order(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card(), *plain = new_card();
    static char big[SD_WRITE_DIRECT + 64];
    char small[100];
    int key, round;

    fill( big, sizeof(big), 7 );
    for (round = 0; round < 12; round++)
    {
        size_t n = (size_t)(round * 7 + 1);

        fill( small, sizeof(small), (unsigned int)round );
        program_write( &list, card, &key, small, n );
        card_write_at( plain, plain->pos, small, n );
        if (round % 3 == 2)
        {
            /* A large write goes straight out, after what was held. */
            program_write( &list, card, &key, big, SD_WRITE_DIRECT + (size_t)round );
            card_write_at( plain, plain->pos, big, SD_WRITE_DIRECT + (size_t)round );
        }
    }
    program_flush( &list, card, &key );
    assert( card->size == plain->size && !memcmp( card->data, plain->data, (size_t)plain->size ) );
    sd_write_forget( &list, &key );
    free( card );
    free( plain );
}

/* A write somewhere else sends out what was held first, then is held itself. */
static void test_jump_elsewhere(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card();
    char a[10], b[10];
    int key;

    fill( a, sizeof(a), 3 );
    fill( b, sizeof(b), 4 );
    assert( sd_write_take( &list, &key, 0, a, sizeof(a), out, card ) == 1 );
    assert( card->requests == 0 );
    assert( sd_write_position( sd_write_find( list, &key ), 0 ) == 10 );
    assert( sd_write_take( &list, &key, 500, b, sizeof(b), out, card ) == 1 );
    assert( card->requests == 1 && !memcmp( card->data, a, sizeof(a) ) );
    assert( sd_write_position( sd_write_find( list, &key ), card->pos ) == 510 );
    program_flush( &list, card, &key );
    assert( card->requests == 2 && !memcmp( card->data + 500, b, sizeof(b) ) && card->size == 510 );
    sd_write_forget( &list, &key );
    free( card );
}

/* A full buffer goes out whole, and the write that did not fit starts the next. */
static void test_full_buffer(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card();
    static char chunk[SD_WRITE_DIRECT - 1];
    size_t held = 0;
    int key;

    fill( chunk, sizeof(chunk), 5 );
    while (held + sizeof(chunk) <= SD_WRITE_BUFFER_SIZE)
    {
        program_write( &list, card, &key, chunk, sizeof(chunk) );
        held += sizeof(chunk);
    }
    assert( card->requests == 0 );
    program_write( &list, card, &key, chunk, sizeof(chunk) );
    assert( card->requests == 1 && card->size == (long long)held );
    assert( sd_write_find( list, &key )->len == sizeof(chunk) );
    sd_write_forget( &list, &key );
    free( card );
}

/* The card taking a little at a time still gets all of it, in place. */
static void test_card_takes_part(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card(), *plain = new_card();
    char value[3];
    int key, i;

    card->max_chunk = 1000;
    for (i = 0; i < 5000; i++)
    {
        fill( value, sizeof(value), (unsigned int)i );
        program_write( &list, card, &key, value, sizeof(value) );
        card_write_at( plain, plain->pos, value, sizeof(value) );
    }
    program_flush( &list, card, &key );
    assert( card->size == plain->size && !memcmp( card->data, plain->data, (size_t)plain->size ) );
    sd_write_forget( &list, &key );
    free( card );
    free( plain );
}

/* A card that refuses is reported when the held bytes go out, and they are dropped. */
static void test_card_refuses(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *card = new_card();
    char value[8] = "abcdefg";
    int key;

    assert( sd_write_take( &list, &key, 0, value, sizeof(value), out, card ) == 1 );
    card->fail = 1;
    assert( sd_write_flush( sd_write_find( list, &key ), out, card ) == -1 );
    assert( !sd_write_dirty( sd_write_find( list, &key ) ) );
    /* Nothing held, so nothing more to send and nothing more to fail. */
    assert( sd_write_flush( sd_write_find( list, &key ), out, card ) == 0 );
    /* A write that has to send held bytes out first reports their failure. */
    card->fail = 0;
    assert( sd_write_take( &list, &key, 0, value, sizeof(value), out, card ) == 1 );
    card->fail = 1;
    assert( sd_write_take( &list, &key, 1000, value, sizeof(value), out, card ) == -1 );
    sd_write_forget( &list, &key );
    free( card );
}

/* Each open file has a buffer of its own. */
static void test_two_files(void)
{
    struct sd_write_buffer *list = NULL;
    struct fake_card *one = new_card(), *two = new_card();
    int key_one, key_two, i;
    char a = 'a', b = 'b';

    for (i = 0; i < 100; i++)
    {
        program_write( &list, one, &key_one, &a, 1 );
        program_write( &list, two, &key_two, &b, 1 );
    }
    assert( one->requests == 0 && two->requests == 0 );
    program_flush( &list, one, &key_one );
    assert( one->size == 100 && two->size == 0 && sd_write_find( list, &key_two )->len == 100 );
    program_flush( &list, two, &key_two );
    for (i = 0; i < 100; i++) assert( one->data[i] == 'a' && two->data[i] == 'b' );
    sd_write_forget( &list, &key_one );
    assert( sd_write_find( list, &key_two ) && !sd_write_find( list, &key_one ) );
    sd_write_forget( &list, &key_two );
    assert( !list );
    free( one );
    free( two );
}

int main(void)
{
    test_value_at_a_time();
    test_mixed_sizes_keep_order();
    test_fill_in_lengths();
    test_periodic_flush_mid_save();
    test_nothing_written();
    test_jump_elsewhere();
    test_full_buffer();
    test_card_takes_part();
    test_card_refuses();
    test_two_files();
    printf( "sd_write_buffer: all tests passed\n" );
    return 0;
}
