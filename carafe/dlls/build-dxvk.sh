#!/bin/sh
# DXVK 3.1.1 with Carafe's patches (carafe/dlls/dxvk), as 32-bit and 64-bit
# Windows DLLs, built the way the DLL repository builds the stock one.
#
#   build-dxvk.sh <DXVK checkout to make> <build directory>
#
# Needs git, meson, ninja, glslang and llvm-mingw (WINE_NX_LLVM_MINGW, or on PATH).
set -eu
here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
src="$1"
build="$2"
revision=b1a1c99ab52b687cf950d62c88bc2fa316b41663

if [ -n "${WINE_NX_LLVM_MINGW:-}" ]; then
    export PATH="$WINE_NX_LLVM_MINGW/bin:$PATH"
fi
for tool in git meson ninja i686-w64-mingw32-clang++ x86_64-w64-mingw32-clang++; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; set WINE_NX_LLVM_MINGW." >&2; exit 1; }
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null ||
    { echo "Missing glslang." >&2; exit 1; }

if [ ! -d "$src/.git" ]; then
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" config core.autocrlf false
    git -C "$src" remote add origin https://github.com/doitsujin/dxvk.git
fi
if [ "$(git -C "$src" rev-parse -q --verify HEAD || true)" != "$revision" ]; then
    git -C "$src" fetch --depth=1 origin "$revision"
    git -C "$src" checkout -q --force --detach FETCH_HEAD
fi
git -C "$src" submodule update --init --recursive --depth=1

git -C "$src" checkout -q --force HEAD -- src
git -C "$src" clean -fdq -- src
for patch in "$here"/dxvk/*.patch; do
    git -C "$src" apply "$patch"
    echo "DXVK: applied $(basename "$patch")"
done

configure()
{
    dir="$build/$1"
    mkdir -p "$dir"
    {
        echo '[binaries]'
        for pair in c:clang cpp:clang++ ar:ar strip:strip windres:windres; do
            echo "${pair%%:*} = '$(command -v "$2-w64-mingw32-${pair#*:}")'"
        done
        printf "\n[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\n"
        printf "system = 'windows'\ncpu_family = '%s'\ncpu = '%s'\nendian = 'little'\n" "$3" "$3"
    } > "$dir/cross.txt"
    meson setup "$dir" "$src" --cross-file "$dir/cross.txt" --buildtype=release --strip \
        --wrap-mode=nodownload -Denable_dxgi=true -Denable_d3d8=true -Denable_d3d9=true \
        -Denable_d3d10=true -Denable_d3d11=true
}

configure x32 i686 x86
configure x64 x86_64 x86_64
for arch in x32 x64; do
    ninja -C "$build/$arch" -j "${WINE_NX_JOBS:-4}" src/d3d8/d3d8.dll src/d3d9/d3d9.dll \
        src/d3d10/d3d10core.dll src/d3d11/d3d11.dll src/dxgi/dxgi.dll
done
