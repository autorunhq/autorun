#!/bin/sh
# Build the experimental WoW64 loader test and its matching SD package.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="$root/wine-nx-probe/build-wine-wow64-pe"
export PATH="$root/wine-nx-probe/toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin:/opt/homebrew/opt/bison/bin:$PATH"
command -v aarch64-w64-mingw32-clang >/dev/null
if [ ! -f "$pe/Makefile" ]; then
    echo "Configure build-wine-wow64-pe with --enable-archs=aarch64,i386 --enable-winebox64=aarch64 first." >&2
    exit 1
fi
# i386 guest modules: the load-time closure of the staged programs, including
# 7-Zip's 7zr.exe (regular imports only; delay-loaded DLLs wait for first use),
# plus imm32, which user32 loads during its process attach.
i386_modules="ntdll kernel32 kernelbase msvcrt ucrtbase advapi32 sechost user32 win32u gdi32
    oleaut32 ole32 combase coml2 rpcrt4 imm32"
i386_targets=""
for module in $i386_modules; do i386_targets="$i386_targets dlls/$module/i386-windows/$module.dll"; done
make -C "$pe" -j8 include/all \
    dlls/winebox64/aarch64-windows/winebox64.dll \
    dlls/wow64/aarch64-windows/wow64.dll \
    dlls/wow64win/aarch64-windows/wow64win.dll \
    dlls/ntdll/aarch64-windows/ntdll.dll \
    dlls/win32u/aarch64-windows/win32u.dll \
    dlls/apisetschema/aarch64-windows/apisetschema.dll \
    $i386_targets
i686-w64-mingw32-clang -Os -nostdlib -Wl,--entry,_start@0 \
    -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-smoke.exe" "$root/wine-nx-probe/tests/pe32_smoke.c" -lntdll
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-functional.exe" "$root/wine-nx-probe/tests/pe32_functional.c" -lkernel32 -lntdll
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-threads.exe" "$root/wine-nx-probe/tests/pe32_threads.c" -lkernel32 -lntdll
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-lifecycle.exe" "$root/wine-nx-probe/tests/pe32_lifecycle.c" -lkernel32 -lntdll
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-timers.exe" "$root/wine-nx-probe/tests/pe32_timers.c" -luser32 -lkernel32 -lntdll
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-messages.exe" "$root/wine-nx-probe/tests/pe32_messages.c" -luser32 -lkernel32 -lntdll
i686-w64-mingw32-windres -I "$root" \
    "$root/wine-nx-probe/tests/pe32_video_startup.rc" "$pe/pe32-video-startup.res.o"
i686-w64-mingw32-clang -Os -Wall -Wextra -Werror -fno-builtin -nostdlib \
    -Wl,--entry,_start@0 -Wl,--image-base,0x10000000 -Wl,--dynamicbase \
    -o "$pe/pe32-video-startup.exe" "$root/wine-nx-probe/tests/pe32_video_startup.c" \
    "$pe/pe32-video-startup.res.o" -luser32 -lkernel32 -lntdll
sh "$root/wine-nx-probe/tools/bootstrap-box64-core.sh"
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work \
    devkitpro/devkita64 sh -ec '
    cmake -S wine-nx-probe -B wine-nx-probe/build-switch-wow64 -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/wine-nx-probe/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/wine-nx-probe/build-wine-wow64-pe \
        -DWINE_NX_BOX64_INTERPRETER=ON -DWINE_NX_BOX64_DYNAREC=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build wine-nx-probe/build-switch-wow64 --target wine-nx-runtime-nro -j 8
    '
stage="$root/wine-nx-probe/build-switch-wow64/sd-card/switch/wine"
mkdir -p "$stage/drive_c/windows/system32" "$stage/drive_c/windows/syswow64" "$stage/share/wine/nls"
for module in winebox64 wow64 wow64win win32u ntdll; do
    cp "$pe/dlls/$module/aarch64-windows/$module.dll" "$stage/drive_c/windows/system32/"
