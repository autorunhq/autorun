#include "setup_boot.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef WINE_NX_BOOT_BUNDLE
#include "setup_boot_manifest.h"
#endif
#ifdef __SWITCH__
#include <switch.h>
#else
#include <openssl/sha.h>
#endif

#define INI_LIMIT (256u * 1024u)
#define PAYLOAD_LIMIT (32u * 1024u * 1024u)
#define BACKUP "bootloader/hekate_ipl.ini.autorun-backup"
#define UPDATE_BACKUP "bootloader/hekate_ipl.ini.autorun-before"
#define STAGED "bootloader/hekate_ipl.ini.autorun-new"
#define JOURNAL "bootloader/hekate_ipl.ini.autorun-transaction"
#define COMMITTED "bootloader/hekate_ipl.ini.autorun-committed"
#define HOC_KIP "atmosphere/kips/hoc.kip"
#define STOCK_KIP "atmosphere/kips/autorun.kip"
#define MESOSPHERE "atmosphere/mesosphere.bin"
#define STOCK_PREVIOUS "atmosphere/kips/autorun.kip.autorun-previous"
#define STOCK_STAGED "atmosphere/kips/autorun.kip.autorun-new"
#define MESOSPHERE_PREVIOUS "atmosphere/mesosphere.bin.autorun-previous"
#define MESOSPHERE_STAGED "atmosphere/mesosphere.bin.autorun-new"
#define HOC_PREVIOUS "bootloader/hoc.kip.autorun-previous"
#define HOC_STAGED "bootloader/hoc.kip.autorun-new"
#define TX_MESOSPHERE 1
#define TX_MESOSPHERE_EXISTED 2
#define TX_STOCK 4
#define TX_STOCK_EXISTED 8

struct buffer { unsigned char *data; size_t size; };
struct line { size_t start, end, next; char key[64], value[512]; };
struct transaction { unsigned char magic[8], before[32], after[32]; };

static enum setup_boot_result report(enum setup_boot_result result, char *out, size_t size,
                                    const char *format, ...)
{
    va_list args;
    if (out && size)
    {
        va_start(args, format);
        vsnprintf(out, size, format, args);
        va_end(args);
    }
    return result;
}

const struct setup_boot_manifest *setup_boot_bundled_manifest(void)
{
#ifdef WINE_NX_BOOT_BUNDLE
    return &setup_boot_release_manifest;
#else
    return NULL;
#endif
}

static void digest(const void *data, size_t size, unsigned char out[32])
{
#ifdef __SWITCH__
    sha256CalculateHash(out, data, size);
#else
    SHA256(data, size, out);
#endif
}

static int hash_present(const unsigned char hash[32])
{
    unsigned int i;
    for (i = 0; i < 32; i++) if (hash[i]) return 1;
    return 0;
}

void setup_boot_bundle_id(const struct setup_boot_manifest *manifest, char out[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char hashes[96], hash[32];
    size_t i;
    if (!manifest) { out[0] = 0; return; }
    memcpy(hashes, manifest->stock.sha256, 32);
    memcpy(hashes + 32, manifest->hoc_code_sha256, 32);
    memcpy(hashes + 64, manifest->mesosphere.sha256, 32);
    digest(hashes, sizeof(hashes), hash);
    for (i = 0; i < 16; i++)
    {
        out[i * 2] = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 15];
    }
    out[32] = 0;
}

static int plain_path(const char *path)
{
    const char *p, *part = path;
    if (!path || !*path) return 0;
    for (p = path;; p++)
    {
        unsigned char c = *p;
        if (!c || c == '/')
        {
            if (p == part || (p - part == 1 && *part == '.') ||
                (p - part == 2 && part[0] == '.' && part[1] == '.') ||
                p[-1] == '.' || p[-1] == ' ') return 0;
            if (!c) return 1;
            part = p + 1;
        }
        else if (c < 32 || c == 127 || strchr("\\:*?\"<>|;", c)) return 0;
    }
}

static int path_join(char out[1024], const char *root, const char *relative)
{
    int n;
    if (!root || !*root || !plain_path(relative)) return 0;
    n = snprintf(out, 1024, "%s/%s", root, relative);
    return n >= 0 && n < 1024;
}

static int safe_parents(const char *root, const char *relative, int create)
{
    char path[1024], *p;
    struct stat st;
    if (!path_join(path, root, relative) || lstat(root, &st) || !S_ISDIR(st.st_mode)) return 0;
    for (p = path + strlen(root) + 1; *p; p++)
    {
        if (*p != '/') continue;
        *p = 0;
        if (lstat(path, &st))
        {
            if (!create || errno != ENOENT || mkdir(path, 0777)) return 0;
        }
        else if (!S_ISDIR(st.st_mode)) return 0;
        *p = '/';
    }
    if (!lstat(path, &st)) return S_ISREG(st.st_mode);
    return errno == ENOENT;
}

static int read_file(const char *root, const char *relative, size_t limit, struct buffer *out)
{
    char path[1024];
    struct stat st;
    FILE *file;
    size_t size;
    int ok;
    memset(out, 0, sizeof(*out));
    if (!safe_parents(root, relative, 0) || !path_join(path, root, relative)) return -1;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > limit) return -1;
    size = (size_t)st.st_size;
    if (!(out->data = malloc(size + 1))) return -1;
    if (!(file = fopen(path, "rb"))) { free(out->data); out->data = NULL; return -1; }
    ok = fread(out->data, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) ok = 0;
    if (!ok) { free(out->data); out->data = NULL; return -1; }
    out->data[size] = 0;
    out->size = size;
    return 1;
}

static int sync_path(const char *path)
{
#ifdef __SWITCH__
    char device[32];
    const char *colon = strchr(path, ':');
    if (!colon || colon - path >= (int)sizeof(device)) return 0;
    memcpy(device, path, colon - path);
    device[colon - path] = 0;
    return R_SUCCEEDED(fsdevCommitDevice(device));
#else
    char parent[1024], *slash;
    int fd, ok;
    if (strlen(path) >= sizeof(parent)) return 0;
    strcpy(parent, path);
    if (!(slash = strrchr(parent, '/'))) return 0;
    *slash = 0;
    if ((fd = open(parent, O_RDONLY | O_DIRECTORY)) < 0) return 0;
    ok = !fsync(fd);
    close(fd);
    return ok;
#endif
}

