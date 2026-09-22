#!/usr/bin/env python3
"""Exercise the AMD64 package dependency closure without running packaging."""
import ast
import functools
import json
from pathlib import Path
from pathlib import PurePosixPath
import re
import shutil
import tempfile
from types import SimpleNamespace
from zipfile import ZipFile


root = Path(__file__).resolve().parents[2]
package = root / 'wine-nx-probe/tools/package-amd64.py'
selected = {'module_name', 'apiset', 'import_host', 'coff_blocks', 'imports', 'forwarders',
            'stage_closure', 'validate_external_imports'}
tree = ast.parse(package.read_text(), filename=str(package))
game_runtime = next(ast.literal_eval(node.value) for node in tree.body
                    if isinstance(node, ast.Assign) and
                    any(isinstance(target, ast.Name) and target.id == 'game_runtime' for target in node.targets))
assert set(game_runtime) == {
    'cfgmgr32', 'dwmapi', 'msvcp140', 'normaliz', 'powrprof', 'vcruntime140', 'wldap32',
    'uiautomationcore', 'x3daudio1_7', 'xapofx1_5',
}
helpers = ast.Module(body=[node for node in tree.body
                           if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in selected],
                     type_ignores=[])
ast.fix_missing_locations(helpers)
namespace = {'functools': functools, 're': re}
exec(compile(helpers, str(package), 'exec'), namespace)
assert selected <= namespace.keys()


def import_dump(*blocks):
    result = []
    for module, symbols in blocks:
        result += ['Import {', f'  Name: {module}']
        result += [f'  Symbol: {name} ({ordinal})' for name, ordinal in symbols]
        result += ['}']
    return '\n'.join(result) + ('\n' if result else '')


def delay_import_dump(*blocks):
    result = []
    for module, symbols in blocks:
        result += ['DelayImport {', f'  Name: {module}', '  Attributes: 0x1']
        for name, ordinal in symbols:
            result += ['  Import {', f'    Symbol: {name} ({ordinal})',
                       '    Address: 0x180001000', '  }']
        result += ['}']
    return '\n'.join(result) + ('\n' if result else '')


def hybrid_import_dump(imports=(), delays=()):
    result = ['HybridObject {', '  Format: COFF-ARM64EC']
    for module, symbols in imports:
        result += ['  Import {', f'    Name: {module}']
        result += [f'    Symbol: {name} ({ordinal})' for name, ordinal in symbols]
        result += ['  }']
    for module, symbols in delays:
        result += ['  DelayImport {', f'    Name: {module}', '    Attributes: 0x1']
        for name, ordinal in symbols:
            result += ['    Import {', f'      Symbol: {name} ({ordinal})',
                       '      Address: 0x180001000', '    }']
        result += ['  }']
    result += ['}']
    return '\n'.join(result) + '\n'


def export_dump(*exports):
    result = []
    for name, ordinal, target in exports:
        result += ['Export {', f'  Name: {name}', f'  Ordinal: {ordinal}']
        if target is not None:
            result += [f'  ForwardedTo: {target}']
        result += ['}']
    return '\n'.join(result) + ('\n' if result else '')


def hybrid_export_dump(*exports):
    result = ['HybridObject {', '  Format: COFF-ARM64EC']
    for name, ordinal, target in exports:
        result += ['  Export {', f'    Ordinal: {ordinal}', f'    Name: {name}']
        if target is not None:
            result += [f'    ForwardedTo: {target}']
        result += ['  }']
    result += ['}']
    return '\n'.join(result) + '\n'


