#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace WineNX::FEX {

inline bool WideHost() {
  const char* value = std::getenv("WINE_NX_FEX_WIDE_HOST");
  return value && value[0] == '1' && value[1] == '\0';
}

inline uint64_t LookupAddressSpace(bool guest64) {
  return WideHost() ? (guest64 ? 1ULL << 36 : 1ULL << 32) : 1ULL << 28;
}

inline size_t CodeCacheLimit(bool guest64) {
  return (WideHost() || guest64 ? 512ULL : 256ULL) * 1024 * 1024;
}

inline size_t L1Entries() {
  return WideHost() ? 256 * 1024 : 64 * 1024;
}

inline size_t L2BackingSize() {
  return (WideHost() ? 32 : 4) * 1024 * 1024;
}

}
