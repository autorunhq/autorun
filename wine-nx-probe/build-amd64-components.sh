#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="${WINE_NX_PE_BUILD_DIR:-$root/wine-nx-probe/build-wine-amd64-pe}"
build="${WINE_NX_BUILD_DIR:-$root/wine-nx-probe/build-switch-amd64}"
jobs="${WINE_NX_JOBS:-8}"
fex=OFF
case "${WINE_NX_FEX:-0}" in
    0) ;;
    1) fex=ON ;;
    *) echo "WINE_NX_FEX must be 0 or 1." >&2; exit 1;;
esac
if { [ "${WINE_NX_DXVK:-0}" = 1 ] || [ "${WINE_NX_VKD3D:-0}" = 1 ]; } && [ -z "${WINE_NX_MESA_SWITCH_DIR:-}" ]; then
    echo "DXVK/VKD3D require WINE_NX_MESA_SWITCH_DIR." >&2
    exit 1
fi
if [ -n "${WINE_NX_LLVM_MINGW:-}" ]; then
    export PATH="$WINE_NX_LLVM_MINGW/bin:$PATH"
fi
for tool in arm64ec-w64-mingw32-clang aarch64-w64-mingw32-clang i686-w64-mingw32-clang x86_64-w64-mingw32-clang x86_64-w64-mingw32-windres llvm-readobj; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; set WINE_NX_LLVM_MINGW." >&2; exit 1; }
done
mkdir -p "$pe" "$build"
pe="$(CDPATH= cd -- "$pe" && pwd)"
build="$(CDPATH= cd -- "$build" && pwd)"
case "$pe" in "$root"/*) ;; *) echo "PE build must be inside the Wine checkout." >&2; exit 1;; esac
case "$build" in "$root"/*) ;; *) echo "Switch build must be inside the Wine checkout." >&2; exit 1;; esac
(
    cd "$pe"
    "$root/configure" --enable-archs=aarch64,arm64ec,i386 \
        --enable-winebox64=aarch64 --enable-winebox64ec=arm64ec \
        --disable-tests --without-x --without-freetype --without-alsa --without-pulse \
        --without-dbus --without-fontconfig --without-udev --without-usb \
        --without-gstreamer --without-vulkan
    make -j"$jobs" include/all
)
sh "$root/wine-nx-probe/tools/bootstrap-box64-core.sh"
sh "$root/wine-nx-probe/tools/bootstrap-libusbhsfs.sh"
if [ -n "${WINE_NX_MESA_SWITCH_DIR:-}" ]; then
    sh "$root/wine-nx-probe/tools/bootstrap-lsfg-vk.sh"
fi
if [ "$fex" = ON ]; then
    sh "$root/wine-nx-probe/build-fex.sh"
fi
docker run --rm --network none --platform linux/arm64 -v "$root:/work" -w /work \
    -e NX_PE="/work/${pe#"$root/"}" -e NX_BUILD="/work/${build#"$root/"}" \
    -e NX_JOBS="$jobs" -e NX_DYNAREC="${WINE_NX_BOX64_DYNAREC:-ON}" \
    -e NX_MESA="${WINE_NX_MESA_SWITCH_DIR:-}" -e NX_FEX="$fex" \
    "${WINE_NX_DEVKIT_IMAGE:-devkitpro/devkita64}" sh -ec '
    cmake -S wine-nx-probe -B "$NX_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/wine-nx-probe/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR="$NX_PE" -DWINE_NX_AMD64=ON -DWINE_NX_FEX="$NX_FEX" \
        -DWINE_NX_BOX64_INTERPRETER=ON -DWINE_NX_BOX64_DYNAREC="$NX_DYNAREC" \
        -DWINE_NX_MESA_SWITCH_DIR="$NX_MESA" -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build "$NX_BUILD" --target wine-nx-runtime-nro -j "$NX_JOBS"
    '
set -- --pe "$pe" --build "$build" --jobs "$jobs"
if [ "$fex" = ON ]; then
    set -- "$@" --fex "${WINE_NX_FEX_BUILD_DIR:-$root/wine-nx-probe/toolchains/build-fex-2609-horizon}/payload"
fi
if [ -n "${WINE_NX_MESA_SWITCH_DIR:-}" ]; then set -- "$@" --vulkan; fi
if [ "${WINE_NX_DXVK:-0}" = 1 ] || [ "${WINE_NX_VKD3D:-0}" = 1 ]; then
    python3 "$root/wine-nx-probe/tools/build-dxvk.py" --jobs "$jobs"
    set -- "$@" --dxvk "$root/wine-nx-probe/build-dxvk-amd64/payload"
fi
if [ "${WINE_NX_VKD3D:-0}" = 1 ]; then
    python3 "$root/wine-nx-probe/tools/build-vkd3d.py" --jobs "$jobs"
    set -- "$@" --vkd3d "$root/wine-nx-probe/build-vkd3d-amd64/payload"
fi
python3 "$root/wine-nx-probe/tools/package-amd64.py" "$@"
