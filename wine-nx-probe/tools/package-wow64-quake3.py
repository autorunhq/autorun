#!/usr/bin/env python3
"""Stage Quake3e over the full package: the engine only, not the game data.

The engine is the project's own 32-bit mingw build, which the release page
serves as quake3e-windows-mingw-x86.zip; WINE_NX_QUAKE3_INPUTS names the folder
holding it (default ~/switch/winebox64_nx/local-inputs). Quake III Arena's paks
belong to whoever owns the game: copy baseq3 from an installation into
switch/wine/drive_c/quake3 on the card, next to the staged engine."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import functools
import hashlib
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
base = Path(os.environ.get('WINE_NX_QUAKE3_BASE', build / 'full-sd-card/switch/wine'))
inputs = Path(os.environ.get('WINE_NX_QUAKE3_INPUTS', Path.home() / 'switch/winebox64_nx/local-inputs'))
engine_zip = inputs / 'quake3e-windows-mingw-x86.zip'
# A CD key belongs to whoever owns the game, so it is read from the inputs
# folder rather than kept here, and staged only when it is there.
key_file = inputs / 'q3key'
stage_root = build / 'quake3-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)

# The game reads its own console commands from the command line, so it needs no
# keyboard to start. Windowed 1280x720 because the driver owns the whole screen
# anyway, mouse through window messages rather than DirectInput, and interpreted
# QVMs so the game's own code generator is not stacked on top of the dynarec.
# r_fbo 0 keeps it drawing on the window's own framebuffer, which has the depth
# buffer its pixel format asked for. Quake III brightens through the hardware
# gamma ramp, which it only sets in fullscreen, so it does that in software
# here instead, or everything looks dark.
ARGUMENTS = (r'+set fs_basepath C:\quake3 +set fs_homepath C:\quake3 +set logfile 2 '
             r'+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 '
             r'+set in_mouse -1 +set r_fbo 0 '
             r'+set r_ignorehwgamma 1 +set r_gamma 1.3 +set r_overBrightBits 0 '
             r'+set s_initsound 1 '
             r'+set vm_game 1 +set vm_cgame 1 +set vm_ui 1')

assert engine_zip.is_file(), f'Missing input: {engine_zip}'
assert (base / 'drive_c/notepad.exe').is_file(), f'{base} is not a Wine package; run package-wow64-full.py first'

shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(base, stage, ignore=shutil.ignore_patterns('*.log', '.DS_Store', '*-README.txt'))
game = stage / 'drive_c/quake3'
game.mkdir(parents=True, exist_ok=True)
with ZipFile(engine_zip) as z:
    exe = game / 'quake3e.exe'
    exe.write_bytes(z.read('quake3e.exe'))
(game / 'quake3e.args.txt').write_text(ARGUMENTS + '\n')
if key_file.is_file():
    (game / 'baseq3').mkdir(parents=True, exist_ok=True)
    shutil.copy2(key_file, game / 'baseq3/q3key')

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

# What the engine imports, what those import, and where forwarded exports lead.
# It loads opengl32 itself, and the sound driver when sound is turned back on.
for name in ('opengl32.dll', 'dsound.dll'):
    stage_dll(name)
while queue:
    path = queue.pop()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', path), re.M | re.S):
        module = re.search(r'Name: (.+)', block).group(1).lower()
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        mapping = forwards_of(stage_dll(module))
        for symbol in sorted(symbols & mapping.keys()):
            stage_dll(mapping[symbol].lower() + '.dll')

(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/quake3/quake3e.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
(stage / f'QUAKE3-README.txt').write_text(f'''Wine-NX build {marker}: Quake III Arena through Quake3e.

Copy the switch folder to the SD card, then copy baseq3 from your own Quake III
Arena installation into switch/wine/drive_c/quake3, so that the paks sit at
switch/wine/drive_c/quake3/baseq3/pak0.pk3 and so on. The engine is staged, the
game data is not.

Choose C:\\\\quake3\\\\quake3e.exe in the launcher. quake3e.args.txt next to it
holds the command line: windowed 1280x720, sound on, brightness applied in
software because Quake III only sets the hardware gamma ramp in fullscreen,
and the game's QVM code generator left as the interpreter.

The menus work with the touchscreen and the sticks, since the runtime moves the
cursor with them, but there is no keyboard yet, so a match cannot be steered.
Quake III asks for a CD key once unless baseq3/q3key holds one; the packager
copies that file from its inputs folder when it is there, so a key stays with
whoever owns the game.

For a benchmark rather than a game, add "+timedemo 1 +demo four": the engine
then plays a recorded demo as fast as it can, with no frame pacing and no sound
to wait for, and writes the frame rate to drive_c/quake3/qconsole.log. Try the
faster QVM code generator with vm_game 2, vm_cgame 2 and vm_ui 2, and fewer
draw calls with +set r_vbo 1.

autorun_runtime.log holds the runtime's own view: [PROGRESS] lines report the
frames, the time inside opengl32 and the slowest calls.
''')

subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
info = readobj('--coff-imports', exe)
assert 'Arch: i386\n' in info
imports = {n.lower() for n in re.findall(r'^  Name: (.+)$', info, re.M)}
present = {p.name.lower() for p in syswow64.glob('*.dll')}
assert imports <= present, f'Quake3e imports not staged: {imports - present}'
assert 'opengl32.dll' in present, 'the engine loads opengl32 at run time'

archive = build / f'wine-nx-quake3-dynarec-{marker}.zip'
overlay = build / f'wine-nx-quake3-overlay-{marker}.zip'
staged_files = [f for f in sorted(stage.rglob('*'))
                if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log']
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    for f in staged_files:
        z.write(f, f.relative_to(stage_root))
# A card that already holds the base needs only what this package adds or
# changes, which is a few megabytes instead of the whole payload.
with ZipFile(overlay, 'w', ZIP_DEFLATED) as z:
    for f in staged_files:
        old_file = base / f.relative_to(stage)
        if not old_file.is_file() or old_file.read_bytes() != f.read_bytes():
            z.write(f, f.relative_to(stage_root))
for path in (archive, overlay):
    with ZipFile(path) as z:
        assert z.testzip() is None
    print(f'{path} ({path.stat().st_size / 2**20:.1f} MiB, {len(ZipFile(path).namelist())} files)')
