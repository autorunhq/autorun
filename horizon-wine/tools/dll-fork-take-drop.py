#!/usr/bin/env python3
"""Put the opengl32.dll this repository's CI built into a fork of the DLL
repository, so that Windows DLLs > Source > fork (source/launcher_dlls.c)
brings it to a card.

The `build` workflow leaves the 32-bit opengl32.dll of the commit it built
under refs/dll-drop/opengl32-i386 of this repository, with the commit and the
runtime features it was built against. Run this in a checkout of the fork
(berlint-hub/autorun-horizon-dlls), then commit and push to its main branch:

    python3 dll-fork-take-drop.py [--autorun berlint-hub/autorun] [--fork berlint-hub/autorun-horizon-dlls]

It replaces switch/wine/drive_c/windows/syswow64/opengl32.dll and its
compressed copy, and rewrites that one entry of the manifest: a version one
higher, the new size and SHA-256, URLs in the fork, and the source commit.
Everything else in the manifest stays as it is, the other files' URLs too; the
launcher takes files from either repository.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
import zlib

FOLDER, NAME, MACHINE = 'drive_c/windows/syswow64', 'opengl32.dll', 0x014c
REF = 'refs/dll-drop/opengl32-i386'

parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
parser.add_argument('--autorun', default='berlint-hub/autorun', help='the repository whose CI built the DLL')
parser.add_argument('--fork', default='berlint-hub/autorun-horizon-dlls', help='the DLL repository this checkout is of')
parser.add_argument('--ref', default='main', help='the branch of the fork that publishes')
parser.add_argument('--repo', type=Path, default=Path.cwd(), help='the checkout of the fork')
args = parser.parse_args()

repo = args.repo
manifest_path = repo / 'switch/wine/horizon-dlls/manifest.json'
assert manifest_path.is_file(), f'{repo} is not a checkout of the DLL repository'

with tempfile.TemporaryDirectory() as temp:
    subprocess.run(['git', 'init', '-q', temp], check=True)
    subprocess.run(['git', '-C', temp, 'fetch', '-q', f'https://github.com/{args.autorun}', REF], check=True)
    subprocess.run(['git', '-C', temp, 'checkout', '-q', 'FETCH_HEAD'], check=True)
    data = (Path(temp) / NAME).read_bytes()
    commit = (Path(temp) / 'commit.txt').read_text().strip()
    features = set((Path(temp) / 'features.txt').read_text().split())

pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[:2] == b'MZ' and data[pe:pe + 4] == b'PE\0\0', 'not a PE file'
assert struct.unpack_from('<H', data, pe + 4)[0] == MACHINE, 'not a 32-bit x86 module'

manifest = json.loads(manifest_path.read_text())
entries = [f for f in manifest['files'] if f['name'] == NAME and f['path'] == FOLDER]
assert len(entries) == 1, f'{FOLDER}/{NAME} is not in the manifest once'
entry = entries[0]
# The runtime the DLL was built beside must be one the entry's requirements name.
missing = set(entry['requires']['features']) - features
assert not missing, f'the DLL was built against another runtime interface: {sorted(missing)}'

digest = hashlib.sha256(data).hexdigest()
if digest == entry['sha256']:
    print(f'{FOLDER}/{NAME} is already that file ({digest[:16]})')
    raise SystemExit(0)

raw = f'https://raw.githubusercontent.com/{args.fork}/{args.ref}/'
packed = zlib.compress(data, 9)
target = repo / 'switch/wine' / FOLDER / NAME
packed_target = repo / 'compressed/switch/wine' / FOLDER / f'{NAME}.z'
target.parent.mkdir(parents=True, exist_ok=True)
packed_target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(data)
packed_target.write_bytes(packed)

entry['version'] += 1
entry['size'] = len(data)
entry['sha256'] = digest
entry['url'] = f'{raw}switch/wine/{FOLDER}/{NAME}'
entry['source']['repo'] = args.autorun
entry['source']['commit'] = commit
entry['compressed'] = dict(encoding='zlib', size=len(packed), sha256=hashlib.sha256(packed).hexdigest(),
                           url=f'{raw}compressed/switch/wine/{FOLDER}/{NAME}.z')
manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
print(f'{FOLDER}/{NAME} v{entry["version"]}: {len(data)} bytes, {digest[:16]}, of {args.autorun} {commit[:8]}')
print('commit switch/, compressed/ and the manifest, and push to', args.ref)
