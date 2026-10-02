#!/usr/bin/env python3
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
from unittest.mock import patch
from zipfile import ZipFile

source = Path(__file__).resolve().parents[1] / 'tools/wine_mono_payload.py'
spec = importlib.util.spec_from_file_location('wine_mono_payload', source)
mono = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mono)


def make_archive(path, extra=None, link_target=None, omit=None):
    with tarfile.open(path, 'w:xz') as archive:
        files = {
            'bin/libmono-2.0-x86.dll': b'x86',
            'bin/libmono-2.0-arm64.dll': b'arm64',
            'bin/libmono-2.0-x86_64.dll': b'x86_64',
            'etc/mono/config': b'config',
            'etc/mono/4.5/machine.config': b'machine',
            'lib/mono/4.5/mscorlib.dll': b'corlib',
            'lib/mono/gac/System/4.0.0.0__key/System.dll': b'assembly',
            'lib/x86/MonoPosixHelper.dll': b'posix',
            'lib/x86_64/MonoPosixHelper.dll': b'x86_64 posix',
            'lib/arm64/MonoPosixHelper.dll': b'arm64 posix',
            'lib/x86/SDL3.dll': b'unused',
            'lib/mono/4.5/Facades/netstandard.dll': b'facade',
            'lib/mono/4.5/mcs.exe': b'compiler',
            'lib/mono/4.5/Microsoft.CodeAnalysis.dll': b'compiler support',
            'lib/mono/4.5-api/mscorlib.dll': b'reference assembly',
            'lib/mono/xbuild/tasks.dll': b'build tools',
            'lib/mono/gac/Microsoft.Build/4.0.0.0__key/Microsoft.Build.dll': b'build',
            'lib/mono/gac/Microsoft.Xna.Framework/4.0.0.0__key/Microsoft.Xna.Framework.dll': b'xna',
        }
        files.update(extra or {})
        if omit:
            del files[omit]
        for name, data in files.items():
            info = tarfile.TarInfo(mono.ROOT + '/' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        alias = tarfile.TarInfo(mono.ROOT + '/lib/mono/4.5/System.dll')
        alias.type = tarfile.SYMTYPE
        alias.linkname = link_target or '../gac/System/4.0.0.0__key/System.dll'
        archive.addfile(alias)


with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    archive = directory / 'mono.tar.xz'
    make_archive(archive)
    mono.ARCHIVE_SHA256 = hashlib.sha256(archive.read_bytes()).hexdigest()
    result = mono.stage_wine_mono(archive, directory / 'stage')
    expected = {
        'bin/libmono-2.0-x86.dll', 'etc/mono/config', 'etc/mono/4.5/machine.config',
        'lib/mono/4.5/mscorlib.dll', 'lib/mono/4.5/System.dll',
        'lib/mono/gac/System/4.0.0.0__key/System.dll',
        'lib/mono/4.5/Facades/netstandard.dll', 'lib/x86/MonoPosixHelper.dll',
        'bin/libmono-2.0-x86_64.dll', 'lib/x86_64/MonoPosixHelper.dll',
    }
    assert result['files'] == len(expected)
    assert result['arch'] == ['i386', 'amd64']
    assert {p.relative_to(directory / 'stage').as_posix()
            for p in (directory / 'stage').rglob('*') if p.is_file()} == expected
    assert (directory / 'stage/lib/mono/4.5/System.dll').read_bytes() == b'assembly'

    output = directory / 'mono.zip'
    with patch('sys.argv', [str(source), str(archive), '--output', str(output)]):
        mono.main()
    with ZipFile(output) as package:
        assert package.testzip() is None
        assert set(package.namelist()) == {
            (mono.INSTALL_PATH / name).as_posix() for name in expected
        } | {'switch/wine/licenses/WineMono.txt'}

    mono.ARCHIVE_SHA256 = '0' * 64
    try:
        mono.stage_wine_mono(archive, directory / 'bad-hash')
    except ValueError as error:
        assert 'checksum mismatch' in str(error)
    else:
        raise AssertionError('Accepted a corrupt archive')
    assert not (directory / 'bad-hash').exists()

    for arguments, message in (
        ({'link_target': '../../../../outside.dll'}, 'Invalid Wine Mono link'),
        ({'link_target': 'System.dll'}, 'Cyclic Wine Mono link'),
        ({'link_target': 'missing.dll'}, 'Invalid Wine Mono link'),
        ({'extra': {'lib/mono/4.5/../../../../outside.dll': b'bad'}}, 'Invalid Wine Mono path'),
        ({'extra': {'lib/mono/4.5/MSCorLib.dll': b'duplicate'}}, 'Duplicate Wine Mono path'),
        ({'omit': 'lib/mono/4.5/mscorlib.dll'}, 'lacks required runtime files'),
        ({'omit': 'bin/libmono-2.0-x86_64.dll'}, 'lacks required runtime files'),
    ):
        make_archive(archive, **arguments)
        mono.ARCHIVE_SHA256 = hashlib.sha256(archive.read_bytes()).hexdigest()
        try:
            mono.stage_wine_mono(archive, directory / 'invalid')
        except ValueError as error:
            assert message in str(error), error
        else:
            raise AssertionError(f'Accepted {arguments}')
        assert not (directory / 'invalid').exists()

print('Wine Mono package validation passed')