static int write_new(const char *root, const char *relative, const void *data, size_t size)
{
    char path[1024];
    const unsigned char *p = data;
    size_t done = 0;
    int fd, ok = 1;
    if (!safe_parents(root, relative, 1) || !path_join(path, root, relative)) return 0;
    if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) return 0;
    while (done < size)
    {
        ssize_t n = write(fd, p + done, size - done);
        if (n <= 0) { ok = 0; break; }
        done += n;
    }
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (ok) ok = sync_path(path);
    if (!ok)
    {
        unlink(path);
        sync_path(path);
    }
    return ok;
}

static int same_hash(const struct buffer *buffer, const unsigned char expected[32])
{
    unsigned char hash[32];
    digest(buffer->data, buffer->size, hash);
    return !memcmp(hash, expected, 32);
}

static int next_line(const struct buffer *text, size_t *position, struct line *line)
{
    size_t p, begin, end, equals;
    if (*position >= text->size) return 0;
    memset(line, 0, sizeof(*line));
    line->start = *position;
    p = *position;
    while (p < text->size && text->data[p] != '\n') p++;
    line->next = p < text->size ? p + 1 : p;
    line->end = p;
    if (p > line->start && text->data[p - 1] == '\r') p--;
    end = p;
    begin = line->start;
    if (!begin && end >= 3 && !memcmp(text->data, "\xef\xbb\xbf", 3)) begin = 3;
    while (begin < end && (text->data[begin] == ' ' || text->data[begin] == '\t')) begin++;
    while (end > begin && (text->data[end - 1] == ' ' || text->data[end - 1] == '\t')) end--;
    *position = line->next;
    if (begin == end || text->data[begin] == '#' || text->data[begin] == ';') return 1;
    if (text->data[begin] == '[' || text->data[begin] == '{')
    {
        int section = text->data[begin] == '[';
        if (end - begin < 3 || text->data[end - 1] != (section ? ']' : '}') || end - begin - 2 >= sizeof(line->value)) return -1;
        strcpy(line->key, section ? "[" : "{");
        memcpy(line->value, text->data + begin + 1, end - begin - 2);
        return 1;
    }
    for (equals = begin; equals < end && text->data[equals] != '='; equals++);
    if (equals == end) return -1;
    p = equals;
    while (p > begin && (text->data[p - 1] == ' ' || text->data[p - 1] == '\t')) p--;
    if (p == begin || p - begin >= sizeof(line->key)) return -1;
    memcpy(line->key, text->data + begin, p - begin);
    begin = equals + 1;
    while (begin < end && (text->data[begin] == ' ' || text->data[begin] == '\t')) begin++;
    if (end - begin >= sizeof(line->value)) return -1;
    memcpy(line->value, text->data + begin, end - begin);
    return 1;
}

static int scan_entries(const struct buffer *ini, struct setup_boot_entry *entries,
                        size_t capacity, size_t *count)
{
    size_t position = 0, n = 0;
    struct setup_boot_entry *current = NULL;
    struct line line;
    unsigned char hash[32];
    int r, package = 0;
    if (memchr(ini->data, 0, ini->size)) return 0;
    digest(ini->data, ini->size, hash);
    while ((r = next_line(ini, &position, &line)) > 0)
    {
        if (!strcmp(line.key, "[") || !strcmp(line.key, "{"))
        {
            if (current)
            {
                current->length = line.start - current->offset;
                if (package != 1) snprintf(current->reason, sizeof(current->reason), "Requires one pkg3/fss0 Atmosphere package");
                current->supported = !*current->reason;
            }
            current = NULL;
            package = 0;
            if (strcmp(line.key, "[") || !strcmp(line.value, "config")) continue;
            if (n >= capacity || strlen(line.value) >= sizeof(entries[n].name)) return 0;
            current = &entries[n++];
            memset(current, 0, sizeof(*current));
            strcpy(current->name, line.value);
            current->offset = line.start;
            memcpy(current->ini_sha256, hash, 32);
            if (!strcasecmp(line.value, SETUP_BOOT_ENTRY_NAME)) strcpy(current->reason, "Autorun entry already exists");
        }
        else if (current)
        {
            if (!strcmp(line.key, "pkg3") || !strcmp(line.key, "fss0")) package++;
            if (!strcmp(line.key, "payload") ||
                (!strcmp(line.key, "l4t") && strcmp(line.value, "0")) ||
                (!strcmp(line.key, "stock") && strcmp(line.value, "0")))
                strcpy(current->reason, "Payload, stock OS and L4T entries cannot load the Autorun kernel");
        }
    }
    if (current)
    {
        current->length = ini->size - current->offset;
        if (package != 1) strcpy(current->reason, "Requires one pkg3/fss0 Atmosphere package");
        current->supported = !*current->reason;
    }
    *count = n;
    return r >= 0;
}

static int managed_path(const char *path, const char *name)
{
    size_t path_size = strlen(path), name_size = strlen(name);
    return !strncmp(path, "bootloader/autorun/", 19) &&
        path_size > name_size + 19 &&
        !strcmp(path + path_size - name_size, name);
}

static int loader_kip(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return !strcasecmp(name, "loader.kip") || !strcasecmp(name, "hoc.kip") ||
           !strcasecmp(name, "autorun.kip");
}

static int managed_entry(const struct buffer *ini, const struct setup_boot_entry *entry)
{
    struct line line;
    size_t position = entry->offset, end = position + entry->length;
    int kernel = 0, loader = 0, hoc_loader = 0, package = 0;
    if (next_line(ini, &position, &line) != 1 || strcmp(line.key, "[") ||
        strcasecmp(line.value, SETUP_BOOT_ENTRY_NAME)) return 0;
    while (position < end)
    {
        if (next_line(ini, &position, &line) != 1) return 0;
        if (!strcmp(line.key, "kernel"))
            kernel += !strcmp(line.value, MESOSPHERE) || managed_path(line.value, "/mesosphere.bin") ? 1 : 2;
        if (!strcmp(line.key, "kip1"))
        {
            if (!strcmp(line.value, STOCK_KIP) || managed_path(line.value, "/autorun.kip")) loader++;
            else if (!strcmp(line.value, HOC_KIP)) hoc_loader = 1;
        }
        if (!strcmp(line.key, "pkg3") || !strcmp(line.key, "fss0")) package++;
    }
    return kernel == 1 && package == 1 && (loader == 1 || (!loader && hoc_loader));
}

static int entry_loads_hoc(const struct buffer *ini, const struct setup_boot_entry *entry)
{
    struct line line;
    size_t position = entry->offset, end = position + entry->length;
    while (position < end)
    {
        const char *value;
        if (next_line(ini, &position, &line) != 1) return 0;
        if (strcmp(line.key, "kip1")) continue;
        value = *line.value == '/' ? line.value + 1 : line.value;
        if (!strcasecmp(value, HOC_KIP) || !strcasecmp(value, "atmosphere/kips/*")) return 1;
    }
    return 0;
}

