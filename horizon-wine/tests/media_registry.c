#include <assert.h>
#include "../source/horizon_dlls.c"
#include "../../dlls/ntdll/unix/horizon_registry.h"

static long long registry_time(void) { return 1; }

static struct horizon_reg_key *open_key(struct horizon_reg *reg, const char *path)
{
    unsigned short wide[256];
    struct horizon_reg_key *key;
    size_t len = strlen(path);
    assert(len < sizeof(wide) / sizeof(wide[0]));
    for (size_t i = 0; i <= len; i++) wide[i] = path[i];
    assert(!horizon_reg_open(reg, reg->root, wide, len * 2, 0, &key));
    return key;
}

static void check_conversion(const char *registry, const char *clsid, const char *category,
                             const unsigned char output_subtype[16])
{
    static const unsigned char video[] = {0x76,0x69,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    static const unsigned char nv12[] = {0x4e,0x56,0x31,0x32,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    static const unsigned short names[][12] = {u"InputTypes", u"OutputTypes"};
    struct horizon_reg reg;
    struct horizon_reg_key *key;
    unsigned int errors = 0, index;
    char path[256];
    char *text;

    assert(registry && horizon_reg_init(&reg, registry_time, NULL));
    assert((text = malloc(strlen(registry) + 32)));
    sprintf(text, "WINE REGISTRY Version 2\n%s", registry);
    assert(!horizon_reg_load(&reg, reg.root, text, strlen(text), &errors) && !errors);
    free(text);
    snprintf(path, sizeof(path), "Software\\Classes\\MediaFoundation\\Transforms\\%s", clsid);
    key = open_key(&reg, path);
    for (unsigned int i = 0; i < 2; i++)
    {
        const struct horizon_reg_value *value = horizon_reg_find_value(key, names[i], (10 + i) * 2, &index);
        unsigned int offset;
        assert(value && value->type == HORIZON_REG_BINARY && !(value->len % 32));
        for (offset = 0; offset < value->len; offset += 32)
            if (!memcmp(value->data + offset, video, 16) &&
                !memcmp(value->data + offset + 16, i ? output_subtype : nv12, 16)) break;
        assert(offset < value->len);
    }
    horizon_reg_release(&reg, key);
    snprintf(path, sizeof(path), "Software\\Classes\\MediaFoundation\\Transforms\\Categories\\%s\\%s", category, clsid);
    horizon_reg_release(&reg, open_key(&reg, path));
    horizon_reg_release(&reg, reg.root);
}

int main(int argc, char **argv)
{
    struct horizon_dll_manifest original = {0}, loaded = {0};
    struct kept *kept;
    assert(argc == 3);
    assert(horizon_dlls_load(argv[1], NULL, 0, &original) == HORIZON_DLLS_OK);
    kept = calloc(original.count, sizeof(*kept));
    assert(kept);
    unsigned int registrations = 0, converters = 0;
    for (unsigned int i = 0; i < original.count; i++)
    {
        kept[i].file = &original.files[i]; kept[i].from = &original;
        registrations += original.files[i].registry != NULL;
        if (!strcmp(original.files[i].name, "colorcnv.dll"))
        {
            static const unsigned char rgb[] = {22,0,0,0,0x4f,0x52,0xce,0x11,0x9f,0x53,0,0x20,0xaf,0xb,0xa7,0x70};
            check_conversion(original.files[i].registry, "98230571-0087-4204-b020-3282538e57d3",
                             "12e17c21-532c-4a6e-8a1c-40825a736397", rgb);
            converters++;
        }
        if (!strcmp(original.files[i].name, "msvproc.dll"))
        {
            static const unsigned char rgb[] = {22,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
            check_conversion(original.files[i].registry, "88753b26-5b24-49bd-b2e7-0c445c78c982",
                             "302ea3fc-aa5f-47f9-9f7a-c2188bb16302", rgb);
            converters++;
        }
    }
    assert(registrations >= 8);
    assert(converters == 4);
    assert(write_record(argv[2], &original, kept, original.count));
    assert(horizon_dlls_load(argv[2], NULL, 0, &loaded) == HORIZON_DLLS_OK);
    assert(loaded.count == original.count);
    for (unsigned int i = 0; i < original.count; i++)
        if (original.files[i].registry)
            assert(loaded.files[i].registry && !strcmp(original.files[i].registry, loaded.files[i].registry));
    horizon_dlls_free(&loaded);
    horizon_dlls_free(&original);
    free(kept);
    puts("Media registration manifest round-trip passed");
    return 0;
}
