#!/usr/bin/env python3
"""Build a release of autorun-horizon-dlls: the Windows components Autorun
downloads instead of shipping, each built from this tree's Wine.

    package-horizon-dlls.py [--previous manifest.json] [--allow-dirty]

--previous is the manifest of the release before this one (manifest.json in a
checkout of the pack repo). A file whose bytes did not change keeps its version
and the URL it was released under, so a release uploads only what changed and a
card downloads only that; a changed file gets the next version. Without it this
is the first release.

What it writes, in build-horizon-dlls/:
  repo/     manifest.json, README.md, NOTICE.md and LICENSES/, for the repo
  assets/   the files new in this release, to attach to its GitHub release
  autorun-horizon-dlls-pack-N.zip
            the whole pack laid out as on a card, for players without a
            network: extracted to the SD card root it is what a download leaves

Every file names the commit it was built from, and whether its sources changed
since Wine 11.0 was imported, which is what the LGPL asks of a changed library.
Sources with changes that are not committed stop the build, since the source
the manifest points to would not be what the file was built from; --allow-dirty
is for trying the packager, and marks the commit so.
"""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import argparse
import functools
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
root = probe.parent
tools = probe / 'tools'
pe = probe / 'build-wine-wow64-pe'
toolchain = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
out = probe / 'build-horizon-dlls'
# The card a release of Autorun leaves; what the pack's DLLs import has to be on
# it or in the pack.
release_stage = probe / 'build-switch-wow64-dynarec/full-sd-card/switch/wine'

PACK_REPO = 'autorunhq/autorun-horizon-dlls'
SOURCE_REPO = 'autorunhq/autorun'
DOWNLOAD = f'https://github.com/{PACK_REPO}/releases/download'
# The commit that imported Wine 11.0; a file whose sources a later commit
# touched is a changed copy of Wine's.
WINE_IMPORT = 'eaa5b16e'
SCHEMA = 1
# What the runtime says it is; a file lists the range it works with. Raise it
# when a runtime change breaks what files already released expect, and give a
# file that needs something new from the runtime a feature name, which the
# runtime lists once it has it.
RUNTIME_ABI = 1
SYSWOW64 = 'drive_c/windows/syswow64'

def group(name, dlls, **requires):
    return [dict(group=name, name=dll if dll.endswith(('.dll', '.drv', '.acm')) else dll + '.dll',
                 path=SYSWOW64, **requires) for dll in dlls]

# The DirectX redistributable a game's installer would have run on a PC: every
# version Wine builds. Those created by class rather than imported (XAudio2
# before 2.8, XACT, DirectMusic, DirectPlay) carry their classes. A file that
# will not work without something new in the runtime says so here, with
# runtime_abi=(min, max) or features=[...]. What they load that a release's
# card lacks comes along as the group 'depends'.
COMPONENTS = (
    group('d3dx9', [f'd3dx9_{n}' for n in range(24, 44)])
    # d3dx9_42 and _43 import their own compiler; 47 comes with the runtime.
    + group('d3dcompiler', [f'd3dcompiler_{n}' for n in (*range(33, 44), 46)])
    # xinput1_1 and 1_2 build xinput1_3's Switch pad code, whose call table the
    # runtime finds by module name (dlls/ntdll/unix/virtual.c); one without
    # their names would give them no pad.
    + group('xinput', ['xinput1_1', 'xinput1_2'], features=['xinput1_1-switch-pad'])
    + group('xinput', ['xinput9_1_0'])
    + group('xaudio2', [f'xaudio2_{n}' for n in range(10)])
    + group('x3daudio', [f'x3daudio1_{n}' for n in range(8)])
    + group('xapofx', [f'xapofx1_{n}' for n in range(1, 6)])
    + group('xact', [f'xactengine2_{n}' for n in (0, 4, 7, 9)] + [f'xactengine3_{n}' for n in range(8)])
    + group('directmusic', 'dmusic dmime dmloader dmstyle dmsynth dmband dmcompos dmscript dswave dsdmo dmusic32'.split())
    + group('directplay', 'dplay dplayx dpnet dpwsockx dpnhpast dpvoice'.split())
)

