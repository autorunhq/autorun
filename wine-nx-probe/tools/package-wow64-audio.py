#!/usr/bin/env python3
"""Stage the native audio checkpoint over the existing GUI package."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import functools
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
base = Path(os.environ.get('WINE_NX_AUDIO_BASE', build / 'switch/wine'))
stage_root = build / 'audio-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
if not (base / 'drive_c/notepad.exe').is_file():
    raise SystemExit('Run package-wow64-notepad.py first.')
# Build both checkpoints from sources, with no CRT dependency in either.
common = [str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
          '-fno-builtin', '-nostdlib']
exe = pe / 'pe32-audio.exe'
driver = pe / 'winenxaudio.drv'
subprocess.run(common + ['-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
    '-o', str(exe), str(probe / 'tests/pe32_audio.c'), '-lwinmm', '-lkernel32', '-lntdll'], check=True, env=env)
subprocess.run(common + ['-shared', '-Wl,--entry,_DllMain@12', '-Wl,--dynamicbase',
    '-o', str(driver), str(probe / 'source/audio_driver.c')], check=True, env=env)
shutil.copytree(base, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns('*.log', '.DS_Store'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
shutil.copy2(exe, stage / 'drive_c/pe32-audio.exe')
syswow64 = stage / 'drive_c/windows/syswow64'
shutil.copy2(driver, syswow64 / driver.name)
queue, staged = [exe, driver], set()

def readobj(option, path):
    return subprocess.check_output([str(tools / 'llvm-readobj'), option, str(path)], text=True)

@functools.lru_cache(maxsize=None)
def forwards(path):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', path), re.M))

def stage_dll(name):
    if name not in staged:
        module = name.removesuffix('.dll')
        assert re.fullmatch(r'[a-z0-9_-]+', module), name
        target = f'dlls/{module}/i386-windows/{name}'
        subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
        shutil.copy2(pe / target, syswow64 / name)
        staged.add(name)
        queue.append(pe / target)
    return str(pe / f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}')

# mmdevapi and avrt are loaded at runtime, not visible in winmm's import table.
for name in ('mmdevapi.dll', 'avrt.dll'):
    stage_dll(name)
while queue:
    path = queue.pop()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', path), re.M | re.S):
        name = re.search(r'Name: (.+)', block).group(1).lower()
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        mapping = forwards(stage_dll(name))
        for symbol in symbols & mapping.keys():
            stage_dll(mapping[symbol].lower() + '.dll')
(stage / 'drive_c/pe32-audio.args.txt').write_text('\n')
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/pe32-audio.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
(stage / 'AUDIO-README.txt').write_text('''Wine-NX build 34: first native audout playback checkpoint.
Copy the entire switch/wine folder to the SD card, merging folders. The new
winenxaudio.drv, mmdevapi.dll, winmm.dll and their dependencies are required;
replacing only the NRO is insufficient for this first audio update.
Choose C:\\pe32-audio.exe in the launcher. Expected: a quiet half-second tone
on the left followed by a half-second tone on the right, then [AUDIO TEST]
PASS with process exit 42. API completion alone does not prove audible output.

Scope: one render client with stereo 48 kHz 16-bit output, event callbacks,
volume, stop and reset. Mono or stereo PCM of 8 to 32 bits and float input are
converted, and other rates such as 44.1 kHz are resampled to 48 kHz. No
microphone, MIDI synthesis or multi-client mixing yet. The audio driver and
MMDeviceEnumerator are registered at startup, and registry changes are saved
to system.reg and user.reg in sdmc:/switch/wine/registry.

OpenTTD plays sound effects with -s win32 -m null, which the OpenTTD package
selects, with the OpenSFX base set.
''')
subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
# The full package stages every checkpoint over the one before it and has only one
# archive to give; asked for a stage alone, this leaves its own unwritten.
stage_only = os.environ.get('WINE_NX_STAGE_ONLY') == '1'
if stage_only:
    print(stage_root)
else:
    archive = build / 'wine-nx-audio-dynarec-34.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
                z.write(f, f.relative_to(stage_root))
    with ZipFile(archive) as z:
        assert z.testzip() is None
    print(archive)
