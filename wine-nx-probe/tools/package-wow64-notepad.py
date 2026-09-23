#!/usr/bin/env python3
"""Build i386 Wine Notepad and stage its import closure with the dynarec NRO."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
from zipfile import ZipFile, ZIP_DEFLATED
probe = Path(__file__).resolve().parents[1]
root = probe.parent
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
baseline = probe / 'build-switch-wow64/sd-card/switch/wine'
stage = build / 'notepad-sd-card/switch/wine'
toolchain = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{toolchain}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
verify = probe / 'tools/verify-wow64-package.py'
subprocess.run([sys.executable, str(verify), str(baseline)], check=True)
subprocess.run(['make', '-C', str(pe), '-j8', 'programs/notepad/i386-windows/notepad.exe'], env=env, check=True)
video_res = pe / 'pe32-video-startup.res.o'
video_exe = pe / 'pe32-video-startup.exe'
subprocess.run([str(toolchain / 'i686-w64-mingw32-windres'), '-I', str(root),
                str(probe / 'tests/pe32_video_startup.rc'), str(video_res)], env=env, check=True)
subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
                '-fno-builtin', '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000',
                '-Wl,--dynamicbase', '-o', str(video_exe), str(probe / 'tests/pe32_video_startup.c'),
                str(video_res), '-luser32', '-lkernel32', '-lntdll'], env=env, check=True)
shutil.copytree(baseline, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns('*.log', '.DS_Store'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
for module in ('ntdll', 'wow64', 'wow64win', 'winebox64', 'win32u'):
    shutil.copy2(pe / f'dlls/{module}/aarch64-windows/{module}.dll',
                 stage / 'drive_c/windows/system32' / f'{module}.dll')
# The API set schema the runtime maps at startup (load_apiset_dll): without it
# no api-ms-win-* import resolves, as in a UCRT-linked DLL.
schema = 'dlls/apisetschema/aarch64-windows/apisetschema.dll'
subprocess.run(['make', '-C', str(pe), '-j8', schema], env=env, check=True)
shutil.copy2(pe / schema, stage / 'drive_c/windows/system32/apisetschema.dll')
exe = pe / 'programs/notepad/i386-windows/notepad.exe'
shutil.copy2(exe, stage / 'drive_c/notepad.exe')
shutil.copy2(video_exe, stage / 'drive_c/pe32-video-startup.exe')
queue = [exe]
seen = set()
# Explicit process-attach dependencies used by the existing GUI path.
extra = ['imm32.dll']
while queue or extra:
    names = extra
    extra = []
    if queue:
        info = subprocess.check_output([str(toolchain / 'llvm-readobj'), '--coff-imports', str(queue.pop())], text=True)
        names += re.findall(r'^Import \{\n  Name: (.+)$', info, re.M)
    for name in names:
        name = name.lower()
        if name in seen:
            continue
        seen.add(name)
        module = name.removesuffix('.dll')
        assert re.fullmatch(r'[a-z0-9_-]+', module), name
        target = f'dlls/{module}/i386-windows/{name}'
        subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
        dll = pe / target
        shutil.copy2(dll, stage / 'drive_c/windows/syswow64' / name)
        queue.append(dll)
fonts = list((root / 'fonts').glob('*.ttf'))
assert fonts, 'The existing GUI path needs Wine fonts'
for folder in ['drive_c/windows/fonts', 'share/wine/fonts']:
    (stage / folder).mkdir(parents=True, exist_ok=True)
    for font in fonts:
        shutil.copy2(font, stage / folder / font.name)
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/notepad.exe\n')
(stage / 'args.txt').write_text('C:\\notepad.exe C:\\notepad-test.txt\n')
(stage / 'drive_c/notepad-test.txt').write_bytes(b'Wine-NX x86 Notepad through the ARM64 dynarec.\r\n\r\nMove the cursor with the right stick. A is the left mouse button, B the right.\r\nOpen a menu, select text by holding A, right-click with B, and try Save As.\r\n')
(stage / 'README.txt').write_text('''Wine-NX x86 programs on the nx-wow64-dynarec-33 runtime.
Build 33 hides the Switch arrow while a program hides the mouse cursor or draws its own
(OpenTTD's second cursor).
Build 32 lets translated x86 blocks jump straight to each other instead of re-checking
their code on every jump, which made loops slow (OpenTTD's long white screen).
Build 31 caches reads from the SD card (128 KB chunks), for OpenTTD's graphics loading.
Its [PROGRESS] lines add the time in file reads and SD requests, and cache hits.
Build 30 fixes OpenTTD's white screen: QueryPerformanceCounter counted from 1601 instead of
from boot, and MSVC's steady_clock overflowed. OpenTTD never drew and slept for ~49 days.
Build 29 replaced Wine's select()-based Sleep with svcSleepThread; the hang remained.
Build 29 logs a [PROGRESS] line every 10 seconds without verbose logs (seconds, file
reads, frames shown), so a program that shows nothing can be told apart from one loading.
Build 28 fixes the cursor not moving at all in build 27: the background poller
sent mouse input from a thread without a TEB and crashed at the first stick
movement. It now only reads the controller and redraws the cursor; the program's
own thread sends the input, including clicks made while it was busy.
Build 27 gates BOX64RUN, DYNAREC and input diagnostics behind verbose mode.
Build 24 implements ARM64 ntdll's direct NULL-frame longjmp for WoW64 callback returns.
Update both system32/ntdll.dll and system32/wow64.dll along with the NRO.
Positioned file reads/writes preserve the file cursor, including Wine's device-header probe.
Verbose mode logs bounded native file-read counts and header words for OpenTTD language diagnosis.

Starting Wine-NX shows a menu of the Windows programs (.exe) in
sdmc:/switch/wine/drive_c and its folders, marked x86 or ARM64:
  Up/Down (D-pad or left stick) choose, L/R page, A start, + quit,
  Y turns verbose logs on or off (for bug reports).
The menu opens on the last program started. A program's own arguments go in a file
next to it (openttd.exe reads openttd.args.txt). Otherwise args.txt is used when its
first word names the chosen program: it holds C:\\notepad.exe C:\\notepad-test.txt,
so Notepad opens its test document and other programs start without arguments.
To pick another program, close Wine-NX from HOME and start it again.

Controls in programs: the right analog stick moves the mouse cursor, A is the left
button and B the right; hold A while moving to drag or select. Touching the screen
clicks and moves the cursor there. Menus from the menu bar take the left button (A).

Try in Notepad: the blinking caret, Edit > Cut/Copy/Paste, the right-click menu (B),
Format > Font..., Format > Word Wrap and Search > Find.
Tests in the menu (each ends with [PE32 TEST] PASS ALL and exit_code=0x0000002a in
autorun_runtime.log): pe32-messages.exe (messages between threads, message waits,
clipboard), pe32-timers.exe (window timers), pe32-lifecycle.exe (threads).
pe32-video-startup.exe reproduces OpenTTD's LoadIcon, LoadCursor, RegisterClass and
CreateWindow startup sequence. Its [VIDEO TEST] lines bracket each call; the final
line reached identifies a call that does not return on hardware.

Verified on hardware: cursor and buttons, menus, Word Wrap, the Format > Font dialog
(build 11), the launcher, and pe32-messages.exe and pe32-timers.exe passing (build 16).
The Notepad caret, Copy/Paste and Find await hardware confirmation.

Logs: sdmc:/switch/wine/logs/autorun_runtime.log, and horizon-trace.log with verbose logs.
Since build 18 the log always has Wine's error messages (err:), including those of
32-bit DLLs such as a DLL that cannot be found; verbose logs add fixme: messages.
Install by merging switch/ into the SD root.
''')
subprocess.run([sys.executable, str(verify), str(stage)], check=True)
# The full package stages every checkpoint over the one before it and has only one
# archive to give; asked for a stage alone, this leaves its own unwritten.
stage_only = os.environ.get('WINE_NX_STAGE_ONLY') == '1'
if stage_only:
    print(build / 'notepad-sd-card')
else:
    archive = build / 'wine-nx-notepad-dynarec-33.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
                z.write(f, f.relative_to(build / 'notepad-sd-card'))
    with ZipFile(archive) as z:
        assert z.testzip() is None
    print(archive)
