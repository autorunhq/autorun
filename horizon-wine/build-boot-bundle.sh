#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
horizon_wine="$root/horizon-wine"
work="$horizon_wine/toolchains/boot-payloads"
output="${1:-$work/bundle}"
ams_rev=4e3f37038d9e82dde7054897213ab32b2ecf651f
libnx_rev=20cbcfd90567f7bf84df7725f27e345e2164fa6b
hoc_rev=7960dd9be629c9ea8f99b26633cf3a3cfedbb34d
ams="$work/atmosphere-1.12.2"
hoc="$work/hoc-2.6.0"
hoc_ams="$work/atmosphere-hoc-1.12.2-2.6.0"
release="$work/atmosphere-1.12.2.zip"
libnx="$work/libnx-20cbcfd"
hoc_dist="$work/hoc-2.6.0.zip"
build=out/nintendo_nx_arm64_armv8a/release

for tool in git make hactool python3 curl; do
    command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }
done

mkdir -p "$work" "$output"
if [ ! -d "$libnx/.git" ]; then
    git init "$libnx"
    git -C "$libnx" fetch --depth 1 https://github.com/switchbrew/libnx.git "$libnx_rev"
    git -C "$libnx" checkout --detach FETCH_HEAD
fi
[ "$(git -C "$libnx" rev-parse HEAD)" = "$libnx_rev" ] || { echo 'libnx revision mismatch' >&2; exit 1; }
if [ ! -d "$ams/.git" ]; then
    git clone --depth 1 --branch 1.12.2 https://github.com/Atmosphere-NX/Atmosphere.git "$ams"
fi
if [ ! -d "$hoc/.git" ]; then
    git clone --depth 1 --branch 2.6.0-rel https://github.com/Horizon-OC/Horizon-OC.git "$hoc"
fi
[ "$(git -C "$ams" rev-parse HEAD)" = "$ams_rev" ] || { echo 'Atmosphere revision mismatch' >&2; exit 1; }
[ "$(git -C "$hoc" rev-parse HEAD)" = "$hoc_rev" ] || { echo 'HOC release revision mismatch' >&2; exit 1; }

if git -C "$ams" diff --quiet && git -C "$ams" diff --cached --quiet; then
    git -C "$ams" apply --check "$horizon_wine/mesosphere/low-window.patch"
    git -C "$ams" apply "$horizon_wine/mesosphere/low-window.patch"
else
    git -C "$ams" apply --reverse --check "$horizon_wine/mesosphere/low-window.patch"
fi

if [ ! -d "$hoc_ams/.git" ]; then
    git clone --local "$ams" "$hoc_ams"
    [ "$(git -C "$hoc_ams" rev-parse HEAD)" = "$ams_rev" ] || exit 1
    cp -R "$hoc/Source/Atmosphere/stratosphere/loader/." "$hoc_ams/stratosphere/loader/"
    git -C "$hoc_ams" apply --check "$horizon_wine/mesosphere/low-window.patch"
    git -C "$hoc_ams" apply "$horizon_wine/mesosphere/low-window.patch"
fi
[ "$(git -C "$hoc_ams" rev-parse HEAD)" = "$ams_rev" ] || { echo 'HOC Atmosphere revision mismatch' >&2; exit 1; }
git -C "$hoc_ams" apply --reverse --check "$horizon_wine/mesosphere/low-window.patch"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export TMPDIR="$work"
export TMP="$work"
export TEMP="$work"
make -s -C "$libnx/nx" lib/libnx.a -j"${WINE_NX_JOBS:-6}"
export LIBNX="$work/sdk$DEVKITPRO/libnx"
mkdir -p "$LIBNX"
cp -pR "$libnx/nx/include" "$libnx/nx/lib" "$LIBNX/"
cp -pR "$libnx/nx/external/bsd/include/." "$LIBNX/include/"
cp -p "$libnx/nx/switch_rules" "$libnx/nx/switch.ld" "$libnx/nx/switch.specs" \
    "$libnx/nx/default_icon.jpg" "$LIBNX/"
make -s -C "$ams/mesosphere" -j"${WINE_NX_JOBS:-6}"
make -s -C "$ams/stratosphere/loader" -j"${WINE_NX_JOBS:-6}"
make -s -C "$hoc_ams/stratosphere/loader" -j"${WINE_NX_JOBS:-6}" ATMOSPHERE_LIBRARIES_DIR="$ams/libraries"

hactool -t kip1 "--uncompressed=$work/loader-hoc.kip" \
    "$hoc_ams/stratosphere/loader/$build/loader.kip" >/dev/null
if [ ! -f "$release" ]; then
    curl -fL --retry 3 -o "$release" \
        'https://github.com/Atmosphere-NX/Atmosphere/releases/download/1.12.2/atmosphere-1.12.2-master-4e3f37038%2Bhbl-2.4.5%2Bhbmenu-3.6.1.zip'
fi
if [ ! -f "$hoc_dist" ]; then
    curl -fL --retry 3 -o "$hoc_dist" \
        'https://github.com/Horizon-OC/Horizon-OC/releases/download/2.6.0-rel/dist.zip'
fi
# devkitPro's own name for it: .exe under MSYS2, none on Linux and macOS.
nm="$DEVKITA64/bin/aarch64-none-elf-nm"
[ -x "$nm.exe" ] && nm="$nm.exe"
python3 "$horizon_wine/tools/make-boot-bundle.py" \
    --stock "$ams/stratosphere/loader/$build/loader.kip" \
    --hoc "$work/loader-hoc.kip" \
    --hoc-elf "$hoc_ams/stratosphere/loader/$build/loader.elf" \
    --nm "$nm" \
    --mesosphere "$ams/mesosphere/$build/mesosphere.bin" \
    --atmosphere-zip "$release" --hoc-zip "$hoc_dist" \
    --atmosphere-license "$ams/LICENSE" \
    --hoc-license "$hoc/LICENSE" --output "$output"
