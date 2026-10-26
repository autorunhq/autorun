#!/usr/bin/env python3
"""Build autorun-NNN.zip, the SD-card archive: the runtime, the files Wine reads
beside it, Autorun's own setup programs and the default settings.

    package-autorun.py [--x86] [--amd64 ZIP] [--dlls DIR]

NNN is the build number in source/runtime.c. The runtime is the AMD64 one
(build-amd64-components.sh), which runs 32- and 64-bit programs. --x86 ships
the x86-only runtime instead (build-x86.sh), for trying it: 32-bit programs
through Box64, without FEX. The archive holds no Windows modules: those are
the DLL repository's (horizon-dlls), which the package is checked against."""
from pathlib import Path
from pathlib import PurePosixPath
from zipfile import ZipFile, ZIP_DEFLATED
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys

horizon_wine = Path(__file__).resolve().parents[1]
root = horizon_wine.parent
tools = horizon_wine / 'tools'
build = horizon_wine / 'build-autorun'
stage_root = build / 'sd-card'
stage = stage_root / 'switch/wine'
marker = re.search(r'nx-wow64-dynarec-(\d+)', (horizon_wine / 'source/runtime.c').read_text()).group(1)
# The toolchain build.sh fetches: its release name is per-platform, and the
# build exports WINE_NX_LLVM_MINGW with the one it chose. Anything already
# fetched will do as a last resort.
llvm_version = '20260505'
machine = 'aarch64' if platform.machine() in ('aarch64', 'arm64') else 'x86_64'
name = ('llvm-mingw-%s-ucrt-macos-universal' % llvm_version if platform.system() == 'Darwin'
        else 'llvm-mingw-%s-ucrt-ubuntu-22.04-%s' % (llvm_version, machine))
toolchain = (Path(os.environ['WINE_NX_LLVM_MINGW']) / 'bin' if os.environ.get('WINE_NX_LLVM_MINGW')
             else horizon_wine / 'toolchains' / name / 'bin')
if not (toolchain / 'i686-w64-mingw32-clang').is_file():
    toolchain = next((p / 'bin' for p in sorted((horizon_wine / 'toolchains').glob('llvm-mingw-*'))
                      if (p / 'bin' / 'i686-w64-mingw32-clang').is_file()), toolchain)
assert (toolchain / 'i686-w64-mingw32-clang').is_file(), \
    f'No llvm-mingw toolchain in {horizon_wine / "toolchains"}; run build.sh, or set WINE_NX_LLVM_MINGW.'
# The FEX build when there is one, as build-amd64-components.sh names it.
default_amd64 = next((path for path in (horizon_wine / f'build-switch-amd64/wine-nx-amd64-box64{kind}-mesa-dxvk-vkd3d.zip'
                                        for kind in ('-fex', '')) if path.is_file()),
                     horizon_wine / 'build-switch-amd64/wine-nx-amd64-box64-mesa-dxvk-vkd3d.zip')
parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--amd64', type=Path,
                    default=Path(os.environ.get('WINE_NX_AMD64_PACKAGE', default_amd64)))
parser.add_argument('--x86', action='store_true', help='ship the x86-only runtime instead of the AMD64 one')
parser.add_argument('--dlls', type=Path, default=root / 'horizon-dlls/switch/wine',
                    help="the DLL repository's card tree, to check the package against")
parser.add_argument('--extra', type=Path,
                    help='a switch/wine overlay to stage last: the tuned FEX modules a build fetched')
parser.add_argument('--fixes', type=Path, default=root / 'game-fixes/switch/wine/game-fixes/index.json',
                    help="the game fixes repository's index, shipped so a card offline knows of them")
args = parser.parse_args()


def compile_setup(source, output, *libs):
    """One of Autorun's own small i386 programs, which the card runs from C:."""
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
                    '-fno-builtin', '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000',
                    '-Wl,--dynamicbase', '-o', str(output), str(source), *libs, '-lkernel32', '-lntdll'],
                   check=True)


# --- What Wine reads beside the runtime --------------------------------------

shutil.rmtree(build, ignore_errors=True)
stage.mkdir(parents=True)
for nls in (root / 'nls').glob('*.nls'):
    (stage / 'share/wine/nls').mkdir(parents=True, exist_ok=True)
    shutil.copy2(nls, stage / 'share/wine/nls' / nls.name)
