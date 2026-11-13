#!/usr/bin/env python3
"""Wine Mono on the card (horizon-wine/source/runtime.c): a program's
environment, which the runtime builds itself, had no WINEDATADIR, so mscoree
looked for Wine Mono only in C:\\windows\\mono\\mono-2.0. It now has the card's
share/wine there, and the runtime logs where it finds Wine Mono, by the
version mscoree asks for."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]

# mscoree's places, which the runtime's log follows.
metahost = (root / 'dlls/mscoree/metahost.c').read_text()
assert "{'\\\\','m','o','n','o','\\\\','m','o','n','o','-','2','.','0', 0}" in metahost
assert 'if ((data_dir = _wgetenv( L"WINEDATADIR" )))' in metahost and 'suffix = L"\\\\mono";' in metahost
assert 'static const WCHAR basedir[] = L"\\\\wine-mono-" WINE_MONO_VERSION;' in metahost
assert 'static const WCHAR libmono2_arch_dll[] = L"\\\\bin\\\\libmono-2.0-x86.dll";' in metahost
version = re.search(r'#define WINE_MONO_VERSION "([^"]+)"', (root / 'dlls/mscoree/mscoree_private.h').read_text()).group(1)

# WINEDATADIR on the card: the runtime builds a program's environment itself
# (runtime_environment), not through ntdll's add_dynamic_environment, so it is
# there, the card's share/wine as Z: names it, where ntdll's data_dir is.
runtime = (root / 'horizon-wine/source/runtime.c').read_text()
block = runtime[runtime.index('static const char runtime_environment[] ='):]
block = block[:block.index(';\n')]
assert '"WINEDATADIR=\\\\??\\\\Z:\\\\switch\\\\wine\\\\share\\\\wine\\0"' in block, block
assert block.index('"windir=') < block.index('"WINEDATADIR=') < block.index('"WINE_D3D_CONFIG=')
paths = (root / 'dlls/ntdll/unix/horizon_runtime_paths.h').read_text()
assert '#define WINE_NX_RUNTIME_DOS_ROOT "\\\\??\\\\Z:\\\\switch\\\\wine"' in paths
assert '#define WINE_NX_SHARED_ROOT "sdmc:/switch/wine"' in paths
loader = (root / 'dlls/ntdll/unix/loader.c').read_text()
assert 'data_dir = WINE_NX_RUNTIME_ROOT "/share/wine";' in loader
assert 'chars += sizeof(runtime_environment)' in runtime

# The runtime's log: the version mscoree asks for, both places, in its order.
assert f'#define RUNTIME_WINE_MONO_VERSION "{version}"' in runtime, version
mono = runtime[runtime.index('static void log_wine_mono( void )'):]
mono = mono[:mono.index('\n}\n')]
assert mono.index('RUNTIME_DIR "/drive_c/windows/mono/mono-2.0",') < \
    mono.index('RUNTIME_DIR "/share/wine/mono/wine-mono-" RUNTIME_WINE_MONO_VERSION,')
assert '"%s/bin/libmono-2.0-x86.dll"' in mono
assert runtime.index('    log_wine_mono();') < runtime.index('log_line( "[INIT] FPSLocker %s (fpslocker)%s"')
print(f'Wine Mono {version}: WINEDATADIR set on the card, and the runtime logs where Mono is')
