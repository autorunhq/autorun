/*
 * Builds the three NCAs a forwarder is made of, on this machine, and takes them
 * apart again: the sections the console will read, the romfs the loader reads
 * the NRO path out of, and the NPDM field that decides the address space.
 *
 * Nothing is installed. The content store is a directory, and the header is
 * left in the clear so it can be read back.
 */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "switch_shim.h"

char shim_out_dir[512];

/* The hash only has to be a hash: what is checked here is the shape of what it
 * is taken over, not the console's acceptance of it. Every byte has to reach
 * every word, though, or two inputs that differ by one would come out alike. */
void sha256CalculateHash( void *dst, const void *src, size_t size )
{
    const u8 *bytes = src;
    u64 state[4] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull };
    u64 rolling = 0xcbf29ce484222325ull;
    size_t i;
    int w;

    for (i = 0; i < size; i++)
    {
        rolling = (rolling ^ bytes[i]) * 0x100000001b3ull;
        rolling ^= rolling >> 29;
    }
    rolling ^= size;
    for (w = 0; w < 4; w++)
    {
        state[w] ^= rolling;
        state[w] *= 0x100000001b3ull;
        state[w] ^= state[w] >> 31;
        rolling = rolling * 6364136223846793005ull + 1442695040888963407ull;
    }
    memcpy( dst, state, 0x20 );
}

/* Left in the clear, so the test can read the header back. */
void aes128XtsContextCreate( Aes128XtsContext *ctx, const void *key0, const void *key1, int encrypt )
{ (void)key0; (void)key1; (void)encrypt; memset( ctx, 0, sizeof(*ctx) ); }
void aes128XtsContextResetSector( Aes128XtsContext *ctx, u64 sector, int nintendo )
{ (void)nintendo; ctx->sector = sector; }
size_t aes128XtsEncrypt( Aes128XtsContext *ctx, void *dst, const void *src, size_t size )
{ (void)ctx; if (dst != src) memcpy( dst, src, size ); return size; }

Result ncmInitialize( void ) { return 0; }
void ncmExit( void ) {}
Result nsInitialize( void ) { return 0; }
void nsExit( void ) {}
/* A console on Atmosphere 1.8.0, where the debug flags moved: the version is
 * packed with the major at bit 56, minor at 48 and micro at 40. */
Result splInitialize( void ) { return 0; }
void splExit( void ) {}
Result splGetConfig( SplConfigItem item, u64 *out )
{
    if (item != 65000) return 1;
    *out = (1ull << 56) | (8ull << 48) | (0ull << 40);
    return 0;
}
Result splCryptoInitialize( void ) { return 0; }
void splCryptoExit( void ) {}
Result splCryptoGenerateAesKek( const void *src, u32 generation, u32 option, void *out )
{ (void)generation; (void)option; memcpy( out, src, 0x10 ); memset( (u8 *)out + 0x10, 0, 0x10 ); return 0; }
Result splCryptoGenerateAesKey( const void *kek, const void *src, void *out )
{ (void)kek; memcpy( out, src, 0x10 ); return 0; }

static int placeholders;
static int registered;
static char placeholder_path[512];

