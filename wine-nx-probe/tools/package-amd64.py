#!/usr/bin/env python3
"""Build and stage matching ARM64X and i386 payloads for the dual-architecture runtime."""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dxvk_payload import DLLS as DXVK_DLLS, validate_payload
from vkd3d_payload import DLLS as VKD3D_DLLS, validate_payload as validate_vkd3d_payload
from fex_payload import DLLS as FEX_DLLS, validate_payload as validate_fex_payload

probe = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pe', type=Path, default=probe / 'build-wine-amd64-pe')
parser.add_argument('--build', type=Path, default=probe / 'build-switch-amd64')
parser.add_argument('--jobs', type=int, default=8)
parser.add_argument('--no-build', action='store_true', help='Package existing DLLs without invoking make')
parser.add_argument('--minimal', action='store_true', help='Only console smoke-test dependencies')
parser.add_argument('--vulkan', action='store_true', help='Include Vulkan DLLs for a mesa-switch runtime')
parser.add_argument('--dxvk', type=Path, help='AMD64 payload produced by tools/build-dxvk.py (requires --vulkan)')
parser.add_argument('--vkd3d', type=Path, help='AMD64 payload produced by tools/build-vkd3d.py (requires --dxvk)')
parser.add_argument('--fex', type=Path, help='ARM64EC and WoW64 payload produced by build-fex.sh')
parser.add_argument('--interpreter-nro', type=Path, help='Include an interpreter-only diagnostic NRO')
args = parser.parse_args()
if args.vulkan and args.minimal:
    parser.error('--vulkan requires the full GUI package')
if args.dxvk and not args.vulkan:
    parser.error('--dxvk requires --vulkan')
if args.vkd3d and not args.dxvk:
    parser.error('--vkd3d requires --dxvk for DXGI')
dxvk_manifest = validate_payload(args.dxvk) if args.dxvk else None
vkd3d_manifest = validate_vkd3d_payload(args.vkd3d) if args.vkd3d else None
fex_manifest = validate_fex_payload(args.fex) if args.fex else None
pe, build = args.pe.resolve(), args.build.resolve()
env = os.environ.copy()
if env.get('WINE_NX_LLVM_MINGW'):
    env['PATH'] = str(Path(env['WINE_NX_LLVM_MINGW']) / 'bin') + os.pathsep + env['PATH']
readobj = shutil.which('llvm-readobj', path=env['PATH'])
cc64 = shutil.which('x86_64-w64-mingw32-clang', path=env['PATH'])
windres64 = shutil.which('x86_64-w64-mingw32-windres', path=env['PATH'])
if not readobj or not cc64 or not windres64 or not (pe / 'Makefile').is_file():
    parser.error('Configure the multi-architecture PE build and put LLVM-MinGW on PATH first.')
cache_path = build / 'CMakeCache.txt'
if not cache_path.is_file():
    parser.error('Missing Switch CMake build configuration')
cache = dict(re.findall(r'^([^#/:\n][^:\n]*):[^=\n]+=(.*)$', cache_path.read_text(), re.M))
enabled = lambda name: cache.get(name, '').upper() in ('ON', 'TRUE', 'YES', '1')
if not enabled('WINE_NX_AMD64'):
    parser.error('The NRO must be built with WINE_NX_AMD64=ON')
if bool(args.fex) != enabled('WINE_NX_FEX'):
    parser.error('--fex must match the NRO FEX build configuration')
if args.vulkan != bool(cache.get('WINE_NX_MESA_SWITCH_DIR')):
    parser.error('--vulkan must match the NRO mesa-switch build configuration')
nro = build / 'wine-nx-runtime.nro'
if not nro.is_file() or nro.read_bytes()[16:20] != b'NRO0':
    parser.error('Missing or invalid wine-nx-runtime.nro')
if args.fex and b'FEX-2609' not in nro.read_bytes():
    parser.error('The NRO has no FEX launch support; rebuild it first')
if args.vkd3d and b'[VKD3D] payload' not in nro.read_bytes():
    parser.error('The NRO has no VKD3D launch support; rebuild it first')
lsfg_revision = None
if enabled('WINE_NX_LSFG') and args.vulkan:
    lsfg_revision = (probe / 'lsfg/revision.txt').read_text().strip()
    if b'[LSFG]' not in nro.read_bytes():
        parser.error('The NRO has no LSFG-VK support; rebuild it first')
