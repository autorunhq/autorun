#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/HostFeatures.h>
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

using namespace FEXCore::X86State;
static FEXCore::Context::Context* context;
static FEXCore::Core::InternalThreadState* thread;
static FEXCore::SignalDelegator signals;
static uintptr_t protected_page, fault_address;
static uint64_t fault_sp, fault_pc;
static volatile sig_atomic_t faults;

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { std::abort(); }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0x100000, 0x140000, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

static void fault_handler(int, siginfo_t* info, void* opaque) {
  if (faults || (reinterpret_cast<uintptr_t>(info->si_addr) & ~uintptr_t(4095)) != protected_page) _exit(2);
  faults = 1;
  auto& host = static_cast<ucontext_t*>(opaque)->uc_mcontext;
  const auto& mapping = signals.GetConfig();
  auto& state = thread->CurrentFrame->State;
  fault_address = reinterpret_cast<uintptr_t>(info->si_addr);
  fault_sp = host.regs[mapping.SRAGPRMapping[REG_RSP]];
  fault_pc = context->RestoreRIPFromHostPC(thread, host.pc);
  uint64_t regs[31];
  memcpy(regs, host.regs, sizeof(regs));
  const auto flags = context->ReconstructCompactedEFLAGS(thread, true, regs, host.pstate);
  for (unsigned i = 0; i < mapping.SRAGPRCount; ++i) state.gregs[i] = regs[mapping.SRAGPRMapping[i]];
  state.rip = fault_pc;
  context->SetFlagsFromCompactedEFLAGS(thread, flags);
  if (mprotect(reinterpret_cast<void*>(protected_page), 4096, PROT_READ | PROT_WRITE)) _exit(3);
  host.pc = mapping.AbsoluteLoopTopAddressFillSRA;
  host.regs[1] = 1;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const bool wide = std::atoi(argv[1]) == 64;
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  auto memory = mmap(reinterpret_cast<void*>(0x100000), 0x40000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (memory == MAP_FAILED) return 2;
  struct sigaction action {};
  action.sa_sigaction = fault_handler;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, nullptr);
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, wide ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "1");
  FEXCore::HostFeatures features {};
  features.DCacheLineLog2 = 4;
  features.HostType = FEXCore::HostFeatures::HostTypeEnum::Wow64;
  features.CPUMIDRs.assign(4, 0x411fd071);
  auto owner = FEXCore::Context::Context::CreateNewContext(features);
  context = owner.get();
  Syscalls syscalls;
  context->SetSignalDelegator(&signals);
  context->SetSyscallHandler(&syscalls);
  context->EnableExitOnHLT();
  if (!context->InitCore()) return 2;

  struct Test { const char* name; std::vector<uint8_t> code; unsigned width; uintptr_t target; bool source_fault; };
  const unsigned width = wide ? 8 : 4;
  constexpr uintptr_t sp = 0x120ff0, dest = 0x110040;
  std::vector<Test> tests = {
    {"base", {0x8f, 0x45, 0x0c}, width, dest, false},
    {"base16", {0x66, 0x8f, 0x45, 0x0c}, 2, dest, false},
    {"stack", {0x8f, 0x04, 0x24}, width, sp + width, false},
    {"stack16", {0x66, 0x8f, 0x04, 0x24}, 2, sp + 2, false},
    {"stack-displacement", {0x8f, 0x44, 0x24, 0x20}, width, sp + width + 0x20, false},
    {"scaled-index", {0x8f, 0x44, 0x8d, 0x04}, width, dest, false},
    {"stack-scaled-index", {0x8f, 0x44, 0x8c, 0x10}, width, sp + width + 0x18, false},
    {"stack-alias", {0x8f, 0x44, 0x24, static_cast<uint8_t>(-width)}, width, sp, false},
    {"segment", {0x64, 0x8f, 0x00}, width, dest, false},
    {"relative-or-absolute", {0x8f, 0x05, 0, 0, 0, 0}, width, dest, false},
    {"source", {0x8f, 0x45, 0x0c}, width, dest, true},
    {"source16", {0x66, 0x8f, 0x45, 0x0c}, 2, dest, true},
  };
  if (wide) {
    tests.push_back({"address32-base", {0x67, 0x8f, 0x45, 0x0c}, width, dest, false});
    tests.push_back({"address32-stack", {0x67, 0x8f, 0x04, 0x24}, width, sp + width, false});
  } else {
    tests.push_back({"address16-segment", {0x64, 0x67, 0x8f, 0x00}, width, dest, false});
  }
  unsigned index = 0;
  for (const auto& test : tests) {
    const uintptr_t entry = 0x100000 + index++ * 0x100;
    memset(reinterpret_cast<void*>(0x110000), 0, 0x30000);
    *reinterpret_cast<uint8_t*>(entry) = 0x90;
    auto code = test.code;
    if (code[0] == 0x8f && code[1] == 0x05) {
      const uint32_t offset = wide ? dest - (entry + 1 + code.size()) : dest;
      memcpy(code.data() + 2, &offset, sizeof(offset));
    }
    memcpy(reinterpret_cast<void*>(entry + 1), code.data(), code.size());
    *reinterpret_cast<uint8_t*>(entry + 1 + test.code.size()) = 0xf4;
    const uint64_t value = 0x123456789abcdef0ULL, sentinel = 0x0fedcba987654321ULL;
    memcpy(reinterpret_cast<void*>(sp), &value, test.width);
    memcpy(reinterpret_cast<void*>(sp + test.width), &sentinel, test.width);
    thread = context->CreateThread();
    auto& state = thread->CurrentFrame->State;
    FEXCore::Core::CPUState::gdt_segment segments[32] {};
    segments[0].D = !wide;
    segments[0].L = wide;
    FEXCore::Core::CPUState::SetGDTLimit(&segments[0], 0xfffff);
    state.segment_arrays[0] = state.segment_arrays[1] = segments;
    state.callret_sp = 0x138000;
    state.rip = entry;
    state.gregs[REG_RSP] = sp;
    state.gregs[REG_RBP] = dest - 12;
    state.gregs[REG_RCX] = 2;
    state.gregs[REG_RAX] = 0x40;
    state.gregs[REG_RBX] = 0x40;
    state.fs_cached = 0x110000;
    context->SetFlagsFromCompactedEFLAGS(thread, 0x246);
    faults = 0;
    protected_page = (test.source_fault ? sp : test.target) & ~uintptr_t(4095);
    if (mprotect(reinterpret_cast<void*>(protected_page), 4096, test.source_fault ? PROT_NONE : PROT_READ)) return 2;
    context->ExecuteThread(thread);
    uint64_t result = 0;
    memcpy(&result, reinterpret_cast<void*>(test.target), test.width);
    const uint64_t mask = test.width == 8 ? ~0ULL : (1ULL << (test.width * 8)) - 1;
    const auto flags = context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
    const bool passed = faults == 1 && fault_pc == entry + 1 && fault_sp == sp &&
      fault_address == (test.source_fault ? sp : test.target) && result == (value & mask) &&
      state.gregs[REG_RSP] == sp + test.width && flags == 0x246 && state.gregs[REG_RBP] == dest - 12;
    printf("POP %u %s: %s fault_sp=%llx final_sp=%llx value=%llx\n", wide ? 64 : 32, test.name,
           passed ? "PASS" : "FAIL", static_cast<unsigned long long>(fault_sp),
           static_cast<unsigned long long>(state.gregs[REG_RSP]), static_cast<unsigned long long>(result));
    context->DestroyThread(thread);
    if (!passed) return 1;
  }
}
