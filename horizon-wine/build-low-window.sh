#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
src="$root/horizon-wine/toolchains/boot-payloads/atmosphere-1.12.2"
out="$root/horizon-wine/toolchains/low-window-2"
runtime="${1:?Pass the rebuilt wine-nx-runtime.nro}"

sh "$root/horizon-wine/build-boot-bundle.sh"
python3 "$root/horizon-wine/tools/package-low-window.py" "$src" "$out" "$runtime"
