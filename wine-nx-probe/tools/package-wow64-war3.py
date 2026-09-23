#!/usr/bin/env python3
"""Stage WarCraft III's needs over a Wine package: the setup program and the
DLLs the game and its movies load, not the game itself.

The game belongs to whoever owns it: copy the installation into
switch/wine/drive_c/WarCraft III on the card. WINE_NX_WAR3_BASE names the
package to stage over (default build-switch-wow64-dynarec/d3d9-sd-card/switch/wine).
package-wow64-full.py runs this as its last checkpoint and makes the one
archive, so this makes none."""
from pathlib import Path
import functools
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
base = Path(os.environ.get('WINE_NX_WAR3_BASE', build / 'd3d9-sd-card/switch/wine'))
stage_root = build / 'war3-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)

# What War3.exe, Game.dll, Storm.dll and blizzard.ax import from Windows (the
# game ships mss32, storm, ijl15 and msvcr80 itself), and what it and its
# movies load at run time: Direct3D, DirectShow and its decoders, sound, and
# the DLLs that earlier runs on the card reported missing.
WAR3_DLLS = '''
    advapi32 comctl32 comdlg32 gdi32 imm32 kernel32 msvcrt ole32 oleaut32 opengl32 shell32
    user32 version wininet winmm wsock32
    d3d8 d3d9 ddraw dsound mswsock wintrust wldap32 psapi uxtheme crypt32 bcrypt mpr
    quartz devenum atl100 avicap32 msdmo msvfw32 msacm32 msacm32.drv midimap
    l3codeca.acm
'''.split()

assert (base / 'drive_c/notepad.exe').is_file(), f'{base} is not a Wine package; run package-wow64-full.py first'

setup = pe / 'war3-setup.exe'
subprocess.run([str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
                '-o', str(setup), str(probe / 'tools/war3_setup.c'),
                '-ladvapi32', '-lkernel32', '-lntdll'], check=True, env=env)

shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(base, stage, ignore=shutil.ignore_patterns('*.log', '.DS_Store', '*-README.txt'))
setup_dir = stage / 'drive_c/WarCraft III Setup'
setup_dir.mkdir(parents=True)
shutil.copy2(setup, setup_dir / 'war3-setup.exe')
(stage / 'drive_c/WarCraft III').mkdir(exist_ok=True)

syswow64 = stage / 'drive_c/windows/syswow64'
queue, staged = [setup], {p.name.lower() for p in syswow64.iterdir()}
rebuilt = set()

def readobj(option, path):
    return subprocess.check_output([str(tools / 'llvm-readobj'), option, str(path)], text=True)

def file_name(name):
    name = name.lower()
    return name if name.endswith(('.dll', '.drv', '.acm')) else name + '.dll'

@functools.lru_cache(maxsize=None)
def forwards_of(path):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', path), re.M))

def stage_dll(name):
    """Build the module and stage it. Even one the base already holds is built
    and copied again, so a module changed since the base was staged (quartz) is
    current here."""
    name = file_name(name)
    assert re.fullmatch(r'[a-z0-9_-]+\.(dll|drv|acm)', name), name
    target = f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}'
    if name not in rebuilt:
        subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
        shutil.copy2(pe / target, syswow64 / name)
        rebuilt.add(name)
        staged.add(name)
        queue.append(pe / target)
    return pe / target

for name in WAR3_DLLS:
    stage_dll(name)
while queue:
    path = queue.pop()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', path), re.M | re.S):
        module = file_name(re.search(r'Name: (.+)', block).group(1))
        if module.startswith(('api-ms-', 'ext-ms-')):
            continue
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        mapping = forwards_of(stage_dll(module))
        for symbol in sorted(symbols & mapping.keys()):
            stage_dll(mapping[symbol])

