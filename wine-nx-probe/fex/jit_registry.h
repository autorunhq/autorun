#ifndef WINE_NX_FEX_JIT_REGISTRY_H
#define WINE_NX_FEX_JIT_REGISTRY_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace WineNX::FEX {
struct JitMapping {
  std::atomic<uintptr_t> RX {}, RW {};
  std::atomic<size_t> Size {};
};

class JitRegistry {
  static constexpr size_t RegionShift = 21;
  static constexpr size_t FilterWords = 4096;
  std::array<std::atomic<uint64_t>, FilterWords> Regions {};

  struct Page {
    std::array<JitMapping, 32> Entries;
    std::atomic<Page*> Next {};
  } First;

public:
  JitRegistry() = default;
  JitRegistry(const JitRegistry&) = delete;
  JitRegistry& operator=(const JitRegistry&) = delete;

  ~JitRegistry() {
    auto* Current = First.Next.load(std::memory_order_relaxed);
    while (Current) {
      auto* Next = Current->Next.load(std::memory_order_relaxed);
      delete Current;
      Current = Next;
    }
  }

  // Writers hold the bridge mutex; published pages remain valid until unload.
  JitMapping* FindFree() {
    for (auto* Current = &First;;) {
      for (auto& Entry : Current->Entries) {
        if (!Entry.RX.load(std::memory_order_relaxed)) return &Entry;
      }
      auto* Next = Current->Next.load(std::memory_order_relaxed);
      if (!Next) {
        Next = new (std::nothrow) Page {};
        if (!Next) return nullptr;
        Current->Next.store(Next, std::memory_order_release);
      }
      Current = Next;
    }
  }

  JitMapping* FindBase(uintptr_t Address) {
    if (!Address) return nullptr;
    for (auto* Current = &First; Current; Current = Current->Next.load(std::memory_order_acquire)) {
      for (auto& Entry : Current->Entries) {
        if (Entry.RX.load(std::memory_order_acquire) == Address) return &Entry;
      }
    }
    return nullptr;
  }

  void Publish(JitMapping* Entry, uintptr_t RX, uintptr_t RW, size_t Size) {
    // Bits remain set after release, so concurrent reuse cannot hide a mapping.
    for (auto Region = RX >> RegionShift; Region <= (RX + Size - 1) >> RegionShift; ++Region)
      Regions[(Region >> 6) % FilterWords].fetch_or(uint64_t{1} << (Region & 63), std::memory_order_release);
    Entry->RW.store(RW, std::memory_order_relaxed);
    Entry->Size.store(Size, std::memory_order_relaxed);
    Entry->RX.store(RX, std::memory_order_release);
  }

  uintptr_t WritableAddress(uintptr_t Address) const {
    const auto Region = Address >> RegionShift;
    if (!(Regions[(Region >> 6) % FilterWords].load(std::memory_order_acquire) & (uint64_t{1} << (Region & 63))))
      return Address;
    for (auto* Current = &First; Current; Current = Current->Next.load(std::memory_order_acquire)) {
      for (auto& Entry : Current->Entries) {
        const auto RX = Entry.RX.load(std::memory_order_acquire);
        if (RX && Address >= RX && Address - RX < Entry.Size.load(std::memory_order_relaxed))
          return Entry.RW.load(std::memory_order_relaxed) + Address - RX;
      }
    }
    return Address;
  }
};
}
#endif
