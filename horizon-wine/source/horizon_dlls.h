/*
 * The Windows modules a card runs, from the DLL repository (autorun-horizon-dlls).
 *
 * Autorun ships no DLLs. The repository publishes every one of them, laid out as
 * on the card, with a manifest that names each file's folder, size, SHA-256,
 * version, category and what it needs of the runtime. The card keeps the
 * manifest it installed from in the same place (switch/wine/horizon-dlls), so
 * a copy of the repository made by hand is an installation like any other.
 *
 * Bringing a card up to date is: read the published manifest, compare it with
 * the card's, download what is new or changed into a .part file beside where it
 * goes, check its size and hash, and put it in place. A file the runtime cannot
 * run (a feature it does not report) is left as it is. What the card then holds
 * is written back as its manifest, with classes.reg, the COM classes its DLLs
 * serve, which the runtime's registry loads.
 *
 * Nothing here draws or knows about the network: a transport fetches a URL,
 * which is curl on the Switch and a folder in the host test.
 */
#ifndef WINE_NX_HORIZON_DLLS_H
#define WINE_NX_HORIZON_DLLS_H

#include <stddef.h>

#define HORIZON_DLLS_RAW "https://raw.githubusercontent.com/autorunhq/autorun-horizon-dlls/"
#define HORIZON_DLLS_MANIFEST_URL HORIZON_DLLS_RAW "main/switch/wine/horizon-dlls/manifest.json"
/* Relative to the runtime directory, as in the repository under switch/wine. */
#define HORIZON_DLLS_MANIFEST "horizon-dlls/manifest.json"
#define HORIZON_DLLS_CLASSES  "horizon-dlls/classes.reg"
/* The runtimes the files are for: an ARM64X system32 and an i386 syswow64. */
#define HORIZON_DLLS_FLAVOR   "arm64x"
#define HORIZON_DLLS_CATEGORIES 40
/* Downloads at once, at most. */
#define HORIZON_DLLS_CONNECTIONS 16

enum horizon_dlls_result
{
    HORIZON_DLLS_OK,
    HORIZON_DLLS_CANCELLED,
    HORIZON_DLLS_NETWORK,
    HORIZON_DLLS_NOT_FOUND,
    HORIZON_DLLS_INVALID,
    HORIZON_DLLS_FLAVOR_MISMATCH,
    HORIZON_DLLS_IO,
    HORIZON_DLLS_HASH,
    HORIZON_DLLS_MEMORY,
};

enum horizon_dll_state
{
    HORIZON_DLL_CURRENT,        /* the card has these bytes */
    HORIZON_DLL_NEW,            /* not on the card */
    HORIZON_DLL_CHANGED,        /* on the card, but not these bytes */
    HORIZON_DLL_UNSUPPORTED,    /* needs what this runtime does not report */
};

struct horizon_dll_file
{
    char name[64];
    char arch[12];
    unsigned char folder;       /* index into horizon_dlls_folders */
    unsigned char category;     /* index into the manifest's categories */
    unsigned char satisfied;    /* the runtime reports everything it requires */
    unsigned char state;        /* enum horizon_dll_state, from horizon_dlls_plan */
    unsigned int version;
    unsigned long long size;
    char sha256[65];
    char url[256];
    /* The zlib-compressed copy that is downloaded, when the repository has one. */
    unsigned long long packed_size;
    char packed_sha256[65];
    char packed_url[256];
    char missing[96];           /* the first feature the runtime lacks */
    unsigned int class_first, class_count;
    unsigned int feature_first, feature_count;
    char *registry;
};

struct horizon_dll_feature
{
    char name[96];
};

struct horizon_dll_class
{
    char clsid[40];
    char name[80];
    char threading[16];
};

struct horizon_dll_category
{
    char key[32];
    char description[192];
};

struct horizon_dll_manifest
{
    int schema;
    char flavor[16];
    char commit[48];
    char wine[16];
    struct horizon_dll_category categories[HORIZON_DLLS_CATEGORIES];
    unsigned int category_count;
    struct horizon_dll_file *files;
    unsigned int count, capacity;
    struct horizon_dll_class *classes;
    unsigned int class_count, class_capacity;
    struct horizon_dll_feature *features;
    unsigned int feature_count, feature_capacity;
    /* Files in folders this Autorun does not know, which it leaves out. */
    unsigned int skipped;
};

/* pending_bytes is what the files take on the card, download_bytes what
 * fetching them transfers. */
struct horizon_dlls_category_plan
{
    unsigned int files, current, pending, unsupported;
    unsigned long long bytes, pending_bytes, download_bytes;
};

struct horizon_dlls_plan
{
    unsigned int files, current, pending, unsupported, removed;
    unsigned long long bytes, pending_bytes, download_bytes;
    struct horizon_dlls_category_plan categories[HORIZON_DLLS_CATEGORIES];
};

/* Where files may go, relative to the runtime directory. */
extern const char *const horizon_dlls_folders[];

