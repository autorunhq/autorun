#!/usr/bin/env python3
"""Stage the dual-architecture runtime: the AMD64 NRO, Wine's fonts and NLS
data, and the licenses of what the NRO is built from.

The Windows modules it runs -- the ARM64X system32 and i386 syswow64, FEX's CPU
modules and the bundled DXVK and VKD3D-Proton -- are the DLL repository's
(horizon-dlls/tools/build-dlls.py), not this package's. --fex, --dxvk and
--vkd3d say that the NRO was built to run them, which the build manifest
reports as features."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from wine_mono_payload import stage_wine_mono
horizon_wine = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--build', type=Path, default=horizon_wine / 'build-switch-amd64')
parser.add_argument('--output', type=Path, help='Archive output path (defaults to the build directory)')
parser.add_argument('--stage-output', type=Path, help='Stage directly into a new directory without creating an archive')
parser.add_argument('--wine-source', type=Path, default=horizon_wine.parent, help='Matching Wine source tree')
parser.add_argument('--vulkan', action='store_true', help='The NRO is linked with mesa-switch')
parser.add_argument('--dxvk', action='store_true', help='The NRO runs the bundled DXVK (requires --vulkan)')
parser.add_argument('--vkd3d', action='store_true', help='The NRO runs the bundled VKD3D-Proton (requires --dxvk)')
parser.add_argument('--fex', action='store_true', help='The NRO runs the FEX CPU modules')
parser.add_argument('--wine-mono', type=Path, help='Stage Win32 and Win64 Mono from wine-mono-11.3.0-arm64.tar.xz')
args = parser.parse_args()
wine_source = args.wine_source.resolve()
if args.output and args.stage_output:
    parser.error('--output and --stage-output are mutually exclusive')
if args.dxvk and not args.vulkan:
    parser.error('--dxvk requires --vulkan')
if args.vkd3d and not args.dxvk:
    parser.error('--vkd3d requires --dxvk for DXGI')
build = args.build.resolve()
cache_path = build / 'CMakeCache.txt'
if not cache_path.is_file():
    parser.error('Missing Switch CMake build configuration')
cache = dict(re.findall(r'^([^#/:\n][^:\n]*):[^=\n]+=(.*)$', cache_path.read_text(), re.M))
enabled = lambda name: cache.get(name, '').upper() in ('ON', 'TRUE', 'YES', '1')
if not enabled('WINE_NX_AMD64'):
    parser.error('The NRO must be built with WINE_NX_AMD64=ON')
if args.fex != enabled('WINE_NX_FEX'):
    parser.error('--fex must match the NRO FEX build configuration')
if args.vulkan != bool(cache.get('WINE_NX_MESA_SWITCH_DIR')):
    parser.error('--vulkan must match the NRO mesa-switch build configuration')
nro = build / 'wine-nx-runtime.nro'
if not nro.is_file() or nro.read_bytes()[16:20] != b'NRO0':
    parser.error('Missing or invalid wine-nx-runtime.nro')
if args.fex and b'FEX-2609' not in nro.read_bytes():
    parser.error('The NRO has no FEX launch support; rebuild it first')
if args.vkd3d and b'[VKD3D] payload' not in nro.read_bytes():
    parser.error('The NRO has no VKD3D launch support; rebuild it first')
# What the switch-dev image the NRO was built in holds (CMake copies it).
switch_dev_path = build / 'switch-dev.json'
if not switch_dev_path.is_file():
    parser.error('The NRO was not built in the switch-dev image; no switch-dev.json in the build')
switch_dev = json.loads(switch_dev_path.read_text())
lsfg_revision = None
if enabled('WINE_NX_LSFG') and args.vulkan:
    lsfg_revision = switch_dev['lsfg_vk']
    if b'[LSFG]' not in nro.read_bytes():
        parser.error('The NRO has no LSFG-VK support; rebuild it first')
mesa_revision = None
if args.vulkan:
    if b'a Vulkan surface has the screen' not in nro.read_bytes():
        parser.error('The NRO has no mesa-switch Vulkan display driver')
    mesa_revision = switch_dev['mesa_switch']
if args.stage_output:
    stage_root = args.stage_output.resolve()
    stage_root.mkdir(parents=True, exist_ok=False)
    staging = None
else:
    staging = tempfile.TemporaryDirectory(prefix='amd64-package-')
    stage_root = Path(staging.name)
stage = stage_root / 'switch/wine'
source_hashes = {}


def stage_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    source_hashes[destination.relative_to(stage).as_posix()] = hashlib.sha256(source.read_bytes()).hexdigest()


drive = stage / 'drive_c'
mono_manifest = None
if args.wine_mono:
    mono_manifest = stage_wine_mono(args.wine_mono, drive / 'windows/mono/mono-2.0')
for name in ('fonts', 'nls'):
    extension = '*.ttf' if name == 'fonts' else '*.nls'
    resources = list((wine_source / name).glob(extension))
    if not resources:
        raise ValueError(f'Missing Wine {name} resources')
    for path in resources:
        stage_file(path, stage / 'share/wine' / name / path.name)
        if name == 'fonts':
            stage_file(path, drive / 'windows/fonts' / path.name)
stage_file(nro, stage / nro.name)
licenses = stage / 'licenses'
stage_file(build / 'licenses/FFmpeg-LGPL-2.1.txt', licenses / 'FFmpeg-LGPL-2.1.txt')
if mono_manifest:
    stage_file(horizon_wine / 'licenses/WineMono.txt', licenses / 'WineMono.txt')
if lsfg_revision:
    stage_file(build / 'licenses/LSFG-VK-GPL-3.0.txt', licenses / 'LSFG-VK-GPL-3.0.txt')
    (stage / 'lsfg').mkdir()
for source, name in ((wine_source / 'COPYING.LIB', 'Wine-LGPL-2.1.txt'),
                     (horizon_wine / 'vendor/box64/LICENSE', 'Box64-MIT.txt'),
                     (horizon_wine / 'licenses/libjpeg-turbo.txt', 'libjpeg-turbo.txt')):
    stage_file(source, licenses / name)

files = sorted(path for path in stage.rglob('*') if path.is_file() and
               path.suffix != '.log' and path.name != 'build-manifest.json')
manifest = {
    'box64': '2f130fab1d6e1a4ee8a71dc60cfdfcc839ad192a',
    'wine': subprocess.check_output(['git', '-C', str(wine_source), 'rev-parse', 'HEAD'], text=True).strip(),
    'features': {'amd64': True, 'dynarec': enabled('WINE_NX_BOX64_DYNAREC'),
                 'vulkan': args.vulkan, 'dxvk': args.dxvk, 'vkd3d': args.vkd3d,
                 'lsfg': bool(lsfg_revision), 'fex': args.fex},
    'mesa_switch': mesa_revision,
    'wine_mono': mono_manifest,
    'lsfg': {'repository': 'https://git.lsfg-vk.dev/lsfg-vk-archive.git',
             'revision': lsfg_revision,
             'port': 'https://github.com/autorunhq/switch-dev'}
            if lsfg_revision else None,
    'switch_dev': switch_dev,
    'files': {path.relative_to(stage).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
}
for name, digest in source_hashes.items():
    if manifest['files'][name] != digest:
        raise ValueError(f'Staged file differs from its build output: {name}')
(stage / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
if args.stage_output:
    print(stage)
    sys.exit(0)
archive = build / ('wine-nx-amd64-box64-mesa-dxvk-vkd3d.zip' if args.vkd3d else
                   'wine-nx-amd64-box64-mesa-dxvk.zip' if args.dxvk else
                   'wine-nx-amd64-box64-mesa-vulkan.zip' if args.vulkan else 'wine-nx-amd64-box64.zip')
if args.fex:
    archive = archive.with_name(archive.name.replace('-box64', '-box64-fex'))
if args.output:
    archive = args.output.resolve()
archive.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(archive, 'w', ZIP_DEFLATED) as output:
    for path in sorted(stage.rglob('*')):
        if path.is_file() and path.suffix != '.log':
            output.write(path, path.relative_to(stage_root))
with ZipFile(archive) as output:
    expected = {**manifest['files'], 'build-manifest.json':
                hashlib.sha256((stage / 'build-manifest.json').read_bytes()).hexdigest()}
    for name, digest in expected.items():
        if hashlib.sha256(output.read('switch/wine/' + name)).hexdigest() != digest:
            raise ValueError(f'Archive integrity check failed: {name}')
print(archive)
staging.cleanup()
