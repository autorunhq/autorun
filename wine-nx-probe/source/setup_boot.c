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
#ifdef __SWITCH__
#include <switch.h>
#else
#include <tomcrypt.h>
#endif

#define INI_LIMIT (256u * 1024u)
#define PAYLOAD_LIMIT (32u * 1024u * 1024u)
#define BACKUP "bootloader/hekate_ipl.ini.autorun-backup"
#define STAGED "bootloader/hekate_ipl.ini.autorun-new"
#define JOURNAL "bootloader/hekate_ipl.ini.autorun-transaction"

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
    return NULL;
}

static void digest(const void *data, size_t size, unsigned char out[32])
{
#ifdef __SWITCH__
    sha256CalculateHash(out, data, size);
#else
    hash_state state;
    sha256_init(&state);
    sha256_process(&state, data, size);
    sha256_done(&state, out);
#endif
}

static int hash_present(const unsigned char hash[32])
{
    unsigned int i;
    for (i = 0; i < 32; i++) if (hash[i]) return 1;
    return 0;
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

static int kip_kind(const char *root, const char *relative)
{
    char path[1024];
    unsigned char header[32];
    FILE *file;
    struct stat st;
    size_t n;
    uint64_t title = 0;
    unsigned int i;
    if (!safe_parents(root, relative, 0) || !path_join(path, root, relative) ||
        lstat(path, &st) || st.st_size < 256 || !(file = fopen(path, "rb"))) return -1;
    n = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (n != sizeof(header) || memcmp(header, "KIP1", 4)) return -1;
    for (i = 0; i < 8; i++) title |= (uint64_t)header[16 + i] << (8 * i);
    return title == UINT64_C(0x0100000000000001) ? 1 : 0;
}

static int add_kip(const char *root, const char *path, const char *newline,
                   struct buffer *clone, char loader_path[512], int *loaders)
{
    const char *relative = *path == '/' ? path + 1 : path;
    int kind = kip_kind(root, relative);
    if (kind < 0) return 0;
    if (kind)
    {
        if (++*loaders > 1 || strlen(relative) >= 512) return 0;
        strcpy(loader_path, relative);
        return 1;
    }
    return add_text(clone, "kip1=") && add_text(clone, path) && add_text(clone, newline);
}

static int clone_kip(const char *root, const struct line *line, const struct buffer *ini,
                     const char *newline, struct buffer *clone, char loader_path[512], int *loaders)
{
    char relative[512], directory[1024], child[512];
    const char *value = line->value;
    size_t length = strlen(value);
    DIR *dir;
    struct dirent *entry;
    int ok = 1, kind;
    if (length < 2 || strcmp(value + length - 2, "/*"))
    {
        const char *path = *value == '/' ? value + 1 : value;
        kind = kip_kind(root, path);
        if (kind < 0) return 0;
        if (!kind) return append(clone, ini->data + line->start, line->next - line->start);
        return add_kip(root, value, newline, clone, loader_path, loaders);
    }
    if (*value == '/') { value++; length--; }
    if (length + 5 >= sizeof(relative)) return 0;
    memcpy(relative, value, length - 1);
    strcpy(relative + length - 1, "probe");
    if (!safe_parents(root, relative, 0)) return 0;
    relative[length - 2] = 0;
    if (!path_join(directory, root, relative) || !(dir = opendir(directory))) return 0;
    errno = 0;
    while ((entry = readdir(dir)))
    {
        const char *ext;
        int n;
        for (ext = entry->d_name; *ext && strncasecmp(ext, ".kip", 4); ext++);
        if (!*ext) { errno = 0; continue; }
        n = snprintf(child, sizeof(child), "%s/%s", relative, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child) || !add_kip(root, child, newline, clone, loader_path, loaders)) { ok = 0; break; }
        errno = 0;
    }
    if (!entry && errno && errno != ENOENT) ok = 0;
    closedir(dir);
    return ok;
}

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

