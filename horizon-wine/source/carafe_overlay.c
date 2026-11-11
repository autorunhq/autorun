#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "carafe_overlay.h"
#include "launcher_catalog.h"
#include "launcher_settings.h"

#define CARAFE_ROOT_DIR "sdmc:/switch/wine/carafe"
#define RUNTIME_ROOT "sdmc:/switch/wine"
#define CATALOG_PATH RUNTIME_ROOT "/" LAUNCHER_CATALOG_FILE

extern void wine_nx_runtime_trace(const char* msg) __attribute__((weak));
extern void wine_nx_runtime_log_flush(void) __attribute__((weak));
extern unsigned long long wine_nx_sd_bytes __attribute__((weak));
extern void carafeSettingsSyncProgram(void) __attribute__((weak));
extern void carafeRegistrySyncProgram(void) __attribute__((weak));

extern const unsigned char wine_nx_icon_any[] __attribute__((weak));
extern const size_t wine_nx_icon_any_size __attribute__((weak));
extern const unsigned char carafe_font_regular[] __attribute__((weak));
extern const size_t carafe_font_regular_size __attribute__((weak));
extern const unsigned char carafe_font_bold[] __attribute__((weak));
extern const size_t carafe_font_bold_size __attribute__((weak));
extern const unsigned char carafe_font_extrabold[] __attribute__((weak));
extern const size_t carafe_font_extrabold_size __attribute__((weak));
extern const unsigned char carafe_menu_txt[] __attribute__((weak));
extern const size_t carafe_menu_txt_size __attribute__((weak));

static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_target[512];
static char g_title[128];
static char g_icon_path[512];
static char g_own_nro[512];
static char g_program_hash[32] = "default";
static bool g_forwarded;
static bool g_reopen_launcher = true;
static bool g_game_opened;
static uint64_t g_bytes_base;

#ifdef __SWITCH__
static pthread_once_t g_romfs_once = PTHREAD_ONCE_INIT;
static bool g_romfs_mounted;

static void mountRomfsOnce(void) {
    FsStorage storage;
    if (R_SUCCEEDED(fsOpenDataStorageByCurrentProcess(&storage))) {
        if (R_SUCCEEDED(romfsMountFromStorage(storage, 0, "carafe-ro"))) {
            g_romfs_mounted = true;
        } else {
            fsStorageClose(&storage);
        }
    }
}
#endif

