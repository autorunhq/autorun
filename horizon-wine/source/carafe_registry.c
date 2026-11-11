#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <switch.h>

#include "carafe_hid.h"
#include "carafe_overlay.h"
#include "carafe_registry.h"
#include "carafe_settings.h"

#define REGISTRY_FILE "/carafe/registry.txt"
#define REGISTRY_LIMIT (1024 * 1024)
#define REGISTRY_HEADER "carafe-registry\t1"
#define PENDING_HEADER "carafe-registry-pending\t1"
#define REMOVED_HEADER "carafe-registry-removed\t1"
#define CARAFE_DIR "sdmc:/switch/wine/carafe"
#define PENDING_LIMIT (2 * 1024 * 1024)
#define MAX_PENDING 256
#define MAX_REMOVED 256
#define MAX_ROWS 64
#define MAX_FIELDS 8
#define MAX_OPTIONS 32
#define MAX_LINE_FIELDS 16
#define PATH_SIZE 768

typedef enum {
    OpSet,
    OpDeleteValue,
    OpDeleteKey,
    OpRestore,
    OpRestoreKey,
} Op;

typedef struct {
    bool key;
    int hive;
    char path[CARAFE_REG_PATH];
    char name[CARAFE_REG_NAME];
} Removed;

typedef struct {
    Op op;
    int hive;
    char path[CARAFE_REG_PATH];
    char name[CARAFE_REG_NAME];
    uint32_t type;
    uint8_t data[CARAFE_REG_DATA];
    uint32_t size;
} Change;

typedef struct {
    int hive;
    const char* path;
    const char* name;
    uint32_t type;
} Field;

typedef struct {
    const char* label;
    const char* data[MAX_FIELDS];
} Option;

typedef struct {
    const char* name;
    const char* hint;
    Field fields[MAX_FIELDS];
    int field_count;
    Option options[MAX_OPTIONS];
    int option_count;
} Row;

extern int wine_nx_registry_lock(void) __attribute__((weak));
extern void wine_nx_registry_unlock(void) __attribute__((weak));
extern unsigned int wine_nx_registry_set(int hive, const unsigned short* path, unsigned int path_len,
    const unsigned short* name, unsigned int name_len, unsigned int type, const void* data, unsigned int size)
    __attribute__((weak));
extern unsigned int wine_nx_registry_delete_value(int hive, const unsigned short* path, unsigned int path_len,
    const unsigned short* name, unsigned int name_len) __attribute__((weak));
extern unsigned int wine_nx_registry_delete_key(int hive, const unsigned short* path, unsigned int path_len)
    __attribute__((weak));
extern int wine_nx_registry_key_info(int hive, const unsigned short* path, unsigned int path_len,
    unsigned int* subkeys, unsigned int* values) __attribute__((weak));
extern int wine_nx_registry_subkey(int hive, const unsigned short* path, unsigned int path_len, unsigned int index,
    const unsigned short** name, unsigned int* name_len) __attribute__((weak));
extern int wine_nx_registry_value(int hive, const unsigned short* path, unsigned int path_len, unsigned int index,
    const unsigned short** name, unsigned int* name_len, unsigned int* type, const unsigned char** data,
    unsigned int* size) __attribute__((weak));
extern int wine_nx_registry_get(int hive, const unsigned short* path, unsigned int path_len,
    const unsigned short* name, unsigned int name_len, unsigned int* type, const unsigned char** data,
    unsigned int* size) __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Change g_pending[MAX_PENDING];
static int g_pending_count;
static bool g_pending_loaded;
static char* g_rows_text;
static Row g_rows[MAX_ROWS];
static int g_row_count;
static Removed g_removed[MAX_REMOVED];
static int g_removed_count;

static void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    carafeOverlayLog(line);
}

static bool available(void) {
    return wine_nx_registry_lock != NULL && wine_nx_registry_set != NULL;
}

const char* carafeRegistryHiveName(int hive) {
    return hive == CARAFE_REG_HIVE_MACHINE ? "HKEY_LOCAL_MACHINE" : "HKEY_CURRENT_USER";
}

static const char* hiveShort(int hive) {
    return hive == CARAFE_REG_HIVE_MACHINE ? "HKLM" : "HKCU";
}

static int hiveFromName(const char* name) {
    if (strcasecmp(name, "HKCU") == 0 || strcasecmp(name, "HKEY_CURRENT_USER") == 0) {
        return CARAFE_REG_HIVE_USER;
    }
    if (strcasecmp(name, "HKLM") == 0 || strcasecmp(name, "HKEY_LOCAL_MACHINE") == 0) {
        return CARAFE_REG_HIVE_MACHINE;
    }
    return -1;
}

