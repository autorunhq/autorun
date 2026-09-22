#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/win32u/d3dkmt_switch.c').read_text()
code = source[source.index('#define D3DKMT_HANDLE_BIT'):source.index('struct vk_physdev_info')]

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntgdi.h"
#include "wine/server_protocol.h"
#define WINE_NX_MESA_SWITCH 1
struct section { unsigned handles, views; void *data; };
struct handle { struct section *section; };
static struct section *sections[4096];
static pthread_mutex_t section_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint native_releases;
static _Thread_local int allocation_failure = -1, reap_on_allocation;
static void reap_shared_resources(void);
static void *test_calloc(size_t count, size_t size) {
    if (reap_on_allocation) { reap_on_allocation = 0; reap_shared_resources(); }
    if (!allocation_failure) return NULL;
    if (allocation_failure > 0) --allocation_failure;
    return calloc(count, size);
}
static void wine_nx_vk_release_memory_reference(void *reference) {
    assert(reference == (void *)1);
    atomic_fetch_add(&native_releases, 1);
}
static void release_section(struct section *section) {
    if (section->handles || section->views) return;
    for (unsigned i = 0; i < ARRAY_SIZE(sections); ++i)
        if (sections[i] == section) { sections[i] = NULL; break; }
    free(section->data);
    free(section);
}
NTSTATUS WINAPI NtCreateSection(HANDLE *result, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                const LARGE_INTEGER *size, ULONG protect, ULONG flags, HANDLE file) {
    struct section *section = calloc(1, sizeof(*section));
    struct handle *handle = malloc(sizeof(*handle));
    assert(access == SECTION_ALL_ACCESS && !attr && size->QuadPart == 4096);
    assert(protect == PAGE_READWRITE && flags == SEC_COMMIT && !file);
    assert(section && handle && (section->data = calloc(1, 4096)));
    section->handles = 1;
    handle->section = section;
    pthread_mutex_lock(&section_lock);
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(sections) && sections[i]; ++i) {}
    assert(i < ARRAY_SIZE(sections));
    sections[i] = section;
    pthread_mutex_unlock(&section_lock);
    *result = handle;
    return 0;
}
NTSTATUS WINAPI NtMapViewOfSection(HANDLE handle, HANDLE process, void **base, ULONG_PTR bits,
                                  SIZE_T commit, const LARGE_INTEGER *offset, SIZE_T *size,
                                  SECTION_INHERIT inherit, ULONG type, ULONG protect) {
    struct section *section = ((struct handle *)handle)->section;
    assert(process == NtCurrentProcess() && !bits && !commit && !offset);
    assert(*size == 4096 && inherit == ViewUnmap && !type && protect == PAGE_READWRITE);
    pthread_mutex_lock(&section_lock);
    section->views++;
    *base = section->data;
    pthread_mutex_unlock(&section_lock);
    return 0;
}
NTSTATUS WINAPI NtUnmapViewOfSection(HANDLE process, void *base) {
    assert(process == NtCurrentProcess());
    pthread_mutex_lock(&section_lock);
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(sections); ++i) if (sections[i] && sections[i]->data == base) break;
    assert(i < ARRAY_SIZE(sections) && sections[i]->views);
    sections[i]->views--;
    release_section(sections[i]);
    pthread_mutex_unlock(&section_lock);
    return 0;
}
NTSTATUS WINAPI NtClose(HANDLE handle) {
    struct section *section = ((struct handle *)handle)->section;
    pthread_mutex_lock(&section_lock);
    assert(section->handles);
    section->handles--;
    release_section(section);
    pthread_mutex_unlock(&section_lock);
    free(handle);
    return 0;
}
NTSTATUS WINAPI NtCompareObjects(HANDLE a, HANDLE b) {
    return ((struct handle *)a)->section == ((struct handle *)b)->section ? 0 : STATUS_NOT_SAME_OBJECT;
}
NTSTATUS WINAPI NtDuplicateObject(HANDLE process, HANDLE source, HANDLE dest, HANDLE *out,
                                  ACCESS_MASK access, ULONG attributes, ULONG options) {
    struct handle *handle = malloc(sizeof(*handle));
    (void)access; (void)attributes; (void)options;
    assert(process == NtCurrentProcess() && dest == NtCurrentProcess() && handle);
    pthread_mutex_lock(&section_lock);
    handle->section = ((struct handle *)source)->section;
    handle->section->handles++;
    pthread_mutex_unlock(&section_lock);
    *out = handle;
    return 0;
}
NTSTATUS WINAPI NtQueryObject(HANDLE handle, OBJECT_INFORMATION_CLASS cls, void *data, ULONG size, ULONG *ret) {
    OBJECT_BASIC_INFORMATION *info = data;
    assert(cls == ObjectBasicInformation && size == sizeof(*info) && !ret);
    memset(info, 0, size);
    pthread_mutex_lock(&section_lock);
    info->HandleCount = ((struct handle *)handle)->section->handles;
    pthread_mutex_unlock(&section_lock);
    return 0;
}
NTSTATUS WINAPI NtQuerySection(HANDLE handle, SECTION_INFORMATION_CLASS cls, void *data, SIZE_T size, SIZE_T *ret) {
    SECTION_BASIC_INFORMATION *info = data;
    assert(handle && cls == SectionBasicInformation && size == sizeof(*info) && !ret);
    memset(info, 0, size);
    info->Size.QuadPart = 4096;
    return 0;
}
#define calloc test_calloc
'''

tests = r'''
#undef calloc
static void *worker(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 200; ++i) {
        D3DKMT_HANDLE global, local, imported;
        HANDLE exported;
        uint32_t id;
        SIZE_T size;
        reap_on_allocation = 1;
        local = d3dkmt_create_nvmap_resource(8192, 42, (void *)1, &global);
        assert(local && global);
        assert(!NtGdiDdDDIShareObjects(1, &local, NULL, 0, &exported));
        assert(!d3dkmt_destroy_resource(local));
        imported = d3dkmt_open_resource(0, exported, NULL, NULL);
        assert(imported && d3dkmt_resource_get_nvmap(imported, &id, &size));
        assert(id == 42 && size == 8192);
        NtClose(exported);
        assert(!d3dkmt_destroy_resource(imported));
    }
    return NULL;
}
int main(void) {
    D3DKMT_HANDLE global, local, other;
    struct d3dkmt_shared_resource *pin;
    pthread_t threads[6];
    unsigned released;

    local = d3dkmt_create_nvmap_resource(4096, 7, (void *)1, &global);
    assert(local);
    pin = get_shared_resource(global, NULL);
    assert(pin && pin->refs == 2);
    assert(!d3dkmt_destroy_resource(local));
    reap_shared_resources();
    assert(pin->refs == 1 && !native_releases);
    for (int fail = 0; fail < 2; ++fail) {
        allocation_failure = fail;
        assert(!open_local_resource(pin));
        allocation_failure = -1;
        assert(pin->refs == 1 && find_global_locked(global) == pin);
    }
    reap_on_allocation = 1;
    local = open_local_resource(pin);
    assert(local && pin->refs == 2);
    release_shared_resource(pin);
    assert(!d3dkmt_destroy_resource(local) && native_releases == 1);
    assert(!d3dkmt_resources);

    other = d3dkmt_create_nvmap_resource(4096, 8, (void *)1, NULL);
    released = native_releases;
    for (int fail = 1; fail < 3; ++fail) {
        allocation_failure = fail;
        assert(!d3dkmt_create_nvmap_resource(4096, 9, (void *)1, NULL));
        allocation_failure = -1;
        assert(d3dkmt_resources && !d3dkmt_resources->next);
    }
    assert(native_releases == released + 2);
    assert(!d3dkmt_destroy_resource(other) && !d3dkmt_resources);
    released = native_releases;
    for (unsigned i = 0; i < ARRAY_SIZE(threads); ++i) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < ARRAY_SIZE(threads); ++i) assert(!pthread_join(threads[i], NULL));
    reap_shared_resources();
    assert(!d3dkmt_resources && native_releases == released + ARRAY_SIZE(threads) * 200);
    for (unsigned i = 0; i < ARRAY_SIZE(sections); ++i) assert(!sections[i]);
    for (unsigned i = 0; i < d3dkmt_object_count; ++i) assert(!d3dkmt_objects[i]);
    free(d3dkmt_objects);
    puts("D3DKMT: exported handles, pinned lookups, allocation failures and concurrent lifetime passed");
}
'''

with tempfile.TemporaryDirectory(prefix='wine-nx-d3dkmt-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + code + tests)
    for sanitizer in ('address,undefined', 'thread'):
        subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11', '-g', '-O1',
                        '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all', '-pthread', '-D__WINESRC__', '-D_WIN64',
                        '-I' + str(root / 'include'), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=60)
