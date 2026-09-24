#include <assert.h>
#include "../source/autorun_install.c"

static int fail_publish;
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    if (fail_publish && strstr(to, "/drive_c/windows/system32/ntdll.dll"))
    { fail_publish = 0; errno = EIO; return -1; }
    return __real_rename(from, to);
}

static void test_managed(void)
{
    const char *replace[] = {
        "wine-nx-runtime.nro", "build-manifest.json", "drive_c/dxvk64/dxgi.dll",
        "drive_c/windows/system32/ntdll.dll", "drive_c/windows/syswow64/kernel32.dll",
        "share/wine/nls/locale.nls", "share/wine/fonts/tahoma.ttf",
        "licenses/Wine-LGPL.txt"
    };
    const char *keep[] = {
        "config/launcher.txt", "registry/user.reg", "drive_c/users/Switch/save.dat",
        "drive_c/windows/custom.ini", "drive_c/windows/system32/ntdll.dll/child",
        "../wine-nx-runtime.nro", "drive_c/windows/system32/../ntdll.dll"
    };
    for (unsigned i = 0; i < sizeof(replace) / sizeof(*replace); i++) assert(managed(replace[i]));
    for (unsigned i = 0; i < sizeof(keep) / sizeof(*keep); i++) assert(!managed(keep[i]));
    puts("app updater: managed paths and state preservation passed");
}

static void test_plan_limits(void)
{
    struct install_plan *plan = calloc(1, sizeof(*plan));
    assert(plan);
    plan->header.magic = INSTALL_MAGIC;
    strcpy(plan->header.tag, "test");
    plan->header.count = 8;
    for (unsigned i = 0; i < 8; i++)
    {
        snprintf(plan->entries[i].path, sizeof(plan->entries[i].path), "drive_c/windows/system32/test%u.dll", i);
        plan->entries[i].size = 512ull * 1024 * 1024;
    }
    strcpy(plan->entries[7].path, INSTALL_NRO);
    assert(valid_plan(plan));
    plan->entries[0].size++;
    assert(!valid_plan(plan));
    plan->entries[0].size--;
    strcpy(plan->entries[7].path, "drive_c/windows/system32/test7.dll");
    strcpy(plan->entries[8].path, INSTALL_NRO);
    plan->entries[8].size = 1;
    plan->header.count = 9;
    assert(!valid_plan(plan));
    plan->header.count = INSTALL_MAX_FILES + 1;
    assert(!valid_plan(plan));
    free(plan);
    puts("app updater: 4GiB plan accepted; aggregate, 512MiB file and 4096-file bounds enforced");
}

int main(int argc, char **argv)
{
    test_managed();
    test_plan_limits();
    if (argc == 1) return 0;
    assert(argc == 4);
    fail_publish = atoi(argv[3]);
    enum autorun_install_result result = autorun_install_archive(argv[1], argv[2], "bundle-test", 1, NULL, NULL);
    if (atoi(argv[3]))
    {
        assert(result == AUTORUN_INSTALL_IO);
        assert(autorun_install_recover(argv[1], 0) == 0);
    }
    else
    {
        if (result != AUTORUN_INSTALL_OK) fprintf(stderr, "%s\n", autorun_install_error(result));
        assert(result == AUTORUN_INSTALL_OK);
        assert(autorun_install_recover(argv[1], 0) == 1);
        assert(autorun_install_finish(argv[1]));
    }
    return 0;
}
