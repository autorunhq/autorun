#!/bin/sh
# The x86 runtime Autorun ships: 32-bit programs through WoW64 and the Box64
# dynarec, linked with mesa-switch (OpenGL through nvc0, Vulkan through NVK),
# into build-switch-wow64-mesa-switch, in the switch-dev image (switch-dev.txt).
# Build the AMD64 runtime first: build-amd64-components.sh configures the Wine
# tree whose headers both runtimes compile against.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="$root/horizon-wine/build-wine-amd64-pe"
# The llvm-mingw build.sh fetched, for this machine's build of it.
llvm="${WINE_NX_LLVM_MINGW:-}"
if [ -z "$llvm" ]; then
    for dir in "$root"/horizon-wine/toolchains/llvm-mingw-*; do
        [ -d "$dir" ] && { llvm="$dir"; break; }
    done
fi
[ -n "$llvm" ] || { echo "No llvm-mingw in horizon-wine/toolchains; build.sh fetches it." >&2; exit 1; }
export PATH="$llvm/bin:$PATH"
if [ ! -f "$pe/Makefile" ]; then
    echo "No Wine tree in build-wine-amd64-pe; build-amd64-components.sh configures it." >&2
    exit 1
fi
# The headers the runtime compiles against. The Windows modules it runs are the
# DLL repository's (horizon-dlls/tools/build-dlls.py); none are built here.
make -C "$pe" -j8 include/all
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
sh "$root/horizon-wine/tools/bootstrap-wma.sh"
# See build-amd64-components.sh: the checkout is the container's to read, even
# when the host owns it.
docker run --rm -v "$root:/work" -w /work -e GIT_CONFIG_COUNT=1 \
    -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -e NX_MESA="${WINE_NX_MESA_SWITCH_DIR:-}" -e NX_BUILD_ID="${WINE_NX_BUILD_ID:-}" \
    "${WINE_NX_DEVKIT_IMAGE:-$(cat "$root/horizon-wine/switch-dev.txt")}" sh -ec '
    cmake -S horizon-wine -B horizon-wine/build-switch-wow64-mesa-switch -G Ninja \
        -DWINE_NX_BUILD_ID="$NX_BUILD_ID" \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/horizon-wine/build-wine-amd64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release \
        ${NX_MESA:+-DWINE_NX_MESA_SWITCH_DIR="$NX_MESA"}
    cmake --build horizon-wine/build-switch-wow64-mesa-switch --target wine-nx-runtime-nro -j 8
    '
