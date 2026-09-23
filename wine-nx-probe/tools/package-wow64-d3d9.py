#!/usr/bin/env python3
"""Stage the Direct3D 9 checkpoint over an existing package: WINE_NX_D3D9_BASE,
by default build-switch-wow64-dynarec/full-sd-card/switch/wine.

Wine's d3d9 and wined3d translate Direct3D to OpenGL, which on the Switch is
Mesa's nouveau driver, so this says whether that chain stands up before a game
is asked to."""
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
base = Path(os.environ.get('WINE_NX_D3D9_BASE', build / 'full-sd-card/switch/wine'))
stage_root = build / 'd3d9-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
assert (base / 'drive_c/notepad.exe').is_file(), f'{base} is not a GUI package; run package-wow64-notepad.py first'

exe = pe / 'pe32-d3d9.exe'
subprocess.run([str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
                '-o', str(exe), str(probe / 'tests/pe32_d3d9.c'),
                '-ld3d9', '-lgdi32', '-luser32', '-lkernel32', '-lntdll'], check=True, env=env)
shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(base, stage, ignore=shutil.ignore_patterns('*.log', '.DS_Store', '*-README.txt'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
shutil.copy2(exe, stage / 'drive_c/pe32-d3d9.exe')
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

# d3d9 and its imports, the DLLs imported functions are forwarded to, and
# wined3d, which d3d9 hands the work to.
while queue:
    path = queue.pop()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', path), re.M | re.S):
        name = re.search(r'Name: (.+)', block).group(1).lower()
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        mapping = forwards_of(stage_dll(name))
        for symbol in sorted(symbols & mapping.keys()):
            stage_dll(mapping[symbol].lower() + '.dll')

(stage / 'drive_c/pe32-d3d9.args.txt').write_text('\n')
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/pe32-d3d9.exe\n')
(stage / 'run-entry.txt').write_text('1\n')  # run the program's entry point
(stage / 'D3D9-README.txt').write_text('''Wine-NX: the Direct3D 9 checkpoint.

Choose C:\\pe32-d3d9.exe in the launcher. It opens a Direct3D 9 device on the
whole screen, clears through red, green and blue for a second each with a white
triangle in the lower left, and reads every frame back from the GPU before
showing it, so a blank screen cannot pass. Expect [D3D9 TEST] PASS and
exit_code=0x0000002a in autorun_runtime.log; a failure reports which step and
the HRESULT it returned.

The adapter, driver and shader-model lines say what wined3d reports over Mesa,
which is what a Direct3D game will find. Direct3D reaches the GPU as OpenGL
here: d3d9.dll hands the work to wined3d.dll, which draws with opengl32.
''')
subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
marker_version = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)
# The full package stages every checkpoint over the one before it and has only one
# archive to give; asked for a stage alone, this leaves its own unwritten.
stage_only = os.environ.get('WINE_NX_STAGE_ONLY') == '1'
if stage_only:
    print(stage_root)
else:
    archive = build / f'wine-nx-d3d9-dynarec-{marker_version}.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
                z.write(f, f.relative_to(stage_root))
    with ZipFile(archive) as z:
        assert z.testzip() is None
    print(archive)