class Fixture:
    def __init__(self, base):
        self.base = Path(base)
        self.build_dir = self.base / 'build'
        self.stage = self.base / 'stage'
        self.objects = {}
        self.builds = []
        self.copies = []

    def add(self, arch, name, *, imports='', exports='', header=None):
        name = name.lower()
        if header is None:
            header = 'Arch: i386\n' if arch == 'i386' else 'Machine: IMAGE_FILE_MACHINE_ARM64\n'
        self.objects[arch, name] = {
            '--file-headers': header,
            '--coff-imports': imports,
            '--coff-exports': exports,
        }

    def built(self, name, arch):
        key = arch, name
        self.builds.append(key)
        if key not in self.objects:
            raise AssertionError(f'unexpected build: {key}')
        path = self.build_dir / arch / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
        return path

    def inspect(self, path, option):
        path = Path(path)
        return self.objects[path.parent.name, path.name][option]

    def copy2(self, source, destination):
        source, destination = Path(source), Path(destination)
        self.copies.append((source.parent.name, source.name, destination.name))
        destination.write_bytes(source.read_bytes())
        return destination

    def install(self):
        namespace['stage'] = self.stage
        namespace['built'] = self.built
        namespace['inspect'] = self.inspect
        namespace['shutil'] = SimpleNamespace(copy2=self.copy2)
        namespace['forwarders'].cache_clear()


assert namespace['module_name']('KeRnEl32') == 'kernel32.dll'
assert namespace['module_name']('WineNXAudio.DRV') == 'winenxaudio.drv'
assert namespace['module_name']('Already.DLL') == 'already.dll'

with tempfile.TemporaryDirectory(prefix='wine-nx-package-amd64-') as temp:
    fixture = Fixture(temp)
    fixture.add(
        'aarch64', 'root.dll',
        imports=import_dump(
            ('KeRnEl32.DLL', [('CreateFileW', 1)]),
            ('FoRwArD.DlL', [('Named', 2), ('', 7)]),
            ('DIRECT.DRV', [('DirectCall', 3)]),
            ('API-MS-Win-Core-Test-L1-1-0.DLL', [('ApiCall', 4)]),
            ('EXT-MS-Win-Test-L1-1-0.DLL', [('ExtCall', 5)]),
        ) + delay_import_dump(
            ('DeLaY.DLL', [('Delayed', 0), ('', 11)]),
        ) + hybrid_import_dump(
            imports=(('HyBrId.DLL', [('HybridOnly', 8)]),),
            delays=(('HyBrIdDeLaY.DLL', [('HybridDelayed', 0), ('', 13)]),),
        ),
    )
    fixture.add(
        'aarch64', 'kernel32.dll',
        exports=export_dump(
            ('CreateFileW', 1, 'KERNELBASE.CreateFileW'),
            ('Win16Thunk', 2, 'krnl386.exe16.LegacyCall'),
        ),
        header='Machine: IMAGE_FILE_MACHINE_AMD64\n',
    )
    fixture.add('aarch64', 'kernelbase.dll')
    fixture.add(
        'aarch64', 'forward.dll',
        exports=export_dump(
            ('Named', 2, 'MID.Step'),
            ('', 7, 'ORDINALHOP.#9'),
        ),
    )
    fixture.add('aarch64', 'mid.dll', exports=export_dump(('Step', 3, 'CYCLE.Back')))
    fixture.add('aarch64', 'cycle.dll', exports=export_dump(('Back', 4, 'MID.Step')))
    fixture.add('aarch64', 'ordinalhop.dll', exports=export_dump(('', 9, 'END.Final')))
    fixture.add('aarch64', 'end.dll')
    fixture.add('aarch64', 'direct.drv')
    fixture.add(
        'aarch64', 'delay.dll',
        exports=export_dump(
            ('Delayed', 1, 'DELAYTARGET.Real'),
            ('', 11, 'DELAYORDINAL.#12'),
        ),
    )
    fixture.add('aarch64', 'delaytarget.dll')
    fixture.add('aarch64', 'delayordinal.dll')
    fixture.add(
        'aarch64', 'hybrid.dll',
        exports=export_dump(('HybridOnly', 8, 'HYBRIDPRIMARY.Real')) +
                hybrid_export_dump(('HybridOnly', 8, 'HYBRIDTARGET.Real')),
    )
    fixture.add('aarch64', 'hybridprimary.dll')
    fixture.add('aarch64', 'hybridtarget.dll')
    fixture.add(
        'aarch64', 'hybriddelay.dll',
        exports=hybrid_export_dump(
            ('HybridDelayed', 1, 'HYBRIDDELAYTARGET.Real'),
            ('', 13, 'HYBRIDDELAYORDINAL.#14'),
        ),
    )
    fixture.add('aarch64', 'hybriddelaytarget.dll')
    fixture.add('aarch64', 'hybriddelayordinal.dll')
    fixture.install()

    root_path = fixture.built('root.dll', 'aarch64')
    parsed = list(namespace['imports'](root_path))
    assert parsed == [
        ('kernel32.dll', {'CreateFileW'}),
        ('forward.dll', {'Named', '#7'}),
        ('direct.drv', {'DirectCall'}),
        ('api-ms-win-core-test-l1-1-0.dll', {'ApiCall'}),
        ('ext-ms-win-test-l1-1-0.dll', {'ExtCall'}),
        ('delay.dll', {'Delayed', '#11'}),
        ('hybrid.dll', {'HybridOnly'}),
        ('hybriddelay.dll', {'HybridDelayed', '#13'}),
    ], parsed
    forwards = namespace['forwarders'](fixture.built('forward.dll', 'aarch64'))
    assert forwards == {
        '#2': {'MID.Step'}, 'Named': {'MID.Step'}, '#7': {'ORDINALHOP.#9'}
    }, forwards
    fixture.builds.clear()

    copied = namespace['stage_closure'](['RoOt'], 'aarch64', 'system32')
    expected = {
        'root.dll', 'kernel32.dll', 'kernelbase.dll', 'forward.dll', 'mid.dll',
        'cycle.dll', 'ordinalhop.dll', 'end.dll', 'direct.drv', 'delay.dll',
        'delaytarget.dll', 'delayordinal.dll', 'hybrid.dll', 'hybridprimary.dll',
        'hybridtarget.dll', 'hybriddelay.dll', 'hybriddelaytarget.dll',
        'hybriddelayordinal.dll',
    }
    assert copied == expected, (copied, expected)
    built_names = [name for _, name in fixture.builds]
    assert not any(name.startswith(('api-ms-', 'ext-ms-')) for name in built_names), built_names
    assert not any(name.startswith('krnl386') for name in built_names), built_names
    assert {name for _, name, _ in fixture.copies} == expected, fixture.copies