if args.interpreter_nro and (not args.interpreter_nro.is_file() or
                            args.interpreter_nro.read_bytes()[16:20] != b'NRO0'):
    parser.error('Invalid interpreter NRO')
mesa_revision = None
if args.vulkan:
    if b'a Vulkan surface has the screen' not in nro.read_bytes():
        parser.error('The NRO has no mesa-switch Vulkan display driver')
    mesa_revision_path = probe / 'build-mesa-switch/source-revision.txt'
    if not mesa_revision_path.is_file():
        parser.error('Missing mesa-switch source revision; rebuild it with build-mesa-switch.sh')
    mesa_revision = mesa_revision_path.read_text().strip()
    if not re.fullmatch(r'[0-9a-f]{40}', mesa_revision):
        parser.error('Invalid or dirty mesa-switch source revision')
staging = tempfile.TemporaryDirectory(prefix='amd64-package-', dir=build)
stage_root = Path(staging.name)
stage = stage_root / 'switch/wine'
prebuilt = set()


def run(command):
    subprocess.run(command, env=env, check=True)


@functools.lru_cache(None)
def inspect(path, option):
    return subprocess.check_output([readobj, option, str(path)], env=env, text=True)


def module_name(name):
    name = name.lower()
    return name if name.endswith(('.dll', '.drv')) else name + '.dll'


def apiset(name):
    return name.startswith(('api-ms-', 'ext-ms-'))


api_sets = dict(re.findall(r'^apiset (\S+) = (\S+)$',
                          (probe.parent / 'dlls/apisetschema/apisetschema.spec').read_text(), re.M))


def import_host(name):
    name = module_name(name)
    if not apiset(name):
        return name
    host = api_sets.get(name.removesuffix('.dll'))
    if not host:
        raise ValueError(f'Unknown API set: {name}')
    return module_name(host)


def coff_blocks(path, option, kinds):
    pattern = rf'^(?P<indent>(?:  )?)(?:{kinds}) \{{\n(?P<body>.*?)^(?P=indent)\}}'
    for match in re.finditer(pattern, inspect(path, option), re.M | re.S):
        indent = len(match['indent'])
        yield '\n'.join(line[indent:] for line in match['body'].splitlines())


def imports(path):
    for block in coff_blocks(path, '--coff-imports', 'Import|DelayImport'):
        name = re.search(r'^  Name: (.+)$', block, re.M).group(1).lower()
        symbols = {name or '#' + ordinal for name, ordinal in
                   re.findall(r'^ +Symbol: (.*?) \((\d+)\)$', block, re.M)}
        yield name, symbols


@functools.lru_cache(None)
def forwarders(path):
    result = {}
    for block in coff_blocks(path, '--coff-exports', 'Export'):
        target = re.search(r'^  ForwardedTo: (.+)$', block, re.M)
        if target:
            name = re.search(r'^  Name: (.*)$', block, re.M).group(1)
            ordinal = re.search(r'^  Ordinal: (\d+)$', block, re.M).group(1)
            result.setdefault('#' + ordinal, set()).add(target.group(1))
            if name:
                result.setdefault(name, set()).add(target.group(1))
    return result


def module_target(name, arch):
    if not re.fullmatch(r'[a-z0-9_.-]+\.(dll|drv)', name) or '..' in name:
        raise ValueError(f'Invalid module name: {name}')
    folder = name.removesuffix('.dll')
    return f'dlls/{folder}/{arch}-windows/{name}'


def prebuild(seeds, arch):
    names = [module_name(name) for name in seeds]
    if args.no_build:
        return
    run(['make', '-C', str(pe), f'-j{args.jobs}'] + [module_target(name, arch) for name in names])
    prebuilt.update((name, arch) for name in names)


@functools.lru_cache(None)
def built(name, arch):
    target = module_target(name, arch)
    if not args.no_build and (name, arch) not in prebuilt:
        run(['make', '-C', str(pe), f'-j{args.jobs}', target])
    path = pe / target
    if not path.is_file():
        raise ValueError(f'Missing built module: {path}')
    return path


