"""Read Mesa provenance from the SDK selected by the Switch build."""
import json
import os
from pathlib import Path
import re


def host_path(value):
    value = os.fspath(value).replace('\\', '/')
    match = re.fullmatch(r'(?:([A-Za-z]):|/(?:mnt/)?([A-Za-z]))(?:/(.*))?', value)
    if match:
        drive, tail = (match[1] or match[2]).lower(), match[3] or ''
        if os.name == 'nt':
            return Path(f'{drive.upper()}:/{tail}')
        if Path(f'/mnt/{drive}').is_dir():
            return Path(f'/mnt/{drive}/{tail}')
    return Path(value)


def mesa_revision(configured_lib_dir, build_revision_path):
    lib_dir = host_path(configured_lib_dir).resolve()
    if not lib_dir.is_dir():
        raise ValueError(f'Missing configured Mesa SDK library directory: {lib_dir}')
    manifest_path = lib_dir.parent / 'share/mesa-switch/manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if not isinstance(manifest, dict):
            raise ValueError('Invalid Mesa SDK manifest')
        revision = manifest.get('git_revision')
        dirty = manifest.get('source_dirty') is True
    else:
        build_revision_path = Path(build_revision_path)
        build_lib = build_revision_path.parent / 'install/opt/devkitpro/portlibs/switch/lib'
        if lib_dir != build_lib.resolve() or not build_revision_path.is_file():
            return None
        revision = build_revision_path.read_text().strip()
        dirty = False
    if not isinstance(revision, str) or not re.fullmatch(r'[0-9a-f]{40}(?:-dirty)?', revision):
        raise ValueError('Invalid Mesa SDK source revision')
    return revision + '-dirty' if dirty and not revision.endswith('-dirty') else revision
