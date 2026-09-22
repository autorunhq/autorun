#!/usr/bin/env python3
"""Check both FEX CPU modules against the packaged Wine interfaces."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(probe / 'tools'))
from fex_payload import required_exports, validate_image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('arm64ec', type=Path)
parser.add_argument('wow64fex', type=Path)
parser.add_argument('ntdll', type=Path)
parser.add_argument('wow64', type=Path)
args = parser.parse_args()


def inspect(path, option):
    return subprocess.check_output(['llvm-readobj', option, str(path)], text=True)


def exports(path):
    result = {}
    for block in re.findall(r'^(?:  )?Export \{\n(.*?)^(?:  )?\}', inspect(path, '--coff-exports'), re.M | re.S):
        name = re.search(r'^ +Name: (.*)$', block, re.M).group(1)
        rva = int(re.search(r'^ +RVA: (0x[0-9A-Fa-f]+)$', block, re.M).group(1), 16)
        result[name] = rva
    return result


native = exports(args.ntdll)
assert 'wine_nx_init_loader_indexes' in native
libraries = {'ntdll.dll': native, 'wow64.dll': exports(args.wow64)}
assert '__wine_switch_cpu_dll' in libraries['wow64.dll']
for path, architecture in ((args.arm64ec, 'arm64ec'), (args.wow64fex, 'arm64')):
    validate_image(path, architecture)
    required = required_exports(architecture)
    assert required <= exports(path).keys(), required - exports(path).keys()
    imports = inspect(path, '--coff-imports')
    expected = {'ntdll.dll'} if architecture == 'arm64ec' else {'ntdll.dll', 'wow64.dll'}
    assert set(re.findall(r'^  Name: (.+)$', imports, re.M)) == expected
    count = 0
    for block in re.findall(r'^Import \{\n(.*?)^\}', imports, re.M | re.S):
        name = re.search(r'^  Name: (.+)$', block, re.M).group(1)
        symbols = set(re.findall(r'^  Symbol: (\S+) \(', block, re.M))
        assert symbols <= libraries[name].keys(), (name, symbols - libraries[name].keys())
        count += len(symbols)
    print(f'{path.name}: {count} resolved imports and {len(required)} CPU exports passed')
syscalls = sorted((name for name in native if name.startswith('Nt') and name != 'NtGetTickCount'), key=native.get)
ids = dict((name, int(number, 16)) for number, name in re.findall(
    r'SYSCALL_ENTRY\( (0x[0-9a-f]+), (\w+),', (probe.parent / 'dlls/ntdll/ntsyscalls.h').read_text()))
for name in ('NtContinue', 'NtAllocateVirtualMemory', 'NtProtectVirtualMemory', 'NtRaiseException'):
    assert syscalls.index(name) == ids[name], (name, syscalls.index(name), ids[name])
assert 'IMAGE_FILE_MACHINE_ARM64EC' in inspect(args.arm64ec, '--file-headers')
assert 'CHPEMetadataPointer: 0x0\n' not in inspect(args.arm64ec, '--coff-load-config')
assert 'IMAGE_FILE_MACHINE_ARM64 (' in inspect(args.wow64fex, '--file-headers')
print('FEX contract: native syscall IDs and ARM64EC/WoW64 image types passed')