def stage_closure(seeds, arch, directory):
    destination = stage / 'drive_c/windows' / directory
    destination.mkdir(parents=True, exist_ok=True)
    pending = [(module_name(name), set()) for name in seeds]
    copied = set()
    required = {}
    while pending:
        name, symbols = pending.pop()
        name = module_name(name)
        if apiset(name):
            continue
        path = built(name, arch)
        if name not in copied:
            info = inspect(path, '--file-headers')
            if arch == 'i386' and 'Arch: i386\n' not in info:
                raise ValueError(f'Not i386: {path}')
            if arch == 'aarch64' and 'IMAGE_FILE_MACHINE_ARM64' not in info and 'IMAGE_FILE_MACHINE_AMD64' not in info:
                raise ValueError(f'Not ARM64/ARM64EC: {path}')
            shutil.copy2(path, destination / name)
            copied.add(name)
            pending.extend(imports(path))
        unseen = symbols - required.setdefault(name, set())
        required[name].update(unseen)
        forwarded = forwarders(path)
        for symbol in unseen & forwarded.keys():
            for target in forwarded[symbol]:
                dependency, export = target.rsplit('.', 1)
                pending.append((module_name(dependency), {export}))
    return copied


def validate_external_imports(paths, modules):
    @functools.lru_cache(None)
    def exports(path):
        result = set()
        for block in coff_blocks(path, '--coff-exports', 'Export'):
            name = re.search(r'^  Name: (.*)$', block, re.M)
            ordinal = re.search(r'^  Ordinal: (\d+)$', block, re.M)
            if name and name.group(1):
                result.add(name.group(1))
            if ordinal:
                result.add('#' + ordinal.group(1))
        return result

    def resolve(name, symbol, chain=()):
        name = import_host(name)
        if name not in modules:
            raise ValueError(f'Missing imported DLL: {name}')
        path = modules[name]
        key = (name, symbol)
        if key in chain:
            raise ValueError(f'Forwarder cycle: {chain + (key,)}')
        if symbol not in exports(path):
            raise ValueError(f'{name} does not export {symbol}')
        for target in forwarders(path).get(symbol, ()):
            dependency, export = target.rsplit('.', 1)
            resolve(dependency, export, chain + (key,))

    for path in paths:
        for name, symbols in imports(path):
            for symbol in symbols:
                try:
                    resolve(name, symbol)
                except ValueError as error:
                    raise ValueError(f'{path.name}: {error}') from error


game_runtime = (
    'cfgmgr32', 'dwmapi', 'msvcp140', 'normaliz', 'powrprof', 'vcruntime140', 'wldap32',
    'uiautomationcore', 'x3daudio1_7', 'xapofx1_5',
)
common = 'ntdll kernel32 kernelbase msvcrt ucrtbase advapi32 sechost'.split()
dxvk_paths = [args.dxvk / name for name in DXVK_DLLS] if args.dxvk else []
vkd3d_paths = [args.vkd3d / name for name in VKD3D_DLLS] if args.vkd3d else []
if args.vulkan:
    common += ['vulkan-1', 'winevulkan']
if not args.minimal:
    common += ('user32 win32u gdi32 imm32 ole32 oleaut32 combase coml2 rpcrt4 shell32 '
               'comdlg32 comctl32 shlwapi shcore version ws2_32 winmm mmdevapi avrt '
               'dsound opengl32 wined3d d3d9 d3d11 dxgi dinput8 xinput1_3 xinput1_4 '
               'xinput9_1_0 dbghelp windowscodecs '
               'd3dx9_38 d3dx9_43 winhttp oleacc wsock32 psapi').split()
    common += game_runtime
native_seeds = common + ['winebox64', 'winebox64ec', 'wow64', 'wow64win', 'apisetschema']
if args.dxvk:
    native_seeds += ['d3d10', 'd3d10_1', 'd3dcompiler_43', 'd3dcompiler_47']
    native_seeds += sorted({import_host(name) for path in dxvk_paths for name, symbols in imports(path)
                            if module_name(name) not in DXVK_DLLS})
if args.vkd3d:
    native_seeds += sorted({import_host(name) for path in vkd3d_paths for name, symbols in imports(path)
                            if module_name(name) not in DXVK_DLLS + VKD3D_DLLS})
prebuild(native_seeds, 'aarch64')
native = stage_closure(native_seeds, 'aarch64', 'system32')
if args.fex:
    destination = stage / 'drive_c/windows/system32'
    for name in FEX_DLLS:
        shutil.copy2(args.fex / name, destination / name)
    validate_external_imports([destination / name for name in FEX_DLLS],
                              {name: destination / name for name in native})
    native.update(FEX_DLLS)