fonts = list((root / 'fonts').glob('*.ttf'))
assert fonts, 'Wine has no fonts to stage'
for folder in ('drive_c/windows/fonts', 'share/wine/fonts'):
    (stage / folder).mkdir(parents=True, exist_ok=True)
    for font in fonts:
        shutil.copy2(font, stage / folder / font.name)
licenses = stage / 'licenses'
licenses.mkdir()
for source, name in ((root / 'COPYING.LIB', 'Wine-LGPL-2.1.txt'),
                     (horizon_wine / 'vendor/box64/LICENSE', 'Box64-MIT.txt'),
                     (horizon_wine / 'licenses/libjpeg-turbo.txt', 'libjpeg-turbo.txt')):
    shutil.copy2(source, licenses / name)

# --- Autorun's setup programs ------------------------------------------------

drive = stage / 'drive_c'
# What wineboot registers on a computer and nothing does on the Switch:
# DirectShow, DirectX Media Objects and the MP3 decoder. The runtime runs it
# before the first program on a card (source/runtime.c), for every game.
compile_setup(tools / 'autorun_setup.c', drive / 'windows/autorun-setup.exe', '-lole32', '-ladvapi32')

# Places to copy a game into. What a game needs beyond Autorun's own setup --
# registry keys, settings files, controls -- comes from the game fixes
# repository (autorun-game-fixes): the launcher recognises the game and offers
# its fix before it starts. The archive ships the index it was built with, so a
# card offline when a game starts still says the game has a fix to download,
# and the files of the fixes game-fixes-extra/ adds, which then apply offline.
(drive / 'WarCraft III').mkdir()
(drive / 'The Sims 2').mkdir()
assert args.fixes.is_file(), f'{args.fixes} is missing; check out game-fixes'
fixes_index = json.loads(args.fixes.read_text())
assert fixes_index['schema'] == 1 and fixes_index['fixes'], f'{args.fixes} is not a game fixes index'
(stage / 'game-fixes').mkdir()

# Game fixes this build adds, from game-fixes-extra/. The game fixes repository
# is where every fix belongs; a folder here is one this build carries until it
# is published there -- a fix.json as that repository takes it (its AGENTS.md),
# written out the way its build would: registry.txt from the registry list and
# the generic setup beside it, the repository's own build of that program. The
# files go into the archive under game-fixes/<id>/, so the launcher applies
# them with no network. The index serial is one past the repository's: a card
# reads whichever index has the higher serial and would read past these fixes
# otherwise. Once the repository has them, the folder goes.
extra_fixes = []
extra_root = root / 'game-fixes-extra'
if extra_root.is_dir():
    setups = list(args.fixes.parent.glob('*/autorun-registry.exe'))
    assert setups, f'no autorun-registry.exe beside {args.fixes}'
    setup = setups[0].read_bytes()
    assert all(p.read_bytes() == setup for p in setups), 'autorun-registry.exe differs between fixes'
    for extra in sorted(p for p in extra_root.iterdir() if p.is_dir()):
        assert re.fullmatch(r'[a-z0-9-]{1,31}', extra.name), f'{extra.name} is not a fix id'
        fix = json.loads((extra / 'fix.json').read_text(encoding='utf-8'))
        assert set(fix) <= {'title', 'version', 'required', 'summary', 'match', 'registry'}, \
            f'{extra}/fix.json has fields this packer does not write out'
        assert 'registry' in fix and 'match' in fix and fix['match'].get('exe')
        assert isinstance(fix['version'], int) and fix['version'] >= 1
        assert len(fix['title'].encode()) < 96 and len(fix['summary'].encode()) < 640
        edits = []
        for edit in fix['registry']:
            assert set(edit) <= {'key', 'name', 'string', 'dword', 'delete', 'view'}
            assert edit['key'].startswith(('HKCU\\', 'HKLM\\')) and '\\' in edit['key'][5:]
            if edit.get('delete'):
                kind, value = 'delete', ''
            elif 'dword' in edit:
                kind, value = 'dword', str(edit['dword'])
            else:
                kind, value = 'sz', edit['string']
                assert '\t' not in value and '\n' not in value and len(value) < 1000
            edits.append('\t'.join((str(edit.get('view', 32)), edit['key'], edit.get('name', ''), kind, value)))
        assert edits, f'{extra}/fix.json writes nothing'
        registry_txt = ('\n'.join(edits) + '\n').encode('utf-8')
        folder = stage / 'game-fixes' / extra.name
        folder.mkdir()
        (folder / 'autorun-registry.exe').write_bytes(setup)
        (folder / 'registry.txt').write_bytes(registry_txt)
        extra_fixes.append({
            'id': extra.name,
            'title': fix['title'],
            'version': fix['version'],
            'required': bool(fix.get('required')),
            'summary': fix['summary'],
            'match': fix['match'],
            'run': 'autorun-registry.exe',
            'files': [
                {'name': 'autorun-registry.exe', 'size': len(setup),
                 'sha256': hashlib.sha256(setup).hexdigest()},
                {'name': 'registry.txt', 'size': len(registry_txt),
                 'sha256': hashlib.sha256(registry_txt).hexdigest()},
            ],
        })
