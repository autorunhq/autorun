#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile

source, output, runtime = map(Path, sys.argv[1:])
root = Path(__file__).resolve().parents[1]
build = Path('out/nintendo_nx_arm64_armv8a/release')
files = {
    'atmosphere/mesosphere.bin': source / 'mesosphere' / build / 'mesosphere.bin',
    'atmosphere/kips/autorun-loader.kip': source / 'stratosphere/loader' / build / 'loader.kip',
    'switch/wine/wine-nx-runtime.nro': runtime,
    'low-window/README.txt': root / 'mesosphere/README.txt',
    'low-window/low-window.patch': root / 'mesosphere/low-window.patch',
    'low-window/LICENSE.Atmosphere': source / 'LICENSE',
}
if b'nx-low-window-2' not in runtime.read_bytes():
    raise SystemExit('The NRO does not contain the low-window build marker')
contents = {name: path.read_bytes() for name, path in files.items()}
loader = contents['atmosphere/kips/autorun-loader.kip']
if (len(loader) < 0x100 or loader[:4] != b'KIP1' or
        loader[4:16].rstrip(b'\0') != b'Loader' or
        struct.unpack_from('<Q', loader, 0x10)[0] != 0x0100000000000001 or
        0x100 + sum(struct.unpack_from('<I', loader, 0x28 + i * 0x10)[0] for i in range(6)) != len(loader)):
    raise SystemExit('Invalid loader KIP header or segment sizes')
manifest = {
    'atmosphere_revision': '5388824be146a89619e8d641acd64599cf1c5f62',
    'program_id': '0548EABB35576000',
    'runtime_build': 'nx-low-window-2',
    'hardware_verified': False,
    'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in contents.items()},
}
output.mkdir(parents=True, exist_ok=True)
archive = output / 'autorun-low-window-2.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as package:
    for name, data in contents.items():
        package.writestr(name, data)
    package.writestr('low-window/manifest.json', json.dumps(manifest, indent=2) + '\n')
print(archive)