enum setup_boot_result setup_boot_list(const char *root, struct setup_boot_entry *entries,
    size_t capacity, size_t *count, char *detail, size_t detail_size)
{
    struct buffer ini;
    int r;
    if (!entries || !count || !capacity) return SETUP_BOOT_INVALID;
    *count = 0;
    r = read_file(root, SETUP_BOOT_INI, INI_LIMIT, &ini);
    if (r != 1) return report(r ? SETUP_BOOT_IO : SETUP_BOOT_MISSING, detail, detail_size, "Cannot read SD:/%s", SETUP_BOOT_INI);
    r = scan_entries(&ini, entries, capacity, count);
    free(ini.data);
    return report(r ? SETUP_BOOT_OK : SETUP_BOOT_INVALID, detail, detail_size,
                  r ? "%zu boot entries found" : "Malformed or oversized Hekate configuration", *count);
}

static int append(struct buffer *out, const void *data, size_t size)
{
    unsigned char *grown;
    if (!size) return 1;
    if (out->size > INI_LIMIT * 2 || size > INI_LIMIT * 2 - out->size) return 0;
    if (!(grown = realloc(out->data, out->size + size + 1))) return 0;
    out->data = grown;
    memcpy(out->data + out->size, data, size);
    out->size += size;
    out->data[out->size] = 0;
    return 1;
}

static int add_text(struct buffer *out, const char *text) { return append(out, text, strlen(text)); }

static int verified_payload(const char *root, const char *name, const struct setup_boot_payload *expected,
                            struct buffer *out)
{
    if (!expected->size || expected->size > PAYLOAD_LIMIT || !hash_present(expected->sha256)) return 0;
    if (read_file(root, name, PAYLOAD_LIMIT, out) != 1) return 0;
    if (out->size == expected->size && same_hash(out, expected->sha256)) return 1;
    free(out->data);
    memset(out, 0, sizeof(*out));
    return 0;
}

static int immutable_file(const char *root, const char *path, const struct buffer *data)
{
    struct buffer existing;
    int r;
    unsigned char hash[32];
    if (!safe_parents(root, path, 1)) return 0;
    r = read_file(root, path, PAYLOAD_LIMIT, &existing);
    if (!r)
    {
        if (!write_new(root, path, data->data, data->size)) return 0;
        r = read_file(root, path, PAYLOAD_LIMIT, &existing);
    }
    if (r < 0) return 0;
    digest(data->data, data->size, hash);
    r = existing.size == data->size && same_hash(&existing, hash) ? 1 : -1;
    free(existing.data);
    return r;
}

static int remove_file(const char *root, const char *relative)
{
    char path[1024];
    struct stat st;
    if (!safe_parents(root, relative, 0) || !path_join(path, root, relative)) return 0;
    if (lstat(path, &st)) return errno == ENOENT;
    return S_ISREG(st.st_mode) && !unlink(path) && sync_path(path);
}

static int remove_ini_backups(const char *root)
{
    int initial = remove_file(root, BACKUP);
    int update = remove_file(root, UPDATE_BACKUP);
    return initial && update;
}

static int move_file(const char *root, const char *source, const char *target)
{
    char from[1024], to[1024];
    struct stat st;
    if (!safe_parents(root, source, 0) || !safe_parents(root, target, 1) ||
        !path_join(from, root, source) || !path_join(to, root, target) ||
        lstat(from, &st) || !S_ISREG(st.st_mode)) return 0;
    if (!lstat(to, &st) || errno != ENOENT) return 0;
    return !rename(from, to) && sync_path(to);
}

static int fixed_state(const char *root, const char *path, const struct buffer *data)
{
    struct buffer current;
    unsigned char hash[32];
    int r = read_file(root, path, PAYLOAD_LIMIT, &current);
    if (r <= 0) return r ? -1 : 1;
    digest(data->data, data->size, hash);
    r = current.size == data->size && same_hash(&current, hash) ? 0 : 2;
    free(current.data);
    return r;
}

static int replace_fixed(const char *root, const char *path, const char *staged,
                         const char *previous, const struct buffer *data, int state)
{
    struct buffer check;
    unsigned char hash[32];
    int r;
    if (!state) return 1;
    if (state == 1) return immutable_file(root, path, data) == 1;
    if (!remove_file(root, staged) || !write_new(root, staged, data->data, data->size) ||
        !move_file(root, path, previous)) return 0;
    if (!move_file(root, staged, path))
    {
        move_file(root, previous, path);
        return 0;
    }
    digest(data->data, data->size, hash);
    r = read_file(root, path, PAYLOAD_LIMIT, &check);
    if (r != 1) return 0;
    r = check.size == data->size && same_hash(&check, hash);
    free(check.data);
    return r;
}

static int recover_missing_fixed(const char *root, const char *path, const char *previous)
{
    struct buffer file;
    int r = read_file(root, path, PAYLOAD_LIMIT, &file);
    if (r == 1) { free(file.data); return 1; }
    if (r < 0) return 0;
    r = read_file(root, previous, PAYLOAD_LIMIT, &file);
    if (r == 1) { free(file.data); return move_file(root, previous, path); }
    return r == 0;
}

static int finish_fixed(const char *root, const char *path, const char *staged,
                        const char *previous, int changed, int existed, int committed)
{
    struct buffer file;
    int r;
    if (!changed) return 1;
    if (committed)
    {
        r = read_file(root, path, PAYLOAD_LIMIT, &file);
        if (r != 1) return 0;
        free(file.data);
        return remove_file(root, staged);
    }
    if (existed)
    {
        r = read_file(root, previous, PAYLOAD_LIMIT, &file);
        if (r < 0) return 0;
        if (r == 1)
        {
            free(file.data);
            if (!remove_file(root, path) || !move_file(root, previous, path)) return 0;
        }
        else
        {
            r = read_file(root, path, PAYLOAD_LIMIT, &file);
            if (r != 1) return 0;
            free(file.data);
        }
    }
    else if (!remove_file(root, path)) return 0;
    return remove_file(root, staged);
}

