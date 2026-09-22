#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/autorun-low-window.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -fPIE -pie -pthread \
    -I"$root/wine-nx-probe/tests/low_window" "$root/wine-nx-probe/tests/low_window.c" -o "$build/low-window"
"$build/low-window"
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$root/wine-nx-probe/tests/horizon_virtmem" "$root/wine-nx-probe/tests/horizon_virtmem.c" -o "$build/stacks"
"$build/stacks"
python3 "$root/wine-nx-probe/tests/check_low_window_routing.py"
python3 "$root/wine-nx-probe/tests/check_horizon_native_view.py"