done
# The API set schema the runtime maps at startup (load_apiset_dll); the package
# check requires it, and the packagers built on this stage start from here.
cp "$pe/dlls/apisetschema/aarch64-windows/apisetschema.dll" "$stage/drive_c/windows/system32/"
for module in $i386_modules; do
    cp "$pe/dlls/$module/i386-windows/$module.dll" "$stage/drive_c/windows/syswow64/"
done
cp "$pe/pe32-smoke.exe" "$pe/pe32-functional.exe" "$pe/pe32-threads.exe" "$pe/pe32-lifecycle.exe" "$pe/pe32-timers.exe" "$pe/pe32-messages.exe" "$pe/pe32-video-startup.exe" \
    "$root/wine-nx-probe/samples/7zr-x86/7zr.exe" "$root/wine-nx-probe/samples/7zr-x86/7zr-sample.7z" \
    "$root/wine-nx-probe/samples/7zr-x86/7zr-tree.7z" \
    "$stage/drive_c/"
python3 "$root/wine-nx-probe/tools/make-7zr-tree.py" "$stage/drive_c"
# "7zr rn" rewrites this copy in place through a temporary file and a rename.
cp "$root/wine-nx-probe/samples/7zr-x86/7zr-tree.7z" "$stage/drive_c/7zr-rename.7z"
rm -f "$stage/drive_c/wine-nx-tree.7z"
cp "$root/nls/"*.nls "$stage/share/wine/nls/"
cp "$root/wine-nx-probe/build-switch-wow64/wine-nx-runtime.nro" "$stage/"
printf '%s\n' 'sdmc:/switch/wine/drive_c/7zr.exe' > "$stage/target.txt"
printf '%s\n' 'C:\7zr.exe b 1 -mmt2 -md18' > "$stage/args.txt"
printf '%s\n' '1' > "$stage/run-entry.txt"
printf '%s\n' 'Real x86 console application: 7-Zip 26.03 7zr.exe (build nx-wow64-console-11).' \
    'args.txt runs the 7-Zip benchmark with two threads: one LZMA compression and decompression pass' \
    'with a 256 KiB dictionary. It exercises real worker threads, semaphores, events and timing under' \
    'the interpreter and can take a few minutes; [BOX64] lines report instructions per second meanwhile.' \
    'Expected: [STDOUT] lines ending with the Avr: and Tot: rows, a [LIFECYCLE] verdict line for the' \
    'benchmark threads, and exit_code=0x00000000. The speed figures depend on the device.' \
    'In-place update (verified in console-9): "C:\7zr.exe rn C:\7zr-rename.7z 7zr-tree\readme.txt' \
    '7zr-tree\README-renamed.txt" expects Archive size: 90160 bytes and Everything is Ok.' \
    'Extraction (verified in console-8): "C:\7zr.exe x C:\7zr-tree.7z -oC:\7zr-out -y" expects' \
    'Everything is Ok, Folders: 3, Files: 3, Size: 325691. Error path (verified in console-7):' \
    '"C:\7zr.exe x C:\no-such-archive.7z -oC:\7zr-out -y" expects System ERROR: and exit code 2.' \
    'Other commands: "C:\7zr.exe a C:\wine-nx-tree.7z C:\7zr-tree -mx1" archives the tree;' \
    '"C:\7zr.exe t C:\7zr-sample.7z" tests the known archive.' \
    'Starting Wine-NX shows a menu of the programs in drive_c: choose with Up/Down and start with A;' \
    'args.txt applies only to the program its first word names. Tests in the menu: pe32-lifecycle.exe' \
    '(threads), pe32-timers.exe (window timers) and pe32-messages.exe (messages and clipboard) each' \
    'expect [PE32 TEST] PASS ALL and exit_code=0x0000002a.' \
    'Log: sdmc:/switch/wine/logs/autorun_runtime.log; close from HOME after it parks.' > "$stage/README.txt"
python3 "$root/wine-nx-probe/tools/verify-wow64-package.py"
echo "Staged WoW64 loader test in $stage"
