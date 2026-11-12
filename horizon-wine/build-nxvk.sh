#!/bin/sh
# nxvk (https://github.com/PalindromicBreadLoaf/nxvk): NVK for Vulkan and Zink
# over it for OpenGL, as static archives for a second runtime,
# wine-nx-runtime-nxvk.nro (CMakeLists.txt, WINE_NX_NXVK_DIR).
#
#   sh horizon-wine/build-nxvk.sh
#
# Clones the pinned commit into vendor/nxvk, builds nxvk's own toolchain image
# (devkitA64, LLVM 15 and a Rust nightly: NAK is Rust) and runs nxvk's
# `make gl` in it, which stages lib/libnvk.a, libnvk_support.a and
# libnvk_gl.a in vendor/nxvk/switch/build/pkg. A package already there for the
# pinned commit is kept: the first build takes the better part of an hour.
#
# nxvk's own files are GPL-2.0-or-later, and it is linked statically: a
# runtime built with it is a combined work under the GPL (its
# switch/docs/LICENSING.md).
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
hw="$root/horizon-wine"
commit=c3abea1efde44027b7e3e20b48eb55beeecd5ab4
src="$hw/vendor/nxvk"
pkg="$src/switch/build/pkg"
image="autorun-nxvk:$commit"

if [ -f "$pkg/lib/libnvk_gl.a" ] && [ "$(cat "$pkg/commit" 2>/dev/null)" = "$commit" ]; then
    echo "nxvk $commit: package already built in $pkg"
    exit 0
fi
command -v docker >/dev/null || { echo "Missing docker." >&2; exit 1; }

if [ ! -d "$src/.git" ]; then
    mkdir -p "$hw/vendor"
    git init -q "$src"
    git -C "$src" remote add origin https://github.com/PalindromicBreadLoaf/nxvk
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
    git -C "$src" fetch -q --depth 1 origin "$commit"
    git -C "$src" checkout -q --force "$commit"
fi

docker build -q -t "$image" "$src/switch/docker" >/dev/null
# CONTAINER= runs nxvk's recipes in this container rather than starting one
# per step, so its archiver, devkitPro's, is the one that bundles them too.
docker run --rm -v "$src:/work" -w /work "$image" \
    sh -c 'make gl CONTAINER= -j"$(nproc)" && chown -R "$1" switch/build' sh "$(id -u):$(id -g)"
for lib in nvk nvk_support nvk_gl; do
    [ -f "$pkg/lib/lib$lib.a" ] || { echo "nxvk did not stage lib$lib.a" >&2; exit 1; }
done
echo "$commit" > "$pkg/commit"
# The licenses go beside the runtime on the card (tools/package-autorun.py).
rm -rf "$pkg/licenses"
cp -R "$src/licenses" "$pkg/licenses"
cp "$src/switch/docs/LICENSING.md" "$pkg/licenses/nxvk-LICENSING.md"
ls -l "$pkg/lib"
# On a CI runner the image's gigabytes are wanted back for the runtime's build.
if [ "${CI:-}" = true ]; then
    docker image rm -f "$image" >/dev/null || true
    rm -rf "$src/switch/build/cross" "$src/switch/build/cross-zink" "$src/switch/build/native-tools"
fi