static uint32_t get32(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int hoc_config(const struct buffer *kip, size_t *offset, uint32_t *revision, uint32_t *version)
{
    size_t i, found = 0;
    if (kip->size < 256 || memcmp(kip->data, "KIP1", 4) ||
        get32(kip->data + 16) != 1 || get32(kip->data + 20) != 0x01000000) return 0;
    for (i = 256; i + 12 <= kip->size; i++)
    {
        if (memcmp(kip->data + i, "CUST", 4)) continue;
        if (found++) return 0;
        *offset = i;
        *revision = get32(kip->data + i + 4);
        *version = get32(kip->data + i + 8);
    }
    return found == 1 && *revision && *revision < 256 && *version && *version < 10000;
}

static int installed_payload_matches(const char *root, const char *path,
                                     const struct setup_boot_payload *payload)
{
    struct buffer installed;
    int match = read_file(root, path, PAYLOAD_LIMIT, &installed) == 1;
    if (match)
    {
        match = installed.size == payload->size && same_hash(&installed, payload->sha256);
        free(installed.data);
    }
    return match;
}

static int installed_hoc_matches(const char *root, const struct setup_boot_manifest *manifest)
{
    struct buffer installed;
    size_t offset;
    uint32_t revision, version;
    int match = read_file(root, HOC_KIP, PAYLOAD_LIMIT, &installed) == 1;
    if (match)
    {
        match = installed.size == manifest->hoc.size &&
            hoc_config(&installed, &offset, &revision, &version) &&
            offset == manifest->hoc_config_offset &&
            revision == manifest->hoc_config_revision &&
            version == manifest->hoc_kip_version &&
            manifest->hoc_config_size >= 12 &&
            manifest->hoc_config_size <= installed.size - offset;
        if (match)
        {
            memset(installed.data + offset + 12, 0, manifest->hoc_config_size - 12);
            match = same_hash(&installed, manifest->hoc_code_sha256);
        }
        free(installed.data);
    }
    return match;
}

int setup_boot_needs_update(const char *root, const struct setup_boot_manifest *manifest,
                            enum setup_boot_loader *installed_loader)
{
    struct setup_boot_entry entries[SETUP_BOOT_MAX_ENTRIES];
    struct buffer ini;
    struct line line;
    size_t count, i, position, end;
    char kernel[512] = "", loader[512] = "";
    int managed = -1, result = 0;
    if (!manifest || manifest->version != 2 || !hash_present(manifest->stock.sha256) ||
        !hash_present(manifest->hoc_code_sha256) || !hash_present(manifest->mesosphere.sha256) ||
        read_file(root, SETUP_BOOT_INI, INI_LIMIT, &ini) != 1) return 0;
    if (!scan_entries(&ini, entries, SETUP_BOOT_MAX_ENTRIES, &count)) goto done;
    for (i = 0; i < count; i++)
    {
        if (strcasecmp(entries[i].name, SETUP_BOOT_ENTRY_NAME)) continue;
        if (managed >= 0 || !managed_entry(&ini, &entries[i])) goto done;
        managed = (int)i;
    }
    if (managed < 0) goto done;
    position = entries[managed].offset;
    end = position + entries[managed].length;
    while (position < end)
    {
        if (next_line(&ini, &position, &line) != 1) goto done;
        if (!strcmp(line.key, "kernel")) strcpy(kernel, line.value);
        else if (!strcmp(line.key, "kip1") &&
                 (!strcmp(line.value, STOCK_KIP) || managed_path(line.value, "/autorun.kip") ||
                  !strcmp(line.value, HOC_KIP)))
            strcpy(loader, line.value);
    }
    if (installed_loader) *installed_loader = !strcmp(loader, HOC_KIP) ? SETUP_BOOT_HOC : SETUP_BOOT_STOCK;
    if (strcmp(kernel, MESOSPHERE) || !*loader ||
        !installed_payload_matches(root, kernel, &manifest->mesosphere))
        result = 1;
    else if (!strcmp(loader, HOC_KIP))
        result = !installed_hoc_matches(root, manifest);
    else if (strcmp(loader, STOCK_KIP)) result = 1;
    else result = !installed_payload_matches(root, loader, &manifest->stock);
done:
    free(ini.data);
    return result;
}

static int recover_hoc(const char *root)
{
    struct buffer current = {0}, previous = {0};
    size_t offset;
    uint32_t revision, version;
    int r = read_file(root, HOC_KIP, PAYLOAD_LIMIT, &current);
    if (r == 1) { free(current.data); return 1; }
    if (r < 0) return 0;
    r = read_file(root, HOC_PREVIOUS, PAYLOAD_LIMIT, &previous);
    if (!r) return 1;
    if (r < 0) return 0;
    r = hoc_config(&previous, &offset, &revision, &version) &&
        move_file(root, HOC_PREVIOUS, HOC_KIP);
    free(previous.data);
    return r;
}

static int replace_hoc(const char *root, const struct setup_boot_manifest *manifest,
                       const struct buffer *loader)
{
    struct buffer current = {0}, check = {0};
    size_t offset;
    uint32_t revision, version;
    unsigned char hash[32];
    int r = read_file(root, HOC_KIP, PAYLOAD_LIMIT, &current), ok = 0;
    if (!r) return immutable_file(root, HOC_KIP, loader) == 1;
    if (r < 0) return 0;
    digest(loader->data, loader->size, hash);
    if (current.size == loader->size && same_hash(&current, hash)) { ok = 1; goto done; }
    if (!hoc_config(&current, &offset, &revision, &version) ||
        revision != manifest->hoc_config_revision ||
        version != manifest->hoc_kip_version) goto done;
    if (!remove_file(root, HOC_STAGED) || !remove_file(root, HOC_PREVIOUS) ||
        !write_new(root, HOC_STAGED, loader->data, loader->size)) goto done;
    if (!move_file(root, HOC_KIP, HOC_PREVIOUS))
    {
        recover_hoc(root);
        goto done;
    }
    if (!move_file(root, HOC_STAGED, HOC_KIP))
    {
        move_file(root, HOC_PREVIOUS, HOC_KIP);
        goto done;
    }
    r = read_file(root, HOC_KIP, PAYLOAD_LIMIT, &check);
    ok = r == 1 && check.size == loader->size && same_hash(&check, hash);
    if (!ok && remove_file(root, HOC_KIP)) move_file(root, HOC_PREVIOUS, HOC_KIP);
done:
    free(current.data);
    free(check.data);
    return ok;
}

static int hoc_target_compatible(const char *root, const struct setup_boot_manifest *manifest)
{
    struct buffer kip = {0};
    size_t offset;
    uint32_t revision, version;
    int r = read_file(root, HOC_KIP, PAYLOAD_LIMIT, &kip), compatible;
    if (!r) return 1;
    if (r < 0) return 0;
    compatible = hoc_config(&kip, &offset, &revision, &version) &&
                 revision == manifest->hoc_config_revision &&
                 version == manifest->hoc_kip_version;
    free(kip.data);
    return compatible;
}

static int probe_hoc_file(const char *root, const char *path, const struct setup_boot_manifest *manifest,
                          struct setup_boot_hoc_info *info)
{
    struct buffer kip;
    size_t offset;
    uint32_t revision, version;
    int compatible;
    if (!plain_path(path) || strlen(path) >= sizeof(info->path) ||
        read_file(root, path, PAYLOAD_LIMIT, &kip) != 1) return 0;
    if (!hoc_config(&kip, &offset, &revision, &version)) { free(kip.data); return 0; }
    compatible = manifest && revision == manifest->hoc_config_revision &&
                 manifest->hoc_config_size >= 12 &&
                 manifest->hoc_config_size <= kip.size - offset;
    if (!*info->path || compatible)
    {
        strcpy(info->path, path);
        info->revision = revision;
        info->kip_version = version;
        info->compatible = compatible;
    }
    free(kip.data);
    return compatible;
}

int setup_boot_hoc_probe(const char *root, const struct setup_boot_manifest *manifest,
                         struct setup_boot_hoc_info *info)
{
    struct buffer ini;
    struct line line;
    size_t position = 0;
    int r;
    memset(info, 0, sizeof(*info));
    if (probe_hoc_file(root, "atmosphere/kips/hoc.kip", manifest, info)) return 1;
    if (*info->path) return 1;
    if (read_file(root, SETUP_BOOT_INI, INI_LIMIT, &ini) != 1) return *info->path != 0;
    while ((r = next_line(&ini, &position, &line)) > 0)
    {
        const char *value;
        size_t length;
        if (strcmp(line.key, "kip1")) continue;
        value = *line.value == '/' ? line.value + 1 : line.value;
        length = strlen(value);
        if (length >= 2 && !strcmp(value + length - 2, "/*"))
        {
            char dir_path[1024], relative[512], checked[520];
            DIR *dir;
            struct dirent *child;
            if (length - 2 >= sizeof(relative)) continue;
            memcpy(relative, value, length - 2);
            relative[length - 2] = 0;
            if (!plain_path(relative) || !path_join(dir_path, root, relative) ||
                snprintf(checked, sizeof(checked), "%s/probe", relative) >= (int)sizeof(checked) ||
                !safe_parents(root, checked, 0) || !(dir = opendir(dir_path))) continue;
            while ((child = readdir(dir)))
            {
                const char *ext = strrchr(child->d_name, '.');
                char candidate[512];
                int n;
                if (!ext || (strcasecmp(ext, ".kip") && strcasecmp(ext, ".kip1"))) continue;
                n = snprintf(candidate, sizeof(candidate), "%s/%s", relative, child->d_name);
                if (n > 0 && n < (int)sizeof(candidate) &&
                    probe_hoc_file(root, candidate, manifest, info)) break;
            }
            closedir(dir);
            if (info->compatible) break;
        }
        else if (probe_hoc_file(root, value, manifest, info)) break;
    }
    free(ini.data);
    return *info->path != 0;
}

static int same_package(const char *root, const struct setup_boot_entry *entry,
                        const struct setup_boot_payload *expected)
{
    struct buffer ini = {0}, package = {0};
    struct line line;
    size_t position, end;
    char package_path[512] = "";
    int count = 0, result = 0;
    if (!entry || !expected || read_file(root, SETUP_BOOT_INI, INI_LIMIT, &ini) != 1) return 0;
    if (!same_hash(&ini, entry->ini_sha256) || entry->offset >= ini.size ||
        entry->length > ini.size - entry->offset)
        goto done;
    position = entry->offset;
    end = position + entry->length;
    if (next_line(&ini, &position, &line) != 1 || strcmp(line.key, "[") || strcmp(line.value, entry->name))
        goto done;
    while (position < end && next_line(&ini, &position, &line) == 1)
    {
        const char *value;
        if (strcmp(line.key, "pkg3") && strcmp(line.key, "fss0")) continue;
        value = *line.value == '/' ? line.value + 1 : line.value;
        if (++count > 1 || !plain_path(value) || strlen(value) >= sizeof(package_path))
            goto done;
        strcpy(package_path, value);
    }
    result = count == 1 && verified_payload(root, package_path, expected, &package);
done:
    free(ini.data);
    free(package.data);
    return result;
}

static int preserve_config(const char *root, const char *path, const struct setup_boot_manifest *manifest,
                            struct buffer *loader)
{
    struct buffer original;
    size_t offset = manifest->hoc_config_offset, source_offset, size = manifest->hoc_config_size;
    uint32_t revision, version;
    int ok = 0;
    if (offset < 256 || size < 12 || !plain_path(path) ||
        offset > loader->size || size > loader->size - offset ||
        read_file(root, path, PAYLOAD_LIMIT, &original) != 1) return 0;
    if (!hoc_config(&original, &source_offset, &revision, &version) ||
        revision != manifest->hoc_config_revision || size > original.size - source_offset ||
        memcmp(loader->data + offset, "CUST", 4) ||
        get32(loader->data + offset + 4) != revision) goto done;
    memcpy(loader->data + offset + 12, original.data + source_offset + 12, size - 12);
    ok = 1;
done:
    free(original.data);
    return ok;
}

static int resize_file(const char *root, const char *relative, size_t size)
{
    char path[1024];
    int fd, ok;
    if (!safe_parents(root, relative, 0) || !path_join(path, root, relative)) return 0;
    if ((fd = open(path, O_WRONLY)) < 0) return 0;
    ok = !ftruncate(fd, size) && !fsync(fd);
    if (close(fd)) ok = 0;
    return ok && sync_path(path);
}

static int flush_file(const char *root, const char *relative)
{
    char path[1024];
    int fd, ok;
    if (!safe_parents(root, relative, 0) || !path_join(path, root, relative)) return 0;
    if ((fd = open(path, O_WRONLY)) < 0) return 0;
    ok = !fsync(fd);
    if (close(fd)) ok = 0;
    return ok && sync_path(path);
}

static int append_commit(const char *root, const struct buffer *output, size_t original_size)
{
    char path[1024];
    struct stat st;
    size_t position = original_size;
    int fd, ok = 1;
    if (!safe_parents(root, SETUP_BOOT_INI, 0) || !path_join(path, root, SETUP_BOOT_INI)) return 0;
    if ((fd = open(path, O_WRONLY | O_APPEND)) < 0) return 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != original_size) ok = 0;
    while (ok && position < output->size)
    {
        ssize_t n = write(fd, output->data + position, output->size - position);
        if (n <= 0) { ok = 0; break; }
        position += n;
    }
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    return ok && sync_path(path);
}

