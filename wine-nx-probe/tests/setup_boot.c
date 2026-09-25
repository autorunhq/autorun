#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t fail_append_after = -1;
static struct stat fail_append_file;
static int fail_staging_skip = -1;
static ssize_t fail_staging_after = -1;
static ssize_t fixture_write(int fd, const void *data, size_t size)
{
    struct stat st;
    ssize_t n;
    if (fail_staging_skip > 0) fail_staging_skip--;
    else if (!fail_staging_skip && fail_staging_after >= 0)
    {
        if (!fail_staging_after)
        {
            fail_staging_skip = -1;
            errno = ENOSPC;
            return -1;
        }
        if (size > (size_t)fail_staging_after) size = fail_staging_after;
        n = write(fd, data, size);
        if (n > 0) fail_staging_after -= n;
        return n;
    }
    if (fail_append_after >= 0 && !fstat(fd, &st) &&
        st.st_dev == fail_append_file.st_dev && st.st_ino == fail_append_file.st_ino)
    {
        if (!fail_append_after)
        {
            fail_append_after = -1;
            errno = ENOSPC;
            return -1;
        }
        if (size > (size_t)fail_append_after) size = fail_append_after;
        n = write(fd, data, size);
        if (n > 0) fail_append_after -= n;
        return n;
    }
    return write(fd, data, size);
}
#define write fixture_write
#include "../source/setup_boot.c"
#undef write

static const char original_ini[] =
    "# user comment\r\n[config]\r\nautoboot=2\r\nautoboot_list=0\r\n"
    "{Custom caption}\r\n[Atmosphere emuMMC]\r\n"
    "fss0=atmosphere/package3\r\nemummcforce=1\r\nemupath=emuMMC/RAW1\r\n"
    "kip1=atmosphere/kips/*\r\nkernel=old/kernel.bin\r\nid=emummc\r\n"
    "icon=bootloader/res/My Icon.bmp\r\n# kept comment\r\n"
    "[Recovery]\r\npayload=bootloader/payloads/recovery.bin";

static void create_file(const char *root, const char *path, const void *data, size_t size)
{
    assert(write_new(root, path, data, size));
}

static void replace_file(const char *root, const char *path, const void *data, size_t size)
{
    char absolute[1024];
    assert(path_join(absolute, root, path));
    assert(!unlink(absolute));
    create_file(root, path, data, size);
}

static void check_file(const char *root, const char *path, const void *data, size_t size)
{
    struct buffer buffer;
    assert(read_file(root, path, PAYLOAD_LIMIT, &buffer) == 1);
    assert(buffer.size == size && !memcmp(buffer.data, data, size));
    free(buffer.data);
}

static void check_absent(const char *root, const char *path)
{
    char absolute[1024];
    assert(path_join(absolute, root, path));
    assert(access(absolute, F_OK) && errno == ENOENT);
}

