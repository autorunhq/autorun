#include "Interface/Core/LookupCache.h"
#include "cache_policy.h"
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/mman.h>
#include <vector>

static void check(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FEX lookup: %s\n", message); std::exit(1); }
}

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  bool wide_guest = false;
  std::map<uint64_t, uint64_t> reservations;
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { std::abort(); }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t address) override {
    return {address & ~4095ULL, (address & ~4095ULL) + 4096, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  void MarkOvercommitRange(uint64_t start, uint64_t length) override {
    const size_t expected = WineNX::FEX::LookupAddressSpace(wide_guest) / 4096 * 8 +
                            WineNX::FEX::L2BackingSize() + WineNX::FEX::L1Entries() * 16;
    check(length == expected, "unexpected per-thread reservation");
    check(reservations.emplace(start, length).second, "duplicate reservation");
  }
  void UnmarkOvercommitRange(uint64_t start, uint64_t length) override {
    check(reservations.at(start) == length, "wrong release size");
    reservations.erase(start);
  }
};

static void evict_l1(FEXCore::LookupCache& cache, uint64_t address) {
  auto entries = reinterpret_cast<FEXCore::LookupCache::LookupCacheEntry*>(cache.GetL1Pointer());
  entries[address & (cache.GetScaledL1PointerMask() / sizeof(*entries))].GuestCode = 0;
}

static void add(FEXCore::Core::InternalThreadState* thread, uint64_t address, uintptr_t host) {
  const std::array<uint64_t, 1> pages {address & ~4095ULL};
  thread->LookupCache->AddBlockMapping(thread, address, pages, reinterpret_cast<void*>(host));
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const bool wide = std::atoi(argv[1]) == 64, dynamic = !strcmp(argv[2], "dynamic");
  const bool wide_host = !strcmp(argv[3], "wide");
  if (wide_host) setenv("WINE_NX_FEX_WIDE_HOST", "1", 1);
  else unsetenv("WINE_NX_FEX_WIDE_HOST");
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, wide ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DYNAMICL1CACHE, dynamic ? "1" : "0");
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  FEXCore::HostFeatures features {};
  features.DCacheLineLog2 = 4;
  features.HostType = FEXCore::HostFeatures::HostTypeEnum::Wow64;
  features.CPUMIDRs.assign(4, 0x411fd071);
  Syscalls syscalls;
  syscalls.wide_guest = wide;
  FEXCore::SignalDelegator signals;
  auto context = FEXCore::Context::Context::CreateNewContext(features);
  context->SetSignalDelegator(&signals);
  context->SetSyscallHandler(&syscalls);
  context->EnableExitOnHLT();
  check(context->InitCore(), "core initialization failed");
  std::vector<FEXCore::Core::InternalThreadState*> threads;
  const unsigned thread_count = wide_host ? 4 : 16;
  for (unsigned i = 0; i < thread_count; ++i) threads.push_back(context->CreateThread());
  check(syscalls.reservations.size() == threads.size(), "missing reservations");
  auto* thread = threads.front();
  auto& cache = *thread->LookupCache;
  check(cache.GetVirtualMemorySize() == WineNX::FEX::LookupAddressSpace(wide), "wrong lookup index mask");
  check(cache.GetScaledL1PointerMask() == ((dynamic ? 8192 : WineNX::FEX::L1Entries()) - 1) * 16,
        "wrong L1 mask");

  const uint64_t low = 0x31001000, high = low + (1ULL << 28);
  const auto page_slot = [wide](uint64_t address) {
    auto page = address >> 12;
    page ^= page >> 16;
    return page & ((WineNX::FEX::LookupAddressSpace(wide) >> 12) - 1);
  };
  check(page_slot(low) != page_slot(high), "lookup hash kept a fixed-window collision");
  add(thread, low, 0x12340);
  add(thread, high, 0x56780);
  for (unsigned i = 0; i < 8; ++i) {
    evict_l1(cache, low);
    check(cache.FindBlock(thread, low) == 0x12340, "low-address alias returned wrong block");
    evict_l1(cache, high);
    check(cache.FindBlock(thread, high) == 0x56780, "high-address alias returned wrong block");
  }
  check(threads[1]->LookupCache->FindBlock(threads[1], low) == 0x12340, "shared lookup failed");
  {
    auto lock = cache.AcquireWriteLock();
    cache.Shared->Erase(low, lock);
  }
  check(cache.InvalidateCacheRange(low, 1), "first thread did not invalidate");
  check(threads[1]->LookupCache->InvalidateCacheRange(low, 1), "second thread did not invalidate");
  check(!cache.FindBlock(thread, low), "invalidated block is still cached");
  check(!threads[1]->LookupCache->FindBlock(threads[1], low), "second thread kept invalidated block");
  check(cache.FindBlock(thread, high) == 0x56780, "invalidation lost aliased shared block");

  for (unsigned pass = 0; pass < 2; ++pass) {
    { auto lock = cache.AcquireWriteLock(); cache.ClearThreadLocalCaches(lock); }
    for (unsigned i = 0; i < (wide_host ? 1100U : 600U); ++i) {
      const uint64_t address = 0x32000000 + i * 4096;
      add(thread, address, 0x10000 + i * 16);
      evict_l1(cache, address);
      check(cache.FindBlock(thread, address) == 0x10000 + i * 16, "L2 refill failed");
    }
    evict_l1(cache, 0x32000000);
    check(cache.FindBlock(thread, 0x32000000) == 0x10000, "L2 rollover lost shared mapping");
  }

  const uintptr_t first = wide ? 0x101000000ULL : 0x01000000ULL;
  const uintptr_t second = first + (1ULL << 28);
  for (auto address : {first, second}) {
    check(mmap(reinterpret_cast<void*>(address), 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED, "guest mapping failed");
    const uint8_t code[] = {0xb8, static_cast<uint8_t>(address == first ? 17 : 29), 0, 0, 0, 0xf4};
    memcpy(reinterpret_cast<void*>(address), code, sizeof(code));
  }
  auto stack = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(stack != MAP_FAILED, "call/return stack mapping failed");
  FEXCore::Core::CPUState::gdt_segment segments[32] {};
  segments[0].D = !wide;
  segments[0].L = wide;
  FEXCore::Core::CPUState::SetGDTLimit(&segments[0], 0xfffff);
  auto& state = thread->CurrentFrame->State;
  state.segment_arrays[0] = state.segment_arrays[1] = segments;
  state.callret_sp = reinterpret_cast<uintptr_t>(stack) + 32768;
  for (unsigned i = 0; i < 12; ++i) {
    const uintptr_t address = i & 1 ? second : first;
    state.rip = address;
    context->SetFlagsFromCompactedEFLAGS(thread, 0x202);
    evict_l1(cache, address);
    context->ExecuteThread(thread);
    check(state.gregs[FEXCore::X86State::REG_RAX] == (i & 1 ? 29 : 17), "dispatcher alias returned wrong code");
  }
  for (auto* item : threads) context->DestroyThread(item);
  check(syscalls.reservations.empty(), "thread cache reservation leaked");
  munmap(reinterpret_cast<void*>(first), 4096);
  munmap(reinterpret_cast<void*>(second), 4096);
  munmap(stack, 65536);
  printf("FEX lookup %u %s host %s: %u threads, aliases, invalidation, rollover, dispatcher and cleanup passed\n",
         wide ? 64 : 32, dynamic ? "dynamic" : "fixed", wide_host ? "wide" : "stock", thread_count);
}