size_t carafeRegistryUtf16(const char* text, uint16_t* out, size_t max) {
    const unsigned char* s = (const unsigned char*)text;
    size_t count = 0;
    while (*s != '\0' && count < max) {
        uint32_t code;
        if (*s < 0x80) {
            code = *s++;
        } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            code = (uint32_t)(*s & 0x1F) << 6 | (s[1] & 0x3F);
            s += 2;
        } else if ((*s & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            code = (uint32_t)(*s & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
            s += 3;
        } else if ((*s & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            code = (uint32_t)(*s & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6
                | (s[3] & 0x3F);
            s += 4;
        } else {
            code = 0xFFFD;
            s++;
        }
        if (code >= 0x10000) {
            if (count + 2 > max) {
                break;
            }
            code -= 0x10000;
            out[count++] = (uint16_t)(0xD800 | code >> 10);
            out[count++] = (uint16_t)(0xDC00 | (code & 0x3FF));
        } else {
            out[count++] = (uint16_t)code;
        }
    }
    return count;
}

static void utf8From16(const uint16_t* text, size_t count, char* out, size_t size) {
    size_t used = 0;
    for (size_t i = 0; i < count && used + 1 < size; i++) {
        uint32_t code = text[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < count && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            i++;
        }
        unsigned char bytes[4];
        size_t length;
        if (code < 0x80) {
            bytes[0] = (unsigned char)code;
            length = 1;
        } else if (code < 0x800) {
            bytes[0] = (unsigned char)(0xC0 | code >> 6);
            bytes[1] = (unsigned char)(0x80 | (code & 0x3F));
            length = 2;
        } else if (code < 0x10000) {
            bytes[0] = (unsigned char)(0xE0 | code >> 12);
            bytes[1] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            bytes[2] = (unsigned char)(0x80 | (code & 0x3F));
            length = 3;
        } else {
            bytes[0] = (unsigned char)(0xF0 | code >> 18);
            bytes[1] = (unsigned char)(0x80 | (code >> 12 & 0x3F));
            bytes[2] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            bytes[3] = (unsigned char)(0x80 | (code & 0x3F));
            length = 4;
        }
        if (used + length + 1 > size) {
            break;
        }
        memcpy(out + used, bytes, length);
        used += length;
    }
    out[used] = '\0';
}

static int splitFields(char* line, char** fields, int max) {
    int count = 0;
    char* cursor = line;
    while (count < max) {
        fields[count++] = cursor;
        char* tab = strchr(cursor, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        cursor = tab + 1;
    }
    return count;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hexDecode(const char* text, uint8_t* out, uint32_t max, uint32_t* size) {
    size_t length = strlen(text);
    if (length % 2 != 0 || length / 2 > max) {
        return false;
    }
    for (size_t i = 0; i < length; i += 2) {
        int high = hexValue(text[i]);
        int low = hexValue(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(high << 4 | low);
    }
    *size = (uint32_t)(length / 2);
    return true;
}

static void hexEncode(const uint8_t* data, uint32_t size, FILE* file) {
    for (uint32_t i = 0; i < size; i++) {
        fprintf(file, "%02x", data[i]);
    }
}

static void trimPath(const char* path, char* out, size_t size) {
    while (*path == '\\') {
        path++;
    }
    snprintf(out, size, "%s", path);
    size_t length = strlen(out);
    while (length > 0 && out[length - 1] == '\\') {
        out[--length] = '\0';
    }
}

static bool samePath(const char* a, const char* b) {
    char left[CARAFE_REG_PATH];
    char right[CARAFE_REG_PATH];
    trimPath(a, left, sizeof(left));
    trimPath(b, right, sizeof(right));
    return strcasecmp(left, right) == 0;
}

static bool programFilePath(const char* name, char* out, size_t size) {
    char dir[PATH_SIZE];
    if (carafeOverlayProgramDir != NULL && carafeOverlayProgramDir(dir, sizeof(dir))) {
        int n = snprintf(out, size, "%s/%s", dir, name);
        return n > 0 && (size_t)n < size;
    }
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/wine", 0777);
    mkdir(CARAFE_DIR, 0777);
    int n = snprintf(out, size, "%s/%s", CARAFE_DIR, name);
    return n > 0 && (size_t)n < size;
}

static void volatilePath(char* out, size_t size) {
    programFilePath("registry-rollback.txt", out, size);
}

static char* readText(const char* path, size_t limit) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    char* text = malloc(limit + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1, limit, file);
    fclose(file);
    text[got] = '\0';
    return text;
}

static char* romfsText(void) {
    size_t size = 0;
    char* data = carafeOverlayReadRomfsFile(REGISTRY_FILE, REGISTRY_LIMIT, &size);
    if (data == NULL) {
        return NULL;
    }
    char* text = realloc(data, size + 1);
    if (text == NULL) {
        free(data);
        return NULL;
    }
    text[size] = '\0';
    return text;
}

static uint64_t textHash(const char* text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool liveKey(int hive, const char* path) {
    uint16_t wide_path[CARAFE_REG_PATH];
    size_t path_len = carafeRegistryUtf16(path, wide_path, CARAFE_REG_PATH);
    unsigned int subkeys = 0;
    unsigned int values = 0;
    return wine_nx_registry_key_info != NULL
        && wine_nx_registry_key_info(hive, wide_path, (unsigned int)path_len * 2, &subkeys, &values) == 0;
}

static unsigned int applyChange(Op op, int hive, const char* path, const char* name, uint32_t type,
    const uint8_t* data, uint32_t size) {
    static const uint8_t empty[2] = { 0, 0 };
    uint16_t wide_path[CARAFE_REG_PATH];
    uint16_t wide_name[CARAFE_REG_NAME];
    size_t path_len = carafeRegistryUtf16(path, wide_path, CARAFE_REG_PATH);
    size_t name_len = carafeRegistryUtf16(name, wide_name, CARAFE_REG_NAME);
    switch (op) {
    case OpSet:
    case OpRestore:
        return wine_nx_registry_set(hive, wide_path, (unsigned int)path_len * 2, wide_name,
            (unsigned int)name_len * 2, type, data, size);
    case OpDeleteValue:
        return wine_nx_registry_delete_value(hive, wide_path, (unsigned int)path_len * 2, wide_name,
            (unsigned int)name_len * 2);
    case OpDeleteKey:
        return wine_nx_registry_delete_key(hive, wide_path, (unsigned int)path_len * 2);
    case OpRestoreKey: {
        if (liveKey(hive, path)) {
            return 0;
        }
        unsigned int status = wine_nx_registry_set(hive, wide_path, (unsigned int)path_len * 2, wide_name, 0,
            CARAFE_REG_SZ, empty, sizeof(empty));
        if (status != 0) {
            return status;
        }
        return wine_nx_registry_delete_value(hive, wide_path, (unsigned int)path_len * 2, wide_name, 0);
    }
    }
    return 0;
}

static bool liveValue(int hive, const char* path, const char* name, uint32_t* type, uint8_t* data, uint32_t max,
    uint32_t* size, bool* truncated) {
    uint16_t wide_path[CARAFE_REG_PATH];
    uint16_t wide_name[CARAFE_REG_NAME];
    size_t path_len = carafeRegistryUtf16(path, wide_path, CARAFE_REG_PATH);
    size_t name_len = carafeRegistryUtf16(name, wide_name, CARAFE_REG_NAME);
    unsigned int live_type = 0;
    unsigned int live_size = 0;
    const unsigned char* live_data = NULL;
    if (wine_nx_registry_get == NULL
        || wine_nx_registry_get(hive, wide_path, (unsigned int)path_len * 2, wide_name, (unsigned int)name_len * 2,
               &live_type, &live_data, &live_size)
            != 0) {
        return false;
    }
    *type = live_type;
    *size = live_size < max ? live_size : max;
    if (*size > 0) {
        memcpy(data, live_data, *size);
    }
    if (truncated != NULL) {
        *truncated = live_size > max;
    }
    return true;
}

static bool parseOp(char** fields, int count, Op* op, int* hive, const char** path, const char** name) {
    if (count < 3) {
        return false;
    }
    if (strcmp(fields[0], "set") == 0 && count >= 6) {
        *op = OpSet;
    } else if (strcmp(fields[0], "delete") == 0 && count >= 4) {
        *op = OpDeleteValue;
    } else if (strcmp(fields[0], "deletekey") == 0) {
        *op = OpDeleteKey;
    } else if (strcmp(fields[0], "restore") == 0 && count >= 6) {
        *op = OpRestore;
    } else if (strcmp(fields[0], "restorekey") == 0) {
        *op = OpRestoreKey;
    } else {
        return false;
    }
    *hive = hiveFromName(fields[1]);
    *path = fields[2];
    *name = *op == OpDeleteKey || *op == OpRestoreKey ? "" : fields[3];
    return *hive >= 0;
}

static int applyEntries(char* text) {
    int applied = 0;
    char* line = text;
    bool first = true;
    while (line != NULL && *line != '\0') {
        char* end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            line[length - 1] = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, REGISTRY_HEADER) != 0) {
                note("[CARAFE] registry: %s has an unknown header", REGISTRY_FILE);
                return 0;
            }
        } else {
            char* fields[MAX_LINE_FIELDS];
            int count = splitFields(line, fields, MAX_LINE_FIELDS);
            Op op;
            int hive;
            const char* path;
            const char* name;
            if (parseOp(fields, count, &op, &hive, &path, &name)) {
                uint8_t* data = NULL;
                uint32_t size = 0;
                uint32_t type = 0;
                bool ok = true;
                if (op == OpSet || op == OpRestore) {
                    type = (uint32_t)strtoul(fields[4], NULL, 10);
                    size_t max = strlen(fields[5]) / 2 + 1;
                    data = malloc(max);
                    ok = data != NULL && hexDecode(fields[5], data, (uint32_t)max, &size);
                }
                if (ok) {
                    unsigned int status = applyChange(op, hive, path, name, type, data, size);
                    if (status == 0) {
                        applied++;
                    } else if (op != OpDeleteValue && op != OpDeleteKey) {
                        note("[CARAFE] registry: %s\\%s › %s not written (0x%08x)", hiveShort(hive), path, name,
                            status);
                    }
                }
                free(data);
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return applied;
}

static bool packageMode(const char* text, bool* always) {
    const char* mode = strstr(text, "\nmode\t");
    if (mode == NULL) {
        *always = false;
        return false;
    }
    *always = strncmp(mode + 6, "always", 6) == 0;
    return true;
}

static void loadPendingLocked(void) {
    if (g_pending_loaded) {
        return;
    }
    g_pending_loaded = true;
    g_pending_count = 0;
    char pending_path[PATH_SIZE];
    if (!programFilePath("registry-pending.txt", pending_path, sizeof(pending_path))) {
        return;
    }
    char* text = readText(pending_path, PENDING_LIMIT);
    if (text == NULL) {
        return;
    }
    char* line = text;
    bool first = true;
    while (line != NULL && *line != '\0' && g_pending_count < MAX_PENDING) {
        char* end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            line[length - 1] = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, PENDING_HEADER) != 0) {
                break;
            }
        } else {
            char* fields[MAX_LINE_FIELDS];
            int count = splitFields(line, fields, MAX_LINE_FIELDS);
            Op op;
            int hive;
            const char* path;
            const char* name;
            if (parseOp(fields, count, &op, &hive, &path, &name) && op != OpRestore && op != OpRestoreKey) {
                Change* change = &g_pending[g_pending_count];
                memset(change, 0, sizeof(*change));
                change->op = op;
                change->hive = hive;
                trimPath(path, change->path, sizeof(change->path));
                snprintf(change->name, sizeof(change->name), "%s", name);
                bool ok = true;
                if (op == OpSet) {
                    change->type = (uint32_t)strtoul(fields[4], NULL, 10);
                    ok = hexDecode(fields[5], change->data, CARAFE_REG_DATA, &change->size);
                }
                if (ok) {
                    g_pending_count++;
                }
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    free(text);
}

static void writeChangeLine(FILE* file, Op op, int hive, const char* path, const char* name, uint32_t type,
    const uint8_t* data, uint32_t size) {
    if (op == OpSet) {
        fprintf(file, "set\t%s\t%s\t%s\t%u\t", hiveShort(hive), path, name, type);
        hexEncode(data, size, file);
        fputc('\n', file);
    } else if (op == OpDeleteValue) {
        fprintf(file, "delete\t%s\t%s\t%s\n", hiveShort(hive), path, name);
    } else {
        fprintf(file, "deletekey\t%s\t%s\n", hiveShort(hive), path);
    }
}

static bool savePendingLocked(void) {
    char pending_path[PATH_SIZE];
    if (!programFilePath("registry-pending.txt", pending_path, sizeof(pending_path))) {
        return false;
    }
    if (g_pending_count == 0) {
        remove(pending_path);
        return true;
    }
    char temp[PATH_SIZE + 8];
    snprintf(temp, sizeof(temp), "%s.new", pending_path);
    FILE* file = fopen(temp, "wb");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%s\n", PENDING_HEADER);
    for (int i = 0; i < g_pending_count; i++) {
        const Change* c = &g_pending[i];
        writeChangeLine(file, c->op, c->hive, c->path, c->name, c->type, c->data, c->size);
    }
    bool ok = fclose(file) == 0;
    if (!ok) {
        remove(temp);
        return false;
    }
    remove(pending_path);
    return rename(temp, pending_path) == 0;
}

static void writeRollback(void) {
    char path[PATH_SIZE];
    volatilePath(path, sizeof(path));
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return;
    }
    fprintf(file, "%s\n", PENDING_HEADER);
    static uint8_t data[CARAFE_REG_DATA];
    for (int i = 0; i < g_pending_count; i++) {
        const Change* c = &g_pending[i];
        if (c->op == OpDeleteKey) {
            continue;
        }
        uint32_t type = 0;
        uint32_t size = 0;
        bool truncated = false;
        if (liveValue(c->hive, c->path, c->name, &type, data, CARAFE_REG_DATA, &size, &truncated)) {
            if (!truncated) {
                writeChangeLine(file, OpSet, c->hive, c->path, c->name, type, data, size);
            }
        } else {
            writeChangeLine(file, OpDeleteValue, c->hive, c->path, c->name, 0, NULL, 0);
        }
    }
    fclose(file);
}

static bool underKey(const char* key, const char* path) {
    char left[CARAFE_REG_PATH];
    char right[CARAFE_REG_PATH];
    trimPath(key, left, sizeof(left));
    trimPath(path, right, sizeof(right));
    size_t length = strlen(left);
    return strncasecmp(left, right, length) == 0 && (right[length] == '\0' || right[length] == '\\');
}

static void loadRemoved(void) {
    g_removed_count = 0;
    char removed_path[PATH_SIZE];
    if (!programFilePath("registry-removed.txt", removed_path, sizeof(removed_path))) {
        return;
    }
    char* text = readText(removed_path, PENDING_LIMIT);
    if (text == NULL) {
        return;
    }
    char* line = text;
    bool first = true;
    while (line != NULL && *line != '\0' && g_removed_count < MAX_REMOVED) {
        char* end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, REMOVED_HEADER) != 0) {
                break;
            }
        } else {
            char* fields[MAX_LINE_FIELDS];
            int count = splitFields(line, fields, MAX_LINE_FIELDS);
            Op op;
            int hive;
            const char* path;
            const char* name;
            if (parseOp(fields, count, &op, &hive, &path, &name) && (op == OpDeleteValue || op == OpDeleteKey)) {
                Removed* removed = &g_removed[g_removed_count++];
                removed->key = op == OpDeleteKey;
                removed->hive = hive;
                trimPath(path, removed->path, sizeof(removed->path));
                snprintf(removed->name, sizeof(removed->name), "%s", name);
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    free(text);
}

static void saveRemoved(void) {
    char removed_path[PATH_SIZE];
    if (!programFilePath("registry-removed.txt", removed_path, sizeof(removed_path))) {
        return;
    }
    if (g_removed_count == 0) {
        remove(removed_path);
        return;
    }
    char temp[PATH_SIZE + 8];
    snprintf(temp, sizeof(temp), "%s.new", removed_path);
    FILE* file = fopen(temp, "wb");
    if (file == NULL) {
        return;
    }
    fprintf(file, "%s\n", REMOVED_HEADER);
    for (int i = 0; i < g_removed_count; i++) {
        const Removed* r = &g_removed[i];
        writeChangeLine(file, r->key ? OpDeleteKey : OpDeleteValue, r->hive, r->path, r->name, 0, NULL, 0);
    }
    if (fclose(file) != 0) {
        remove(temp);
        return;
    }
    remove(removed_path);
    rename(temp, removed_path);
}

static bool removedCovers(int hive, const char* path, const char* name, bool key) {
    for (int i = 0; i < g_removed_count; i++) {
        const Removed* r = &g_removed[i];
        if (r->hive != hive) {
            continue;
        }
        if (r->key ? underKey(r->path, path) : !key && samePath(r->path, path) && strcasecmp(r->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool noteRemoval(const Change* change) {
    bool changed = false;
    if (change->op == OpSet) {
        for (int i = g_removed_count - 1; i >= 0; i--) {
            const Removed* r = &g_removed[i];
            bool same_value = !r->key && samePath(r->path, change->path) && strcasecmp(r->name, change->name) == 0;
            if (r->hive == change->hive && (same_value || (r->key && underKey(r->path, change->path)))) {
                memmove(&g_removed[i], &g_removed[i + 1], (size_t)(g_removed_count - i - 1) * sizeof(Removed));
                g_removed_count--;
                changed = true;
            }
        }
        return changed;
    }
    bool key = change->op == OpDeleteKey;
    if (removedCovers(change->hive, change->path, change->name, key) || g_removed_count >= MAX_REMOVED) {
        return false;
    }
    Removed* removed = &g_removed[g_removed_count++];
    removed->key = key;
    removed->hive = change->hive;
    trimPath(change->path, removed->path, sizeof(removed->path));
    snprintf(removed->name, sizeof(removed->name), "%s", key ? "" : change->name);
    return true;
}

static void restoreMissing(char* text) {
    static uint8_t live[CARAFE_REG_DATA];
    static uint8_t data[CARAFE_REG_DATA];
    int restored = 0;
    char* line = text;
    bool first = true;
    while (line != NULL && *line != '\0') {
        char* end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            line[length - 1] = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, REGISTRY_HEADER) != 0) {
                return;
            }
        } else {
            char* fields[MAX_LINE_FIELDS];
            int count = splitFields(line, fields, MAX_LINE_FIELDS);
            Op op;
            int hive;
            const char* path;
            const char* name;
            if (parseOp(fields, count, &op, &hive, &path, &name) && (op == OpRestore || op == OpRestoreKey)
                && !removedCovers(hive, path, name, op == OpRestoreKey)) {
                uint32_t type = 0;
                uint32_t size = 0;
                bool missing = op == OpRestoreKey
                    ? !liveKey(hive, path)
                    : !liveValue(hive, path, name, &type, live, CARAFE_REG_DATA, &size, NULL);
                bool ok = true;
                if (missing && op == OpRestore) {
                    type = (uint32_t)strtoul(fields[4], NULL, 10);
                    ok = hexDecode(fields[5], data, CARAFE_REG_DATA, &size);
                }
                if (missing && ok && applyChange(op, hive, path, name, type, data, size) == 0) {
                    restored++;
                    note("[CARAFE] registry: %s\\%s › %s was missing, written again", hiveShort(hive), path,
                        op == OpRestoreKey ? "(key)" : name[0] != '\0' ? name : "@");
                }
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    if (restored > 0) {
        note("[CARAFE] registry: %d missing values of the installer written again", restored);
    }
}

void carafeRegistryLoaded(void) {
    carafeHidRegister();
    if (!available()) {
        return;
    }
    char* text = romfsText();
    char* restore_text = text != NULL ? strdup(text) : NULL;
    if (text != NULL) {
        bool always = false;
        packageMode(text, &always);
        char hash[32];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)textHash(text));
        char mark[32] = "";
        char mark_path[PATH_SIZE];
        FILE* file = programFilePath("registry-applied", mark_path, sizeof(mark_path)) ? fopen(mark_path, "rb") : NULL;
        if (file != NULL) {
            size_t got = fread(mark, 1, sizeof(mark) - 1, file);
            mark[got] = '\0';
            fclose(file);
        }
        if (always || strcmp(mark, hash) != 0) {
            int applied = applyEntries(text);
            file = mark_path[0] != '\0' ? fopen(mark_path, "wb") : NULL;
            if (file != NULL) {
                fputs(hash, file);
                fclose(file);
            }
            note("[CARAFE] registry: %d entries of the package laid over the registry (%s)", applied,
                always ? "every start" : "once");
        }
        free(text);
    }
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    loadRemoved();
    if (g_pending_count > 0) {
        if (carafeTrialActive()) {
            writeRollback();
        }
        int applied = 0;
        bool removed_changed = false;
        for (int i = 0; i < g_pending_count; i++) {
            const Change* c = &g_pending[i];
            applied += applyChange(c->op, c->hive, c->path, c->name, c->type, c->data, c->size) == 0;
            removed_changed |= noteRemoval(c);
        }
        note("[CARAFE] registry: %d of %d changes made in the settings menu written", applied, g_pending_count);
        g_pending_count = 0;
        savePendingLocked();
        if (removed_changed) {
            saveRemoved();
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (restore_text != NULL) {
        restoreMissing(restore_text);
        free(restore_text);
    }
}

static Row* parseRows(char* text) {
    g_row_count = 0;
    Row* row = NULL;
    char* line = text;
    bool first = true;
    while (line != NULL && *line != '\0') {
        char* end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            line[length - 1] = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, REGISTRY_HEADER) != 0) {
                return NULL;
            }
        } else {
            char* fields[MAX_LINE_FIELDS];
            int count = splitFields(line, fields, MAX_LINE_FIELDS);
            if (strcmp(fields[0], "row") == 0 && count >= 2 && g_row_count < MAX_ROWS) {
                row = &g_rows[g_row_count++];
                memset(row, 0, sizeof(*row));
                row->name = fields[1];
                row->hint = count >= 3 ? fields[2] : "";
            } else if (strcmp(fields[0], "field") == 0 && count >= 5 && row != NULL
                && row->field_count < MAX_FIELDS) {
                int hive = hiveFromName(fields[1]);
                if (hive >= 0) {
                    row->fields[row->field_count++]
                        = (Field){ hive, fields[2], fields[3], (uint32_t)strtoul(fields[4], NULL, 10) };
                }
            } else if (strcmp(fields[0], "option") == 0 && count >= 2 && row != NULL
                && row->option_count < MAX_OPTIONS) {
                Option* option = &row->options[row->option_count++];
                memset(option, 0, sizeof(*option));
                option->label = fields[1];
                for (int i = 0; i < MAX_FIELDS && i + 2 < count; i++) {
                    option->data[i] = fields[i + 2];
                }
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return g_rows;
}

bool carafeRegistryOpen(void) {
    carafeRegistryClose();
    g_rows_text = romfsText();
    if (g_rows_text != NULL && parseRows(g_rows_text) == NULL) {
        g_row_count = 0;
    }
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    pthread_mutex_unlock(&g_lock);
    return available();
}

void carafeRegistryClose(void) {
    free(g_rows_text);
    g_rows_text = NULL;
    g_row_count = 0;
}

static Change* findPendingLocked(int hive, const char* path, const char* name, bool key) {
    for (int i = 0; i < g_pending_count; i++) {
        Change* c = &g_pending[i];
        if (c->hive == hive && (c->op == OpDeleteKey) == key && samePath(c->path, path)
            && (key || strcasecmp(c->name, name) == 0)) {
            return c;
        }
    }
    return NULL;
}

static void removePendingLocked(Change* change) {
    int index = (int)(change - g_pending);
    memmove(&g_pending[index], &g_pending[index + 1], (size_t)(g_pending_count - index - 1) * sizeof(Change));
    g_pending_count--;
}

static bool currentValue(int hive, const char* path, const char* name, uint32_t* type, uint8_t* data,
    uint32_t* size) {
    bool found = false;
    if (available() && wine_nx_registry_lock() == 0) {
        found = liveValue(hive, path, name, type, data, CARAFE_REG_DATA, size, NULL);
        wine_nx_registry_unlock();
    }
    pthread_mutex_lock(&g_lock);
    const Change* change = findPendingLocked(hive, path, name, false);
    if (change != NULL) {
        if (change->op == OpSet) {
            *type = change->type;
            *size = change->size;
            memcpy(data, change->data, change->size);
            found = true;
        } else {
            found = false;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int carafeRegistryRowCount(void) {
    return g_row_count;
}

const char* carafeRegistryRowName(int row) {
    return row >= 0 && row < g_row_count ? g_rows[row].name : "";
}

const char* carafeRegistryRowHint(int row) {
    return row >= 0 && row < g_row_count ? g_rows[row].hint : "";
}

int carafeRegistryRowOptionCount(int row) {
    return row >= 0 && row < g_row_count ? g_rows[row].option_count : 0;
}

const char* carafeRegistryRowOptionLabel(int row, int option) {
    if (row < 0 || row >= g_row_count || option < 0 || option >= g_rows[row].option_count) {
        return "";
    }
    return g_rows[row].options[option].label;
}

int carafeRegistryRowCurrent(int row) {
    if (row < 0 || row >= g_row_count) {
        return -1;
    }
    const Row* r = &g_rows[row];
    static uint8_t data[CARAFE_REG_DATA];
    static uint8_t want[CARAFE_REG_DATA];
    uint32_t types[MAX_FIELDS];
    uint32_t sizes[MAX_FIELDS];
    bool found[MAX_FIELDS];
    static uint8_t values[MAX_FIELDS][CARAFE_REG_DATA];
    for (int f = 0; f < r->field_count; f++) {
        found[f] = currentValue(r->fields[f].hive, r->fields[f].path, r->fields[f].name, &types[f], data, &sizes[f]);
        if (found[f]) {
            memcpy(values[f], data, sizes[f]);
        }
    }
    for (int o = 0; o < r->option_count; o++) {
        bool match = true;
        for (int f = 0; f < r->field_count && match; f++) {
            uint32_t size = 0;
            const char* hex = r->options[o].data[f];
            if (hex == NULL || !hexDecode(hex, want, CARAFE_REG_DATA, &size)) {
                match = false;
            } else {
                match = found[f] && types[f] == r->fields[f].type && sizes[f] == size
                    && memcmp(values[f], want, size) == 0;
            }
        }
        if (match) {
            return o;
        }
    }
    return -1;
}

bool carafeRegistryRowChanged(int row) {
    if (row < 0 || row >= g_row_count) {
        return false;
    }
    bool changed = false;
    pthread_mutex_lock(&g_lock);
    for (int f = 0; f < g_rows[row].field_count && !changed; f++) {
        const Field* field = &g_rows[row].fields[f];
        changed = findPendingLocked(field->hive, field->path, field->name, false) != NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return changed;
}

void carafeRegistryRowSet(int row, int option) {
    if (row < 0 || row >= g_row_count || option < 0 || option >= g_rows[row].option_count) {
        return;
    }
    const Row* r = &g_rows[row];
    static uint8_t data[CARAFE_REG_DATA];
    for (int f = 0; f < r->field_count; f++) {
        uint32_t size = 0;
        const char* hex = r->options[option].data[f];
        if (hex != NULL && hexDecode(hex, data, CARAFE_REG_DATA, &size)) {
            carafeRegistrySetPending(r->fields[f].hive, r->fields[f].path, r->fields[f].name, r->fields[f].type,
                data, size);
        }
    }
}

void carafeRegistryRowReset(int row) {
    if (row < 0 || row >= g_row_count) {
        return;
    }
    for (int f = 0; f < g_rows[row].field_count; f++) {
        const Field* field = &g_rows[row].fields[f];
        carafeRegistryDropPending(field->hive, field->path, field->name);
    }
}

void carafeRegistryRowWhere(int row, char* out, size_t size) {
    out[0] = '\0';
    if (row < 0 || row >= g_row_count || g_rows[row].field_count == 0) {
        return;
    }
    const Row* r = &g_rows[row];
    size_t used = (size_t)snprintf(out, size, "%s\\%s ›", hiveShort(r->fields[0].hive), r->fields[0].path);
    for (int f = 0; f < r->field_count && used < size; f++) {
        used += (size_t)snprintf(out + used, size - used, "%s %s", f ? "," : "",
            r->fields[f].name[0] != '\0' ? r->fields[f].name : "@");
    }
}

static int appendKeys(int hive, const uint16_t* path, size_t path_len, unsigned int subkeys, CarafeRegEntry* out,
    int count, int max) {
    for (unsigned int i = 0; i < subkeys && count < max; i++) {
        const unsigned short* name = NULL;
        unsigned int name_len = 0;
        if (wine_nx_registry_subkey(hive, path, (unsigned int)path_len * 2, i, &name, &name_len) != 0) {
            continue;
        }
        CarafeRegEntry* entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        entry->key = true;
        utf8From16(name, name_len / 2, entry->name, sizeof(entry->name));
    }
    return count;
}

static int appendValues(int hive, const uint16_t* path, size_t path_len, unsigned int values, CarafeRegEntry* out,
    int count, int max) {
    for (unsigned int i = 0; i < values && count < max; i++) {
        const unsigned short* name = NULL;
        unsigned int name_len = 0;
        unsigned int type = 0;
        unsigned int size = 0;
        const unsigned char* data = NULL;
        if (wine_nx_registry_value(hive, path, (unsigned int)path_len * 2, i, &name, &name_len, &type, &data, &size)
            != 0) {
            continue;
        }
        CarafeRegEntry* entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        utf8From16(name, name_len / 2, entry->name, sizeof(entry->name));
        entry->type = entry->live_type = type;
        entry->size = entry->live_size = size < CARAFE_REG_DATA ? size : CARAFE_REG_DATA;
        entry->truncated = size > CARAFE_REG_DATA;
        if (entry->size > 0) {
            memcpy(entry->data, data, entry->size);
            memcpy(entry->live_data, data, entry->size);
        }
    }
    return count;
}

int carafeRegistryKeyExists(int hive, const char* path) {
    if (!available() || wine_nx_registry_key_info == NULL || wine_nx_registry_lock() != 0) {
        return 0;
    }
    uint16_t wide[CARAFE_REG_PATH];
    size_t length = carafeRegistryUtf16(path, wide, CARAFE_REG_PATH);
    unsigned int subkeys = 0;
    unsigned int values = 0;
    int exists = wine_nx_registry_key_info(hive, wide, (unsigned int)length * 2, &subkeys, &values) == 0;
    wine_nx_registry_unlock();
    return exists;
}

int carafeRegistryList(int hive, const char* path, CarafeRegEntry* out, int max) {
    if (!available() || wine_nx_registry_key_info == NULL || wine_nx_registry_lock() != 0) {
        return -1;
    }
    uint16_t wide[CARAFE_REG_PATH];
    size_t length = carafeRegistryUtf16(path, wide, CARAFE_REG_PATH);
    unsigned int subkeys = 0;
    unsigned int values = 0;
    if (wine_nx_registry_key_info(hive, wide, (unsigned int)length * 2, &subkeys, &values) != 0) {
        wine_nx_registry_unlock();
        return -1;
    }
    int count = appendKeys(hive, wide, length, subkeys, out, 0, max);
    int first_value = count;
    count = appendValues(hive, wide, length, values, out, count, max);
    wine_nx_registry_unlock();

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_pending_count; i++) {
        const Change* c = &g_pending[i];
        if (c->hive != hive) {
            continue;
        }
        if (c->op == OpDeleteKey) {
            const char* slash = strrchr(c->path, '\\');
            char parent[CARAFE_REG_PATH];
            snprintf(parent, sizeof(parent), "%.*s", slash != NULL ? (int)(slash - c->path) : 0, c->path);
            const char* leaf = slash != NULL ? slash + 1 : c->path;
            if (samePath(parent, path)) {
                for (int e = 0; e < first_value; e++) {
                    if (strcasecmp(out[e].name, leaf) == 0) {
                        out[e].state = CarafeRegDeleted;
                    }
                }
            }
            continue;
        }
        if (!samePath(c->path, path)) {
            continue;
        }
        int found = -1;
        for (int e = first_value; e < count; e++) {
            if (strcasecmp(out[e].name, c->name) == 0) {
                found = e;
                break;
            }
        }
        if (c->op == OpDeleteValue) {
            if (found >= 0) {
                out[found].state = CarafeRegDeleted;
            }
            continue;
        }
        if (found < 0) {
            if (count >= max) {
                continue;
            }
            found = count++;
            memset(&out[found], 0, sizeof(out[found]));
            snprintf(out[found].name, sizeof(out[found].name), "%s", c->name);
            out[found].state = CarafeRegAdded;
        } else {
            out[found].state = CarafeRegChanged;
        }
        out[found].type = c->type;
        out[found].size = c->size;
        out[found].truncated = false;
        memcpy(out[found].data, c->data, c->size);
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

bool carafeRegistrySetPending(int hive, const char* path, const char* name, uint32_t type, const uint8_t* data,
    uint32_t size) {
    if (size > CARAFE_REG_DATA) {
        return false;
    }
    static uint8_t live[CARAFE_REG_DATA];
    uint32_t live_type = 0;
    uint32_t live_size = 0;
    bool truncated = false;
    bool exists = false;
    if (available() && wine_nx_registry_lock() == 0) {
        exists = liveValue(hive, path, name, &live_type, live, CARAFE_REG_DATA, &live_size, &truncated);
        wine_nx_registry_unlock();
    }
    bool same = exists && !truncated && live_type == type && live_size == size && memcmp(live, data, size) == 0;
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    Change* change = findPendingLocked(hive, path, name, false);
    bool ok = true;
    if (same) {
        if (change != NULL) {
            removePendingLocked(change);
        }
    } else {
        if (change == NULL) {
            if (g_pending_count >= MAX_PENDING) {
                pthread_mutex_unlock(&g_lock);
                return false;
            }
            change = &g_pending[g_pending_count++];
        }
        memset(change, 0, sizeof(*change));
        change->op = OpSet;
        change->hive = hive;
        trimPath(path, change->path, sizeof(change->path));
        snprintf(change->name, sizeof(change->name), "%s", name);
        change->type = type;
        change->size = size;
        memcpy(change->data, data, size);
    }
    ok = savePendingLocked();
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool carafeRegistryDeletePending(int hive, const char* path, const char* name) {
    static uint8_t live[CARAFE_REG_DATA];
    uint32_t type = 0;
    uint32_t size = 0;
    bool exists = false;
    if (available() && wine_nx_registry_lock() == 0) {
        exists = liveValue(hive, path, name, &type, live, CARAFE_REG_DATA, &size, NULL);
        wine_nx_registry_unlock();
    }
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    Change* change = findPendingLocked(hive, path, name, false);
    if (!exists) {
        if (change != NULL) {
            removePendingLocked(change);
        }
    } else {
        if (change == NULL) {
            if (g_pending_count >= MAX_PENDING) {
                pthread_mutex_unlock(&g_lock);
                return false;
            }
            change = &g_pending[g_pending_count++];
        }
        memset(change, 0, sizeof(*change));
        change->op = OpDeleteValue;
        change->hive = hive;
        trimPath(path, change->path, sizeof(change->path));
        snprintf(change->name, sizeof(change->name), "%s", name);
    }
    bool ok = savePendingLocked();
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool carafeRegistryDropPending(int hive, const char* path, const char* name) {
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    Change* change = findPendingLocked(hive, path, name, false);
    bool ok = true;
    if (change != NULL) {
        removePendingLocked(change);
        ok = savePendingLocked();
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int carafeRegistryPendingCount(void) {
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    int count = g_pending_count;
    pthread_mutex_unlock(&g_lock);
    return count;
}

bool carafeRegistryPendingLine(int index, char* name, size_t name_size, char* from, size_t from_size, char* to,
    size_t to_size) {
    pthread_mutex_lock(&g_lock);
    if (index < 0 || index >= g_pending_count) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    Change change = g_pending[index];
    pthread_mutex_unlock(&g_lock);
    if (change.op == OpDeleteKey) {
        const char* slash = strrchr(change.path, '\\');
        snprintf(name, name_size, "%s", slash != NULL ? slash + 1 : change.path);
    } else {
        snprintf(name, name_size, "%s", change.name[0] != '\0' ? change.name : "@");
    }
    static uint8_t live[CARAFE_REG_DATA];
    uint32_t type = 0;
    uint32_t size = 0;
    bool exists = false;
    if (change.op != OpDeleteKey && available() && wine_nx_registry_lock() == 0) {
        exists = liveValue(change.hive, change.path, change.name, &type, live, CARAFE_REG_DATA, &size, NULL);
        wine_nx_registry_unlock();
    }
    if (exists) {
        carafeRegistryFormat(type, live, size, from, from_size);
    } else {
        snprintf(from, from_size, "—");
    }
    if (change.op == OpSet) {
        carafeRegistryFormat(change.type, change.data, change.size, to, to_size);
    } else {
        to[0] = '\0';
    }
    return true;
}

void carafeRegistryUndoPending(void) {
    pthread_mutex_lock(&g_lock);
    loadPendingLocked();
    g_pending_count = 0;
    savePendingLocked();
    pthread_mutex_unlock(&g_lock);
}

void carafeRegistryReset(void) {
    char applied_path[PATH_SIZE];
    char removed_path[PATH_SIZE];
    carafeRegistryUndoPending();
    if (programFilePath("registry-applied", applied_path, sizeof(applied_path))) {
        remove(applied_path);
    }
    if (programFilePath("registry-removed.txt", removed_path, sizeof(removed_path))) {
        remove(removed_path);
    }
    note("[CARAFE] registry: the package's entries are laid over the registry again at the next start");
}

void carafeRegistryBringBack(void) {
    char path[PATH_SIZE];
    char pending_path[PATH_SIZE];
    volatilePath(path, sizeof(path));
    char* text = readText(path, PENDING_LIMIT);
    if (text == NULL) {
        return;
    }
    if (programFilePath("registry-pending.txt", pending_path, sizeof(pending_path))) {
        FILE* file = fopen(pending_path, "wb");
        if (file != NULL) {
            fputs(text, file);
            fclose(file);
            note("[CARAFE] registry: the values from before the trial run come back at the next start");
        }
    }
    free(text);
    remove(path);
    pthread_mutex_lock(&g_lock);
    g_pending_loaded = false;
    pthread_mutex_unlock(&g_lock);
}

void carafeRegistrySyncProgram(void) {
    pthread_mutex_lock(&g_lock);
    g_pending_loaded = false;
    g_pending_count = 0;
    g_removed_count = 0;
    pthread_mutex_unlock(&g_lock);
}

void carafeRegistryTrialClear(void) {
    char path[PATH_SIZE];
    volatilePath(path, sizeof(path));
    remove(path);
}

static uint64_t littleEndian(const uint8_t* data, uint32_t size) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < size && i < 8; i++) {
        value |= (uint64_t)data[i] << (8 * i);
    }
    return value;
}

void carafeRegistryFormat(uint32_t type, const uint8_t* data, uint32_t size, char* out, size_t out_size) {
    out[0] = '\0';
    if (type == CARAFE_REG_DWORD && size == 4) {
        snprintf(out, out_size, "%lu", (unsigned long)littleEndian(data, 4));
        return;
    }
    if (type == CARAFE_REG_QWORD && size == 8) {
        snprintf(out, out_size, "%llu", (unsigned long long)littleEndian(data, 8));
        return;
    }
    if (type == CARAFE_REG_SZ || type == CARAFE_REG_EXPAND_SZ || type == CARAFE_REG_MULTI_SZ) {
        uint16_t wide[CARAFE_REG_DATA / 2];
        size_t count = size / 2;
        if (count > CARAFE_REG_DATA / 2) {
            count = CARAFE_REG_DATA / 2;
        }
        for (size_t i = 0; i < count; i++) {
            wide[i] = (uint16_t)(data[2 * i] | data[2 * i + 1] << 8);
        }
        while (count > 0 && wide[count - 1] == 0) {
            count--;
        }
        if (type == CARAFE_REG_MULTI_SZ) {
            for (size_t i = 0; i < count; i++) {
                if (wide[i] == 0) {
                    wide[i] = ';';
                }
            }
        }
        utf8From16(wide, count, out, out_size);
        return;
    }
    size_t used = 0;
    for (uint32_t i = 0; i < size && used + 4 < out_size; i++) {
        used += (size_t)snprintf(out + used, out_size - used, "%s%02x", i ? " " : "", data[i]);
        if (i == 15 && size > 16) {
            snprintf(out + used, out_size - used, " …");
            break;
        }
    }
}
