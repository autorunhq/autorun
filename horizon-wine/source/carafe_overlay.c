#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "carafe_overlay.h"

extern void wine_nx_runtime_trace(const char* msg) __attribute__((weak));

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

__attribute__((weak)) uint64_t carafeOverlayBytesRead(void) {
    return 0;
}

__attribute__((weak)) bool carafeOverlayGameOpened(void) {
    return false;
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
    if (written <= 0 || (size_t)written >= sizeof(romfs_path)) {
        return NULL;
    }
    FILE* file = fopen(romfs_path, "rb");
    if (file == NULL && strcmp(device, "romfs") != 0) {
        written = snprintf(romfs_path, sizeof(romfs_path), "romfs:/%s", rel);
        if (written > 0 && (size_t)written < sizeof(romfs_path)) {
            file = fopen(romfs_path, "rb");
        }
    }
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
}
