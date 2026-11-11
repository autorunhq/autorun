#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "carafe_overlay.h"
#include "carafe_registry.h"
#include "carafe_settings.h"
#include "fex_options.h"
#include "input_profile.h"
#include "launcher_settings.h"

#define MENU_FILE "/carafe/menu.txt"
#define MENU_LIMIT (1024 * 1024)
#define CHANGES_DIR "sdmc:/switch/wine/carafe"
#define RUNTIME_ROOT "sdmc:/switch/wine"
#define ARGV_SIZE 2048
#define PATH_SIZE 768
#define FILE_LIMIT (64 * 1024)
#define KEY_SIZE 64
#define MAX_CHANGES 512
#define MAX_SECTIONS 8
#define MAX_SETTINGS 320
#define MAX_GROUPS 32
#define MAX_PAGES 12
#define MAX_ROWS 96
#define MAX_PRESETS 8
#define MAX_SETS 32
#define MAX_SUGGESTS 48
#define MAX_KEYS 192
#define MAX_PADS 32
#define MAX_VALUES 24
#define FIELDS 16
#define TRIAL_SECONDS 300

typedef struct {
    CarafeFile file;
    char key[KEY_SIZE];
    char value[CARAFE_VALUE_SIZE];
} Change;

typedef struct {
    char when[2][96];
} PendingWhen;

const char* const carafe_source_names[CARAFE_SOURCES] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "STICKL", "STICKR",
    "UP", "DOWN", "LEFT", "RIGHT", "LUP", "LDOWN", "LLEFT", "LRIGHT", "RUP", "RDOWN", "RLEFT", "RRIGHT",
};

static const char* const FILE_NAMES[CarafeFileCount] = { "program", "keys", "box64" };
static const char* const FILE_SUFFIXES[CarafeFileCount] = { ".wine-nx.txt", ".keys.txt", ".box64.txt" };

extern struct input_profile wine_nx_input_profile;
extern int wine_nx_swkbd_auto_enabled;
extern void wine_nx_input_profile_set(const struct input_profile* profile) __attribute__((weak));
extern void wine_nx_graphics_configure(int frame_limit, int vsync) __attribute__((weak));
extern void wine_nx_request_quit(const char* why) __attribute__((weak));

static pthread_mutex_t g_tick_lock = PTHREAD_MUTEX_INITIALIZER;
static Change g_changes[MAX_CHANGES];
static int g_change_count;
static bool g_changes_loaded;
static char* g_start_changes;
static bool g_restart;
static bool g_trial_active;
static u64 g_start_tick;
static bool g_trial_done;
static bool g_trial_tick_cleared;
static char* g_effective[CarafeFileCount];
static char* g_snapshot_disk[CarafeFileCount];
static int g_swkbd_auto_fallback = 1;

static void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    carafeOverlayLog(line);
}

static void ensureParentDir(const char* path) {
    char buf[PATH_SIZE];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len + 1);
    char* slash = strrchr(buf, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    for (char* p = buf; *p != '\0'; p++) {
        if (*p == '/' && p != buf && p[-1] != ':') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static bool programFilePath(const char* name, char* out, size_t size) {
    char dir[PATH_SIZE];
    if (carafeOverlayProgramDir != NULL && carafeOverlayProgramDir(dir, sizeof(dir))) {
        int written = snprintf(out, size, "%s/%s", dir, name);
        return written > 0 && (size_t)written < size;
    }
    u64 program_id = 0;
    svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/wine", 0777);
    mkdir(CHANGES_DIR, 0777);
    snprintf(dir, sizeof(dir), "%s/%016lx", CHANGES_DIR, program_id);
    mkdir(dir, 0777);
    int written = snprintf(out, size, "%s/%s", dir, name);
    return written > 0 && (size_t)written < size;
}

static CarafeLanguage languageOf(SetLanguage set) {
    switch (set) {
    case SetLanguage_RU:
        return CarafeLanguageRu;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:
        return CarafeLanguageZhHans;
    case SetLanguage_JA:
        return CarafeLanguageJa;
    case SetLanguage_KO:
        return CarafeLanguageKo;
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT:
        return CarafeLanguageZhHant;
    case SetLanguage_FR:
        return CarafeLanguageFr;
    case SetLanguage_FRCA:
        return CarafeLanguageFrCa;
    case SetLanguage_DE:
        return CarafeLanguageDe;
    case SetLanguage_IT:
        return CarafeLanguageIt;
    case SetLanguage_ES:
        return CarafeLanguageEs;
    case SetLanguage_ES419:
        return CarafeLanguageEs419;
    case SetLanguage_NL:
        return CarafeLanguageNl;
    case SetLanguage_PT:
        return CarafeLanguagePt;
    case SetLanguage_PTBR:
        return CarafeLanguagePtBr;
    default:
        return CarafeLanguageEn;
    }
}

CarafeLanguage carafeLanguage(void) {
    static int language = -1;
    if (language >= 0) {
        return (CarafeLanguage)language;
    }
    language = CarafeLanguageEn;
    if (R_FAILED(setInitialize())) {
        return (CarafeLanguage)language;
    }
    u64 code = 0;
    SetLanguage set = SetLanguage_ENUS;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &set))) {
        language = languageOf(set);
    }
    setExit();
    return (CarafeLanguage)language;
}

const char* carafeWindowsLocale(void) {
    static const char* const locales[CarafeLanguageCount] = {
        [CarafeLanguageRu] = "ru-RU",
        [CarafeLanguageEn] = "en-US",
        [CarafeLanguageZhHans] = "zh-CN",
        [CarafeLanguageJa] = "ja-JP",
        [CarafeLanguageKo] = "ko-KR",
        [CarafeLanguageZhHant] = "zh-TW",
        [CarafeLanguageFr] = "fr-FR",
        [CarafeLanguageFrCa] = "fr-CA",
        [CarafeLanguageDe] = "de-DE",
        [CarafeLanguageIt] = "it-IT",
        [CarafeLanguageEs] = "es-ES",
        [CarafeLanguageEs419] = "es-MX",
        [CarafeLanguageNl] = "nl-NL",
        [CarafeLanguagePt] = "pt-PT",
        [CarafeLanguagePtBr] = "pt-BR",
    };
    return locales[carafeLanguage()];
}

int carafePluralForm(int count) {
    switch (carafeLanguage()) {
    case CarafeLanguageRu:
        return count % 10 == 1 && count % 100 != 11 ? 0
            : count % 10 >= 2 && count % 10 <= 4 && (count % 100 < 12 || count % 100 > 14) ? 1
                                                                                           : 2;
    case CarafeLanguageJa:
    case CarafeLanguageKo:
    case CarafeLanguageZhHans:
    case CarafeLanguageZhHant:
        return 0;
    case CarafeLanguageFr:
    case CarafeLanguageFrCa:
    case CarafeLanguagePtBr:
        return count == 0 || count == 1 ? 0 : 1;
    default:
        return count == 1 ? 0 : 1;
    }
}

const char* carafeText(const char* const texts[CARAFE_TEXTS]) {
    return texts[0][0] != '\0' ? texts[0] : texts[1];
}

