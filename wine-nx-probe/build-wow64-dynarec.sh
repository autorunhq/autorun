#!/bin/sh
# Opt-in dynarec runtime, using the same verified PE payload as the interpreter.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sh "$root/wine-nx-probe/build-wow64-components.sh"
# USB drives as D: to H:, through libusbhsfs.
sh "$root/wine-nx-probe/tools/bootstrap-libusbhsfs.sh" >/dev/null
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work \
    devkitpro/devkita64 sh -ec '
    cmake -S wine-nx-probe -B wine-nx-probe/build-switch-wow64-dynarec -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/wine-nx-probe/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/wine-nx-probe/build-wine-wow64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build wine-nx-probe/build-switch-wow64-dynarec --target wine-nx-runtime-nro -j 8
    '
python3 "$root/wine-nx-probe/tools/package-wow64-dynarec.py"