static int replace_commit(const char *root)
{
    char target[1024], staged[1024];
    if (!path_join(target, root, SETUP_BOOT_INI) || !path_join(staged, root, STAGED) ||
        !safe_parents(root, SETUP_BOOT_INI, 0) || !safe_parents(root, STAGED, 0)) return 0;
    return !unlink(target) && !rename(staged, target) && sync_path(target);
}

enum setup_boot_result setup_boot_recover(const char *root, char *detail, size_t detail_size)
{
    struct buffer journal = {0}, before = {0}, current = {0}, after = {0}, committed = {0};
    struct transaction transaction;
    char target[1024], backup[1024], record[1024], staged[1024], marker[1024];
    int r, update;
    unsigned int flags;
    enum setup_boot_result result = SETUP_BOOT_RECOVERY;
    if (!recover_hoc(root))
        return report(SETUP_BOOT_RECOVERY, detail, detail_size, "HOC loader recovery needs attention");
    if (!path_join(target, root, SETUP_BOOT_INI) || !path_join(backup, root, BACKUP) ||
        !path_join(record, root, JOURNAL) || !path_join(staged, root, STAGED) ||
        !path_join(marker, root, COMMITTED)) return SETUP_BOOT_INVALID;
    r = read_file(root, JOURNAL, sizeof(transaction), &journal);
    if (!r)
        return report(remove_file(root, COMMITTED) &&
                      recover_missing_fixed(root, STOCK_KIP, STOCK_PREVIOUS) &&
                      recover_missing_fixed(root, MESOSPHERE, MESOSPHERE_PREVIOUS) ?
                      SETUP_BOOT_OK : SETUP_BOOT_RECOVERY, detail, detail_size,
                      "No boot transaction pending");
    if (r < 0 || journal.size != sizeof(transaction)) goto done;
    memcpy(&transaction, journal.data, sizeof(transaction));
    update = !memcmp(transaction.magic, "ARBOOT2", 7);
    flags = transaction.magic[7];
    if ((!update && memcmp(transaction.magic, "ARBOOT1", 7)) || (flags & ~15u) ||
        read_file(root, update ? UPDATE_BACKUP : BACKUP, INI_LIMIT, &before) != 1 ||
        !same_hash(&before, transaction.before)) goto done;
    r = read_file(root, COMMITTED, sizeof(transaction.after), &committed);
    if (r < 0) goto done;
    r = read_file(root, SETUP_BOOT_INI, INI_LIMIT * 2, &current);
    if (r == 1 && same_hash(&current, transaction.after) &&
        (memcmp(transaction.before, transaction.after, 32) ||
         (committed.size == sizeof(transaction.after) &&
          !memcmp(committed.data, transaction.after, sizeof(transaction.after)))))
        result = SETUP_BOOT_ALREADY;
    else if (r == 1 && same_hash(&current, transaction.before)) result = SETUP_BOOT_OK;
    else if (!update && r == 1 && current.size > before.size &&
        !memcmp(current.data, before.data, before.size) &&
        read_file(root, STAGED, INI_LIMIT * 2, &after) == 1 &&
        same_hash(&after, transaction.after) && current.size < after.size &&
        !memcmp(current.data, after.data, current.size) &&
        resize_file(root, SETUP_BOOT_INI, before.size)) result = SETUP_BOOT_OK;
    else if (!r && write_new(root, SETUP_BOOT_INI, before.data, before.size)) result = SETUP_BOOT_OK;
    else goto done;
    if (!finish_fixed(root, STOCK_KIP, STOCK_STAGED, STOCK_PREVIOUS,
                      flags & TX_STOCK, flags & TX_STOCK_EXISTED, result == SETUP_BOOT_ALREADY) ||
        !finish_fixed(root, MESOSPHERE, MESOSPHERE_STAGED, MESOSPHERE_PREVIOUS,
                      flags & TX_MESOSPHERE, flags & TX_MESOSPHERE_EXISTED, result == SETUP_BOOT_ALREADY))
    {
        result = SETUP_BOOT_RECOVERY;
        goto done;
    }
    if (!flush_file(root, SETUP_BOOT_INI) || !remove_file(root, COMMITTED) ||
        (unlink(staged) && errno != ENOENT) || unlink(record) || !sync_path(record)) result = SETUP_BOOT_RECOVERY;
    else if (!remove_ini_backups(root)) result = SETUP_BOOT_IO;
done:
    free(journal.data);
    free(before.data);
    free(current.data);
    free(after.data);
    free(committed.data);
    return report(result, detail, detail_size, result == SETUP_BOOT_RECOVERY ?
        "Boot recovery needs attention; temporary INI copy and transaction were retained" :
        result == SETUP_BOOT_IO ? "Cannot remove temporary Hekate INI copy" :
        result == SETUP_BOOT_ALREADY ? "Autorun boot entry committed" :
        "Original boot configuration restored");
}

