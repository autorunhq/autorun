#ifndef WINE_NX_DXVK_RELEASES_H
#define WINE_NX_DXVK_RELEASES_H

#include <stddef.h>

#define DXVK_MAX_RELEASES 128

/* Where the Carafe builds of DXVK and FEX are released (carafe/dlls and
 * .github/workflows/carafe-dlls.yml): the repository this runtime is built
 * from, unless the build names another. */
#ifndef WINE_NX_CARAFE_DLLS_REPO
#define WINE_NX_CARAFE_DLLS_REPO "berlint-hub/autorun"
#endif

enum dxvk_source
{
    DXVK_SOURCE_OFFICIAL,
    DXVK_SOURCE_SAREK,
    DXVK_SOURCE_GPLASYNC,
    DXVK_SOURCE_CARAFE,     /* DXVK with Carafe's patches */
    DXVK_SOURCE_COUNT,
    /* Not a DXVK, but released, listed and installed like one: FEX for 32-bit
     * programs with Carafe's patches. A program's settings never name it as
     * their DXVK source; the catalog and install calls below take it, with
     * the 32-bit machine. */
    DXVK_BACKEND_FEX_CARAFE = DXVK_SOURCE_COUNT,
    /* The same again with what is being tried on top of Carafe's patches
     * (carafe/dlls/fex-experimental), released and kept apart from it, so that
     * an experiment never replaces the build games already run well on. */
    DXVK_BACKEND_FEX_EXPERIMENTAL,
    DXVK_BACKEND_COUNT
};

/* A program's FEX build as its settings hold it (launcher_settings.h,
 * fex_carafe): 1 Carafe's, 2 the experimental one. */
#define DXVK_FEX_BACKEND( build ) ((build) == 2 ? DXVK_BACKEND_FEX_EXPERIMENTAL : DXVK_BACKEND_FEX_CARAFE)
#define DXVK_FEX_BUILD_NAME( build ) ((build) == 2 ? "Experimental" : "Carafe")

struct dxvk_release
{
    char version[32];
    char url[768];
    char digest[65];
    unsigned long long size;
    int prerelease;
};

struct dxvk_version
{
    char version[32];
    int installed, bundled;
};

enum dxvk_result
{
    DXVK_OK,
    DXVK_NETWORK_ERROR,
    DXVK_NOT_FOUND,
    DXVK_INVALID_RESPONSE,
    DXVK_INVALID_ARCHIVE,
    DXVK_HASH_MISMATCH,
    DXVK_IO_ERROR,
    DXVK_CANCELLED
};

enum dxvk_progress_stage
{
    DXVK_PROGRESS_DOWNLOAD,
    DXVK_PROGRESS_VERIFY,
    DXVK_PROGRESS_INSTALL
};

typedef int (*dxvk_progress_callback)( void *opaque, enum dxvk_progress_stage stage,
                                        unsigned long long current, unsigned long long total );

enum dxvk_result dxvk_release_catalog( enum dxvk_source source, const char *runtime_dir, struct dxvk_release *releases,
                                       int max_releases, int *count, int cache_only,
                                       dxvk_progress_callback progress, void *opaque );
enum dxvk_result dxvk_install_release( enum dxvk_source source, const char *runtime_dir, const struct dxvk_release *release,
                                       dxvk_progress_callback progress, void *opaque );
int dxvk_release_installed( enum dxvk_source source, const char *runtime_dir, unsigned short machine, const char *version );
int dxvk_root_version( enum dxvk_source source, const char *runtime_dir, unsigned short machine, char *version, size_t size );
void dxvk_resolve_version( enum dxvk_source source, const char *runtime_dir, unsigned short machine, const char *requested,
                           struct dxvk_version *selected );
const char *dxvk_result_message( enum dxvk_result result );
/* The folder a release of a source is installed in, or its bundled one when
 * version is empty. */
int dxvk_release_path( enum dxvk_source source, const char *runtime_dir, unsigned short machine, const char *version,
                       char *path, size_t size );

enum dxvk_result vkd3d_release_catalog( const char *runtime_dir, struct dxvk_release *releases,
                                       int max_releases, int *count, int cache_only,
                                       dxvk_progress_callback progress, void *opaque );
enum dxvk_result vkd3d_install_release( const char *runtime_dir, const struct dxvk_release *release,
                                       dxvk_progress_callback progress, void *opaque );
int vkd3d_release_installed( const char *runtime_dir, unsigned short machine, const char *version );
int vkd3d_root_version( const char *runtime_dir, unsigned short machine, char *version, size_t size );
void vkd3d_resolve_version( const char *runtime_dir, unsigned short machine, const char *requested,
                            struct dxvk_version *selected );

#endif