bool carafeGameArgv(char* out, size_t size) {
    if (carafeOverlayProgramTarget != NULL && carafeOverlayProgramTarget(out, size)) {
        char quoted[ARGV_SIZE];
        int written = snprintf(quoted, sizeof(quoted), "\"%s\"", out);
        if (written > 0 && (size_t)written < size) {
            snprintf(out, size, "%s", quoted);
            return true;
        }
    }
    if (!carafeOverlayReadRomfsText("/carafe/argv", out, size)) {
        return false;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

static bool gameTarget(char* out, size_t size) {
    if (carafeOverlayProgramTarget != NULL && carafeOverlayProgramTarget(out, size)) {
        return true;
    }
    char argv[ARGV_SIZE];
    if (!carafeGameArgv(argv, sizeof(argv))) {
        return false;
    }
    char* open = strchr(argv, '"');
    char* close = open != NULL ? strchr(open + 1, '"') : NULL;
    if (close == NULL || (size_t)(close - open - 1) >= size) {
        return false;
    }
    snprintf(out, size, "%.*s", (int)(close - open - 1), open + 1);
    return true;
}

static bool filePath(CarafeFile file, char* out, size_t size) {
    char target[PATH_SIZE];
    if (!gameTarget(target, sizeof(target))) {
        return false;
    }
    if (file == CarafeFileProgram) {
        return launcher_program_settings_path(RUNTIME_ROOT, target, out, size) != 0;
    }
    if (file == CarafeFileKeys && launcher_settings_on_usb(target)) {
        return launcher_program_settings_path(RUNTIME_ROOT, target, out, size) != 0;
    }
    size_t length = strlen(target);
    if (length < 4 || strcasecmp(target + length - 4, ".exe") != 0) {
        return false;
    }
    int written = snprintf(out, size, "%.*s%s", (int)(length - 4), target, FILE_SUFFIXES[file]);
    return written > 0 && (size_t)written < size;
}

static char* readWhole(const char* path, size_t limit) {
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

static bool writeWhole(const char* path, const char* text) {
    ensureParentDir(path);
    char temp[PATH_SIZE];
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE* file = fopen(temp, "wb");
    if (file == NULL) {
        return false;
    }
    size_t length = strlen(text);
    bool ok = fwrite(text, 1, length, file) == length;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove(temp);
        return false;
    }
    remove(path);
    return rename(temp, path) == 0;
}

static char* romfsText(CarafeFile file) {
    char path[PATH_SIZE];
    if (!filePath(file, path, sizeof(path)) || strncmp(path, "sdmc:", 5) != 0) {
        return NULL;
    }
    size_t size = 0;
    char* data = carafeOverlayReadRomfsFile(path + 5, FILE_LIMIT, &size);
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

static bool lineKey(const char* line, size_t length, const char* key, const char** value, size_t* value_length) {
    size_t p = 0;
    size_t key_length = strlen(key);
    while (p < length && (line[p] == ' ' || line[p] == '\t')) {
        p++;
    }
    if (length - p < key_length || strncasecmp(line + p, key, key_length) != 0) {
        return false;
    }
    p += key_length;
    while (p < length && (line[p] == ' ' || line[p] == '\t')) {
        p++;
    }
    if (p >= length || line[p] != '=') {
        return false;
    }
    p++;
    while (p < length && (line[p] == ' ' || line[p] == '\t')) {
        p++;
    }
    size_t end = length;
    while (end > p && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) {
        end--;
    }
    *value = line + p;
    *value_length = end - p;
    return true;
}

static bool textValue(const char* text, const char* key, char* out, size_t size) {
    if (text == NULL) {
        return false;
    }
    const char* line = text;
    while (*line != '\0') {
        const char* end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        const char* value = NULL;
        size_t value_length = 0;
        if (lineKey(line, length, key, &value, &value_length)) {
            snprintf(out, size, "%.*s", (int)value_length, value);
            return true;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return false;
}

static char* textSet(const char* text, const char* key, const char* value) {
    size_t length = text != NULL ? strlen(text) : 0;
    size_t room = length + strlen(key) + (value != NULL ? strlen(value) : 0) + 8;
    char* out = malloc(room);
    if (out == NULL) {
        return NULL;
    }
    size_t used = 0;
    bool placed = false;
    const char* line = text != NULL ? text : "";
    while (*line != '\0') {
        const char* end = strchr(line, '\n');
        size_t line_length = end != NULL ? (size_t)(end - line) : strlen(line);
        const char* found = NULL;
        size_t found_length = 0;
        if (lineKey(line, line_length, key, &found, &found_length)) {
            if (value != NULL && !placed) {
                used += (size_t)snprintf(out + used, room - used, "%s=%s\n", key, value);
            }
            placed = true;
        } else {
            memcpy(out + used, line, line_length);
            used += line_length;
            out[used++] = '\n';
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    if (!placed && value != NULL) {
        used += (size_t)snprintf(out + used, room - used, "%s=%s\n", key, value);
    }
    out[used] = '\0';
    return out;
}

static bool fileFromName(const char* name, CarafeFile* file) {
    for (int i = 0; i < CarafeFileCount; i++) {
        if (strcmp(name, FILE_NAMES[i]) == 0) {
            *file = (CarafeFile)i;
            return true;
        }
    }
    return false;
}

static int parseChangesText(const char* text, Change* out, int max_changes) {
    int count = 0;
    if (text == NULL || strncmp(text, "carafe-changes\t1", 16) != 0) {
        return 0;
    }
    const char* line = strchr(text, '\n');
    while (line != NULL && *++line != '\0' && count < max_changes) {
        const char* end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        char copy[KEY_SIZE + CARAFE_VALUE_SIZE + 16];
        snprintf(copy, sizeof(copy), "%.*s", (int)(length < sizeof(copy) ? length : sizeof(copy) - 1), line);
        copy[strcspn(copy, "\r")] = '\0';
        char* key = strchr(copy, '\t');
        char* value = key != NULL ? strchr(key + 1, '\t') : NULL;
        CarafeFile file;
        if (value != NULL) {
            *key++ = '\0';
            *value++ = '\0';
            if (fileFromName(copy, &file) && key[0] != '\0') {
                Change* change = &out[count++];
                change->file = file;
                snprintf(change->key, sizeof(change->key), "%s", key);
                snprintf(change->value, sizeof(change->value), "%s", value);
            }
        }
        line = end;
    }
    return count;
}

static void parseChanges(const char* text) {
    g_change_count = parseChangesText(text, g_changes, MAX_CHANGES);
}

static void loadChanges(void) {
    if (__atomic_load_n(&g_changes_loaded, __ATOMIC_ACQUIRE)) {
        return;
    }
    char path[PATH_SIZE];
    if (programFilePath("changes.txt", path, sizeof(path))) {
        char* text = readWhole(path, FILE_LIMIT);
        parseChanges(text);
        free(text);
    }
    __atomic_store_n(&g_changes_loaded, true, __ATOMIC_RELEASE);
}

static Change* findChange(CarafeFile file, const char* key) {
    for (int i = 0; i < g_change_count; i++) {
        if (g_changes[i].file == file && strcasecmp(g_changes[i].key, key) == 0) {
            return &g_changes[i];
        }
    }
    return NULL;
}

static void removeChange(CarafeFile file, const char* key) {
    for (int i = 0; i < g_change_count; i++) {
        if (g_changes[i].file == file && strcasecmp(g_changes[i].key, key) == 0) {
            if (i + 1 < g_change_count) {
                memmove(&g_changes[i], &g_changes[i + 1], (size_t)(g_change_count - i - 1) * sizeof(Change));
            }
            g_change_count--;
            return;
        }
    }
}

static bool recordChange(CarafeFile file, const char* key, const char* value) {
    Change* change = findChange(file, key);
    if (change == NULL) {
        if (g_change_count >= MAX_CHANGES) {
            return false;
        }
        change = &g_changes[g_change_count++];
        change->file = file;
        snprintf(change->key, sizeof(change->key), "%s", key);
    }
    snprintf(change->value, sizeof(change->value), "%s", value);
    return true;
}

static char* changesText(void) {
    size_t room = 32 + (size_t)g_change_count * (KEY_SIZE + CARAFE_VALUE_SIZE + 16);
    char* text = malloc(room);
    if (text == NULL) {
        return NULL;
    }
    size_t used = (size_t)snprintf(text, room, "carafe-changes\t1\n");
    for (int i = 0; i < g_change_count; i++) {
        used += (size_t)snprintf(text + used, room - used, "%s\t%s\t%s\n", FILE_NAMES[g_changes[i].file],
            g_changes[i].key, g_changes[i].value);
    }
    return text;
}

bool carafeSaveChanges(void) {
    char path[PATH_SIZE];
    if (!programFilePath("changes.txt", path, sizeof(path))) {
        return false;
    }
    if (g_change_count == 0) {
        remove(path);
        return true;
    }
    char* text = changesText();
    bool ok = text != NULL && writeWhole(path, text);
    free(text);
    return ok;
}

bool carafeReadChangesText(char* out, size_t size) {
    char path[PATH_SIZE];
    if (!programFilePath("changes.txt", path, sizeof(path))) {
        return false;
    }
    char* text = readWhole(path, FILE_LIMIT);
    if (text == NULL) {
        return false;
    }
    snprintf(out, size, "%s", text);
    free(text);
    return true;
}

bool carafeWriteChangesText(const char* text) {
    char path[PATH_SIZE];
    if (!programFilePath("changes.txt", path, sizeof(path))) {
        return false;
    }
    __atomic_store_n(&g_changes_loaded, false, __ATOMIC_RELEASE);
    if (text == NULL || text[0] == '\0') {
        remove(path);
        return true;
    }
    return writeWhole(path, text);
}

bool carafeChangesExist(void) {
    char path[PATH_SIZE];
    if (!programFilePath("changes.txt", path, sizeof(path))) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0;
}

static char* mergedText(CarafeFile file, const char* base) {
    char* text = strdup(base != NULL ? base : "");
    for (int i = 0; text != NULL && i < g_change_count; i++) {
        if (g_changes[i].file != file) {
            continue;
        }
        char* next = textSet(text, g_changes[i].key, strcmp(g_changes[i].value, "-") == 0 ? NULL : g_changes[i].value);
        free(text);
        text = next;
    }
    return text;
}

/* Write a single key=value directly into a program's disk file, preserving unrelated Launcher keys. */
static bool applyOneKeyToDisk(CarafeFile file, const char* key, const char* value) {
    char path[PATH_SIZE];
    if (!filePath(file, path, sizeof(path))) {
        return false;
    }
    char* disk = readWhole(path, FILE_LIMIT);
    const char* val = (value == NULL || strcmp(value, "-") == 0) ? NULL : value;
    char* updated = textSet(disk, key, val);
    free(disk);
    if (updated == NULL) {
        return false;
    }
    bool ok = true;
    if (updated[0] == '\0') {
        remove(path);
    } else {
        ok = writeWhole(path, updated);
    }
    free(updated);
    /* If a keys.txt setting was previously overridden inside .wine-nx.txt by Launcher, clear that duplicate so keys.txt wins cleanly. */
    if (file == CarafeFileKeys) {
        char prog_path[PATH_SIZE];
        if (filePath(CarafeFileProgram, prog_path, sizeof(prog_path)) && strcmp(prog_path, path) != 0) {
            char* prog = readWhole(prog_path, FILE_LIMIT);
            if (prog != NULL) {
                char dummy[CARAFE_VALUE_SIZE];
                if (textValue(prog, key, dummy, sizeof(dummy))) {
                    char* trimmed = textSet(prog, key, NULL);
                    if (trimmed != NULL) {
                        if (trimmed[0] == '\0') {
                            remove(prog_path);
                        } else {
                            writeWhole(prog_path, trimmed);
                        }
                        free(trimmed);
                    }
                }
                free(prog);
            }
        }
    }
    return ok;
}

static void applyPendingArrayToDisk(void) {
    for (int f = 0; f < CarafeFileCount; f++) {
        bool touched = false;
        for (int i = 0; i < g_change_count; i++) {
            if (g_changes[i].file == (CarafeFile)f) {
                touched = true;
                break;
            }
        }
        if (!touched) {
            continue;
        }
        char path[PATH_SIZE];
        if (!filePath((CarafeFile)f, path, sizeof(path))) {
            continue;
        }
        char* base = readWhole(path, FILE_LIMIT);
        char* merged = mergedText((CarafeFile)f, base);
        free(base);
        if (merged != NULL) {
            if (merged[0] == '\0') {
                remove(path);
            } else {
                writeWhole(path, merged);
            }
            free(merged);
        }
        if ((CarafeFile)f == CarafeFileKeys) {
            char prog_path[PATH_SIZE];
            if (filePath(CarafeFileProgram, prog_path, sizeof(prog_path)) && strcmp(prog_path, path) != 0) {
                char* prog = readWhole(prog_path, FILE_LIMIT);
                if (prog != NULL) {
                    bool prog_changed = false;
                    for (int i = 0; i < g_change_count; i++) {
                        if (g_changes[i].file != CarafeFileKeys) {
                            continue;
                        }
                        char dummy[CARAFE_VALUE_SIZE];
                        if (textValue(prog, g_changes[i].key, dummy, sizeof(dummy))) {
                            char* trimmed = textSet(prog, g_changes[i].key, NULL);
                            if (trimmed != NULL) {
                                free(prog);
                                prog = trimmed;
                                prog_changed = true;
                            }
                        }
                    }
                    if (prog_changed) {
                        if (prog[0] == '\0') {
                            remove(prog_path);
                        } else {
                            writeWhole(prog_path, prog);
                        }
                    }
                    free(prog);
                }
            }
        }
    }
}

static void volatilePath(const char* name, char* out, size_t size) {
    programFilePath(name, out, size);
}

static char* programText(void) {
    char path[PATH_SIZE];
    if (!filePath(CarafeFileProgram, path, sizeof(path))) {
        return NULL;
    }
    char* text = readWhole(path, FILE_LIMIT);
    return text != NULL ? text : romfsText(CarafeFileProgram);
}

bool carafeMeasuring(void) {
    char* text = programText();
    char value[16] = "";
    bool profiling = textValue(text, "profile", value, sizeof(value)) && strcmp(value, "1") == 0;
    free(text);
    return profiling;
}

bool carafeOrchestrating(void) {
    char* text = programText();
    char value[16] = "";
    bool on = !textValue(text, "orchestrator", value, sizeof(value)) || strcmp(value, "0") != 0;
    free(text);
    return on;
}

static void applyEnvironmentFromProgram(void) {
    setenv("NVK_SWITCH_CPU_WRITE_MEM_UNCACHED", "0", 1);
    char* text = programText();
    if (text == NULL) {
        unsetenv("CARAFE_LARGE_GUEST_PAGES");
        return;
    }
    char value[16] = "";
    if (textValue(text, "large-pages", value, sizeof(value)) && strcmp(value, "1") == 0) {
        setenv("CARAFE_LARGE_GUEST_PAGES", "1", 1);
        note("[CARAFE] settings: large views of the game's memory start on 2 MB");
    } else {
        unsetenv("CARAFE_LARGE_GUEST_PAGES");
    }
    free(text);
}

void carafeSettingsSyncProgram(void) {
    pthread_mutex_lock(&g_tick_lock);
    g_start_tick = armGetSystemTick();
    g_swkbd_auto_fallback = __atomic_load_n(&wine_nx_swkbd_auto_enabled, __ATOMIC_RELAXED);
    __atomic_store_n(&g_changes_loaded, false, __ATOMIC_RELEASE);
    g_change_count = 0;
    g_restart = false;
    g_trial_done = false;
    __atomic_store_n(&g_trial_tick_cleared, false, __ATOMIC_RELEASE);
    free(g_start_changes);
    g_start_changes = NULL;
    for (int f = 0; f < CarafeFileCount; f++) {
        free(g_snapshot_disk[f]);
        g_snapshot_disk[f] = NULL;
    }

    char changes_path[PATH_SIZE];
    if (programFilePath("changes.txt", changes_path, sizeof(changes_path))) {
        g_start_changes = readWhole(changes_path, FILE_LIMIT);
        parseChanges(g_start_changes);
    }

    char trial[PATH_SIZE];
    volatilePath("trial.txt", trial, sizeof(trial));
    struct stat st;
    g_trial_active = stat(trial, &st) == 0;
    __atomic_store_n(&g_changes_loaded, true, __ATOMIC_RELEASE);

    if (g_change_count > 0) {
        applyPendingArrayToDisk();
        g_change_count = 0;
        remove(changes_path);
        note("[CARAFE] settings: pending changes written to program configuration%s",
            g_trial_active ? ", trial run" : "");
    }

    /* Capture initial disk texts so Trial rollback / Undo knows exact prior values even without changes.txt. */
    for (int f = 0; f < CarafeFileCount; f++) {
        char p[PATH_SIZE];
        if (filePath((CarafeFile)f, p, sizeof(p))) {
            g_snapshot_disk[f] = readWhole(p, FILE_LIMIT);
        }
    }

    applyEnvironmentFromProgram();
    pthread_mutex_unlock(&g_tick_lock);
}

void carafeSettingsPrepareError(void) {
    pthread_mutex_lock(&g_tick_lock);
    g_start_tick = armGetSystemTick();
    g_swkbd_auto_fallback = __atomic_load_n(&wine_nx_swkbd_auto_enabled, __ATOMIC_RELAXED);
    __atomic_store_n(&g_changes_loaded, false, __ATOMIC_RELEASE);
    g_change_count = 0;
    g_restart = false;
    g_trial_done = false;
    __atomic_store_n(&g_trial_tick_cleared, false, __ATOMIC_RELEASE);
    free(g_start_changes);
    g_start_changes = NULL;
    for (int f = 0; f < CarafeFileCount; f++) {
        free(g_snapshot_disk[f]);
        g_snapshot_disk[f] = NULL;
    }

    char trial[PATH_SIZE];
    volatilePath("trial.txt", trial, sizeof(trial));
    struct stat st;
    g_trial_active = stat(trial, &st) == 0;

    /* Capture disk texts without applying or deleting changes.txt so deferred
     * changes stay visible and become part of any trial restart. */
    for (int f = 0; f < CarafeFileCount; f++) {
        char p[PATH_SIZE];
        if (filePath((CarafeFile)f, p, sizeof(p))) {
            g_snapshot_disk[f] = readWhole(p, FILE_LIMIT);
        }
    }
    pthread_mutex_unlock(&g_tick_lock);
}

bool carafeOptionsSummary(char* out, size_t size) {
    char* text = programText();
    if (text == NULL || size == 0) {
        free(text);
        return false;
    }
    size_t used = 0;
    out[0] = '\0';
    for (const char* line = text; *line != '\0' && used + 1 < size;) {
        const char* end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        const char* start = line;
        while (start < line + length && (*start == ' ' || *start == '\t')) {
            start++;
        }
        const char* equals = memchr(start, '=', (size_t)(line + length - start));
        if (equals != NULL &&
            (strncmp(start, "FEX_", 4) == 0 || strncmp(start, "d3d9.", 5) == 0 || strncmp(start, "dxvk.", 5) == 0)) {
            const char* key_end = equals;
            while (key_end > start && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
                key_end--;
            }
            const char* value = equals + 1;
            const char* value_end = line + length;
            while (value < value_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r')) {
                value_end--;
            }
            used += (size_t)snprintf(out + used, size - used, "%s%.*s=%.*s", used > 0 ? " " : "",
                (int)(key_end - start), start, (int)(value_end - value), value);
            if (used >= size) {
                used = size - 1;
            }
        }
        line = end != NULL ? end + 1 : line + length;
    }
    free(text);
    return true;
}

static char* mergeTextLayers(const char* base, const char* over) {
    if (base == NULL) {
        return over != NULL ? strdup(over) : NULL;
    }
    if (over == NULL) {
        return strdup(base);
    }
    size_t blen = strlen(base);
    size_t olen = strlen(over);
    char* out = malloc(blen + olen + 4);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, base, blen);
    size_t used = blen;
    if (used > 0 && out[used - 1] != '\n') {
        out[used++] = '\n';
    }
    memcpy(out + used, over, olen);
    used += olen;
    out[used] = '\0';
    /* Replay keys from over cleanly via textSet so textValue() reads the winning value. */
    char* res = strdup(base);
    const char* line = over;
    while (res != NULL && *line != '\0') {
        const char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        const char* eq = memchr(line, '=', len);
        if (eq != NULL) {
            const char* ks = line;
            while (ks < eq && (*ks == ' ' || *ks == '\t')) ks++;
            const char* ke = eq;
            while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            const char* vs = eq + 1;
            const char* ve = line + len;
            while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
            while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
            if (ke > ks && (size_t)(ke - ks) < KEY_SIZE && (size_t)(ve - vs) < CARAFE_VALUE_SIZE) {
                char k[KEY_SIZE];
                char v[CARAFE_VALUE_SIZE];
                snprintf(k, sizeof(k), "%.*s", (int)(ke - ks), ks);
                snprintf(v, sizeof(v), "%.*s", (int)(ve - vs), vs);
                char* next = textSet(res, k, v);
                free(res);
                res = next;
            }
        }
        line = end != NULL ? end + 1 : line + len;
    }
    free(out);
    return res;
}

static char* effectiveText(CarafeFile file) {
    char path[PATH_SIZE];
    if (!filePath(file, path, sizeof(path))) {
        return NULL;
    }
    if (file == CarafeFileKeys) {
        char* global_keys = readWhole(RUNTIME_ROOT "/config/keys.txt", FILE_LIMIT);
        char* keys_file = readWhole(path, FILE_LIMIT);
        char* merged = mergeTextLayers(global_keys, keys_file);
        free(global_keys);
        free(keys_file);
        char prog_path[PATH_SIZE];
        if (filePath(CarafeFileProgram, prog_path, sizeof(prog_path)) && strcmp(prog_path, path) != 0) {
            char* prog = readWhole(prog_path, FILE_LIMIT);
            if (prog != NULL) {
                char* next = mergeTextLayers(merged, prog);
                free(merged);
                merged = next;
                free(prog);
            }
        }
        return merged;
    }
    return readWhole(path, FILE_LIMIT);
}

static char* nextField(char** cursor) {
    char* start = *cursor;
    if (start == NULL) {
        return NULL;
    }
    char* tab = strchr(start, '\t');
    if (tab != NULL) {
        *tab = '\0';
        *cursor = tab + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static uint32_t apiMask(const char* text) {
    static const char* const NAMES[] = { "d3d8", "d3d9", "d3d10", "d3d11", "d3d12" };
    if (text == NULL || strcmp(text, "-") == 0) {
        return 0;
    }
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        const char* at = text;
        size_t length = strlen(NAMES[i]);
        while ((at = strstr(at, NAMES[i])) != NULL) {
            if ((at == text || at[-1] == ',') && (at[length] == '\0' || at[length] == ',')) {
                mask |= 1u << i;
            }
            at += length;
        }
    }
    return mask;
}

static CarafeFailure failureFromName(const char* name) {
    if (strcmp(name, "crash") == 0) {
        return CarafeKindCrash;
    }
    if (strcmp(name, "memory") == 0) {
        return CarafeKindMemory;
    }
    if (strcmp(name, "threads") == 0) {
        return CarafeKindThreads;
    }
    return CarafeKindNone;
}

int carafeCatalogFind(const CarafeCatalog* catalog, const char* id) {
    for (int i = 0; i < catalog->setting_count; i++) {
        if (strcmp(catalog->settings[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int carafeCatalogFindKey(const CarafeCatalog* catalog, CarafeFile file, const char* key) {
    for (int i = 0; i < catalog->setting_count; i++) {
        if (catalog->settings[i].file == file && strcasecmp(catalog->settings[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static void parseWhen(const CarafeCatalog* catalog, const char* text, CarafeWhen* when) {
    when->kind = CarafeWhenAlways;
    when->setting = -1;
    when->values = 0;
    if (text == NULL || strcmp(text, "-") == 0) {
        return;
    }
    CarafeWhenKind kind = strncmp(text, "show:", 5) == 0 ? CarafeWhenShow : strncmp(text, "on:", 3) == 0 ? CarafeWhenOn
                                                                                                           : CarafeWhenAlways;
    if (kind == CarafeWhenAlways) {
        return;
    }
    const char* body = strchr(text, ':') + 1;
    const char* equals = strchr(body, '=');
    if (equals == NULL) {
        return;
    }
    char id[KEY_SIZE];
    snprintf(id, sizeof(id), "%.*s", (int)(equals - body), body);
    int setting = carafeCatalogFind(catalog, id);
    if (setting < 0) {
        return;
    }
    uint32_t values = 0;
    for (const char* at = equals + 1; *at != '\0';) {
        char* end = NULL;
        long index = strtol(at, &end, 10);
        if (end == at) {
            break;
        }
        if (index >= 0 && index < 32) {
            values |= 1u << index;
        }
        at = *end == ',' ? end + 1 : end;
    }
    when->kind = kind;
    when->setting = setting;
    when->values = values;
}

static const char* normalizeAliasValue(const CarafeSetting* setting, const char* text) {
    if (text == NULL || text[0] == '\0') {
        return "-";
    }
    const char* key = setting->key;
    if (strcasecmp(key, "button-layout") == 0) {
        if (strcasecmp(text, "xbox") == 0 || strcasecmp(text, "abxy") == 0) {
            return "0";
        }
        if (strcasecmp(text, "switch") == 0 || strcasecmp(text, "nintendo") == 0 || strcasecmp(text, "bayx") == 0) {
            return "1";
        }
    } else if (strcasecmp(key, "upscaling") == 0) {
        if (strcasecmp(text, "off") == 0 || strcasecmp(text, "none") == 0 || strcmp(text, "0") == 0) {
            return "-";
        }
        if (strcmp(text, "1") == 0) {
            return "fsr";
        }
        if (strcmp(text, "2") == 0) {
            return "integer";
        }
    } else if (strcasecmp(key, "upscaling-sharpness") == 0 && strcmp(text, "40%") == 0) {
        return "-";
    } else if ((strcasecmp(key, "vsync") == 0 || strcasecmp(key, "four-cores") == 0
                   || strcasecmp(key, "lsfg-performance") == 0)
        && (strcmp(text, "1") == 0 || strcasecmp(text, "on") == 0)) {
        return "-";
    } else if ((strcasecmp(key, "lsfg") == 0 || strcasecmp(key, "lazy-file-views") == 0
                   || strcasecmp(key, "large-pages") == 0 || strcasecmp(key, "dxvk-hud") == 0)
        && (strcmp(text, "0") == 0 || strcasecmp(text, "off") == 0)) {
        return "-";
    } else if (strcasecmp(key, "lsfg-flow") == 0 && strcmp(text, "0.25") == 0) {
        return "-";
    } else if (strcasecmp(key, "d3d") == 0 && strcasecmp(text, "dxvk") == 0) {
        return "-";
    } else if (strcasecmp(key, "dxvk-source") == 0 && strcasecmp(text, "official") == 0) {
        return "-";
    } else if (strcasecmp(key, "cpu") == 0 && strcasecmp(text, "fex") == 0) {
        return "-";
    } else if (strcasecmp(key, "sync") == 0 && strcasecmp(text, "standard") == 0) {
        return "-";
    } else if (strcasecmp(key, "frame-limit") == 0 && strcmp(text, "0") == 0) {
        return "-";
    } else if (strncasecmp(key, "FEX_", 4) == 0) {
        for (size_t i = 0; i < NX_FEX_OPTION_COUNT; i++) {
            if (strcasecmp(nx_fex_options[i].name, key) == 0) {
                const char* def = nx_fex_options[i].values[nx_fex_options[i].default_choice];
                if (def != NULL && strcasecmp(text, def) == 0) {
                    return "-";
                }
                break;
            }
        }
    }
    return NULL;
}

static int valueIndex(const CarafeSetting* setting, const char* text, bool present) {
    for (int i = 0; i < setting->value_count; i++) {
        const char* value = setting->values[i].text;
        if (!present ? strcmp(value, "-") == 0 : strcasecmp(value, text) == 0) {
            return i;
        }
    }
    if (present) {
        const char* alias = normalizeAliasValue(setting, text);
        if (alias != NULL) {
            for (int i = 0; i < setting->value_count; i++) {
                if (strcasecmp(setting->values[i].text, alias) == 0) {
                    return i;
                }
            }
        }
    }
    if (!present || (text != NULL && text[0] == '\0')) {
        return setting->default_index >= 0 && setting->default_index < setting->value_count ? setting->default_index : 0;
    }
    return -1;
}

static void settingValue(const CarafeCatalog* catalog, CarafeSetting* setting, const char* const* texts, bool start) {
    char value[CARAFE_VALUE_SIZE] = "";
    bool present = textValue(texts[setting->file], setting->key, value, sizeof(value));
    if (!start) {
        Change* change = findChange(setting->file, setting->key);
        if (change != NULL) {
            present = strcmp(change->value, "-") != 0;
            snprintf(value, sizeof(value), "%s", present ? change->value : "");
        }
    }
    (void)catalog;
    int index = valueIndex(setting, value, present);
    if (start) {
        setting->start = index;
        snprintf(setting->start_text, sizeof(setting->start_text), "%s", value);
    } else {
        setting->current = index;
        snprintf(setting->current_text, sizeof(setting->current_text), "%s", value);
    }
}

static bool parseChord(const char* text, CarafeChord* chord) {
    chord->count = 0;
    if (text == NULL || strcasecmp(text, "none") == 0 || text[0] == '\0') {
        return true;
    }
    for (const char* at = text; *at != '\0' && chord->count < CARAFE_CHORD_MAX;) {
        char* end = NULL;
        unsigned long code = strtoul(at, &end, 0);
        if (end == at) {
            return false;
        }
        chord->code[chord->count++] = (uint16_t)code;
        while (*end == ' ' || *end == '+') {
            end++;
        }
        at = end;
    }
    return true;
}

static void chordText(const CarafeChord* chord, char* out, size_t size) {
    if (chord->count == 0) {
        snprintf(out, size, "none");
        return;
    }
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < chord->count && used < size; i++) {
        used += (size_t)snprintf(out + used, size - used, "%s0x%x", i ? "+" : "", chord->code[i]);
    }
}

static void loadBindings(CarafeCatalog* catalog, const char* keys) {
    for (int i = 0; i < CARAFE_SOURCES; i++) {
        for (int pad = 0; pad < 2; pad++) {
            char key[KEY_SIZE];
            char value[CARAFE_VALUE_SIZE] = "";
            snprintf(key, sizeof(key), "%s.%s", pad ? "pad" : "key", carafe_source_names[i]);
            CarafeChord* start = pad ? &catalog->pad_start[i] : &catalog->key_start[i];
            CarafeChord* current = pad ? &catalog->pad_current[i] : &catalog->key_current[i];
            if (!textValue(keys, key, value, sizeof(value)) || !parseChord(value, start)) {
                start->count = 0;
            }
            *current = *start;
            Change* change = findChange(CarafeFileKeys, key);
            if (change != NULL && !parseChord(change->value, current)) {
                *current = *start;
            }
        }
    }
}

static void* grow(void* array, int count, size_t size, int capacity) {
    if (count >= capacity) {
        return NULL;
    }
    return (char*)array + (size_t)count * size;
}

static CarafeRow* addRow(CarafeRow* rows, int* count, CarafeRowKind kind, int index) {
    CarafeRow* row = grow(rows, *count, sizeof(CarafeRow), MAX_ROWS);
    if (row != NULL) {
        row->kind = kind;
        row->index = index;
        (*count)++;
    }
    return row;
}

static void texts(char** cursor, const char** out) {
    CarafeLanguage own = carafeLanguage();
    out[0] = "";
    out[1] = "";
    for (int i = 0; i < CarafeLanguageCount; i++) {
        const char* field = nextField(cursor);
        if (field == NULL) {
            field = "";
        }
        if (i == (int)own) {
            out[0] = field;
        }
        if (i == CarafeLanguageEn) {
            out[1] = field;
        }
    }
}

static bool languagesMatch(char* line) {
    static const char* const CODES[CarafeLanguageCount] = {
        "ru", "en", "zh-Hans", "ja", "ko", "zh-Hant", "fr", "fr-CA", "de", "it", "es", "es-419", "nl", "pt", "pt-BR",
    };
    char* cursor = line;
    const char* kind = nextField(&cursor);
    if (kind == NULL || strcmp(kind, "languages") != 0) {
        return false;
    }
    for (int i = 0; i < CarafeLanguageCount; i++) {
        const char* code = nextField(&cursor);
        if (code == NULL || strcmp(code, CODES[i]) != 0) {
            return false;
        }
    }
    return cursor == NULL;
}

static bool parseCatalog(CarafeCatalog* catalog, PendingWhen* pending, char** group_when) {
    char* line = catalog->text;
    char* end = strchr(line, '\n');
    if (end == NULL || strncmp(line, "carafe-menu\t2", 13) != 0 || (line[13] != '\n' && line[13] != '\r')) {
        note("[CARAFE] settings menu: %s is not version 2", MENU_FILE);
        return false;
    }
    line = end + 1;
    end = strchr(line, '\n');
    if (end == NULL) {
        return false;
    }
    *end = '\0';
    line[strcspn(line, "\r")] = '\0';
    if (!languagesMatch(line)) {
        note("[CARAFE] settings menu: %s lists other languages", MENU_FILE);
        return false;
    }
    int section = -1;
    int group = -1;
    int last_setting = -1;
    int last_preset = -1;
    line = end + 1;
    while (*line != '\0') {
        end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        line[strcspn(line, "\r")] = '\0';
        char* cursor = line;
        char* kind = nextField(&cursor);
        if (kind == NULL) {
        } else if (strcmp(kind, "api") == 0) {
            catalog->api |= apiMask(nextField(&cursor));
        } else if (strcmp(kind, "section") == 0 && catalog->section_count < MAX_SECTIONS) {
            CarafeSection* s = &catalog->sections[catalog->section_count];
            s->id = nextField(&cursor);
            texts(&cursor,s->name);
            s->rows = calloc(MAX_ROWS, sizeof(CarafeRow));
            s->presets = calloc(MAX_PRESETS, sizeof(CarafePreset));
            if (s->id == NULL || s->rows == NULL || s->presets == NULL) {
                return false;
            }
            section = catalog->section_count++;
            group = -1;
            last_preset = -1;
        } else if (strcmp(kind, "group") == 0 && section >= 0 && catalog->group_count < MAX_GROUPS) {
            CarafeGroup* g = &catalog->groups[catalog->group_count];
            g->id = nextField(&cursor);
            group_when[catalog->group_count] = nextField(&cursor);
            texts(&cursor,g->name);
            g->pages = calloc(MAX_PAGES, sizeof(CarafePage));
            if (g->id == NULL || g->pages == NULL) {
                return false;
            }
            group = catalog->group_count++;
            addRow(catalog->sections[section].rows, &catalog->sections[section].row_count, CarafeRowGroup, group);
        } else if (strcmp(kind, "page") == 0 && group >= 0) {
            CarafeGroup* g = &catalog->groups[group];
            if (g->page_count < MAX_PAGES) {
                CarafePage* p = &g->pages[g->page_count++];
                p->id = nextField(&cursor);
                texts(&cursor,p->name);
                p->rows = calloc(MAX_ROWS, sizeof(CarafeRow));
                if (p->rows == NULL) {
                    return false;
                }
            }
        } else if (strcmp(kind, "end") == 0) {
            group = -1;
        } else if (strcmp(kind, "setting") == 0 && section >= 0 && catalog->setting_count < MAX_SETTINGS) {
            CarafeSetting* s = &catalog->settings[catalog->setting_count];
            s->id = nextField(&cursor);
            const char* file = nextField(&cursor);
            s->key = nextField(&cursor);
            const char* apply = nextField(&cursor);
            const char* index = nextField(&cursor);
            const char* when = nextField(&cursor);
            const char* api = nextField(&cursor);
            texts(&cursor,s->name);
            texts(&cursor,s->hint);
            if (s->id == NULL || file == NULL || s->key == NULL || !fileFromName(file, &s->file)) {
                last_setting = -1;
                goto next;
            }
            s->now = apply != NULL && strcmp(apply, "now") == 0;
            s->default_index = index != NULL ? atoi(index) : 0;
            s->api = apiMask(api);
            s->values = calloc(MAX_VALUES, sizeof(CarafeValue));
            if (s->values == NULL) {
                return false;
            }
            snprintf(pending[catalog->setting_count].when[0], sizeof(pending[0].when[0]), "%s", when != NULL ? when : "-");
            last_setting = catalog->setting_count++;
            if (group >= 0) {
                CarafeGroup* g = &catalog->groups[group];
                if (g->page_count == 0) {
                    g->pages[0].id = g->id;
                    memcpy(g->pages[0].name, g->name, sizeof(g->name));
                    g->pages[0].rows = calloc(MAX_ROWS, sizeof(CarafeRow));
                    if (g->pages[0].rows == NULL) {
                        return false;
                    }
                    g->page_count = 1;
                }
                CarafePage* p = &g->pages[g->page_count - 1];
                addRow(p->rows, &p->row_count, CarafeRowSetting, last_setting);
            } else {
                addRow(catalog->sections[section].rows, &catalog->sections[section].row_count, CarafeRowSetting,
                    last_setting);
            }
        } else if (strcmp(kind, "value") == 0 && last_setting >= 0) {
            CarafeSetting* s = &catalog->settings[last_setting];
            if (s->value_count < MAX_VALUES) {
                CarafeValue* v = &s->values[s->value_count++];
                const char* text = nextField(&cursor);
                v->text = text != NULL ? text : "-";
                texts(&cursor,v->label);
            }
        } else if (strcmp(kind, "preset") == 0 && section >= 0) {
            CarafeSection* s = &catalog->sections[section];
            if (s->preset_count < MAX_PRESETS) {
                CarafePreset* p = &s->presets[s->preset_count];
                p->id = nextField(&cursor);
                texts(&cursor,p->name);
                texts(&cursor,p->hint);
                p->sets = calloc(MAX_SETS, sizeof(CarafeSet));
                if (p->sets == NULL) {
                    return false;
                }
                last_preset = s->preset_count++;
            }
        } else if (strcmp(kind, "set") == 0 && section >= 0 && last_preset >= 0) {
            CarafePreset* p = &catalog->sections[section].presets[last_preset];
            const char* id = nextField(&cursor);
            const char* value = nextField(&cursor);
            int setting = id != NULL ? carafeCatalogFind(catalog, id) : -1;
            if (setting >= 0 && value != NULL && p->set_count < MAX_SETS) {
                p->sets[p->set_count].setting = setting;
                p->sets[p->set_count].value = atoi(value);
                p->set_count++;
            }
        } else if (strcmp(kind, "suggest") == 0 && catalog->suggest_count < MAX_SUGGESTS) {
            const char* which = nextField(&cursor);
            const char* id = nextField(&cursor);
            const char* value = nextField(&cursor);
            int setting = id != NULL ? carafeCatalogFind(catalog, id) : -1;
            if (which != NULL && setting >= 0 && value != NULL) {
                CarafeSuggest* s = &catalog->suggests[catalog->suggest_count++];
                s->kind = failureFromName(which);
                s->setting = setting;
                s->value = atoi(value);
                texts(&cursor,s->why);
            }
        } else if ((strcmp(kind, "key") == 0 && catalog->key_count < MAX_KEYS)
            || (strcmp(kind, "pad") == 0 && catalog->pad_count < MAX_PADS)) {
            bool pad = kind[0] == 'p';
            CarafeKey* k = pad ? &catalog->pads[catalog->pad_count++] : &catalog->keys[catalog->key_count++];
            const char* code = nextField(&cursor);
            k->code = code != NULL ? (unsigned int)strtoul(code, NULL, 0) : 0;
            texts(&cursor,k->label);
        }
    next:
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return catalog->section_count > 0;
}

CarafeCatalog* carafeCatalogLoad(void) {
    size_t size = 0;
    char* data = carafeOverlayReadRomfsFile(MENU_FILE, MENU_LIMIT, &size);
    if (data == NULL) {
        note("[CARAFE] settings menu: no %s in this package", MENU_FILE);
        return NULL;
    }
    CarafeCatalog* catalog = calloc(1, sizeof(CarafeCatalog));
    PendingWhen* pending = calloc(MAX_SETTINGS, sizeof(PendingWhen));
    char** group_when = calloc(MAX_GROUPS, sizeof(char*));
    char* text = realloc(data, size + 1);
    if (catalog == NULL || pending == NULL || group_when == NULL || text == NULL) {
        free(catalog);
        free(pending);
        free(group_when);
        free(text != NULL ? text : data);
        return NULL;
    }
    text[size] = '\0';
    catalog->text = text;
    catalog->sections = calloc(MAX_SECTIONS, sizeof(CarafeSection));
    catalog->settings = calloc(MAX_SETTINGS, sizeof(CarafeSetting));
    catalog->groups = calloc(MAX_GROUPS, sizeof(CarafeGroup));
    catalog->suggests = calloc(MAX_SUGGESTS, sizeof(CarafeSuggest));
    catalog->keys = calloc(MAX_KEYS, sizeof(CarafeKey));
    catalog->pads = calloc(MAX_PADS, sizeof(CarafeKey));
    bool parsed = catalog->sections != NULL && catalog->settings != NULL && catalog->groups != NULL
        && catalog->suggests != NULL && catalog->keys != NULL && catalog->pads != NULL
        && parseCatalog(catalog, pending, group_when);
    if (parsed) {
        for (int i = 0; i < catalog->setting_count; i++) {
            parseWhen(catalog, pending[i].when[0], &catalog->settings[i].when);
        }
        for (int i = 0; i < catalog->group_count; i++) {
            parseWhen(catalog, group_when[i], &catalog->groups[i].when);
        }
    }
    free(pending);
    free(group_when);
    if (!parsed) {
        note("[CARAFE] settings menu: %s is not understood", MENU_FILE);
        carafeCatalogFree(catalog);
        return NULL;
    }
    loadChanges();
    for (int f = 0; f < CarafeFileCount; f++) {
        free(g_effective[f]);
        g_effective[f] = effectiveText((CarafeFile)f);
        if (g_effective[f] == NULL) {
            g_effective[f] = romfsText((CarafeFile)f);
        }
    }
    char* package[CarafeFileCount];
    for (int f = 0; f < CarafeFileCount; f++) {
        if (f == CarafeFileKeys) {
            package[f] = readWhole(RUNTIME_ROOT "/config/keys.txt", FILE_LIMIT);
            if (package[f] == NULL) {
                package[f] = romfsText((CarafeFile)f);
            }
        } else {
            package[f] = romfsText((CarafeFile)f);
        }
    }
    for (int i = 0; i < catalog->setting_count; i++) {
        CarafeSetting* setting = &catalog->settings[i];
        char value[CARAFE_VALUE_SIZE] = "";
        bool present = textValue(package[setting->file], setting->key, value, sizeof(value));
        setting->base = valueIndex(setting, value, present);
        settingValue(catalog, setting, (const char* const*)g_effective, true);
        settingValue(catalog, setting, (const char* const*)g_effective, false);
    }
    for (int f = 0; f < CarafeFileCount; f++) {
        free(package[f]);
    }
    loadBindings(catalog, g_effective[CarafeFileKeys]);
    note("[CARAFE] settings menu: %d sections, %d settings, %d changes on the console", catalog->section_count,
        catalog->setting_count, g_change_count);
    return catalog;
}

void carafeCatalogFree(CarafeCatalog* catalog) {
    if (catalog == NULL) {
        return;
    }
    for (int i = 0; catalog->sections != NULL && i < catalog->section_count; i++) {
        for (int p = 0; p < catalog->sections[i].preset_count; p++) {
            free(catalog->sections[i].presets[p].sets);
        }
        free(catalog->sections[i].rows);
        free(catalog->sections[i].presets);
    }
    for (int i = 0; catalog->groups != NULL && i < catalog->group_count; i++) {
        for (int p = 0; p < catalog->groups[i].page_count; p++) {
            free(catalog->groups[i].pages[p].rows);
        }
        free(catalog->groups[i].pages);
    }
    for (int i = 0; catalog->settings != NULL && i < catalog->setting_count; i++) {
        free(catalog->settings[i].values);
    }
    free(catalog->sections);
    free(catalog->settings);
    free(catalog->groups);
    free(catalog->suggests);
    free(catalog->keys);
    free(catalog->pads);
    free(catalog->text);
    free(catalog);
    for (int f = 0; f < CarafeFileCount; f++) {
        free(g_effective[f]);
        g_effective[f] = NULL;
    }
}

bool carafeWhenHolds(const CarafeCatalog* catalog, const CarafeWhen* when, CarafeWhenKind kind) {
    if (when->kind != kind || when->setting < 0) {
        return true;
    }
    int value = catalog->settings[when->setting].current;
    return value >= 0 && value < 32 && (when->values & (1u << value)) != 0;
}

bool carafeSettingVisible(const CarafeCatalog* catalog, const CarafeSetting* setting) {
    if (catalog->api != 0 && setting->api != 0 && (catalog->api & setting->api) == 0) {
        return false;
    }
    return carafeWhenHolds(catalog, &setting->when, CarafeWhenShow);
}

const char* carafeValueLabel(const CarafeSetting* setting) {
    if (setting->current >= 0 && setting->current < setting->value_count) {
        return carafeText(setting->values[setting->current].label);
    }
    return setting->current_text;
}

static void applyKeysLive(void) {
    char* keys = g_effective[CarafeFileKeys];
    char* merged = mergedText(CarafeFileKeys, keys);
    if (merged == NULL) {
        return;
    }
    char path[PATH_SIZE];
    if (filePath(CarafeFileKeys, path, sizeof(path))) {
        for (int i = 0; i < g_change_count; i++) {
            if (g_changes[i].file == CarafeFileKeys) {
                applyOneKeyToDisk(CarafeFileKeys, g_changes[i].key, g_changes[i].value);
            }
        }
    }
    free(g_effective[CarafeFileKeys]);
    g_effective[CarafeFileKeys] = merged;
    for (int i = g_change_count - 1; i >= 0; i--) {
        if (g_changes[i].file == CarafeFileKeys) {
            if (i + 1 < g_change_count) {
                memmove(&g_changes[i], &g_changes[i + 1], (size_t)(g_change_count - i - 1) * sizeof(Change));
            }
            g_change_count--;
        }
    }
    carafeSaveChanges();

    char target[PATH_SIZE];
    if (!gameTarget(target, sizeof(target))) {
        return;
    }
    struct input_profile* profile = calloc(1, sizeof(*profile));
    struct launcher_kv* kv = calloc(1, sizeof(*kv));
    if (profile != NULL && kv != NULL
        && input_profile_program(RUNTIME_ROOT, target, g_swkbd_auto_fallback, profile, kv, path, sizeof(path))) {
        if (wine_nx_input_profile_set != NULL) {
            wine_nx_input_profile_set(profile);
        } else {
            wine_nx_input_profile = *profile;
        }
        __atomic_store_n(&wine_nx_swkbd_auto_enabled, profile->keyboard_auto, __ATOMIC_RELEASE);
        note("[CARAFE] settings menu: controls changed while the game runs");
    }
    free(kv);
    free(profile);
}

static void applyLive(CarafeCatalog* catalog, const CarafeSetting* setting) {
    if (setting->file == CarafeFileKeys) {
        applyKeysLive();
        return;
    }
    if (strcasecmp(setting->key, "frame-limit") == 0) {
        const char* val = setting->current_text[0] == '\0' ? "-" : setting->current_text;
        applyOneKeyToDisk(CarafeFileProgram, setting->key, val);
        removeChange(CarafeFileProgram, setting->key);
        carafeSaveChanges();
        if (wine_nx_graphics_configure != NULL) {
            int vsync = 1;
            int index = carafeCatalogFindKey(catalog, CarafeFileProgram, "vsync");
            if (index >= 0) {
                const CarafeSetting* v = &catalog->settings[index];
                vsync = !(v->start >= 0 && strcmp(v->values[v->start].text, "0") == 0);
            }
            int limit = strcmp(setting->current_text, "") == 0 ? 0 : atoi(setting->current_text);
            wine_nx_graphics_configure(limit, vsync);
            note("[CARAFE] settings menu: frame limit %d while the game runs", limit);
        }
    }
}

bool carafeSettingSet(CarafeCatalog* catalog, int index, int value) {
    if (index < 0 || index >= catalog->setting_count) {
        return false;
    }
    CarafeSetting* setting = &catalog->settings[index];
    if (value < 0 || value >= setting->value_count) {
        return false;
    }
    const char* text = setting->values[value].text;
    if (!recordChange(setting->file, setting->key, text)) {
        return false;
    }
    setting->current = value;
    snprintf(setting->current_text, sizeof(setting->current_text), "%s", strcmp(text, "-") == 0 ? "" : text);
    carafeSaveChanges();
    if (setting->now) {
        applyLive(catalog, setting);
    }
    return true;
}

bool carafeSettingPending(const CarafeSetting* setting) {
    return !setting->now && (setting->current != setting->start
        || (setting->current < 0 && strcmp(setting->current_text, setting->start_text) != 0));
}

int carafePendingCount(const CarafeCatalog* catalog) {
    int count = 0;
    for (int i = 0; i < catalog->setting_count; i++) {
        count += carafeSettingPending(&catalog->settings[i]);
    }
    return count;
}

int carafePresetCurrent(const CarafeCatalog* catalog, const CarafeSection* section) {
    for (int p = 0; p < section->preset_count; p++) {
        const CarafePreset* preset = &section->presets[p];
        bool all = true;
        for (int i = 0; i < preset->set_count && all; i++) {
            all = catalog->settings[preset->sets[i].setting].current == preset->sets[i].value;
        }
        if (all) {
            for (int r = 0; r < section->row_count && all; r++) {
                const CarafeRow* row = &section->rows[r];
                if (row->kind != CarafeRowSetting) {
                    continue;
                }
                const CarafeSetting* s = &catalog->settings[row->index];
                bool listed = false;
                for (int i = 0; i < preset->set_count; i++) {
                    listed |= preset->sets[i].setting == row->index;
                }
                bool counted = false;
                for (int q = 0; q < section->preset_count && !counted; q++) {
                    for (int i = 0; i < section->presets[q].set_count; i++) {
                        counted |= section->presets[q].sets[i].setting == row->index;
                    }
                }
                if (counted && !listed) {
                    all = s->current == (s->default_index >= 0 ? s->default_index : 0);
                }
            }
        }
        if (all) {
            return p;
        }
    }
    return -1;
}

bool carafePresetApply(CarafeCatalog* catalog, const CarafePreset* preset) {
    bool ok = true;
    const CarafeSection* section = NULL;
    for (int s = 0; s < catalog->section_count && section == NULL; s++) {
        for (int p = 0; p < catalog->sections[s].preset_count; p++) {
            if (&catalog->sections[s].presets[p] == preset) {
                section = &catalog->sections[s];
                break;
            }
        }
    }
    if (section != NULL) {
        for (int q = 0; q < section->preset_count; q++) {
            const CarafePreset* other = &section->presets[q];
            for (int i = 0; i < other->set_count; i++) {
                int setting = other->sets[i].setting;
                bool listed = false;
                for (int j = 0; j < preset->set_count; j++) {
                    if (preset->sets[j].setting == setting) {
                        listed = true;
                        break;
                    }
                }
                if (!listed) {
                    const CarafeSetting* st = &catalog->settings[setting];
                    int def = st->default_index >= 0 && st->default_index < st->value_count ? st->default_index : 0;
                    ok &= carafeSettingSet(catalog, setting, def);
                }
            }
        }
    }
    for (int i = 0; i < preset->set_count; i++) {
        ok &= carafeSettingSet(catalog, preset->sets[i].setting, preset->sets[i].value);
    }
    return ok;
}

bool carafeBindingSet(CarafeCatalog* catalog, bool pad, int source, const CarafeChord* chord) {
    if (source < 0 || source >= CARAFE_SOURCES) {
        return false;
    }
    char key[KEY_SIZE];
    char value[CARAFE_VALUE_SIZE];
    snprintf(key, sizeof(key), "%s.%s", pad ? "pad" : "key", carafe_source_names[source]);
    chordText(chord, value, sizeof(value));
    if (!recordChange(CarafeFileKeys, key, value)) {
        return false;
    }
    if (pad) {
        catalog->pad_current[source] = *chord;
    } else {
        catalog->key_current[source] = *chord;
    }
    carafeSaveChanges();
    applyKeysLive();
    return true;
}

void carafeChordLabel(const CarafeCatalog* catalog, bool pad, const CarafeChord* chord, char* out, size_t size) {
    static const char* const NONE[] = {
        "Нет", "None", "无", "なし", "없음", "無", "Aucun", "Aucun", "Keine", "Nessuno", "Ninguno", "Ninguno", "Geen",
        "Nenhum", "Nenhum",
    };
    _Static_assert(sizeof(NONE) / sizeof(NONE[0]) == CarafeLanguageCount, "a word for every language");
    if (chord->count == 0) {
        snprintf(out, size, "%s", NONE[carafeLanguage()]);
        return;
    }
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < chord->count && used < size; i++) {
        const char* label = NULL;
        const CarafeKey* list = pad ? catalog->pads : catalog->keys;
        int count = pad ? catalog->pad_count : catalog->key_count;
        for (int k = 0; k < count && label == NULL; k++) {
            if (list[k].code == chord->code[i]) {
                label = carafeText(list[k].label);
            }
        }
        char fallback[16];
        if (label == NULL) {
            snprintf(fallback, sizeof(fallback), "0x%x", chord->code[i]);
            label = fallback;
        }
        used += (size_t)snprintf(out + used, size - used, "%s%s", i ? " + " : "", label);
    }
}

bool carafeUndoPending(CarafeCatalog* catalog) {
    for (int i = 0; i < catalog->setting_count; i++) {
        CarafeSetting* s = &catalog->settings[i];
        if (!carafeSettingPending(s)) {
            continue;
        }
        removeChange(s->file, s->key);
        s->current = s->start;
        snprintf(s->current_text, sizeof(s->current_text), "%s", s->start_text);
    }
    return carafeSaveChanges();
}

static void stripKeysFromFile(const CarafeCatalog* catalog, const char* path) {
    char* disk = readWhole(path, FILE_LIMIT);
    if (disk == NULL) {
        return;
    }
    for (int i = 0; i < catalog->setting_count; i++) {
        if (catalog->settings[i].file == CarafeFileKeys) {
            char* next = textSet(disk, catalog->settings[i].key, NULL);
            if (next != NULL) {
                free(disk);
                disk = next;
            }
        }
    }
    for (int s = 0; s < CARAFE_SOURCES; s++) {
        char k[KEY_SIZE];
        snprintf(k, sizeof(k), "key.%s", carafe_source_names[s]);
        char* next = textSet(disk, k, NULL);
        if (next != NULL) {
            free(disk);
            disk = next;
        }
        snprintf(k, sizeof(k), "pad.%s", carafe_source_names[s]);
        next = textSet(disk, k, NULL);
        if (next != NULL) {
            free(disk);
            disk = next;
        }
    }
    if (disk[0] == '\0') {
        remove(path);
    } else {
        writeWhole(path, disk);
    }
    free(disk);
}

bool carafeResetAll(CarafeCatalog* catalog, bool live) {
    carafeRegistryReset();
    g_change_count = 0;
    bool ok = carafeSaveChanges();
    char trial[PATH_SIZE];
    volatilePath("trial.txt", trial, sizeof(trial));
    remove(trial);
    CarafeCatalog* owned = NULL;
    if (catalog == NULL) {
        owned = carafeCatalogLoad();
        catalog = owned;
        if (catalog == NULL) {
            return ok;
        }
    }
    for (int f = 0; f < CarafeFileCount; f++) {
        if ((CarafeFile)f == CarafeFileKeys) {
            char path[PATH_SIZE];
            if (filePath(CarafeFileKeys, path, sizeof(path))) {
                stripKeysFromFile(catalog, path);
                char prog_path[PATH_SIZE];
                if (filePath(CarafeFileProgram, prog_path, sizeof(prog_path)) && strcmp(prog_path, path) != 0) {
                    stripKeysFromFile(catalog, prog_path);
                }
            }
        }
        for (int i = 0; i < catalog->setting_count; i++) {
            CarafeSetting* s = &catalog->settings[i];
            if (s->file != (CarafeFile)f) {
                continue;
            }
            int def = (s->base >= 0 && s->base < s->value_count) ? s->base
                : (s->default_index >= 0 && s->default_index < s->value_count ? s->default_index : 0);
            if (s->now) {
                if ((CarafeFile)f != CarafeFileKeys) {
                    applyOneKeyToDisk((CarafeFile)f, s->key, "-");
                }
                s->current = def;
                s->start = def;
                s->current_text[0] = '\0';
                s->start_text[0] = '\0';
            } else {
                recordChange(s->file, s->key, "-");
                s->current = def;
                s->current_text[0] = '\0';
                if (!live) {
                    s->start = def;
                    s->start_text[0] = '\0';
                }
            }
        }
    }
    if (!live && g_change_count > 0) {
        applyPendingArrayToDisk();
        g_change_count = 0;
    }
    carafeSaveChanges();
    free(g_effective[CarafeFileKeys]);
    g_effective[CarafeFileKeys] = effectiveText(CarafeFileKeys);
    loadBindings(catalog, g_effective[CarafeFileKeys]);
    if (live) {
        applyKeysLive();
        if (wine_nx_graphics_configure != NULL) {
            wine_nx_graphics_configure(0, 1);
        }
    }
    if (owned != NULL) {
        carafeCatalogFree(owned);
    }
    return ok;
}

bool carafeRestartRequested(void) {
    return g_restart;
}

static bool hasTrialEntry(const Change* list, int count, CarafeFile file, const char* key) {
    for (int i = 0; i < count; i++) {
        if (list[i].file == file && strcasecmp(list[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

void carafeTrialWrite(bool trial) {
    char path[PATH_SIZE];
    volatilePath("trial.txt", path, sizeof(path));
    if (trial) {
        loadChanges();
        Change* prevs = calloc(MAX_CHANGES, sizeof(Change));
        int prev_count = 0;
        if (prevs != NULL) {
            char* existing = readWhole(path, FILE_LIMIT);
            if (existing != NULL) {
                prev_count = parseChangesText(existing, prevs, MAX_CHANGES);
                free(existing);
            }
            for (int i = 0; i < g_change_count && prev_count < MAX_CHANGES; i++) {
                if (hasTrialEntry(prevs, prev_count, g_changes[i].file, g_changes[i].key)) {
                    continue;
                }
                char prev[CARAFE_VALUE_SIZE] = "";
                const char* src = g_snapshot_disk[g_changes[i].file];
                bool had = src != NULL && textValue(src, g_changes[i].key, prev, sizeof(prev));
                prevs[prev_count].file = g_changes[i].file;
                snprintf(prevs[prev_count].key, sizeof(prevs[prev_count].key), "%s", g_changes[i].key);
                snprintf(prevs[prev_count].value, sizeof(prevs[prev_count].value), "%s",
                    (had && prev[0] != '\0') ? prev : "-");
                prev_count++;
            }
        }
        FILE* file = fopen(path, "wb");
        if (file != NULL) {
            fprintf(file, "carafe-changes\t1\n");
            for (int i = 0; i < prev_count; i++) {
                fprintf(file, "%s\t%s\t%s\n", FILE_NAMES[prevs[i].file], prevs[i].key, prevs[i].value);
            }
            fclose(file);
        }
        free(prevs);
    } else {
        remove(path);
    }
    carafeOverlayCommit("before restarting with new settings");
}

void carafeRequestRestart(bool trial) {
    carafeTrialWrite(trial);
    if (g_change_count > 0) {
        applyPendingArrayToDisk();
        g_change_count = 0;
        carafeSaveChanges();
    }
    g_restart = true;
    if (wine_nx_request_quit != NULL) {
        wine_nx_request_quit("restart with new settings");
    }
}

bool carafeTrialActive(void) {
    return g_trial_active && !__atomic_load_n(&g_trial_done, __ATOMIC_ACQUIRE);
}

bool carafeTrialRead(char* out, size_t size) {
    char path[PATH_SIZE];
    volatilePath("trial.txt", path, sizeof(path));
    char* text = readWhole(path, FILE_LIMIT);
    if (text == NULL) {
        return false;
    }
    snprintf(out, size, "%s", text);
    free(text);
    return true;
}

void carafeTrialClear(void) {
    char path[PATH_SIZE];
    volatilePath("trial.txt", path, sizeof(path));
    remove(path);
    __atomic_store_n(&g_trial_done, true, __ATOMIC_RELEASE);
    carafeRegistryTrialClear();
}

unsigned int carafeFailures(void) {
    char path[PATH_SIZE];
    volatilePath("failures.txt", path, sizeof(path));
    char* text = readWhole(path, 32);
    unsigned int count = text != NULL ? (unsigned int)strtoul(text, NULL, 10) : 0;
    free(text);
    return count;
}

unsigned int carafeFailuresAdd(void) {
    unsigned int count = carafeFailures() + 1;
    char path[PATH_SIZE];
    volatilePath("failures.txt", path, sizeof(path));
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        fprintf(file, "%u\n", count);
        fclose(file);
    }
    return count;
}

void carafeFailuresClear(void) {
    char path[PATH_SIZE];
    volatilePath("failures.txt", path, sizeof(path));
    remove(path);
}

void carafeSettingsTick(void) {
    if (!__atomic_load_n(&g_changes_loaded, __ATOMIC_ACQUIRE)
        || __atomic_load_n(&g_trial_tick_cleared, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&g_tick_lock);
    if (__atomic_load_n(&g_changes_loaded, __ATOMIC_ACQUIRE)
        && !__atomic_load_n(&g_trial_tick_cleared, __ATOMIC_ACQUIRE)
        && armTicksToNs(armGetSystemTick() - g_start_tick) >= (u64)TRIAL_SECONDS * 1000000000ULL) {
        __atomic_store_n(&g_trial_tick_cleared, true, __ATOMIC_RELEASE);
        if (g_trial_active && !__atomic_load_n(&g_trial_done, __ATOMIC_ACQUIRE)) {
            carafeTrialClear();
            note("[CARAFE] settings: the trial run lasted %d s, its settings stay", TRIAL_SECONDS);
        }
        carafeFailuresClear();
    }
    pthread_mutex_unlock(&g_tick_lock);
}
