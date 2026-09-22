#!/usr/bin/env python3
"""Exercise the downstream FEX heap and native allocation routing."""
from pathlib import Path
import os
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('WINE_NX_FEX_DIR', probe / 'toolchains/fex-2609'))
crt = source / 'Source/Windows/Common/CRT'
header = (crt / 'HorizonHeap.h').read_text().replace('#pragma once', '')
for include in ('../Priv.h', 'unixlib.h'):
    header = header.replace(f'#include "{include}"', '')
assert 'ProcessHeap' not in header and 'RtlAllocateHeap' not in header
bootstrap = (crt / 'CRT.cpp').read_text().split('void InitCRTProcess()', 1)[1]
assert bootstrap.index('HorizonInitialize()') < bootstrap.index('RunFuncArray(')
unix = (probe / 'source/fex_unix.c').read_text()
heap_bridge = unix[unix.index('static NTSTATUS heap_allocate('):unix.index('const unixlib_entry_t')]
heap_bridge = heap_bridge.replace('*p = args;', '*p = static_cast<wine_nx_fex_heap*>(args);')
fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
using ULONG = unsigned long;
using ULONG_PTR = uintptr_t;
using NTSTATUS = uint32_t;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_NO_MEMORY = 0xc0000017, STATUS_INVALID_PARAMETER = 0xc000000d;
constexpr ULONG HEAP_ZERO_MEMORY = 8;
static std::atomic<size_t> live {}, largest {};
static thread_local bool fail;
static void* host_malloc(size_t size) {
  if (fail) return nullptr;
  size_t old = largest;
  while (size > old && !largest.compare_exchange_weak(old, size)) {}
  void* p = std::malloc(size);
  if (p) live++;
  return p;
}
static void host_free(void* p) {
  if (p) live--;
  std::free(p);
}
static void* host_realloc(void* p, size_t size) {
  if (fail) return nullptr;
  void* result = std::realloc(p, size);
  if (result && !p) live++;
  return result;
}
static void* host_calloc(size_t count, size_t size) {
  void* result = host_malloc(count * size);
  if (result) std::memset(result, 0, count * size);
  return result;
}
#define malloc host_malloc
#define realloc host_realloc
#define calloc host_calloc
#define free host_free
'''
heap_dispatch = r'''
#undef malloc
#undef realloc
#undef calloc
#undef free
namespace FEX::Windows::HorizonHeap {
NTSTATUS Call(unsigned op, void* args) {
  if (op == WINE_NX_FEX_HEAP_ALLOC) return heap_allocate(args);
  if (op == WINE_NX_FEX_HEAP_REALLOC) return heap_reallocate(args);
  assert(op == WINE_NX_FEX_HEAP_FREE);
  return heap_free(args);
}
}
'''
tests = r'''
namespace Heap = FEX::Windows::HorizonHeap;
template<typename T> struct Allocator {
  using value_type = T;
  T* allocate(size_t count) { return static_cast<T*>(Heap::Allocate(count * sizeof(T), alignof(T))); }
  void deallocate(T* p, size_t) { Heap::Free(p); }
};
int main() {
  assert(heap_allocate(nullptr) == STATUS_INVALID_PARAMETER);
  assert(heap_reallocate(nullptr) == STATUS_INVALID_PARAMETER);
  assert(heap_free(nullptr) == STATUS_INVALID_PARAMETER);
  wine_nx_fex_heap args {0, 0, 0};
  assert(heap_allocate(&args) == STATUS_INVALID_PARAMETER);
  args = {32, 0, 2};
  assert(heap_allocate(&args) == STATUS_INVALID_PARAMETER);
  assert(heap_reallocate(&args) == STATUS_INVALID_PARAMETER);
  using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;
  {
    String home("C:\\users\\switch\\AppData\\Local");
    home += "/.local/share/fex-emu/";
    assert(home.find("AppData") != String::npos);
  }
  assert(largest < 4096 && live == 0);
  for (size_t alignment = 1; alignment <= 65536; alignment *= 2) {
    for (size_t size : {0u, 1u, 27u, 32u, 4095u, 65537u, 1048577u}) {
      auto* p = static_cast<unsigned char*>(Heap::Allocate(size, alignment));
      assert(p && !(reinterpret_cast<uintptr_t>(p) & (alignment - 1)));
      assert(Heap::Size(p) == size);
      std::memset(p, 0x55, size);
      auto* q = static_cast<unsigned char*>(Heap::Reallocate(p, size + 19));
      assert(q && Heap::Size(q) == size + 19);
      for (size_t i = 0; i < size; i++) assert(q[i] == 0x55);
      std::memset(q, 0x33, size + 19);
      auto* r = static_cast<unsigned char*>(Heap::Reallocate(q, 1));
      assert(r && r[0] == 0x33 && Heap::Size(r) == 1);
      assert(!Heap::Reallocate(r, 0));
    }
  }
  auto* zero = static_cast<unsigned char*>(Heap::Calloc(23, 17));
  assert(zero);
  for (size_t i = 0; i < 23 * 17; i++) assert(!zero[i]);
  Heap::Free(zero);
  assert(!Heap::Allocate(1, 0) && errno == EINVAL);
  assert(!Heap::Allocate(1, 3) && errno == EINVAL);
  assert(!Heap::Allocate(SIZE_MAX) && errno == ENOMEM);
  assert(!Heap::Allocate(SIZE_MAX - 64, 4096) && errno == ENOMEM);
  assert(!Heap::Calloc(SIZE_MAX / 2 + 1, 2) && errno == ENOMEM);
  auto* p = static_cast<unsigned char*>(Heap::Reallocate(nullptr, 32));
  assert(p);
  std::memset(p, 0x77, 32);
  assert(!Heap::Reallocate(p, SIZE_MAX) && errno == ENOMEM);
  void* output = reinterpret_cast<void*>(0x1234);
  assert(Heap::PosixMemalign(&output, 1, 32) == EINVAL && output == reinterpret_cast<void*>(0x1234));
  assert(Heap::PosixMemalign(&output, 24, 32) == EINVAL && output == reinterpret_cast<void*>(0x1234));
  fail = true;
  assert(!Heap::Allocate(32) && errno == ENOMEM);
  assert(!Heap::Reallocate(p, 128) && errno == ENOMEM);
  for (size_t i = 0; i < 32; i++) assert(p[i] == 0x77);
  assert(Heap::PosixMemalign(&output, 64, 32) == ENOMEM && output == reinterpret_cast<void*>(0x1234));
  fail = false;
  Heap::Free(p);
  assert(Heap::PosixMemalign(&output, 64, 32) == 0 && !(reinterpret_cast<uintptr_t>(output) & 63));
  Heap::Free(output);
  Heap::Free(nullptr);
  assert(Heap::Size(nullptr) == 0);
  std::vector<void*> allocations(8000);
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 8; t++) threads.emplace_back([&, t] {
    for (size_t i = t; i < allocations.size(); i += 8) {
      allocations[i] = Heap::Allocate(32 + i % 4096, 64);
      assert(allocations[i]);
      std::memset(allocations[i], 0x19, 32 + i % 4096);
    }
  });
  for (auto& thread : threads) thread.join();
  threads.clear();
  for (size_t t = 0; t < 8; t++) threads.emplace_back([&, t] {
    for (size_t i = t; i < allocations.size(); i += 8) {
      size_t index = allocations.size() - 1 - i;
      assert(*static_cast<unsigned char*>(allocations[index]) == 0x19);
      Heap::Free(allocations[index]);
    }
  });
  for (auto& thread : threads) thread.join();
  assert(live == 0);
  puts("FEX heap: config string, alignment, overflow, realloc failure and cross-thread free passed");
}
'''

winapi = (source / 'Source/Windows/Common/WinAPI/Alloc.cpp').read_text()
begin = winapi.index('#if !defined(_M_ARM64EC)')
alloc = winapi[begin:winapi.index('DLLEXPORT_FUNC(SIZE_T, VirtualQuery')]
alloc += winapi[winapi.index('DLLEXPORT_FUNC(void*, VirtualAlloc2,'):winapi.index('DLLEXPORT_FUNC(WINBOOL, VirtualFree')]
vm_fixture = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
using SIZE_T = size_t;
using DWORD = uint32_t;
using ULONG = uint32_t;
using NTSTATUS = uint32_t;
using HANDLE = void*;
struct MEM_EXTENDED_PARAMETER { uint64_t Type; void* Pointer; };
#define DLLEXPORT_FUNC(ret, name, args) ret name args
static constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_TOP_DOWN = 0x100000;
static constexpr DWORD PAGE_READWRITE = 4;
static DWORD last_error;
static NTSTATUS failure;
static unsigned native_calls, extended_calls;
static HANDLE process;
static MEM_EXTENDED_PARAMETER* parameters;
static ULONG count, allocation_type;
static HANDLE NtCurrentProcess() { return reinterpret_cast<void*>(-1); }
static DWORD RtlNtStatusToDosError(NTSTATUS status) { return status; }
static void SetLastError(DWORD error) { last_error = error; }
static NTSTATUS NtAllocateVirtualMemory(HANDLE p, void** base, uintptr_t zero_bits, SIZE_T* size, ULONG type, ULONG protect) {
  assert(p == NtCurrentProcess() && !zero_bits && *size && protect == PAGE_READWRITE);
  native_calls++;
  allocation_type = type;
  if (!failure && !*base) *base = reinterpret_cast<void*>(0x200000);
  return failure;
}
static NTSTATUS NtAllocateVirtualMemoryEx(HANDLE p, void** base, SIZE_T* size, ULONG type, ULONG protect,
                                         MEM_EXTENDED_PARAMETER* params, ULONG n) {
  assert(*size && protect == PAGE_READWRITE);
  extended_calls++;
  process = p; parameters = params; count = n; allocation_type = type;
  if (!failure && !*base) *base = reinterpret_cast<void*>(0x300000);
  return failure;
}
'''
vm_tests = r'''
int main() {
  auto* low = reinterpret_cast<void*>(0x200000);
  assert(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE) == low);
  assert(native_calls == 1 && extended_calls == 0 && allocation_type == (MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN));
  assert(VirtualAlloc(low, 4096, MEM_COMMIT, PAGE_READWRITE) == low && native_calls == 2);
  assert(VirtualAlloc2(nullptr, nullptr, 4096, MEM_RESERVE, PAGE_READWRITE, nullptr, 0));
  assert(extended_calls == 1 && process == NtCurrentProcess() && !parameters && !count);
  MEM_EXTENDED_PARAMETER req {1, low};
  HANDLE remote = reinterpret_cast<void*>(5);
  assert(VirtualAlloc2(remote, low, 4096, MEM_RESERVE, PAGE_READWRITE, &req, 1) == low);
  assert(extended_calls == 2 && process == remote && parameters == &req && count == 1);
  assert(VirtualAlloc2(nullptr, nullptr, 4096, MEM_RESERVE, PAGE_READWRITE, &req, 1));
  assert(extended_calls == 3 && parameters == &req && count == 1);
  failure = 0xc0000018;
  assert(!VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_READWRITE) && last_error == failure);
  assert(!VirtualAlloc2(nullptr, nullptr, 4096, MEM_RESERVE, PAGE_READWRITE, nullptr, 0) && last_error == failure);
  puts("FEX virtual allocations: low addresses, fixed commits, explicit requirements and failures passed");
}
'''
cc = os.environ.get('WINE_NX_HOST_CXX', '/usr/bin/clang++')
with tempfile.TemporaryDirectory(prefix='fex-heap-') as directory:
    build = Path(directory)
    for name, code, defines in (
            ('heap', '#include <cstdint>\n' + (probe / 'fex/unixlib.h').read_text() + fixture + heap_bridge + heap_dispatch + header + tests, []),
            ('wow64', vm_fixture + alloc + vm_tests, ['-DFEX_HORIZON']),
            ('arm64ec', vm_fixture + alloc + vm_tests, ['-DFEX_HORIZON', '-D_M_ARM64EC'])):
        file = build / (name + '.cpp')
        file.write_text(code)
        subprocess.run([cc, '-std=c++20', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', *defines, str(file), '-o', str(build / name)], check=True)
        subprocess.run([str(build / name)], check=True)
