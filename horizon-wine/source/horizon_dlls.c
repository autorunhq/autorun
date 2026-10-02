#include "horizon_dlls.h"
#include "json_reader.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>
#include <zstd.h>

#ifdef __SWITCH__
#include <switch.h>
typedef Sha256Context hash_context;
static void hash_begin( hash_context *c ) { sha256ContextCreate( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { sha256ContextUpdate( c, data, size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { sha256ContextGetHash( c, out ); }
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX hash_context;
static void hash_begin( hash_context *c ) { CC_SHA256_Init( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { CC_SHA256_Update( c, data, (CC_LONG)size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { CC_SHA256_Final( out, c ); }
#else
#include <openssl/sha.h>
typedef SHA256_CTX hash_context;
static void hash_begin( hash_context *c ) { SHA256_Init( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { SHA256_Update( c, data, size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { SHA256_Final( out, c ); }
#endif

/* The published manifest is a MB or two; this is room for it to grow. */
#define MANIFEST_MAX   (32u * 1024u * 1024u)
/* The largest file the repository holds is a few tens of MB. */
#define FILE_MAX       (512ull * 1024 * 1024)

const char *const horizon_dlls_folders[] =
{
    "drive_c/windows/system32",
    "drive_c/windows/syswow64",
    "drive_c/dxvk",
    "drive_c/dxvk64",
    "drive_c/vkd3d64",
    "drive_c/physx/Engine/v2.7.1",
    "drive_c/physx/Engine/v2.7.3",
    "drive_c/physx/Engine/v2.7.4",
    "drive_c/physx/Engine/v2.7.5",
    "drive_c/physx/Engine/v2.7.6",
    "drive_c/physx/Engine/v2.8.0",
    "drive_c/physx/Engine/v2.8.1",
    "drive_c/physx/Engine/v2.8.3",
    NULL
};

/***********************************************************************
 * Reading a manifest
 */

static int folder_index( const char *path )
{
    int i;

    for (i = 0; horizon_dlls_folders[i]; i++)
        if (!strcmp( path, horizon_dlls_folders[i] )) return i;
    return -1;
}

/* A file name as the repository has them: no folders, nothing hidden. */
static int valid_name( const char *name )
{
    const char *p;

    if (!name[0] || name[0] == '.' || strlen( name ) >= 64) return 0;
    for (p = name; *p; p++)
        if (!isalnum( (unsigned char)*p ) && !strchr( "._-+", *p )) return 0;
    return 1;
}

static int valid_hash( const char *text )
{
    size_t i;

    if (strlen( text ) != 64) return 0;
    for (i = 0; i < 64; i++) if (!isxdigit( (unsigned char)text[i] )) return 0;
    return 1;
}

/* A URL from the repository, for the file it says it is. */
static int official_url( const char *url, const char *folder, const char *name )
{
    char tail[160];
    size_t length = strlen( url ), tail_length;

    if (strncmp( url, HORIZON_DLLS_RAW, sizeof(HORIZON_DLLS_RAW) - 1 ) || strstr( url, ".." )) return 0;
    tail_length = (size_t)snprintf( tail, sizeof(tail), "/switch/wine/%s/%s", folder, name );
    return tail_length < sizeof(tail) && length > tail_length && !strcmp( url + length - tail_length, tail );
}

/* The compressed copy's URL, for the file it says it is. */
static int official_packed_url( const char *url, const char *folder, const char *name )
{
    char tail[176];
    size_t length = strlen( url ), tail_length;

    if (strncmp( url, HORIZON_DLLS_RAW, sizeof(HORIZON_DLLS_RAW) - 1 ) || strstr( url, ".." )) return 0;
    tail_length = (size_t)snprintf( tail, sizeof(tail), "/compressed/switch/wine/%s/%s.z", folder, name );
    return tail_length < sizeof(tail) && length > tail_length && !strcmp( url + length - tail_length, tail );
}

static int grow( void **array, unsigned int *capacity, unsigned int count, size_t size )
{
    unsigned int wanted;
    void *grown;

    if (count < *capacity) return 1;
    wanted = *capacity ? *capacity * 2 : 256;
    if (!(grown = realloc( *array, (size_t)wanted * size ))) return 0;
    *array = grown;
    *capacity = wanted;
    return 1;
}

static int category_index( struct horizon_dll_manifest *m, const char *key )
{
    unsigned int i;

    for (i = 0; i < m->category_count; i++)
        if (!strcmp( m->categories[i].key, key )) return i;
    if (m->category_count >= HORIZON_DLLS_CATEGORIES || strlen( key ) >= sizeof(m->categories[0].key)) return -1;
    snprintf( m->categories[m->category_count].key, sizeof(m->categories[0].key), "%s", key );
    m->categories[m->category_count].description[0] = 0;
    return m->category_count++;
}

/* Each key of an object in turn: calls field with the parser at its value. */
static int object( struct parser *p, int (*field)( struct parser *p, const char *key, void *data ), void *data )
{
    char key[64];

    if (!consume( p, '{' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
    for (;;)
    {
        if (!json_string( p, key, sizeof(key) ) || !consume( p, ':' )) return 0;
        if (!field( p, key, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

static int array( struct parser *p, int (*item)( struct parser *p, void *data ), void *data )
{
    if (!consume( p, '[' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
    for (;;)
    {
        if (!item( p, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

struct reading
{
    struct horizon_dll_manifest *manifest;
    const char *const *features;
    size_t feature_count;
    struct horizon_dll_file *file;
    char path[64], category[32];
    unsigned int seen;
};

static int has_feature( const struct reading *r, const char *feature )
{
    size_t i;

    for (i = 0; i < r->feature_count; i++)
        if (!strcmp( r->features[i], feature )) return 1;
    return 0;
}

static int read_feature( struct parser *p, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    char feature[96];

    if (!json_string( p, feature, sizeof(feature) )) return 0;
    /* Kept, so the card's manifest says what its files need too. */
    if (!grow( (void **)&m->features, &m->feature_capacity, m->feature_count, sizeof(*m->features) )) return 0;
    snprintf( m->features[m->feature_count++].name, sizeof(m->features[0].name), "%s", feature );
    r->file->feature_count++;
    if (!has_feature( r, feature ) && r->file->satisfied)
    {
        r->file->satisfied = 0;
        snprintf( r->file->missing, sizeof(r->file->missing), "%s", feature );
    }
    return 1;
}

static int read_requires( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    char flavor[16];

    if (!strcmp( key, "features" )) return array( p, read_feature, r );
    if (!strcmp( key, "flavor" ))
    {
        if (!json_string( p, flavor, sizeof(flavor) )) return 0;
        /* A record written for another runtime's files is not this one's. */
        if (strcmp( flavor, HORIZON_DLLS_FLAVOR ) && r->manifest->schema >= 2 && r->file->satisfied)
        {
            r->file->satisfied = 0;
            snprintf( r->file->missing, sizeof(r->file->missing), "flavor %s", flavor );
        }
        return 1;
    }
    return skip_value( p );
}

static int read_class_field( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_class *c = data;

    if (!strcmp( key, "clsid" )) return json_string( p, c->clsid, sizeof(c->clsid) );
    if (!strcmp( key, "name" )) return json_string( p, c->name, sizeof(c->name) );
    if (!strcmp( key, "threading" )) return json_string( p, c->threading, sizeof(c->threading) );
    return skip_value( p );
}

static int read_class( struct parser *p, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    struct horizon_dll_class *c;
    const char *s;

    if (!grow( (void **)&m->classes, &m->class_capacity, m->class_count, sizeof(*m->classes) )) return 0;
    c = &m->classes[m->class_count];
    memset( c, 0, sizeof(*c) );
    if (!object( p, read_class_field, c )) return 0;
    /* A class id is hex and dashes, and the rest goes into a .reg file. */
    if (strlen( c->clsid ) != 36) return 0;
    for (s = c->clsid; *s; s++) if (!isxdigit( (unsigned char)*s ) && *s != '-') return 0;
    for (s = c->name; *s; s++) if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r') return 0;
    for (s = c->threading; *s; s++) if (!isalpha( (unsigned char)*s )) return 0;
    if (!c->threading[0]) snprintf( c->threading, sizeof(c->threading), "Both" );
    m->class_count++;
    r->file->class_count++;
    return 1;
}

static int read_packed( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct horizon_dll_file *f = r->file;
    char encoding[16];

    if (!strcmp( key, "encoding" ))
    {
        if (!json_string( p, encoding, sizeof(encoding) )) return 0;
        /* One this does not know how to unpack is not used. */
        if (strcmp( encoding, "zlib" )) r->seen |= 64;
        return 1;
    }
    if (!strcmp( key, "size" )) return json_uint( p, &f->packed_size );
    if (!strcmp( key, "sha256" )) return json_string( p, f->packed_sha256, sizeof(f->packed_sha256) );
    if (!strcmp( key, "url" )) return json_string( p, f->packed_url, sizeof(f->packed_url) );
    return skip_value( p );
}

static int read_file_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct horizon_dll_file *f = r->file;
    unsigned long long value;

    if (!strcmp( key, "name" )) { r->seen |= 1; return json_string( p, f->name, sizeof(f->name) ); }
    if (!strcmp( key, "path" )) { r->seen |= 2; return json_string( p, r->path, sizeof(r->path) ); }
    if (!strcmp( key, "arch" )) return json_string( p, f->arch, sizeof(f->arch) );
    /* schema 1 called the category a group */
    if (!strcmp( key, "category" ) || (!strcmp( key, "group" ) && !r->category[0]))
        return json_string( p, r->category, sizeof(r->category) );
    if (!strcmp( key, "version" ))
    {
        if (!json_uint( p, &value ) || value > 0xffffffffu) return 0;
        f->version = (unsigned int)value;
        return 1;
    }
    if (!strcmp( key, "size" )) { r->seen |= 4; return json_uint( p, &f->size ); }
    if (!strcmp( key, "sha256" )) { r->seen |= 8; return json_string( p, f->sha256, sizeof(f->sha256) ); }
    if (!strcmp( key, "url" )) { r->seen |= 16; return json_string( p, f->url, sizeof(f->url) ); }
    if (!strcmp( key, "compressed" )) return object( p, read_packed, r );
    if (!strcmp( key, "requires" ))
    {
        f->feature_first = r->manifest->feature_count;
        f->feature_count = 0;
        return object( p, read_requires, r );
    }
    if (!strcmp( key, "classes" ))
    {
        f->class_first = r->manifest->class_count;
        f->class_count = 0;
        return array( p, read_class, r );
    }
    if (!strcmp( key, "registry" ))
    {
        struct parser end = *p;
        size_t capacity;
        char *registry;
        if (f->registry || !skip_value( &end )) return 0;
        capacity = end.p - p->p + 1;
        if (capacity > 65536 || !(registry = malloc( capacity ))) return 0;
        if (!json_string( p, registry, capacity )) { free( registry ); return 0; }
        f->registry = registry;
        return 1;
    }
    return skip_value( p );
}

static int read_file( struct parser *p, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    struct horizon_dll_file *f;
    int folder, category;

    if (!grow( (void **)&m->files, &m->capacity, m->count, sizeof(*m->files) )) return 0;
    f = r->file = &m->files[m->count++];
    memset( f, 0, sizeof(*f) );
    f->satisfied = 1;
    f->class_first = m->class_count;
    f->feature_first = m->feature_count;
    r->path[0] = r->category[0] = 0;
    r->seen = 0;
    if (!object( p, read_file_field, r )) return 0;
    if ((r->seen & 31) != 31 || !valid_name( f->name ) || !r->path[0] || strstr( r->path, ".." ) ||
        !f->size || f->size > FILE_MAX || !valid_hash( f->sha256 ) || !official_url( f->url, r->path, f->name ))
        return 0;
    /* A folder a later Autorun knows and this one does not: the file is for
     * that Autorun, and is left out rather than the whole manifest with it. */
    if ((folder = folder_index( r->path )) < 0)
    {
        m->class_count = f->class_first;
        m->feature_count = f->feature_first;
        m->skipped++;
        free( f->registry );
        m->count--;
        return 1;
    }
    /* A compressed copy that is not all there, or not the repository's, is
     * not one: the file itself is downloaded instead. */
    if ((r->seen & 64) || !f->packed_size || f->packed_size > FILE_MAX || !valid_hash( f->packed_sha256 ) ||
        !official_packed_url( f->packed_url, r->path, f->name ))
    {
        if (f->packed_url[0] && !(r->seen & 64) && !official_packed_url( f->packed_url, r->path, f->name )) return 0;
        f->packed_size = 0;
        f->packed_sha256[0] = f->packed_url[0] = 0;
    }
    for (char *c = f->packed_sha256; *c; c++) *c = tolower( (unsigned char)*c );
    for (char *c = f->sha256; *c; c++) *c = tolower( (unsigned char)*c );
    if ((category = category_index( m, r->category[0] ? r->category : "system" )) < 0) return 0;
    f->folder = (unsigned char)folder;
    f->category = (unsigned char)category;
    return 1;
}

static int read_category( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_manifest *m = data;
    int index = category_index( m, key );

    if (index < 0) return 0;
    return json_string( p, m->categories[index].description, sizeof(m->categories[0].description) );
}

static int read_source( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_manifest *m = data;

    if (!strcmp( key, "commit" )) return json_string( p, m->commit, sizeof(m->commit) );
    if (!strcmp( key, "wine" )) return json_string( p, m->wine, sizeof(m->wine) );
    return skip_value( p );
}

static int read_top( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    unsigned long long schema;

    if (!strcmp( key, "schema" ))
    {
        if (!json_uint( p, &schema ) || !schema || schema > 2) return 0;
        m->schema = (int)schema;
        return 1;
    }
    if (!strcmp( key, "flavor" )) return json_string( p, m->flavor, sizeof(m->flavor) );
    if (!strcmp( key, "source" )) return object( p, read_source, m );
    if (!strcmp( key, "categories" )) return object( p, read_category, m );
    if (!strcmp( key, "files" )) return m->schema ? array( p, read_file, r ) : 0;
    return skip_value( p );
}

enum horizon_dlls_result horizon_dlls_parse( const char *text, size_t size, const char *const *features,
                                             size_t feature_count, struct horizon_dll_manifest *out )
{
    struct parser p = { (const unsigned char *)text, (const unsigned char *)text + size, 0 };
    struct reading r = { .manifest = out, .features = features, .feature_count = feature_count };
    unsigned int i, j;

    memset( out, 0, sizeof(*out) );
    if (!text || !object( &p, read_top, &r )) goto invalid;
    whitespace( &p );
    if (p.p != p.end || !out->schema || !out->flavor[0]) goto invalid;
    /* One file per place on the card. */
    for (i = 0; i < out->count; i++)
        for (j = i + 1; j < out->count; j++)
            if (out->files[i].folder == out->files[j].folder && !strcasecmp( out->files[i].name, out->files[j].name ))
                goto invalid;
    return HORIZON_DLLS_OK;
invalid:
    horizon_dlls_free( out );
    return HORIZON_DLLS_INVALID;
}

void horizon_dlls_free( struct horizon_dll_manifest *m )
{
    for (unsigned int i = 0; i < m->count; i++) free( m->files[i].registry );
    free( m->files );
    free( m->classes );
    free( m->features );
    memset( m, 0, sizeof(*m) );
}

static int join( char *out, size_t size, const char *root, const char *a, const char *b )
{
    int length = b ? snprintf( out, size, "%s/%s/%s", root, a, b ) : snprintf( out, size, "%s/%s", root, a );
    return length > 0 && (size_t)length < size;
}

enum horizon_dlls_result horizon_dlls_load( const char *root, const char *const *features, size_t feature_count,
                                            struct horizon_dll_manifest *out )
{
    char path[768];
    enum horizon_dlls_result result;
    FILE *file;
    char *text;
    long size;

    memset( out, 0, sizeof(*out) );
    if (!join( path, sizeof(path), root, HORIZON_DLLS_MANIFEST, NULL )) return HORIZON_DLLS_INVALID;
    if (!(file = fopen( path, "rb" ))) return errno == ENOENT ? HORIZON_DLLS_NOT_FOUND : HORIZON_DLLS_IO;
    if (fseek( file, 0, SEEK_END ) || (size = ftell( file )) < 0 || size > (long)MANIFEST_MAX ||
        fseek( file, 0, SEEK_SET ))
    {
        fclose( file );
        return HORIZON_DLLS_INVALID;
    }
    if (!(text = malloc( size + 1 ))) { fclose( file ); return HORIZON_DLLS_MEMORY; }
    if (fread( text, 1, size, file ) != (size_t)size) { free( text ); fclose( file ); return HORIZON_DLLS_IO; }
    fclose( file );
    text[size] = 0;
    result = horizon_dlls_parse( text, size, features, feature_count, out );
    free( text );
    return result;
}

struct memory
{
    char *data;
    size_t size, capacity;
};

static int to_memory( void *context, const void *data, size_t size )
{
    struct memory *m = context;
    size_t wanted;
    char *grown;

    if (size > MANIFEST_MAX - m->size) return 0;
    if (m->size + size + 1 > m->capacity)
    {
        wanted = m->capacity ? m->capacity : 65536;
        while (wanted < m->size + size + 1) wanted *= 2;
        if (!(grown = realloc( m->data, wanted ))) return 0;
        m->data = grown;
        m->capacity = wanted;
    }
    memcpy( m->data + m->size, data, size );
    m->size += size;
    m->data[m->size] = 0;
    return 1;
}

enum horizon_dlls_result horizon_dlls_fetch_manifest( const struct horizon_dlls_transport *transport,
        const char *url, const char *const *features, size_t feature_count,
        struct horizon_dll_manifest *out, horizon_dlls_progress progress, void *opaque )
{
    struct memory body = {0};
    enum horizon_dlls_result result;

    memset( out, 0, sizeof(*out) );
    result = transport->fetch( transport->opaque, url, to_memory, &body, progress, opaque );
    if (result == HORIZON_DLLS_OK)
    {
        result = horizon_dlls_parse( body.data, body.size, features, feature_count, out );
        if (result == HORIZON_DLLS_OK && (out->schema < 2 || strcmp( out->flavor, HORIZON_DLLS_FLAVOR )))
        {
            horizon_dlls_free( out );
            result = HORIZON_DLLS_FLAVOR_MISMATCH;
        }
    }
    free( body.data );
    return result;
}

/***********************************************************************
 * What the card has
 */

static void hex( const unsigned char hash[32], char out[65] )
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++)
    {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[64] = 0;
}

/* The SHA-256 of a file on the card; 0 when it cannot be read. */
static int hash_file( const char *path, char out[65], const char *what, unsigned long long *done,
                      unsigned long long total, horizon_dlls_progress progress, void *opaque, int *cancelled )
{
    const size_t buffer_size = 256 * 1024;
    unsigned char hash[32], *buffer;
    hash_context context;
    FILE *file;
    size_t got;

    if (!(buffer = malloc( buffer_size ))) return 0;
    if (!(file = fopen( path, "rb" ))) { free( buffer ); return 0; }
    hash_begin( &context );
    while ((got = fread( buffer, 1, buffer_size, file )) > 0)
    {
        hash_add( &context, buffer, got );
        *done += got;
        if (progress && progress( opaque, what, *done, total )) { *cancelled = 1; break; }
    }
    fclose( file );
    free( buffer );
    if (*cancelled) return 0;
    hash_end( &context, hash );
    hex( hash, out );
    return 1;
}

static const struct horizon_dll_file *find( const struct horizon_dll_manifest *m, unsigned int folder,
                                            const char *name )
{
    unsigned int i;

    if (!m) return NULL;
    for (i = 0; i < m->count; i++)
        if (m->files[i].folder == folder && !strcasecmp( m->files[i].name, name )) return &m->files[i];
    return NULL;
}

enum horizon_dlls_result horizon_dlls_plan( const char *root, struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, int verify, horizon_dlls_progress progress, void *opaque,
        struct horizon_dlls_plan *plan )
{
    unsigned long long to_hash = 0, hashed = 0;
    unsigned int i;
    int cancelled = 0;

    memset( plan, 0, sizeof(*plan) );
    /* What has to be read to be known: everything when verifying, and a file
     * the card has with no record of where it came from. */
    for (i = 0; i < remote->count; i++)
    {
        struct horizon_dll_file *f = &remote->files[i];
        const struct horizon_dll_file *known = find( local, f->folder, f->name );
        char path[768];
        struct stat st;

        f->state = HORIZON_DLL_NEW;
        if (!join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name )) return HORIZON_DLLS_INVALID;
        if (!stat( path, &st ) && S_ISREG( st.st_mode ) && (unsigned long long)st.st_size == f->size &&
            (verify || !known))
            to_hash += f->size;
    }
    for (i = 0; i < remote->count; i++)
    {
        struct horizon_dll_file *f = &remote->files[i];
        struct horizon_dlls_category_plan *c = &plan->categories[f->category];
        const struct horizon_dll_file *known = find( local, f->folder, f->name );
        char path[768], digest[65], what[96];
        struct stat st;
        int present;

        join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name );
        present = !stat( path, &st ) && S_ISREG( st.st_mode );
        if (!f->satisfied) f->state = HORIZON_DLL_UNSUPPORTED;
        else if (!present) f->state = HORIZON_DLL_NEW;
        else if ((unsigned long long)st.st_size != f->size) f->state = HORIZON_DLL_CHANGED;
        else if (known && !verify) f->state = strcmp( known->sha256, f->sha256 ) ? HORIZON_DLL_CHANGED : HORIZON_DLL_CURRENT;
        else
        {
            snprintf( what, sizeof(what), "Checking %s", f->name );
            if (!hash_file( path, digest, what, &hashed, to_hash, progress, opaque, &cancelled ))
            {
                if (cancelled) return HORIZON_DLLS_CANCELLED;
                f->state = HORIZON_DLL_CHANGED;
            }
            else f->state = strcmp( digest, f->sha256 ) ? HORIZON_DLL_CHANGED : HORIZON_DLL_CURRENT;
        }
        plan->files++;
        c->files++;
        plan->bytes += f->size;
        c->bytes += f->size;
        switch (f->state)
        {
        case HORIZON_DLL_CURRENT: plan->current++; c->current++; break;
        case HORIZON_DLL_UNSUPPORTED: plan->unsupported++; c->unsupported++; break;
        default:
            plan->pending++; c->pending++;
            plan->pending_bytes += f->size; c->pending_bytes += f->size;
            plan->download_bytes += f->packed_size ? f->packed_size : f->size;
            c->download_bytes += f->packed_size ? f->packed_size : f->size;
            break;
        }
    }
    if (local)
        for (i = 0; i < local->count; i++)
            if (!find( remote, local->files[i].folder, local->files[i].name )) plan->removed++;
    return HORIZON_DLLS_OK;
}

/***********************************************************************
 * Bringing the card up to date
 */

/* The card is written by one download at a time, each file whole in one
 * write: eight writing their files a piece at a time held a first install to
 * 13 MB/s on hardware, most of each download spent there, while one file
 * written in one go is laid down in one run. The downloads go on meanwhile. */
static pthread_mutex_t card_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_whole( const char *path, const unsigned char *data, size_t size )
{
    FILE *file;
    int ok;

    pthread_mutex_lock( &card_lock );
    if ((file = fopen( path, "wb" )))
    {
        /* No stdio buffer to copy through: the file's bytes go as they are. */
        setvbuf( file, NULL, _IONBF, 0 );
        ok = fwrite( data, 1, size, file ) == size;
        ok = !fclose( file ) && ok;
        if (!ok) remove( path );
    }
    else ok = 0;
    pthread_mutex_unlock( &card_lock );
    return ok;
}

/* Put a checked file in place: straight where it goes when the card lacks it,
 * beside the old one and renamed over it otherwise, so a failure never costs
 * the card a working DLL. */
static int put_file( const char *path, const unsigned char *data, size_t size )
{
    char part[800];
    struct stat st;
    int ok;

    if (stat( path, &st )) return write_whole( path, data, size );
    if ((size_t)snprintf( part, sizeof(part), "%s.part", path ) >= sizeof(part) ||
        !write_whole( part, data, size ))
        return 0;
    pthread_mutex_lock( &card_lock );
    ok = (!remove( path ) || errno == ENOENT) && !rename( part, path );
    pthread_mutex_unlock( &card_lock );
    if (!ok) remove( part );
    return ok;
}

struct download
{
    unsigned char *data;    /* the file, as it comes */
    hash_context hash;
    unsigned long long size, expected;
    int failed;
    /* A compressed download: what came, and the stream unpacking it. */
    int packed, ended;
    z_stream stream;
    hash_context packed_hash;
    unsigned long long packed_size, packed_expected;
    unsigned char *out;     /* what the stream unpacks into, each download its own */
};

#define UNPACK_BUFFER (128 * 1024)

static int write_out( struct download *d, const void *data, size_t size )
{
    if (size > d->expected - d->size) { d->failed = HORIZON_DLLS_INVALID; return 0; }
    memcpy( d->data + d->size, data, size );
    hash_add( &d->hash, data, size );
    d->size += size;
    return 1;
}

static int to_file( void *context, const void *data, size_t size )
{
    struct download *d = context;
    int status;

    if (!d->packed) return write_out( d, data, size );
    if (d->ended || size > d->packed_expected - d->packed_size) { d->failed = HORIZON_DLLS_INVALID; return 0; }
    hash_add( &d->packed_hash, data, size );
    d->packed_size += size;
    d->stream.next_in = (Bytef *)data;
    d->stream.avail_in = (uInt)size;
    do
    {
        d->stream.next_out = d->out;
        d->stream.avail_out = UNPACK_BUFFER;
        status = inflate( &d->stream, Z_NO_FLUSH );
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
        {
            d->failed = HORIZON_DLLS_HASH;
            return 0;
        }
        if (!write_out( d, d->out, UNPACK_BUFFER - d->stream.avail_out )) return 0;
        if (status == Z_STREAM_END) { d->ended = 1; break; }
    } while (d->stream.avail_in || !d->stream.avail_out);
    /* Anything after the end of the stream is not the repository's. */
    if (d->ended && d->stream.avail_in) { d->failed = HORIZON_DLLS_INVALID; return 0; }
    return 1;
}

static int make_folders( const char *root, const char *folder )
{
    char path[768];
    char *slash;

    if (!join( path, sizeof(path), root, folder, NULL )) return 0;
    for (slash = path + strlen( root ) + 1; (slash = strchr( slash, '/' )); slash++)
    {
        *slash = 0;
        if (mkdir( path, 0777 ) && errno != EEXIST) return 0;
        *slash = '/';
    }
    return !mkdir( path, 0777 ) || errno == EEXIST;
}

static unsigned long long now_us( void )
{
    struct timespec t;

    clock_gettime( CLOCK_MONOTONIC, &t );
    return (unsigned long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* A download thread on a core of its own: Horizon runs a thread on the core it
 * was made on and does not move it, so eight made on one would take turns there
 * decrypting, unpacking and hashing while the others idle. */
static void spread_thread( unsigned int index )
{
#ifdef __SWITCH__
    svcSetThreadCoreMask( threadGetCurHandle(), index % 3, 7 );
#else
    (void)index;
#endif
}

/* How often a download that failed on the way -- a name that did not resolve,
 * a handshake that did not finish, a connection that dropped -- is tried again. */
#define ATTEMPTS 3

static void pause_before( unsigned int attempt )
{
    struct timespec wait = { 0, 400000000L * attempt };

    nanosleep( &wait, NULL );
}

static enum horizon_dlls_result download_file( const char *root, const struct horizon_dll_file *f,
        const struct horizon_dlls_transport *transport, horizon_dlls_progress progress, void *opaque )
{
    char path[768], digest[65];
    unsigned char hash[32], *data;
    struct download d;
    enum horizon_dlls_result result = HORIZON_DLLS_NETWORK;
    unsigned int attempt;

    if (!make_folders( root, horizon_dlls_folders[f->folder] ) ||
        !join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name ))
        return HORIZON_DLLS_IO;
    /* The file comes into memory, is checked there, and only then goes to the
     * card, whole: nothing half-written is ever on it. */
    if (!(data = malloc( f->size ))) return HORIZON_DLLS_MEMORY;
    for (attempt = 0; attempt < ATTEMPTS; attempt++)
    {
        if (attempt) pause_before( attempt );
        memset( &d, 0, sizeof(d) );
        d.data = data;
        d.expected = f->size;
        d.packed = f->packed_size != 0;
        d.packed_expected = f->packed_size;
        if (d.packed && (!(d.out = malloc( UNPACK_BUFFER )) || inflateInit( &d.stream ) != Z_OK))
        {
            free( d.out );
            free( data );
            return HORIZON_DLLS_MEMORY;
        }
        hash_begin( &d.hash );
        if (d.packed) hash_begin( &d.packed_hash );
        result = transport->fetch( transport->opaque, d.packed ? f->packed_url : f->url, to_file, &d, progress, opaque );
        if (d.packed) inflateEnd( &d.stream );
        free( d.out );
        if (result != HORIZON_DLLS_NETWORK) break;
    }
    if (result == HORIZON_DLLS_OK && d.failed) result = d.failed;
    if (result == HORIZON_DLLS_OK && d.packed)
    {
        hash_end( &d.packed_hash, hash );
        hex( hash, digest );
        if (d.packed_size != f->packed_size || !d.ended) result = HORIZON_DLLS_INVALID;
        else if (strcmp( digest, f->packed_sha256 )) result = HORIZON_DLLS_HASH;
    }
    if (result == HORIZON_DLLS_OK && d.size != f->size) result = HORIZON_DLLS_INVALID;
    if (result == HORIZON_DLLS_OK)
    {
        hash_end( &d.hash, hash );
        hex( hash, digest );
        if (strcmp( digest, f->sha256 )) result = HORIZON_DLLS_HASH;
    }
    if (result == HORIZON_DLLS_OK && !put_file( path, data, f->size )) result = HORIZON_DLLS_IO;
    free( data );
    return result;
}

struct text
{
    char *data;
    size_t size, capacity;
    int failed;
};

static void add( struct text *t, const char *format, ... ) __attribute__((format(printf, 2, 3)));
static void add( struct text *t, const char *format, ... )
{
    va_list args;
    size_t wanted;
    int length;
    char *grown;

    if (t->failed) return;
    for (;;)
    {
        va_start( args, format );
        length = vsnprintf( t->data ? t->data + t->size : NULL, t->data ? t->capacity - t->size : 0, format, args );
        va_end( args );
        if (length < 0) { t->failed = 1; return; }
        if (t->data && t->size + (size_t)length < t->capacity) { t->size += length; return; }
        wanted = (t->capacity ? t->capacity * 2 : (size_t)65536) + (size_t)length;
        if (!(grown = realloc( t->data, wanted )))
        {
            t->failed = 1;
            return;
        }
        t->capacity = wanted;
        t->data = grown;
    }
}

/* Names and descriptions come from a manifest this parsed; quotes and
 * backslashes are all a JSON string needs escaped of them. */
static void add_string( struct text *t, const char *s )
{
    add( t, "\"" );
    for (; *s; s++)
    {
        if (*s == '"' || *s == '\\') add( t, "\\%c", *s );
        else if ((unsigned char)*s < 0x20) add( t, "\\u%04x", *s );
        else add( t, "%c", *s );
    }
    add( t, "\"" );
}

struct kept
{
    const struct horizon_dll_manifest *from;
    const struct horizon_dll_file *file;
};

static int write_atomically( const char *root, const char *name, const char *data, size_t size )
{
    char path[768], part[800];
    FILE *file;
    int ok;

    if (!join( path, sizeof(path), root, name, NULL ) ||
        (size_t)snprintf( part, sizeof(part), "%s.part", path ) >= sizeof(part) ||
        !make_folders( root, "horizon-dlls" ))
        return 0;
    if (!(file = fopen( part, "wb" ))) return 0;
    ok = fwrite( data, 1, size, file ) == size && !fflush( file ) && !fsync( fileno( file ) );
    ok = !fclose( file ) && ok;
    if (ok) ok = (!remove( path ) || errno == ENOENT) && !rename( part, path );
    if (!ok) remove( part );
    return ok;
}

/* The card's manifest, of the files it holds, and the classes they serve. */
static int write_record( const char *root, const struct horizon_dll_manifest *remote, const struct kept *kept,
                         unsigned int count )
{
    struct text manifest = {0}, classes = {0};
    unsigned int i, j, pass, written = 0, claimed_capacity = 0;
    const char **claimed;
    int ok;

    for (i = 0; i < count; i++) claimed_capacity += kept[i].file->class_count;
    if (!(claimed = calloc( claimed_capacity + 1, sizeof(*claimed) ))) return 0;

    add( &manifest, "{\"schema\":2,\"flavor\":" );
    add_string( &manifest, HORIZON_DLLS_FLAVOR );
    add( &manifest, ",\"source\":{\"commit\":" );
    add_string( &manifest, remote->commit );
    add( &manifest, ",\"wine\":" );
    add_string( &manifest, remote->wine );
    add( &manifest, "},\"categories\":{" );
    for (i = 0; i < remote->category_count; i++)
    {
        add( &manifest, "%s", i ? "," : "" );
        add_string( &manifest, remote->categories[i].key );
        add( &manifest, ":" );
        add_string( &manifest, remote->categories[i].description );
    }
    add( &manifest, "},\"files\":[" );
    for (i = 0; i < count; i++)
    {
        const struct horizon_dll_file *f = kept[i].file;
        const struct horizon_dll_manifest *m = kept[i].from;

        add( &manifest, "%s\n{\"name\":", i ? "," : "" );
        add_string( &manifest, f->name );
        add( &manifest, ",\"path\":" );
        add_string( &manifest, horizon_dlls_folders[f->folder] );
        add( &manifest, ",\"arch\":" );
        add_string( &manifest, f->arch );
        add( &manifest, ",\"category\":" );
        add_string( &manifest, m->categories[f->category].key );
        add( &manifest, ",\"version\":%u,\"size\":%llu,\"sha256\":\"%s\",\"url\":", f->version, f->size, f->sha256 );
        add_string( &manifest, f->url );
        add( &manifest, ",\"requires\":{\"flavor\":\"%s\",\"features\":[", HORIZON_DLLS_FLAVOR );
        for (j = 0; j < f->feature_count; j++)
        {
            add( &manifest, "%s", j ? "," : "" );
            add_string( &manifest, m->features[f->feature_first + j].name );
        }
        add( &manifest, "]},\"classes\":[" );
        for (j = 0; j < f->class_count; j++)
        {
            const struct horizon_dll_class *c = &m->classes[f->class_first + j];

            add( &manifest, "%s{\"clsid\":\"%s\",\"name\":", j ? "," : "", c->clsid );
            add_string( &manifest, c->name );
            add( &manifest, ",\"threading\":\"%s\"}", c->threading );
        }
        add( &manifest, "]" );
        if (f->registry)
        {
            add( &manifest, ",\"registry\":" );
            add_string( &manifest, f->registry );
        }
        add( &manifest, "}" );
    }
    add( &manifest, "\n]}\n" );

    /* The first file to claim a class keeps it, 32-bit ones first, as the
     * repository's own classes.reg has them. */
    add( &classes, "WINE REGISTRY Version 2\n;; The COM classes the DLLs on this card serve, from their manifest.\n\n" );
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < count; i++)
        {
            const struct horizon_dll_file *f = kept[i].file;
            const struct horizon_dll_manifest *m = kept[i].from;
            const char *dot = strrchr( f->name, '.' );

            if ((pass == 0) != !strcmp( f->arch, "i386" )) continue;
            for (j = 0; j < f->class_count; j++)
            {
                const struct horizon_dll_class *c = &m->classes[f->class_first + j];
                unsigned int k;

                for (k = 0; k < written; k++) if (!strcasecmp( claimed[k], c->clsid )) break;
                if (k < written) continue;
                if (written < claimed_capacity) claimed[written++] = c->clsid;
                add( &classes, ";; %.*s: %s\n[Software\\\\Classes\\\\CLSID\\\\{%s}\\\\InprocServer32]\n"
                     "@=\"%s\"\n\"ThreadingModel\"=\"%s\"\n\n",
                     (int)(dot ? dot - f->name : (long)strlen( f->name )), f->name, c->name, c->clsid,
                     f->name, c->threading );
            }
            if (f->registry) add( &classes, "%s\n", f->registry );
        }
    ok = !manifest.failed && !classes.failed &&
         write_atomically( root, HORIZON_DLLS_CLASSES, classes.data, classes.size ) &&
         write_atomically( root, HORIZON_DLLS_MANIFEST, manifest.data, manifest.size );
    free( manifest.data );
    free( classes.data );
    free( claimed );
    return ok;
}

/* A file the repository no longer has goes, unless someone put other bytes
 * there since, which are theirs. */
static void remove_stale( const char *root, const struct horizon_dll_file *f )
{
    char path[768], digest[65];
    unsigned long long done = 0;
    int cancelled = 0;

    if (!join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name )) return;
    if (hash_file( path, digest, NULL, &done, 0, NULL, NULL, &cancelled ) && !strcmp( digest, f->sha256 ))
        remove( path );
}

/* What the card holds now, written as its manifest and classes.reg: the
 * repository's files it has (done), and of the rest, those it had and still
 * has. remove_old takes away what the repository no longer has. Frees done. */
static enum horizon_dlls_result record( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, unsigned char *done, enum horizon_dlls_result result,
        int remove_old )
{
    struct kept *kept = calloc( remote->count + (local ? local->count : 0) + 1, sizeof(*kept) );
    unsigned int i, count = 0;

    if (!kept) { free( done ); return HORIZON_DLLS_MEMORY; }
    for (i = 0; i < remote->count; i++)
    {
        const struct horizon_dll_file *f = &remote->files[i], *before = find( local, f->folder, f->name );

        if (done[i]) kept[count++] = (struct kept){ remote, f };
        else if (before) kept[count++] = (struct kept){ local, before };
    }
    if (local)
        for (i = 0; i < local->count; i++)
        {
            const struct horizon_dll_file *f = &local->files[i];

            if (find( remote, f->folder, f->name )) continue;
            if (remove_old) remove_stale( root, f );
            else kept[count++] = (struct kept){ local, f };
        }
    if (!write_record( root, remote, kept, count ) && result == HORIZON_DLLS_OK) result = HORIZON_DLLS_IO;
    free( kept );
    free( done );
    return result;
}

/* Several downloads at once, one connection each: an update is hundreds of
 * small files, and one at a time the wait between them is most of the time. */
struct apply
{
    pthread_mutex_t lock;
    const char *root;
    const struct horizon_dll_manifest *remote;
    const struct horizon_dlls_transport *transports;
    horizon_dlls_progress progress;
    void *opaque;
    unsigned char *done;
    unsigned int next, started, pending;
    unsigned long long total, finished;
    unsigned long long in_flight[HORIZON_DLLS_CONNECTIONS];
    enum horizon_dlls_result result;
    int stop;
};

struct apply_worker
{
    struct apply *a;
    unsigned int index;
    char what[128];
};

static int apply_progress( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    struct apply_worker *w = opaque;
    struct apply *a = w->a;
    unsigned long long sum;
    unsigned int i;
    int stop;

    (void)what;
    (void)total;
    pthread_mutex_lock( &a->lock );
    a->in_flight[w->index] = current;
    for (sum = a->finished, i = 0; i < HORIZON_DLLS_CONNECTIONS; i++) sum += a->in_flight[i];
    stop = a->stop;
    pthread_mutex_unlock( &a->lock );
    if (!stop && a->progress && a->progress( a->opaque, w->what, sum, a->total ))
    {
        pthread_mutex_lock( &a->lock );
        a->stop = 1;
        if (a->result == HORIZON_DLLS_OK) a->result = HORIZON_DLLS_CANCELLED;
        pthread_mutex_unlock( &a->lock );
        stop = 1;
    }
    /* One failing or cancelled stops the others' downloads too. */
    return stop;
}

static void *apply_thread( void *opaque )
{
    struct apply_worker *w = opaque;
    struct apply *a = w->a;

    spread_thread( w->index );
    for (;;)
    {
        const struct horizon_dll_file *f = NULL;
        enum horizon_dlls_result result;
        unsigned int i;

        pthread_mutex_lock( &a->lock );
        for (i = a->next; !a->stop && i < a->remote->count; i++)
            if (a->remote->files[i].state == HORIZON_DLL_NEW || a->remote->files[i].state == HORIZON_DLL_CHANGED)
                break;
        if (!a->stop && i < a->remote->count)
        {
            f = &a->remote->files[i];
            a->next = i + 1;
            snprintf( w->what, sizeof(w->what), "%s (%u of %u)", f->name, ++a->started, a->pending );
        }
        pthread_mutex_unlock( &a->lock );
        if (!f) break;
        if (apply_progress( w, NULL, 0, 0 )) break;
        result = download_file( a->root, f, &a->transports[w->index], apply_progress, w );
        pthread_mutex_lock( &a->lock );
        a->in_flight[w->index] = 0;
        a->finished += f->packed_size ? f->packed_size : f->size;
        if (result == HORIZON_DLLS_OK) a->done[i] = 1;
        else
        {
            if (a->result == HORIZON_DLLS_OK) a->result = result;
            a->stop = 1;
        }
        pthread_mutex_unlock( &a->lock );
    }
    return NULL;
}

enum horizon_dlls_result horizon_dlls_apply( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, const struct horizon_dlls_transport *transports,
        unsigned int connections, horizon_dlls_progress progress, void *opaque )
{
    struct apply a = { .root = root, .remote = remote, .transports = transports, .progress = progress,
                       .opaque = opaque, .result = HORIZON_DLLS_OK };
    struct apply_worker workers[HORIZON_DLLS_CONNECTIONS];
    pthread_t threads[HORIZON_DLLS_CONNECTIONS];
    int running[HORIZON_DLLS_CONNECTIONS] = {0};
    enum horizon_dlls_result result;
    unsigned char *done;
    unsigned int i;

    if (!connections) connections = 1;
    if (connections > HORIZON_DLLS_CONNECTIONS) connections = HORIZON_DLLS_CONNECTIONS;
    for (i = 0; i < remote->count; i++)
        if (remote->files[i].state == HORIZON_DLL_NEW || remote->files[i].state == HORIZON_DLL_CHANGED)
        {
            a.total += remote->files[i].packed_size ? remote->files[i].packed_size : remote->files[i].size;
            a.pending++;
        }
    if (!(done = calloc( remote->count + 1, 1 ))) return HORIZON_DLLS_MEMORY;
    for (i = 0; i < remote->count; i++) done[i] = remote->files[i].state == HORIZON_DLL_CURRENT;
    a.done = done;
    pthread_mutex_init( &a.lock, NULL );
    for (i = 0; i < connections; i++) workers[i] = (struct apply_worker){ &a, i, "" };
    /* The others on threads of their own, the first on this one. */
    for (i = 1; i < connections; i++)
    {
        pthread_attr_t attr;

        pthread_attr_init( &attr );
        pthread_attr_setstacksize( &attr, 1024 * 1024 );  /* curl and mbedTLS take a good deal */
        running[i] = !pthread_create( &threads[i], &attr, apply_thread, &workers[i] );
        pthread_attr_destroy( &attr );
    }
    apply_thread( &workers[0] );
    for (i = 1; i < connections; i++) if (running[i]) pthread_join( threads[i], NULL );
    pthread_mutex_destroy( &a.lock );
    result = a.result;
    return record( root, remote, local, done, result, result == HORIZON_DLLS_OK );
}

/***********************************************************************
 * The whole set at once, from the repository's release
 */

/* The largest bundle this takes into memory. */
#define BUNDLE_MAX (1024ull * 1024 * 1024)

struct ranges
{
    pthread_mutex_t lock;
    horizon_dlls_progress progress;
    void *opaque;
    unsigned long long total;
    unsigned long long got[HORIZON_DLLS_CONNECTIONS];
    int stop;
};

struct range_job
{
    struct ranges *all;
    const struct horizon_dlls_transport *transport;
    const char *url;
    unsigned char *data;
    unsigned long long offset, length, got;
    unsigned int index;
    enum horizon_dlls_result result;
};

static int to_range( void *context, const void *data, size_t size )
{
    struct range_job *j = context;

    /* More than was asked: a server that did not take the range. */
    if (size > j->length - j->got) return 0;
    memcpy( j->data + j->offset + j->got, data, size );
    j->got += size;
    return 1;
}

static int range_progress( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    struct range_job *j = opaque;
    struct ranges *all = j->all;
    unsigned long long sum = 0;
    unsigned int i;
    int stop;

    (void)what;
    (void)current;
    (void)total;
    pthread_mutex_lock( &all->lock );
    all->got[j->index] = j->got;
    for (i = 0; i < HORIZON_DLLS_CONNECTIONS; i++) sum += all->got[i];
    stop = all->stop;
    pthread_mutex_unlock( &all->lock );
    if (!stop && all->progress && all->progress( all->opaque, "Downloading the Windows DLLs", sum, all->total ))
    {
        pthread_mutex_lock( &all->lock );
        all->stop = 1;
        pthread_mutex_unlock( &all->lock );
        stop = 1;
    }
    return stop;
}

static void *range_thread( void *opaque )
{
    struct range_job *j = opaque;
    unsigned int attempt;
    int stop;

    spread_thread( j->index );
    for (attempt = 0; attempt < ATTEMPTS; attempt++)
    {
        if (attempt) pause_before( attempt );
        j->got = 0;
        j->result = j->transport->fetch_range( j->transport->opaque, j->url, j->offset, j->length,
                                               to_range, j, range_progress, j );
        if (j->result == HORIZON_DLLS_OK && j->got != j->length) j->result = HORIZON_DLLS_INVALID;
        pthread_mutex_lock( &j->all->lock );
        stop = j->all->stop;
        pthread_mutex_unlock( &j->all->lock );
        if (j->result != HORIZON_DLLS_NETWORK || stop) break;
    }
    return NULL;
}

struct growing
{
    unsigned char *data;
    size_t size, capacity;
};

static int to_growing( void *context, const void *data, size_t size )
{
    struct growing *g = context;
    unsigned char *grown;
    size_t wanted;

    if (size > BUNDLE_MAX - g->size) return 0;
    if (g->size + size > g->capacity)
    {
        wanted = g->capacity ? g->capacity * 2 : 16 * 1024 * 1024;
        while (wanted < g->size + size) wanted *= 2;
        if (!(grown = realloc( g->data, wanted ))) return 0;
        g->data = grown;
        g->capacity = wanted;
    }
    memcpy( g->data + g->size, data, size );
    g->size += size;
    return 1;
}

enum horizon_dlls_result horizon_dlls_download( const struct horizon_dlls_transport *transports,
        unsigned int connections, const char *url, unsigned char **data, unsigned long long *size,
        horizon_dlls_progress progress, void *opaque, struct horizon_dlls_bundle_stats *stats )
{
    struct ranges all = { .progress = progress, .opaque = opaque };
    struct range_job jobs[HORIZON_DLLS_CONNECTIONS];
    pthread_t threads[HORIZON_DLLS_CONNECTIONS];
    int running[HORIZON_DLLS_CONNECTIONS] = {0};
    enum horizon_dlls_result result;
    unsigned long long started = now_us(), total = 0, chunk;
    unsigned int i;

    *data = NULL;
    *size = 0;
    if (connections > HORIZON_DLLS_CONNECTIONS) connections = HORIZON_DLLS_CONNECTIONS;
    if (!connections) connections = 1;
    /* Without its size, or ranges, one connection takes it all. */
    if (!transports[0].size || !transports[0].fetch_range ||
        transports[0].size( transports[0].opaque, url, &total ) != HORIZON_DLLS_OK || !total ||
        total > BUNDLE_MAX)
    {
        struct growing g = {0};

        result = transports[0].fetch( transports[0].opaque, url, to_growing, &g, progress, opaque );
        if (result == HORIZON_DLLS_OK) { *data = g.data; *size = g.size; }
        else free( g.data );
        if (stats) *stats = (struct horizon_dlls_bundle_stats){ .bytes = g.size, .connections = 1,
                                .download_ms = (unsigned int)((now_us() - started) / 1000) };
        return result;
    }
    if (!(*data = malloc( total ))) return HORIZON_DLLS_MEMORY;
    all.total = total;
    pthread_mutex_init( &all.lock, NULL );
    /* The ranges in whole MiB, so no connection is left a sliver. */
    chunk = (total + connections - 1) / connections;
    chunk = (chunk + (1 << 20) - 1) & ~((unsigned long long)(1 << 20) - 1);
    for (i = 0; i < connections && (unsigned long long)i * chunk < total; i++)
    {
        unsigned long long offset = (unsigned long long)i * chunk;

        jobs[i] = (struct range_job){ &all, &transports[i], url, *data, offset,
                                      offset + chunk > total ? total - offset : chunk, 0, i, HORIZON_DLLS_OK };
    }
    connections = i;
    for (i = 1; i < connections; i++)
    {
        pthread_attr_t attr;

        pthread_attr_init( &attr );
        pthread_attr_setstacksize( &attr, 1024 * 1024 );  /* curl and mbedTLS take a good deal */
        running[i] = !pthread_create( &threads[i], &attr, range_thread, &jobs[i] );
        pthread_attr_destroy( &attr );
        if (!running[i]) range_thread( &jobs[i] );
    }
    range_thread( &jobs[0] );
    for (i = 1; i < connections; i++) if (running[i]) pthread_join( threads[i], NULL );
    pthread_mutex_destroy( &all.lock );
    result = all.stop ? HORIZON_DLLS_CANCELLED : HORIZON_DLLS_OK;
    for (i = 0; i < connections && result == HORIZON_DLLS_OK; i++) result = jobs[i].result;
    if (stats) *stats = (struct horizon_dlls_bundle_stats){ .bytes = total, .connections = connections,
                            .download_ms = (unsigned int)((now_us() - started) / 1000) };
    if (result != HORIZON_DLLS_OK)
    {
        free( *data );
        *data = NULL;
        return result;
    }
    *size = total;
    return HORIZON_DLLS_OK;
}

/* A tar read as zstd unpacks it: a 512-byte header, the file, padding to 512.
 * The repository's tools write plain ustar with no extended headers. */
struct bundle
{
    const char *root;
    const struct horizon_dll_manifest *remote;
    unsigned char *done;
    unsigned char header[512];
    size_t have;
    int in_file, ended;
    unsigned long long left, padding;
    /* The entry being read, when it is a file the card wants. */
    const struct horizon_dll_file *file;
    unsigned int index;
    unsigned char *entry;
    unsigned long long written;
    hash_context hash;
    char path[768], what[128];
    int failed;
    unsigned long long write_us;
    unsigned int unpacked, skipped;
};

static unsigned long long octal( const unsigned char *field, size_t size )
{
    unsigned long long value = 0;
    size_t i;

    for (i = 0; i < size && field[i] == ' '; i++) ;
    for (; i < size && field[i] >= '0' && field[i] <= '7'; i++) value = value * 8 + (field[i] - '0');
    return value;
}

/* Which file of the manifest an entry is, if the card wants it at this size. */
static const struct horizon_dll_file *bundle_target( struct bundle *b, const char *name, unsigned long long size,
                                                     unsigned int *index )
{
    static const char prefix[] = "switch/wine/";
    const char *rest, *slash;
    char folder_path[512];
    unsigned int i;
    int folder;

    if (strncmp( name, prefix, sizeof(prefix) - 1 )) return NULL;
    rest = name + sizeof(prefix) - 1;
    if (!(slash = strrchr( rest, '/' )) || (size_t)(slash - rest) >= sizeof(folder_path)) return NULL;
    memcpy( folder_path, rest, slash - rest );
    folder_path[slash - rest] = 0;
    if ((folder = folder_index( folder_path )) < 0 || !valid_name( slash + 1 )) return NULL;
    for (i = 0; i < b->remote->count; i++)
    {
        const struct horizon_dll_file *f = &b->remote->files[i];

        if (f->folder != folder || strcasecmp( f->name, slash + 1 ) || b->done[i]) continue;
        if (f->state != HORIZON_DLL_NEW && f->state != HORIZON_DLL_CHANGED) return NULL;
        if (f->size != size) return NULL;   /* another version: the file on its own brings it */
        *index = i;
        return f;
    }
    return NULL;
}

static void bundle_header( struct bundle *b )
{
    char name[512];
    unsigned long long size;
    size_t i;

    for (i = 0; i < 512 && !b->header[i]; i++) ;
    if (i == 512) { b->ended = 1; return; }   /* the end: blocks of zeros */
    /* The name, and in ustar its prefix before it. */
    if (!memcmp( b->header + 257, "ustar", 5 ) && b->header[345])
        snprintf( name, sizeof(name), "%.155s/%.100s", b->header + 345, b->header );
    else snprintf( name, sizeof(name), "%.100s", b->header );
    size = octal( b->header + 124, 12 );
    b->left = size;
    b->padding = (512 - size % 512) % 512;
    b->in_file = 1;
    b->file = NULL;
    /* Only regular files; folders and anything else pass. */
    if (b->header[156] != '0' && b->header[156] != 0) return;
    if (!(b->file = bundle_target( b, name, size, &b->index ))) { b->skipped++; return; }
    if (!join( b->path, sizeof(b->path), b->root, horizon_dlls_folders[b->file->folder], b->file->name ) ||
        !make_folders( b->root, horizon_dlls_folders[b->file->folder] ))
    {
        b->failed = HORIZON_DLLS_IO;
        b->file = NULL;
        return;
    }
    if (!(b->entry = malloc( size ? size : 1 ))) { b->failed = HORIZON_DLLS_MEMORY; b->file = NULL; return; }
    b->written = 0;
    hash_begin( &b->hash );
    snprintf( b->what, sizeof(b->what), "%s (%u unpacked)", b->file->name, b->unpacked );
}

static void bundle_entry_end( struct bundle *b )
{
    unsigned char hash[32];
    char digest[65];

    if (!b->file) return;
    hash_end( &b->hash, hash );
    hex( hash, digest );
    /* One that is not the manifest's is left for the file on its own. */
    if (b->written == b->file->size && !strcmp( digest, b->file->sha256 ))
    {
        unsigned long long start = now_us();

        if (put_file( b->path, b->entry, b->file->size ))
        {
            b->done[b->index] = 1;
            b->unpacked++;
        }
        else b->failed = HORIZON_DLLS_IO;
        b->write_us += now_us() - start;
    }
    else b->skipped++;
    free( b->entry );
    b->entry = NULL;
    b->file = NULL;
}

/* What zstd unpacked, through the tar reader. */
static void bundle_feed( struct bundle *b, const unsigned char *p, size_t size )
{
    while (size && !b->ended && !b->failed)
    {
        size_t take;

        if (!b->in_file)
        {
            take = 512 - b->have < size ? 512 - b->have : size;
            memcpy( b->header + b->have, p, take );
            b->have += take; p += take; size -= take;
            if (b->have == 512) { b->have = 0; bundle_header( b ); }
            continue;
        }
        if (b->left)
        {
            take = b->left < size ? b->left : size;
            if (b->file)
            {
                memcpy( b->entry + b->written, p, take );
                hash_add( &b->hash, p, take );
                b->written += take;
            }
            b->left -= take; p += take; size -= take;
            if (!b->left) bundle_entry_end( b );
            continue;
        }
        if (b->file) bundle_entry_end( b );   /* an empty file */
        take = b->padding < size ? b->padding : size;
        b->padding -= take; p += take; size -= take;
        if (!b->padding) b->in_file = 0;
    }
}

enum horizon_dlls_result horizon_dlls_apply_bundle( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, const unsigned char *data, unsigned long long size,
        horizon_dlls_progress progress, void *opaque, struct horizon_dlls_bundle_stats *stats )
{
    struct bundle *b = calloc( 1, sizeof(*b) );
    const size_t out_size = ZSTD_DStreamOutSize() * 8;
    unsigned char *out = malloc( out_size );
    ZSTD_DCtx *z = ZSTD_createDCtx();
    ZSTD_inBuffer in = { data, size, 0 };
    enum horizon_dlls_result result = HORIZON_DLLS_OK;
    unsigned long long started = now_us();
    unsigned char *done;
    unsigned int i;

    if (!b || !out || !z || !(done = calloc( remote->count + 1, 1 )))
    {
        free( b ); free( out ); ZSTD_freeDCtx( z );
        return HORIZON_DLLS_MEMORY;
    }
    for (i = 0; i < remote->count; i++) done[i] = remote->files[i].state == HORIZON_DLL_CURRENT;
    b->root = root;
    b->remote = remote;
    b->done = done;
    /* The window long-range mode compressed it with. */
    ZSTD_DCtx_setParameter( z, ZSTD_d_windowLogMax, 27 );
    while (in.pos < in.size && !b->ended && !b->failed)
    {
        ZSTD_outBuffer o = { out, out_size, 0 };
        size_t status = ZSTD_decompressStream( z, &o, &in );

        if (ZSTD_isError( status )) { result = HORIZON_DLLS_INVALID; break; }
        bundle_feed( b, out, o.pos );
        if (progress && progress( opaque, b->what[0] ? b->what : "Unpacking the Windows DLLs", in.pos, in.size ))
        {
            result = HORIZON_DLLS_CANCELLED;
            break;
        }
    }
    if (b->file) { free( b->entry ); b->file = NULL; }
    if (result == HORIZON_DLLS_OK && b->failed) result = b->failed;
    if (stats)
    {
        stats->unpack_ms = (unsigned int)((now_us() - started) / 1000);
        stats->write_ms = (unsigned int)(b->write_us / 1000);
        stats->unpacked = b->unpacked;
        stats->skipped = b->skipped;
    }
    ZSTD_freeDCtx( z );
    free( out );
    free( b );
    /* What came is kept, also when it stopped part of the way; what did not,
     * or came as another version, comes on its own. */
    return record( root, remote, local, done, result, 0 );
}

int horizon_dlls_ready( const char *root, const char *const *features, size_t feature_count,
                        char *why, size_t why_size )
{
    /* What every program loads before its own code runs. */
    static const struct { int folder; const char *name; } core[] =
    {
        { 0, "ntdll.dll" }, { 0, "wow64.dll" }, { 0, "wow64win.dll" }, { 0, "win32u.dll" },
        { 0, "winebox64.dll" }, { 0, "apisetschema.dll" }, { 0, "kernel32.dll" }, { 0, "kernelbase.dll" },
        { 1, "ntdll.dll" }, { 1, "kernel32.dll" }, { 1, "kernelbase.dll" },
    };
    struct horizon_dll_manifest record;
    enum horizon_dlls_result result;
    char path[768];
    struct stat st;
    size_t i;
    int ready = 1;

    result = horizon_dlls_load( root, features, feature_count, &record );
    if (result != HORIZON_DLLS_OK)
    {
        snprintf( why, why_size, "%s", result == HORIZON_DLLS_NOT_FOUND ?
                  "The Windows DLLs have not been downloaded to this card." :
                  "The card's record of its Windows DLLs is damaged." );
        return 0;
    }
    for (i = 0; i < sizeof(core) / sizeof(core[0]) && ready; i++)
    {
        const struct horizon_dll_file *f = find( &record, core[i].folder, core[i].name );

        join( path, sizeof(path), root, horizon_dlls_folders[core[i].folder], core[i].name );
        if (!f || stat( path, &st ) || (unsigned long long)st.st_size != f->size)
        {
            snprintf( why, why_size, "%s/%s is missing or not the one downloaded.",
                      horizon_dlls_folders[core[i].folder], core[i].name );
            ready = 0;
        }
        else if (!f->satisfied)
        {
            snprintf( why, why_size, "The Windows DLLs on the card are for another Autorun (%s needs %s).",
                      core[i].name, f->missing );
            ready = 0;
        }
    }
    horizon_dlls_free( &record );
    return ready;
}

const char *horizon_dlls_error( enum horizon_dlls_result result )
{
    switch (result)
    {
    case HORIZON_DLLS_OK: return "The Windows DLLs are up to date.";
    case HORIZON_DLLS_CANCELLED: return "Stopped. The files already downloaded are kept.";
    case HORIZON_DLLS_NETWORK: return "Could not reach the DLL repository on GitHub. Check the connection and try again.";
    case HORIZON_DLLS_NOT_FOUND: return "The DLL repository has no manifest where Autorun looks for it.";
    case HORIZON_DLLS_INVALID: return "The DLL repository returned a manifest or file that is not valid.";
    case HORIZON_DLLS_FLAVOR_MISMATCH: return "The DLL repository's files are for another Autorun runtime.";
    case HORIZON_DLLS_IO: return "A file could not be written to the SD card.";
    case HORIZON_DLLS_HASH: return "A download did not match its SHA-256. Try again.";
    case HORIZON_DLLS_MEMORY: return "Not enough memory to read the DLL manifest.";
    }
    return "The DLLs could not be updated.";
}

const char *horizon_dlls_category_title( const char *key, char *buffer, size_t size )
{
    static const struct { const char *key, *title; } titles[] =
    {
        { "core", "Windows core" },
        { "directx-graphics", "DirectX graphics" },
        { "directx-audio", "DirectX audio" },
        { "directx-input", "DirectX input" },
        { "directx-play", "DirectPlay" },
        { "directx-media", "DirectShow and Media Foundation" },
        { "translation-layers", "DXVK and VKD3D-Proton" },
        { "gaming", "Gaming" },
        { "audio", "Audio" },
        { "opengl-vulkan", "OpenGL and Vulkan" },
        { "imaging-text", "Imaging and text" },
        { "c-runtime", "C and C++ runtimes" },
        { "dotnet", ".NET" },
        { "com-ole", "COM and OLE" },
        { "winrt", "Windows Runtime" },
        { "xml", "XML" },
        { "data", "Databases" },
        { "network", "Networking" },
        { "security", "Security" },
        { "shell-ui", "Shell and controls" },
        { "printing", "Printing" },
        { "installers", "Installers" },
        { "legacy-16bit", "16-bit Windows" },
        { "drivers", "Drivers" },
        { "programs", "Programs" },
        { "system", "System" },
    };
    size_t i;

    for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        if (!strcmp( key, titles[i].key )) return titles[i].title;
    /* One the repository added since: its key, readably. */
    snprintf( buffer, size, "%s", key );
    for (i = 0; buffer[i]; i++) if (buffer[i] == '-') buffer[i] = ' ';
    if (buffer[0]) buffer[0] = toupper( (unsigned char)buffer[0] );
    return buffer;
}
