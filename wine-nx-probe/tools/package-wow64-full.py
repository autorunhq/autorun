#!/usr/bin/env python3
"""Stage everything an SD card needs for this build in one archive: the Wine
payload with its test programs, OpenTTD, the audio, OpenGL and Direct3D 9
checkpoints and what WarCraft III needs, with the WarCraft III setup
preselected in the launcher.

Each checkpoint packager runs over the previous one's stage, so the result holds
every program they stage; the build number comes from the runtime's marker.
The Need for Speed games' DLLs are staged on top, with what they import."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import functools
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
build = probe / 'build-switch-wow64-dynarec'
tools = probe / 'tools'
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)
stage_root = build / 'full-sd-card'
stage = stage_root / 'switch/wine'

# Each checkpoint packager stages the one before it and, run on its own, writes an
# archive of its stage. Here they are steps towards one package, so they are asked
# for the stage alone and only the last archive below is written.
def package(script, **base):
    subprocess.run([sys.executable, str(tools / script)], check=True,
                   env=dict(os.environ, WINE_NX_STAGE_ONLY='1', **base))

package('package-wow64-notepad.py')
package('package-wow64-openttd.py')
shutil.rmtree(build / 'audio-sd-card', ignore_errors=True)
package('package-wow64-audio.py', WINE_NX_AUDIO_BASE=str(build / 'openttd-sd-card/switch/wine'))
package('package-wow64-opengl.py', WINE_NX_OPENGL_BASE=str(build / 'audio-sd-card/switch/wine'))
package('package-wow64-d3d9.py', WINE_NX_D3D9_BASE=str(build / 'opengl-sd-card/switch/wine'))
package('package-wow64-war3.py', WINE_NX_WAR3_BASE=str(build / 'd3d9-sd-card/switch/wine'))

# The card runs Direct3D 9 through DXVK, so the runtime it carries has to be the
# one linked with mesa-switch: NVK behind winevulkan. Building
# build-switch-wow64-dynarec writes its own runtime, without Vulkan, over the NRO
# the checkpoints stage, and a game on DXVK then dies with "Failed to create
# Vulkan instance". Take it from the Mesa build and say so if it is not there.
mesa_nro = probe / 'build-switch-wow64-mesa-switch/wine-nx-runtime.nro'
assert mesa_nro.is_file(), f'{mesa_nro} is missing; build it with -DWINE_NX_MESA_SWITCH_DIR'
assert b'a Vulkan surface has the screen' in mesa_nro.read_bytes(), \
    f'{mesa_nro} has no Vulkan display driver; it is not the mesa-switch build'
assert f'nx-wow64-dynarec-{marker}'.encode() + b'\0' in mesa_nro.read_bytes(), \
    f'{mesa_nro} is stale; rebuild the runtime for build {marker}'

# The WarCraft III stage already left the other checkpoints' READMEs out, which
# describe one checkpoint each; its own is kept next to this package's.
shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(build / 'war3-sd-card/switch/wine', stage,
                ignore=shutil.ignore_patterns('*.log', '.DS_Store', 'BUILD-*-README.txt'))
shutil.copy2(mesa_nro, stage / 'wine-nx-runtime.nro')

# What the Need for Speed games import that no checkpoint stages: NFSU2's
# SPEED2.EXE, and the XtendedInput dinput8.dll in its folder (which loads the
# real dinput8.dll from syswow64), and Most Wanted's speed.exe, which adds
# d3dx9_26, with the scripts\NFS_XtendedInput.asi its ASI loader loads, which
# adds msvcp140, which loads concrt140 when it starts. quartz delay-loads ddraw
# too. Their imports, and the DLLs that exports they use forward to, come along.
NFS_DLLS = 'ddraw dinput dinput8 netapi32 shfolder tapi32 dbghelp vcruntime140 msvcp140 concrt140 xinput1_4 d3dx9_26'.split()
# Fallout New Vegas (GOG) imports xinput1_3 and d3dx9_38, and its Galaxy.dll and
# GalaxyWrp.dll import the 2012 runtimes. d3dx9 loads images through
# windowscodecs, which it delay-imports, so no import walk reaches it.
FALLOUT_DLLS = 'xinput1_3 msvcp110 msvcr110 d3dx9_38 windowscodecs'.split()
# Left 4 Dead 2's launcher loads bin\\valve_avi.dll as one of the app systems the
# engine cannot start without, and that imports AVIFIL32. msvfw32, which it
# needs in turn, is already staged for WarCraft III's movies.
SOURCE_DLLS = ['avifil32']
# Halo's keystone.dll is what checks that its own files are whole, and it
# imports WINSPOOL.DRV. Without it keystone does not load, the check cannot
# run, and Halo stops with "one of the Halo PC files is missing or corrupted".
HALO_DLLS = ['winspool.drv']
# The Sims 2 loads Activation.dll, which draws with GDI+; everything else it and
# the game import is staged already. Its own VP6 codec is the release's to
# supply, being nobody else's to give away.
SIMS2_DLLS = ['gdiplus']
# The runtime staged here is the one linked with Mesa, so the card can run
# Vulkan; the DXVK overlay adds DXVK's d3d9 in C:\\dxvk, which needs Wine's
# 32-bit loader in syswow64. vulkan-1 loads winevulkan by hand and imports
# nothing else of it, so no import walk reaches either: name both.
VULKAN_DLLS = 'vulkan-1 winevulkan'.split()
GAME_DLLS = NFS_DLLS + FALLOUT_DLLS + SOURCE_DLLS + HALO_DLLS + SIMS2_DLLS + VULKAN_DLLS
pe = probe / 'build-wine-wow64-pe'
toolchain = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{toolchain}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
syswow64 = stage / 'drive_c/windows/syswow64'
staged = {p.name.lower() for p in syswow64.iterdir()}
queue = []

def readobj(option, path):
    return subprocess.check_output([str(toolchain / 'llvm-readobj'), option, str(path)], text=True)

def dll_name(name):
    name = name.lower()
    return name if name.endswith(('.dll', '.drv')) else name + '.dll'

@functools.lru_cache(maxsize=None)
def built(name):
    assert re.fullmatch(r'[a-z0-9_-]+\.(dll|drv)', name), name
    target = f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}'
    subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
    return pe / target

@functools.lru_cache(maxsize=None)
def forwards_of(name):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', built(name)), re.M))

def need(name):
    # api-ms-win-* and ext-ms-* are API sets, which ntdll resolves; no file backs them.
    if name.startswith(('api-ms-', 'ext-ms-')) or name in staged:
        return
    shutil.copy2(built(name), syswow64 / name)
    staged.add(name)
    queue.append(name)

for name in GAME_DLLS:
    need(dll_name(name))
while queue:
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', syswow64 / queue.pop()), re.M | re.S):
        module = dll_name(re.search(r'Name: (.+)', block).group(1))
        if module.startswith(('api-ms-', 'ext-ms-')):
            continue
        need(module)
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        for symbol in sorted(symbols & forwards_of(module).keys()):
            need(dll_name(forwards_of(module)[symbol]))
for name in GAME_DLLS:
    assert 'Arch: i386\n' in readobj('--file-headers', syswow64 / dll_name(name)), name

# A program of our own that does what a game does, for the times a game says
# only that something went wrong. The launcher lists it beside the games.
apc_test = stage / 'drive_c/APC Test/apc-test.exe'
apc_test.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-O1', '-mwindows',
                '-o', str(apc_test), str(probe / 'tests/win32/apc-test.c')], check=True)
assert 'Arch: i386\n' in readobj('--file-headers', apc_test)

# What Asio does with sockets -- a completion port, AcceptEx, ConnectEx and
# overlapped reads -- which is all The Sims 2 Legacy's launcher emulation does.
socket_test = stage / 'drive_c/Socket Test/socket-test.exe'
socket_test.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-O1', '-mwindows',
                '-o', str(socket_test), str(probe / 'tests/win32/socket-test.c'), '-lws2_32'], check=True)
assert 'Arch: i386\n' in readobj('--file-headers', socket_test)

# What wineboot registers on a computer and nothing does on the Switch:
# DirectShow, DirectX Media Objects and the MP3 decoder. The runtime runs it
# before the first program on a card (source/runtime.c), for every game.
autorun_setup = stage / 'drive_c/windows/autorun-setup.exe'
subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
                '-fno-builtin', '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000',
                '-Wl,--dynamicbase', '-o', str(autorun_setup), str(tools / 'autorun_setup.c'),
                '-lole32', '-ladvapi32', '-lkernel32', '-lntdll'], check=True)
assert 'Arch: i386\n' in readobj('--file-headers', autorun_setup)

# The Sims 2 Ultimate Collection is shipped installed; what is left is telling
# the game where each of its packs is, which its release does with a batch file
# of reg add lines whose every path comes from the folder it is run in.
sims2 = stage / 'drive_c/The Sims 2 Setup'
sims2.mkdir(parents=True, exist_ok=True)
subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
                '-fno-builtin', '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000',
                '-Wl,--dynamicbase', '-o', str(sims2 / 'sims2-setup.exe'),
                str(tools / 'sims2_setup.c'), '-ladvapi32', '-lkernel32', '-lntdll'], check=True)
assert 'Arch: i386\n' in readobj('--file-headers', sims2 / 'sims2-setup.exe')
# DXVK's settings for the game, which the setup copies beside each executable.
shutil.copy2(tools / 'sims2/dxvk.conf', sims2 / 'dxvk.conf')
(sims2 / 'README.txt').write_text('''The Sims 2 Ultimate Collection
==============================

Copy the collection onto the card -- leave __Installer and Support behind, they
are for a computer -- and run sims2-setup.exe once from the launcher. It writes
what the release's own "Instalar Registros" batch file writes, with the card's
paths, and says what it did in autorun_runtime.log as [SIMS2 SETUP] lines.
Running it again is harmless.

Where the packs go does not matter much. Each one is recognised by the
executable in its TSBin, not by the name of the folder around it, so a release
that calls them Base and EP1-EP9 and one that spells out "The Sims 2 Nightlife"
both work, and the collection may keep a folder of its own around them. Put
sims2-setup.exe's folder beside the packs, or beside the folder holding them.

The setup also copies dxvk.conf from its folder into each pack's TSBin, next to
the executable. It holds the game to 512 MB of video memory: DXVK's own profile
for The Sims 2 reports 2 GB, and on the Switch the game fills the shared 1.5 GB
and crashes. A dxvk.conf already in TSBin is left as it is.

It also sets the game's own Graphics Rules.sgr (TSData\\Res\\Config in each pack)
for the Switch's 1280x720 screen: in its screen resolution option every default
becomes 1280x720, and a maximum below that is raised to it. Nothing else in the
file changes, and the file as it was is kept as Graphics Rules.sgr.original.

The game is the newest expansion's executable, TSBin\\Sims2EP9.exe. It has no
relocations and is linked for 0x400000, so it requires the 39-bit Autorun
forwarder and the Atmosphere low-address patch.

The game's own movies -- the intro, the EA logo, what plays on a television --
are .movie files in Maxis' own format, which the game reads itself: they need no
codec and nothing registered.

VP6 is for the video the game writes rather than the video it reads. It records
gameplay through Video for Windows (AVIStreamWrite and ICSeqCompressFrame are
what Sims2EP9.exe imports), and a custom video made as a VP6 AVI is read the
same way. The codec is the release's to supply and nobody else's to give away:
copy __Installer\\customcomponent\\vp6\\vp6vfw.dll into C:\\windows\\syswow64 on
the card if you want either. The setup registers it under both the names Video
for Windows opens it by, whether or not the file is there.

The language number does two things: the game reads it, and so does the
release's own launcher emulation, which anadius.cfg points at a key of its own
by setting its language to "invalid". The setup writes both. Getting only the
first is how the game comes up saying "open: Invalid handle" and stops.

For a language other than English, put its number in language.txt beside
sims2-setup.exe before running it:

  1 English (United States)   10 Portuguese (Brazil)    17 Chinese (Simplified)
  2 French                    11 Czech                  18 Chinese (Traditional)
  3 German                    13 English (United Kingdom) 20 Polish
  4 Italian                   14 Japanese               21 Thai
  5 Spanish                   15 Korean                 22 Norwegian
  6 Swedish                   16 Russian                23 Portuguese (Portugal)
  7 Finnish                                             24 Hungarian
  8 Dutch
  9 Danish
''')
(stage / 'drive_c/The Sims 2').mkdir(exist_ok=True)

# Fallout New Vegas hands itself over to its own launcher unless it recognises
# the card's display: FalloutNV.exe compares sD3DDevice with what adapter 0
# calls itself, and with no FalloutPrefs.ini that default is the empty string,
# which never matches. It then ShellExecutes FalloutNVLauncher.exe and returns.
# The card gets the file the launcher would have written, naming the Switch's
# adapter the way DXVK reports it, at 720p. The game rewrites this file itself
# once its own options are used, so the values are a starting point, not a rule.
fallout = stage / 'drive_c/users/steamuser/Documents/My Games/FalloutNV'
fallout.mkdir(parents=True, exist_ok=True)
(fallout / 'FalloutPrefs.ini').write_bytes('\r\n'.join((
    '[Display]',
    'sD3DDevice="NVIDIA Tegra X1 (GM20B) (NVK GM20B)"',
    'iAdapter=0',
    'iSize W=1280',
    'iSize H=720',
    'bFull Screen=1',
    'iMultiSample=0',
    '')).encode())

# The classes those DLLs serve, which on Windows their own DllRegisterServer
# would have written when they were installed.
subprocess.run([sys.executable, str(tools / 'make-classes-reg.py'), str(stage)], check=True)

# The launcher lists every program in drive_c; target.txt only preselects one.
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/WarCraft III Setup/war3-setup.exe\n')
# Everything a person sets, in one file, where a dozen loose toggles were.
config = stage / 'config'
config.mkdir(parents=True, exist_ok=True)
(config / 'settings.json').write_text('''{
  "run-the-chosen-program": true,
  "verbose-log": false,
  "profiler": false,
  "core-balancing": true,
  "display-devices": true,
  "windows-through-opengl": true,
  "vulkan-probe": false,
  "reopen-the-launcher-on-exit": true,
  "dxvk-for-new-games": true,
  "hand-the-process-back-anyway": false,
  "gl-pinned-buffers-cached": true,
  "gl-clean-before-submit": true,
  "gl-clean-test": false
}
''')
for gone in ('run-entry.txt', 'verbose.txt', 'profile.txt', 'framebuffer.txt', 'no-balance.txt',
             'no-display-devices.txt', 'vulkan-probe.txt', 'reload-launcher.txt', 'loader-anyway.txt',
             'gl-uncached.txt', 'gl-noclean.txt', 'gl-clean-test.txt'):
    (stage / gone).unlink(missing_ok=True)
# The controller stands in for a keyboard; this lists what each control sends
# and how to change it, with every line commented out so the defaults hold.
(config / 'keys.txt').write_text('''# Keys the controller sends, one NAME=code line each, where code is a Windows
# virtual-key code in decimal or 0x form. Remove the # to change one. A and B
# are not here: they stay the left and right mouse buttons.
#
# UP=0x26      d-pad up, and the left stick pushed up unless LUP is set
# DOWN=0x28
# LEFT=0x25
# RIGHT=0x27
# X=0x20       space
# Y=0x46       f
# L=0x09       tab
# R=0x10       shift
# ZL=0x28      down arrow, a brake in a racing game
# ZR=0x26      up arrow, the accelerator
# PLUS=0x1B    escape
# MINUS=0x09   tab
# STICKL=0x11  control
# STICKR=0x12  alt
# LUP=0        the left stick alone, for a game that walks with one set of keys
# LDOWN=0      and works its menus with another. Unset, it sends what the d-pad
# LLEFT=0      does.
# LRIGHT=0
# RUP=0x26     the right stick, when it is set to send keys rather than move
# RDOWN=0x28   the mouse
# RLEFT=0x25
# RRIGHT=0x27
# TUP=0x26     a finger dragged across the screen, likewise
# TDOWN=0x28
# TLEFT=0x25
# TRIGHT=0x27
#
# And what each of the four that can point does, which is mouse or keys:
#
# LSTICK=keys
# RSTICK=mouse
# DPAD=keys
# TOUCH=mouse
''')
(stage / f'BUILD-{marker}-README.txt').write_text(f'''Wine-NX build {marker}: the whole SD-card payload.
Copy the switch folder to the SD card, merging folders; it replaces the runtime
NRO and the Wine payload of any earlier build.

The launcher lists the programs in drive_c. The WarCraft III setup is
preselected; WARCRAFT-III-README.txt says how to add the game and run it.

C:\\\\openttd\\\\openttd.exe draws with OpenGL on the Switch GPU (Mesa), plays sound
effects through the win32 driver, and reads openttd.args.txt next to it; putting
-v win32:no_threads there goes back to GDI drawing.
Also staged: C:\\\\pe32-opengl.exe (red, green and blue frames, then PASS and
exit_code=0x0000002a), C:\\\\pe32-audio.exe (audout playback), C:\\\\notepad.exe and
the 7zr benchmark.

Need for Speed Underground 2 and Most Wanted: the DLLs SPEED2.EXE and speed.exe
import are staged (ddraw, dinput8, netapi32, shfolder, tapi32, d3dx9_26 and what
they import), with dbghelp, msvcp140, vcruntime140 and xinput1_4 for XtendedInput:
NFSU2's dinput8.dll and Most Wanted's NFS_XtendedInput.asi. Neither executable
can be moved in memory, so start them through a forwarder set to a 32-bit
address space.

Fallout New Vegas (GOG): xinput1_3, d3dx9_38 and the windowscodecs that loads its
textures are staged, with msvcp110 and msvcr110 for Galaxy.dll and GalaxyWrp.dll.
Its executable relocates, so it needs no forwarder. Started with no settings of
its own the game hands itself to FalloutNVLauncher.exe and closes, because the
display it is told to use is not one it recognises, so the payload brings the
settings file it would have written:

    C:\\users\\steamuser\\Documents\\My Games\\FalloutNV\\FalloutPrefs.ini

It names the Switch's GPU as DXVK reports it, at 1280x720. The game rewrites
that file once its own options are used; if it already holds settings worth
keeping, keep the [Display] sD3DDevice line and merge the rest.

The Sims 2 Ultimate Collection: everything Sims2EP9.exe imports is already
staged, and gdiplus for the Activation.dll it loads. The release comes installed, so nothing has to be unpacked; what is
missing is the registry the game reads to find each pack, which its own batch
file writes with the paths of the computer it was unpacked on. C:\\The Sims 2
Setup\\sims2-setup.exe writes the same with the card's, and its README says how.

Halo: Combat Evolved: winspool.drv is staged, which is what Halo checks its own
files with. Delete the ._ files a Mac leaves beside every file on the card if
the game was copied from one: Halo loads every DLL in its Controls folder and
one of those is not a DLL. Halo walks with w, a, s and d and works its menus
with the arrows, so the left stick takes one set and the d-pad the other. Put
this beside HALO.EXE as HALO.keys.txt, which is applied over keys.txt:

    LUP=0x57    w, forward: the left stick alone
    LDOWN=0x53  s
    LLEFT=0x41  a
    LRIGHT=0x44 d
    UP=0x26     the arrows, for the menus: the d-pad alone
    DOWN=0x28
    LEFT=0x25
    RIGHT=0x27
    X=0x0d      Enter, to choose a menu item
    Y=0x20      space, to jump
    L=0x45      e, the action key
    R=0x52      r, to reload
    ZL=0x11     left control, to crouch
    ZR=0x09     tab, the scores
    MINUS=0x1b  Escape, to go back

A and B stay the left and right mouse buttons, so A fires and clicks. The right
stick turns the view and the touchscreen drags it like a trackpad. Holding +
and - together leaves the game whatever the keys say. Everything else is in
Halo's own Settings, Controls, which the mouse can now reach.

Left 4 Dead 2: the engine will not start without bin\\valve_avi.dll, which is one
of the app systems its launcher creates, and that imports AVIFIL32, so avifil32
is staged; msvfw32, which avifil32 needs, was already there for WarCraft III's
movies. left4dead2.exe relocates, so it needs no forwarder either. Steam's
GameOverlayRenderer.dll not loading is expected and harmless.

The screen: windows are now shown through OpenGL on the GPU, each in its own
layer drawn in stacking order, instead of copying their pixels straight to the
framebuffer. A program's own OpenGL (OpenTTD, Direct3D games) still takes the
whole screen while it draws; the windows come back when it stops. The log shows
"[INIT] windows shown by the OpenGL compositor" and "[NXCOMP]" lines. If windows
do not show or look wrong, put a file switch/wine/framebuffer.txt containing 1
on the SD card to go back to the framebuffer.

Settings: switch/wine/config/settings.json holds every one of them, and
config/keys.txt the keys the controller sends. Both are set from the launcher --
Settings, Defaults, Controls for the keys everything sends, and a program's own
Controls row for the keys it alone sends -- so neither has to be written by
hand; left and right change a control, A opens the whole list of keys, Y puts
one back to its default. The left stick, the right stick, the d-pad and a
finger dragged across the screen each move the mouse or send four keys, which
the same screen sets: a game played with the mouse wants both sticks on it, one
played with the keyboard wants the keys it walks with. A program uses Autorun's
controls until its own are turned on in its settings, and turning them off again
keeps the keys that were set. A card written by an earlier
build has them loose beside the launcher -- verbose.txt, framebuffer.txt,
no-balance.txt and the rest -- and the first run moves each into settings.json
and takes the file away, saying so in the log. A setting a newer build added is
kept when an older one writes the file back.

autorun_runtime.log holds the run. Its [PROGRESS] lines report OpenGL frames,
the time in eglSwapBuffers and in opengl32 calls, the megabytes Wine copies for
32-bit buffer mappings (copy_mb), whether the GPU maps the program's own pages
(pinned=1, or -1 with pin_rc when nvservices refused them), and the slowest
opengl32 calls of the last ten seconds.
''')
subprocess.run([sys.executable, str(tools / 'verify-wow64-package.py'), str(stage)], check=True)

archive = build / f'wine-nx-full-dynarec-{marker}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    for f in sorted(stage.rglob('*')):
        if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
            z.write(f, f.relative_to(stage_root))
        # Empty folders are places to copy things into, such as drive_c/WarCraft III.
        elif f.is_dir() and not any(f.iterdir()):
            z.write(f, f.relative_to(stage_root))
with ZipFile(archive) as z:
    assert z.testzip() is None

# The checkpoints' stages are steps on the way here, and each is most of a card's
# worth of files; the one this package was made from stays.
for step in ('notepad', 'openttd', 'audio', 'opengl', 'd3d9', 'war3'):
    shutil.rmtree(build / f'{step}-sd-card', ignore_errors=True)
print(archive)
