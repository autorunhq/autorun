#!/bin/sh
set -eu
probe="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="${WINE_NX_FEX_DIR:-$probe/toolchains/fex-2609}"
build="${WINE_NX_FEX_CORE_BUILD_DIR:-$probe/toolchains/build-fex-2609-core-check}"
host_cc="${WINE_NX_HOST_CC:-/usr/bin/clang}"
host_cxx="${WINE_NX_HOST_CXX:-/usr/bin/clang++}"
test "$(uname -s)" = Linux && test "$(uname -m)" = aarch64
if git -C "$source_dir" apply --reverse --check "$probe/fex/horizon.patch" 2>/dev/null; then
    :
elif [ -z "$(git -C "$source_dir" status --porcelain)" ]; then
    git -C "$source_dir" apply --check "$probe/fex/horizon.patch"
    git -C "$source_dir" apply "$probe/fex/horizon.patch"
else
    echo "FEX has unexpected changes; no files were reset." >&2
    exit 1
fi
cmake -S "$source_dir" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$host_cc" -DCMAKE_CXX_COMPILER="$host_cxx" \
    -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DENABLE_LTO=OFF -DENABLE_OFFLINE_TELEMETRY=OFF -DBUILD_STEAM_SUPPORT=ON \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_CXX_FLAGS="-DFEX_HORIZON_CACHE_TEST -I$probe/fex"
cmake --build "$build" --target FEXCore JemallocDummy -j "${WINE_NX_JOBS:-4}"
for test in fex_pop_fault fex_lookup_cache fex_code_buffer fex_writable_code fex_segmented_cache; do
    "$host_cxx" -std=c++20 -O2 -g -Wall -Wextra -Werror \
        -DARCHITECTURE_arm64=1 -DFEX_DISABLE_TELEMETRY=1 -DFEX_HORIZON_CACHE_TEST \
        -I"$probe/fex" -isystem "$source_dir/FEXCore/include" -isystem "$source_dir/FEXHeaderUtils" \
        -isystem "$source_dir/FEXCore/Source" -isystem "$source_dir/Source" \
        -isystem "$source_dir/External/fmt/include" -isystem "$source_dir/External/unordered_dense/include" \
        -isystem "$source_dir/External/xxhash" \
        -isystem "$build/generated" -isystem "$build/include" -isystem "$build/FEXCore/Source" \
        "$probe/tests/$test.cpp" -Wl,--start-group \
        "$build/FEXCore/Source/libFEXCore.a" "$build/FEXCore/Source/libFEXCore_Base.a" \
        "$build/FEXCore/Source/libJemallocDummy.a" "$build/External/fmt/libfmt.a" \
        "$build/External/xxhash/cmake_unofficial/libxxhash.a" "$build/External/cephes/libcephes_128bit.a" \
        "$build/External/SoftFloat-3e/libsoftfloat_3e.a" -Wl,--end-group -pthread -o "$build/$test"
done
"$build/fex_code_buffer"
for mode in 32 64; do
    "$build/fex_segmented_cache" "$mode"
    "$build/fex_writable_code" "$mode"
    "$build/fex_pop_fault" "$mode"
    "$build/fex_lookup_cache" "$mode" fixed stock
    "$build/fex_lookup_cache" "$mode" dynamic stock
    "$build/fex_lookup_cache" "$mode" fixed wide
    "$build/fex_lookup_cache" "$mode" dynamic wide
done
