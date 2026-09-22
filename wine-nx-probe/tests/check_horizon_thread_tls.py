#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
start = source.index('static Result check_thread_local_range(')
end = source.index('\nstatic void *horizon_mmap_fixed(', start)
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wine/rbtree.h"
typedef uint64_t u64;
typedef uint32_t u32, Handle, Result;
typedef int32_t s32;
typedef void (*ThreadFunc)(void*);
typedef struct { u64 addr, size; u32 type; } MemoryInfo;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define Module_Kernel 1
#define KernelError_InvalidMemoryState 106
#define KernelError_OutOfMemory 104
#define MAKERESULT(module, code) ((module) | ((code) << 9))
#define MemType_ThreadLocal 12
#define SECTION_HOLE 2
struct horizon_mapping {
  void *addr;
  size_t size;
  void *reservation;
  int section_state;
  struct rb_entry entry;
};
static int compare(const void *key, const struct rb_entry *entry) {
  uintptr_t address = (uintptr_t)RB_ENTRY_VALUE(entry, struct horizon_mapping, entry)->addr;
  return (uintptr_t)key < address ? -1 : (uintptr_t)key > address;
}
static struct rb_tree mappings = {compare, NULL};
static struct { char *start, *end; } anchor_regions[2];
static unsigned int anchor_region_count;
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned creates, closes, live, collisions, query_error, create_error, close_error;
static u64 collision_address = 0x2ddf000, tls_address;
static void check_locked(void) { assert(pthread_mutex_trylock(&mapping_mutex) == EBUSY); }
static void entry(void *arg) { (void)arg; assert(0); }
Result __real_svcCreateThread(Handle *handle, ThreadFunc fn, void *arg, void *stack, s32 priority, s32 core) {
  check_locked();
  assert(fn == entry && arg == (void *)1 && stack == (void *)2 && priority == 0x3b && core == -2);
  ++creates;
  if (create_error) return create_error;
  *handle = creates;
  ++live;
  tls_address = collisions ? (--collisions, collision_address) : 0x38000000;
  return 0;
}
static Result svcCloseHandle(Handle handle) {
  check_locked();
  assert(handle && live);
  if (close_error) return close_error;
  ++closes;
  --live;
  tls_address = 0;
  return 0;
}
static Result svcQueryMemory(MemoryInfo *info, u32 *page_info, u64 cursor) {
  check_locked();
  *page_info = 0;
  if (query_error == 1) return 77;
  if (cursor < tls_address) *info = (MemoryInfo){0, tls_address, 0};
  else if (cursor < tls_address + 4096) *info = (MemoryInfo){tls_address, 4096, MemType_ThreadLocal};
  else *info = (MemoryInfo){tls_address + 4096, UINT64_MAX - tls_address - 4096, 0};
  if (query_error == 2) info->size = 0;
  if (query_error == 3) info->addr = cursor + 4096;
  if (query_error == 4) *info = (MemoryInfo){UINT64_MAX - 1, 4096, 0};
  if (query_error == 5) *info = (MemoryInfo){0, cursor, 0};
  return 0;
}
static void wine_nx_runtime_trace(const char *message) { assert(strstr(message, "[TLS]")); }
'''
tests = r'''
static Result create(Handle *handle) {
  return __wrap_svcCreateThread(handle, entry, (void *)1, (void *)2, 0x3b, -2);
}
static void close_thread(Handle handle) {
  assert(!pthread_mutex_lock(&mapping_mutex));
  assert(!svcCloseHandle(handle));
  assert(!pthread_mutex_unlock(&mapping_mutex));
}
int main(void) {
  Handle handle = 0;
  assert(!create(&handle));
  close_thread(handle);
  struct horizon_mapping low = {.addr=(void *)0x2c20000, .size=0x400000, .reservation=(void *)1};
  struct horizon_mapping high = {.addr=(void *)0x800000000ULL, .size=0x400000, .section_state=SECTION_HOLE};
  rb_put(&mappings, low.addr, &low.entry);
  rb_put(&mappings, high.addr, &high.entry);
  anchor_regions[0].start = (void *)0x2010c000;
  anchor_regions[0].end = (void *)0x2210c000;
  anchor_region_count = 1;
  collision_address = 0x214b3000;
  collisions = 1;
  unsigned anchor_creates = creates;
  assert(!create(&handle) && creates == anchor_creates + 2);
  close_thread(handle);
  anchor_regions[1].start = (void *)0x900000000ULL;
  anchor_regions[1].end = (void *)0x902000000ULL;
  anchor_region_count = 2;
  for (unsigned i = 0; i < 2; ++i) {
    collision_address = (uintptr_t)anchor_regions[i].end - 4096;
    collisions = 1;
    anchor_creates = creates;
    assert(!create(&handle) && creates == anchor_creates + 2);
    close_thread(handle);
  }
  anchor_region_count = 0;
  collision_address = 0x2ddf000;
  collisions = 3;
  unsigned before = creates, closed_before = closes;
  assert(!create(&handle));
  assert(creates == before + 4 && closes == closed_before + 3 && live == 1 && tls_address == 0x38000000);
  close_thread(handle);
  collision_address = 0x800003000ULL;
  collisions = 2;
  assert(!create(&handle));
  close_thread(handle);
  low.reservation = NULL;
  collision_address = (uintptr_t)low.addr;
  collisions = 1;
  before = creates;
  assert(!create(&handle) && creates == before + 1);
  close_thread(handle);
  low.reservation = (void *)1;
  for (unsigned i = 0; i < 2; ++i) {
    collision_address = (uintptr_t)low.addr + (i ? low.size - 4096 : 0);
    collisions = 1;
    before = creates;
    assert(!create(&handle) && creates == before + 2);
    close_thread(handle);
  }
  collision_address = (uintptr_t)low.addr + low.size;
  collisions = 1;
  before = creates;
  assert(!create(&handle) && creates == before + 1);
  close_thread(handle);
  handle = 0xfeed;
  create_error = 88;
  assert(create(&handle) == 88 && handle == 0xfeed && !live);
  create_error = 0;
  for (query_error = 1; query_error <= 5; ++query_error) {
    assert(create(&handle) == (query_error == 1 ? 77 : MAKERESULT(Module_Kernel, KernelError_InvalidMemoryState)));
    assert(handle == 0xfeed && !live);
  }
  query_error = 0;
  collision_address = (uintptr_t)low.addr + 4096;
  collisions = 256;
  before = creates;
  assert(create(&handle) == MAKERESULT(Module_Kernel, KernelError_OutOfMemory));
  assert(creates == before + 256 && !live && creates == closes + 1 && handle == 0xfeed);
  close_error = 99;
  collisions = 1;
  before = creates;
  assert(create(&handle) == 99 && creates == before + 1 && live == 1 && handle == 0xfeed);
  close_error = 0;
  close_thread(creates);
  assert(!pthread_mutex_trylock(&mapping_mutex));
  assert(!pthread_mutex_unlock(&mapping_mutex));
  puts("Horizon thread TLS: guest reservations, unused anchor arenas, section holes, 64-bit addresses and failure cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='horizon-thread-tls-') as directory:
    path = Path(directory)
    (path / 'tls.c').write_text(fixture + source[start:end] + tests)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-I', str(root / 'include'),
                    str(path / 'tls.c'), '-o', str(path / 'tls')], check=True)
    subprocess.run([str(path / 'tls')], check=True)
