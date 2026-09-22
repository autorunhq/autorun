#!/usr/bin/env python3
"""Build one SD-card archive with the x86 and AMD64 graphics runtimes."""
from pathlib import Path
from pathlib import PurePosixPath
from zipfile import ZipFile, ZIP_DEFLATED
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
tools = probe / 'tools'
build = probe / 'build-switch-wow64-dynarec'
stage_root = build / 'full-sd-card'
stage = stage_root / 'switch/wine'
default_amd64 = probe / 'build-switch-amd64/wine-nx-amd64-box64-mesa-dxvk-vkd3d.zip'
parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--amd64', type=Path,
                    default=Path(os.environ.get('WINE_NX_AMD64_PACKAGE', default_amd64)))
args = parser.parse_args()


def merge_amd64(archive, root):
    keep = {
        'switch/wine/run-entry.txt',
        'switch/wine/target.txt',
        'switch/wine/vulkan-probe.txt',
    }
    required = {
        'switch/wine/build-manifest.json',
        'switch/wine/wine-nx-runtime.nro',
        'switch/wine/drive_c/windows/system32/winebox64ec.dll',
        'switch/wine/drive_c/dxvk64/dxgi.dll',
        'switch/wine/drive_c/vkd3d64/d3d12.dll',
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
        if features.get('fex'):
            for dll in ('libarm64ecfex.dll', 'libwow64fex.dll'):
                assert f'switch/wine/drive_c/windows/system32/{dll}' in names, \
                    f'{archive} has no FEX CPU module: {dll}'
        nro = z.read('switch/wine/wine-nx-runtime.nro')
        match = re.search(rb'nx-amd64-(?:box64-(\d+)|(fex-\d+))\0', nro)
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

subprocess.run([sys.executable, str(tools / 'package-wow64-full.py')], check=True)
subprocess.run([sys.executable, str(tools / 'package-wow64-dxvk.py')], check=True)

wow64_marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)
full = build / f'wine-nx-full-dynarec-{wow64_marker}.zip'
overlay = build / f'wine-nx-dxvk-overlay-dynarec-{wow64_marker}.zip'
assert full.is_file() and overlay.is_file(), 'a half is missing'

# The overlay's paths are the card's own, so it unpacks onto the staged payload
# the way it would onto the card: a newer runtime and the DXVK files.
with ZipFile(overlay) as z:
    for name in z.namelist():
        assert name.startswith('switch/wine/'), name
    z.extractall(stage_root)

assert args.amd64.is_file(), f'{args.amd64} is missing; build the AMD64 DXVK/VKD3D package first'
marker = merge_amd64(args.amd64, stage_root)
subprocess.run([sys.executable, str(tools / 'verify-wow64-package.py'), str(stage)], check=True)

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

full.unlink()
overlay.unlink()
print(f'{archive} ({archive.stat().st_size / 2**20:.1f} MiB, {files} files)')
