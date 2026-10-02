"""Package Win32 and Win64 Wine Mono runtimes without development tools."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import tarfile
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

VERSION = '11.3.0'
ARCHIVE_SHA256 = '227cbeef943c71d9bbcbd9d00ea026decbc4ecfb758346f13d2a34a7fd8ecfcf'
ROOT = f'wine-mono-{VERSION}'
INSTALL_PATH = Path('switch/wine/drive_c/windows/mono/mono-2.0')
RUNTIME_ARCHES = ('x86', 'x86_64')
NATIVE_LIBRARIES = {
    'MonoPosixHelper.dll', 'libmono-btls-shared.dll', 'PresentationNative_cor3.dll',
    'wpfgfx_cor3.dll', 'wmwpfdwhelper.dll',
}
EXCLUDED_ASSEMBLIES = (
    'Microsoft.Build', 'Microsoft.CodeAnalysis', 'Microsoft.DirectX',
    'Microsoft.Xna.', 'WineMono.FNA', 'WineMono.XBuild',
)


def is_runtime_file(path):
    parts = path.parts
    if parts[0] == 'bin':
        return len(parts) == 2 and parts[1] in {f'libmono-2.0-{arch}.dll' for arch in RUNTIME_ARCHES}
    if parts[:2] == ('etc', 'mono'):
        return len(parts) >= 3 and parts[2] != 'mconfig'
    if parts[0] == 'lib' and len(parts) >= 2 and parts[1] in RUNTIME_ARCHES:
        return len(parts) == 3 and parts[2] in NATIVE_LIBRARIES
    if parts[:2] != ('lib', 'mono') or len(parts) < 4:
        return False
    if parts[2] not in ('gac', '4.5') or path.name.startswith(EXCLUDED_ASSEMBLIES):
        return False
    if parts[2] == '4.5' and len(parts) > 4 and parts[3] != 'Facades':
        return False
    return path.name.endswith(('.dll', '.dll.config'))


def stage_wine_mono(archive, destination):
    checksum = hashlib.sha256()
    with archive.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            checksum.update(chunk)
    if checksum.hexdigest() != ARCHIVE_SHA256:
        raise ValueError('Wine Mono archive checksum mismatch')

    with tarfile.open(archive, 'r:xz') as source:
        members = {}
        links = {}
        selected = {}
        seen = set()
        for member in source.getmembers():
            path = PurePosixPath(member.name)
            if (len(path.parts) < 2 or path.parts[0] != ROOT or
                    '\\' in member.name or ':' in member.name or
                    '..' in path.parts or member.name.startswith('/')):
                if member.name not in (ROOT, ROOT + '/'):
                    raise ValueError(f'Invalid Wine Mono path: {member.name}')
                continue
            if member.name in members:
                raise ValueError(f'Duplicate Wine Mono path: {member.name}')
            members[member.name] = member
            if member.issym():
                if '\\' in member.linkname or ':' in member.linkname or member.linkname.startswith('/'):
                    raise ValueError(f'Invalid Wine Mono link: {member.name}')
                link_target = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), member.linkname))
                if not link_target.startswith(ROOT + '/'):
                    raise ValueError(f'Invalid Wine Mono link: {member.name}')
                links[member.name] = link_target
            elif not (member.isfile() or member.isdir()):
                raise ValueError(f'Unsupported Wine Mono member: {member.name}')
            relative = PurePosixPath(*path.parts[1:])
            if member.isdir() or not is_runtime_file(relative):
                continue
            folded = relative.as_posix().casefold()
            if folded in seen:
                raise ValueError(f'Duplicate Wine Mono path: {member.name}')
            seen.add(folded)
            selected[relative] = member.name

        required = {f'bin/libmono-2.0-{arch}.dll' for arch in RUNTIME_ARCHES}
        required.update(('lib/mono/4.5/mscorlib.dll', 'etc/mono/config', 'etc/mono/4.5/machine.config'))
        if not required <= {path.as_posix() for path in selected}:
            raise ValueError('Wine Mono archive lacks required runtime files')
        for name in links:
            visited = set()
            original = name
            while original in links:
                if original in visited:
                    raise ValueError(f'Cyclic Wine Mono link: {name}')
                visited.add(original)
                original = links[original]
            if original not in members or not members[original].isfile():
                raise ValueError(f'Invalid Wine Mono link: {name}')
            links[name] = original

        destination.mkdir(parents=True, exist_ok=False)
        copies = {}
        for relative, name in selected.items():
            copies.setdefault(links.get(name, name), []).append(relative)
        for original in sorted(copies, key=lambda name: members[name].offset_data):
            paths = copies[original]
            target = destination.joinpath(*paths[0].parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(members[original]) as payload, target.open('wb') as output:
                shutil.copyfileobj(payload, output)
            for relative in paths[1:]:
                alias = destination.joinpath(*relative.parts)
                alias.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(target, alias)

    return {'version': VERSION, 'sha256': ARCHIVE_SHA256, 'arch': ['i386', 'amd64'], 'files': len(selected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, help=f'Official wine-mono-{VERSION}-arm64.tar.xz')
    parser.add_argument('--output', type=Path, required=True, help='SD-ready runtime ZIP')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wine-mono-') as temporary:
        stage = Path(temporary)
        result = stage_wine_mono(args.archive, stage / INSTALL_PATH)
        license_path = stage / 'switch/wine/licenses/WineMono.txt'
        license_path.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'licenses/WineMono.txt', license_path)
        files = sorted(path for path in stage.rglob('*') if path.is_file())
        size = sum(path.stat().st_size for path in files)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(args.output, 'w', ZIP_DEFLATED) as output:
            for path in files:
                output.write(path, path.relative_to(stage))
        with ZipFile(args.output) as output:
            if output.testzip():
                raise ValueError('Wine Mono archive integrity check failed')
        print(f"Wine Mono {VERSION}: {result['files']} runtime files, {size / 1048576:.1f} MiB installed")
        print(args.output)


if __name__ == '__main__':
    main()
