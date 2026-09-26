#!/usr/bin/env python3
"""Write the COM classes the staged DLLs serve, as a registry file the runtime
reads before a program asks for one.

On Windows each DLL's DllRegisterServer writes these when it is installed, and
Wine runs them from wine.inf at first boot. Nothing here installs anything, so a
program that asked for a class got REGDB_E_CLASSNOTREG and, more often than not,
used the null pointer it did not check: Fallout New Vegas creating its filter
graph, Halo its text service.

A class comes from the IDL of the DLL that serves it -- a coclass with a uuid,
which is what widl turns into those same registry keys -- and only from a DLL
whose spec exports DllGetClassObject, since one that does not cannot serve a
class whatever the registry says.

Some DLLs are one source built many times, XAudio2 2.0 to 2.7 and XACT among
them, and the IDL picks each version's class with #if, which a scan of the text
cannot follow. widl has already followed it: the registration script it builds
into the DLL, which its DllRegisterServer writes from, names the classes of that
version. Those come second, so a class the IDL gave a DLL stays with it.
"""
import argparse
from pathlib import Path
import os
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]

COCLASS = re.compile(
    r'\[(?P<attrs>[^\]]*?)\]\s*coclass\s+(?P<name>\w+)', re.S)
EXTRA_IDL = {'gameux': ('include/gameux.idl',)}


def idl_text(path, defines):
    compiler = os.environ.get('WINE_NX_IDL_CPP') or shutil.which('clang') or shutil.which('cpp')
    if not compiler:
        raise RuntimeError('IDL registration requires clang or cpp')
    return subprocess.check_output(
        [compiler, '-E', '-P', '-x', 'c', '-I', str(root / 'include'),
         *defines, str(path)], text=True)


def classes_of(dll):
    """(uuid, threading, coclass name) for each class this DLL serves."""
    source = root / 'dlls' / dll
    spec = source / f'{dll}.spec'
    if not spec.exists() or 'DllGetClassObject' not in spec.read_text():
        return []
    makefile = (source / 'Makefile.in').read_text()
    defines = ['-D' + item for item in re.findall(r'(?<!\S)-D([A-Za-z_]\w*(?:=\w+)?)', makefile)]
    found = []
    seen = set()
    idls = list(sorted(source.glob('*.idl'))) + [root / name for name in EXTRA_IDL.get(dll, ())]
    parent = re.search(r'^PARENTSRC\s*=\s*(\S+)', makefile, re.M)
    if parent:
        idls += sorted((source / parent.group(1)).glob('*.idl'))
    for idl in idls:
        # A typelib is a description of interfaces, not a list of what is served.
        if idl.name.endswith('_tlb.idl'):
            continue
        for match in COCLASS.finditer(idl_text(idl, defines)):
            attrs = match.group('attrs')
            uuid = re.search(r'uuid\s*\(\s*([0-9a-fA-F-]{36})\s*\)', attrs)
            if not uuid:
                continue
            if dll.startswith('xaudio2_') and match.group('name') not in (
                    'XAudio2', 'AudioVolumeMeter', 'AudioReverb'):
                continue
            if uuid.group(1).lower() in seen:
                continue
            seen.add(uuid.group(1).lower())
            threading = re.search(r'threading\s*\(\s*(\w+)\s*\)', attrs)
            threading = (threading.group(1) if threading else 'both').capitalize()
            found.append((uuid.group(1).lower(), threading, match.group('name')))
    return found

RGS_CLASS = re.compile(
    r"'\{(?P<uuid>[0-9a-fA-F-]{36})\}' = s '(?P<name>[^']*)'\s*\{\s*"
    r"InprocServer32 = s '%MODULE%'(?:\s*\{\s*val ThreadingModel = s '(?P<threading>\w+)')?")

def registered_classes_of(path):
    """(uuid, threading, name) for each class the DLL's registration script
    gives an InprocServer32 to; one with no server of its own is left out."""
    return [(m.group('uuid').lower(), (m.group('threading') or 'both').capitalize(), m.group('name'))
            for m in RGS_CLASS.finditer(path.read_bytes().decode('latin-1'))]

def write(stage, dlls):
    lines = ['WINE REGISTRY Version 2',
             ';; The classes the staged DLLs serve. Written by make-classes-reg.py from',
             ';; the IDL each DLL is built from, which is where widl reads them too.',
             '']
    seen = {}
    syswow64 = stage / 'drive_c/windows/syswow64'
    found = [(dll, classes_of(dll)) for dll in sorted(dlls)]
    found += [(dll, registered_classes_of(syswow64 / f'{dll}.dll')) for dll in sorted(dlls)]
    for dll, classes in found:
        for uuid, threading, name in classes:
            # The first DLL to claim a class keeps it, as the load order would.
            if uuid in seen:
                continue
            seen[uuid] = dll
            lines.append(f';; {dll}: {name}')
            lines.append(f'[Software\\\\Classes\\\\CLSID\\\\{{{uuid}}}\\\\InprocServer32]')
            lines.append(f'@="{dll}.dll"')
            lines.append(f'"ThreadingModel"="{threading}"')
            lines.append('')
    out = stage / 'config/classes.reg'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(lines))
    return len(seen)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', type=Path)
    parser.add_argument('--wine-source', type=Path, default=root)
    args = parser.parse_args()
    root = args.wine_source.resolve()
    stage = args.stage
    staged = sorted(p.stem for p in (stage / 'drive_c/windows/syswow64').glob('*.dll'))
    print(f'classes.reg: {write(stage, staged)} classes from {len(staged)} DLLs')