prebuild(common, 'i386')
guest = stage_closure(common, 'i386', 'syswow64')
if not args.minimal:
    for compiler, directory, entry, modules in (
            ('x86_64', 'system32', 'DllMain', native),
            ('i686', 'syswow64', '_DllMain@12', guest)):
        driver = stage / 'drive_c/windows' / directory / 'winenxaudio.drv'
        run([f'{compiler}-w64-mingw32-clang', '-Os', '-Wall', '-Wextra', '-Werror',
             '-fno-builtin', '-nostdlib', '-shared', f'-Wl,--entry,{entry}', '-Wl,--dynamicbase',
             '-o', str(driver), str(probe / 'source/audio_driver.c')])
        if b'winenxaudio.drv\0' not in driver.read_bytes():
            raise ValueError('Audio driver has no module identity')
        modules.add('winenxaudio.drv')

drive = stage / 'drive_c'
run(['x86_64-w64-mingw32-clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-msse2',
     str(probe / 'tests/pe64_smoke.c'), '-o', str(drive / 'pe64-smoke.exe')])

win64 = drive / 'win64-tests'
win64.mkdir()
win64_tests = []
pe64_flags = [cc64, '-Os', '-Wall', '-Wextra', '-Werror', '-fno-builtin', '-nostdlib',
              '-Wl,--entry,start', '-Wl,--image-base,0x140000000', '-Wl,--dynamicbase']


def build_pe64(name, source, libraries, extra=()):
    output = win64 / f'pe64-{name}.exe'
    run(pe64_flags + list(extra) + [str(probe / f'tests/{source}'), '-o', str(output)] +
        [f'-l{library}' for library in libraries])
    win64_tests.append(output.name)


for test in ('functional', 'threads', 'lifecycle'):
    build_pe64(test, f'pe32_{test}.c', ('kernel32', 'ntdll'))
if not args.minimal:
    for test in ('messages', 'timers'):
        build_pe64(test, f'pe32_{test}.c', ('user32', 'kernel32', 'ntdll'))
    resource = stage_root / 'pe64-video-startup.res.o'
    run([windres64, '-I', str(probe.parent), str(probe / 'tests/pe32_video_startup.rc'), str(resource)])
    output = win64 / 'pe64-video-startup.exe'
    run(pe64_flags + [str(probe / 'tests/pe32_video_startup.c'), str(resource), '-o', str(output),
                      '-luser32', '-lkernel32', '-lntdll'])
    win64_tests.append(output.name)
    build_pe64('section', 'pe32_section.c', ('kernel32', 'ntdll'))
    build_pe64('wasapi', 'pe32_wasapi.c', ('ole32', 'user32', 'kernel32', 'ntdll'))
    build_pe64('audio', 'pe32_audio.c', ('winmm', 'kernel32', 'ntdll'))
    build_pe64('opengl', 'pe32_opengl.c', ('opengl32', 'gdi32', 'user32', 'kernel32', 'ntdll'))
    build_pe64('d3d9', 'pe32_d3d9.c', ('d3d9', 'gdi32', 'user32', 'kernel32', 'ntdll'))
if args.vulkan:
    imports64 = stage_root / 'imports64'
    imports64.mkdir()
    winebuild = pe / 'tools/winebuild/winebuild'
    if not winebuild.is_file():
        raise ValueError(f'Missing configured winebuild: {winebuild}')
    run([str(winebuild), '-w', '--implib', '-o', str(imports64 / 'libvulkan-1.a'),
         '-b', 'x86_64-windows', '--export', str(probe.parent / 'dlls/vulkan-1/vulkan-1.spec')])
    build_pe64('vulkan', 'pe32_vulkan.c', ('vulkan-1', 'user32', 'kernel32', 'ntdll'),
               ('-Wno-missing-field-initializers', '-idirafter', str(probe.parent / 'include'),
                '-L', str(imports64)))
