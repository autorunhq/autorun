#ifndef WINE_NX_FEX_CODE_STORAGE_H
#define WINE_NX_FEX_CODE_STORAGE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace WineNX::FEX {
class CodeStorage {
public:
  using AllocateFn = void* (*)(size_t);
  using FreeFn = void (*)(void*, size_t);
  struct Allocation {
    const uint8_t* BufferBase {};
    uint8_t* BufferAllocationOffset {};
  };

  void Initialize(void* Base, size_t Size, size_t Limit, AllocateFn Allocate, FreeFn Free) {
    Chunks[0].Base = static_cast<uint8_t*>(Base);
    Chunks[0].Size = Size;
    ByteLimit = Limit > Size ? Limit : Size;
    AllocateMemory = Allocate;
    FreeMemory = Free;
    Total.store(Size, std::memory_order_relaxed);
    Largest.store(Size, std::memory_order_relaxed);
    Count.store(1, std::memory_order_release);
  }

  ~CodeStorage() {
    for (unsigned i = 0; i < Count.load(std::memory_order_relaxed); ++i)
      FreeMemory(Chunks[i].Base, Chunks[i].Size);
  }

  Allocation Allocate(size_t Size) {
    if (!Size || Size > MaxChunk - GuardSize) return {};
    Size = (Size + 15) & ~size_t(15);
    unsigned Index = Current.load(std::memory_order_acquire);
    if (auto Result = TryAllocate(Chunks[Index], Size); Result.BufferBase) return Result;

    std::scoped_lock Lock(GrowLock);
    Index = Current.load(std::memory_order_relaxed);
    if (auto Result = TryAllocate(Chunks[Index], Size); Result.BufferBase) return Result;

    const auto NumChunks = Count.load(std::memory_order_relaxed);
    while (Index + 1 < NumChunks) {
      Current.store(++Index, std::memory_order_release);
      if (auto Result = TryAllocate(Chunks[Index], Size); Result.BufferBase) return Result;
    }
    const auto Bytes = Total.load(std::memory_order_relaxed);
    if (NumChunks == MaxChunks || Bytes >= ByteLimit) return {};
    size_t Minimum = (Size + GuardSize * 2 - 1) & ~(GuardSize - 1);
    if (Minimum < MinChunk) Minimum = MinChunk;
    if (Minimum > ByteLimit - Bytes) return {};
    size_t Capacity = Chunks[Index].Size * 2;
    if (Capacity < Minimum) Capacity = Minimum;
    if (Capacity > MaxChunk) Capacity = MaxChunk;
    if (Capacity > ByteLimit - Bytes) Capacity = ByteLimit - Bytes;
    Capacity &= ~(GuardSize - 1);
    void* Base;
    for (;;) {
      Base = AllocateMemory(Capacity);
      if (Base && Base != reinterpret_cast<void*>(-1)) break;
      if (Capacity == Minimum) return {};
      Capacity = (Capacity / 2) & ~(GuardSize - 1);
      if (Capacity < Minimum) Capacity = Minimum;
    }

    auto& Next = Chunks[NumChunks];
    Next.Base = static_cast<uint8_t*>(Base);
    Next.Size = Capacity;
    auto Result = TryAllocate(Next, Size);
    Total.store(Bytes + Capacity, std::memory_order_relaxed);
    if (Capacity > Largest.load(std::memory_order_relaxed)) Largest.store(Capacity, std::memory_order_relaxed);
    // Published chunks remain owned until the entire cache generation is unused.
    Count.store(NumChunks + 1, std::memory_order_release);
    Current.store(NumChunks, std::memory_order_release);
    return Result;
  }

  bool Contains(uintptr_t Address) const {
    const auto NumChunks = Count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < NumChunks; ++i) {
      const auto Base = reinterpret_cast<uintptr_t>(Chunks[i].Base);
      if (Address >= Base && Address - Base < Chunks[i].Size - GuardSize) return true;
    }
    return false;
  }

  size_t Capacity() const { return Total.load(std::memory_order_relaxed); }
  size_t UsableSize() const { return Largest.load(std::memory_order_relaxed) - GuardSize; }
  size_t Used() const {
    size_t Bytes = 0;
    const auto NumChunks = Count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < NumChunks; ++i) Bytes += Chunks[i].Used.load(std::memory_order_relaxed);
    return Bytes;
  }

  // Reset, like destruction, requires exclusive ownership of the generation.
  void Reset() {
    for (unsigned i = 0; i < Count.load(std::memory_order_relaxed); ++i)
      Chunks[i].Used.store(0, std::memory_order_relaxed);
    Current.store(0, std::memory_order_release);
  }

private:
  static constexpr size_t GuardSize = 4096;
  static constexpr size_t MinChunk = 16 * 1024 * 1024;
  static constexpr size_t MaxChunk = 64 * 1024 * 1024;
  static constexpr unsigned MaxChunks = 32;
  struct Chunk {
    uint8_t* Base {};
    size_t Size {};
    std::atomic<size_t> Used {};
  };

  static Allocation TryAllocate(Chunk& Entry, size_t Size) {
    auto Used = Entry.Used.load(std::memory_order_relaxed);
    while (Used <= Entry.Size - GuardSize && Size <= Entry.Size - GuardSize - Used) {
      if (Entry.Used.compare_exchange_weak(Used, Used + Size, std::memory_order_relaxed))
        return {Entry.Base, Entry.Base + Used};
    }
    return {};
  }

  Chunk Chunks[MaxChunks];
  std::atomic<unsigned> Count {}, Current {};
  std::atomic<size_t> Total {}, Largest {};
  size_t ByteLimit {};
  AllocateFn AllocateMemory {};
  FreeFn FreeMemory {};
  std::mutex GrowLock;
};
}
#endif
