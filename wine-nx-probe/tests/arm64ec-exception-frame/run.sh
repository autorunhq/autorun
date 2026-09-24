#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
build=${BUILD_DIR:-"$root/wine-nx-probe/toolchains/build-arm64ec-exception-frame"}
cc=${CC:-cc}

mkdir -p "$build"
python3 "$here/extract_functions.py" "${SIGNAL_SOURCE:-$root/dlls/ntdll/unix/signal_arm64.c}" \
    "$build/extracted_functions.inc"
"$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$build" \
    "$here/exception_frame_test.c" -o "$build/arm64ec-exception-frame"
exec "$build/arm64ec-exception-frame"