if extra_fixes:
    fixes_index['fixes'] = fixes_index['fixes'] + extra_fixes
    fixes_index['serial'] += 1
    (stage / 'game-fixes/bundled.json').write_text(
        json.dumps(fixes_index, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
else:
    shutil.copy2(args.fixes, stage / 'game-fixes/bundled.json')


# --- Settings ----------------------------------------------------------------

config = stage / 'config'
config.mkdir()
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

(stage / 'README.txt').write_text(f'''Autorun build {marker}
================

Copy the switch folder to the SD card, merging folders; it replaces the runtime
NRO of any earlier build. The Windows DLLs are not in this archive: Autorun
downloads them itself (Settings, System, Windows DLLs), or copy the switch
folder of autorun-horizon-dlls' release the same way.

The launcher lists the programs in drive_c.

Game fixes: some games need more than Autorun gives every program -- registry
keys their installer would have written, a settings file their own launcher
would have made, controls that suit them. Those come from the game fixes
repository (github.com/autorunhq/autorun-game-fixes). When a game in the
library has one, the launcher asks before the game starts, and says when the
game may not start without it; the fix is downloaded, checked and applied, and
the game starts after it. This archive knows which games have fixes even
offline; applying one needs the internet once, except a fix whose files this
archive holds under game-fixes/, which applies offline as well. Which games
have one:
github.com/autorunhq/autorun-game-fixes/blob/main/FIXES.md. A game's Library
settings show its fix, and choosing it asks again at the next
start. Without a network, copy the switch folder of autorun-game-fixes to the
card the same way as the DLLs.

WarCraft III: start war3.exe through a forwarder set to a 36-bit or 39-bit
address space. With 32-bit its DLL layout changes from launch to launch and
Game.dll sometimes fails to load.

The Sims 2 Ultimate Collection: the game is the newest expansion's executable,
TSBin\\Sims2EP9.exe. It cannot be moved in memory, so it needs the 39-bit
Autorun forwarder and the Atmosphere low-address patch.

Need for Speed Underground 2 and Most Wanted: neither executable can be moved in
memory, so start them through a forwarder set to a 32-bit address space.

Fallout New Vegas (GOG): its executable relocates, so it needs no forwarder.

Halo: Combat Evolved: delete the ._ files a Mac leaves beside every file on the
card if the game was copied from one: Halo loads every DLL in its Controls
folder and one of those is not a DLL.

Left 4 Dead 2: left4dead2.exe relocates, so it needs no forwarder either.
Steam's GameOverlayRenderer.dll not loading is expected and harmless.

Settings: switch/wine/config/settings.json holds every one of them, and
config/keys.txt the keys the controller sends. Both are set from the launcher
(Settings, Defaults, Controls, and a program's own Controls row), so neither
has to be written by hand.

Logs: switch/wine/logs/autorun_runtime.log holds the last run.
''')


# --- The AMD64 runtime -------------------------------------------------------

def merge_amd64(archive, root):
    keep = {
        'switch/wine/run-entry.txt',
        'switch/wine/target.txt',
        'switch/wine/vulkan-probe.txt',
    }
    required = {
        'switch/wine/build-manifest.json',
        'switch/wine/wine-nx-runtime.nro',
    }
    with ZipFile(archive) as z:
        assert z.testzip() is None, f'{archive} is damaged'
        names = set()
        for info in z.infolist():
            path = PurePosixPath(info.filename)
            assert path.parts[:2] == ('switch', 'wine') and '..' not in path.parts, info.filename
            assert not ((info.external_attr >> 16) & 0o170000) == 0o120000, info.filename
            folded = info.filename.rstrip('/').casefold()
            assert folded not in names, info.filename
            names.add(folded)
        assert {name.casefold() for name in required} <= names, f'{archive} is not a full AMD64 graphics package'
        manifest = json.loads(z.read('switch/wine/build-manifest.json'))
        features = manifest.get('features', {})
        for feature in ('amd64', 'dynarec', 'vulkan', 'dxvk', 'vkd3d', 'lsfg'):
            assert features.get(feature) is True, f'{archive} has no {feature} support'
        amd64_nro = z.read('switch/wine/wine-nx-runtime.nro')
        match = re.search(rb'nx-amd64-(?:box64-(\d+)|(fex-\d+))\0', amd64_nro)
        assert match, f'{archive} does not contain the AMD64 runtime'
        assert bool(match.group(2)) == bool(features.get('fex')), f'{archive} has inconsistent FEX support'
        for info in z.infolist():
            if info.filename.rstrip('/') in keep:
                continue
            destination = root.joinpath(*PurePosixPath(info.filename).parts)
            if info.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as source, destination.open('wb') as output:
                    shutil.copyfileobj(source, output)
    return (match.group(1) or match.group(2)).decode()


if args.x86:
    # The card runs Direct3D 9 through DXVK, so the x86 runtime is the one
    # linked with mesa-switch: NVK behind winevulkan. Without it a game on DXVK
    # dies with "Failed to create Vulkan instance".
    nro = horizon_wine / 'build-switch-wow64-mesa-switch/wine-nx-runtime.nro'
    assert nro.is_file(), f'{nro} is missing; build it with build-x86.sh'
    assert b'a Vulkan surface has the screen' in nro.read_bytes(), \
        f'{nro} has no Vulkan display driver; it is not the mesa-switch build'
    assert f'nx-wow64-dynarec-{marker}'.encode() + b'\0' in nro.read_bytes(), \
        f'{nro} is stale; rebuild the runtime for build {marker}'
    shutil.copy2(nro, stage / 'wine-nx-runtime.nro')
    # The build copies LSFG-VK's license out of the switch-dev image.
    shutil.copy2(nro.parent / 'licenses/LSFG-VK-GPL-3.0.txt', licenses / 'LSFG-VK-GPL-3.0.txt')
    shutil.copy2(nro.parent / 'licenses/FFmpeg-LGPL-2.1.txt', licenses / 'FFmpeg-LGPL-2.1.txt')
    print('x86 runtime staged')
else:
    assert args.amd64.is_file(), f'{args.amd64} is missing; run build-amd64-components.sh, or pass --x86'
    print(f'AMD64 runtime build {merge_amd64(args.amd64, stage_root)} merged')
# The tuned FEX modules a build fetched (build.sh, from the fex-opt release),
# staged last beside the DLL repository's own: the variant file that came with
# them turns them on. horizon-wine/fex/variants-build.md is why this exists.
if args.extra:
    assert args.extra.is_dir(), f'{args.extra} is missing'
    shutil.copytree(args.extra, stage, dirs_exist_ok=True)
    print(f'{args.extra} overlaid')

subprocess.run([sys.executable, str(tools / 'verify-package.py'), str(stage), '--dlls', str(args.dlls)],
               check=True)

archive = build / f'autorun-{marker}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    for f in sorted(stage.rglob('*')):
        if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
            z.write(f, f.relative_to(stage_root))
        # Empty folders are places to copy a game into, such as drive_c/WarCraft III.
        elif f.is_dir() and not any(f.iterdir()):
            z.write(f, f.relative_to(stage_root))
with ZipFile(archive) as z:
    assert z.testzip() is None
    files = len(z.infolist())
print(f'{archive} ({archive.stat().st_size / 2**20:.1f} MiB, {files} files)')
