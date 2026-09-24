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

struct fixture
{
    char root[64], payloads[1024];
    unsigned char stock[1024], hoc[1024], old[1024], kernel[512], package[2048];
    struct setup_boot_manifest manifest;
    struct setup_boot_options options;
};

static void fixture_init(struct fixture *fixture)
{
    struct setup_boot_entry entries[64];
    unsigned char normalized[1024], other[256] = "KIP1sysmodule";
    size_t count;
    char detail[256];
    memset(fixture, 0, sizeof(*fixture));
    strcpy(fixture->root, "/tmp/autorun-boot-fixture-XXXXXX");
    assert(mkdtemp(fixture->root));
    assert(path_join(fixture->payloads, fixture->root, "payloads"));
    assert(!mkdir(fixture->payloads, 0700));
    memcpy(fixture->stock, "KIP1loader", 10);
    fixture->stock[16] = 1;
    fixture->stock[23] = 1;
    memcpy(fixture->hoc, fixture->stock, sizeof(fixture->stock));
    memcpy(fixture->hoc + 512, "CUST\x02\x00\x00\x00", 8);
    memcpy(fixture->old, fixture->hoc, sizeof(fixture->old));
    fixture->old[524] = 42;
    memset(fixture->kernel, 0xaa, sizeof(fixture->kernel));
    memset(fixture->package, 0x44, sizeof(fixture->package));
    fixture->manifest.version = 1;
    strcpy(fixture->manifest.release, "fixture-v1");
    fill_payload(&fixture->manifest.stock, fixture->stock, sizeof(fixture->stock));
    fill_payload(&fixture->manifest.hoc, fixture->hoc, sizeof(fixture->hoc));
    fill_payload(&fixture->manifest.mesosphere, fixture->kernel, sizeof(fixture->kernel));
    fill_payload(&fixture->manifest.package3, fixture->package, sizeof(fixture->package));
    fixture->manifest.hoc_config_offset = 512;
    fixture->manifest.hoc_config_size = 64;
    fixture->manifest.hoc_config_revision = 2;
    fill_payload(&fixture->manifest.hoc_original, fixture->old, sizeof(fixture->old));
    memcpy(normalized, fixture->old, sizeof(normalized));
    memset(normalized + 512, 0, 64);
    digest(normalized, sizeof(normalized), fixture->manifest.hoc_original_normalized_sha256);
    other[16] = 55;
    create_file(fixture->root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    create_file(fixture->root, "atmosphere/package3", fixture->package, sizeof(fixture->package));
    create_file(fixture->root, "atmosphere/kips/renamed-loader.kip", fixture->old, sizeof(fixture->old));
    create_file(fixture->root, "atmosphere/kips/custom.kip", other, sizeof(other));
    create_file(fixture->payloads, "loader-stock.kip", fixture->stock, sizeof(fixture->stock));
    create_file(fixture->payloads, "loader-hoc.kip", fixture->hoc, sizeof(fixture->hoc));
    create_file(fixture->payloads, "mesosphere.bin", fixture->kernel, sizeof(fixture->kernel));
    assert(setup_boot_list(fixture->root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(count == 2 && entries[0].supported && !entries[1].supported);
    fixture->options.entry = entries[0];
}

static enum setup_boot_result install(struct fixture *fixture)
{
    char detail[256];
    enum setup_boot_result result = setup_boot_install(fixture->root, fixture->payloads,
        &fixture->manifest, &fixture->options, detail, sizeof(detail));
    return result;
}

static void no_writes(struct fixture *fixture)
{
    char absolute[1024];
    check_file(fixture->root, SETUP_BOOT_INI, original_ini, strlen(original_ini));
    assert(path_join(absolute, fixture->root, BACKUP));
    assert(access(absolute, F_OK) && errno == ENOENT);
    assert(path_join(absolute, fixture->root, "bootloader/autorun"));
    assert(access(absolute, F_OK) && errno == ENOENT);
}

static void test_validation(void)
{
    struct fixture f;
    char detail[256];
    fixture_init(&f);
    assert(!setup_boot_bundled_manifest());
    assert(setup_boot_install(f.root, f.payloads, NULL, &f.options, detail, sizeof(detail)) == SETUP_BOOT_MISSING);
    no_writes(&f);
    strcpy(f.manifest.release, "../../evil");
    assert(install(&f) == SETUP_BOOT_INVALID);
    no_writes(&f);
    strcpy(f.manifest.release, "fixture-v1");
    f.manifest.package3.sha256[0] ^= 1;
    assert(install(&f) == SETUP_BOOT_HASH);
    no_writes(&f);
    f.manifest.package3.sha256[0] ^= 1;
    f.manifest.stock.sha256[0] ^= 1;
    assert(install(&f) == SETUP_BOOT_MISSING);
    no_writes(&f);
    f.manifest.stock.sha256[0] ^= 1;
    f.options.entry.ini_sha256[0] ^= 1;
    assert(install(&f) == SETUP_BOOT_CHANGED);
    no_writes(&f);
    remove_fixture(f.root);
}

static void test_clone(int hoc)
{
    struct fixture f;
    struct buffer result;
    struct setup_boot_entry entries[64];
    size_t count;
    char detail[256], *clone, *path, *end, relative[256];
    fixture_init(&f);
    f.options.loader = hoc ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    f.options.preserve_hoc = hoc;
    assert(install(&f) == SETUP_BOOT_OK);
    check_file(f.root, BACKUP, original_ini, strlen(original_ini));
    check_file(f.root, "atmosphere/kips/renamed-loader.kip", f.old, sizeof(f.old));
    assert(read_file(f.root, SETUP_BOOT_INI, INI_LIMIT, &result) == 1);
    assert(result.size > strlen(original_ini));
    assert(!memcmp(result.data, original_ini, strlen(original_ini)));
    clone = strstr((char *)result.data, "[Autorun]\r\n");
    assert(clone && !strstr(clone + 1, "[Autorun]"));
    assert(strstr(clone, "emummcforce=1\r\nemupath=emuMMC/RAW1\r\n"));
    assert(strstr(clone, "icon=bootloader/res/My Icon.bmp\r\n# kept comment\r\n"));
    assert(strstr(clone, "kip1=atmosphere/kips/custom.kip\r\n"));
    assert(!strstr(clone, "id=emummc") && !strstr(clone, "kernel=old/") && !strstr(clone, "renamed-loader"));
    assert((path = strstr(clone, "kip1=bootloader/autorun/")));
    path += 5;
    assert((end = strchr(path, '\r')));
    assert((size_t)(end - path) < sizeof(relative));
    memcpy(relative, path, end - path);
    relative[end - path] = 0;
    check_file(f.root, relative, hoc ? f.old : f.stock, sizeof(f.stock));
    assert(setup_boot_list(f.root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK && count == 3);
    f.options.entry = entries[0];
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    check_file(f.root, SETUP_BOOT_INI, result.data, result.size);
    free(result.data);
    remove_fixture(f.root);
}

static void test_hoc_rejection(void)
{
    struct fixture f;
    fixture_init(&f);
    f.options.loader = SETUP_BOOT_HOC;
    f.options.preserve_hoc = 1;
    f.manifest.hoc_config_size = 0;
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
    f.manifest.hoc_config_size = 64;
    f.old[800] ^= 1;
    replace_file(f.root, "atmosphere/kips/renamed-loader.kip", f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
    remove_fixture(f.root);
}

static void test_unknown_and_duplicate_kip(void)
{
    struct fixture f;
    fixture_init(&f);
    create_file(f.root, "atmosphere/kips/extra.kip.backup", f.old, sizeof(f.old));
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
    replace_file(f.root, "atmosphere/kips/extra.kip.backup", "bad", 3);
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
    remove_fixture(f.root);
}

static void test_symlink(void)
{
    struct fixture f;
    char absolute[1024];
    fixture_init(&f);
    assert(path_join(absolute, f.root, "atmosphere/kips/custom.kip"));
    assert(!unlink(absolute));
    assert(!symlink("/etc/passwd", absolute));
    assert(install(&f) == SETUP_BOOT_UNSUPPORTED);
    no_writes(&f);
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
    check_file(f.root, BACKUP, original_ini, strlen(original_ini));
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
    char detail[256], absolute[1024];
    size_t count;
    fixture_init(&f);
    replace_file(f.root, SETUP_BOOT_INI, ini, strlen(ini));
    f.package[0] ^= 1;
    create_file(f.root, "atmosphere/wrong-package3", f.package, sizeof(f.package));
    assert(setup_boot_list(f.root, entries, 64, &count, detail, sizeof(detail)) == SETUP_BOOT_OK);
    assert(count == 1 && entries[0].supported);
    f.options.entry = entries[0];
    assert(install(&f) == SETUP_BOOT_HASH);
    check_file(f.root, SETUP_BOOT_INI, ini, strlen(ini));
    assert(path_join(absolute, f.root, BACKUP));
    assert(access(absolute, F_OK) && errno == ENOENT);
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
    test_clone(0);
    test_clone(1);
    test_hoc_rejection();
    test_unknown_and_duplicate_kip();
    test_symlink();
    for (i = 0; i < 7; i++) test_recovery(i);
    test_append_failure(0);
    test_append_failure(1);
    test_append_failure(60);
    for (i = 0; i < 5; i++) test_staging_failure(i);
    test_staging_conflict();
    test_case_sensitive_package();
    puts("boot setup: clone, hashes, HOC preservation, path safety and recovery passed");
    return 0;
}