static void remove_fixture(const char *root)
{
    DIR *dir = opendir(root);
    struct dirent *entry;
    char path[1024];
    struct stat st;
    assert(dir);
    while ((entry = readdir(dir)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        assert(path_join(path, root, entry->d_name));
        assert(!lstat(path, &st));
        if (S_ISDIR(st.st_mode)) remove_fixture(path);
        else assert(!unlink(path));
    }
    closedir(dir);
    assert(!rmdir(root));
}

static void fill_payload(struct setup_boot_payload *payload, const void *data, size_t size)
{
    payload->size = size;
    digest(data, size, payload->sha256);
}

static void fill_hoc_code_hash(struct setup_boot_manifest *manifest, const unsigned char *data, size_t size)
{
    unsigned char masked[1024];
    assert(size <= sizeof(masked));
    memcpy(masked, data, size);
    memset(masked + manifest->hoc_config_offset + 12, 0, manifest->hoc_config_size - 12);
    digest(masked, size, manifest->hoc_code_sha256);
}

struct fixture
{
    char root[1024], payloads[1024];
    unsigned char stock[1024], hoc[1024], old[1024], kernel[512], package[2048];
    struct setup_boot_manifest manifest;
    struct setup_boot_options options;
};

static void fixture_init(struct fixture *fixture)
{
    struct setup_boot_entry entries[64];
    unsigned char other[256] = "KIP1sysmodule";
    size_t count;
    char detail[256];
    const char *temp = getenv("TMPDIR");
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->root, sizeof(fixture->root), "%s/autorun-boot-fixture-XXXXXX",
             temp && *temp ? temp : "/tmp");
    assert(mkdtemp(fixture->root));
    assert(path_join(fixture->payloads, fixture->root, "payloads"));
    assert(!mkdir(fixture->payloads, 0700));
    memcpy(fixture->stock, "KIP1loader", 10);
    fixture->stock[16] = 1;
    fixture->stock[23] = 1;
    memcpy(fixture->hoc, fixture->stock, sizeof(fixture->stock));
    fixture->hoc[300] = 17;
    memcpy(fixture->hoc + 512, "CUST\x07\x00\x00\x00\x2c\x01\x00\x00", 12);
    memcpy(fixture->old, fixture->hoc, sizeof(fixture->old));
    fixture->old[300] = 0;
    fixture->old[524] = 42;
    memset(fixture->kernel, 0xaa, sizeof(fixture->kernel));
    memset(fixture->package, 0x44, sizeof(fixture->package));
    fixture->manifest.version = 2;
    strcpy(fixture->manifest.release, "ams-1.11.2-hoc-2.5.1");
    fill_payload(&fixture->manifest.stock, fixture->stock, sizeof(fixture->stock));
    fill_payload(&fixture->manifest.hoc, fixture->hoc, sizeof(fixture->hoc));
    fill_payload(&fixture->manifest.mesosphere, fixture->kernel, sizeof(fixture->kernel));
    fixture->manifest.hoc_config_offset = 512;
    fixture->manifest.hoc_config_size = 64;
    fixture->manifest.hoc_config_revision = 7;
    fixture->manifest.hoc_kip_version = 300;
    fill_hoc_code_hash(&fixture->manifest, fixture->hoc, sizeof(fixture->hoc));
    other[16] = 55;
    create_file(fixture->root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    create_file(fixture->root, "atmosphere/package3", fixture->package, sizeof(fixture->package));
    create_file(fixture->root, HOC_KIP, fixture->old, sizeof(fixture->old));
    create_file(fixture->root, "atmosphere/kips/custom.kip1", other, sizeof(other));
    create_file(fixture->payloads, "loader-stock.kip", fixture->stock, sizeof(fixture->stock));
    create_file(fixture->payloads, "loader-hoc.kip", fixture->hoc, sizeof(fixture->hoc));
    create_file(fixture->payloads, "mesosphere.bin", fixture->kernel, sizeof(fixture->kernel));
    assert(setup_boot_list(fixture->root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(count == 2 && entries[0].supported && !entries[1].supported);
    fixture->options.entry = entries[0];
    strcpy(fixture->options.hoc_source, HOC_KIP);
}

static enum setup_boot_result install(struct fixture *fixture)
{
    char detail[256];
    enum setup_boot_result result = setup_boot_install(fixture->root, fixture->payloads,
        &fixture->manifest, &fixture->options, detail, sizeof(detail));
    return result;
}

static void refresh_entry(struct fixture *fixture)
{
    struct setup_boot_entry entries[64];
    size_t count;
    char detail[256];
    assert(setup_boot_list(fixture->root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK);
    fixture->options.entry = entries[0];
}

static void no_writes(struct fixture *fixture)
{
    char absolute[1024];
    check_file(fixture->root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    check_absent(fixture->root, BACKUP);
    check_absent(fixture->root, UPDATE_BACKUP);
    assert(path_join(absolute, fixture->root, "bootloader/autorun"));
    assert(access(absolute, F_OK) && errno == ENOENT);
}

static void test_validation(void)
{
    struct fixture f;
    char detail[256];
    fixture_init(&f);
    assert(!setup_boot_bundled_manifest());
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(setup_boot_install(f.root, f.payloads, NULL, &f.options, detail, sizeof(detail)) == SETUP_BOOT_MISSING);
    no_writes(&f);
    strcpy(f.manifest.release, "../../evil");
    assert(install(&f) == SETUP_BOOT_INVALID);
    no_writes(&f);
    strcpy(f.manifest.release, "ams-1.11.2-hoc-2.5.1");
    f.manifest.stock.sha256[0] ^= 1;
    assert(install(&f) == SETUP_BOOT_MISSING);
    no_writes(&f);
    f.manifest.stock.sha256[0] ^= 1;
    f.options.entry.ini_sha256[0] ^= 1;
    assert(install(&f) == SETUP_BOOT_CHANGED);
    no_writes(&f);
    remove_fixture(f.root);
}

static void test_fork_package(int hoc)
{
    struct fixture f;
    fixture_init(&f);
    f.package[100] ^= 1;
    replace_file(f.root, "atmosphere/package3", f.package, sizeof(f.package));
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    assert(install(&f) == SETUP_BOOT_OK);
    remove_fixture(f.root);
}

static void test_clone(int hoc)
{
    struct fixture f;
    enum setup_boot_loader installed_loader = SETUP_BOOT_HOC;
    struct buffer result;
    struct setup_boot_entry entries[64];
    size_t count;
    char detail[256], *clone;
    unsigned char expected[1024];
    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    assert(install(&f) == SETUP_BOOT_OK);
    assert(!setup_boot_needs_update(f.root, &f.manifest, &installed_loader));
    assert(installed_loader == (hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK));
    check_absent(f.root, BACKUP);
    check_absent(f.root, UPDATE_BACKUP);
    memcpy(expected, f.hoc, sizeof(expected));
    expected[524] = 42;
    check_file(f.root, HOC_KIP, hoc ? expected : f.old, sizeof(f.old));
    if (hoc) check_file(f.root, HOC_PREVIOUS, f.old, sizeof(f.old));
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert(result.size > strlen(original_ini));
    assert(!memcmp(result.data, original_ini, strlen(original_ini)));
    clone = strstr((char *)result.data, "[Autorun]\r\n");
    assert(clone && !strstr(clone + 1, "[Autorun]"));
    assert(strstr(clone, "emummcforce=1\r\nemupath=emuMMC/RAW1\r\n"));
    assert(strstr(clone, "icon=bootloader/res/My Icon.bmp\r\n# kept comment\r\n"));
    assert(strstr(clone, "kip1=atmosphere/kips/*\r\n"));
    assert(!strstr(clone, "secmon="));
    assert(!strstr(clone, "id=emummc") && !strstr(clone, "kernel=old/"));
    assert(strstr(clone, hoc ? "kip1=" HOC_KIP "\r\nkernel=" MESOSPHERE "\r\n" :
                               "kip1=" STOCK_KIP "\r\nkernel=" MESOSPHERE "\r\n"));
    assert(!strstr(clone, "\r\n\r\nkernel="));
    if (hoc) assert(strstr(clone, "kip1=" HOC_KIP "\r\n"));
    else
    {
        assert(strstr(clone, "kip1=" STOCK_KIP "\r\n"));
        check_file(f.root, STOCK_KIP, f.stock, sizeof(f.stock));
    }
    check_file(f.root, MESOSPHERE, f.kernel, sizeof(f.kernel));
    assert(setup_boot_list(f.root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK && count == 3);
    f.options.entry = entries[0];
    assert(install(&f) == SETUP_BOOT_OK);
    check_file(f.root, SETUP_BOOT_INI, result.data, result.size);
    free(result.data);
    remove_fixture(f.root);
}

static void test_existing_secmon(void)
{
    static const char ini[] = "[HOC]\n"
        "pkg3=atmosphere/package3\n"
        "kip1=" HOC_KIP "\n"
        "secmon=atmosphere/exosphere.bin\n"
        "emummcforce=1\n";
    static const char exosphere[] = "user exosphere";
    struct fixture f;
    struct buffer result;
    char *clone;
    fixture_init(&f);
    replace_file(f.root, SETUP_BOOT_INI, ini, sizeof(ini) - 1);
    create_file(f.root, "atmosphere/exosphere.bin", exosphere, sizeof(exosphere));
    refresh_entry(&f);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    assert(install(&f) == SETUP_BOOT_OK);
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert(!memcmp(result.data, ini, sizeof(ini) - 1));
    assert((clone = strstr((char *)result.data, "[Autorun]\n")));
    assert(strstr(clone, "kernel=" MESOSPHERE "\nsecmon=atmosphere/exosphere.bin\n"));
    check_file(f.root, "atmosphere/exosphere.bin", exosphere, sizeof(exosphere));
    free(result.data);
    remove_fixture(f.root);
}

static void test_update(int hoc)
{
    struct fixture f;
    struct buffer before, after;
    unsigned char expected[1024];
    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    assert(install(&f) == SETUP_BOOT_OK);
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &before) == 1);
    refresh_entry(&f);
    f.kernel[0] ^= 1;
    fill_payload(&f.manifest.mesosphere, f.kernel, sizeof(f.kernel));
    replace_file(f.payloads, "mesosphere.bin", f.kernel, sizeof(f.kernel));
    if (hoc)
    {
        f.hoc[400] ^= 1;
        fill_payload(&f.manifest.hoc, f.hoc, sizeof(f.hoc));
        fill_hoc_code_hash(&f.manifest, f.hoc, sizeof(f.hoc));
        replace_file(f.payloads, "loader-hoc.kip", f.hoc, sizeof(f.hoc));
    }
    assert(setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(install(&f) == SETUP_BOOT_OK);
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    check_absent(f.root, BACKUP);
    check_absent(f.root, UPDATE_BACKUP);
    check_absent(f.root, COMMITTED);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &after) == 1);
    assert(strstr((char *)after.data, "[Autorun]") &&
           !strstr(strstr((char *)after.data, "[Autorun]") + 1, "[Autorun]"));
    assert(after.size == before.size && !memcmp(after.data, before.data, before.size));
    assert(strstr((char *)after.data, "kernel=" MESOSPHERE));
    check_file(f.root, MESOSPHERE, f.kernel, sizeof(f.kernel));
    if (hoc)
    {
        memcpy(expected, f.hoc, sizeof(expected));
        expected[524] = 42;
        check_file(f.root, HOC_KIP, expected, sizeof(expected));
        assert(strstr((char *)after.data, "kip1=" HOC_KIP));
    }
    refresh_entry(&f);
    assert(install(&f) == SETUP_BOOT_OK);
    check_file(f.root, SETUP_BOOT_INI, after.data, after.size);
    free(before.data);
    free(after.data);
    remove_fixture(f.root);
}

static void test_update_detection(void)
{
    struct fixture f;
    enum setup_boot_loader loader = SETUP_BOOT_STOCK;
    char before[33], after[33];
    unsigned char installed[1024];
    fixture_init(&f);
    setup_boot_bundle_id(&f.manifest, before);
    assert(strlen(before) == 32);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    assert(install(&f) == SETUP_BOOT_OK);
    memcpy(installed, f.hoc, sizeof(installed));
    installed[524] = 91;
    replace_file(f.root, HOC_KIP, installed, sizeof(installed));
    assert(!setup_boot_needs_update(f.root, &f.manifest, &loader));
    assert(loader == SETUP_BOOT_HOC);
    installed[300] ^= 1;
    replace_file(f.root, HOC_KIP, installed, sizeof(installed));
    assert(setup_boot_needs_update(f.root, &f.manifest, NULL));
    installed[300] ^= 1;
    replace_file(f.root, HOC_KIP, installed, sizeof(installed));
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    f.manifest.mesosphere.sha256[0] ^= 1;
    assert(setup_boot_needs_update(f.root, &f.manifest, NULL));
    setup_boot_bundle_id(&f.manifest, after);
    assert(strcmp(before, after));
    remove_fixture(f.root);
}

static void test_hoc_rejection(void)
{
    struct fixture f;
    fixture_init(&f);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    f.manifest.hoc_config_size = 0;
    assert(install(&f) == SETUP_BOOT_INVALID);
    no_writes(&f);
    f.manifest.hoc_config_size = 64;
    f.old[516] = 6;
    replace_file(f.root, HOC_KIP, f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
    remove_fixture(f.root);
}

static void test_shared_hoc_package(void)
{
    struct fixture f;
    struct buffer ini;
    unsigned char other_package[2048] = {0};
    const char other_entry[] = "\r\n[Other Atmosphere]\r\nfss0=atmosphere/other-package3\r\nkip1=atmosphere/kips/*\r\n";
    fixture_init(&f);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &ini) == 1);
    assert(append(&ini, other_entry, sizeof(other_entry) - 1));
    replace_file(f.root, SETUP_BOOT_INI, ini.data, ini.size);
    free(ini.data);
    create_file(f.root, "atmosphere/other-package3", other_package, sizeof(other_package));
    refresh_entry(&f);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    check_file(f.root, HOC_KIP, f.old, sizeof(f.old));
    check_file(f.root, "atmosphere/other-package3", other_package, sizeof(other_package));
    remove_fixture(f.root);
}

static void test_hoc_probe(void)
{
    struct fixture f;
    struct setup_boot_hoc_info info;
    char path[1024];
    fixture_init(&f);
    assert(setup_boot_hoc_probe(f.root, &f.manifest, &info));
    assert(info.compatible && info.kip_version == 300 &&
           !strcmp(info.path, HOC_KIP));
    f.old[520] = 0xfb;
    f.old[521] = 0;
    replace_file(f.root, HOC_KIP, f.old, sizeof(f.old));
    assert(setup_boot_hoc_probe(f.root, &f.manifest, &info));
    assert(info.compatible && info.kip_version == 251);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    strcpy(f.options.hoc_source, info.path);
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    remove_fixture(f.root);

    fixture_init(&f);
    assert(path_join(path, f.root, HOC_KIP));
    assert(!unlink(path));
    assert(!setup_boot_hoc_probe(f.root, &f.manifest, &info));
    assert(!*info.path && !info.compatible);
    remove_fixture(f.root);
}

static void test_existing_kip(int hoc)
{
    struct fixture f;
    struct buffer ini, with_kip = {0}, result;
    const char extra[] = "kip1=atmosphere/kips/loader.kip\r\n";
    char *recovery;
    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    create_file(f.root, "atmosphere/kips/extra.kip.backup", f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_OK);
    remove_fixture(f.root);

    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    create_file(f.root, "atmosphere/kips/extra.kip", f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_OK);
    remove_fixture(f.root);

    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &ini) == 1);
    assert((recovery = strstr((char *)ini.data, "[Recovery]")));
    assert(append(&with_kip, ini.data, recovery - (char *)ini.data));
    assert(append(&with_kip, extra, sizeof(extra) - 1));
    assert(append(&with_kip, recovery, ini.size - (recovery - (char *)ini.data)));
    replace_file(f.root, SETUP_BOOT_INI, with_kip.data, with_kip.size);
    free(ini.data);
    free(with_kip.data);
    create_file(f.root, "atmosphere/kips/loader.kip", f.old, sizeof(f.old));
    refresh_entry(&f);
    assert(install(&f) == SETUP_BOOT_OK);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert(!strstr(strstr((char *)result.data, "[Autorun]"), "kip1=atmosphere/kips/loader.kip\r\n"));
    assert(strstr((char *)result.data, hoc ? "kip1=" HOC_KIP : "kip1=" STOCK_KIP));
    free(result.data);
    remove_fixture(f.root);
}

