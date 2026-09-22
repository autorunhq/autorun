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

  uintptr_t WritableAddress(uintptr_t Address) const {
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
