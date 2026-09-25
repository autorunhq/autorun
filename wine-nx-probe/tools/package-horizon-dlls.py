#!/usr/bin/env python3
"""Build autorun-horizon-dlls, the DLL repository: every Windows module Autorun
runs, built from this tree's Wine, written into a checkout of the repository.

    package-horizon-dlls.py [--repo ~/autorun-horizon-dlls] [--allow-dirty] [--no-build]

That is all of system32 (the aarch64 modules) and syswow64 (the i386 ones):
every DLL, driver and program Wine's PE build makes, with Autorun's own
winenxaudio.drv. The files are stripped of their debug information, which the
build tree keeps for reading logs.

The repo is laid out as the SD card is: each file at switch/wine/<path>/<name>
and the manifest at switch/wine/horizon-dlls/manifest.json, which is also what a
card keeps to know what it has. A download of the repo is what a player without
a network copies to the card.

A file's URL is its raw path on main, and its version changes only when its
bytes do, so a card downloads only what changed, and git keeps one copy of a
file however many commits carry it. The packager writes the new tree over the
old one; committing and pushing it is what publishes it.

A module that calls straight into the runtime needs a runtime built against
the same interface. Its entry requires the features the runtime reports for
that: unixlib:<name> or unixlib32:<name> for being in the runtime's static
unix-call tables (read from dlls/ntdll/unix/virtual.c), and iface:<name>:<hash>
for the headers runtime-interfaces.json lists for it. Autorun leaves a file its
runtime does not satisfy as it is.

Every file names the commit it was built from, and whether its sources changed
since Wine 11.0 was imported, which is what the LGPL asks of a changed library.
Sources with changes that are not committed stop the build, since the source
the manifest points to would not be what the file was built from; --allow-dirty
is for trying the packager, and marks the commit so.
"""
from pathlib import Path
import argparse
import functools
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
root = probe.parent
tools = probe / 'tools'
pe = probe / 'build-wine-wow64-pe'
toolchain = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'

PACK_REPO = 'autorunhq/autorun-horizon-dlls'
SOURCE_REPO = 'autorunhq/autorun'
RAW = f'https://raw.githubusercontent.com/{PACK_REPO}'
# Where a card, and the repo, keep the manifest; relative to switch/wine.
MANIFEST = 'horizon-dlls/manifest.json'
# The commit that imported Wine 11.0; a file whose sources a later commit
# touched is a changed copy of Wine's.
WINE_IMPORT = 'eaa5b16e'
SCHEMA = 1
# The runtime these files are for. The AMD64 runtime has its own system32
# (ARM64X, from build-wine-amd64-pe) at the same paths, and gets entries of its
# own when it is built.
FLAVOR = 'x86'
# Where each architecture's modules go, as on Windows on ARM.
ARCHES = {'i386': 'drive_c/windows/syswow64', 'aarch64': 'drive_c/windows/system32'}
MODULE = r'(?:dll|drv|exe|sys|ocx|cpl|acm|ax|tlb|ds|msstyles|dll16|drv16|exe16|mod16|vxd)'
# Wine's test runner carries every test program inside it, hundreds of MB per
# architecture, over what GitHub keeps in one file; it is no part of Windows.
EXCLUDE = {'winetest.exe'}

# What a file belongs to, for showing a player what is there; the rest is Wine.
GROUPS = [
    ('core', r'(ntdll|wow64|wow64win|wow64cpu|win32u|winebox64|apisetschema|kernel32|kernelbase)\.dll'),
    ('d3dx9', r'd3dx9_\d+\.dll'), ('d3dcompiler', r'd3dcompiler_\d+\.dll'),
    ('xinput', r'xinput.*\.dll'), ('xaudio2', r'xaudio2_\d\.dll'), ('x3daudio', r'x3daudio1_\d\.dll'),
    ('xapofx', r'xapofx1_\d\.dll'), ('xact', r'xactengine\d_\d\.dll'),
    ('directmusic', r'(dmusic|dmusic32|dmime|dmloader|dmstyle|dmsynth|dmband|dmcompos|dmscript|dswave|dsdmo)\.dll'),
    ('directplay', r'(dplay|dplayx|dpnet|dpwsockx|dpnhpast|dpvoice)\.dll'),
    ('direct3d', r'(d3d\w*|ddraw\w*|dxgi|wined3d|d3dim\w*|d3drm|d3dxof)\.dll'),
    ('audio', r'(dsound|winmm|mmdevapi|winenxaudio)\.(dll|drv)'),
    ('programs', r'.*\.exe'),
]