static void test_explicit_loader(void)
{
    static const char ini[] = "[Atmosphere]\n"
        "pkg3=atmosphere/package3\n"
        "kip1=atmosphere/kips/loader.kip\n"
        "emummc_force_disable=1\n"
        "memmode=1\n"
        "icon=bootloader/res/emummc.bmp\n\n";
    struct fixture f;
    struct buffer result;
    char *entry;
    fixture_init(&f);
    replace_file(f.root, SETUP_BOOT_INI, ini, sizeof(ini) - 1);
    refresh_entry(&f);
    assert(install(&f) == SETUP_BOOT_OK);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert((entry = strstr((char *)result.data, "[Autorun]\n")));
    assert(strstr(entry, "pkg3=atmosphere/package3\nkip1=" STOCK_KIP "\n"
                         "kernel=" MESOSPHERE "\nemummc_force_disable=1\n"));
    assert(strstr(entry, "memmode=1\nicon=bootloader/res/emummc.bmp\n"));
    assert(!strstr(entry, "kip1=atmosphere/kips/loader.kip"));
    free(result.data);
    remove_fixture(f.root);
}

static void test_legacy_entry(void)
{
    static const char old_entry[] = "\r\n[Autorun]\r\nfss0=atmosphere/package3\r\n"
        "kernel=bootloader/autorun/old/mesosphere.bin\r\n"
        "kip1=bootloader/autorun/old/autorun.kip\r\n";
    struct fixture f;
    struct buffer ini, result;
    char *entry;
    fixture_init(&f);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &ini) == 1);
    assert(append(&ini, old_entry, sizeof(old_entry) - 1));
    replace_file(f.root, SETUP_BOOT_INI, ini.data, ini.size);
    free(ini.data);
    create_file(f.root, "bootloader/autorun/old/mesosphere.bin", f.kernel, sizeof(f.kernel));
    create_file(f.root, "bootloader/autorun/old/autorun.kip", f.stock, sizeof(f.stock));
    refresh_entry(&f);
    assert(setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(install(&f) == SETUP_BOOT_OK);
    assert(!setup_boot_needs_update(f.root, &f.manifest, NULL));
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert((entry = strstr((char *)result.data, "[Autorun]\r\n")));
    assert(strstr(entry, "kernel=" MESOSPHERE "\r\n"));
    assert(strstr(entry, "kip1=" STOCK_KIP "\r\n"));
    assert(!strstr(entry, "bootloader/autorun/old/"));
    free(result.data);
    remove_fixture(f.root);
}

