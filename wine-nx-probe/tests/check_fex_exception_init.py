#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
source_dir = Path(os.environ.get('WINE_NX_FEX_DIR', probe / 'toolchains/fex-2609'))
source = (source_dir / 'Source/Windows/WOW64/Module.cpp').read_text()
start = source.index('bool BTCpuResetToConsistentStateImpl(')
end = source.index('    if (Context::HandleSuspendInterrupt', start)
entry = source[start:end] + '    return false;\n  }\n  return false;\n}\n'
fixture = r'''
#include <FEXCore/Utils/SHMStats.h>
#include <cassert>
#include <cstdio>
constexpr unsigned EXCEPTION_ACCESS_VIOLATION = 0xc0000005;
struct NativeContext { uint64_t X25; };
struct Record { unsigned ExceptionCode; uint64_t ExceptionInformation[2]; };
struct EXCEPTION_POINTERS { Record* ExceptionRecord; NativeContext* ContextRecord; };
struct ThreadState { FEXCore::SHMStats::ThreadStats* ThreadStats; };
static ThreadState* current;
struct TLS { ThreadState* ThreadState() const { return current; } };
static TLS GetTLS() { return {}; }
static unsigned stack_calls, overcommit_calls;
static bool stack_handles, overcommit_handles;
namespace FEX::Windows::CallRetStack {
bool HandleAccessViolation(ThreadState* thread, uint64_t address, uint64_t sp) {
  assert(thread && address == 0x123000 && sp == 0x456000);
  ++stack_calls;
  return stack_handles;
}
}
struct Tracker {
  bool HandleAccessViolation(uint64_t address) {
    assert(address == 0x123000);
    ++overcommit_calls;
    return overcommit_handles;
  }
};
static Tracker tracker, *OvercommitTracker = &tracker;
'''
tests = r'''
int main() {
  NativeContext context {0x456000};
  Record record {EXCEPTION_ACCESS_VIOLATION, {1, 0x123000}};
  EXCEPTION_POINTERS pointers {&record, &context};
  overcommit_handles = true;
  assert(BTCpuResetToConsistentStateImpl(&pointers));
  assert(!stack_calls && overcommit_calls == 1);
  overcommit_handles = false;
  assert(!BTCpuResetToConsistentStateImpl(&pointers));
  OvercommitTracker = nullptr;
  assert(!BTCpuResetToConsistentStateImpl(&pointers));
  record.ExceptionCode = 0xc000001d;
  assert(!BTCpuResetToConsistentStateImpl(&pointers));
  assert(!stack_calls && overcommit_calls == 2);
  record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  ThreadState thread {};
  current = &thread;
  stack_handles = true;
  assert(BTCpuResetToConsistentStateImpl(&pointers));
  FEXCore::SHMStats::ThreadStats stats {};
  thread.ThreadStats = &stats;
  assert(BTCpuResetToConsistentStateImpl(&pointers));
  assert(stack_calls == 2 && overcommit_calls == 2);
  puts("FEX exception initialization: absent thread, overcommit and live statistics passed");
}
'''
with tempfile.TemporaryDirectory(prefix='fex-exception-init-') as directory:
    path = Path(directory)
    (path / 'entry.cpp').write_text(fixture + entry + tests)
    cxx = os.environ.get('WINE_NX_HOST_CXX', '/usr/bin/clang++')
    subprocess.run([cxx, '-std=c++20', '-DARCHITECTURE_arm64=1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(source_dir / 'FEXCore/include'),
                    str(path / 'entry.cpp'), '-o', str(path / 'entry')], check=True)
    subprocess.run([str(path / 'entry')], check=True)
