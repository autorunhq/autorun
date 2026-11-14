#!/bin/sh
# The launcher's DLL manager (source/horizon_dlls.c) against the DLL repository
# checkout in horizon-dlls/, with a folder standing in for GitHub: a card
# brought up from nothing, stopped and resumed, damaged and verified, updated
# when the repository changes and drops files, and refusing a bad download.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
horizon_wine="$root/horizon-wine"
repo="$root/horizon-dlls"
[ -f "$repo/switch/wine/horizon-dlls/manifest.json" ] || { echo "no DLL repository in $repo" >&2; exit 1; }
build="$(mktemp -d "${TMPDIR:-/tmp}/horizon-dlls.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
# OpenSSL's SHA-256 for horizon_dlls.c on Linux; macOS has it in CommonCrypto.
crypto=""; [ "$(uname -s)" = Linux ] && crypto=-lcrypto

python3 "$horizon_wine/tools/runtime_features.py" > "$build/features.txt"
clang -std=gnu11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$horizon_wine/source" \
    -I/opt/homebrew/include "$horizon_wine/tests/horizon_dlls.c" "$horizon_wine/source/horizon_dlls.c" -L/opt/homebrew/lib -lzstd -lz $crypto \
    -o "$build/horizon_dlls"

# Three states of the repository: a part of it (1), then with d3d9.dll changed
# and dsound.dll and xinput1_3.dll gone (2), then naming a hash quartz.dll does
# not have (3).
python3 - "$repo" "$build" <<'PY'
import hashlib, json, pathlib, shutil, sys, zlib
repo, build = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
manifest = json.loads((repo / 'switch/wine/horizon-dlls/manifest.json').read_text())
wanted = {
    'drive_c/windows/system32': {'ntdll.dll', 'wow64.dll', 'wow64win.dll', 'winebox64.dll', 'win32u.dll',
                                 'apisetschema.dll', 'kernel32.dll', 'kernelbase.dll'},
    'drive_c/windows/syswow64': {'ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'msvcrt.dll', 'ucrtbase.dll',
                                 'quartz.dll', 'devenum.dll', 'd3d8.dll', 'd3d9.dll', 'dsound.dll',
                                 'xinput1_3.dll', 'msxml3.dll', 'xaudio2_7.dll', 'winenxaudio.drv',
                                 'physxloader.dll', 'physxdevice.dll', 'cudart32_41_22.dll', 'xlive.dll'},
    'drive_c/dxvk64': {'dxgi.dll', 'dxvk-manifest.json'},
}
for sdk in ('2.7.1', '2.7.2', '2.7.3', '2.7.4', '2.7.5', '2.7.6', '2.8.0', '2.8.1', '2.8.3'):
    wanted[f'drive_c/physx/Engine/v{sdk}'] = {'physxcore.dll', 'physxcooking.dll'}
manifest['files'] = [f for f in manifest['files'] if f['name'] in wanted.get(f['path'], ())]
assert len(manifest['files']) == sum(map(len, wanted.values())), len(manifest['files'])
assert all(f.get('compressed') for f in manifest['files']), 'the repository has no compressed copies'
# One file as an older manifest had it, with no compressed copy: the file
# itself is downloaded.
for f in manifest['files']:
    if f['name'] == 'msvcrt.dll':
        del f['compressed']

def serve(name, m, files={}):
    card = build / name / 'switch/wine'
    (card / 'horizon-dlls').mkdir(parents=True)
    (card / 'horizon-dlls/manifest.json').write_text(json.dumps(m, indent=1))
    for path, data in files.items():
        (build / name / path).parent.mkdir(parents=True, exist_ok=True)
        (build / name / path).write_bytes(data)

serve('served1', manifest)
# The release: the same files as one tar compressed with zstd, as
# tools/make-release.py writes it, but with quartz.dll as another version, as
# when main has moved on since.
import io, subprocess, tarfile
release = build / 'served1/releases/latest/download'
release.mkdir(parents=True)
with tarfile.open(release / 'horizon-dlls.tar', 'w', format=tarfile.USTAR_FORMAT) as t:
    for f in manifest['files']:
        data = (repo / 'switch/wine' / f['path'] / f['name']).read_bytes()
        if f['name'] == 'quartz.dll' and f['path'].endswith('syswow64'):
            data = data[:-1] + bytes([data[-1] ^ 1])
        info = tarfile.TarInfo(f"switch/wine/{f['path']}/{f['name']}")
        info.size = len(data)
        t.addfile(info, io.BytesIO(data))
    info = tarfile.TarInfo('switch/wine/horizon-dlls')
    info.type = tarfile.DIRTYPE
    t.addfile(info)
subprocess.run(['zstd', '-q', '-19', '--long=27', '--rm', '-f', str(release / 'horizon-dlls.tar')], check=True)
changed = (repo / 'switch/wine/drive_c/windows/syswow64/d3d8.dll').read_bytes()
m2 = json.loads(json.dumps(manifest))
m2['files'] = [f for f in m2['files'] if not (f['path'].endswith('syswow64') and f['name'] in ('dsound.dll', 'xinput1_3.dll'))]
packed = zlib.compress(changed, 9)
for f in m2['files']:
    if f['path'].endswith('syswow64') and f['name'] == 'd3d9.dll':
        f.update(size=len(changed), sha256=hashlib.sha256(changed).hexdigest(), version=f['version'] + 1)
        f['compressed'].update(size=len(packed), sha256=hashlib.sha256(packed).hexdigest())
files = {'switch/wine/drive_c/windows/syswow64/d3d9.dll': changed,
         'compressed/switch/wine/drive_c/windows/syswow64/d3d9.dll.z': packed}
serve('served2', m2, files)
m3 = json.loads(json.dumps(m2))
for f in m3['files']:
    if f['path'].endswith('syswow64') and f['name'] == 'quartz.dll':
        f['sha256'] = '0' * 64
# And a compressed copy that is not what the manifest says it is.
for f in m3['files']:
    if f['path'].endswith('syswow64') and f['name'] == 'devenum.dll':
        f['sha256'] = '0' * 64
        f['compressed']['sha256'] = '1' * 64
serve('served3', m3, files)
PY

# A card brought up from the release, then what it did not bring.
mkdir -p "$build/release-card"
"$build/horizon_dlls" release "$repo" "$build/served1" "$build/features.txt" "$build/release-card"

card="$build/card"
mkdir -p "$card"
"$build/horizon_dlls" install "$repo" "$build/served1" "$build/features.txt" "$card"
# The player puts their own xinput1_3.dll in place of the repository's.
printf 'mine' >> "$card/drive_c/windows/syswow64/xinput1_3.dll"
"$build/horizon_dlls" update "$repo" "$build/served2" "$build/features.txt" "$card"
"$build/horizon_dlls" damaged "$repo" "$build/served3" "$build/features.txt" "$card"
echo "horizon-dlls: release, install, resume, verify, adopt, update, removal and damaged download passed"