if args.dxvk:
    destination = drive / 'dxvk64'
    destination.mkdir()
    for path in dxvk_paths:
        shutil.copy2(path, destination / path.name)
    shutil.copy2(args.dxvk / 'dxvk-manifest.json', destination / 'dxvk-manifest.json')
    build_pe64('dxvk-d3d9', 'pe32_d3d9.c', ('d3d9', 'gdi32', 'user32', 'kernel32', 'ntdll'))
    libraries = ('d3d11', 'dxgi', 'd3dcompiler_47', 'dxguid', 'user32', 'kernel32', 'ntdll')
    build_pe64('dxvk-d3d11', 'pe64_d3d11.c', libraries, ('-DTEST_REQUIRE_DXVK=1',))
    build_pe64('dxvk-d3d11-fullscreen', 'pe64_d3d11.c', libraries,
               ('-DTEST_REQUIRE_DXVK=1', '-DTEST_FULLSCREEN=1', '-DTEST_WIDTH=800', '-DTEST_HEIGHT=600'))
    for name in ('dxvk-d3d9', 'dxvk-d3d11', 'dxvk-d3d11-fullscreen'):
        (win64 / f'pe64-{name}.wine-nx.txt').write_text('d3d=dxvk\n')
    (destination / 'DarkSoulsII.wine-nx.txt').write_text('d3d=dxvk\n')
    shutil.copy2(probe / 'DXVK.md', stage / 'DXVK-README.md')
if args.vkd3d:
    destination = drive / 'vkd3d64'
    destination.mkdir()
    for path in vkd3d_paths:
        shutil.copy2(path, destination / path.name)
    shutil.copy2(args.vkd3d / 'vkd3d-manifest.json', destination / 'vkd3d-manifest.json')
for test in ('smoke', 'functional', 'threads', 'lifecycle'):
    run(['i686-w64-mingw32-clang', '-Os', '-fno-builtin', '-nostdlib', '-Wl,--entry,_start@0',
         '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
         str(probe / f'tests/pe32_{test}.c'), '-o', str(drive / f'pe32-{test}.exe'), '-lkernel32', '-lntdll'])
for name in ('fonts', 'nls'):
    destination = stage / 'share/wine' / name
    destination.mkdir(parents=True, exist_ok=True)
    extension = '*.ttf' if name == 'fonts' else '*.nls'
    resources = list((probe.parent / name).glob(extension))
    if not resources:
        raise ValueError(f'Missing Wine {name} resources')
    for path in resources:
        shutil.copy2(path, destination / path.name)
        if name == 'fonts':
            (drive / 'windows/fonts').mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, drive / 'windows/fonts' / path.name)
shutil.copy2(nro, stage / 'wine-nx-runtime.nro')
if args.interpreter_nro:
    shutil.copy2(args.interpreter_nro, stage / 'wine-nx-runtime-interpreter.nro')
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/win64-tests/pe64-functional.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
if args.vulkan:
    (stage / 'vulkan-probe.txt').write_text('1\n')
shutil.copy2(probe / 'AMD64.md', stage / 'AMD64-README.md')
licenses = stage / 'licenses'
licenses.mkdir()
if args.fex:
    shutil.copy2(args.fex / 'fex-manifest.json', stage / 'fex-manifest.json')
    for name in fex_manifest['licenses']:
        shutil.copy2(args.fex / 'licenses' / name, licenses / name)
if lsfg_revision:
    shutil.copy2(probe / 'vendor/lsfg-vk/LICENSE.md', licenses / 'LSFG-VK-GPL-3.0.txt')
    shutil.copy2(probe / 'lsfg/README.md', stage / 'LSFG-README.md')
    (stage / 'lsfg').mkdir()
for source, name in ((probe.parent / 'COPYING.LIB', 'Wine-LGPL-2.1.txt'),
                     (probe / 'vendor/box64/LICENSE', 'Box64-MIT.txt'),
                     (probe.parent / 'dlls/winebox64ec/LICENSE.FEX', 'FEX-MIT.txt')):
    shutil.copy2(source, licenses / name)
if args.dxvk:
    for name in dxvk_manifest['licenses']:
        shutil.copy2(args.dxvk / 'licenses' / name, licenses / name)
    modules = {path.name: path for path in (drive / 'windows/system32').iterdir()}
    modules.update({path.name: path for path in dxvk_paths})
    validate_external_imports(dxvk_paths + sorted(win64.glob('pe64-dxvk-*.exe')), modules)
if args.vkd3d:
    for name in vkd3d_manifest['licenses']:
        shutil.copy2(args.vkd3d / 'licenses' / name, licenses / name)
    modules.update({path.name: path for path in vkd3d_paths})
    validate_external_imports(vkd3d_paths, modules)

for directory, modules in (('system32', native), ('syswow64', guest)):
    for name in modules:
        path = stage / 'drive_c/windows' / directory / name
        missing = [dep for dep, symbols in imports(path) if not apiset(dep) and dep not in modules]
        if missing:
            raise ValueError(f'{path}: missing {missing}')
