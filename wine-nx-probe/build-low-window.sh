#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
src="$root/wine-nx-probe/vendor/Atmosphere"
patch="$root/wine-nx-probe/mesosphere/low-window.patch"
revision=5388824be146a89619e8d641acd64599cf1c5f62
out="$root/wine-nx-probe/toolchains/low-window-2"

if [ ! -d "$src/.git" ]; then
    git clone --depth 1 --branch 1.11.2 https://github.com/Atmosphere-NX/Atmosphere.git "$src"
fi
[ "$(git -C "$src" rev-parse HEAD)" = "$revision" ] || { echo 'Atmosphere revision mismatch' >&2; exit 1; }
if git -C "$src" diff --quiet && git -C "$src" diff --cached --quiet; then
    git -C "$src" apply --check "$patch"
    git -C "$src" apply "$patch"
fi
git -C "$src" diff --cached --quiet || { echo 'Atmosphere has staged changes' >&2; exit 1; }
actual="$(git -C "$src" -c core.autocrlf=true diff --no-ext-diff --binary | git hash-object --stdin)"
[ "$actual" = "$(git hash-object --no-filters "$patch")" ] || {
    echo 'Atmosphere has changes other than the pinned low-window patch' >&2; exit 1;
}
make -C "$src/mesosphere" -j"${WINE_NX_JOBS:-6}"
make -C "$src/stratosphere/loader" -j"${WINE_NX_JOBS:-6}"
python3 "$root/wine-nx-probe/tools/package-low-window.py" "$src" "$out" "${1:?Pass the rebuilt wine-nx-runtime.nro}"
