#ifndef WINE_NX_SHADER_ALLOC_CACHE_H
#define WINE_NX_SHADER_ALLOC_CACHE_H

#include <stddef.h>

extern __thread unsigned int wine_nx_shader_alloc_active;

void wine_nx_shader_alloc_begin(void);
void wine_nx_shader_alloc_end(void);
void *wine_nx_shader_alloc_take(size_t size, size_t alignment);
int wine_nx_shader_alloc_put(void *pointer, size_t usable);
int wine_nx_shader_alloc_trim(void);

#endif
