#!/usr/bin/env python3
"""Pack the Carafe builds of FEX and DXVK the way a card downloads them.

    package.py fex  <libwow64fex.dll> <FEX checkout> <output folder>
    package.py fex-experimental <libwow64fex.dll> <FEX checkout> <output folder>
    package.py dxvk <DXVK build folder> <DXVK checkout> <output folder>

Each makes <name>-carafe-<version>.tar.gz, laid out like a DXVK release:
<name>-carafe-<version>/x32/*.dll for 32-bit programs and, for DXVK, x64/*.dll
for 64-bit ones. The version is the one in versions.txt, and the launcher
(horizon-wine/source/dxvk_releases.c) looks for a release tagged
<name>-carafe-<version> that holds an asset of that name. The experimental FEX
(build-fex.sh ... experimental) is fex-experimental-<version> throughout.
"""
import gzip
import hashlib
import io
import json
import re
import shutil
import struct
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FEX_REVISION = '395b132f346b1a45def246d10c52245edba1ef02'
DXVK_REVISION = 'b1a1c99ab52b687cf950d62c88bc2fa316b41663'
DXVK_DLLS = ('d3d8.dll', 'd3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll')
MACHINES = {'x32': (0x014c, 0x10b), 'x64': (0x8664, 0x20b), 'arm64': (0xaa64, 0x20b)}


def versions():
    found = {}
    for line in (HERE / 'versions.txt').read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            key, value = line.split('=', 1)
            found[key.strip()] = value.strip()
    for key in ('fex', 'fex-experimental', 'dxvk'):
        if not re.fullmatch(r'\d+(\.\d+)*-\d+', found.get(key, '')):
            sys.exit(f'versions.txt: {key} must look like 1.2.3-4')
    return found


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_image(path, kind):
    """The checks the launcher makes before it lets a DLL be used."""
    data = Path(path).read_bytes()
    machine, magic = MACHINES[kind]
    if len(data) < 64 or data[:2] != b'MZ':
        sys.exit(f'{path} is not a PE image')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if (offset < 64 or offset > len(data) - 26 or data[offset:offset + 4] != b'PE\0\0' or
            struct.unpack_from('<H', data, offset + 4)[0] != machine or
            struct.unpack_from('<H', data, offset + 24)[0] != magic or
            not struct.unpack_from('<H', data, offset + 22)[0] & 0x2000):
        sys.exit(f'{path} is not a {kind} DLL')
    return data


def tool(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f'Missing {name}; put llvm-mingw on PATH')
    return path


def patches(folder):
    return {p.name: digest(p.read_bytes()) for p in sorted((HERE / folder).glob('*.patch'))}


def write_archive(output, top, files):
    """files: archive path under top -> bytes. Plain ustar, sorted, no owner or
    time, so the same input makes the same archive."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(files):
            info = tarfile.TarInfo(f'{top}/{name}')
            if len(info.name) > 99:
                sys.exit(f'{info.name} is too long for the launcher to read')
            info.size = len(files[name])
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(files[name]))
    output.mkdir(parents=True, exist_ok=True)
    path = output / f'{top}.tar.gz'
    with open(path, 'wb') as f, gzip.GzipFile(fileobj=f, mode='wb', mtime=0, filename='') as z:
        z.write(raw.getvalue())
    print(f'{path}: {path.stat().st_size} bytes, sha256 {digest(path.read_bytes())}')
    return path


def pack_fex(dll, source, output, experimental=False):
    version = versions()['fex-experimental' if experimental else 'fex']
    name = 'fex-experimental' if experimental else 'fex-carafe'
    stripped = output / 'libwow64fex.dll'
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([tool('llvm-strip'), '--strip-debug', '-o', str(stripped), str(dll)], check=True)
    data = check_image(stripped, 'arm64')
    info = subprocess.check_output([tool('llvm-readobj'), '--coff-imports', '--coff-exports', str(stripped)],
                                   text=True)
    imports = set(re.findall(r'Import \{\n  Name: (.+)', info))
    if imports != {'ntdll.dll', 'wow64.dll'}:
        sys.exit(f'libwow64fex.dll imports {sorted(imports)}; the runtime gives it ntdll.dll and wow64.dll')
    required = set(re.findall(r'GET_PTR\( (BTCpu\w+|__wine_get_unix_opcode) \)',
                              (ROOT / 'dlls/wow64/syscall.c').read_text()))
    exports = set(re.findall(r'^  Name: (.+)$', info, re.M))
    if not required or not required <= exports:
        sys.exit(f'libwow64fex.dll lacks what wow64.dll asks of it: {sorted(required - exports)}')
    fex = ROOT / 'horizon-wine/fex'
    manifest = {
        'name': name, 'version': version, 'fex': 'FEX-2609', 'revision': FEX_REVISION,
        'built_from': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
        'horizon_patch_sha256': digest((fex / 'horizon.patch').read_bytes()),
        'abi_sha256': {name: digest((fex / name).read_bytes())
                       for name in ('unixlib.h', 'jit_registry.h', 'code_storage.h', 'cache_policy.h')},
        'carafe_patches': patches('fex'),
        'files': {'x32/libwow64fex.dll': digest(data)},
    }
    if experimental:
        manifest['experimental_patches'] = patches('fex-experimental')
    files = {'x32/libwow64fex.dll': data,
             f'{name}-manifest.json': (json.dumps(manifest, indent=2) + '\n').encode(),
             'licenses/FEX-MIT.txt': (Path(source) / 'LICENSE').read_bytes()}
    stripped.unlink()
    return write_archive(output, f'{name}-{version}', files)


def pack_dxvk(build, source, output):
    version = versions()['dxvk']
    files = {}
    listed = {}
    for arch in ('x32', 'x64'):
        for name in DXVK_DLLS:
            component = 'd3d10' if name == 'd3d10core.dll' else name[:-4]
            data = check_image(Path(build) / arch / 'src' / component / name, arch)
            files[f'{arch}/{name}'] = data
            listed[f'{arch}/{name}'] = digest(data)
    manifest = {
        'name': 'dxvk-carafe', 'version': version, 'dxvk': '3.1.1', 'revision': DXVK_REVISION,
        'built_from': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
        'carafe_patches': patches('dxvk'),
        'files': listed,
    }
    files['dxvk-carafe-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    files['licenses/DXVK-zlib.txt'] = (Path(source) / 'LICENSE').read_bytes()
    return write_archive(output, f'dxvk-carafe-{version}', files)


def main():
    if len(sys.argv) == 2 and sys.argv[1] in ('fex-version', 'fex-experimental-version', 'dxvk-version'):
        print(versions()[sys.argv[1][:-len('-version')]])
        return
    if len(sys.argv) != 5 or sys.argv[1] not in ('fex', 'fex-experimental', 'dxvk'):
        sys.exit(__doc__)
    what, built, source, output = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    if what == 'dxvk':
        pack_dxvk(built, source, output)
    else:
        pack_fex(built, source, output, experimental=what == 'fex-experimental')


if __name__ == '__main__':
    main()
