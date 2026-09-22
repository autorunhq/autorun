#!/bin/sh
set -eu
probe="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="${WINE_NX_FEX_DIR:-$probe/toolchains/fex-2609}"
build="${WINE_NX_FEX_BUILD_DIR:-$probe/toolchains/build-fex-2609-horizon}"
wow64_build="$build-wow64"
revision=395b132f346b1a45def246d10c52245edba1ef02
patch="$probe/fex/horizon.patch"

if [ -n "${WINE_NX_LLVM_MINGW:-}" ]; then
    export PATH="$WINE_NX_LLVM_MINGW/bin:$PATH"
fi
for tool in git cmake ninja python3 llvm-readobj llvm-strip arm64ec-w64-mingw32-clang arm64ec-w64-mingw32-clang++ aarch64-w64-mingw32-clang aarch64-w64-mingw32-clang++; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; set WINE_NX_LLVM_MINGW." >&2; exit 1; }
done
if [ ! -d "$source_dir/.git" ]; then
    if [ -e "$source_dir" ]; then
        echo "Refusing to overwrite $source_dir" >&2
        exit 1
    fi
    mkdir -p "$source_dir"
    git -C "$source_dir" init -q
    git -C "$source_dir" remote add origin https://github.com/FEX-Emu/FEX.git
    git -C "$source_dir" fetch --depth=1 origin "$revision"
    git -C "$source_dir" checkout -q --detach FETCH_HEAD
fi
if [ "$(git -C "$source_dir" rev-parse HEAD)" != "$revision" ]; then
    echo "FEX must be at $revision; no files were reset." >&2
    exit 1
fi
if git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
    :
elif [ -z "$(git -C "$source_dir" status --porcelain)" ]; then
    git -C "$source_dir" apply --check "$patch"
    git -C "$source_dir" apply "$patch"
else
    echo "FEX has unexpected changes; no files were reset." >&2
    exit 1
fi
git -C "$source_dir" submodule update --init --depth=1 \
    External/fmt External/xxhash External/range-v3 External/unordered_dense \
    External/rpmalloc Source/Common/cpp-optparse
source_dir="$(CDPATH= cd -- "$source_dir" && pwd)"

build_module()
{
    cmake -S "$source_dir" -B "$1" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$source_dir/Data/CMake/toolchain_mingw.cmake" \
        -DMINGW_TRIPLE="$2" -DCMAKE_BUILD_TYPE=Release \
        -DOVERRIDE_VERSION=FEX-2609 -DOVERRIDE_HASH="$revision" \
        -DENABLE_LTO=OFF -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF \
        -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_OFFLINE_TELEMETRY=OFF -DTUNE_CPU=none \
        -DFEX_HORIZON=ON -DFEX_HORIZON_ABI_DIR="$probe/fex"
    cmake --build "$1" --target "$3" -j "${WINE_NX_JOBS:-4}"
}
build_module "$build" arm64ec-w64-mingw32 arm64ecfex
build_module "$wow64_build" aarch64-w64-mingw32 wow64fex
python3 "$probe/tools/fex_payload.py" "$source_dir" "$build/Bin/libarm64ecfex.dll" \
    "$wow64_build/Bin/libwow64fex.dll" "$build/payload"
