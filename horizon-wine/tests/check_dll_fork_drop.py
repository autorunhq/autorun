#!/usr/bin/env python3
"""tools/dll-fork-take-drop.py: the Wine DLLs CI builds, into a fork of the DLL
repository.

A drop as the `build` workflow leaves it (the DLLs by folder, features.txt,
commit.txt) and a checkout of the fork with a manifest whose entries are shaped
as the DLL repository's are: every DLL replaced, its version raised, its size,
SHA-256 and compressed copy written, its URLs in the fork; a DLL already there
left alone; --only; and a DLL for the wrong machine, missing from the
manifest, or built against another runtime interface refused, with nothing at
all written."""
import hashlib
import importlib.util
import json
import pathlib
import struct
import tempfile
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('take_drop', root / 'horizon-wine/tools/dll-fork-take-drop.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

MODULES = [('system32', n) for n in ('ntdll.dll', 'wow64.dll', 'dinput.dll', 'dinput8.dll', 'opengl32.dll')] + \
          [('syswow64', n) for n in ('ntdll.dll', 'dinput.dll', 'dinput8.dll', 'opengl32.dll')]
# What the DLL repository's manifest requires of each (horizon-dlls @ 67e2303).
REQUIRES = {
    ('system32', 'ntdll.dll'): ['iface:ntdll.dll:882b3e3a314b'],
    ('system32', 'dinput.dll'): ['unixlib:dinput.dll'],
    ('system32', 'dinput8.dll'): ['unixlib:dinput8.dll'],
    ('system32', 'opengl32.dll'): ['unixlib:opengl32.dll', 'iface:opengl32.dll:8ccacd56f03e'],
    ('syswow64', 'dinput.dll'): ['unixlib32:dinput.dll'],
    ('syswow64', 'dinput8.dll'): ['unixlib32:dinput8.dll'],
    ('syswow64', 'opengl32.dll'): ['unixlib32:opengl32.dll', 'iface:opengl32.dll:8ccacd56f03e'],
}
FEATURES = sorted({f for r in REQUIRES.values() for f in r} | {'unixlib:libwow64fex.dll'})


def pe(machine, tag):
    data = bytearray(0x200)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x40)
    data[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<H', data, 0x44, machine)
    data[0x100:0x100 + len(tag)] = tag.encode()
    return bytes(data)


def make_fork(path):
    files = []
    for folder, name in MODULES + [('system32', 'kernel32.dll')]:
        old = pe(tool.FOLDERS[folder], f'old {folder} {name}')
        files.append(dict(name=name, path=f'drive_c/windows/{folder}', version=3, size=len(old),
                          sha256=hashlib.sha256(old).hexdigest(),
                          url=f'https://raw.githubusercontent.com/autorunhq/autorun-horizon-dlls/main/switch/wine/'
                              f'drive_c/windows/{folder}/{name}',
                          requires=dict(flavor='arm64x', features=REQUIRES.get((folder, name), [])),
                          source=dict(repo='autorunhq/autorun', commit='fb9e3918', origin='wine')))
    manifest = path / 'switch/wine/horizon-dlls/manifest.json'
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps(dict(schema=2, flavor='arm64x', files=files), indent=1) + '\n')
    return manifest


def make_drop(path, machines=None):
    for folder, name in MODULES:
        (path / folder).mkdir(parents=True, exist_ok=True)
        machine = (machines or {}).get((folder, name), tool.FOLDERS[folder])
        (path / folder / name).write_bytes(pe(machine, f'new {folder} {name}'))
    (path / 'features.txt').write_text('\n'.join(FEATURES) + '\n')
    (path / 'commit.txt').write_text('9f5573c480b71340c6dbae81e1cc0fdcab713e7b\n')


def snapshot(path):
    return {p.relative_to(path): p.read_bytes() for p in sorted(path.rglob('*')) if p.is_file()}