with tempfile.TemporaryDirectory(prefix='wine-nx-package-amd64-arch-') as temp:
    fixture = Fixture(temp)
    fixture.add('i386', 'wrong32.dll', header='Machine: IMAGE_FILE_MACHINE_ARM64\n')
    fixture.add('aarch64', 'wrong64.dll', header='Arch: i386\n')
    fixture.install()
    try:
        namespace['stage_closure'](['wrong32'], 'i386', 'syswow64')
    except ValueError as error:
        assert str(error).startswith('Not i386:'), error
    else:
        raise AssertionError('i386 staging accepted an ARM64 image')
    try:
        namespace['stage_closure'](['wrong64'], 'aarch64', 'system32')
    except ValueError as error:
        assert str(error).startswith('Not ARM64/ARM64EC:'), error
    else:
        raise AssertionError('ARM64 staging accepted an i386 image')

print('PASS: AMD64 dependency staging unions ARM64 and embedded ARM64EC normal and delayed imports '
      'through used named and ordinal forwarders, handles cycles and case, skips API sets and '
      'unused Win16 forwarders, and rejects mismatched architectures')

with tempfile.TemporaryDirectory(prefix='wine-nx-package-dxvk-') as temp:
    fixture = Fixture(temp)
    fixture.add('aarch64', 'dxgi.dll', imports=import_dump(
        ('API-MS-Win-Test-L1-1-0.DLL', [('Named', 0)])) + delay_import_dump(
        ('host.dll', [('', 7)])))
    fixture.add('aarch64', 'host.dll', exports=hybrid_export_dump(
        ('Named', 1, 'FORWARD.Real'), ('', 7, None)))
    fixture.add('aarch64', 'forward.dll', exports=export_dump(('Real', 3, None)))
    fixture.install()
    namespace['api_sets'] = {'api-ms-win-test-l1-1-0': 'host.dll'}
    paths = {name: fixture.built(name, 'aarch64') for name in ('dxgi.dll', 'host.dll', 'forward.dll')}
    namespace['validate_external_imports']([paths['dxgi.dll']], paths)

    def rejects_imports(message):
        namespace['forwarders'].cache_clear()
        try:
            namespace['validate_external_imports']([paths['dxgi.dll']], paths)
        except ValueError as error:
            assert message in str(error), error
        else:
            raise AssertionError('accepted an unresolved external import')

    fixture.objects['aarch64', 'forward.dll']['--coff-exports'] = export_dump(('Other', 3, None))
    rejects_imports('does not export Real')
    fixture.objects['aarch64', 'forward.dll']['--coff-exports'] = export_dump(('Real', 3, 'HOST.Named'))
    rejects_imports('Forwarder cycle')
    paths.pop('forward.dll')
    rejects_imports('Missing imported DLL')
    namespace['api_sets'] = {}
    rejects_imports('Unknown API set')

