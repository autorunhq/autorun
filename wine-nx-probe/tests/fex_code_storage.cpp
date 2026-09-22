#include "../fex/code_storage.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <sys/mman.h>
#include <thread>
#include <vector>

static constexpr size_t MiB = 1024 * 1024;
static std::map<void*, size_t> allocations;
static std::vector<size_t> requests;
static size_t allocation_limit = 64 * MiB;
static bool fail_with_null;

static void* allocate(size_t size) {
  requests.push_back(size);
  if (size > allocation_limit) return fail_with_null ? nullptr : MAP_FAILED;
  auto* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(base != MAP_FAILED && allocations.emplace(base, size).second);
  return base;
}

static void release(void* base, size_t size) {
  assert(allocations.at(base) == size);
  allocations.erase(base);
  assert(!munmap(base, size));
}

int main() {
  using WineNX::FEX::CodeStorage;
  {
    CodeStorage storage;
    auto* first = static_cast<uint8_t*>(allocate(16 * MiB));
    storage.Initialize(first, 16 * MiB, 112 * MiB, allocate, release);
    auto a = storage.Allocate(16 * MiB - 4096);
    assert(a.BufferBase == first && a.BufferAllocationOffset == first);
    *a.BufferAllocationOffset = 0x57;
    auto b = storage.Allocate(32 * MiB - 4096);
    assert(b.BufferBase && b.BufferBase != first);
    auto c = storage.Allocate(64 * MiB - 4096);
    assert(c.BufferBase && c.BufferBase != b.BufferBase);
    assert(storage.Capacity() == 112 * MiB && storage.Used() == 112 * MiB - 3 * 4096);
    assert(!storage.Allocate(16).BufferBase && !storage.Allocate(SIZE_MAX).BufferBase);
    assert(*first == 0x57 && allocations.size() == 3);
    for (auto [base, size] : allocations) {
      assert(storage.Contains(reinterpret_cast<uintptr_t>(base)));
      assert(storage.Contains(reinterpret_cast<uintptr_t>(base) + size - 4097));
      assert(!storage.Contains(reinterpret_cast<uintptr_t>(base) + size - 4096));
    }
    storage.Reset();
    assert(!storage.Used());
    auto reuse = storage.Allocate(64 * MiB - 4096);
    assert(reuse.BufferBase == c.BufferBase && allocations.size() == 3);
  }
  assert(allocations.empty());
  for (bool null_failure : {false, true}) {
    fail_with_null = null_failure;
    allocation_limit = 64 * MiB;
    {
      CodeStorage storage;
      auto* first = allocate(16 * MiB);
      storage.Initialize(first, 16 * MiB, 64 * MiB, allocate, release);
      assert(storage.Allocate(16 * MiB - 4096).BufferBase == first);
      requests.clear();
      allocation_limit = 16 * MiB;
      assert(storage.Allocate(4096).BufferBase);
      assert((requests == std::vector<size_t>{32 * MiB, 16 * MiB}));
      allocation_limit = 0;
      assert(!storage.Allocate(16 * MiB - 4096).BufferBase);
      assert(storage.Capacity() == 32 * MiB && allocations.size() == 2);
      assert(storage.Allocate(4096).BufferBase);
    }
    assert(allocations.empty());
  }
  allocation_limit = 64 * MiB;
  {
    CodeStorage storage;
    auto* first = allocate(16 * MiB);
    storage.Initialize(first, 16 * MiB, 128 * MiB, allocate, release);
    std::vector<uintptr_t> results[16];
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 16; ++i) threads.emplace_back([&, i] {
      for (;;) {
        auto result = storage.Allocate(4096);
        if (!result.BufferBase) break;
        const auto address = reinterpret_cast<uintptr_t>(result.BufferAllocationOffset);
        assert(!(address & 15) && storage.Contains(address));
        *result.BufferAllocationOffset = i;
        results[i].push_back(address);
      }
    });
    for (auto& thread : threads) thread.join();
    std::vector<uintptr_t> all;
    for (unsigned i = 0; i < 16; ++i) for (auto address : results[i]) {
      assert(*reinterpret_cast<uint8_t*>(address) == i);
      all.push_back(address);
    }
    std::sort(all.begin(), all.end());
    for (size_t i = 1; i < all.size(); ++i) assert(all[i] - all[i - 1] >= 4096);
    assert(storage.Capacity() == 128 * MiB && all.size() * 4096 == storage.Used());
    assert(storage.Used() == storage.Capacity() - allocations.size() * 4096);
  }
  assert(allocations.empty());
  puts("FEX code storage: segmented growth, bounded capacity, retained pointers, reuse, failures and 16 concurrent writers passed");
}
