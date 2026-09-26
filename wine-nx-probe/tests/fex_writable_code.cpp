#include "Interface/Core/LookupCache.h"
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

static bool permission_mode;
static FEXCore::Context::Context* active_context;
static FEXCore::Core::InternalThreadState* active_thread;
static FEXCore::SignalDelegator* active_signals;
static std::vector<FEXCore::Core::InternalThreadState*> threads;
static volatile sig_atomic_t write_faults;

static void write_fault(int, siginfo_t* info, void* opaque) {
  const uintptr_t page = reinterpret_cast<uintptr_t>(info->si_addr) & ~uintptr_t(4095);
  if (page != 0x100000 || !permission_mode) _exit(2);
  write_faults = write_faults + 1;
  auto& host = static_cast<ucontext_t*>(opaque)->uc_mcontext;
  auto& context = *active_context;
  auto* thread = active_thread;
  {
    std::scoped_lock lock(context.GetCodeInvalidationMutex());
    context.InvalidateCodeBuffersCodeRange(page, 4096);
    for (auto* entry : threads) context.InvalidateThreadCachedCodeRange(entry, page, 4096);
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE)) _exit(3);
  }
  if (context.IsAddressInCodeBuffer(thread, host.pc) && !context.IsCurrentBlockSingleInst(thread) &&
      context.IsAddressInCurrentBlock(thread, page, 4096)) {
    const auto& mapping = active_signals->GetConfig();
    auto& state = thread->CurrentFrame->State;
    uint64_t regs[31];
    memcpy(regs, host.regs, sizeof(regs));
    const auto flags = context.ReconstructCompactedEFLAGS(thread, true, regs, host.pstate);
    for (unsigned i = 0; i < mapping.SRAGPRCount; ++i) state.gregs[i] = regs[mapping.SRAGPRMapping[i]];
    state.rip = context.RestoreRIPFromHostPC(thread, host.pc);
    context.SetFlagsFromCompactedEFLAGS(thread, flags);
    host.pc = mapping.AbsoluteLoopTopAddressFillSRA;
    host.regs[1] = 1;
  }
}

