#!/usr/bin/env python3
"""Stage the Vulkan checkpoint as an overlay for a card that holds a full
package: the runtime linked with mesa-switch (build-switch-wow64-mesa-switch,
configured with -DWINE_NX_MESA_SWITCH_DIR; see build-mesa-switch.sh), Wine's
i386 vulkan-1.dll and winevulkan.dll, and pe32-vulkan.exe. The test maps Vulkan
memory from a 32-bit process and presents red, green and blue through a Win32
surface, so this says whether Windows programs reach NVK before DXVK is asked
to."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import argparse
import os
import re
import subprocess

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
nro = probe / 'build-switch-wow64-mesa-switch/wine-nx-runtime.nro'
base = build / 'full-sd-card/switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)

assert b'a Vulkan surface has the screen' in nro.read_bytes(), \
    f'{nro} has no Vulkan display driver; configure it with -DWINE_NX_MESA_SWITCH_DIR'
assert f'nx-wow64-dynarec-{marker}'.encode() + b'\0' in nro.read_bytes(), \
    f'{nro} is stale; rebuild the runtime for build {marker}'
assert (base / 'drive_c/windows/syswow64').is_dir(), f'{base} is not a full package; run package-wow64-full.py first'

def readobj(option, path):
    return subprocess.check_output([str(tools / 'llvm-readobj'), option, str(path)], text=True)

dlls = {}
for name in ('vulkan-1.dll', 'winevulkan.dll'):
    target = f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}'
    subprocess.run(['make', '-C', str(pe), '-j8', target, 'dlls/vulkan-1/i386-windows/libvulkan-1.a'], env=env, check=True)
    dlls[name] = pe / target

exe = pe / 'pe32-vulkan.exe'
subprocess.run([str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-fno-builtin',
                '-nostdlib', '-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
                # after mingw's own headers: only wine/vulkan.h comes from Wine's tree
                '-idirafter', str(probe.parent / 'include'),
                '-o', str(exe), str(probe / 'tests/pe32_vulkan.c'),
                '-L', str(pe / 'dlls/vulkan-1/i386-windows'),
                '-lvulkan-1', '-luser32', '-lkernel32', '-lntdll'], check=True, env=env)

# Everything these import must already be on a card with the full package.
present = {p.name.lower() for p in (base / 'drive_c/windows/syswow64').iterdir()} | set(dlls)
for path in (exe, *dlls.values()):
    assert 'Arch: i386\n' in readobj('--file-headers', path), f'{path.name} is not i386'
    imports = {n.lower() for n in re.findall(r'^  Name: (.+)$', readobj('--coff-imports', path), re.M)}
    missing = {n for n in imports - {'ntdll.dll'} if not n.startswith(('api-ms-', 'ext-ms-'))} - present
    assert not missing, f'{path.name} imports what the full package lacks: {missing}'

readme = f'''Wine-NX build {marker}: the Vulkan checkpoint, as an overlay for a card that
holds a full package. It replaces the runtime NRO with one linked with Mesa 26
(mesa-switch: OpenGL through nvc0, Vulkan through NVK) and adds Wine's Vulkan
(vulkan-1.dll and winevulkan.dll) and C:\\\\pe32-vulkan.exe.

It has not run on a Switch yet. Keep your current switch/wine/wine-nx-runtime.nro
somewhere else to go back, then copy the switch folder to the SD card, merging
folders.

Run C:\\\\pe32-vulkan.exe from the launcher. It creates a Vulkan instance and
device, maps cached and coherent Vulkan memory into its 32-bit address space,
checks remapping, then performs two CPU-GPU-CPU buffer copies through the
coherent mapping without explicit cache flush/invalidate calls. Look for
"coherent CPU-GPU-CPU copy passed" with values 1 and 2. It then shows red,
green and blue for a second each
through a Win32 surface, copying one pixel of every frame back from the GPU.
Its [VULKAN TEST] lines in autorun_runtime.log end with PASS and the program
exits with 0x2a; on a failure, "FAIL step" names the step. The screen should
turn red, green and blue.

Also in the log: [NXVK] lines at the start (vulkan-probe.txt holds 1), and
"[NXVK] hwnd ...: a Vulkan surface has the screen" when the test's surface takes
the screen from the compositor.
'''

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--name', default='vulkan', help='label in the zip name, new for each rebuild')
archive = build / f'wine-nx-{parser.parse_args().name}-overlay-dynarec-{marker}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    z.write(nro, 'switch/wine/wine-nx-runtime.nro')
    z.write(exe, 'switch/wine/drive_c/pe32-vulkan.exe')
    z.writestr('switch/wine/drive_c/pe32-vulkan.args.txt', '\n')
    for name, path in dlls.items():
        z.write(path, f'switch/wine/drive_c/windows/syswow64/{name}')
    z.writestr('switch/wine/vulkan-probe.txt', '1\n')
    z.writestr('switch/wine/VULKAN-README.txt', readme)
with ZipFile(archive) as z:
    assert z.testzip() is None
    print('\n'.join(f'{i.file_size:>10} {i.filename}' for i in z.infolist()))
print(f'{archive} ({archive.stat().st_size / 2**20:.1f} MiB)')