# The licenses of what the files are built from: Wine, and the libraries some
# of them link in (by the $(NAME_PE_LIBS) their Makefile.in imports).
LICENSES = {
    'wine': ('LGPL-2.1-or-later', root / 'COPYING.LIB', 'LICENSES/Wine-LGPL-2.1.txt'),
    'faudio': ('Zlib', root / 'libs/faudio/LICENSE', 'LICENSES/FAudio-Zlib.txt'),
    'fluidsynth': ('LGPL-2.1-or-later', root / 'COPYING.LIB', 'LICENSES/FluidSynth-LGPL-2.1.txt'),
}

env = dict(os.environ, PATH=f'{toolchain}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])

def git(*args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

def readobj(option, path):
    return subprocess.check_output([str(toolchain / 'llvm-readobj'), option, str(path)], text=True)

spec = importlib.util.spec_from_file_location('classes', tools / 'make-classes-reg.py')
classes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classes)

def module_dir(name):
    return name.rsplit('.', 1)[0]

def makefile(name):
    text = (root / 'dlls' / module_dir(name) / 'Makefile.in').read_text()
    return dict(re.findall(r'^(\w+)\s*=\s*(.*(?:\\\n.*)*)', text, re.M))

def libraries(name):
    """The bundled libraries a module links in, by name under libs/."""
    imports = makefile(name).get('IMPORTS', '')
    return sorted(lib.lower() for lib in re.findall(r'\$\((\w+)_PE_LIBS\)', imports)
                  if (root / 'libs' / lib.lower()).is_dir())

def sources(name):
    """The directories a module is built from: its own, the one it shares
    sources with, and the bundled libraries it links in."""
    paths = [f'dlls/{module_dir(name)}']
    parent = makefile(name).get('PARENTSRC')
    if parent:
        paths.append(os.path.normpath(f'dlls/{module_dir(name)}/{parent.strip()}'))
    return paths + [f'libs/{lib}' for lib in libraries(name)]

@functools.lru_cache(maxsize=None)
def built(name):
    target = f'dlls/{module_dir(name)}/i386-windows/{name}'
    subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    return pe / target

@functools.lru_cache(maxsize=None)
def forwards_of(path):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', path), re.M))

def needed(dll):
    """The modules a DLL loads at start: its imports, and the modules the
    exports it uses from them forward to."""
    found = set()
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', dll), re.M | re.S):
        module = re.search(r'Name: (.+)', block).group(1).lower()
        if module.startswith(('api-ms-', 'ext-ms-')):
            continue
        found.add(module)
        source = release_stage / SYSWOW64 / module
        if not source.exists():
            source = built(module)
        forwards = forwards_of(source)
        for symbol in set(re.findall(r'Symbol: (\S+) \(', block)) & forwards.keys():
            if not forwards[symbol].lower().startswith(('api-ms-', 'ext-ms-')):
                found.add(forwards[symbol].lower() + '.dll')
    return found

def with_dependencies(components, on_card):
    """The components, and what they load that a release's card does not
    have, which comes with them as the group 'depends'."""
    components = list(components)
    queue = [c['name'] for c in components]
    in_pack = {name.lower() for name in queue}
    while queue:
        for module in sorted(needed(built(queue.pop()))):
            if module in on_card or module in in_pack:
                continue
            assert (root / 'dlls' / module_dir(module)).is_dir(), f'nothing builds {module}'
            components += group('depends', [module])
            in_pack.add(module)
            queue.append(module)
    return components

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--previous', type=Path, help="the last release's manifest.json")
    parser.add_argument('--allow-dirty', action='store_true', help='build from uncommitted sources, for trying it')
    args = parser.parse_args()

    previous = json.loads(args.previous.read_text()) if args.previous else None
    if previous:
        assert previous['schema'] == SCHEMA, f"previous manifest is schema {previous['schema']}"
    pack = previous['pack'] + 1 if previous else 1
    tag = f'pack-{pack}'
    earlier = {f"{f['path']}/{f['name']}": f for f in previous['files']} if previous else {}

    commit = git('rev-parse', 'HEAD')
    on_card = {p.name.lower() for p in (release_stage / SYSWOW64).iterdir()}
    components = with_dependencies(COMPONENTS, on_card)
    names = [c['name'] for c in components]
    assert len(set(names)) == len(names), 'a file is listed twice'
    dirty = git('status', '--porcelain', '--', *sorted({p for n in names for p in sources(n)}))
    if dirty:
        assert args.allow_dirty, f'uncommitted changes in what the pack is built from:\n{dirty}'
        commit += '-dirty'
    elif not git('branch', '-r', '--contains', commit):
        print(f'warning: {commit[:8]} is not pushed; push it before publishing, the manifest points there')

    shutil.rmtree(out, ignore_errors=True)
    (out / 'repo/LICENSES').mkdir(parents=True)
    (out / 'assets').mkdir()
    card = out / 'card/switch/wine'

    files, licenses, claimed = [], {'wine'}, {}
    for component in components:
        name, path = component['name'], component['path']
        dll = built(name)
        assert 'Arch: i386\n' in readobj('--file-headers', dll), f'{name} is not i386'

        digest = sha256(dll)
        before = earlier.get(f'{path}/{name}')
        if before and before['sha256'] == digest:
            version, url = before['version'], before['url']
        else:
            version, url = (before['version'] + 1 if before else 1), f'{DOWNLOAD}/{tag}/{name}'
            shutil.copy2(dll, out / 'assets' / name)

        served = []
        for uuid, threading, coclass in classes.classes_of(module_dir(name)) + classes.registered_classes_of(dll):
            if uuid in claimed:
                continue
            claimed[uuid] = name
            served.append(dict(clsid=uuid, name=coclass, threading=threading))

        changes = git('log', '--format=%h', f'{WINE_IMPORT}..HEAD', '--', *sources(name)).split()
        libs = libraries(name)
        licenses.update(libs)
        files.append(dict(
            name=name, path=path, group=component['group'], version=version,
            size=dll.stat().st_size, sha256=digest, url=url,
            source=dict(repo=SOURCE_REPO, commit=commit, modified=bool(changes or commit.endswith('-dirty')),
                        paths=sources(name)),
            license=' AND '.join(dict.fromkeys(LICENSES[l][0] for l in ['wine'] + libs)),
            requires=dict(runtime_abi=list(component.get('runtime_abi', (RUNTIME_ABI, None))),
                          features=component.get('features', [])),
            classes=served))
        (card / path).mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll, card / path / name)

    manifest = dict(schema=SCHEMA, pack=pack, tag=tag,
                    source=dict(repo=SOURCE_REPO, commit=commit, wine='11.0', wine_import=WINE_IMPORT),
                    files=files)
    text = json.dumps(manifest, indent=2) + '\n'
    (out / 'repo/manifest.json').write_text(text)
    (out / 'assets/manifest.json').write_text(text)
    # A card keeps the manifest of what it has, so the next check knows.
    (card / 'horizon-dlls').mkdir(parents=True)
    (card / 'horizon-dlls/manifest.json').write_text(text)

    for key in sorted(licenses):
        _, source, target = LICENSES[key]
        shutil.copy2(source, out / 'repo' / target)
        (card / 'horizon-dlls' / target).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, card / 'horizon-dlls' / target)
    notice = write_notice(manifest, licenses)
    (out / 'repo/NOTICE.md').write_text(notice)
    (card / 'horizon-dlls/NOTICE.md').write_text(notice)
    (out / 'repo/README.md').write_text(README)

    archive = out / f'autorun-horizon-dlls-{tag}.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as zipf:
        for item in sorted((out / 'card').rglob('*')):
            if item.is_file():
                zipf.write(item, item.relative_to(out / 'card'))
    shutil.copy2(archive, out / 'assets' / archive.name)

    depends = [f['name'] for f in files if f['group'] == 'depends']
    print(f'  brought along, since the release card lacks them: {", ".join(depends) or "nothing"}')
    new = [f['name'] for f in files if f['url'].startswith(f'{DOWNLOAD}/{tag}/')]
    print(f'{tag}: {len(files)} files, {len(new)} new in this release; source {commit[:12]}')
    print(f'  modified since Wine 11.0: {", ".join(f["name"] for f in files if f["source"]["modified"]) or "none"}')
    print(f'  classes: {sum(len(f["classes"]) for f in files)}; offline zip {archive.stat().st_size >> 20} MB')
    print(f'  publish: commit {out}/repo to {PACK_REPO}, then attach {out}/assets to release {tag}')

