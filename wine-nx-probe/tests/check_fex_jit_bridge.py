#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
patch = (probe / 'fex/horizon.patch').read_text()
section = patch.split('+++ b/Source/Windows/Common/Horizon.cpp\n', 1)[1].split('\ndiff --git', 1)[0]
source = '\n'.join(line[1:] for line in section.splitlines() if line.startswith('+'))
for header in ('<FEXCore/Utils/Horizon.h>', '<FEXCore/Utils/LogManager.h>', '"FEXUnixLib.h"', '<libloaderapi.h>'):
    source = source.replace(f'#include {header}', '')
fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include "unixlib.h"
extern "C" {
#include "fex_jit.h"
}
using NTSTATUS = unsigned int;
using FEXUnixLibFunctions = unsigned int;
static void* GetModuleHandleW(const wchar_t*) { return reinterpret_cast<void*>(1); }
#define ERROR_AND_DIE_FMT(...) std::abort()
static bool fail_metadata;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (fail_metadata) { fail_metadata = false; return nullptr; }
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
namespace FEX::Windows::UnixLib {
static bool available;
static bool Available() { return available; }
static bool Init(void* module) { assert(module); return available = true; }
static unsigned alloc_calls, fail_allocate, fail_free;
NTSTATUS Call(FEXUnixLibFunctions function, void* argument) {
  if (function == WINE_NX_FEX_QUERY || function == WINE_NX_FEX_ATTACH_THREAD) return 0;
  auto* args = static_cast<wine_nx_fex_memory*>(argument);
  if (function == WINE_NX_FEX_ALLOC) {
    ++alloc_calls;
    if (fail_allocate) { --fail_allocate; return 1; }
    void *rx, *rw;
    auto error = wine_nx_fex_jit_create(args->size, &rx, &rw);
    args->rx = reinterpret_cast<uintptr_t>(rx);
    args->rw = reinterpret_cast<uintptr_t>(rw);
    return error;
  }
  if (function == WINE_NX_FEX_FREE) {
    if (fail_free) return 1;
    assert(wine_nx_fex_jit_size(reinterpret_cast<void*>(args->rx)) == args->size);
    return wine_nx_fex_jit_close(reinterpret_cast<void*>(args->rx));
  }
  assert(function == WINE_NX_FEX_FLUSH);
  return wine_nx_fex_jit_flush(reinterpret_cast<void*>(args->rx), args->size);
}
}
'''
tests = r'''
int main() {
  using namespace FEXCore::Allocator;
  assert(HorizonInitialize() && HorizonAttachThread());
  assert(!HorizonFreeJit(nullptr));
  int plain;
  assert(HorizonWritableAddress(&plain) == &plain);
  std::vector<void*> buffers;
  for (unsigned i = 0; i < 96; ++i) {
    if (i == 32) {
      const auto calls = FEX::Windows::UnixLib::alloc_calls;
      fail_metadata = true;
      assert(!HorizonAllocateJit(4096));
      assert(FEX::Windows::UnixLib::alloc_calls == calls);
    }
    FEX::Windows::UnixLib::fail_allocate = 1;
    assert(!HorizonAllocateJit(4096));
    auto* rx = static_cast<unsigned char*>(HorizonAllocateJit(4096));
    assert(rx);
    auto* rw = static_cast<unsigned char*>(HorizonWritableAddress(rx));
    assert(rw != rx && HorizonWritableAddress(rx + 4095) == rw + 4095);
    memset(rw, i, 4096);
    HorizonFlushICache(rx, 4096);
    assert(!HorizonFreeJit(rx + 1));
    buffers.push_back(rx);
  }
  for (unsigned i = 0; i < buffers.size(); ++i) assert(*static_cast<unsigned char*>(buffers[i]) == i);
  for (unsigned i = 0; i < buffers.size(); i += 2) {
    assert(HorizonFreeJit(buffers[i]));
    assert(HorizonWritableAddress(buffers[i]) == buffers[i]);
    assert(!HorizonFreeJit(buffers[i]));
  }
  std::atomic<bool> stop {};
  std::thread reader([&] {
    while (!stop.load()) {
      for (unsigned i = 1; i < buffers.size(); i += 2) {
        auto* rx = static_cast<unsigned char*>(buffers[i]);
        auto* rw = static_cast<unsigned char*>(HorizonWritableAddress(rx));
        assert(rw != rx && *rw == i && *rx == i);
      }
    }
  });
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 4; ++i) workers.emplace_back([] {
    for (unsigned round = 0; round < 20; ++round) {
      std::vector<void*> live;
      for (unsigned j = 0; j < 40; ++j) {
        auto* rx = static_cast<unsigned char*>(HorizonAllocateJit(4096));
        assert(rx);
        auto* rw = static_cast<unsigned char*>(HorizonWritableAddress(rx));
        assert(rw != rx);
        rw[0] = 0x5a;
        assert(rx[0] == 0x5a);
        live.push_back(rx);
      }
      for (auto* rx : live) assert(HorizonFreeJit(rx));
    }
  });
  for (auto& worker : workers) worker.join();
  stop.store(true);
  reader.join();
  for (unsigned i = 1; i < buffers.size(); i += 2) assert(HorizonFreeJit(buffers[i]));
  assert(!wine_nx_fex_jit_release());
  puts("FEX DLL bridge: 96 retained buffers, growing metadata, allocation failures, alias bounds and concurrent reuse passed");
}
'''
with tempfile.TemporaryDirectory(prefix='fex-jit-bridge-') as directory:
    path = Path(directory)
    (path / 'bridge.cpp').write_text(fixture + source + tests)
    cc = os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang')
    cxx = os.environ.get('WINE_NX_HOST_CXX', '/usr/bin/clang++')
    flags = ['-g', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
    subprocess.run([cc, *flags, '-c', str(probe / 'source/fex_jit.c'), '-o', str(path / 'jit.o')], check=True)
    subprocess.run([cxx, '-std=c++20', *flags, '-I', str(probe / 'fex'), '-I', str(probe / 'source'),
                    str(path / 'bridge.cpp'), str(path / 'jit.o'), '-o', str(path / 'bridge')], check=True)
    subprocess.run([str(path / 'bridge')], check=True)
