#!/bin/sh
# The Upscaling setting's FSR 1.0 passes (dlls/win32u/fshack_fsr_spv.h) on
# Mesa's lavapipe, in a Debian container: tests/fshack_fsr.c. Also checks that
# the header is what tools/fshack_*.comp compile to, when glslangValidator is
# on this machine. PNGs of the results go to $FSHACK_SHOTS when it is set.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
if command -v glslangValidator >/dev/null; then
    python3 "$root/tools/make_fshack_shaders.py" --check
fi
docker image inspect autorun-lavapipe >/dev/null 2>&1 || printf '%s\n' \
    'FROM debian:bookworm' \
    'RUN apt-get update -qq && apt-get install -y -qq mesa-vulkan-drivers libvulkan-dev gcc libpng-dev && rm -rf /var/lib/apt/lists/*' |
    docker build -q -t autorun-lavapipe - >/dev/null
shots="${FSHACK_SHOTS:-}"
docker run --rm -v "$root:/src:ro" ${shots:+-v "$shots:/shots"} autorun-lavapipe sh -ec "
    gcc -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -Werror -O1 -g -fsanitize=address,undefined /src/wine-nx-probe/tests/fshack_fsr.c \
        -o /tmp/fshack_fsr -lvulkan -lpng -lm
    ASAN_OPTIONS=detect_leaks=0 VK_ICD_FILENAMES=\$(ls /usr/share/vulkan/icd.d/lvp_icd.*.json) /tmp/fshack_fsr ${shots:+/shots}"
