#!/bin/sh
# FEX-2609 for 32-bit programs (libwow64fex.dll) with Carafe's patches, built
# against this checkout's side of the ABI (horizon-wine/fex), the way the DLL
# repository builds the stock one: its Horizon patch first, then Carafe's
# series from carafe/dlls/fex. A patch of the series that horizon.patch already
# holds is passed over; any other that does not apply stops the build.
#
#   build-fex.sh <FEX checkout to make> <build directory> [experimental]
#
# With "experimental", the patches of carafe/dlls/fex-experimental go on top,
# in order; every one of them has to apply. That is the build experiments are
# made in, released apart from Carafe's (fex-experimental-<version>).
#
# Needs git, cmake, ninja, python3 and llvm-mingw (WINE_NX_LLVM_MINGW, or on PATH).
set -eu
here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
autorun="$(CDPATH= cd -- "$here/../.." && pwd)"
src="$1"
build="$2"
flavour="${3:-carafe}"
case "$flavour" in
    carafe|experimental) ;;
    *) echo "build-fex.sh: the third argument is \"experimental\" or nothing." >&2; exit 1;;
esac
horizon_wine="$autorun/horizon-wine"
revision=395b132f346b1a45def246d10c52245edba1ef02

if [ -n "${WINE_NX_LLVM_MINGW:-}" ]; then
    export PATH="$WINE_NX_LLVM_MINGW/bin:$PATH"
fi
for tool in git cmake ninja python3 llvm-strip aarch64-w64-mingw32-clang aarch64-w64-mingw32-clang++; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; set WINE_NX_LLVM_MINGW." >&2; exit 1; }
done

if [ ! -d "$src/.git" ]; then
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" remote add origin https://github.com/FEX-Emu/FEX.git
fi
if [ "$(git -C "$src" rev-parse -q --verify HEAD || true)" != "$revision" ]; then
    git -C "$src" fetch --depth=1 origin "$revision"
    git -C "$src" checkout -q --force --detach FETCH_HEAD
fi
git -C "$src" submodule update --init --depth=1 \
    External/fmt External/xxhash External/range-v3 External/unordered_dense \
    External/rpmalloc Source/Common/cpp-optparse

git -C "$src" checkout -q --force HEAD -- .
git -C "$src" clean -fdq
git -C "$src" apply "$horizon_wine/fex/horizon.patch"
echo "FEX: applied horizon.patch"
applied=0
for patch in "$here"/fex/*.patch; do
    name="$(basename "$patch")"
    if git -C "$src" apply --check "$patch" 2>/dev/null; then
        git -C "$src" apply "$patch"
        applied=$((applied + 1))
        echo "FEX: applied $name"
    elif git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
        echo "FEX: $name is already in horizon.patch"
    else
        echo "FEX: $name does not apply" >&2
        git -C "$src" apply --check "$patch" || true
        exit 1
    fi
done
echo "FEX: $applied patches of Carafe's applied"
if [ "$flavour" = experimental ]; then
    tried=0
    for patch in "$here"/fex-experimental/*.patch; do
        [ -f "$patch" ] || continue
        if ! git -C "$src" apply "$patch"; then
            echo "FEX: experimental $(basename "$patch") does not apply" >&2
            exit 1
        fi
        tried=$((tried + 1))
        echo "FEX: applied experimental $(basename "$patch")"
    done
    echo "FEX: $tried experimental patches applied"
fi

# Newer CMake refuses the old minimum versions some of FEX's externals name.
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$src/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE=aarch64-w64-mingw32 -DCMAKE_BUILD_TYPE=Release \
    -DOVERRIDE_VERSION=FEX-2609 -DOVERRIDE_HASH="$revision" \
    -DENABLE_LTO=OFF -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_OFFLINE_TELEMETRY=OFF -DTUNE_CPU=none \
    -DFEX_HORIZON=ON -DFEX_HORIZON_ABI_DIR="$horizon_wine/fex"
cmake --build "$build" --target wow64fex -j "${WINE_NX_JOBS:-4}"
ls -l "$build/Bin/libwow64fex.dll"
