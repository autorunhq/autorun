#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write dlls/ntdll/unix/horizon_directshow_reg.h: the DirectShow filters the
runtime registers at every start.

    make-directshow-reg.py [OUTPUT]

On Windows a filter is registered by its DLL's DllRegisterServer. Here
autorun-setup.exe (tools/autorun_setup.c) calls that for quartz.dll and its
neighbours once per card, but not for winegstreamer.dll, so the one splitter
that plays a file through the runtime's own parser
(horizon-wine/source/media_unix.c) was in no filter graph's reach, and nothing
at all was registered on a card whose setup had not run yet.

The entries are that splitter, the two categories a graph enumerates, and the
quartz filters between a file and the screen, as winegstreamer and quartz
would write them (dlls/winegstreamer/main.c and dlls/quartz/main.c, with the
same merits and pins), in the layout of FM2_WriteFilterData
(dlls/quartz/filtermapper.c). The choice of these six filters is Carafe's
(runtime/dshow/add-directshow-filters.py), which adds the same keys to
classes.reg when it packs a game. They are loaded before system.reg, so what
a DllRegisterServer or a program has written since wins.

The header is checked in; horizon-wine/tests/check_directshow_reg.py fails
when it no longer matches this script."""
from pathlib import Path
import struct
import sys
import uuid

root = Path(__file__).resolve().parents[2]
default_output = root / 'dlls/ntdll/unix/horizon_directshow_reg.h'

# include/axextend.idl
MERIT_PREFERRED = 0x800000
MERIT_NORMAL = 0x600000
MERIT_UNLIKELY = 0x400000
REG_PINFLAG_B_RENDERER = 0x2
REG_PINFLAG_B_OUTPUT = 0x8

GUID_NULL = '00000000-0000-0000-0000-000000000000'
MEDIATYPE_Stream = 'e436eb83-524f-11ce-9f53-0020af0ba770'
MEDIATYPE_Video = '73646976-0000-0010-8000-00aa00389b71'
MEDIATYPE_Audio = '73647561-0000-0010-8000-00aa00389b71'

CLSID_ActiveMovieCategories = 'da4e3da0-d07d-11d0-bd50-00a0c911ce86'
CLSID_LegacyAmFilterCategory = '083863f1-70de-11d0-bd40-00a0c911ce86'
CLSID_AudioRendererCategory = 'e0f158e1-cb04-11d0-bd4e-00a0c911ce86'

# (CLSID, name, merit): IFilterMapper2::CreateCategory, as quartz calls it.
CATEGORIES = (
    (CLSID_LegacyAmFilterCategory, 'DirectShow Filters', MERIT_NORMAL),
    (CLSID_AudioRendererCategory, 'Audio Renderers', MERIT_NORMAL),
)


def pin(flags, *types):
    return flags, types


# (CLSID, name, merit, pins), every one in CLSID_LegacyAmFilterCategory.
FILTERS = (
    # winegstreamer: reg_decodebin_parser
    ('f9d8d64e-a144-47dc-8ee0-f53498372c29', 'GStreamer splitter filter', MERIT_NORMAL - 1,
     (pin(0, (MEDIATYPE_Stream, GUID_NULL)),
      pin(REG_PINFLAG_B_OUTPUT, (MEDIATYPE_Audio, GUID_NULL)),
      pin(REG_PINFLAG_B_OUTPUT, (MEDIATYPE_Video, GUID_NULL)))),
    # quartz: video_renderer_reg
    ('70e102b0-5556-11ce-97c0-00aa0055595a', 'Video Renderer', MERIT_PREFERRED,
     (pin(REG_PINFLAG_B_RENDERER, (MEDIATYPE_Video, GUID_NULL)),)),
    # quartz: video_renderer_default_reg
    ('6bc1cffa-8fc1-4261-ac22-cfb4cc38db50', 'Video Renderer', MERIT_PREFERRED + 1,
     (pin(REG_PINFLAG_B_RENDERER, (MEDIATYPE_Video, GUID_NULL)),)),
    # quartz: async_reader_reg
    ('e436ebb5-524f-11ce-9f53-0020af0ba770', 'File Source (Async.)', MERIT_UNLIKELY,
     (pin(REG_PINFLAG_B_OUTPUT, (MEDIATYPE_Stream, GUID_NULL)),)),
    # quartz: acm_wrapper_reg
    ('6a08cf80-0e18-11cf-a24d-0020afd79767', 'ACM Wrapper', MERIT_NORMAL - 16,
     (pin(0, (MEDIATYPE_Audio, GUID_NULL)),
      pin(REG_PINFLAG_B_OUTPUT, (MEDIATYPE_Audio, GUID_NULL)))),
    # quartz: avi_decompressor_reg
    ('cf49d4e0-1115-11ce-b03a-0020af0ba770', 'AVI Decompressor', MERIT_NORMAL - 16,
     (pin(0, (MEDIATYPE_Video, GUID_NULL)),
      pin(REG_PINFLAG_B_OUTPUT, (MEDIATYPE_Video, GUID_NULL)))),
)


def filter_data(merit, pins):
    """FM2_WriteFilterData for a version 2 REGFILTER2 whose pins have neither a
    category nor mediums: the fixed part, then the GUIDs it points at."""
    fixed_size = 16 + sum(24 + 16 * len(types) for _, types in pins)
    fixed = bytearray(struct.pack('<4I', 2, merit, len(pins), 0))
    guids = []

    def offset(value):
        if value not in guids:
            guids.append(value)
        return fixed_size + 16 * guids.index(value)

    for number, (flags, types) in enumerate(pins):
        # REG_RFP: signature, flags, instances, media types, mediums, category
        fixed += struct.pack('<4s5I', bytes([ord('0') + number]) + b'pi3', flags, 0, len(types), 0, 0)
        for index, (major, minor) in enumerate(types):
            # REG_TYPE: signature, unused, major type, minor type
            fixed += struct.pack('<4s3I', bytes([ord('0') + index]) + b'ty3', 0, offset(major), offset(minor))
    assert len(fixed) == fixed_size
    return bytes(fixed) + b''.join(uuid.UUID(value).bytes_le for value in guids)


def braced(value):
    return '{' + value.upper() + '}'


def registry_text():
    """The entries as a Wine registry file, relative to HKEY_LOCAL_MACHINE."""
    def key(*parts):
        return '[' + '\\\\'.join(('Software', 'Classes', 'CLSID') + parts) + ']'

    lines = ['WINE REGISTRY Version 2', ';; DirectShow: the categories and filters of make-directshow-reg.py', '']
    for clsid, name, merit in CATEGORIES:
        lines += [key(braced(CLSID_ActiveMovieCategories), 'Instance', braced(clsid)),
                  f'"CLSID"="{braced(clsid)}"', f'"FriendlyName"="{name}"', f'"Merit"=dword:{merit:08x}', '']
    for clsid, name, merit, pins in FILTERS:
        data = ','.join(f'{byte:02x}' for byte in filter_data(merit, pins))
        lines += [key(braced(CLSID_LegacyAmFilterCategory), 'Instance', braced(clsid)),
                  f'"CLSID"="{braced(clsid)}"', f'"FriendlyName"="{name}"', f'"FilterData"=hex:{data}', '']
    return '\n'.join(lines)


def c_string(line):
    return '"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"'


def header():
    out = ['/* SPDX-License-Identifier: LGPL-2.1-or-later */',
           '/* Written by horizon-wine/tools/make-directshow-reg.py; change that, not this.',
           ' *',
           ' * The DirectShow splitter of winegstreamer.dll, which plays a file through',
           ' * the runtime\'s FFmpeg parser and which no DllRegisterServer registers here,',
           ' * with the categories and the quartz.dll filters a filter graph connects it',
           ' * to. The same keys Carafe adds to classes.reg. Loaded into',
           ' * \\Registry\\Machine by horizon_registry_init, before system.reg. */',
           '#ifndef __WINE_NX_HORIZON_DIRECTSHOW_REG_H',
           '#define __WINE_NX_HORIZON_DIRECTSHOW_REG_H',
           '',
           'static const char horizon_directshow_seed[] =']
    out += ['    ' + c_string(line) for line in registry_text().split('\n')]
    out[-1] += ';'
    out += ['', '#endif', '']
    return '\n'.join(out)


if __name__ == '__main__':
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else default_output
    output.write_text(header(), encoding='ascii', newline='\n')
    print(f'DirectShow: {len(FILTERS)} filters in {len(CATEGORIES)} categories written to {output}')
