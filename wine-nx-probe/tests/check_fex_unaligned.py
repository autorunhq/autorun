#!/usr/bin/env python3
"""Run FEX's unaligned-access handler against separate RX/RW mappings."""
from pathlib import Path
import os
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('WINE_NX_FEX_DIR', probe / 'toolchains/fex-2609'))


def structure(path, name):
    text = path.read_text()
    start = text.index('struct ' + name + ' {')
    return text[start:text.index('};', start) + 2]


backend = source / 'FEXCore/Source/Interface/Core/CPUBackend.h'
thread = source / 'FEXCore/include/FEXCore/Debug/InternalThreadState.h'
fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <CodeEmitter/Buffer.h>
#include <FEXCore/Utils/LogManager.h>
extern "C" {
#include "fex_jit.h"
}
namespace FEXCore::CPU {
struct CPUBackend {
''' + structure(backend, 'JITCodeHeader') + structure(backend, 'JITCodeTail') + r'''
};
}
namespace FEXCore::Context {
struct ContextImpl {
  struct { bool StrictInProcessSplitLocks; } Config {};
  uint32_t StrictSplitLockMutex {};
};
}
namespace FEXCore::Core {
''' + structure(thread, 'UnalignedExclusiveStore') + r'''
struct CpuStateFrame { struct { uint64_t InlineJITBlockHeader; } State; };
struct InternalThreadState {
  Context::ContextImpl* CTX;
  CpuStateFrame* CurrentFrame;
  UnalignedExclusiveStore ExclusiveStore {};
};
}
static uint8_t *rx, *rw;
static constexpr size_t mapping_size = 8192, tail_offset = 4096;
static std::atomic<unsigned> flushes {};
namespace FEXCore::Allocator {
void* HorizonWritableAddress(const void* pointer) {
  auto offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(rx);
  return offset < mapping_size ? rw + offset : const_cast<void*>(pointer);
}
void HorizonFlushICache(void* pointer, size_t size) {
  auto* tail = reinterpret_cast<CPU::CPUBackend::JITCodeTail*>(rw + tail_offset);
  assert(std::atomic_ref(tail->SpinLockFutex).load() == 1);
  assert(!wine_nx_fex_jit_flush(pointer, size));
  ++flushes;
}
}
void LogMan::Msg::MFmtImpl(DebugLevels, const char* format, const fmt::format_args& args) {
  fmt::vprint(stderr, format, args);
  std::abort();
}
'''
handler = (source / 'FEXCore/Source/Utils/ArchHelpers/Arm64.cpp').read_text()
for header in ('"Interface/Core/CPUBackend.h"', '"Interface/Context/Context.h"',
               '<FEXCore/Debug/InternalThreadState.h>'):
    handler = handler.replace('#include ' + header, '')

tests = r'''
int main() {
  using namespace FEXCore;
  using namespace FEXCore::ArchHelpers::Arm64;
  void *exec, *write;
  assert(!wine_nx_fex_jit_create(mapping_size, &exec, &write));
  rx = static_cast<uint8_t*>(exec);
  rw = static_cast<uint8_t*>(write);
  assert(rx != rw);
  auto* header = reinterpret_cast<CPU::CPUBackend::JITCodeHeader*>(rw);
  header->OffsetToBlockTail = tail_offset;
  auto* tail = reinterpret_cast<CPU::CPUBackend::JITCodeTail*>(rw + tail_offset);
  auto* code = reinterpret_cast<uint32_t*>(rw + 64);
  Context::ContextImpl context {};
  Core::CpuStateFrame frame {{reinterpret_cast<uintptr_t>(rx)}};
  Core::InternalThreadState thread {&context, &frame};
  uint64_t regs[32] {};
  constexpr uint32_t nop = 0xd503201f;
  for (auto mode : {UnalignedHandlerType::HalfBarrier, UnalignedHandlerType::NonAtomic}) {
    for (unsigned size = 1; size <= 3; ++size) {
      for (auto op : {LDAR_INST, LDAPR_INST, STLR_INST, LDAPUR_INST, STLUR_INST}) {
        const bool store = op == STLR_INST || op == STLUR_INST;
        const bool offset = op == LDAPUR_INST || op == STLUR_INST;
        const uint32_t operands = (size << 30) | 1 | (offset ? (3 << 12) : 0);
        const uint32_t patched = (store ? (offset ? STUR_INST : STR_INST) :
                                          (offset ? LDUR_INST : LDR_INST)) | operands;
        const uintptr_t pc = reinterpret_cast<uintptr_t>(rx + 68);
        code[0] = code[2] = nop;
        code[1] = op | operands;
        code[3] = 0xaa0103e0; // mov x0, x1
        code[4] = 0xd65f03c0; // ret
        assert(!wine_nx_fex_jit_flush(rx + 64, 20));
        unsigned before = flushes;
        auto result = HandleUnalignedAccess(&thread, mode, pc, regs);
        assert(result && *result == (store ? -4 : 0));
        assert(flushes == before + 1 && !tail->SpinLockFutex);
        assert(code[1] == patched);
        assert(code[0] == (store && mode == UnalignedHandlerType::HalfBarrier ? DMB : nop));
        assert(code[2] == (!store && mode == UnalignedHandlerType::HalfBarrier ? DMB_LD : nop));
        result = HandleUnalignedAccess(&thread, mode, pc, regs);
        assert(result && *result == (store && mode == UnalignedHandlerType::HalfBarrier ? -4 : 0));
        assert(flushes == before + 1 && !tail->SpinLockFutex);
        alignas(16) uint8_t data[32] {};
        uint64_t expected = 0x1234, value = 0;
        auto* address = data + 1 + (offset ? 3 : 0);
        if (!store) std::memcpy(address, &expected, 1u << size);
        auto run = reinterpret_cast<uint64_t (*)(void*, uint64_t)>(rx + 64);
        assert(run(data + 1, expected) == expected);
        std::memcpy(&value, address, 1u << size);
        assert(value == expected);
      }
    }
  }
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    code[0] = code[2] = nop;
    code[1] = LDAR_INST | (3u << 30) | 1;
    assert(!wine_nx_fex_jit_flush(rx + 64, 12));
    std::atomic<bool> start {};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
      uint64_t regs[32] {};
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      auto result = HandleUnalignedAccess(&thread, UnalignedHandlerType::HalfBarrier,
                                         reinterpret_cast<uintptr_t>(rx + 68), regs);
      assert(result && *result == 0);
    });
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    assert(!tail->SpinLockFutex && code[1] == (LDR_INST | (3u << 30) | 1) && code[2] == DMB_LD);
  }
  assert(!wine_nx_fex_jit_close(rx));
  puts("FEX unaligned access: RX lock, load/store patching, barriers, execution and contention passed");
}
'''

with tempfile.TemporaryDirectory(prefix='fex-unaligned-') as temp:
    build = Path(temp)
    cpp = build / 'unaligned.cpp'
    cpp.write_text(fixture + handler + tests)
    sanitizers = ['-fsanitize=address,undefined', '-fno-sanitize=function']
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-g', '-pthread',
                    *sanitizers, '-c', str(probe / 'source/fex_jit.c'), '-o', str(build / 'jit.o')], check=True)
    subprocess.run([os.environ.get('WINE_NX_HOST_CXX', '/usr/bin/clang++'), '-std=c++20', '-O2', '-g',
                    '-pthread', *sanitizers, '-DFEX_HORIZON', '-DARCHITECTURE_arm64',
                    '-DFEX_DISABLE_TELEMETRY', '-DFMT_HEADER_ONLY',
                    '-I', str(source / 'CodeEmitter'), '-I', str(source / 'FEXCore/include'),
                    '-I', str(source / 'FEXHeaderUtils'), '-I', str(source / 'External/fmt/include'),
                    '-I', str(probe / 'source'), str(cpp), str(build / 'jit.o'),
                    '-o', str(build / 'unaligned')], check=True)
    subprocess.run([str(build / 'unaligned')], check=True, timeout=60)
