/* Host test for the SD card stat cache (source/sd_stat_cache.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/sd_stat_cache.h"

static struct stat file_of_size( long long size )
{
    struct stat st;

    memset( &st, 0, sizeof(st) );
    st.st_mode = S_IFREG;
    st.st_size = (off_t)size;
    return st;
}

/* F.E.A.R. asking for PunkBuster, the way Wine looks it up. */
static void test_missing_files_asked_again(void)
{
    static struct sd_stat_cache cache;
    static const char *const lookup[] =
    {
        "sdmc:/switch/wine/drive_c/users",
        "sdmc:/switch/wine/drive_c/users/Public",
        "sdmc:/switch/wine/drive_c/users/Public/Documents",
        "sdmc:/switch/wine/drive_c/users/Public/Documents/Monolith Productions",
        "sdmc:/switch/wine/drive_c/users/Public/Documents/Monolith Productions/FEAR",
        "sdmc:/switch/wine/drive_c/users/Public/Documents/Monolith Productions/FEAR/pb",
        "sdmc:/switch/wine/drive_c/users/Public/Documents/Monolith Productions/FEAR/pb/pbcl.dll",
        "sdmc:/switch/wine/drive_c/users/Public/Documents/Monolith Productions/FEAR/pb/pbclnew.dll",
    };
    unsigned int round, i, asked = 0;
    struct stat st, dir;
    int error;

    memset( &dir, 0, sizeof(dir) );
    dir.st_mode = S_IFDIR;
    for (round = 0; round < 1000; round++)
    {
        for (i = 0; i < sizeof(lookup) / sizeof(lookup[0]); i++)
        {
            if (sd_stat_lookup( &cache, lookup[i], &st, &error ))
            {
                /* The last two are not there; the rest are directories. */
                assert( i >= 6 ? error == ENOENT : !error && S_ISDIR( st.st_mode ) );
                continue;
            }
            asked++;
            if (i >= 6) sd_stat_store( &cache, lookup[i], NULL, ENOENT );
            else sd_stat_store( &cache, lookup[i], &dir, 0 );
        }
    }
    assert( asked == sizeof(lookup) / sizeof(lookup[0]) );
    printf( "PunkBuster lookups: the card asked %u times instead of %u\n", asked,
            1000 * (unsigned int)(sizeof(lookup) / sizeof(lookup[0])) );
    sd_stat_forget_all( &cache );
}

/* The same path however it is written. */
static void test_path_spellings(void)
{
    static struct sd_stat_cache cache;
    struct stat st = file_of_size( 10 ), got;
    int error;

    sd_stat_store( &cache, "sdmc:/switch/wine/Save.SAV", &st, 0 );
    assert( sd_stat_lookup( &cache, "/switch//wine/save.sav", &got, &error ) && !error && got.st_size == 10 );
    assert( sd_stat_lookup( &cache, "sdmc:/SWITCH/wine/save.sav/", &got, &error ) && got.st_size == 10 );
    assert( !sd_stat_lookup( &cache, "sdmc:/switch/wine/save.sa", &got, &error ) );
    /* A relative path is never kept. */
    sd_stat_store( &cache, "wine/save.sav", &st, 0 );
    assert( !sd_stat_lookup( &cache, "wine/save.sav", &got, &error ) );
    sd_stat_forget_all( &cache );
}

/* Creating, writing or deleting a path takes it out; other failures are not kept. */
static void test_forget(void)
{
    static struct sd_stat_cache cache;
    struct stat st = file_of_size( 4 ), got;
    int error;

    sd_stat_store( &cache, "/a/new.sav", NULL, ENOENT );
    sd_stat_store( &cache, "/a/old.sav", &st, 0 );
    sd_stat_forget( &cache, "sdmc:/A/NEW.sav" );
    assert( !sd_stat_lookup( &cache, "/a/new.sav", &got, &error ) );
    assert( sd_stat_lookup( &cache, "/a/old.sav", &got, &error ) );
    sd_stat_forget_all( &cache );
    assert( !sd_stat_lookup( &cache, "/a/old.sav", &got, &error ) );
    sd_stat_store( &cache, "/a/busy", NULL, EIO );
    assert( !sd_stat_lookup( &cache, "/a/busy", &got, &error ) );
    /* A later answer replaces an earlier one. */
    sd_stat_store( &cache, "/a/grow", &st, 0 );
    st.st_size = 8;
    sd_stat_store( &cache, "/a/grow", &st, 0 );
    assert( sd_stat_lookup( &cache, "/a/grow", &got, &error ) && got.st_size == 8 );
    sd_stat_forget_all( &cache );
}

/* More paths than slots: the ones asked for least recently make room, and
 * whatever is kept is what the card said for that path. */
static void test_many_paths(void)
{
    static struct sd_stat_cache cache;
    unsigned int i, kept = 0;
    char path[64];
    struct stat st, got;
    int error;

    for (i = 0; i < 5000; i++)
    {
        snprintf( path, sizeof(path), "/games/file%u", i );
        st = file_of_size( i );
        sd_stat_store( &cache, path, &st, 0 );
        /* One path asked for all the time stays. */
        assert( sd_stat_lookup( &cache, "/games/file0", &got, &error ) && got.st_size == 0 );
    }
    for (i = 0; i < 5000; i++)
    {
        snprintf( path, sizeof(path), "/games/file%u", i );
        if (!sd_stat_lookup( &cache, path, &got, &error )) continue;
        assert( got.st_size == (off_t)i );
        kept++;
    }
    assert( kept > 0 && kept <= SD_STAT_SLOTS );
    sd_stat_forget_all( &cache );
}

/* Wine's reparse point lookup of a missing file, never a question for the card. */
static void test_impossible_names(void)
{
    static struct sd_stat_cache cache;
    struct stat got;
    int error = 0;

    assert( sd_stat_lookup( &cache, "sdmc:/switch/wine/drive_c/FEAR/pb/pbcl.dll?", &got, &error ) && error == ENOENT );
    assert( sd_stat_lookup( &cache, "/a/b*c", &got, &error ) && error == ENOENT );
    assert( !sd_stat_lookup( &cache, "sdmc:/switch/wine/drive_c/FEAR/pb/pbcl.dll", &got, &error ) );
    sd_stat_forget_all( &cache );
}

int main(void)
{
    test_impossible_names();
    test_missing_files_asked_again();
    test_path_spellings();
    test_forget();
    test_many_paths();
    printf( "sd_stat_cache: all tests passed\n" );
    return 0;
}