static void test_existing_patch_files(void)
{
    struct fixture f;
    unsigned char old_kernel[512];
    fixture_init(&f);
    memset(old_kernel, 0x77, sizeof(old_kernel));
    create_file(f.root, MESOSPHERE, old_kernel, sizeof(old_kernel));
    create_file(f.root, STOCK_KIP, f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_OK);
    check_file(f.root, MESOSPHERE, f.kernel, sizeof(f.kernel));
    check_file(f.root, STOCK_KIP, f.stock, sizeof(f.stock));
    check_file(f.root, MESOSPHERE_PREVIOUS, old_kernel, sizeof(old_kernel));
    check_file(f.root, STOCK_PREVIOUS, f.old, sizeof(f.old));
    remove_fixture(f.root);
}

static void test_recovery(int state)
{
    struct fixture f;
    struct transaction transaction = {{'A','R','B','O','O','T','1',0},{0},{0}};
    char updated[sizeof(original_ini) + 128];
    char absolute[1024], detail[256];
    enum setup_boot_result result;
    fixture_init(&f);
    snprintf(updated, sizeof(updated), "%s\r\n[Autorun]\r\nfss0=atmosphere/package3\r\n", original_ini);
    digest(original_ini, strlen(original_ini), transaction.before);
    digest(updated, strlen(updated), transaction.after);
    create_file(f.root, BACKUP, original_ini, strlen(original_ini));
    create_file(f.root, STAGED, updated, strlen(updated));
    create_file(f.root, JOURNAL, &transaction, sizeof(transaction));
    if (state == 1)
    {
        assert(path_join(absolute, f.root, SETUP_BOOT_INI));
        assert(!unlink(absolute));
    }
    else if (state == 2) replace_file(f.root, SETUP_BOOT_INI, updated, strlen(updated));
    else if (state == 3) replace_file(f.root, SETUP_BOOT_INI, "# user changed it", 17);
    else if (state == 4) replace_file(f.root, BACKUP, "bad backup", 10);
    else if (state == 5) replace_file(f.root, SETUP_BOOT_INI, updated, strlen(original_ini) + 10);
    else if (state == 6)
    {
        updated[strlen(original_ini) + 2] = '!';
        replace_file(f.root, SETUP_BOOT_INI, updated, strlen(original_ini) + 10);
    }
    result = setup_boot_recover(f.root, detail, sizeof(detail));
    assert(result == (state == 2 ? SETUP_BOOT_ALREADY : state == 3 || state == 4 || state == 6 ? SETUP_BOOT_RECOVERY : SETUP_BOOT_OK));
    if (state < 2 || state == 5) check_file(f.root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    if (state == 2) check_file(f.root, SETUP_BOOT_INI, updated, strlen(updated));
    if (state == 3) check_file(f.root, SETUP_BOOT_INI, "# user changed it", 17);
    if (result != SETUP_BOOT_RECOVERY) check_absent(f.root, BACKUP);
    remove_fixture(f.root);
}

static void test_update_recovery(void)
{
    struct fixture f;
    struct buffer before;
    struct transaction transaction = {{'A','R','B','O','O','T','2',0},{0},{0}};
    char absolute[1024], detail[256];
    const char after[] = "[Autorun]\nupdated\n";
    fixture_init(&f);
    assert(install(&f) == SETUP_BOOT_OK);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &before) == 1);
    digest(before.data, before.size, transaction.before);
    digest(after, sizeof(after) - 1, transaction.after);
    create_file(f.root, UPDATE_BACKUP, before.data, before.size);
    create_file(f.root, STAGED, after, sizeof(after) - 1);
    create_file(f.root, JOURNAL, &transaction, sizeof(transaction));
    assert(path_join(absolute, f.root, SETUP_BOOT_INI));
    assert(!unlink(absolute));
    assert(setup_boot_recover(f.root, detail, sizeof(detail)) == SETUP_BOOT_OK);
    check_file(f.root, SETUP_BOOT_INI, before.data, before.size);
    check_absent(f.root, UPDATE_BACKUP);
    free(before.data);
    remove_fixture(f.root);
}