/* what says what is being done; a non-zero return cancels. */
typedef int (*horizon_dlls_progress)( void *opaque, const char *what, unsigned long long current,
                                      unsigned long long total );
/* Takes the bytes of a download as they come; zero stops it. */
typedef int (*horizon_dlls_sink)( void *context, const void *data, size_t size );

struct horizon_dlls_transport
{
    enum horizon_dlls_result (*fetch)( void *opaque, const char *url, horizon_dlls_sink sink, void *context,
                                       horizon_dlls_progress progress, void *progress_opaque );
    /* How large what url names is, without downloading it. */
    enum horizon_dlls_result (*size)( void *opaque, const char *url, unsigned long long *size );
    /* Bytes offset to offset + length - 1 of it; a server that sends more
     * than was asked fails it. */
    enum horizon_dlls_result (*fetch_range)( void *opaque, const char *url, unsigned long long offset,
                                             unsigned long long length, horizon_dlls_sink sink, void *context,
                                             horizon_dlls_progress progress, void *progress_opaque );
    void *opaque;
};

enum horizon_dlls_result horizon_dlls_parse( const char *text, size_t size, const char *const *features,
                                             size_t feature_count, struct horizon_dll_manifest *out );
/* The published manifest; it must be for this runtime's flavor. */
enum horizon_dlls_result horizon_dlls_fetch_manifest( const struct horizon_dlls_transport *transport,
        const char *url, const char *const *features, size_t feature_count,
        struct horizon_dll_manifest *out, horizon_dlls_progress progress, void *opaque );
/* The card's own; HORIZON_DLLS_NOT_FOUND when it has none. */
enum horizon_dlls_result horizon_dlls_load( const char *root, const char *const *features, size_t feature_count,
                                            struct horizon_dll_manifest *out );
void horizon_dlls_free( struct horizon_dll_manifest *manifest );

/* What the card has of remote, into each file's state and plan. local may be
 * NULL. verify hashes every file on the card rather than trusting its manifest. */
enum horizon_dlls_result horizon_dlls_plan( const char *root, struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, int verify, horizon_dlls_progress progress, void *opaque,
        struct horizon_dlls_plan *plan );
/* Download and put in place every file the plan left new or changed, and write
 * the card's manifest and classes.reg for what it then holds, also when it
 * stops part of the way. connections downloads run at once, one transport
 * each (transports[0] to [connections - 1]), and progress is called from
 * each of them. */
enum horizon_dlls_result horizon_dlls_apply( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, const struct horizon_dlls_transport *transports,
        unsigned int connections, horizon_dlls_progress progress, void *opaque );

/* The repository's latest release: every file as one tar compressed with zstd
 * (-19, long range), about 90 MB where the files one by one are twice that, for
 * a card that has little of the set; and as a zip, for a player without a
 * network. */
#define HORIZON_DLLS_RELEASE "https://github.com/autorunhq/autorun-horizon-dlls/releases/latest/download/"
#define HORIZON_DLLS_BUNDLE_URL HORIZON_DLLS_RELEASE "horizon-dlls.tar.zst"

/* How downloading and unpacking the bundle went, for the log. */
struct horizon_dlls_bundle_stats
{
    unsigned long long bytes;
    unsigned int connections, download_ms, unpack_ms, write_ms, unpacked, skipped;
};

/* Download url whole into memory, over connections transports at once, each
 * a range of it; one that fails is tried again on its own. *data is malloc'd. */
enum horizon_dlls_result horizon_dlls_download( const struct horizon_dlls_transport *transports,
        unsigned int connections, const char *url, unsigned char **data, unsigned long long *size,
        horizon_dlls_progress progress, void *opaque, struct horizon_dlls_bundle_stats *stats );
/* Unpack a downloaded bundle: each file the plan left new or changed, at the
 * manifest's size and SHA-256, is written to the card whole, one at a time;
 * the rest -- another version, or damaged -- is left for horizon_dlls_apply to
 * bring on its own after a new plan. Writes the card's manifest for what it
 * then holds, also when it stops part of the way. */
enum horizon_dlls_result horizon_dlls_apply_bundle( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, const unsigned char *data, unsigned long long size,
        horizon_dlls_progress progress, void *opaque, struct horizon_dlls_bundle_stats *stats );

/* Whether the card holds the DLLs a program needs to start, installed from the
 * repository for this runtime: the core modules in the card's manifest, the
 * size it says, and needing nothing this runtime does not report. A card with
 * an earlier release's DLLs and no manifest has not. why says what is wrong. */
int horizon_dlls_ready( const char *root, const char *const *features, size_t feature_count,
                        char *why, size_t why_size );
const char *horizon_dlls_error( enum horizon_dlls_result result );
/* A category's name as a player reads it: "directx-graphics" is DirectX graphics. */
const char *horizon_dlls_category_title( const char *key, char *buffer, size_t size );

#endif
