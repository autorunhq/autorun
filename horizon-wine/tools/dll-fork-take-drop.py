#!/usr/bin/env python3
"""Put the Wine DLLs this repository's CI built into a fork of the DLL
repository, so that Windows DLLs > Source > fork (source/launcher_dlls.c)
brings them to a card.

The `build` workflow builds, from the commit it built, the modules of Wine's
own whose source here differs from what the DLL repository ships, and leaves
them under refs/dll-drop/wine-dlls of this repository, with the commit and the
runtime features it was built against:

    system32 (ARM64X)  ntdll.dll wow64.dll dinput.dll dinput8.dll opengl32.dll
    syswow64 (i386)    ntdll.dll dinput.dll dinput8.dll opengl32.dll

opengl32's object and context tables without their locks, and Carafe's four
changes this source has had all along (carafe/integration/PORTING.md, stage
5): wow64.dll suspends a 32-bit thread through FEX, ntdll.dll takes an ended
thread's address wait out, DirectInput reads the pad through the HID gamepad.

Run this in a checkout of the fork (berlint-hub/autorun-horizon-dlls), then
commit and push to its main branch:

    python3 dll-fork-take-drop.py [--only ntdll,dinput] [--autorun berlint-hub/autorun]
                                  [--fork berlint-hub/autorun-horizon-dlls] [--drop DIR]

For each DLL it replaces the file and its compressed copy, and rewrites that
one entry of the manifest: a version one higher, the new size and SHA-256, URLs
in the fork, and the source commit. Everything else in the manifest stays as it
is, the other files' URLs too; the launcher takes files from either
repository. A DLL that is not a module for its folder, that the manifest does
not hold once, or whose entry needs a runtime feature the build was not made
against is refused, and then nothing at all is written.

To go back: Windows DLLs > Source > Autorun, then Update, on the console.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import zlib

FOLDERS = {'syswow64': 0x014c, 'system32': 0xaa64}   # i386; ARM64X is an ARM64 image
NAMES = ('ntdll.dll', 'wow64.dll', 'dinput.dll', 'dinput8.dll', 'opengl32.dll')
REF = 'refs/dll-drop/wine-dlls'


def pe_machine(data):
    if len(data) < 0x40 or data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe + 6 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        return None
    return struct.unpack_from('<H', data, pe + 4)[0]


def read_drop(drop):
    """{(folder, name): bytes}, the commit and the features of a drop folder."""
    files = {}
    for folder in FOLDERS:
        for name in NAMES:
            path = drop / folder / name
            if path.is_file():
                files[(folder, name)] = path.read_bytes()
    # A drop from before there was more than one file: opengl32.dll for syswow64 at the top.
    if not files and (drop / 'opengl32.dll').is_file():
        files[('syswow64', 'opengl32.dll')] = (drop / 'opengl32.dll').read_bytes()
    commit = (drop / 'commit.txt').read_text().strip()
    features = set((drop / 'features.txt').read_text().split())
    return files, commit, features


def take(repo, files, commit, features, autorun, fork, ref='main', only=None):
    """Writes the files into the fork checkout at repo; returns the lines it
    reports. Raises SystemExit, having written nothing, on any refusal."""
    manifest_path = repo / 'switch/wine/horizon-dlls/manifest.json'
    if not manifest_path.is_file():
        raise SystemExit(f'{repo} is not a checkout of the DLL repository')
    manifest = json.loads(manifest_path.read_text())
    raw = f'https://raw.githubusercontent.com/{fork}/{ref}/'
    wanted = {name if name.endswith('.dll') else name + '.dll' for name in only} if only else None

    plan, report, problems = [], [], []
    for (folder, name), data in sorted(files.items()):
        if wanted is not None and name not in wanted:
            continue
        path = f'drive_c/windows/{folder}'
        if pe_machine(data) != FOLDERS[folder]:
            problems.append(f'{folder}/{name} is not {"a 32-bit x86" if folder == "syswow64" else "an ARM64"} module')
            continue
        entries = [f for f in manifest['files'] if f['name'] == name and f['path'] == path]
        if len(entries) != 1:
            problems.append(f'{path}/{name} is not in the manifest once')
            continue
        entry = entries[0]
        missing = set(entry.get('requires', {}).get('features', [])) - features
        if missing:
            problems.append(f'{folder}/{name} was built against another runtime interface: {sorted(missing)}')
            continue
        digest = hashlib.sha256(data).hexdigest()
        if digest == entry['sha256']:
            report.append(f'{folder}/{name} is already that file ({digest[:16]})')
            continue
        plan.append((folder, name, path, data, digest, entry))
    if wanted is not None:
        unknown = wanted - {name for _, name in files}
        if unknown:
            problems.append(f'not in the drop: {", ".join(sorted(unknown))}')
    if problems:
        raise SystemExit('nothing written:\n  ' + '\n  '.join(problems))

    for folder, name, path, data, digest, entry in plan:
        packed = zlib.compress(data, 9)
        target = repo / 'switch/wine' / path / name
        packed_target = repo / 'compressed/switch/wine' / path / f'{name}.z'
        target.parent.mkdir(parents=True, exist_ok=True)
        packed_target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        packed_target.write_bytes(packed)
        entry['version'] = entry.get('version', 0) + 1
        entry['size'] = len(data)
        entry['sha256'] = digest
        entry['url'] = f'{raw}switch/wine/{path}/{name}'
        entry.setdefault('source', {})
        entry['source']['repo'] = autorun
        entry['source']['commit'] = commit
        entry['compressed'] = dict(encoding='zlib', size=len(packed), sha256=hashlib.sha256(packed).hexdigest(),
                                   url=f'{raw}compressed/switch/wine/{path}/{name}.z')
        report.append(f'{folder}/{name} v{entry["version"]}: {len(data)} bytes, {digest[:16]}, of {autorun} {commit[:8]}')
    if plan:
        manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
        report.append(f'{len(plan)} replaced: commit switch/, compressed/ and the manifest, and push to {ref}')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--autorun', default='berlint-hub/autorun', help='the repository whose CI built the DLLs')
    parser.add_argument('--fork', default='berlint-hub/autorun-horizon-dlls', help='the DLL repository this checkout is of')
    parser.add_argument('--ref', default='main', help='the branch of the fork that publishes')
    parser.add_argument('--repo', type=Path, default=Path.cwd(), help='the checkout of the fork')
    parser.add_argument('--drop', type=Path, help='a drop folder already here, instead of fetching ' + REF)
    parser.add_argument('--drop-ref', default=REF, help='the ref of the drop in the --autorun repository')
    parser.add_argument('--only', help='comma-separated DLL names to take, e.g. ntdll,dinput')
    args = parser.parse_args()
    only = [name.strip().lower() for name in args.only.split(',') if name.strip()] if args.only else None

    if args.drop:
        files, commit, features = read_drop(args.drop)
    else:
        with tempfile.TemporaryDirectory() as temp:
            subprocess.run(['git', 'init', '-q', temp], check=True)
            subprocess.run(['git', '-C', temp, 'fetch', '-q', f'https://github.com/{args.autorun}', args.drop_ref], check=True)
            subprocess.run(['git', '-C', temp, 'checkout', '-q', 'FETCH_HEAD'], check=True)
            files, commit, features = read_drop(Path(temp))
    if not files:
        raise SystemExit('the drop holds no DLL')
    for line in take(args.repo, files, commit, features, args.autorun, args.fork, args.ref, only):
        print(line)


if __name__ == '__main__':
    sys.exit(main())
