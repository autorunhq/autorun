#!/usr/bin/env python3
"""Stage the OpenGL checkpoint, pe32-opengl.exe, over an existing GUI package:
WINE_NX_OPENGL_BASE, by default build-switch-wow64-dynarec/switch/wine."""
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
base = Path(os.environ.get('WINE_NX_OPENGL_BASE', build / 'switch/wine'))
stage_root = build / 'opengl-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
assert (base / 'drive_c/notepad.exe').is_file(), f'{base} is not a GUI package; run package-wow64-notepad.py first'

exe = pe / 'pe32-opengl.exe'
subprocess.run([str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
                '-o', str(exe), str(probe / 'tests/pe32_opengl.c'),
                '-lopengl32', '-lgdi32', '-luser32', '-lkernel32', '-lntdll'], check=True, env=env)
shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(base, stage, ignore=shutil.ignore_patterns('*.log', '.DS_Store', '*-README.txt'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
shutil.copy2(exe, stage / 'drive_c/pe32-opengl.exe')
syswow64 = stage / 'drive_c/windows/syswow64'
queue, staged = [exe], {p.name.lower() for p in syswow64.glob('*.dll')}

def readobj(option, path):
    return subprocess.check_output([str(tools / 'llvm-readobj'), option, str(path)], text=True)

@functools.lru_cache(maxsize=None)
def forwards_of(path):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', path), re.M))

def stage_dll(name):
    target = f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}'
    if name not in staged:
        assert re.fullmatch(r'[a-z0-9_-]+\.dll', name), name
        subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
        shutil.copy2(pe / target, syswow64 / name)
        staged.add(name)
        queue.append(pe / target)
    return pe / target

# opengl32 and its imports, and the DLLs imported functions are forwarded to.
while queue:
    path = queue.pop()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', path), re.M | re.S):
        name = re.search(r'Name: (.+)', block).group(1).lower()
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        mapping = forwards_of(stage_dll(name))
        for symbol in sorted(symbols & mapping.keys()):
            stage_dll(mapping[symbol].lower() + '.dll')

(stage / 'drive_c/pe32-opengl.args.txt').write_text('\n')
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/pe32-opengl.exe\n')
(stage / 'run-entry.txt').write_text('1\n')  # run the program's entry point
(stage / 'OPENGL-README.txt').write_text('''Wine-NX build 41: OpenGL checkpoint.
Build 41 gives 32-bit programs zero-copy GPU buffers: Mesa's nouveau maps their
pages for the GPU (GL_AMD_pinned_memory), so persistent buffer mappings work and
Wine no longer copies every mapped buffer in and out. [PROGRESS] reports pinned=1
when that works, pinned=-1 with pin_rc when nvservices refused the pages and the
copies stay, plus frames, time in eglSwapBuffers and the slowest opengl32 calls.
Build 38 creates contexts with their pixel format's EGL config: devkitPro's Mesa
lacks EGL_KHR_no_config_context, so build 37's wglCreateContext failed.
Copy the switch/wine folder to the SD card, merging folders; the runtime NRO
changed and opengl32.dll must be in drive_c/windows/syswow64.
Choose C:\\\\pe32-opengl.exe in the launcher. Expected: one second each of full
red, green and blue, then [OPENGL TEST] PASS and exit_code=0x0000002a in
autorun_runtime.log. The GL_VENDOR, GL_RENDERER and GL_VERSION lines name the
driver; [NXGL] lines show the screen passing to OpenGL and back.

OpenGL goes through devkitPro's Mesa (nouveau) and EGL on the Switch GPU. While
a program has an OpenGL window, it owns the whole screen and other windows are
not shown; when it closes the window, GDI drawing returns. Programs that do not
use OpenGL keep the GDI path.
''')
subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
# The full package stages every checkpoint over the one before it and has only one
# archive to give; asked for a stage alone, this leaves its own unwritten.
stage_only = os.environ.get('WINE_NX_STAGE_ONLY') == '1'
if stage_only:
    print(stage_root)
else:
    archive = build / 'wine-nx-opengl-dynarec-41.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
                z.write(f, f.relative_to(stage_root))
    with ZipFile(archive) as z:
        assert z.testzip() is None
    print(archive)