with tempfile.TemporaryDirectory() as directory:
    base = pathlib.Path(directory)

    # All nine, as the workflow leaves them.
    fork, drop = base / 'fork', base / 'drop'
    manifest = make_fork(fork)
    make_drop(drop)
    files, commit, features = tool.read_drop(drop)
    assert len(files) == 9 and commit.startswith('9f5573c') and 'iface:ntdll.dll:882b3e3a314b' in features
    report = tool.take(fork, files, commit, features, 'berlint-hub/autorun', 'berlint-hub/autorun-horizon-dlls')
    assert report[-1].startswith('9 replaced'), report
    entries = {(e['path'].split('/')[-1], e['name']): e for e in json.loads(manifest.read_text())['files']}
    for folder, name in MODULES:
        data = (drop / folder / name).read_bytes()
        target = fork / 'switch/wine/drive_c/windows' / folder / name
        packed = fork / 'compressed/switch/wine/drive_c/windows' / folder / f'{name}.z'
        entry = entries[(folder, name)]
        assert target.read_bytes() == data and zlib.decompress(packed.read_bytes()) == data
        assert entry['version'] == 4 and entry['size'] == len(data)
        assert entry['sha256'] == hashlib.sha256(data).hexdigest()
        assert entry['url'] == f'https://raw.githubusercontent.com/berlint-hub/autorun-horizon-dlls/main/switch/wine/' \
                               f'drive_c/windows/{folder}/{name}'
        assert entry['compressed']['sha256'] == hashlib.sha256(packed.read_bytes()).hexdigest()
        assert entry['compressed']['url'].endswith(f'compressed/switch/wine/drive_c/windows/{folder}/{name}.z')
        assert entry['source'] == dict(repo='berlint-hub/autorun', commit=commit, origin='wine')
        assert entry['requires']['features'] == REQUIRES.get((folder, name), [])  # unchanged
    # A file the drop does not hold stays as it was, URL and all.
    assert entries[('system32', 'kernel32.dll')]['version'] == 3
    assert 'autorunhq' in entries[('system32', 'kernel32.dll')]['url']

    # Taken again: every one is already that file, nothing written.
    before = snapshot(fork)
    report = tool.take(fork, files, commit, features, 'berlint-hub/autorun', 'berlint-hub/autorun-horizon-dlls')
    assert all('already that file' in line for line in report) and len(report) == 9 and snapshot(fork) == before

    # --only: the two DirectInput DLLs, in both folders.
    fork2 = base / 'fork2'
    make_fork(fork2)
    report = tool.take(fork2, files, commit, features, 'berlint-hub/autorun', 'berlint-hub/autorun-horizon-dlls',
                       only=['dinput', 'dinput8.dll'])
    assert report[-1].startswith('4 replaced'), report
    entries = {(e['path'].split('/')[-1], e['name']): e for e in
               json.loads((fork2 / 'switch/wine/horizon-dlls/manifest.json').read_text())['files']}
    assert entries[('syswow64', 'dinput.dll')]['version'] == 4 and entries[('system32', 'ntdll.dll')]['version'] == 3

    # Refused, with nothing written: a wrong machine, another runtime
    # interface, a DLL the manifest does not hold, a name not in the drop.
    def refused(files, features, only=None, words=''):
        fork = base / f'refused{refused.count}'
        refused.count += 1
        make_fork(fork)
        before = snapshot(fork)
        try:
            tool.take(fork, files, commit, features, 'berlint-hub/autorun', 'berlint-hub/autorun-horizon-dlls', only=only)
        except SystemExit as error:
            assert words in str(error), (words, str(error))
        else:
            raise AssertionError('not refused: ' + words)
        assert snapshot(fork) == before
    refused.count = 0
    bad = base / 'bad'
    make_drop(bad, {('syswow64', 'ntdll.dll'): 0xaa64})
    refused(tool.read_drop(bad)[0], features, words='syswow64/ntdll.dll is not a 32-bit x86 module')
    bad64 = dict(files)
    bad64[('system32', 'wow64.dll')] = pe(0x014c, 'i386 in system32')
    refused(bad64, features, words='system32/wow64.dll is not an ARM64 module')
    refused(files, features - {'iface:ntdll.dll:882b3e3a314b'}, words='another runtime interface')
    extra = dict(files)
    extra[('system32', 'wow64.dll')] = files[('system32', 'wow64.dll')]
    extra[('syswow64', 'wow64.dll')] = pe(0x014c, 'no such entry')
    refused(extra, features, words='drive_c/windows/syswow64/wow64.dll is not in the manifest once')
    refused(files, features, only=['xinput1_3'], words='not in the drop: xinput1_3.dll')
    assert tool.pe_machine(b'not a pe') is None

    # A drop from before: opengl32.dll alone at the top, for syswow64.
    old = base / 'old-drop'
    old.mkdir()
    (old / 'opengl32.dll').write_bytes(pe(0x014c, 'old drop'))
    (old / 'features.txt').write_text('unixlib32:opengl32.dll\niface:opengl32.dll:8ccacd56f03e\n')
    (old / 'commit.txt').write_text('8924c90\n')
    assert list(tool.read_drop(old)[0]) == [('syswow64', 'opengl32.dll')]

# The workflow builds the same nine and lays them out as the tool reads them.
workflow = (root / '.github/workflows/build.yml').read_text()
step = workflow[workflow.index('- name: Wine DLLs of this source'):workflow.index('- name: Upload those Wine DLLs')]
for folder, name in MODULES:
    arch = 'aarch64' if folder == 'system32' else 'i386'
    assert f'{folder}:{name[:-4]}:{arch}:{name}' in step, (folder, name)
assert 'refs/dll-drop/$name' in step and 'for name in wine-dlls opengl32-i386' in step
assert 'runtime_features.py > "$drop/features.txt"' in step and 'continue-on-error: true' in step
assert tool.REF == 'refs/dll-drop/wine-dlls'
print('DLL fork drop: nine Wine DLLs taken with versions, hashes, compressed copies and fork URLs; '
      'identical, --only, wrong machine, other interface, unknown entry and old drops handled')
