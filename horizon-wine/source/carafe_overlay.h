#ifndef CARAFE_OVERLAY_H
#define CARAFE_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t carafeOverlayBytesRead(void);
bool carafeOverlayGameOpened(void);
bool carafeOverlayReadRomfsText(const char* path, char* out, size_t size);
void* carafeOverlayReadRomfsFile(const char* path, size_t limit, size_t* size);
void carafeOverlayLog(const char* line);
void carafeOverlayCommit(const char* reason);

/* Autorun multi-game context hooks (weak so standalone host tests can mock carafe_overlay.c). */
void carafeAutorunSetProgram(const char* target, const char* own_nro, bool forwarded, bool reopen_launcher)
    __attribute__((weak));
void carafeAutorunMarkGameOpened(void) __attribute__((weak));
bool carafeOverlayProgramDir(char* out, size_t size) __attribute__((weak));
bool carafeOverlayProgramTarget(char* out, size_t size) __attribute__((weak));
bool carafeOverlayOwnNro(char* out, size_t size) __attribute__((weak));
bool carafeOverlayIsForwarded(void) __attribute__((weak));
bool carafeOverlayReopenLauncher(void) __attribute__((weak));
void carafeOverlaySetMeta(const char* target, const char* title, const char* icon_path, const char* own_nro,
    bool forwarded, bool reopen_launcher) __attribute__((weak));
void carafeOverlayGetMeta(char* target, size_t target_size, char* title, size_t title_size, char* icon_path,
    size_t icon_size, char* own_nro, size_t own_size, bool* forwarded, bool* reopen_launcher)
    __attribute__((weak));

#endif