# Modules built outside Wine's make: Autorun's own, as its packagers build them.
def build_audio_driver():
    driver = pe / 'winenxaudio.drv'
    subprocess.run([str(toolchain / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
                    '-fno-builtin', '-nostdlib', '-shared', '-Wl,--entry,_DllMain@12', '-Wl,--dynamicbase',
                    '-o', str(driver), str(probe / 'source/audio_driver.c')], check=True, env=env)
    return driver

EXTRA = [dict(arch='i386', name='winenxaudio.drv', build=build_audio_driver,
              sources=['wine-nx-probe/source/audio_driver.c'], origin='autorun')]

# The licenses of what the files are built from: Wine, compiler-rt, which the
# build links into every module, and the libraries some of them link in (by the
# $(NAME_PE_LIBS) their Makefile.in imports).
ALWAYS = ['wine', 'compiler-rt']
LICENSES = {
    'wine': ('LGPL-2.1-or-later', 'COPYING.LIB', 'Wine'),
    'compiler-rt': ('NCSA OR MIT', 'libs/compiler-rt/LICENSE.TXT', 'compiler-rt'),
    'capstone': ('BSD-3-Clause', 'libs/capstone/LICENSE.TXT', 'Capstone'),
    'musl': ('MIT', 'libs/musl/COPYRIGHT', 'musl'),
    'faudio': ('Zlib', 'libs/faudio/LICENSE', 'FAudio'),
    'fluidsynth': ('LGPL-2.1-or-later', 'COPYING.LIB', 'FluidSynth'),
    'gsm': ('TU-Berlin-2.0', 'libs/gsm/COPYRIGHT', 'libgsm'),
    'jpeg': ('IJG', 'libs/jpeg/LICENSE', 'libjpeg'),
    'jxr': ('BSD-2-Clause', 'libs/jxr/LICENSE', 'jxrlib'),
    'lcms2': ('MIT', 'libs/lcms2/COPYING', 'Little CMS'),
    'ldap': ('OLDAP-2.8', 'libs/ldap/LICENSE', 'OpenLDAP'),
    'mpg123': ('LGPL-2.1-only', 'libs/mpg123/LICENSE', 'mpg123'),
    'png': ('Libpng', 'libs/png/LICENSE', 'libpng'),
    'tiff': ('libtiff', 'libs/tiff/COPYRIGHT', 'libtiff'),
    'tomcrypt': ('Unlicense', 'libs/tomcrypt/LICENSE', 'LibTomCrypt'),
    'vkd3d': ('LGPL-2.1-or-later', 'libs/vkd3d/COPYING', 'vkd3d'),
    'xml2': ('MIT', 'libs/xml2/COPYING', 'libxml2'),
    'xslt': ('MIT', 'libs/xslt/COPYING', 'libxslt'),
    'zlib': ('Zlib', 'libs/zlib/LICENSE', 'zlib'),
}

env = dict(os.environ, PATH=f'{toolchain}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])

def git(*args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

def pack_git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()

def readobj(option, path):
    return subprocess.check_output([str(toolchain / 'llvm-readobj'), option, str(path)], text=True)

spec = importlib.util.spec_from_file_location('classes', tools / 'make-classes-reg.py')
classes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classes)

def targets():
    """Every PE module Wine's make builds, as (arch, source dir, file name)."""
    database = subprocess.run(['make', '-C', str(pe), '-pnq', 'all'], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    found = set(re.findall(rf'\b((?:dlls|programs)/[^ :/]+)/(i386|aarch64)-windows/([^ :/]+\.{MODULE})\b', database))
    return sorted((arch, directory, name) for directory, arch, name in found if name.lower() not in EXCLUDE)

def makefile(directory):
    text = (root / directory / 'Makefile.in').read_text()
    return dict(re.findall(r'^(\w+)\s*=\s*(.*(?:\\\n.*)*)', text, re.M))

def libraries(directory):
    """The bundled libraries a module links in, by name under libs/."""
    imports = makefile(directory).get('IMPORTS', '')
    return sorted(lib.lower() for lib in re.findall(r'\$\((\w+)_PE_LIBS\)', imports)
                  if (root / 'libs' / lib.lower()).is_dir())

def sources(directory):
    """The directories a module is built from: its own, the one it shares
    sources with, and the bundled libraries it links in."""
    paths = [directory]
    parent = makefile(directory).get('PARENTSRC')
    if parent:
        paths.append(os.path.normpath(f'{directory}/{parent.strip()}'))
    return paths + [f'libs/{lib}' for lib in libraries(directory)]

def static_unix_libs():
    """The modules the runtime's static unix-call tables name: native ones by a
    wide string, WoW64 ones by a narrow one."""
    text = (root / 'dlls/ntdll/unix/virtual.c').read_text()
    native = text[text.index('wine_nx_static_unix_libs[] ='):]
    native = native[:native.index('};')]
    wow64 = text[text.index('wine_nx_static_wow64_unix_libs[] ='):]
    wow64 = wow64[:wow64.index('};')]
    return ({''.join(re.findall(r"'(.)'", entry)) for entry in re.findall(r'\{\s*\{([^}]*)\}', native)},
            set(re.findall(r'\{\s*"([^"]+)"', wow64)))

def interfaces():
    table = json.loads((probe / 'runtime-interfaces.json').read_text())
    result = {}
    for name, files in table.items():
        if name.startswith('_'):
            continue
        digest = hashlib.sha256()
        for path in files:
            digest.update((root / path).read_bytes())
        result[name] = f'iface:{name}:{digest.hexdigest()[:12]}'
    return result

def group_of(name):
    for group, pattern in GROUPS:
        if re.fullmatch(pattern, name.lower()):
            return group
    return 'wine'

def build(modules):
    subprocess.run(['make', '-C', str(pe), '-k', f'-j{os.cpu_count()}',
                    *[f'{directory}/{arch}-windows/{name}' for arch, directory, name in modules]],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def stripped(path, into):
    """The file without its debug information, as the repository carries it."""
    out = into / path.name
    subprocess.run([str(toolchain / 'llvm-strip'), '--strip-debug', '-o', str(out), str(path)], check=True)
    return out

def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--repo', type=Path, default=root.parent / 'autorun-horizon-dlls',
                        help='a checkout of the DLL repository')
    parser.add_argument('--allow-dirty', action='store_true', help='build from uncommitted sources, for trying it')
    parser.add_argument('--no-build', action='store_true', help='package what is built already')
    args = parser.parse_args()

    repo = args.repo.resolve()
    assert (repo / '.git').exists(), f'{repo} is not a checkout of {PACK_REPO}'
    assert not pack_git(repo, 'status', '--porcelain'), f'{repo} has uncommitted changes'
    card = repo / 'switch/wine'
    previous = json.loads((card / MANIFEST).read_text()) if (card / MANIFEST).exists() else None
    if previous:
        assert previous['schema'] == SCHEMA, f"previous manifest is schema {previous['schema']}"
    earlier = {f"{f['path']}/{f['name']}": f for f in previous['files']} if previous else {}

    modules = targets()
    if not args.no_build:
        build(modules)
    missing = [f'{d}/{a}-windows/{n}' for a, d, n in modules if not (pe / d / f'{a}-windows' / n).exists()]
    assert not missing, f'{len(missing)} modules did not build, e.g. {missing[:5]}'
    entries = [dict(arch=a, name=n, file=pe / d / f'{a}-windows' / n, sources=sources(d),
                    libs=libraries(d), module_dir=d, origin='wine') for a, d, n in modules]
    for extra in EXTRA:
        entries.append(dict(arch=extra['arch'], name=extra['name'], file=extra['build'](),
                            sources=extra['sources'], libs=[], module_dir=None, origin=extra['origin']))
    names = [(e['arch'], e['name'].lower()) for e in entries]
    assert len(set(names)) == len(names), 'a file is built twice'

    commit = git('rev-parse', 'HEAD')
    dirty = git('status', '--porcelain', '--', 'include', 'libs', 'dlls', 'programs',
                'wine-nx-probe/source/audio_driver.c', 'wine-nx-probe/runtime-interfaces.json')
    if dirty:
        assert args.allow_dirty, f'uncommitted changes in what the DLLs are built from:\n{dirty}'
        commit += '-dirty'
    elif not git('branch', '-r', '--contains', commit):
        print(f'warning: {commit[:8]} is not pushed; push it before publishing, the manifest points there')

    native_table, wow64_table = static_unix_libs()
    iface = interfaces()
    loaded = {arch: {name for a, name in names if a == arch} for arch in ARCHES}

    # The tree is written anew; git sees what did not change as unchanged.
    shutil.rmtree(repo / 'switch', ignore_errors=True)
    shutil.rmtree(repo / 'LICENSES', ignore_errors=True)
    (repo / 'manifest.json').unlink(missing_ok=True)

    files, licenses, unresolved = [], set(ALWAYS), []
    claimed = {arch: {} for arch in ARCHES}
    scratch = Path(tempfile.mkdtemp())
    for entry in sorted(entries, key=lambda e: (ARCHES[e['arch']], e['name'].lower())):
        name, arch, path = entry['name'], entry['arch'], ARCHES[entry['arch']]
        lower = name.lower()
        shipped = stripped(entry['file'], scratch)
        machine = readobj('--file-headers', shipped)
        assert f'Arch: {arch}\n' in machine, f'{name} is not {arch}'

        # What it imports at load time is in the repository, for its architecture.
        for module in re.findall(r'^Import \{\n  Name: (.+)$', readobj('--coff-imports', shipped), re.M):
            module = module.lower()
            if not module.startswith(('api-ms-', 'ext-ms-')) and module not in loaded[arch]:
                unresolved.append(f'{arch} {name} -> {module}')

        digest = hashlib.sha256(shipped.read_bytes()).hexdigest()
        before = earlier.get(f'{path}/{name}')
        version = before['version'] + (before['sha256'] != digest) if before else 1
        url = f'{RAW}/main/switch/wine/{path}/{name}'

        served = []
        found = classes.registered_classes_of(shipped)
        if entry['module_dir'] and entry['module_dir'].startswith('dlls/'):
            found = classes.classes_of(entry['module_dir'][5:]) + found
        for uuid, threading, coclass in found:
            if uuid in claimed[arch]:
                continue
            claimed[arch][uuid] = name
            served.append(dict(clsid=uuid, name=coclass, threading=threading))

        features = []
        if arch == 'aarch64' and lower in native_table:
            features.append(f'unixlib:{lower}')
        if arch == 'i386' and lower in wow64_table:
            features.append(f'unixlib32:{lower}')
        # The interface a module shares with the runtime matters where it calls
        # in: what a table names, and the native ntdll and win32u's syscalls.
        if lower in iface and (features or (arch == 'aarch64' and lower in ('ntdll.dll', 'win32u.dll'))):
            features.append(iface[lower])

        changes = git('log', '--format=%h', f'{WINE_IMPORT}..HEAD', '--', *entry['sources']).split()
        licenses.update(entry['libs'])
        files.append(dict(
            name=name, path=path, arch=arch, group=group_of(name), version=version,
            size=shipped.stat().st_size, sha256=digest, url=url,
            source=dict(repo=SOURCE_REPO, commit=commit, origin=entry['origin'],
                        modified=entry['origin'] != 'wine' or bool(changes) or commit.endswith('-dirty'),
                        paths=entry['sources']),
            license=' AND '.join(f'({LICENSES[l][0]})' if ' OR ' in LICENSES[l][0] else LICENSES[l][0]
                              for l in dict.fromkeys(ALWAYS + entry['libs'])),
            requires=dict(flavor=FLAVOR, features=features),
            classes=served))
        (card / path).mkdir(parents=True, exist_ok=True)
        shutil.copy2(shipped, card / path / name)
    shutil.rmtree(scratch)

    manifest = dict(schema=SCHEMA, flavor=FLAVOR,
                    source=dict(repo=SOURCE_REPO, commit=commit, wine='11.0', wine_import=WINE_IMPORT),
                    files=files)
    (card / MANIFEST).parent.mkdir(parents=True, exist_ok=True)
    (card / MANIFEST).write_text(json.dumps(manifest, indent=1) + '\n')

    (repo / 'LICENSES').mkdir()
    for key in sorted(licenses):
        shutil.copy2(root / LICENSES[key][1], repo / 'LICENSES' / f'{LICENSES[key][2]}.txt')
    (repo / 'NOTICE.md').write_text(write_notice(manifest, licenses))
    (repo / 'README.md').write_text(README)

    new = [f for f in files if f['version'] == 1 or f['sha256'] != earlier.get(f"{f['path']}/{f['name']}", {}).get('sha256')]
    for arch, path in ARCHES.items():
        mine = [f for f in files if f['arch'] == arch]
        print(f'{path}: {len(mine)} files, {sum(f["size"] for f in mine) >> 20} MB')
    print(f'{len(files)} files ({sum(f["size"] for f in files) >> 20} MB), {len(new)} new or changed '
          f'({sum(f["size"] for f in new) >> 20} MB); source {commit[:12]}')
    print(f'  modified from Wine 11.0 or Autorun\'s own: {sum(f["source"]["modified"] for f in files)}')
    print(f'  tied to the runtime: {", ".join(f["arch"] + " " + f["name"] for f in files if f["requires"]["features"])}')
    print(f'  classes: {sum(len(f["classes"]) for f in files)}')
    if unresolved:
        print(f'  imports nothing in the repository provides ({len(unresolved)}): {", ".join(unresolved[:12])}')
    print(f'  publish from {repo}:')
    print(f'    git add -A && git commit -m "..." && git push origin main')

def write_notice(manifest, licenses):
    source = manifest['source']
    modified = [f for f in manifest['files'] if f['source']['modified']]
    lines = ['# autorun-horizon-dlls', '',
             f"Built from Wine {source['wine']} as carried by "
             f"https://github.com/{source['repo']} at commit `{source['commit']}`. "
             f"The source of every file is there, under the paths its manifest entry lists.", '',
             '| Component | License | Text |', '|---|---|---|']
    for key in sorted(licenses):
        spdx, _, title = LICENSES[key]
        lines.append(f'| {title} | {spdx} | `LICENSES/{title}.txt` |')
    lines += ['', 'compiler-rt is built into every file; the other libraries besides Wine into the',
              'files whose manifest entry names their license.', '']
    if modified:
        lines += ["Changed from Wine 11.0, or Autorun's own (see the commit history of the paths listed):", '']
        lines += [f"- `{f['path']}/{f['name']}`: {', '.join(f['source']['paths'])}" for f in modified]
    else:
        lines.append('No file here is changed from Wine 11.0.')
    return '\n'.join(lines) + '\n'

README = '''# autorun-horizon-dlls

The Windows side of [Autorun](https://github.com/autorunhq/autorun), the
Windows compatibility layer for the Nintendo Switch: every module in
`system32` (aarch64) and `syswow64` (i386), built from Autorun's Wine. Autorun
downloads them itself; nothing here needs to be copied by hand.

**Without a network:** download this repository (Code, Download ZIP) and copy
its `switch` folder to the root of the SD card, merging folders.

Every file is built from Wine, some with changes for Horizon; `NOTICE.md` says
which, and where their source is. None of it is Microsoft's.

## Layout

The repository is laid out as the SD card is: `switch/wine/drive_c/windows/`
holds the files, and `switch/wine/horizon-dlls/manifest.json` describes them.

## manifest.json

Autorun reads the manifest on `main`, and a card keeps the one it installed
from in the same place.

```
schema        format version; Autorun ignores a manifest it does not know
flavor        the runtime the files are for (x86: the WoW64 runtime)
source        repo, commit and Wine version everything was built from
files[]       one per file:
  name, path  where it goes, relative to switch/wine
  arch        i386 (syswow64) or aarch64 (system32)
  group       what it belongs to (core, d3dx9, xaudio2, programs, wine, ...)
  version     this file's own version; it changes only when its bytes do
  size, sha256, url
              what to download and how to check it; url is the file's raw
              path on main
  source      repo, commit, origin (wine or autorun), the source paths, and
              whether they changed since Wine was imported
  license     SPDX expression
  requires    flavor, and the features the runtime has to report for the
              file: unixlib:<name> / unixlib32:<name> for a module the
              runtime's unix-call tables name, iface:<name>:<hash> for the
              headers it shares with the runtime. Autorun leaves a file its
              runtime does not satisfy as it is.
  classes[]   COM classes the file serves (clsid, name, threading), which
              Autorun registers, as DllRegisterServer would on a PC
```

The files are built with `wine-nx-probe/tools/package-horizon-dlls.py` in the
Autorun repository.
'''

if __name__ == '__main__':
    main()