(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/WarCraft III Setup/war3-setup.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
(stage / 'WARCRAFT-III-README.txt').write_text(f'''Wine-NX build {marker}: the whole SD-card payload, with what WarCraft III needs.

1. Copy the switch folder to the SD card, merging folders. It replaces the runtime
   NRO and the Wine payload of any earlier build.
2. Copy your own WarCraft III installation into switch/wine/drive_c/WarCraft III,
   so the game is at switch/wine/drive_c/WarCraft III/war3.exe. The game is not
   in this package.
3. Start Wine-NX and run C:\\\\WarCraft III Setup\\\\war3-setup.exe once (it is
   preselected), and wait for [EXIT]. It replaces the older "Register DirectShow",
   "WarCraft III Settings" and "WarCraft III Movies" programs; those folders can be
   deleted. Run it again after changing the resolution in the game's options.
4. Start WarCraft III (war3.exe) through a forwarder set to a 36-bit or 39-bit
   address space. With 32-bit its DLL layout changes from launch to launch and
   Game.dll sometimes fails to load. Leave verbose logs (Y) off: they slow the
   game down a lot.

What the setup program does, each step as a [WAR3 SETUP] line in
autorun_runtime.log, ending with "done, all steps worked":
- Sets the game to 1280x720, 32-bit colour, 60 Hz. The Switch screen, where the
  pointer and the touchscreen are, is always 1280x720; at another resolution the
  game's menus do not line up with them (no highlight, clicks landing elsewhere).
- Lets the intro movie play at startup: removes seenintromovie, which earlier
  setups set to skip it.
- Makes the game draw with its own OpenGL renderer (Gfx OpenGL) instead of
  Direct3D. Through Wine's Direct3D the game took about two cores for the frame
  rate OpenGL gives on one. A war3.args.txt holding -opengl next to war3.exe
  does the same for one launch.
- Offers the game only the Switch's own 1280x720 display mode (EmulateModelist
  for war3.exe), so the movies play across the full width.

DirectShow, which the movies play through, and the MP3 decoder for their sound
are registered by Autorun itself before the first program on the card
(C:\\windows\\autorun-setup.exe, [AUTORUN SETUP] lines in the log). The game
registers its own video decoder, blizzard.ax.

The movies: WarCraft III's Movies are AVI files, which quartz.dll reads without
GStreamer; the picture goes through Wine's GDI video renderer, and build 84 showed
it on the Switch. For each movie the game switches the screen to 800x600. Wine used
to fake that mode by scaling it into a 960x720 box in the middle of the screen,
with a white bar beside it. Since build 85, with the setup program run, only
1280x720 is offered, so the game keeps it and the movie fills the width.

The screen: windows are shown through OpenGL on the GPU, each in its own layer
drawn in stacking order, instead of copying their pixels straight to the
framebuffer. The game's Direct3D still takes the whole screen while it draws, and
the windows (the movies among them) come back when it stops. The log shows
"[INIT] windows shown by the OpenGL compositor" and "[NXCOMP]" lines. If windows
do not show or look wrong, put a file switch/wine/framebuffer.txt containing 1
on the SD card to go back to the framebuffer.

The rest of the full package is staged too: OpenTTD, the OpenGL, Direct3D 9 and
audio tests, Notepad and 7-Zip.
''')

subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
present = {p.name.lower() for p in syswow64.iterdir()}
for exe in (setup, pe / 'dlls/quartz/i386-windows/quartz.dll', pe / 'dlls/l3codeca.acm/i386-windows/l3codeca.acm'):
    info = readobj('--coff-imports', exe)
    assert 'Arch: i386\n' in info, exe
    imports = {file_name(n) for n in re.findall(r'^  Name: (.+)$', info, re.M)} - {'ntdll.dll'}
    imports = {n for n in imports if not n.startswith(('api-ms-', 'ext-ms-'))}
    assert imports <= present, f'{exe.name} imports not staged: {imports - present}'
assert (syswow64 / 'quartz.dll').read_bytes() == (pe / 'dlls/quartz/i386-windows/quartz.dll').read_bytes()
print(stage_root)
