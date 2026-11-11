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

#endif
