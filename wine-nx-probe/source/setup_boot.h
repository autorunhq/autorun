#ifndef WINE_NX_SETUP_BOOT_H
#define WINE_NX_SETUP_BOOT_H

#include <stddef.h>
#include <stdint.h>

#define SETUP_BOOT_MAX_ENTRIES 64
#define SETUP_BOOT_ENTRY_NAME "Autorun"
#define SETUP_BOOT_INI "bootloader/hekate_ipl.ini"

enum setup_boot_result
{
    SETUP_BOOT_OK, SETUP_BOOT_ALREADY, SETUP_BOOT_MISSING, SETUP_BOOT_INVALID,
    SETUP_BOOT_UNSUPPORTED, SETUP_BOOT_CHANGED, SETUP_BOOT_HASH, SETUP_BOOT_IO,
    SETUP_BOOT_RECOVERY
};

enum setup_boot_loader { SETUP_BOOT_STOCK, SETUP_BOOT_HOC };

struct setup_boot_entry
{
    char name[128];
    char reason[128];
    size_t offset, length;
    unsigned char ini_sha256[32];
    int supported;
};

struct setup_boot_payload
{
    uint64_t size;
    unsigned char sha256[32];
};

/* Compiled release metadata, never accepted from an untrusted SD manifest. */
struct setup_boot_manifest
{
    unsigned int version;
    char release[33];
    uint32_t atmosphere_version;
    struct setup_boot_payload stock, hoc, mesosphere;
    unsigned char hoc_code_sha256[32];
    uint32_t hoc_config_offset, hoc_config_size, hoc_config_revision, hoc_kip_version;
};

struct setup_boot_hoc_info
{
    char path[512];
    uint32_t revision, kip_version;
    int compatible;
};

struct setup_boot_options
{
    struct setup_boot_entry entry;
    enum setup_boot_loader loader;
    int preserve_hoc;
    char hoc_source[512];
};

const struct setup_boot_manifest *setup_boot_bundled_manifest(void);
void setup_boot_bundle_id(const struct setup_boot_manifest *manifest, char out[33]);
int setup_boot_needs_update(const char *sd_root, const struct setup_boot_manifest *manifest,
    enum setup_boot_loader *installed_loader);
int setup_boot_hoc_probe(const char *sd_root, const struct setup_boot_manifest *manifest,
    struct setup_boot_hoc_info *info);
enum setup_boot_result setup_boot_list(const char *sd_root, struct setup_boot_entry *entries,
    size_t capacity, size_t *count, char *detail, size_t detail_size);
enum setup_boot_result setup_boot_install(const char *sd_root, const char *payload_root,
    const struct setup_boot_manifest *manifest, const struct setup_boot_options *options,
    char *detail, size_t detail_size);
/* Partial appends are truncated only when both saved hashes and the prefix match. */
enum setup_boot_result setup_boot_recover(const char *sd_root, char *detail, size_t detail_size);
const char *setup_boot_error(enum setup_boot_result result);

#endif