Result ncmOpenContentStorage( NcmContentStorage *out, NcmStorageId id ) { (void)id; out->handle = 1; return 0; }
void ncmContentStorageClose( NcmContentStorage *cs ) { (void)cs; }
Result ncmContentStorageGeneratePlaceHolderId( NcmContentStorage *cs, NcmPlaceHolderId *out )
{ (void)cs; memset( out, 0, sizeof(*out) ); out->c[0] = ++placeholders; return 0; }
Result ncmContentStorageDeletePlaceHolder( NcmContentStorage *cs, NcmPlaceHolderId *id ) { (void)cs; (void)id; return 0; }
Result ncmContentStorageCreatePlaceHolder( NcmContentStorage *cs, const NcmContentId *content,
                                           const NcmPlaceHolderId *id, u64 size )
{
    (void)cs; (void)content; (void)size;
    snprintf( placeholder_path, sizeof(placeholder_path), "%s/nca-%d.bin", shim_out_dir, id->c[0] );
    return 0;
}
Result ncmContentStorageWritePlaceHolder( NcmContentStorage *cs, const NcmPlaceHolderId *id, u64 offset,
                                          const void *data, size_t size )
{
    FILE *file;

    (void)cs; (void)id; (void)offset;
    if (!(file = fopen( placeholder_path, "wb" ))) return 1;
    fwrite( data, 1, size, file );
    fclose( file );
    return 0;
}
Result ncmContentStorageDelete( NcmContentStorage *cs, const NcmContentId *id ) { (void)cs; (void)id; return 0; }
Result ncmContentStorageRegister( NcmContentStorage *cs, const NcmContentId *content, const NcmPlaceHolderId *id )
{ (void)cs; (void)content; (void)id; registered++; return 0; }
Result ncmContentStorageHas( NcmContentStorage *cs, bool *out, const NcmContentId *id )
{ (void)cs; (void)id; *out = true; return 0; }
/* Nothing else is installed here, so there is nothing to hold ours against. */
Result ncmContentStorageGetSizeFromContentId( NcmContentStorage *cs, s64 *out, const NcmContentId *id )
{ (void)cs; (void)id; *out = 0; return 1; }
Result ncmContentStorageReadContentIdFile( NcmContentStorage *cs, void *out, size_t size,
                                           const NcmContentId *id, s64 offset )
{ (void)cs; (void)out; (void)size; (void)id; (void)offset; return 1; }
Result ncmContentMetaDatabaseList( NcmContentMetaDatabase *db, s32 *total, s32 *written, NcmContentMetaKey *keys,
                                   s32 count, NcmContentMetaType type, u64 id, u64 id_min, u64 id_max,
                                   NcmContentInstallType install_type )
{
    (void)db; (void)keys; (void)count; (void)type; (void)id; (void)id_min; (void)id_max; (void)install_type;
    *total = *written = 0;
    return 0;
}
Result ncmContentMetaDatabaseListContentInfo( NcmContentMetaDatabase *db, s32 *written, NcmContentInfo *infos,
                                              s32 count, const NcmContentMetaKey *key, s32 start )
{ (void)db; (void)infos; (void)count; (void)key; (void)start; *written = 0; return 0; }
Result ncmOpenContentMetaDatabase( NcmContentMetaDatabase *out, NcmStorageId id ) { (void)id; out->handle = 1; return 0; }
void ncmContentMetaDatabaseClose( NcmContentMetaDatabase *db ) { (void)db; }
Result ncmContentMetaDatabaseSet( NcmContentMetaDatabase *db, const NcmContentMetaKey *key,
                                  const void *data, size_t size )
{ (void)db; (void)key; (void)data; (void)size; return 0; }
Result ncmContentMetaDatabaseCommit( NcmContentMetaDatabase *db ) { (void)db; return 0; }
static Service manager_service;
Result nsGetApplicationManagerInterface( Service *out ) { *out = manager_service; return 0; }
Service *nsGetServiceSession_ApplicationManagerInterface( void ) { return &manager_service; }
/* What was taken away, and whether anything had been written by then: the
 * contents of an entry must go before the new ones are registered, or they are
 * the same ones and they go with them. */
static u64 deleted_completely[4];
static int deleted_completely_count;
static u64 deleted_entity;
static int deleted_after_write;

Result nsDeleteApplicationCompletely( u64 id )
{
    if (placeholders) deleted_after_write = 1;
    if (deleted_completely_count < 4) deleted_completely[deleted_completely_count++] = id;
    return 0;
}
Result nsDeleteApplicationEntity( u64 id )
{
    if (placeholders) deleted_after_write = 1;
    deleted_entity = id;
    return 0;
}
void serviceClose( Service *s ) { (void)s; }
int hosversionAtLeast( int major, int minor, int micro ) { (void)major; (void)minor; (void)micro; return 1; }

/* The loader and the icons the real build embeds; here, something the right
 * shape, with an NPDM taken from the one devkitPro's npdmtool wrote. */
const unsigned char *wine_nx_hbl_main;
size_t wine_nx_hbl_main_size;
const unsigned char *wine_nx_game_forwarder_main;
size_t wine_nx_game_forwarder_main_size;
const unsigned char *wine_nx_hbl_npdm;
size_t wine_nx_hbl_npdm_size;
const unsigned char *wine_nx_icon_any;
size_t wine_nx_icon_any_size;

#define WINE_NX_FORWARDER_EMBED_EXTERN
#include "../source/forwarder.c"

