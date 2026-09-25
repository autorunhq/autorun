#ifndef WINE_NX_DXVK_RELEASES_H
#define WINE_NX_DXVK_RELEASES_H

#include <stddef.h>

#define DXVK_MAX_RELEASES 128

enum dxvk_source
{
    DXVK_SOURCE_OFFICIAL,
    DXVK_SOURCE_SAREK,
    DXVK_SOURCE_GPLASYNC,
    DXVK_SOURCE_COUNT
};

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