static void test_fixed_update_recovery(int committed)
{
    struct fixture f;
    struct buffer ini;
    struct transaction transaction = {{'A','R','B','O','O','T','2',
        TX_MESOSPHERE | TX_MESOSPHERE_EXISTED | TX_STOCK | TX_STOCK_EXISTED},{0},{0}};
    unsigned char new_kernel[512], new_stock[1024];
    char detail[256];
    fixture_init(&f);
    assert(install(&f) == SETUP_BOOT_OK);
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &ini) == 1);
    digest(ini.data, ini.size, transaction.before);
    memcpy(transaction.after, transaction.before, sizeof(transaction.after));
    create_file(f.root, UPDATE_BACKUP, ini.data, ini.size);
    create_file(f.root, STAGED, ini.data, ini.size);
    create_file(f.root, JOURNAL, &transaction, sizeof(transaction));
    if (committed) create_file(f.root, COMMITTED, transaction.after, sizeof(transaction.after));
    memcpy(new_kernel, f.kernel, sizeof(new_kernel));
    memcpy(new_stock, f.stock, sizeof(new_stock));
    new_kernel[0] ^= 1;
    new_stock[300] ^= 1;
    assert(move_file(f.root, MESOSPHERE, MESOSPHERE_PREVIOUS));
    assert(move_file(f.root, STOCK_KIP, STOCK_PREVIOUS));
    create_file(f.root, MESOSPHERE, new_kernel, sizeof(new_kernel));
    create_file(f.root, STOCK_KIP, new_stock, sizeof(new_stock));
    assert(setup_boot_recover(f.root, detail, sizeof(detail)) ==
           (committed ? SETUP_BOOT_ALREADY : SETUP_BOOT_OK));
    check_file(f.root, MESOSPHERE, committed ? new_kernel : f.kernel, sizeof(f.kernel));
    check_file(f.root, STOCK_KIP, committed ? new_stock : f.stock, sizeof(f.stock));
    check_file(f.root, SETUP_BOOT_INI, ini.data, ini.size);
    if (committed)
    {
        check_file(f.root, MESOSPHERE_PREVIOUS, f.kernel, sizeof(f.kernel));
        check_file(f.root, STOCK_PREVIOUS, f.stock, sizeof(f.stock));
    }
    else
    {
        check_absent(f.root, MESOSPHERE_PREVIOUS);
        check_absent(f.root, STOCK_PREVIOUS);
    }
    check_absent(f.root, COMMITTED);
    free(ini.data);
    remove_fixture(f.root);
}

