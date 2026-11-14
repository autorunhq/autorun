#!/bin/sh
# Host tests for Horizon thread lifecycle and synchronization state.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/wine-nx-threads.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc="${CC:-clang}"
"$cc" -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$root/horizon-wine/tests/horizon_threads.c" -o "$build/asan"
"$build/asan"
"$cc" -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=thread \
    "$root/horizon-wine/tests/horizon_threads.c" -o "$build/tsan"
"$build/tsan"
python3 "$root/horizon-wine/tests/check_select_wait.py"
python3 "$root/horizon-wine/tests/check_horizon_fast_sync.py"
python3 "$root/horizon-wine/tests/check_server_pool.py"
python3 "$root/horizon-wine/tests/check_horizon_suspend.py"