def write_notice(manifest, licenses):
    source = manifest['source']
    modified = [f for f in manifest['files'] if f['source']['modified']]
    lines = [f"# autorun-horizon-dlls {manifest['tag']}", '',
             f"Built from Wine {source['wine']} as carried by "
             f"https://github.com/{source['repo']} at commit `{source['commit']}`. "
             f"The source of every file is there, under the paths its manifest entry lists.", '',
             '| Component | License | Text |', '|---|---|---|']
    names = {'wine': 'Wine', 'faudio': 'FAudio', 'fluidsynth': 'FluidSynth'}
    for key in sorted(licenses):
        lines.append(f'| {names[key]} | {LICENSES[key][0]} | `{LICENSES[key][2]}` |')
    lines += ['', 'FAudio is in the XAudio2, X3DAudio, XAPOFX and XACT files; FluidSynth is in dmsynth.dll.', '']
    if modified:
        lines += ['Changed from Wine 11.0 (see the commit history of the paths listed):', '']
        lines += [f"- `{f['name']}`: {', '.join(f['source']['paths'])}" for f in modified]
    else:
        lines.append('No file in this release is changed from Wine 11.0.')
    return '\n'.join(lines) + '\n'

README = '''# autorun-horizon-dlls

Windows components for [Autorun](https://github.com/autorunhq/autorun), the
Windows compatibility layer for the Nintendo Switch: the DirectX a game's
installer would have run on a PC (D3DX9, D3DCompiler, XInput, XAudio2, X3DAudio, XAPOFX,
XACT, DirectMusic, DirectPlay), built from Autorun's Wine. Autorun downloads
them itself; nothing here needs to be copied by hand.

**Without a network:** download `autorun-horizon-dlls-pack-N.zip` from the
latest release and extract it to the root of the SD card, merging folders.

Every file is built from Wine, some with changes for Horizon; `NOTICE.md` says
which, and where their source is. None of it is Microsoft's.

## manifest.json

`manifest.json` on `main` describes the latest release. Autorun reads it, and a
card keeps the one it installed from in `switch/wine/horizon-dlls/`.

```
schema        format version; Autorun ignores a manifest it does not know
pack, tag     the release number, and its GitHub release (pack-N)
source        repo, commit and Wine version everything was built from
files[]       one per file:
  name, path  where it goes, relative to switch/wine
  group       what it belongs to (d3dx9, xaudio2, directmusic, ...)
  version     this file's own version; it changes only when its bytes do
  size, sha256, url
              what to download and how to check it; an unchanged file keeps
              the URL of the release it came in
  source      repo, commit, the source paths, and whether they changed since
              Wine was imported
  license     SPDX expression
  requires    runtime_abi [min, max or null] and the runtime features the file
              needs; Autorun skips a file its runtime does not satisfy
  classes[]   COM classes the file serves (clsid, name, threading), which
              Autorun registers, as DllRegisterServer would on a PC
```

Releases are made with `wine-nx-probe/tools/package-horizon-dlls.py` in the
Autorun repository.
'''

if __name__ == '__main__':
    main()