static void test_stale_backup_cleanup(void)
{
    struct fixture f;
    fixture_init(&f);
    create_file(f.root, BACKUP, original_ini, strlen(original_ini));
    create_file(f.root, UPDATE_BACKUP, original_ini, strlen(original_ini));
    assert(install(&f) == SETUP_BOOT_OK);
    check_absent(f.root, BACKUP);
    check_absent(f.root, UPDATE_BACKUP);
    remove_fixture(f.root);
}

static void test_hoc_recovery(void)
{
    struct fixture f;
    char detail[256];
    unsigned char expected[1024];
    fixture_init(&f);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    assert(install(&f) == SETUP_BOOT_OK);
    memcpy(expected, f.hoc, sizeof(expected));
    expected[524] = 42;
    assert(remove_file(f.root, HOC_PREVIOUS));
    assert(move_file(f.root, HOC_KIP, HOC_PREVIOUS));
    assert(setup_boot_recover(f.root, detail, sizeof(detail)) == SETUP_BOOT_OK);
    check_file(f.root, HOC_KIP, expected, sizeof(expected));
    remove_fixture(f.root);
}

static void test_append_failure(size_t bytes)
{
    struct fixture f;
    char detail[256], absolute[1024];
    fixture_init(&f);
    assert(path_join(absolute, f.root, SETUP_BOOT_INI));
    assert(!lstat(absolute, &fail_append_file));
    fail_append_after = bytes;
    assert(install(&f) == SETUP_BOOT_IO);
    fail_append_after = -1;
    check_file(f.root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    check_absent(f.root, BACKUP);
    assert(setup_boot_recover(f.root, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(install(&f) == SETUP_BOOT_OK);
    remove_fixture(f.root);
}

static void test_staging_failure(int stage)
{
    struct fixture f;
    char detail[256];
    fixture_init(&f);
    fail_staging_skip = stage;
    fail_staging_after = 7;
    assert(install(&f) == SETUP_BOOT_IO);
    fail_staging_skip = -1;
    fail_staging_after = -1;
    check_file(f.root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    assert(setup_boot_recover(f.root, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(install(&f) == SETUP_BOOT_OK);
    remove_fixture(f.root);
}

static void test_staging_conflict(void)
{
    struct fixture f;
    char detail[512];
    fixture_init(&f);
    create_file(f.root, STAGED, "incomplete", 10);
    assert(setup_boot_install(f.root, f.payloads, &f.manifest, &f.options, detail, sizeof(detail)) == SETUP_BOOT_IO);
    assert(strstr(detail, STAGED) && strstr(detail, "manually"));
    check_file(f.root, STAGED, "incomplete", 10);
    check_file(f.root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    remove_fixture(f.root);
}

static void test_case_sensitive_package(void)
{
    struct fixture f;
    struct setup_boot_entry entries[64];
    const char ini[] = "[Test]\nfss0=atmosphere/wrong-package3\nFSS0=atmosphere/package3\n";
    char detail[256];
    size_t count;
    fixture_init(&f);
    replace_file(f.root, SETUP_BOOT_INI, ini, strlen(ini));
    assert(setup_boot_list(f.root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(count == 1 && entries[0].supported);
    f.options.entry = entries[0];
    assert(install(&f) == SETUP_BOOT_MISSING);
    check_file(f.root, SETUP_BOOT_INI, ini, strlen(ini));
    check_absent(f.root, BACKUP);
    remove_fixture(f.root);
}

int main(void)
{
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    unsigned char hash[32];
    unsigned int i;
    digest("abc", 3, hash);
    assert(!memcmp(hash, expected, 32));
    test_validation();
    test_fork_package(0);
    test_fork_package(1);
    test_clone(0);
    test_clone(1);
    test_existing_secmon();
    test_update(0);
    test_update(1);
    test_update_detection();
    test_hoc_rejection();
    test_shared_hoc_package();
    test_hoc_probe();
    test_existing_kip(0);
    test_existing_kip(1);
    test_explicit_loader();
    test_legacy_entry();
    test_existing_patch_files();
    for (i = 0; i < 7; i++) test_recovery(i);
    test_update_recovery();
    test_fixed_update_recovery(0);
    test_fixed_update_recovery(1);
    test_stale_backup_cleanup();
    test_hoc_recovery();
    test_append_failure(0);
    test_append_failure(1);
    test_append_failure(60);
    for (i = 0; i < 5; i++) test_staging_failure(i);
    test_staging_conflict();
    test_case_sensitive_package();
    puts("boot setup: clone, updates, HOC preservation, path safety and recovery passed");
    return 0;
}
