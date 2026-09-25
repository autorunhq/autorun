#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
probe="$root/wine-nx-probe"
work="$probe/toolchains/boot-payloads"
output="${1:-$work/bundle}"
ams_rev=5388824be146a89619e8d641acd64599cf1c5f62
hoc_release_rev=a6f1732e1d96577a8009d47c9b98e018080f3fe7
hoc_loader_rev=d9906a794c6015a892c60744d520faf793d22549
ams="$work/atmosphere"
hoc="$work/hoc"
hoc_ams="$work/atmosphere-hoc"
release="$work/atmosphere-1.11.2.zip"
hoc_dist="$work/hoc-2.5.1.zip"
build=out/nintendo_nx_arm64_armv8a/release

for tool in git make hactool python3 curl; do
    command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }
done

mkdir -p "$work" "$output"
if [ ! -d "$ams/.git" ]; then
    git clone --depth 1 --branch 1.11.2 https://github.com/Atmosphere-NX/Atmosphere.git "$ams"
fi
if [ ! -d "$hoc/.git" ]; then
    git clone --depth 40 --branch 2.5.1 https://github.com/Horizon-OC/Horizon-OC.git "$hoc"
fi
[ "$(git -C "$ams" rev-parse HEAD)" = "$ams_rev" ] || { echo 'Atmosphere revision mismatch' >&2; exit 1; }
[ "$(git -C "$hoc" rev-parse 2.5.1)" = "$hoc_release_rev" ] || { echo 'HOC release tag mismatch' >&2; exit 1; }
if [ "$(git -C "$hoc" rev-parse HEAD)" = "$hoc_release_rev" ]; then
    git -C "$hoc" cat-file -e "$hoc_loader_rev^{commit}" || git -C "$hoc" fetch --depth 1 origin "$hoc_loader_rev"
    git -C "$hoc" checkout --detach "$hoc_loader_rev"
fi
[ "$(git -C "$hoc" rev-parse HEAD)" = "$hoc_loader_rev" ] || { echo 'HOC loader source mismatch' >&2; exit 1; }
if git -C "$hoc" diff --quiet && git -C "$hoc" diff --cached --quiet; then
    git -C "$hoc" apply --check "$probe/mesosphere/hoc-2.5.1-version.patch"
    git -C "$hoc" apply "$probe/mesosphere/hoc-2.5.1-version.patch"
else
    git -C "$hoc" apply --reverse --check "$probe/mesosphere/hoc-2.5.1-version.patch"
fi

if git -C "$ams" diff --quiet && git -C "$ams" diff --cached --quiet; then
    git -C "$ams" apply --check "$probe/mesosphere/low-window.patch"
    git -C "$ams" apply "$probe/mesosphere/low-window.patch"
else
    git -C "$ams" apply --reverse --check "$probe/mesosphere/low-window.patch"
fi

if [ ! -d "$hoc_ams/.git" ]; then
    git clone --local "$ams" "$hoc_ams"
    [ "$(git -C "$hoc_ams" rev-parse HEAD)" = "$ams_rev" ] || exit 1
    git -C "$hoc_ams" apply --check "$probe/mesosphere/low-window.patch"
    git -C "$hoc_ams" apply "$probe/mesosphere/low-window.patch"
    cp -R "$hoc/Source/Atmosphere/stratosphere/loader/." "$hoc_ams/stratosphere/loader/"
    git -C "$hoc_ams" apply --check "$probe/mesosphere/hoc-low-window.patch"
    git -C "$hoc_ams" apply "$probe/mesosphere/hoc-low-window.patch"
fi

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export TMPDIR="$work"
export TMP="$work"
export TEMP="$work"
make -s -C "$ams/mesosphere" -j"${WINE_NX_JOBS:-6}"
make -s -C "$ams/stratosphere/loader" -j"${WINE_NX_JOBS:-6}"
make -s -C "$hoc_ams/stratosphere/loader" -j"${WINE_NX_JOBS:-6}"

hactool -t kip1 "--uncompressed=$work/loader-hoc.kip" \
    "$hoc_ams/stratosphere/loader/$build/loader.kip" >/dev/null
if [ ! -f "$release" ]; then
    curl -fL --retry 3 -o "$release" \
        'https://github.com/Atmosphere-NX/Atmosphere/releases/download/1.11.2/atmosphere-1.11.2-master-5388824be%2Bhbl-2.4.5%2Bhbmenu-3.6.1.zip'
fi
if [ ! -f "$hoc_dist" ]; then
    curl -fL --retry 3 -o "$hoc_dist" \
        'https://github.com/Horizon-OC/Horizon-OC/releases/download/2.5.1/dist.zip'
fi
python3 "$probe/tools/make-boot-bundle.py" \
    --stock "$ams/stratosphere/loader/$build/loader.kip" \
    --hoc "$work/loader-hoc.kip" \
    --hoc-elf "$hoc_ams/stratosphere/loader/$build/loader.elf" \
    --nm "$DEVKITA64/bin/aarch64-none-elf-nm.exe" \
    --mesosphere "$ams/mesosphere/$build/mesosphere.bin" \
    --atmosphere-zip "$release" --hoc-zip "$hoc_dist" \
    --atmosphere-license "$ams/LICENSE" \
    --hoc-license "$hoc/LICENSE" --output "$output"
