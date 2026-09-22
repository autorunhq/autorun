#include "Interface/Core/LookupCache.h"
#include "Interface/Core/CPUBackend.h"
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/mman.h>

static constexpr size_t MiB = 1024 * 1024;
static constexpr uintptr_t Guest = 0x1000000;
static uintptr_t next_mapping = 0x1000000000ULL;
static std::map<void*, size_t> executable;

static void check(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FEX segmented cache: %s\n", message); std::exit(1); }
}

static void* map_memory(void* base, size_t size, int prot, int flags, int fd, off_t offset) {
  if (prot & PROT_EXEC) {
    base = reinterpret_cast<void*>(next_mapping);
    next_mapping += 512 * MiB;
    flags |= MAP_FIXED_NOREPLACE;
  }
  auto* result = ::mmap(base, size, prot, flags, fd, offset);
  check(result != MAP_FAILED, "mapping failed");
  if (prot & PROT_EXEC) executable.emplace(result, size);
  return result;
}

static int unmap_memory(void* base, size_t size) {
  if (auto it = executable.find(base); it != executable.end()) {
    check(it->second == size, "incorrect release size");
    executable.erase(it);
  }
  return ::munmap(base, size);
}

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { std::abort(); }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t address) override {
    return {address & ~4095ULL, 4096, false};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const bool wide = std::atoi(argv[1]) == 64;
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, wide ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "0");
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  FEXCore::Allocator::mmap = map_memory;
  FEXCore::Allocator::munmap = unmap_memory;
  check(mmap(reinterpret_cast<void*>(Guest), 0x10000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED, "guest mapping failed");
  const uint8_t callee[] = {0xb8, 17, 0, 0, 0, 0xc3};
  memcpy(reinterpret_cast<void*>(Guest), callee, sizeof(callee));
  for (unsigned i : {1, 2}) {
    auto* caller = reinterpret_cast<uint8_t*>(Guest + i * 4096);
    const int32_t displacement = -static_cast<int32_t>(i * 4096 + 5);
    caller[0] = 0xe8;
    memcpy(caller + 1, &displacement, sizeof(displacement));
    caller[5] = 0xf4;
  }
  const uint8_t new_code[] = {0xb8, 43, 0, 0, 0, 0xf4};
  memcpy(reinterpret_cast<void*>(Guest + 0x3000), new_code, sizeof(new_code));

  FEXCore::HostFeatures features {};
  features.DCacheLineLog2 = 4;
  features.HostType = FEXCore::HostFeatures::HostTypeEnum::Wow64;
  features.CPUMIDRs.assign(4, 0x411fd071);
  Syscalls syscalls;
  FEXCore::SignalDelegator signals;
  auto context = FEXCore::Context::Context::CreateNewContext(features);
  auto& manager = static_cast<FEXCore::Context::ContextImpl&>(*context);
  manager.SetCodeCacheLimit(64 * MiB);
  context->SetSignalDelegator(&signals);
  context->SetSyscallHandler(&syscalls);
  context->EnableExitOnHLT();
  check(context->InitCore(), "initialization failed");

  FEXCore::Core::InternalThreadState* threads[2];
  FEXCore::Core::CPUState::gdt_segment segments[32] {};
  segments[0].D = !wide;
  segments[0].L = wide;
  FEXCore::Core::CPUState::SetGDTLimit(&segments[0], 0xfffff);
  const size_t stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  for (auto& thread : threads) {
    thread = context->CreateThread();
    thread->CallRetStackBase = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(thread->CallRetStackBase != MAP_FAILED, "return stack mapping failed");
    auto& state = thread->CurrentFrame->State;
    state.segment_arrays[0] = state.segment_arrays[1] = segments;
    state.callret_sp = reinterpret_cast<uintptr_t>(thread->CallRetStackBase) + stack_size / 4;
  }
  auto execute = [&](unsigned index, uintptr_t rip, uint64_t result) {
    auto* thread = threads[index];
    auto& state = thread->CurrentFrame->State;
    state.rip = rip;
    state.gregs[FEXCore::X86State::REG_RSP] = Guest + 0x8000 + index * 4096;
    context->SetFlagsFromCompactedEFLAGS(thread, 0x202);
    context->ExecuteThread(thread);
    check(state.gregs[FEXCore::X86State::REG_RAX] == result, "execution returned the wrong value");
  };
  auto invalidate = [&](uintptr_t rip) {
    std::scoped_lock lock(context->GetCodeInvalidationMutex());
    context->InvalidateCodeBuffersCodeRange(rip, 1);
    for (auto* thread : threads) context->InvalidateThreadCachedCodeRange(thread, rip, 1);
  };

  execute(0, Guest + 0x1000, 17);
  auto cache = manager.GetLatest();
  auto* lookup = cache->LookupCache.get();
  const auto original = threads[0]->LookupCache->FindBlock(threads[0], Guest);
  check(original != 0, "callee was not cached");
  check(cache->AtomicAllocateBuffer(cache->UsableSize() - cache->AllocatedSpaceUsed()).BufferBase, "padding failed");
  execute(0, Guest + 0x2000, 17);
  const auto caller = threads[0]->LookupCache->FindBlock(threads[0], Guest + 0x2000);
  check(caller > original + 128 * MiB, "test did not exercise distant code buffers");
  check(manager.GetLatest() == cache && cache->LookupCache.get() == lookup, "growth replaced the translation cache");
  check(cache->TotalAllocationSize() == 48 * MiB && cache->Contains(original) && cache->Contains(caller), "segment tracking failed");
  const auto used = cache->AllocatedSpaceUsed();
  for (unsigned i = 0; i < 100; ++i) execute(i & 1, Guest + 0x2000, 17);
  check(cache->AllocatedSpaceUsed() == used, "unchanged code was recompiled after growth");
  check(threads[1]->CPUBackend->IsAddressInCodeBuffer(caller), "second thread cannot recognize a later segment");

  *reinterpret_cast<uint8_t*>(Guest + 1) = 29;
  invalidate(Guest);
  execute(0, Guest + 0x2000, 29);
  execute(1, Guest + 0x2000, 29);
  check(manager.GetLatest() == cache, "invalidation discarded unrelated code");
  while (cache->AtomicAllocateBuffer(4096).BufferBase) {}
  while (cache->AtomicAllocateBuffer(16).BufferBase) {}
  check(cache->TotalAllocationSize() == 64 * MiB, "cache exceeded its budget");
  std::weak_ptr<FEXCore::CPU::CodeBuffer> old = cache;
  execute(0, Guest + 0x3000, 43);
  check(manager.GetLatest() != cache, "full cache did not rotate");
  cache.reset();
  execute(1, Guest + 0x2000, 29);
  check(!old.expired(), "old code was freed while a thread still used it");
  *reinterpret_cast<uint8_t*>(Guest + 1) = 57;
  invalidate(Guest);
  execute(1, Guest + 0x2000, 57);
  check(old.expired(), "unused generation was not reclaimed");

  auto retained = manager.GetLatest();
  const auto retained_base = reinterpret_cast<uintptr_t>(retained->GetBufferBase());
  old = retained;
  retained.reset();
  threads[0]->CurrentFrame->SignalHandlerRefCounter = 1;
  context->ClearCodeCache(threads[0]);
  context->ClearCodeCache(threads[1]);
  check(!old.expired() && threads[0]->CPUBackend->IsAddressInCodeBuffer(retained_base), "signal reference did not retain old code");
  threads[0]->CurrentFrame->SignalHandlerRefCounter = 0;
  context->ClearCodeCache(threads[0]);
  check(old.expired(), "signal-held generation was not released");
  for (auto* thread : threads) {
    auto* stack = thread->CallRetStackBase;
    context->DestroyThread(thread);
    munmap(stack, stack_size);
  }
  context.reset();
  check(executable.empty(), "executable segments leaked");
  munmap(reinterpret_cast<void*>(Guest), 0x10000);
  FEXCore::Allocator::mmap = ::mmap;
  FEXCore::Allocator::munmap = ::munmap;
  printf("FEX segmented cache %u: distant calls, unchanged translations, invalidation, budget rollover, thread and signal lifetimes passed\n", wide ? 64 : 32);
}