static void ensureDirTree(const char* dir) {
    char buf[512];
    size_t len = strlen(dir);
    if (len == 0 || len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, dir, len + 1);
    for (char* p = buf; *p != '\0'; p++) {
        if (*p == '/' && p != buf && p[-1] != ':') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static uint64_t hashTarget64(const char* target) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char* p = (const unsigned char*)target; *p != '\0'; p++) {
        unsigned char c = *p == '\\' ? '/' : (unsigned char)tolower(*p);
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void titleFromPath(const char* target, char* out, size_t size) {
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (target == NULL || target[0] == '\0') {
        return;
    }
    const char* slash = strrchr(target, '/');
    const char* back = strrchr(target, '\\');
    const char* base = slash && (!back || slash > back) ? slash + 1 : back ? back + 1 : target;
    snprintf(out, size, "%s", base);
    size_t len = strlen(out);
    if (len > 4 && strcasecmp(out + len - 4, ".exe") == 0) {
        out[len - 4] = '\0';
    }
}

static bool fileReadable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void probeIconSidecars(const char* target, char* out, size_t size) {
    static const char* const NAMES[] = { "icon.png", "icon.jpg", "cover.png", "cover.jpg" };
    if (target == NULL || target[0] == '\0' || size == 0) {
        return;
    }
    out[0] = '\0';
    const char* slash = strrchr(target, '/');
    if (slash == NULL) {
        return;
    }
    size_t dir_len = (size_t)(slash + 1 - target);
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        char candidate[512];
        if (dir_len + strlen(NAMES[i]) >= sizeof(candidate)) {
            continue;
        }
        int n = snprintf(candidate, sizeof(candidate), "%.*s%s", (int)dir_len, target, NAMES[i]);
        if (n > 0 && (size_t)n < sizeof(candidate) && fileReadable(candidate)) {
            snprintf(out, size, "%s", candidate);
            return;
        }
    }
}

static void resolveGameMetadata(const char* target, char* title_out, size_t title_size, char* icon_out, size_t icon_size) {
    title_out[0] = '\0';
    icon_out[0] = '\0';
    if (target == NULL || target[0] == '\0') {
        return;
    }
    char settings_path[520];
    if (launcher_program_settings_path(RUNTIME_ROOT, target, settings_path, sizeof(settings_path))) {
        struct launcher_kv* kv = calloc(1, sizeof(*kv));
        if (kv != NULL && launcher_kv_load(kv, settings_path)) {
            launcher_kv_get(kv, "title", title_out, title_size);
        }
        free(kv);
    }
    struct launcher_catalog* catalog = calloc(1, sizeof(*catalog));
    if (catalog != NULL) {
        if (launcher_catalog_load(catalog, CATALOG_PATH) == LAUNCHER_CATALOG_OK) {
            int idx = launcher_catalog_find(catalog, target);
            if (idx >= 0) {
                const struct launcher_catalog_entry* e = &catalog->entries[idx];
                if (title_out[0] == '\0' && e->title[0] != '\0') {
                    snprintf(title_out, title_size, "%s", e->title);
                }
                if (e->square_art[0] != '\0' && fileReadable(e->square_art)) {
                    snprintf(icon_out, icon_size, "%s", e->square_art);
                } else if (e->portrait_art[0] != '\0' && fileReadable(e->portrait_art)) {
                    snprintf(icon_out, icon_size, "%s", e->portrait_art);
                }
            }
        }
        free(catalog);
    }
    if (title_out[0] == '\0') {
        titleFromPath(target, title_out, title_size);
    }
    if (icon_out[0] == '\0') {
        probeIconSidecars(target, icon_out, icon_size);
    }
}

static void* readDiskFile(const char* path, size_t limit, size_t* size) {
    if (size != NULL) {
        *size = 0;
    }
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long len = ftell(file);
    if (len < 0 || (size_t)len > limit || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size_t alloc = (size_t)len > 0 ? (size_t)len : 1;
    void* buf = malloc(alloc);
    if (buf == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = len > 0 ? fread(buf, 1, (size_t)len, file) : 0;
    fclose(file);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    if (size != NULL) {
        *size = got;
    }
    return buf;
}

static void* copyBuffer(const void* data, size_t len, size_t limit, size_t* size) {
    if (data == NULL || len == 0 || len > limit) {
        return NULL;
    }
    void* out = malloc(len);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, data, len);
    if (size != NULL) {
        *size = len;
    }
    return out;
}

void carafeAutorunSetProgram(const char* target, const char* own_nro, bool forwarded, bool reopen_launcher) {
    char resolved_title[128] = "";
    char resolved_icon[512] = "";
    if (target != NULL && target[0] != '\0') {
        resolveGameMetadata(target, resolved_title, sizeof(resolved_title), resolved_icon, sizeof(resolved_icon));
    }

    pthread_mutex_lock(&g_meta_lock);
    snprintf(g_target, sizeof(g_target), "%s", target != NULL ? target : "");
    snprintf(g_title, sizeof(g_title), "%s", resolved_title);
    snprintf(g_icon_path, sizeof(g_icon_path), "%s", resolved_icon);
    snprintf(g_own_nro, sizeof(g_own_nro), "%s", own_nro != NULL ? own_nro : "");
    g_forwarded = forwarded;
    g_reopen_launcher = reopen_launcher;
    if (g_target[0] != '\0') {
        snprintf(g_program_hash, sizeof(g_program_hash), "%016llx",
            (unsigned long long)hashTarget64(g_target));
    } else {
        snprintf(g_program_hash, sizeof(g_program_hash), "default");
    }
    if (&wine_nx_sd_bytes != NULL) {
        g_bytes_base = __atomic_load_n(&wine_nx_sd_bytes, __ATOMIC_RELAXED);
    }
    g_game_opened = g_target[0] != '\0';
    pthread_mutex_unlock(&g_meta_lock);

    char dir[512];
    if (carafeOverlayProgramDir(dir, sizeof(dir))) {
        ensureDirTree(dir);
    }
    if (carafeSettingsSyncProgram != NULL) {
        carafeSettingsSyncProgram();
    }
    if (carafeRegistrySyncProgram != NULL) {
        carafeRegistrySyncProgram();
    }
}

void carafeAutorunMarkGameOpened(void) {
    pthread_mutex_lock(&g_meta_lock);
    g_game_opened = true;
    pthread_mutex_unlock(&g_meta_lock);
}

void carafeOverlaySetMeta(const char* target, const char* title, const char* icon_path, const char* own_nro,
    bool forwarded, bool reopen_launcher) {
    pthread_mutex_lock(&g_meta_lock);
    snprintf(g_target, sizeof(g_target), "%s", target != NULL ? target : "");
    snprintf(g_title, sizeof(g_title), "%s", title != NULL ? title : "");
    snprintf(g_icon_path, sizeof(g_icon_path), "%s", icon_path != NULL ? icon_path : "");
    snprintf(g_own_nro, sizeof(g_own_nro), "%s", own_nro != NULL ? own_nro : "");
    g_forwarded = forwarded;
    g_reopen_launcher = reopen_launcher;
    if (g_target[0] != '\0') {
        snprintf(g_program_hash, sizeof(g_program_hash), "%016llx",
            (unsigned long long)hashTarget64(g_target));
        if (g_title[0] == '\0') {
            titleFromPath(g_target, g_title, sizeof(g_title));
        }
    } else {
        snprintf(g_program_hash, sizeof(g_program_hash), "default");
    }
    pthread_mutex_unlock(&g_meta_lock);

    char dir[512];
    if (carafeOverlayProgramDir(dir, sizeof(dir))) {
        ensureDirTree(dir);
    }
}

void carafeOverlayGetMeta(char* target, size_t target_size, char* title, size_t title_size, char* icon_path,
    size_t icon_size, char* own_nro, size_t own_size, bool* forwarded, bool* reopen_launcher) {
    pthread_mutex_lock(&g_meta_lock);
    if (target != NULL && target_size > 0) {
        snprintf(target, target_size, "%s", g_target);
    }
    if (title != NULL && title_size > 0) {
        snprintf(title, title_size, "%s", g_title);
    }
    if (icon_path != NULL && icon_size > 0) {
        snprintf(icon_path, icon_size, "%s", g_icon_path);
    }
    if (own_nro != NULL && own_size > 0) {
        snprintf(own_nro, own_size, "%s", g_own_nro);
    }
    if (forwarded != NULL) {
        *forwarded = g_forwarded;
    }
    if (reopen_launcher != NULL) {
        *reopen_launcher = g_reopen_launcher;
    }
    pthread_mutex_unlock(&g_meta_lock);
}

bool carafeOverlayProgramDir(char* out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    pthread_mutex_lock(&g_meta_lock);
    int n = snprintf(out, size, "%s/%s", CARAFE_ROOT_DIR, g_program_hash);
    pthread_mutex_unlock(&g_meta_lock);
    if (n <= 0 || (size_t)n >= size) {
        return false;
    }
    ensureDirTree(out);
    return true;
}

bool carafeOverlayProgramTarget(char* out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    pthread_mutex_lock(&g_meta_lock);
    snprintf(out, size, "%s", g_target);
    pthread_mutex_unlock(&g_meta_lock);
    return out[0] != '\0';
}

bool carafeOverlayOwnNro(char* out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    pthread_mutex_lock(&g_meta_lock);
    snprintf(out, size, "%s", g_own_nro);
    pthread_mutex_unlock(&g_meta_lock);
    return out[0] != '\0';
}

bool carafeOverlayIsForwarded(void) {
    pthread_mutex_lock(&g_meta_lock);
    bool value = g_forwarded;
    pthread_mutex_unlock(&g_meta_lock);
    return value;
}

bool carafeOverlayReopenLauncher(void) {
    pthread_mutex_lock(&g_meta_lock);
    bool value = g_reopen_launcher;
    pthread_mutex_unlock(&g_meta_lock);
    return value;
}

__attribute__((weak)) uint64_t carafeOverlayBytesRead(void) {
    if (&wine_nx_sd_bytes == NULL) {
        return 0;
    }
    uint64_t now = __atomic_load_n(&wine_nx_sd_bytes, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_meta_lock);
    uint64_t base = g_bytes_base;
    pthread_mutex_unlock(&g_meta_lock);
    return now > base ? now - base : 0;
}

__attribute__((weak)) bool carafeOverlayGameOpened(void) {
    pthread_mutex_lock(&g_meta_lock);
    bool opened = g_game_opened;
    pthread_mutex_unlock(&g_meta_lock);
    return opened;
}

__attribute__((weak)) void* carafeOverlayReadRomfsFile(const char* path, size_t limit, size_t* size) {
    if (size != NULL) {
        *size = 0;
    }
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    char romfs_path[1024];
    const char* rel = path[0] == '/' ? path + 1 : path;
    const char* device = "romfs";
#ifdef __SWITCH__
    pthread_once(&g_romfs_once, mountRomfsOnce);
    if (g_romfs_mounted) {
        device = "carafe-ro";
    }
#endif
    int written = snprintf(romfs_path, sizeof(romfs_path), "%s:/%s", device, rel);
    if (written > 0 && (size_t)written < sizeof(romfs_path)) {
        void* buf = readDiskFile(romfs_path, limit, size);
        if (buf != NULL) {
            return buf;
        }
    }
    if (strcmp(device, "romfs") != 0) {
        written = snprintf(romfs_path, sizeof(romfs_path), "romfs:/%s", rel);
        if (written > 0 && (size_t)written < sizeof(romfs_path)) {
            void* buf = readDiskFile(romfs_path, limit, size);
            if (buf != NULL) {
                return buf;
            }
        }
    }

    /* Optional SD overrides under sdmc:/switch/wine/carafe/ */
    const char* carafe_rel = strncmp(rel, "carafe/", 7) == 0 ? rel + 7 : rel;
    if (strcmp(path, "/carafe/icon.jpg") != 0 && strcmp(path, "/carafe/title") != 0
        && strcmp(path, "/carafe/argv") != 0) {
        written = snprintf(romfs_path, sizeof(romfs_path), "%s/%s", CARAFE_ROOT_DIR, carafe_rel);
        if (written > 0 && (size_t)written < sizeof(romfs_path)) {
            void* buf = readDiskFile(romfs_path, limit, size);
            if (buf != NULL) {
                return buf;
            }
        }
    }

    /* Autorun dynamic metadata and embedded assets. */
    if (strcmp(path, "/carafe/title") == 0) {
        char title[128];
        pthread_mutex_lock(&g_meta_lock);
        snprintf(title, sizeof(title), "%s", g_title);
        pthread_mutex_unlock(&g_meta_lock);
        if (title[0] != '\0') {
            return copyBuffer(title, strlen(title), limit, size);
        }
        return NULL;
    }
    if (strcmp(path, "/carafe/argv") == 0) {
        char target[512];
        pthread_mutex_lock(&g_meta_lock);
        snprintf(target, sizeof(target), "%s", g_target);
        pthread_mutex_unlock(&g_meta_lock);
        if (target[0] != '\0') {
            char quoted[520];
            int qlen = snprintf(quoted, sizeof(quoted), "\"%s\"", target);
            if (qlen > 0 && (size_t)qlen < sizeof(quoted)) {
                return copyBuffer(quoted, (size_t)qlen, limit, size);
            }
        }
        return NULL;
    }
    if (strcmp(path, "/carafe/icon.jpg") == 0) {
        char icon_path[512];
        pthread_mutex_lock(&g_meta_lock);
        snprintf(icon_path, sizeof(icon_path), "%s", g_icon_path);
        pthread_mutex_unlock(&g_meta_lock);
        if (icon_path[0] != '\0') {
            void* buf = readDiskFile(icon_path, limit, size);
            if (buf != NULL) {
                return buf;
            }
        }
        if (&wine_nx_icon_any_size != NULL && wine_nx_icon_any_size > 0) {
            return copyBuffer(wine_nx_icon_any, wine_nx_icon_any_size, limit, size);
        }
        return NULL;
    }
    if (strcmp(path, "/carafe/fonts/MPLUS1p-Regular.ttf") == 0 && &carafe_font_regular_size != NULL
        && carafe_font_regular_size > 0) {
        return copyBuffer(carafe_font_regular, carafe_font_regular_size, limit, size);
    }
    if (strcmp(path, "/carafe/fonts/MPLUS1p-Bold.ttf") == 0 && &carafe_font_bold_size != NULL
        && carafe_font_bold_size > 0) {
        return copyBuffer(carafe_font_bold, carafe_font_bold_size, limit, size);
    }
    if (strcmp(path, "/carafe/fonts/MPLUS1p-ExtraBold.ttf") == 0 && &carafe_font_extrabold_size != NULL
        && carafe_font_extrabold_size > 0) {
        return copyBuffer(carafe_font_extrabold, carafe_font_extrabold_size, limit, size);
    }
    if (strcmp(path, "/carafe/menu.txt") == 0 && &carafe_menu_txt_size != NULL && carafe_menu_txt_size > 0) {
        return copyBuffer(carafe_menu_txt, carafe_menu_txt_size, limit, size);
    }
    return NULL;
}

__attribute__((weak)) bool carafeOverlayReadRomfsText(const char* path, char* out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';
    size_t got = 0;
    char* data = carafeOverlayReadRomfsFile(path, size - 1, &got);
    if (data == NULL) {
        return false;
    }
    memcpy(out, data, got);
    out[got] = '\0';
    free(data);
    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r' || out[got - 1] == ' ' || out[got - 1] == '\t')) {
        out[--got] = '\0';
    }
    return true;
}

__attribute__((weak)) void carafeOverlayLog(const char* line) {
    if (line != NULL && wine_nx_runtime_trace != NULL) {
        wine_nx_runtime_trace(line);
    }
}

__attribute__((weak)) void carafeOverlayCommit(const char* reason) {
    (void)reason;
    if (wine_nx_runtime_log_flush != NULL) {
        wine_nx_runtime_log_flush();
    }
}