static void *load_file( const char *path, size_t *size )
{
    FILE *file = fopen( path, "rb" );
    void *data;
    long length;

    if (!file) { fprintf( stderr, "cannot open %s\n", path ); exit( 2 ); }
    fseek( file, 0, SEEK_END );
    length = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = malloc( length );
    if (fread( data, 1, length, file ) != (size_t)length) exit( 2 );
    fclose( file );
    *size = length;
    return data;
}

/* An NRO with assets, so the path that reads the NACP out of one is the path
 * the test walks: code, then ASET, then a NACP with something recognisable in
 * a field the builder never sets. */
static const u8 nacptool_ratings[0x20] =
{
    0x0c, 0xff, 0xff, 0x0a, 0xff, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0d, 0x0d, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void write_fake_nro( const char *path, const char *isbn )
{
    u8 header[0x40] = {0};
    u8 assets[0x38] = {0};
    u8 *nacp = calloc( 1, 0x4000 );
    u32 nro_size = sizeof(header);
    u64 value;
    FILE *file = fopen( path, "wb" );

    assert( file && nacp );
    memcpy( header + 0x10, "NRO0", 4 );
    memcpy( header + 0x18, &nro_size, 4 );
    memcpy( assets, "ASET", 4 );
    value = sizeof(assets);            /* icon, empty */
    memcpy( assets + 0x08, &value, 8 );
    value = 0;
    memcpy( assets + 0x10, &value, 8 );
    value = sizeof(assets);            /* the NACP follows the header */
    memcpy( assets + 0x18, &value, 8 );
    value = 0x4000;
    memcpy( assets + 0x20, &value, 8 );
    /* Isbn is at 0x3000 and nothing in the builder touches it. */
    memcpy( nacp + 0x3000, isbn, strlen( isbn ) );
    /* The rating ages nacptool writes, and the languages it says are there. */
    memcpy( nacp + 0x3040, nacptool_ratings, sizeof(nacptool_ratings) );
    memcpy( nacp + 0x302C, &(u32){ 0xbff }, 4 );
    fwrite( header, 1, sizeof(header), file );
    fwrite( assets, 1, sizeof(assets), file );
    fwrite( nacp, 1, 0x4000, file );
    fclose( file );
    free( nacp );
}

static u8 *read_nca( int index, size_t *size )
{
    char path[512];

    snprintf( path, sizeof(path), "%s/nca-%d.bin", shim_out_dir, index );
    return load_file( path, size );
}

/* Walk a section of the NCA the way the console would: the table says where. */
static const struct nca_header *nca_of( const u8 *data ) { return (const struct nca_header *)data; }

static void check_program( const u8 *data, size_t size, const char *nro_path )
{
    const struct nca_header *header = nca_of( data );
    u64 romfs_start = (u64)header->fs_table[1].media_start_offset * 0x200;
    const struct integrity_level *levels = header->fs_header[1].hash_data.integrity_meta_info.info_level_hash.levels;
    const romfs_header *romfs;
    const romfs_dir *root;
    const u8 *base;
    u32 offset;
    int found = 0;

    assert( header->magic == NCA3_MAGIC );
    assert( header->content_type == NCA_CONTENT_PROGRAM );
    assert( header->size == size );
    /* An exefs and a romfs, in that order, and nothing else. */
    assert( header->fs_header[0].fs_type == NCA_FS_PFS0 && header->fs_header[0].hash_type == NCA_HASH_SHA256 );
    assert( header->fs_header[1].fs_type == NCA_FS_ROMFS && header->fs_header[1].hash_type == NCA_HASH_INTEGRITY );
    assert( header->fs_header[0].encryption_type == NCA_ENCRYPTION_NONE );
    assert( !header->fs_table[2].media_start_offset && !header->fs_table[3].media_start_offset );
    assert( (u64)header->fs_table[0].media_start_offset * 0x200 == sizeof(*header) );
    assert( (u64)header->fs_table[1].media_end_offset * 0x200 <= size );

    /* The sixth level is the image; the five above it are its hashes. */
    base = data + romfs_start + levels[5].logical_offset;
    romfs = (const romfs_header *)base;
    assert( romfs->headerSize == sizeof(*romfs) );
    assert( romfs->fileDataOff == 0x200 );
    root = (const romfs_dir *)(base + romfs->dirTableOff);
    assert( root->childDir == 0xFFFFFFFFu );
    /* Four-byte aligned entries, so they are copied out before they are read. */
    for (offset = root->childFile; offset != 0xFFFFFFFFu;)
    {
        const u8 *at = base + romfs->fileTableOff + offset;
        char name[64] = {0};
        romfs_file file;

        memcpy( &file, at, sizeof(file) );
        assert( file.nameLen < sizeof(name) );
        memcpy( name, at + sizeof(file), file.nameLen );
        if (!strcmp( name, "nextNroPath" ) || !strcmp( name, "nextArgv" ))
        {
            /* With no arguments of its own, the loader is handed the path twice. */
            assert( file.dataSize == strlen( nro_path ) );
            assert( !memcmp( base + romfs->fileDataOff + file.dataOff, nro_path, file.dataSize ) );
            found |= name[4] == 'N' ? 1 : 2;
        }
        offset = file.sibling;
    }
    assert( found == 3 );
}

static void check_exefs_npdm( const u8 *data, u64 tid, int relay )
{
    const struct nca_header *header = nca_of( data );
    u64 start = (u64)header->fs_table[0].media_start_offset * 0x200;
    const struct sha256_data *hash = &header->fs_header[0].hash_data.hierarchical_sha256_data;
    const u8 *pfs0 = data + start + hash->pfs0_layer.offset;
    const struct pfs0_header *pfs = (const struct pfs0_header *)pfs0;
    const struct pfs0_entry *entries = (const struct pfs0_entry *)(pfs0 + sizeof(*pfs));
    const char *strings = (const char *)(entries + pfs->total_files);
    const u8 *files = (const u8 *)strings + pfs->string_table_size;
    const struct npdm_meta *meta = NULL;
    u8 *npdm = NULL;
    const struct npdm_aci0 *aci0;
    const struct npdm_acid *acid;
    u32 i;

    assert( pfs->magic == PFS0_MAGIC );
    assert( pfs->total_files == 2 );
    for (i = 0; i < pfs->total_files; i++)
        if (!strcmp( strings + entries[i].name_offset, "main.npdm" ))
        {
            npdm = malloc( entries[i].data_size );
            assert( npdm );
            memcpy( npdm, files + entries[i].data_offset, entries[i].data_size );
            meta = (const struct npdm_meta *)npdm;
        }
        else if (!strcmp( strings + entries[i].name_offset, "main" ))
        {
            assert( entries[i].data_size == (relay ? wine_nx_game_forwarder_main_size : wine_nx_hbl_main_size) );
            assert( !memcmp( files + entries[i].data_offset, relay ? wine_nx_game_forwarder_main : wine_nx_hbl_main,
                             entries[i].data_size ) );
        }
    assert( meta );
    assert( !memcmp( &meta->magic, "META", 4 ) );
    /* The three bits that decide where the program's address space begins. */
    assert( ((meta->flags >> 1) & 7) == 3 );
    aci0 = (const struct npdm_aci0 *)((const u8 *)meta + meta->aci0_offset);
    acid = (const struct npdm_acid *)((const u8 *)meta + meta->acid_offset);
    assert( !memcmp( &aci0->magic, "ACI0", 4 ) && !memcmp( &acid->magic, "ACID", 4 ) );
    assert( aci0->program_id == tid && acid->program_id_min == tid && acid->program_id_max == tid );
    /* ForceDebug (bit 19) and nothing else in the debug flags, in both the
     * ACI0 and the ACID: what the loader needs on Atmosphere 1.8.0 and later. */
    {
        const u8 *kac[2] = { (const u8 *)aci0 + aci0->kac_offset, (const u8 *)acid + acid->kac_offset };
        u32 sizes[2] = { aci0->kac_size, acid->kac_size };
        int found = 0, cores = 0, k;

        for (k = 0; k < 2; k++)
            for (i = 0; i < sizes[k]; i += 4)
            {
                u32 capability;

                memcpy( &capability, kac[k] + i, 4 );
                if ((capability & 15) == 7)
                {
                    assert( capability == 0x030073f7 );
                    cores++;
                }
                if ((capability & 0x1FFFF) != 0xFFFF) continue;
                assert( capability == (BIT( 19 ) | 0xFFFF) );
                found++;
            }
        assert( found == 2 );
        assert( cores == 2 );
    }
    free( npdm );
}

static void check_control( const u8 *data, size_t size, const char *name, const char *author,
                           const char *isbn, u64 tid )
{
    const struct nca_header *header = nca_of( data );
    u64 start = (u64)header->fs_table[0].media_start_offset * 0x200;
    const struct integrity_level *levels = header->fs_header[0].hash_data.integrity_meta_info.info_level_hash.levels;
    const u8 *base = data + start + levels[5].logical_offset;
    const romfs_header *romfs = (const romfs_header *)base;
    const romfs_dir *root = (const romfs_dir *)(base + romfs->dirTableOff);
    const NacpLanguageEntry *titles = NULL;
    const u8 *nacp = NULL;
    u32 offset;
    int found = 0;

    assert( header->content_type == NCA_CONTENT_CONTROL );
    assert( header->size == size );
    assert( header->fs_header[0].fs_type == NCA_FS_ROMFS );
    for (offset = root->childFile; offset != 0xFFFFFFFFu;)
    {
        const u8 *at = base + romfs->fileTableOff + offset;
        char entry[64] = {0};
        romfs_file file;

        memcpy( &file, at, sizeof(file) );
        memcpy( entry, at + sizeof(file), file.nameLen < sizeof(entry) ? file.nameLen : sizeof(entry) - 1 );
        if (!strcmp( entry, "control.nacp" ))
        {
            nacp = base + romfs->fileDataOff + file.dataOff;
            titles = (const NacpLanguageEntry *)nacp;
            assert( file.dataSize == sizeof(NacpStruct) );
            found |= 1;
        }
        else if (!strcmp( entry, "icon_AmericanEnglish.dat" )) found |= 2;
        offset = file.sibling;
    }
    assert( found == 3 );
    assert( titles && !strcmp( titles[0].name, name ) && !strcmp( titles[15].name, name ) );
    /* The publisher the console shows beside the name, in every language. */
    assert( !strcmp( titles[0].author, author ) && !strcmp( titles[15].author, author ) );
    /* Carried over from the NRO's own NACP rather than built out of zeroes. */
    if (isbn) assert( !memcmp( nacp + 0x3000, isbn, strlen( isbn ) ) );
    /* The languages the NACP claims are the languages there are icons for: the
     * home menu asks for the icon of whichever it picks, and waits for ever for
     * one that is not there. */
    {
        u32 languages;

        memcpy( &languages, nacp + 0x302C, 4 );
        assert( languages == 1 );
    }
    /* The rest is the NRO's, as nacptool wrote it: the ratings are kept, not
     * flattened -- only what sphaira patches, and the language, are changed. */
    assert( !memcmp( nacp + 0x3040, nacptool_ratings, sizeof(nacptool_ratings) ) );
    (void)tid;
}

int main( int argc, char **argv )
{
    static const char nro_path[] = "sdmc:/switch/wine/wine-nx-runtime.nro";
    struct wine_nx_forwarder request;
    char nro_file[512], title_key[256];
    const char *step = NULL;
    size_t program_size, control_size, meta_size;
    u8 *program, *control, *meta;
    u64 tid, plain_hash[SHA256_HASH_SIZE / sizeof(u64)], plain_tid;

    if (argc < 4)
    {
        fprintf( stderr, "usage: %s NPDM ICON OUTDIR\n", argv[0] );
        return 2;
    }
    wine_nx_hbl_npdm = load_file( argv[1], &wine_nx_hbl_npdm_size );
    wine_nx_icon_any = load_file( argv[2], &wine_nx_icon_any_size );
    wine_nx_hbl_main_size = 4096;
    wine_nx_hbl_main = calloc( 1, wine_nx_hbl_main_size );
    wine_nx_game_forwarder_main = (const unsigned char *)"game relay";
    wine_nx_game_forwarder_main_size = 10;
    snprintf( shim_out_dir, sizeof(shim_out_dir), "%s", argv[3] );

    /* The builder reads the NACP out of this, so it has to be a real file. */
    snprintf( nro_file, sizeof(nro_file), "%s/fake.nro", argv[3] );
    write_fake_nro( nro_file, "autorun-nro-nacp" );

    tid = wine_nx_forwarder_title_id( nro_file, NULL );
    assert( (tid >> 56) == 0x05 );
    assert( !(tid & 0xFFF) );
    assert( tid == wine_nx_forwarder_title_id( nro_file, NULL ) );
    assert( tid != wine_nx_forwarder_title_id( nro_file, "argument" ) );
    snprintf( title_key, sizeof(title_key), "%s%s\naddress-space=3\nautorun-forwarder=2", nro_path, nro_path );
    sha256CalculateHash( plain_hash, title_key, strlen( title_key ) );
    assert( wine_nx_forwarder_title_id( nro_path, NULL ) ==
            (0x0500000000000000ull | (plain_hash[0] & 0x00FFFFFFFFFFF000ull)) );
    forwarder_hash( nro_file, NULL, 0, plain_hash );
    plain_tid = 0x0500000000000000ull | (plain_hash[0] & 0x00FFFFFFFFFFF000ull);
    assert( tid != plain_tid );

    memset( &request, 0, sizeof(request) );
    request.nro_path = nro_file;
    request.name = "Autorun";
    request.author = "ticoverse.com";
    request.icon = wine_nx_icon_any;
    request.icon_size = wine_nx_icon_any_size;

    assert( !wine_nx_forwarder_install( &request, &step ) );
    assert( !step );
    assert( registered == 3 );
    /* Its own entry's contents were taken away, and before anything was written. */
    assert( deleted_entity == tid && !deleted_after_write );
    assert( deleted_completely_count == 2 && deleted_completely[1] == tid );
    for (int i = 0; i < deleted_completely_count; i++)
        assert( deleted_completely[i] != plain_tid );
    assert( deleted_completely[0] != tid );

    program = read_nca( 1, &program_size );
    control = read_nca( 2, &control_size );
    meta = read_nca( 3, &meta_size );
    check_program( program, program_size, nro_file );
    check_exefs_npdm( program, tid, 0 );
    check_control( control, control_size, "Autorun", "ticoverse.com", "autorun-nro-nacp", tid );
    assert( nca_of( meta )->content_type == NCA_CONTENT_META );
    assert( nca_of( meta )->fs_header[0].fs_type == NCA_FS_PFS0 );
    assert( nca_of( meta )->size == meta_size );

    free( program );
    free( control );
    free( meta );

    assert( !wine_nx_forwarder_install( &request, &step ) );
    program = read_nca( 4, &program_size );
    check_exefs_npdm( program, tid, 0 );
    free( program );

    request.game_id = 42;
    placeholders = registered = deleted_completely_count = deleted_after_write = 0;
    tid = wine_nx_forwarder_game_title_id( 42 );
    assert( tid != wine_nx_forwarder_title_id( nro_file, NULL ) );
    assert( tid != wine_nx_forwarder_game_title_id( 43 ) );
    assert( !wine_nx_forwarder_install( &request, &step ) );
    assert( deleted_completely_count == 1 && deleted_completely[0] == tid );
    program = read_nca( 1, &program_size );
    check_exefs_npdm( program, tid, 1 );
    {
        const struct nca_header *header = nca_of( program );
        const struct integrity_level *levels = header->fs_header[1].hash_data.integrity_meta_info.info_level_hash.levels;
        const u8 *base = program + (u64)header->fs_table[1].media_start_offset * 0x200 + levels[5].logical_offset;
        const romfs_header *romfs = (const romfs_header *)base;
        const romfs_dir *root = (const romfs_dir *)(base + romfs->dirTableOff);
        romfs_file file;
        struct autorun_game_launch launch;
        memcpy( &file, base + romfs->fileTableOff + root->childFile, sizeof(file) );
        assert( file.nameLen == 4 && file.sibling == 0xffffffff );
        assert( !memcmp( base + romfs->fileTableOff + root->childFile + sizeof(file), "game", 4 ) );
        assert( file.dataSize == sizeof(launch) );
        memcpy( &launch, base + romfs->fileDataOff + file.dataOff, sizeof(launch) );
        assert( autorun_game_launch_valid( &launch, file.dataSize ) && launch.game_id == 42 );
        assert( !autorun_game_launch_valid( &launch, sizeof(launch) - 1 ) );
        launch.version++;
        assert( !autorun_game_launch_valid( &launch, sizeof(launch) ) );
        launch = autorun_game_launch_make( 0 );
        assert( !autorun_game_launch_valid( &launch, sizeof(launch) ) );
    }
    free( program );
    control = read_nca( 2, &control_size );
    check_control( control, control_size, "Autorun", "ticoverse.com", "autorun-nro-nacp", tid );
    free( control );

    puts( "forwarder: title ids, program exefs and romfs, the address space and ForceDebug in the NPDM, the "
          "four-core permissions, control romfs and the NACP it inherits, and taking the old entry away before writing passed" );
    return 0;
}
