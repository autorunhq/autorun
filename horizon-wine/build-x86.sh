#!/bin/sh
# The x86 runtime Autorun ships: 32-bit programs through WoW64 and the Box64
# dynarec, linked with mesa-switch (OpenGL through nvc0, Vulkan through NVK),
# into build-switch-wow64-mesa-switch, in the switch-dev image (switch-dev.txt).
# Build the AMD64 runtime first: build-amd64-components.sh configures the Wine
# tree whose headers both runtimes compile against.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="$root/horizon-wine/build-wine-amd64-pe"
# The toolchain build.sh fetches: its name is per-platform, and the build
# exports WINE_NX_LLVM_MINGW with the one it chose.
if [ -z "${WINE_NX_LLVM_MINGW:-}" ]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-*) llvm_name="llvm-mingw-20260505-ucrt-macos-universal" ;;
        Linux-aarch64|Linux-arm64) llvm_name="llvm-mingw-20260505-ucrt-ubuntu-22.04-aarch64" ;;
        *) llvm_name="llvm-mingw-20260505-ucrt-ubuntu-22.04-x86_64" ;;
    esac
    WINE_NX_LLVM_MINGW="$root/horizon-wine/toolchains/$llvm_name"
fi
export PATH="$WINE_NX_LLVM_MINGW/bin:/opt/homebrew/opt/bison/bin:$PATH"
if [ ! -f "$pe/Makefile" ]; then
    echo "No Wine tree in build-wine-amd64-pe; build-amd64-components.sh configures it." >&2
    exit 1
fi
# The headers the runtime compiles against. The Windows modules it runs are the
# DLL repository's (horizon-dlls/tools/build-dlls.py); none are built here.
make -C "$pe" -j8 include/all
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
sh "$root/horizon-wine/tools/bootstrap-wma.sh"
# git in the image must trust the mounted tree: bind mounts keep the host's
# UIDs, and the container runs as root.
docker run --rm -v "$root:/work" -w /work -e NX_MESA="${WINE_NX_MESA_SWITCH_DIR:-}" \
    -e GIT_CONFIG_COUNT=2 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -e GIT_CONFIG_KEY_1=safe.directory -e GIT_CONFIG_VALUE_1='/work/horizon-wine/vendor/box64' \
    "${WINE_NX_DEVKIT_IMAGE:-$(cat "$root/horizon-wine/switch-dev.txt")}" sh -ec '
    cmake -S horizon-wine -B horizon-wine/build-switch-wow64-mesa-switch -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/horizon-wine/build-wine-amd64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release \
        ${NX_MESA:+-DWINE_NX_MESA_SWITCH_DIR="$NX_MESA"}
    cmake --build horizon-wine/build-switch-wow64-mesa-switch --target wine-nx-runtime-nro -j 8
    '
