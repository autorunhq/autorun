#!/usr/bin/env python3
"""Exercise the WoW64 OpenGL copy-buffer map, flush and unmap paths."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/opengl32/unix_wgl.c').read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __SWITCH__ 1
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define FIXME(...) ((void)0)
#define ERR(...) ((void)0)
#define GL_MAP_READ_BIT 1
#define GL_MAP_WRITE_BIT 2
#define GL_MAP_INVALIDATE_RANGE_BIT 4
#define GL_MAP_INVALIDATE_BUFFER_BIT 8
#define GL_MAP_FLUSH_EXPLICIT_BIT 16
#define GL_MAP_PERSISTENT_BIT 64
#define GL_INVALID_OPERATION 0x502
#define GL_INVALID_VALUE 0x501
#define GL_BUFFER_SIZE 0x8764
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE 6
#define PtrToUlong(p) ((uint32_t)(uintptr_t)(p))
#define ULongToPtr(p) ((void *)(uintptr_t)(p))
typedef int BOOL, GLint, VkResult;
typedef unsigned int GLuint, GLenum, GLbitfield;
typedef intptr_t GLintptr;
typedef struct { int sType; void *memory; size_t offset, size; } VkMappedMemoryRange;
struct opengl_funcs {
    void (*p_glGetNamedBufferParameteriv)(GLuint, GLenum, GLint *);
    void (*p_glGetBufferParameteriv)(GLenum, GLenum, GLint *);
    void (*p_glUnmapNamedBuffer)(GLuint);
    void (*p_glUnmapBuffer)(GLenum);
};
typedef struct { const struct opengl_funcs *glTable; } TEB;
struct vk_device { void *vk_device; VkResult (*p_vkFlushMappedMemoryRanges)(void *, int, VkMappedMemoryRange *); };
struct buffer {
    GLuint name;
    size_t size, copy_length, vm_size, map_length;
    void *host_ptr, *map_ptr, *vm_ptr, *vk_memory;
    BOOL pinned, explicit_flush, copy_on_flush;
    GLbitfield map_access;
    struct vk_device *vk_device;
};
static unsigned long long wine_nx_gl_copy_bytes;
static unsigned int wine_nx_gl_explicit_flushes, wine_nx_gl_persistent_failures;
static int last_error, fail_alloc, native_unmaps, clean_calls;
static struct buffer *registered;
static void set_gl_error(TEB *teb, int err) { (void)teb; last_error = err; }
static void get_size(GLuint name, GLenum pname, GLint *size) { (void)name; (void)pname; *size = 64; }
static void native_unmap(GLuint name) { (void)name; native_unmaps++; }
static GLuint get_target_name(TEB *teb, GLenum target) { (void)teb; (void)target; return 1; }
static struct buffer *set_named_buffer_storage(TEB *teb, GLuint name, struct buffer *buffer)
{ (void)teb; (void)name; registered = buffer; return NULL; }
static int buffer_vm_alloc(TEB *teb, struct buffer *buffer, size_t size)
{
    (void)teb;
    if (fail_alloc) return 0;
    if (buffer->vm_size >= size) return 1;
    buffer->vm_ptr = realloc(buffer->vm_ptr, size);
    assert(buffer->vm_ptr);
    memset(buffer->vm_ptr, 0x5a, size);
    buffer->vm_size = size;
    return 1;
}
static void wine_nx_nouveau_cpu_clean_range(void *ptr, size_t size) { (void)ptr; (void)size; clean_calls++; }
static void unmap_vk_buffer(struct buffer *buffer) { (void)buffer; abort(); }
'''
fixture += block('static void flush_buffer(')
mapping = block('static void *wow64_map_buffer(')
# Native and Vulkan direct mappings bypass the copy-buffer fallback tested here.
fixture += mapping[:mapping.index('    if (buffer && buffer->map_ptr)')]
fixture += mapping[mapping.index('    if (!ptr) return NULL;'):]
fixture += block('static BOOL wow64_unmap_buffer(')
fixture += r'''
int main(void)
{
    const struct opengl_funcs funcs = {get_size, get_size, native_unmap, native_unmap};
    TEB teb = {&funcs};
    unsigned char *host = malloc(128), *mapped;
    assert(host && (uintptr_t)host > UINT32_MAX);
    struct buffer buffer = {.size = 64};
    memset(host, 0x33, 128);
    mapped = wow64_map_buffer(&teb, &buffer, 1, 0, 3, 64,
                             GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT, host + 3);
    assert(mapped && mapped == (unsigned char *)buffer.vm_ptr + 3);
    assert(buffer.copy_on_flush && !buffer.copy_length && buffer.map_length == 64);
    assert(!wine_nx_gl_copy_bytes);
    for (int i = 0; i < 64; i++) assert(mapped[i] == 0x5a);
    memset(mapped + 11, 0x44, 17);
    flush_buffer(&teb, &buffer, 11, 17);
    for (int i = 0; i < 128; i++) assert(host[i] == (i >= 14 && i < 31 ? 0x44 : 0x33));
    assert(wine_nx_gl_copy_bytes == 17);
    flush_buffer(&teb, &buffer, 60, 10); assert(last_error == GL_INVALID_VALUE);
    flush_buffer(&teb, &buffer, (size_t)-1, 1); assert(last_error == GL_INVALID_VALUE);
    flush_buffer(&teb, &buffer, 0, (size_t)-1); assert(last_error == GL_INVALID_VALUE);
    last_error = 0;
    flush_buffer(&teb, &buffer, 64, 0);
    assert(!last_error && wine_nx_gl_copy_bytes == 17);
    memset(mapped, 0x77, 64);
    assert(wow64_unmap_buffer(&teb, &buffer));
    assert(!buffer.copy_on_flush && !buffer.map_ptr && !buffer.host_ptr);
    assert(wine_nx_gl_copy_bytes == 17);
    for (int i = 0; i < 128; i++) assert(host[i] == (i >= 14 && i < 31 ? 0x44 : 0x33));
    mapped = wow64_map_buffer(&teb, &buffer, 1, 0, 0, 64, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT, host);
    assert(mapped && !buffer.copy_on_flush && buffer.copy_length == 64);
    assert(!memcmp(mapped, host, 64));
    mapped[0] = 0x99;
    assert(wow64_unmap_buffer(&teb, &buffer) && host[0] == 0x99);
    assert(wine_nx_gl_copy_bytes == 145);
    mapped = wow64_map_buffer(&teb, &buffer, 1, 0, 0, 0, GL_MAP_READ_BIT, host);
    assert(mapped && !buffer.copy_on_flush && !buffer.copy_length);
    assert(!memcmp(mapped, host, 64));
    assert(wow64_unmap_buffer(&teb, &buffer));
    fail_alloc = 1;
    assert(!wow64_map_buffer(&teb, &buffer, 1, 0, 0, 64,
                            GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT, host));
    assert(native_unmaps == 1 && !buffer.copy_on_flush);
    fail_alloc = 0;
    mapped = wow64_map_buffer(&teb, NULL, 0, 2, 0, 32,
                             GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_INVALIDATE_RANGE_BIT, host);
    assert(mapped && registered && registered->copy_on_flush);
    mapped[31] = 0x88;
    flush_buffer(&teb, registered, 31, 1);
    assert(host[31] == 0x88 && wow64_unmap_buffer(&teb, registered));
    free(registered->vm_ptr); free(registered);
    buffer.pinned = buffer.explicit_flush = 1;
    buffer.map_ptr = host;
    buffer.map_length = 64;
    buffer.map_access = GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
    flush_buffer(&teb, &buffer, 1, 3);
    assert(clean_calls == 1 && wine_nx_gl_explicit_flushes == 1);
    free(buffer.vm_ptr); free(host);
    puts("WoW64 GL buffers: partial flush, offsets, unmap, reuse, read/write and pinned paths passed");
}
'''
for name in ('void wow64_glFlushMappedBufferRange(', 'void wow64_glFlushMappedNamedBufferRange('):
    body = block(name)
    assert body.index('flush_buffer(') < body.index('use_driver_buffer_map(')
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'buffer.c'
    exe = Path(tmp) / 'buffer'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
