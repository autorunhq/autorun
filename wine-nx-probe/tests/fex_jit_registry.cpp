#include "../fex/jit_registry.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

using WineNX::FEX::JitMapping;
using WineNX::FEX::JitRegistry;

static std::array<JitMapping, 32> linear;

__attribute__((noinline)) static uintptr_t linear_lookup(uintptr_t address)
{
    for (const auto& entry : linear) {
        const auto rx = entry.RX.load(std::memory_order_acquire);
        if (rx && address >= rx && address - rx < entry.Size.load(std::memory_order_relaxed))
            return entry.RW.load(std::memory_order_relaxed) + address - rx;
    }
    return address;
}

__attribute__((noinline)) static uintptr_t filtered_lookup(const JitRegistry& registry, uintptr_t address)
{
    return registry.WritableAddress(address);
}

int main()
{
    JitRegistry registry;
    std::vector<JitMapping*> entries;
    constexpr uintptr_t region = 2 * 1024 * 1024;
    constexpr uintptr_t wrap = 512ULL * 1024 * 1024 * 1024;
    for (uintptr_t i = 0; i < 96; ++i) {
        auto* entry = registry.FindFree();
        assert(entry);
        const auto rx = (i + 1) * region - 4096;
        const auto rw = rx + 0x100000000ULL;
        registry.Publish(entry, rx, rw, 8192);
        assert(registry.WritableAddress(rx) == rw);
        assert(registry.WritableAddress(rx + 8191) == rw + 8191);
        assert(registry.WritableAddress(rx - 1) == rx - 1);
        assert(registry.WritableAddress(rx + 8192) == rx + 8192);
        assert(registry.WritableAddress(rx + wrap) == rx + wrap);
        entries.push_back(entry);
    }
    for (auto* entry : entries) {
        const auto rx = entry->RX.exchange(0, std::memory_order_release);
        assert(registry.WritableAddress(rx) == rx);
    }
    auto* entry = registry.FindFree();
    registry.Publish(entry, wrap - 4096, wrap + 4096, 8192);
    assert(registry.WritableAddress(wrap - 1) == wrap + 8191);
    assert(registry.WritableAddress(wrap + 4095) == wrap + 12287);
    entry->RX.store(0, std::memory_order_release);

    JitRegistry loaded;
    for (uintptr_t i = 0; i < 19; ++i) {
        const auto rx = 0x44245f7000ULL + i * 64 * 1024 * 1024;
        loaded.Publish(loaded.FindFree(), rx, rx + 0x100000000ULL, 64 * 1024 * 1024);
        linear[i].RX.store(rx);
        linear[i].RW.store(rx + 0x100000000ULL);
        linear[i].Size.store(64 * 1024 * 1024);
    }
    constexpr unsigned iterations = 5000000;
    uintptr_t baseline = 0, filtered = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) baseline += linear_lookup(0x7fd66d2af0ULL + (i & 4095));
    const auto middle = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) filtered += filtered_lookup(loaded, 0x7fd66d2af0ULL + (i & 4095));
    const auto end = std::chrono::steady_clock::now();
    assert(baseline == filtered);
    printf("FEX JIT registry: region boundaries, hash collisions, reuse and growth passed; miss lookup %.2f -> %.2f ns\n",
           std::chrono::duration<double, std::nano>(middle - start).count() / iterations,
           std::chrono::duration<double, std::nano>(end - middle).count() / iterations);
}
