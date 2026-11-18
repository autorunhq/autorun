#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The DirectShow registry header matches its generator, and every FilterData
in it reads back the way quartz reads one (FM2_ReadFilterData in
dlls/quartz/filtermapper.c): the pins, their flags and the media types a
filter graph matches against."""
from pathlib import Path
import importlib.util
import re
import struct
import sys
import tempfile
import uuid

root = Path(__file__).resolve().parents[2]
script = root / 'horizon-wine/tools/make-directshow-reg.py'
header = root / 'dlls/ntdll/unix/horizon_directshow_reg.h'

spec = importlib.util.spec_from_file_location('make_directshow_reg', script)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

assert header.read_text(encoding='ascii') == generator.header(), \
    f'{header.name} is stale: run {script.relative_to(root)}'


def c_literal(text):
    """The string a run of adjacent C string literals stands for."""
    out = []
    for part in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        out.append(re.sub(r'\\(.)', lambda m: {'n': '\n', '\\': '\\', '"': '"'}[m[1]], part))
    return ''.join(out)


body = header.read_text(encoding='ascii').split('horizon_directshow_seed[] =', 1)[1].split(';\n\n#endif', 1)[0]
text = c_literal(body)
assert text == generator.registry_text() + '\n'
assert text.startswith('WINE REGISTRY Version 2\n')


def read_filter_data(data):
    """FM2_ReadFilterData: returns (merit, ((flags, ((major, minor), ...)), ...))."""
    version, merit, pins, unused = struct.unpack_from('<4I', data, 0)
    assert version == 2 and unused == 0
    position, out, offsets = 16, [], []
    for number in range(pins):
        signature, flags, instances, types, mediums, category = struct.unpack_from('<4s5I', data, position)
        assert signature == bytes([ord('0') + number]) + b'pi3', signature
        assert instances == 0 and mediums == 0 and category == 0
        position += 24
        found = []
        for index in range(types):
            signature, unused, major, minor = struct.unpack_from('<4s3I', data, position)
            assert signature == bytes([ord('0') + index]) + b'ty3', signature
            assert unused == 0
            offsets += [major, minor]
            found.append((str(uuid.UUID(bytes_le=data[major:major + 16])),
                          str(uuid.UUID(bytes_le=data[minor:minor + 16]))))
            position += 16
        out.append((flags, tuple(found)))
    # The GUIDs follow the fixed part, whole, and nothing else does.
    assert (len(data) - position) % 16 == 0
    assert all(position <= offset <= len(data) - 16 and (offset - position) % 16 == 0 for offset in offsets)
    assert set(offsets) == set(range(position, len(data), 16))
    return merit, tuple(out)


keys, key = {}, None
for line in text.split('\n'):
    if line.startswith('['):
        key = line[1:-1].replace('\\\\', '\\')
        keys[key] = {}
    elif line.startswith('"'):
        name, value = line.split('=', 1)
        keys[key][name.strip('"')] = value

prefix = 'Software\\Classes\\CLSID\\'
for clsid, name, merit in generator.CATEGORIES:
    entry = keys[prefix + generator.braced(generator.CLSID_ActiveMovieCategories) + '\\Instance\\' + generator.braced(clsid)]
    assert entry == {'CLSID': f'"{generator.braced(clsid)}"', 'FriendlyName': f'"{name}"', 'Merit': f'dword:{merit:08x}'}
for clsid, name, merit, pins in generator.FILTERS:
    entry = keys[prefix + generator.braced(generator.CLSID_LegacyAmFilterCategory) + '\\Instance\\' + generator.braced(clsid)]
    assert entry['CLSID'] == f'"{generator.braced(clsid)}"' and entry['FriendlyName'] == f'"{name}"'
    assert entry['FilterData'].startswith('hex:')
    data = bytes(int(byte, 16) for byte in entry['FilterData'][4:].split(','))
    assert read_filter_data(data) == (merit, tuple(pins)), name
assert len(keys) == len(generator.CATEGORIES) + len(generator.FILTERS)

# Written again, it is the same file.
with tempfile.TemporaryDirectory() as scratch:
    copy = Path(scratch) / 'again.h'
    copy.write_text(generator.header(), encoding='ascii', newline='\n')
    assert copy.read_bytes() == header.read_bytes()

# The filters and merits are the ones Wine's own DllRegisterServer names.
quartz = (root / 'dlls/quartz/main.c').read_text()
gstreamer = (root / 'dlls/winegstreamer/main.c').read_text()
for expected in ('L"Video Renderer"', 'L"AVI Decompressor"', 'L"File Source (Async.)"', 'L"ACM Wrapper"',
                 '.dwMerit = MERIT_PREFERRED + 1', '.dwMerit = MERIT_NORMAL - 16', '.dwMerit = MERIT_UNLIKELY'):
    assert expected in quartz, expected
assert 'L"GStreamer splitter filter"' in gstreamer and '.dwMerit = MERIT_NORMAL - 1' in gstreamer

print(f'DirectShow registry: {len(generator.FILTERS)} filters and {len(generator.CATEGORIES)} categories '
      'match the generator and read back as quartz reads them')
sys.exit(0)