print('PASS: DXVK normal/delayed imports, API sets, ordinal and ARM64EC forwarded exports; '
      'missing symbols/modules and forwarder cycles are rejected')

autorun = root / 'wine-nx-probe/tools/package-autorun.py'
autorun_tree = ast.parse(autorun.read_text(), filename=str(autorun))
merge = next(node for node in autorun_tree.body
             if isinstance(node, ast.FunctionDef) and node.name == 'merge_amd64')
helpers = ast.Module(body=[merge], type_ignores=[])
ast.fix_missing_locations(helpers)
autorun_namespace = {
    'PurePosixPath': PurePosixPath,
    'ZipFile': ZipFile,
    'json': json,
    're': re,
    'shutil': shutil,
}
exec(compile(helpers, str(autorun), 'exec'), autorun_namespace)

with tempfile.TemporaryDirectory(prefix='autorun-amd64-merge-') as temp:
    temp = Path(temp)
    stage = temp / 'stage'
    runtime = stage / 'switch/wine'
    runtime.mkdir(parents=True)
    for name in ('run-entry.txt', 'target.txt', 'vulkan-probe.txt'):
        (runtime / name).write_text('keep\n')
    archive = temp / 'amd64.zip'
    def make_archive(fex, missing=None):
        manifest = {'features': {name: True for name in
                    ('amd64', 'dynarec', 'vulkan', 'dxvk', 'vkd3d', 'lsfg')}}
        manifest['features']['fex'] = fex
        marker = 'fex-2609' if fex else 'box64-3'
        with ZipFile(archive, 'w') as z:
            z.writestr('switch/wine/build-manifest.json', json.dumps(manifest))
            z.writestr('switch/wine/wine-nx-runtime.nro', f'NRO0 nx-amd64-{marker}\0'.encode())
            z.writestr('switch/wine/drive_c/windows/system32/winebox64ec.dll', b'cpu')
            if fex:
                for dll in ('libarm64ecfex.dll', 'libwow64fex.dll'):
                    if dll != missing:
                        z.writestr(f'switch/wine/drive_c/windows/system32/{dll}', b'fex')
            z.writestr('switch/wine/drive_c/dxvk64/dxgi.dll', b'dxvk')
            z.writestr('switch/wine/drive_c/vkd3d64/d3d12.dll', b'vkd3d')
            for name in ('run-entry.txt', 'target.txt', 'vulkan-probe.txt'):
                z.writestr(f'switch/wine/{name}', 'replace\n')

    for fex, expected in ((False, '3'), (True, 'fex-2609')):
        make_archive(fex)
        assert autorun_namespace['merge_amd64'](archive, stage) == expected
        assert (runtime / 'drive_c/dxvk64/dxgi.dll').read_bytes() == b'dxvk'
        if fex:
            for dll in ('libarm64ecfex.dll', 'libwow64fex.dll'):
                assert (runtime / 'drive_c/windows/system32' / dll).read_bytes() == b'fex'
        for name in ('run-entry.txt', 'target.txt', 'vulkan-probe.txt'):
            assert (runtime / name).read_text() == 'keep\n'

    for dll in ('libarm64ecfex.dll', 'libwow64fex.dll'):
        make_archive(True, missing=dll)
        try:
            autorun_namespace['merge_amd64'](archive, stage)
        except AssertionError as error:
            assert f'no FEX CPU module: {dll}' in str(error), error
        else:
            raise AssertionError(f'accepted a FEX package without {dll}')

print('PASS: the Autorun package merges the complete AMD64 graphics runtime without replacing package settings')
