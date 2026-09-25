#ifndef GRAPHICS_TEST_SWITCH_H
#define GRAPHICS_TEST_SWITCH_H

#include <stddef.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define lstat stat
#endif

#define SHA256_HASH_SIZE 32
void sha256CalculateHash( void *dst, const void *src, size_t size );

#endif
