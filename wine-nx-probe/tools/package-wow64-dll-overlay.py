#!/usr/bin/env python3
"""Build the i386 DLLs a program found missing, with what they import, as an
overlay for a card that already holds a Wine-NX package.

Wine's loader names every DLL it cannot find ("Library X (which is needed by Y)
not found"), so pass those names, or the autorun_runtime.log that has them:

    package-wow64-dll-overlay.py ddraw dinput8 netapi32 shfolder tapi32
    package-wow64-dll-overlay.py --log debug/autorun_runtime.log

With a log, the DLLs that same run loaded from syswow64 are left out, since the
card evidently has them. Anything else the imports reach is included, which can
repeat a DLL the card already holds but never leaves one behind. Delay-loaded
DLLs are not followed: they are loaded on first use, not at startup."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import argparse
import functools
import os
import re
import shutil
import subprocess

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('dlls', nargs='*', help='DLL names, with or without .dll')
parser.add_argument('--log', type=Path, help='a autorun_runtime.log to read missing and present DLLs from')
parser.add_argument('--name', default='dlls', help='label for the overlay zip')
args = parser.parse_args()

def dll_name(name):
    name = name.strip().lower()
    return name if name.endswith(('.dll', '.drv')) else name + '.dll'

wanted = [dll_name(n) for n in args.dlls]
# The runtime maps the i386 ntdll itself (runtime_start_wow64), so no log line
# shows it opened, yet every program that starts at all has it.
present = {'ntdll.dll'}
if args.log:
    text = args.log.read_text(errors='replace')
    wanted += [dll_name(n) for n in re.findall(r'Library (\S+) \(which is needed by', text)]
    opened = {n.lower() for n in re.findall(r"C:\\windows\\syswow64\\([^'\\]+\.(?:dll|drv))'", text, re.I)}
    failed = {n.lower() for n in re.findall(r'Failed to load module L"([^"]+)"', text)}
    present |= opened - failed
wanted = list(dict.fromkeys(wanted))
assert wanted, 'no DLLs to stage: name some, or pass a log that reports missing ones'

stage_root = build / f'{args.name}-overlay-sd-card'
syswow64 = stage_root / 'switch/wine/drive_c/windows/syswow64'
shutil.rmtree(stage_root, ignore_errors=True)
syswow64.mkdir(parents=True)

def readobj(option, path):
    return subprocess.check_output([str(tools / 'llvm-readobj'), option, str(path)], text=True)

def module_dir(name):
    return name.removesuffix('.dll')

@functools.lru_cache(maxsize=None)
def built(name):
    """The DLL from the PE tree, built if it is not yet: even a DLL the card has
    is read here, because its forwarded exports can name one it lacks."""
    assert re.fullmatch(r'[a-z0-9_-]+\.(dll|drv)', name), name
    target = f'dlls/{module_dir(name)}/i386-windows/{name}'
    subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
    return pe / target

@functools.lru_cache(maxsize=None)
def forwards_of(name):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', built(name)), re.M))

included, queue = [], []

def need(name):
    # api-ms-win-* and ext-ms-* are API sets, which ntdll resolves; no file backs them.
    if name.startswith(('api-ms-', 'ext-ms-')) or name in present or name in included:
        return
    shutil.copy2(built(name), syswow64 / name)
    included.append(name)
    queue.append(name)

for name in wanted:
    need(name)
while queue:
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', built(queue.pop())), re.M | re.S):
        name = dll_name(re.search(r'Name: (.+)', block).group(1))
        if name.startswith(('api-ms-', 'ext-ms-')):
            continue
        need(name)
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        for symbol in sorted(symbols & forwards_of(name).keys()):
            need(dll_name(forwards_of(name)[symbol]))

for name in included:
    assert 'Arch: i386\n' in readobj('--file-headers', syswow64 / name), f'{name} is not i386'
# A DLL with a Unix half may refuse to load without one: its DllMain can fail on
# __wine_init_unix_call, as crypt32's did. The Switch runtime links only the
# static WoW64 tables in dlls/ntdll/unix/virtual.c, so these need a look.
unix = [n for n in included if re.search(r'^UNIXLIB', (Path(pe).parents[1] / 'dlls' / module_dir(n) / 'Makefile.in').read_text(), re.M)]

archive = build / f'wine-nx-{args.name}-overlay-dynarec-{marker}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    for name in included:
        z.write(syswow64 / name, (syswow64 / name).relative_to(stage_root))
with ZipFile(archive) as z:
    assert z.testzip() is None

print(f'asked for: {" ".join(wanted)}')
if args.log:
    print(f'left out, loaded in that run: {len(present)} DLLs')
for name in included:
    print(f'  {name:24} {(syswow64 / name).stat().st_size / 2**10:8.0f} KiB')
if unix:
    print(f'with a Unix half, check their DllMain: {" ".join(unix)}')
print(f'{archive} ({archive.stat().st_size / 2**20:.1f} MiB, {len(included)} DLLs)')
