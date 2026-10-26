#!/bin/sh
# Type-check FEX's Horizon changes without Docker or llvm-mingw: cross-compile
# FEXCore for aarch64 with the Tegra X1 tuning and the Horizon cache headers,
# on any x86_64 Linux host.
#
#   sh horizon-wine/check-fex-cross.sh [--clean]
#
# This is not the release build. The two modules a card runs
# (libarm64ecfex.dll and libwow64fex.dll) are Windows ARM64EC/ARM64 PE files and
# are built with llvm-mingw by autorun-horizon-dlls/tools/build-fex.sh, or by
# fex-variant-release.yml in the FEX tree. What this catches is the ordinary
# thing a change breaks: a file that no longer compiles, a type or a field that
# moved, a header the Horizon code needs.
#
# Needs cmake, ninja and zig (pip install ziglang; zig's clang is the cross
# compiler) and the FEX checkout with its submodules:
#
#   git -C horizon-wine/toolchains/fex-2609 submodule update --init --depth=1 \
#       External/fmt External/xxhash External/range-v3 External/unordered_dense \
#       External/rpmalloc Source/Common/cpp-optparse
#
# zig rejects -march= for aarch64, so the wrapper below turns the Horizon
# build's -march=armv8-a+crc into zig's spelling of the same CPU.
set -eu
horizon_wine="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="${WINE_NX_FEX_DIR:-$horizon_wine/toolchains/fex-2609}"
abi_dir="${WINE_NX_FEX_ABI_DIR:-$source_dir/Source/Windows/include}"
build="${WINE_NX_FEX_CROSS_BUILD_DIR:-$horizon_wine/toolchains/build-fex-cross}"
jobs="${WINE_NX_JOBS:-4}"
clean=0
[ "${1:-}" = "--clean" ] && clean=1

if [ ! -d "$source_dir/FEXCore" ]; then
    echo "No FEX checkout at $source_dir; set WINE_NX_FEX_DIR." >&2
    exit 1
fi
if [ ! -f "$abi_dir/cache_policy.h" ]; then
    echo "No Horizon ABI headers at $abi_dir; set WINE_NX_FEX_ABI_DIR." >&2
    exit 1
fi
for tool in cmake ninja python3; do
    command -v "$tool" >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
zig="$(command -v zig 2>/dev/null || true)"
if [ -z "$zig" ]; then
    for candidate in "$HOME/.local/bin/zig" \
                     "$(python3 -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig"))' 2>/dev/null || true)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then zig="$candidate"; break; fi
    done
fi
[ -n "$zig" ] || { echo "Missing zig (pip install ziglang)." >&2; exit 1; }

[ "$clean" = 1 ] && rm -rf "$build"
cc_dir="$build/toolchain"
mkdir -p "$cc_dir"
for kind in cc cxx; do
    cat > "$cc_dir/aarch64-$kind" <<EOF
#!/usr/bin/env python3
import os, sys
strip = ("-march=", "-mcpu=")
args = [a for a in sys.argv[1:] if not a.startswith(strip)]
os.execv("$zig", ["zig", "$([ "$kind" = cxx ] && echo c++ || echo cc)", "-target", "aarch64-linux-gnu", "-mcpu=cortex_a57", *args])
EOF
    chmod +x "$cc_dir/aarch64-$kind"
done
cat > "$cc_dir/aarch64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $cc_dir/aarch64-cc)
set(CMAKE_CXX_COMPILER $cc_dir/aarch64-cxx)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF

# FEX_HORIZON_CACHE_TEST compiles the Horizon cache policy into FEXCore on a
# host that is not Windows, which is the same thing check-fex-core.sh does on an
# aarch64 host; the headers come from the ABI directory.
mkdir -p "$build"
if ! cmake -S "$source_dir" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$cc_dir/aarch64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_LTO=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF \
    -DFEX_TUNE_TEGRA_X1=ON \
    -DCMAKE_CXX_FLAGS="-DFEX_HORIZON_CACHE_TEST -I$abi_dir" > "$build/configure.log" 2>&1; then
    echo "FEX configure failed:" >&2
    tail -20 "$build/configure.log" >&2
    exit 1
fi
cmake --build "$build" --target FEXCore -j "$jobs"
echo "FEXCore compiles for aarch64 with the Horizon changes."