static uint32_t get32(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int preserve_config(const char *root, const char *path, const struct setup_boot_manifest *manifest,
                            struct buffer *loader)
{
    struct buffer original;
    size_t offset = manifest->hoc_config_offset, size = manifest->hoc_config_size;
    int ok = 0;
    unsigned char hash[32];
    if (offset < 256 || size < 8 || !manifest->hoc_original.size ||
        !hash_present(manifest->hoc_original_normalized_sha256) || !*path ||
        offset > loader->size || size > loader->size - offset ||
        read_file(root, path, PAYLOAD_LIMIT, &original) != 1) return 0;
    if (original.size != manifest->hoc_original.size || offset > original.size || size > original.size - offset) goto done;
    if (memcmp(original.data + offset, "CUST", 4) || memcmp(loader->data + offset, "CUST", 4) ||
        get32(original.data + offset + 4) != manifest->hoc_config_revision ||
        get32(loader->data + offset + 4) != manifest->hoc_config_revision) goto done;
    {
        unsigned char *config = malloc(size);
        if (!config) goto done;
        memcpy(config, original.data + offset, size);
        memset(original.data + offset, 0, size);
        digest(original.data, original.size, hash);
        if (!memcmp(hash, manifest->hoc_original_normalized_sha256, 32))
        {
            memcpy(loader->data + offset, config, size);
            ok = 1;
        }
        free(config);
    }
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

enum setup_boot_result setup_boot_recover(const char *root, char *detail, size_t detail_size)
{
    struct buffer journal = {0}, before = {0}, current = {0}, after = {0};
    struct transaction transaction;
    char target[1024], backup[1024], record[1024], staged[1024];
    int r;
    enum setup_boot_result result = SETUP_BOOT_RECOVERY;
    if (!path_join(target, root, SETUP_BOOT_INI) || !path_join(backup, root, BACKUP) ||
        !path_join(record, root, JOURNAL) || !path_join(staged, root, STAGED)) return SETUP_BOOT_INVALID;
    r = read_file(root, JOURNAL, sizeof(transaction), &journal);
    if (!r) return report(SETUP_BOOT_OK, detail, detail_size, "No boot transaction pending");
    if (r < 0 || journal.size != sizeof(transaction)) goto done;
    memcpy(&transaction, journal.data, sizeof(transaction));
    if (memcmp(transaction.magic, "ARBOOT1", 8) || read_file(root, BACKUP, INI_LIMIT, &before) != 1 ||
        !same_hash(&before, transaction.before)) goto done;
    r = read_file(root, SETUP_BOOT_INI, INI_LIMIT * 2, &current);
    if (r == 1 && same_hash(&current, transaction.after)) result = SETUP_BOOT_ALREADY;
    else if (r == 1 && same_hash(&current, transaction.before)) result = SETUP_BOOT_OK;
    else if (r == 1 && current.size > before.size &&
        !memcmp(current.data, before.data, before.size) &&
        read_file(root, STAGED, INI_LIMIT * 2, &after) == 1 &&
        same_hash(&after, transaction.after) && current.size < after.size &&
        !memcmp(current.data, after.data, current.size) &&
        resize_file(root, SETUP_BOOT_INI, before.size)) result = SETUP_BOOT_OK;
    else if (!r && write_new(root, SETUP_BOOT_INI, before.data, before.size)) result = SETUP_BOOT_OK;
    else goto done;
    if (!flush_file(root, SETUP_BOOT_INI) ||
        (unlink(staged) && errno != ENOENT) || unlink(record) || !sync_path(record)) result = SETUP_BOOT_RECOVERY;
done:
    free(journal.data);
    free(before.data);
    free(current.data);
    free(after.data);
    return report(result, detail, detail_size, result == SETUP_BOOT_RECOVERY ?
        "Boot recovery needs attention; backup and transaction were retained" :
        result == SETUP_BOOT_ALREADY ? "Autorun boot entry committed; original INI backup retained" :
        "Original boot configuration restored; backup retained");
}

enum setup_boot_result setup_boot_install(const char *root, const char *payload_root,
    const struct setup_boot_manifest *manifest, const struct setup_boot_options *options,
    char *detail, size_t detail_size)
{
    struct buffer ini = {0}, clone = {0}, output = {0}, loader = {0}, kernel = {0}, package = {0}, check = {0};
    struct setup_boot_entry entries[SETUP_BOOT_MAX_ENTRIES];
    struct transaction transaction = {{'A','R','B','O','O','T','1',0},{0},{0}};
    struct line line;
    size_t count = 0, position, i;
    const char *newline, *payload;
    char package_path[512] = "", loader_path[512] = "";
    char loader_dest[256], kernel_dest[256], suffix[32];
    unsigned char hash[32];
    int r, loaders = 0, found = 0;
    enum setup_boot_result result = SETUP_BOOT_INVALID;
#define FAIL(code, ...) do { result = report(code, detail, detail_size, __VA_ARGS__); goto done; } while (0)
    if (!options || (options->loader != SETUP_BOOT_STOCK && options->loader != SETUP_BOOT_HOC)) return SETUP_BOOT_INVALID;
    if (!manifest) return report(SETUP_BOOT_MISSING, detail, detail_size,
        "Boot payloads pending: patched stock loader, HOC loader, Mesosphere and trusted release hashes");
    if (manifest->version != 1 || !memchr(manifest->release, 0, sizeof(manifest->release)) || !*manifest->release)
        return report(SETUP_BOOT_INVALID, detail, detail_size, "Invalid compiled boot manifest");
    for (i = 0; manifest->release[i]; i++)
        if (!isalnum((unsigned char)manifest->release[i]) && manifest->release[i] != '-' && manifest->release[i] != '_')
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
            FAIL(SETUP_BOOT_UNSUPPORTED, "An entry named Autorun already exists; it was left unchanged");
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
        if (next_line(&ini, &position, &line) != 1) FAIL(SETUP_BOOT_INVALID, "Invalid boot entry line");
        if (!strcmp(line.key, "kernel") || !strcmp(line.key, "id")) continue;
        if (!strcmp(line.key, "pkg3") || !strcmp(line.key, "fss0"))
        {
            const char *value = *line.value == '/' ? line.value + 1 : line.value;
            if (!plain_path(value)) FAIL(SETUP_BOOT_INVALID, "Unsafe Atmosphere package path");
            strcpy(package_path, value);
        }
        if (!strcmp(line.key, "kip1"))
        {
            if (!clone_kip(root, &line, &ini, newline, &clone, loader_path, &loaders))
                FAIL(SETUP_BOOT_UNSUPPORTED, "Cannot safely identify existing loader KIPs (including wildcard entries)");
        }
        else if (!append(&clone, ini.data + line.start, line.next - line.start)) FAIL(SETUP_BOOT_IO, "Boot entry too large");
    }
    if (!verified_payload(root, package_path, &manifest->package3, &package))
        FAIL(SETUP_BOOT_HASH, "Atmosphere package does not match the compatible compiled release hash");
    payload = options->loader == SETUP_BOOT_HOC ? "loader-hoc.kip" : "loader-stock.kip";
    if (!verified_payload(payload_root, payload, options->loader == SETUP_BOOT_HOC ? &manifest->hoc : &manifest->stock, &loader) ||
        !verified_payload(payload_root, "mesosphere.bin", &manifest->mesosphere, &kernel))
        FAIL(SETUP_BOOT_MISSING, "Missing or invalid %s / mesosphere.bin; nothing installed", payload);
    if (loader.size < 256 || memcmp(loader.data, "KIP1", 4) || get32(loader.data + 16) != 1 || get32(loader.data + 20) != 0x01000000)
        FAIL(SETUP_BOOT_INVALID, "Selected payload is not a loader KIP");
    if (options->preserve_hoc && !preserve_config(root, loader_path, manifest, &loader))
        FAIL(SETUP_BOOT_UNSUPPORTED, "HOC OC preservation needs a matching installed loader and trusted CUST layout/hash");
    digest(loader.data, loader.size, hash);
    for (i = 0; i < 8; i++) snprintf(suffix + i * 2, 3, "%02x", hash[i]);
    snprintf(loader_dest, sizeof(loader_dest), "bootloader/autorun/%s-%s/autorun.kip", manifest->release, suffix);
    snprintf(kernel_dest, sizeof(kernel_dest), "bootloader/autorun/%s-%s/mesosphere.bin", manifest->release, suffix);
    if (!append(&output, ini.data, ini.size) ||
        (output.size && output.data[output.size - 1] != '\n' && !add_text(&output, newline)) ||
        !add_text(&output, newline) || !add_text(&output, "[" SETUP_BOOT_ENTRY_NAME "]") || !add_text(&output, newline) ||
        !append(&output, clone.data, clone.size) ||
        (output.size && output.data[output.size - 1] != '\n' && !add_text(&output, newline)) ||
        !add_text(&output, "kernel=") || !add_text(&output, kernel_dest) || !add_text(&output, newline) ||
        !add_text(&output, "kip1=") || !add_text(&output, loader_dest) || !add_text(&output, newline))
        FAIL(SETUP_BOOT_IO, "Cannot create cloned boot entry");
    /* All payload, compatibility and OC checks above precede the first write. */
    {
        const char *paths[] = {loader_dest, kernel_dest, BACKUP, STAGED};
        const struct buffer *buffers[] = {&loader, &kernel, &ini, &output};
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
    if (!write_new(root, JOURNAL, &transaction, sizeof(transaction))) FAIL(SETUP_BOOT_IO, "Cannot save boot recovery transaction");
    if (!append_commit(root, &output, ini.size))
    {
        r = setup_boot_recover(root, detail, detail_size);
        if (r == SETUP_BOOT_ALREADY) { result = SETUP_BOOT_OK; goto done; }
        FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO,
             "Boot publish interrupted; recovery restored the original when possible");
    }
    r = setup_boot_recover(root, detail, detail_size);
    if (r == SETUP_BOOT_ALREADY) result = SETUP_BOOT_OK;
    else FAIL(r == SETUP_BOOT_RECOVERY ? r : SETUP_BOOT_IO, "Boot publish interrupted; recovery restored the original when possible");
done:
    free(ini.data); free(clone.data); free(output.data); free(loader.data);
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