enum setup_boot_result setup_boot_install(const char *root, const char *payload_root,
    const struct setup_boot_manifest *manifest, const struct setup_boot_options *options,
    char *detail, size_t detail_size)
{
    struct buffer ini = {0}, clone = {0}, patched = {0}, output = {0}, loader = {0}, kernel = {0};
    struct buffer package = {0}, check = {0};
    struct setup_boot_entry entries[SETUP_BOOT_MAX_ENTRIES];
    struct transaction transaction = {{'A','R','B','O','O','T','1',0},{0},{0}};
    struct line line;
    size_t count = 0, position, i, patch_at = 0, package_at = 0;
    const char *newline, *payload;
    char package_path[512] = "";
    int r, found = 0, existing = -1, kernel_state, stock_state = 0, has_kip = 0;
    size_t hoc_offset;
    uint32_t hoc_revision, hoc_version;
    enum setup_boot_result result = SETUP_BOOT_INVALID;
#define FAIL(code, ...) do { result = report(code, detail, detail_size, __VA_ARGS__); goto done; } while (0)
    if (!options || (options->loader != SETUP_BOOT_STOCK && options->loader != SETUP_BOOT_HOC)) return SETUP_BOOT_INVALID;
    if (!manifest) return report(SETUP_BOOT_MISSING, detail, detail_size,
        "Boot payloads pending: patched stock loader, HOC loader, Mesosphere and trusted release hashes");
    if (manifest->version != 2 || !memchr(manifest->release, 0, sizeof(manifest->release)) || !*manifest->release)
        return report(SETUP_BOOT_INVALID, detail, detail_size, "Invalid compiled boot manifest");
    for (i = 0; manifest->release[i]; i++)
        if (!isalnum((unsigned char)manifest->release[i]) && manifest->release[i] != '-' &&
            manifest->release[i] != '_' && manifest->release[i] != '.')
            return report(SETUP_BOOT_INVALID, detail, detail_size, "Invalid boot release identifier");
    if (options->preserve_hoc && options->loader != SETUP_BOOT_HOC) return SETUP_BOOT_INVALID;
    result = setup_boot_recover(root, detail, detail_size);
    if (result != SETUP_BOOT_OK) return result;
    if (read_file(root, SETUP_BOOT_INI, INI_LIMIT, &ini) != 1) FAIL(SETUP_BOOT_IO, "Cannot read Hekate configuration");
    if (!same_hash(&ini, options->entry.ini_sha256)) FAIL(SETUP_BOOT_CHANGED, "Hekate configuration changed; choose the entry again");
    if (!scan_entries(&ini, entries, SETUP_BOOT_MAX_ENTRIES, &count)) FAIL(SETUP_BOOT_INVALID, "Malformed Hekate configuration");
    for (i = 0; i < count; i++)
    {
        if (!strcasecmp(entries[i].name, SETUP_BOOT_ENTRY_NAME))
        {
            if (existing >= 0 || !managed_entry(&ini, &entries[i]))
                FAIL(SETUP_BOOT_UNSUPPORTED, "An unrecognized Autorun boot entry already exists");
            existing = (int)i;
        }
        if (entries[i].offset == options->entry.offset && entries[i].length == options->entry.length &&
            !strcmp(entries[i].name, options->entry.name))
        {
            if (!entries[i].supported) FAIL(SETUP_BOOT_UNSUPPORTED, "%s", entries[i].reason);
            found = 1;
        }
    }
    if (!found) FAIL(SETUP_BOOT_CHANGED, "Selected boot entry no longer exists");
    newline = strstr((char *)ini.data, "\r\n") ? "\r\n" : "\n";
    position = options->entry.offset;
    if (next_line(&ini, &position, &line) != 1) FAIL(SETUP_BOOT_INVALID, "Invalid boot entry header");
    while (position < options->entry.offset + options->entry.length)
    {
        size_t j;
        if (next_line(&ini, &position, &line) != 1) FAIL(SETUP_BOOT_INVALID, "Invalid boot entry line");
        if (!strcmp(line.key, "kernel") || !strcmp(line.key, "id")) continue;
        if (!strcmp(line.key, "kip1") && loader_kip(line.value))
        {
            patch_at = clone.size;
            has_kip = 1;
            continue;
        }
        if (!*line.key)
        {
            for (j = line.start; j < line.end && isspace(ini.data[j]); j++);
            if (j == line.end) continue;
        }
        if (!strcmp(line.key, "pkg3") || !strcmp(line.key, "fss0"))
        {
            const char *value = *line.value == '/' ? line.value + 1 : line.value;
            if (!plain_path(value)) FAIL(SETUP_BOOT_INVALID, "Unsafe Atmosphere package path");
            strcpy(package_path, value);
        }
        if (!append(&clone, ini.data + line.start, line.next - line.start)) FAIL(SETUP_BOOT_IO, "Boot entry too large");
        if (!strcmp(line.key, "kip1")) { patch_at = clone.size; has_kip = 1; }
        if (!strcmp(line.key, "pkg3") || !strcmp(line.key, "fss0")) package_at = clone.size;
    }
    if (!has_kip) patch_at = package_at;
    if ((patch_at && !append(&patched, clone.data, patch_at)) ||
        (patch_at && clone.data[patch_at - 1] != '\n' && !add_text(&patched, newline)) ||
        !add_text(&patched, "kip1=") ||
        !add_text(&patched, options->loader == SETUP_BOOT_HOC ? HOC_KIP : STOCK_KIP) ||
        !add_text(&patched, newline) || !add_text(&patched, "kernel=" MESOSPHERE) ||
        !add_text(&patched, newline) ||
        (patch_at < clone.size && !append(&patched, clone.data + patch_at, clone.size - patch_at)))
        FAIL(SETUP_BOOT_IO, "Boot entry too large");
    free(clone.data);
    clone = patched;
    memset(&patched, 0, sizeof(patched));
    if (read_file(root, package_path, PAYLOAD_LIMIT, &package) != 1 || !package.size)
        FAIL(SETUP_BOOT_MISSING, "Cannot read the selected Atmosphere package3");
    if (options->loader == SETUP_BOOT_HOC)
    {
        struct setup_boot_payload current;
        current.size = package.size;
        digest(package.data, package.size, current.sha256);
        for (i = 0; i < count; i++)
        {
            if ((int)i == existing || !entry_loads_hoc(&ini, &entries[i])) continue;
            if (!same_package(root, &entries[i], &current))
                FAIL(SETUP_BOOT_UNSUPPORTED, "Boot entry %s loads HOC with a different Atmosphere package", entries[i].name);
        }
    }
    payload = options->loader == SETUP_BOOT_HOC ? "loader-hoc.kip" : "loader-stock.kip";
    if (!verified_payload(payload_root, payload, options->loader == SETUP_BOOT_HOC ? &manifest->hoc : &manifest->stock, &loader) ||
        !verified_payload(payload_root, "mesosphere.bin", &manifest->mesosphere, &kernel))
        FAIL(SETUP_BOOT_MISSING, "Missing or invalid %s / mesosphere.bin; nothing installed", payload);
    if (loader.size < 256 || memcmp(loader.data, "KIP1", 4) || get32(loader.data + 16) != 1 || get32(loader.data + 20) != 0x01000000)
        FAIL(SETUP_BOOT_INVALID, "Selected payload is not a loader KIP");
    if (options->loader == SETUP_BOOT_HOC &&
        (!hoc_config(&loader, &hoc_offset, &hoc_revision, &hoc_version) ||
         hoc_offset != manifest->hoc_config_offset || hoc_revision != manifest->hoc_config_revision ||
         hoc_version != manifest->hoc_kip_version ||
         manifest->hoc_config_size < 12 ||
         manifest->hoc_config_size > loader.size - hoc_offset))
        FAIL(SETUP_BOOT_INVALID, "Bundled HOC configuration layout does not match its manifest");
    if (options->loader == SETUP_BOOT_HOC && !hoc_target_compatible(root, manifest))
        FAIL(SETUP_BOOT_UNSUPPORTED, "Installed HOC loader does not match bundled HOC %u.%u.%u",
             manifest->hoc_kip_version / 100, manifest->hoc_kip_version / 10 % 10,
             manifest->hoc_kip_version % 10);
    if (options->preserve_hoc &&
        !preserve_config(root, options->hoc_source, manifest, &loader))
        FAIL(SETUP_BOOT_UNSUPPORTED, "Existing HOC settings are absent or use an incompatible CUST revision");
    kernel_state = fixed_state(root, MESOSPHERE, &kernel);
    if (options->loader == SETUP_BOOT_STOCK)
        stock_state = fixed_state(root, STOCK_KIP, &loader);
    if (kernel_state < 0 || stock_state < 0)
        FAIL(SETUP_BOOT_UNSUPPORTED, "An existing Atmosphere patch file cannot be read");
    if (!append(&output, ini.data, existing < 0 ? ini.size : entries[existing].offset) ||
        (output.size && output.data[output.size - 1] != '\n' && !add_text(&output, newline)) ||
        (existing < 0 && !add_text(&output, newline)) ||
        !add_text(&output, "[" SETUP_BOOT_ENTRY_NAME "]") || !add_text(&output, newline) ||
        !append(&output, clone.data, clone.size) ||
        (output.size && output.data[output.size - 1] != '\n' && !add_text(&output, newline)) ||
        (existing >= 0 && !append(&output, ini.data + entries[existing].offset + entries[existing].length,
                                  ini.size - entries[existing].offset - entries[existing].length)))
        FAIL(SETUP_BOOT_IO, "Cannot create cloned boot entry");
    if (output.size > INI_LIMIT) FAIL(SETUP_BOOT_UNSUPPORTED, "Hekate configuration is too large for a safe update");
    if (existing >= 0) memcpy(transaction.magic, "ARBOOT2", 8);
    if (!remove_ini_backups(root)) FAIL(SETUP_BOOT_IO, "Cannot prepare temporary boot recovery copy");
    {
        const char *paths[] = {existing < 0 ? BACKUP : UPDATE_BACKUP, STAGED};
        const struct buffer *buffers[] = {&ini, &output};
        if ((kernel_state && (!remove_file(root, MESOSPHERE_STAGED) ||
                              !remove_file(root, MESOSPHERE_PREVIOUS))) ||
            (stock_state && (!remove_file(root, STOCK_STAGED) ||
                             !remove_file(root, STOCK_PREVIOUS))))
            FAIL(SETUP_BOOT_IO, "Cannot prepare Atmosphere patch files");
        for (i = 0; i < sizeof(paths) / sizeof(*paths); i++)
        {
            r = immutable_file(root, paths[i], buffers[i]);
            if (r != 1) FAIL(SETUP_BOOT_IO, r < 0 ?
                "Conflicting or interrupted setup file: %s. Inspect it manually before retrying; original Hekate INI unchanged" :
                "Cannot stage %s; original Hekate INI unchanged", paths[i]);
        }
    }
    if (read_file(root, SETUP_BOOT_INI, INI_LIMIT, &check) != 1 || !same_hash(&check, options->entry.ini_sha256))
        FAIL(SETUP_BOOT_CHANGED, "Hekate configuration changed during staging; original retained");
    memcpy(transaction.before, options->entry.ini_sha256, 32);
    digest(output.data, output.size, transaction.after);
    if (kernel_state) transaction.magic[7] |= TX_MESOSPHERE;
    if (kernel_state == 2) transaction.magic[7] |= TX_MESOSPHERE_EXISTED;
    if (stock_state) transaction.magic[7] |= TX_STOCK;
    if (stock_state == 2) transaction.magic[7] |= TX_STOCK_EXISTED;
    if (!write_new(root, JOURNAL, &transaction, sizeof(transaction))) FAIL(SETUP_BOOT_IO, "Cannot save boot recovery transaction");
    if (!replace_fixed(root, MESOSPHERE, MESOSPHERE_STAGED, MESOSPHERE_PREVIOUS, &kernel, kernel_state) ||
        (options->loader == SETUP_BOOT_STOCK &&
         !replace_fixed(root, STOCK_KIP, STOCK_STAGED, STOCK_PREVIOUS, &loader, stock_state)))
    {
        r = setup_boot_recover(root, detail, detail_size);
        FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO, "Cannot install Atmosphere patch files");
    }
    if (options->loader == SETUP_BOOT_HOC && !replace_hoc(root, manifest, &loader))
    {
        r = setup_boot_recover(root, detail, detail_size);
        FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO, "Cannot replace the HOC loader");
    }
    if (!(existing < 0 ? append_commit(root, &output, ini.size) :
          !memcmp(transaction.before, transaction.after, 32) || replace_commit(root)))
    {
        r = setup_boot_recover(root, detail, detail_size);
        if (r == SETUP_BOOT_ALREADY) { result = SETUP_BOOT_OK; goto done; }
        FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO,
             "Boot publish interrupted; recovery restored the original when possible");
    }
    if (!memcmp(transaction.before, transaction.after, 32) &&
        !write_new(root, COMMITTED, transaction.after, sizeof(transaction.after)))
    {
        r = setup_boot_recover(root, detail, detail_size);
        FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO, "Cannot complete boot update");
    }
    r = setup_boot_recover(root, detail, detail_size);
    if (r == SETUP_BOOT_ALREADY) result = SETUP_BOOT_OK;
    else FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO, "Boot publish interrupted; recovery restored the original when possible");
done:
    free(ini.data); free(clone.data); free(patched.data); free(output.data); free(loader.data);
    free(kernel.data); free(package.data); free(check.data);
    return result;
#undef FAIL
}

const char *setup_boot_error(enum setup_boot_result result)
{
    switch (result)
    {
    case SETUP_BOOT_OK: return "Boot setup complete";
    case SETUP_BOOT_ALREADY: return "Autorun boot entry already exists";
    case SETUP_BOOT_MISSING: return "Required boot payloads are not available";
    case SETUP_BOOT_INVALID: return "Invalid boot setup data";
    case SETUP_BOOT_UNSUPPORTED: return "This boot configuration is not supported";
    case SETUP_BOOT_CHANGED: return "Boot configuration changed; select it again";
    case SETUP_BOOT_HASH: return "Boot payload compatibility or hash check failed";
    case SETUP_BOOT_IO: return "Could not write boot setup files";
    case SETUP_BOOT_RECOVERY: return "Boot setup recovery needs attention";
    }
    return "Unknown boot setup error";
}
