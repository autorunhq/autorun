#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build="${WINE_NX_TEST_BUILD_DIR:-$root/wine-nx-probe/toolchains/build-box64-amd64-tests}"
test "$(uname -s)" = Linux && test "$(uname -m)" = aarch64
sh "$root/wine-nx-probe/tools/bootstrap-box64-core.sh"
cmake -S "$root/wine-nx-probe/tests/box64-core" -B "$build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build" -j "${WINE_NX_JOBS:-4}"
ctest --test-dir "$build" --output-on-failure
sh "$root/wine-nx-probe/tests/arm64ec-transitions/run.sh"
sh "$root/wine-nx-probe/tests/arm64ec-dispatchers/run.sh"
sh "$root/wine-nx-probe/tests/arm64ec-exception-frame/run.sh"
sh "$root/wine-nx-probe/tests/arm64ec-registry/run.sh"
sh "$root/wine-nx-probe/tests/arm64ec-startup/run.sh"
sh "$root/wine-nx-probe/tests/arm64ec-thread-init/run.sh"
python3 "$root/wine-nx-probe/tests/check_arm64ec_fpcsr.py"
python3 "$root/wine-nx-probe/tests/check_arm64ec_box64_unix.py"
python3 "$root/wine-nx-probe/tests/check_horizon_image_info.py"
python3 "$root/wine-nx-probe/tests/check_horizon_guest_reserve.py"
python3 "$root/wine-nx-probe/tests/check_horizon_address_space_limit.py"
python3 "$root/wine-nx-probe/tests/check_wow64_unix_tables.py"
python3 "$root/wine-nx-probe/tests/check_package_amd64.py"
python3 "$root/wine-nx-probe/tests/check_horizon_thread_fds.py"
python3 "$root/wine-nx-probe/tests/check_shared_cpu_context.py"
if [ -d "$root/wine-nx-probe/vendor/libusbhsfs/.git" ]; then
    python3 "$root/wine-nx-probe/tests/check_usb_storage.py"
fi
