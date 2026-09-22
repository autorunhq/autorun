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
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]

COCLASS = re.compile(
    r'\[(?P<attrs>[^\]]*?)\]\s*coclass\s+(?P<name>\w+)', re.S)
INCLUDE = re.compile(r'^\s*#include\s+"(?P<name>[^"\n]+\.idl)"', re.M)
EXTRA_IDL = {'gameux': (root / 'include/gameux.idl',)}


def idl_text(path, seen=None):
    seen = set() if seen is None else seen
    path = path.resolve()
    if path in seen:
        return ''
    seen.add(path)
    text = path.read_text()
    expanded = [text]
    for include in INCLUDE.finditer(text):
        name = include.group('name')
        for candidate in (path.parent / name, root / 'include' / name):
            if candidate.is_file():
                expanded.append(idl_text(candidate, seen))
                break
    return '\n'.join(expanded)


def classes_of(dll):
    """(uuid, threading, coclass name) for each class this DLL serves."""
    source = root / 'dlls' / dll
    spec = source / f'{dll}.spec'
    if not spec.exists() or 'DllGetClassObject' not in spec.read_text():
        return []
    found = []
    idls = list(sorted(source.glob('*.idl'))) + list(EXTRA_IDL.get(dll, ()))
    for idl in idls:
        # A typelib is a description of interfaces, not a list of what is served.
        if idl.name.endswith('_tlb.idl'):
            continue
        for match in COCLASS.finditer(idl_text(idl)):
            attrs = match.group('attrs')
            uuid = re.search(r'uuid\s*\(\s*([0-9a-fA-F-]{36})\s*\)', attrs)
            if not uuid:
                continue
            threading = re.search(r'threading\s*\(\s*(\w+)\s*\)', attrs)
            threading = (threading.group(1) if threading else 'both').capitalize()
            found.append((uuid.group(1).lower(), threading, match.group('name')))
    return found

def write(stage, dlls):
    lines = ['WINE REGISTRY Version 2',
             ';; The classes the staged DLLs serve. Written by make-classes-reg.py from',
             ';; the IDL each DLL is built from, which is where widl reads them too.',
             '']
    seen = {}
    for dll in sorted(dlls):
        for uuid, threading, name in classes_of(dll):
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
    stage = Path(sys.argv[1])
    staged = sorted(p.stem for p in (stage / 'drive_c/windows/syswow64').glob('*.dll'))
    print(f'classes.reg: {write(stage, staged)} classes from {len(staged)} DLLs')
