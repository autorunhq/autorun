#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct

path = Path(__file__).resolve().parents[1] / 'tools/pe_release.py'
spec = importlib.util.spec_from_file_location('pe_release', path)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def image(debug, machine):
    data = bytearray(1536 if debug else 1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 64)
    data[64:68] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 68, machine, 2 if debug else 1, 123, 0, 0, 240, 0x2022)
    struct.pack_into('<H', data, 88, 0x20b)
    struct.pack_into('<II', data, 88 + 32, 4096, 512)
    struct.pack_into('<II', data, 88 + 56, 12288 if debug else 8192, 512)
    data[328:333] = b'.text'
    struct.pack_into('<4I', data, 328 + 8, 64, 4096, 512, 512)
    struct.pack_into('<I', data, 328 + 36, 0x60000020)
    data[512:576] = bytes(range(64))
    if debug:
        data[368:374] = b'.debug'
        struct.pack_into('<4I', data, 368 + 8, 512, 8192, 512, 1024)
        struct.pack_into('<I', data, 368 + 36, 0x42000040)
        data[1024:] = b'D' * 512
    return data


for machine in (0x14c, 0xaa64, 0xa64e, 0xa641):
    original, stripped = image(True, machine), image(False, machine)
    release.verify_release(original, stripped)
    padded = bytearray(stripped)
    padded[576:1024] = b'\xcc' * (1024 - 576)
    release.verify_release(original, padded)
    for offset, value in ((512, 0xff), (328 + 12, 0xff), (88 + 16, 1), (88 + 112, 1), (88 + 56, 0xff)):
        corrupted = bytearray(stripped)
        corrupted[offset] = value
        try:
            release.verify_release(original, corrupted)
            raise AssertionError(f'accepted runtime change at {offset}')
        except ValueError:
            pass
print('PE release validation preserves code, RVAs, entry points, directories and image bounds')