for path in drive.rglob('pe*.exe'):
    headers = inspect(path, '--file-headers')
    if path.name.startswith('pe32-') and 'Arch: i386\n' not in headers:
        raise ValueError(f'Not i386: {path}')
    if path.name.startswith('pe64-') and 'Arch: x86_64\n' not in headers:
        raise ValueError(f'Not AMD64: {path}')
    modules = guest if 'Arch: i386\n' in headers else native
    missing = [dep for dep, symbols in imports(path) if not apiset(dep) and dep not in modules]
    if missing:
        raise ValueError(f'{path}: missing {missing}')
tls = inspect(win64 / 'pe64-lifecycle.exe', '--coff-tls-directory')
if not re.search(r'AddressOfCallBacks: 0x[1-9a-fA-F][0-9a-fA-F]*', tls):
    raise ValueError('pe64-lifecycle.exe has no TLS callbacks')
if 'Type: DIR64' not in inspect(win64 / 'pe64-lifecycle.exe', '--coff-basereloc'):
    raise ValueError('pe64-lifecycle.exe has no 64-bit base relocations')
audio_driver = stage / 'drive_c/windows/system32/winenxaudio.drv'
if not args.minimal and 'Arch: x86_64\n' not in inspect(audio_driver, '--file-headers'):
    raise ValueError('The native audio driver is not AMD64')
for name in ('ntdll', 'kernel32', 'kernelbase'):
    info = inspect(stage / f'drive_c/windows/system32/{name}.dll', '--coff-load-config')
    if not re.search(r'CHPEMetadataPointer: 0x[1-9a-fA-F][0-9a-fA-F]*', info):
        raise ValueError(f'{name}.dll has no ARM64X metadata')
cpu = stage / 'drive_c/windows/system32/winebox64ec.dll'
exports = set(re.findall(r'^  Name: (.+)$', inspect(cpu, '--coff-exports'), re.M))
required = set(re.findall(r'^@ (?:stdcall|extern) (\w+)',
                          (probe.parent / 'dlls/winebox64ec/winebox64ec.spec').read_text(), re.M))
if not required <= exports:
    raise ValueError(f'CPU64 exports missing: {required - exports}')
files = sorted(path for path in stage.rglob('*') if path.is_file() and
               path.suffix != '.log' and path.name != 'build-manifest.json')
manifest = {
    'box64': '2f130fab1d6e1a4ee8a71dc60cfdfcc839ad192a',
    'wine': subprocess.check_output(['git', '-C', str(probe.parent), 'rev-parse', 'HEAD'], text=True).strip(),
    'hardware_verified': False,
    'features': {'amd64': True, 'dynarec': enabled('WINE_NX_BOX64_DYNAREC'),
                 'vulkan': args.vulkan, 'dxvk': bool(args.dxvk), 'vkd3d': bool(args.vkd3d),
                 'lsfg': bool(lsfg_revision), 'fex': bool(args.fex),
                 'interpreter_fallback': bool(args.interpreter_nro)},
    'mesa_switch': mesa_revision,
    'dxvk': dxvk_manifest,
    'vkd3d': vkd3d_manifest,
    'fex': fex_manifest,
    'lsfg': {'repository': 'https://git.lsfg-vk.dev/lsfg-vk-archive.git',
             'revision': lsfg_revision,
             'patch_sha256': hashlib.sha256((probe / 'lsfg/horizon.patch').read_bytes()).hexdigest()}
            if lsfg_revision else None,
    'validation': {'default': 'win64-tests/pe64-functional.exe',
                   'win64': ['pe64-smoke.exe'] + win64_tests},
    'files': {str(path.relative_to(stage)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
}
(stage / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
archive = build / ('wine-nx-amd64-box64-mesa-dxvk-vkd3d.zip' if args.vkd3d else
                   'wine-nx-amd64-box64-mesa-dxvk.zip' if args.dxvk else
                   'wine-nx-amd64-box64-mesa-vulkan.zip' if args.vulkan else 'wine-nx-amd64-box64.zip')
if args.fex:
    archive = archive.with_name(archive.name.replace('-box64', '-box64-fex'))
with ZipFile(archive, 'w', ZIP_DEFLATED) as output:
    for path in sorted(stage.rglob('*')):
        if path.is_file() and path.suffix != '.log':
            output.write(path, path.relative_to(stage_root))
with ZipFile(archive) as output:
    if output.testzip() is not None:
        raise ValueError('Archive integrity check failed')
print(archive)
staging.cleanup()
