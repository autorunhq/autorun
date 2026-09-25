#!/bin/sh
set -eu
export UBSAN_OPTIONS=halt_on_error=1
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
python3 "$root/wine-nx-probe/tests/check_swap_ui.py"
python3 "$root/wine-nx-probe/tests/check_swap_game.py"
python3 "$root/wine-nx-probe/tests/check_swap_threads.py"
python3 "$root/wine-nx-probe/tests/check_swap_native.py"
python3 "$root/wine-nx-probe/tests/check_commit_chunks.py"
build="$(mktemp -d "${TMPDIR:-/tmp}/autorun-swap.XXXXXX")"
trap 'rm -f "$build/pager" "$build/store" "$build/file" "$build/ipc" "$build/pool" "$build/index"; rmdir "$build"' EXIT HUP INT TERM
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$root/wine-nx-probe/tests/swap_index.c" -o "$build/index"
"$build/index"
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$root/wine-nx-probe/tests/horizon_pool.c" -o "$build/pool"
"$build/pool"
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    "$root/wine-nx-probe/tests/swap_pager.c" "$root/wine-nx-probe/source/swap_pager.c" -o "$build/pager"
"$build/pager"
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$root/wine-nx-probe/tests/swap_store.c" "$root/wine-nx-probe/source/swap_store.c" \
    -Wl,--wrap=rename,--wrap=fsync -o "$build/store"
"$build/store"
"${CC:-clang}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$root/wine-nx-probe/tests/swap_file.c" "$root/wine-nx-probe/source/swap_file.c" \
    "$root/wine-nx-probe/source/swap_store.c" -o "$build/file"
"$build/file"
if [ -n "${LIBNX_INCLUDE:-}" ]; then
    "${CC:-clang}" -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$LIBNX_INCLUDE" "$root/wine-nx-probe/tests/swap_ipc.c" -o "$build/ipc"
    "$build/ipc"
fi