static void check(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FEX writable code: %s\n", message); std::exit(1); }
}

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  FEXCore::Context::Context* context {};
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { std::abort(); }
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* thread, uint64_t start, uint64_t size) override {
    std::scoped_lock lock(context->GetCodeInvalidationMutex());
    context->InvalidateCodeBuffersCodeRange(start, size);
    context->InvalidateThreadCachedCodeRange(thread, start, size);
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t address) override {
    const auto page = address & ~4095ULL;
    const bool writable = page == 0x100000;
    bool tracked = permission_mode && writable;
    if (tracked && mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ)) std::abort();
    return {page, 4096, writable, tracked};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  permission_mode = argc == 3;
  struct sigaction action {};
  action.sa_sigaction = write_fault;
  action.sa_flags = SA_SIGINFO;
  if (permission_mode) sigaction(SIGSEGV, &action, nullptr);
  const bool wide = std::atoi(argv[1]) == 64;
  check(mmap(reinterpret_cast<void*>(0x0ff000), 0x21000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED, "mapping failed");
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, wide ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  FEXCore::HostFeatures features {};
  features.DCacheLineLog2 = 4;
  features.HostType = FEXCore::HostFeatures::HostTypeEnum::Wow64;
  features.CPUMIDRs.assign(4, 0x411fd071);
  Syscalls syscalls;
  FEXCore::SignalDelegator signals;
  auto context = FEXCore::Context::Context::CreateNewContext(features);
  syscalls.context = context.get();
  context->SetSignalDelegator(&signals);
  context->SetSyscallHandler(&syscalls);
  context->EnableExitOnHLT();
  check(context->InitCore(), "core initialization failed");
  auto* thread = context->CreateThread();
  active_context = context.get();
  active_signals = &signals;
  active_thread = thread;
  threads.push_back(thread);
  const size_t callret_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  auto callret = mmap(nullptr, callret_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(callret != MAP_FAILED, "call/return stack mapping failed");
  thread->CallRetStackBase = callret;
  FEXCore::Core::CPUState::gdt_segment segments[32] {};
  segments[0].D = !wide;
  segments[0].L = wide;
  FEXCore::Core::CPUState::SetGDTLimit(&segments[0], 0xfffff);
  auto& state = thread->CurrentFrame->State;
  state.segment_arrays[0] = state.segment_arrays[1] = segments;
  state.callret_sp = reinterpret_cast<uintptr_t>(callret) + callret_size / 4;
  auto execute = [&](uint64_t address) {
    state.rip = address;
    state.gregs[FEXCore::X86State::REG_RSP] = 0x110000;
    context->SetFlagsFromCompactedEFLAGS(thread, 0x202);
    context->ExecuteThread(thread);
  };

  const uint8_t data_write[] = {0x89, 0x03, 0xb8, 17, 0, 0, 0, 0xf4};
  memcpy(reinterpret_cast<void*>(0x100000), data_write, sizeof(data_write));
  state.gregs[FEXCore::X86State::REG_RBX] = 0x100800;
  execute(0x100000);
  auto compiled = thread->LookupCache->FindBlock(thread, 0x100000);
  if (!permission_mode) check(compiled != 0, "block not cached");
  for (unsigned i = 0; i < (permission_mode ? 100U : 10000U); ++i) {
    state.gregs[FEXCore::X86State::REG_RAX] = i;
    execute(0x100000);
    check(*reinterpret_cast<uint32_t*>(0x100800) == i, "data write failed");
    check(state.gregs[FEXCore::X86State::REG_RAX] == 17, "wrong result");
    if (!permission_mode) check(thread->LookupCache->FindBlock(thread, 0x100000) == compiled, "data write recompiled unchanged code");
  }
  *reinterpret_cast<uint8_t*>(0x100003) = 29;
  execute(0x100000);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 29, "external instruction change missed");

  const uint8_t inline_write[] = {0xc6, 0x03, 42, 0xb8, 17, 0, 0, 0, 0xf4};
  memcpy(reinterpret_cast<void*>(0x100100), inline_write, sizeof(inline_write));
  state.gregs[FEXCore::X86State::REG_RBX] = 0x100104;
  execute(0x100100);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 42, "inline instruction change missed");
  *reinterpret_cast<uint8_t*>(0x100102) = 57;
  execute(0x100100);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 57, "repeated inline change missed");

  const uint8_t loop[] = {0xb9, 2, 0, 0, 0, 0xb8, 17, 0, 0, 0, 0xc6, 0x03, 42, 0xff, 0xc9, 0x75, 0xf4, 0xf4};
  memcpy(reinterpret_cast<void*>(0x100200), loop, sizeof(loop));
  state.gregs[FEXCore::X86State::REG_RBX] = 0x100206;
  execute(0x100200);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 42, "backward-branch split lost code validation");

  const uint8_t boundary[] = {0xb8, 71, 0, 0, 0, 0xf4};
  memcpy(reinterpret_cast<void*>(0x100ffe), boundary, sizeof(boundary));
  execute(0x100ffe);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 71, "cross-page decode failed");
  *reinterpret_cast<uint8_t*>(0x100fff) = 93;
  execute(0x100ffe);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 93, "writable-to-read-only boundary change missed");
  memcpy(reinterpret_cast<void*>(0x0ffffe), boundary, sizeof(boundary));
  execute(0x0ffffe);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 71, "reverse boundary decode failed");
  *reinterpret_cast<uint8_t*>(0x100000) = 1;
  execute(0x0ffffe);
  check(state.gregs[FEXCore::X86State::REG_RAX] == 327, "read-only-to-writable boundary change missed");
  if (permission_mode) {
    check(write_faults > 0, "permission tracking did not fault");
    const uint8_t code[] = {0xb8, 19, 0, 0, 0, 0xf4};
    memcpy(reinterpret_cast<void*>(0x100400), code, sizeof(code));
    execute(0x100400);
    auto* second = context->CreateThread();
    threads.push_back(second);
    second->CurrentFrame->State.segment_arrays[0] = segments;
    second->CurrentFrame->State.segment_arrays[1] = segments;
    second->CurrentFrame->State.callret_sp = state.callret_sp;
    second->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] = 0x110000;
    context->SetFlagsFromCompactedEFLAGS(second, 0x202);
    second->CallRetStackBase = callret;
    second->CurrentFrame->State.rip = 0x100400;
    active_thread = second;
    context->ExecuteThread(second);
    check(second->LookupCache->FindBlock(second, 0x100400) != 0, "second thread did not cache code");
    *reinterpret_cast<uint8_t*>(0x100401) = 37;
    check(thread->LookupCache->FindBlock(thread, 0x100400) == 0, "first thread retained stale code");
    check(second->LookupCache->FindBlock(second, 0x100400) == 0, "second thread retained stale code");
    second->CurrentFrame->State.rip = 0x100400;
    context->ExecuteThread(second);
    check(second->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] == 37, "second thread executed stale code");
    active_thread = thread;
    threads.pop_back();
    context->DestroyThread(second);
    printf("FEX permission SMC %u: inline/external writes, cross-page instructions and cross-thread invalidation passed (%d faults)\n",
           wide ? 64 : 32, static_cast<int>(write_faults));
  }
  context->DestroyThread(thread);
  munmap(callret, callret_size);
  munmap(reinterpret_cast<void*>(0x0ff000), 0x21000);
  if (!permission_mode)
    printf("FEX writable code %u: 10000 data writes without recompilation, external/inline SMC, block splits and page boundaries passed\n", wide ? 64 : 32);
}
