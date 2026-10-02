#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="${WINE_NX_PE_BUILD_DIR:-$root/horizon-wine/build-wine-amd64-pe}"
build="${WINE_NX_BUILD_DIR:-$root/horizon-wine/build-switch-amd64}"
boot_bundle="${WINE_NX_BOOT_BUNDLE_DIR:-$root/horizon-wine/toolchains/boot-payloads/bundle}"
jobs="${WINE_NX_JOBS:-8}"
fex=OFF
case "${WINE_NX_FEX:-0}" in
    0) ;;
    1) fex=ON ;;
    *) echo "WINE_NX_FEX must be 0 or 1." >&2; exit 1;;
esac
if [ -n "${WINE_NX_LLVM_MINGW:-}" ]; then
    export PATH="$WINE_NX_LLVM_MINGW/bin:$PATH"
fi
# Wine's configure wants a newer bison than macOS has.
[ -d /opt/homebrew/opt/bison/bin ] && export PATH="/opt/homebrew/opt/bison/bin:$PATH"
for tool in arm64ec-w64-mingw32-clang aarch64-w64-mingw32-clang i686-w64-mingw32-clang x86_64-w64-mingw32-clang x86_64-w64-mingw32-windres llvm-readobj; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; set WINE_NX_LLVM_MINGW." >&2; exit 1; }
done
mkdir -p "$pe" "$build"
pe="$(CDPATH= cd -- "$pe" && pwd)"
build="$(CDPATH= cd -- "$build" && pwd)"
case "$pe" in "$root"/*) ;; *) echo "PE build must be inside the Wine checkout." >&2; exit 1;; esac
case "$build" in "$root"/*) ;; *) echo "Switch build must be inside the Wine checkout." >&2; exit 1;; esac
if [ -f "$boot_bundle/setup_boot_manifest.h" ]; then
    boot_bundle="$(CDPATH= cd -- "$boot_bundle" && pwd)"
    case "$boot_bundle" in "$root"/*) ;; *) echo "Boot bundle must be inside the Wine checkout." >&2; exit 1;; esac
    boot_bundle="/work/${boot_bundle#"$root"/}"
elif [ -n "${WINE_NX_BOOT_BUNDLE_DIR:-}" ]; then
    echo "Boot bundle is incomplete: $boot_bundle" >&2
    exit 1
else
    boot_bundle=""
fi
(
    cd "$pe"
    "$root/configure" --enable-archs=aarch64,arm64ec,i386 \
        --enable-winebox64=aarch64 --enable-winebox64ec=arm64ec \
        --disable-tests --without-x --without-freetype --without-alsa --without-pulse \
        --without-dbus --without-fontconfig --without-udev --without-usb \
        --without-gstreamer --enable-winegstreamer --without-vulkan
    make -j"$jobs" include/all
)
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
sh "$root/horizon-wine/tools/bootstrap-wma.sh"
# The switch-dev image (switch-dev.txt) has libnx, mesa-switch, LSFG-VK and
# libusbhsfs in portlibs. WINE_NX_MESA_SWITCH_DIR links another mesa-switch.
docker run --rm --network none -v "$root:/work" -w /work \
    -e NX_PE="/work/${pe#"$root/"}" -e NX_BUILD="/work/${build#"$root/"}" \
    -e NX_JOBS="$jobs" -e NX_DYNAREC="${WINE_NX_BOX64_DYNAREC:-ON}" \
    -e WINE_NX_JOBS="$jobs" \
    -e NX_MESA="${WINE_NX_MESA_SWITCH_DIR:-}" -e NX_FEX="$fex" -e NX_BOOT_BUNDLE="$boot_bundle" \
    "${WINE_NX_DEVKIT_IMAGE:-$(cat "$root/horizon-wine/switch-dev.txt")}" sh -ec '
    cmake -S horizon-wine -B "$NX_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR="$NX_PE" -DWINE_NX_AMD64=ON -DWINE_NX_FEX="$NX_FEX" \
        -DWINE_NX_BOX64_INTERPRETER=ON -DWINE_NX_BOX64_DYNAREC="$NX_DYNAREC" \
        ${NX_MESA:+-DWINE_NX_MESA_SWITCH_DIR="$NX_MESA"} -DWINE_NX_USB_STORAGE=ON \
        -DWINE_NX_BOOT_BUNDLE="$NX_BOOT_BUNDLE" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$NX_BUILD" --target wine-nx-runtime-nro -j "$NX_JOBS"
    '
# The Windows modules this runtime runs -- system32, syswow64, FEX's CPU modules,
# DXVK and VKD3D-Proton -- are the DLL repository's: horizon-dlls/tools/build-dlls.py
# builds them in $pe. These say what the NRO was built to run.
set -- --build "$build"
if [ "$fex" = ON ]; then set -- "$@" --fex; fi
set -- "$@" --vulkan
if [ "${WINE_NX_DXVK:-0}" = 1 ] || [ "${WINE_NX_VKD3D:-0}" = 1 ]; then set -- "$@" --dxvk; fi
if [ "${WINE_NX_VKD3D:-0}" = 1 ]; then set -- "$@" --vkd3d; fi
python3 "$root/horizon-wine/tools/package-amd64.py" "$@"
