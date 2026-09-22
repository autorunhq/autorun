#!/bin/sh
set -eu
probe="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="${WINE_NX_FEX_DIR:-$probe/toolchains/fex-2609}"
host_cc="${WINE_NX_HOST_CC:-/usr/bin/clang}"
host_cxx="${WINE_NX_HOST_CXX:-/usr/bin/clang++}"
test "$(uname -s)" = Linux && test "$(uname -m)" = aarch64
build="$(mktemp -d /tmp/wine-nx-fex-tests.XXXXXX)"
trap 'rm -rf "$build"' EXIT HUP INT TERM
"$host_cc" -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    "$probe/source/fex_jit.c" "$probe/tests/fex_jit.c" -o "$build/jit"
"$build/jit"
"$host_cc" -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -D__SWITCH__ -I"$probe/tests/fex_horizon" "$probe/tests/fex_horizon_jit.c" -o "$build/horizon-jit"
"$build/horizon-jit"
python3 "$probe/tests/check_fex_jit_bridge.py"
"$host_cxx" -std=c++20 -O3 -Wall -Wextra -Werror \
    "$probe/tests/fex_jit_registry.cpp" -o "$build/jit-registry"
"$build/jit-registry"
"$host_cxx" -std=c++20 -O2 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    "$probe/tests/fex_code_storage.cpp" -o "$build/code-storage"
"$build/code-storage"
python3 "$probe/tests/check_horizon_native_view.py"
python3 "$probe/tests/check_horizon_sections.py"
python3 "$probe/tests/check_horizon_guest_reserve.py"
"$host_cc" -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -c "$probe/source/fex_jit.c" -o "$build/jit.o"
"$host_cxx" -std=c++20 -g -O2 -pthread -fsanitize=address,undefined -fno-sanitize=function \
    -DFEX_HORIZON -DFMT_HEADER_ONLY -I "$source_dir/CodeEmitter" \
    -I "$source_dir/FEXCore/include" -I "$source_dir/FEXHeaderUtils" -I "$source_dir/External/fmt/include" \
    "$probe/tests/fex_emitter.cpp" "$build/jit.o" -o "$build/emitter"
"$build/emitter"
python3 "$probe/tests/check_fex_unaligned.py"
python3 "$probe/tests/check_fex_context.py"
python3 "$probe/tests/check_fex_exception_init.py"
python3 "$probe/tests/check_fex_memory.py"
python3 "$probe/tests/check_fex_heap.py"
python3 "$probe/tests/check_wow64_thread_context.py"
python3 "$probe/tests/check_horizon_thread_handles.py"
python3 "$probe/tests/check_native_stack_write.py"
python3 "$probe/tests/check_loader_indexes.py"
"$host_cc" -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$probe/tests/launcher_settings.c" -o "$build/settings"
"$build/settings"
"$host_cc" -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$probe/tests/fex_options.c" -o "$build/fex-options"
"$build/fex-options"
if [ "$#" -eq 4 ]; then
    python3 "$probe/tests/check_fex_contract.py" "$1" "$2" "$3" "$4"
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [arm64ecfex.dll wow64fex.dll ntdll.dll wow64.dll]" >&2
    exit 1
fi
