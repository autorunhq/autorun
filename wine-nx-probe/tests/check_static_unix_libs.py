#!/usr/bin/env python3
"""Exercise the Horizon named Unix-library loader for native and WoW64 callers."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/virtual.c').read_text()


def function(name):
    start = source.index('static ', source.index(name) - 20)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


tables = []
for name in ('wine_nx_static_unix_libs', 'wine_nx_static_wow64_unix_libs'):
    at = source.index('} ' + name)
    tables.append(source[source.rfind('static const struct', 0, at):source.index('};', at) + 2])
symbols = sorted(set(re.findall(r'\bwine_nx_\w+_funcs\b', '\n'.join(tables))))
counts = sorted(set(re.findall(r'\bwine_nx_\w+_count\b', '\n'.join(tables))))
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef int NTSTATUS, BOOL;
typedef uint16_t WCHAR;
typedef uintptr_t UINT_PTR;
typedef uint64_t UINT64, unixlib_module_t;
typedef NTSTATUS (*unixlib_entry_t)(void *);
typedef struct { uint16_t Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
#define FALSE 0
#define TRUE 1
#define STATUS_SUCCESS 0
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xc0000135)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xc0000008)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define WINE_NX_FEX
#define WINE_NX_BOX64_INTERPRETER
#define WINE_NX_AMD64
#define WINE_NX_MESA_SWITCH
static int initialized, init_status;
static NTSTATUS wine_nx_win32u_unix_init(void) { initialized++; return init_status; }
'''
fixture += '\n'.join(f'static const unixlib_entry_t {name}[1] = {{0}};' for name in symbols)
fixture += '\n' + '\n'.join(f'static const unsigned int {name} = 1;' for name in counts)
fixture += '\n' + '\n'.join(tables)
fixture += '\n' + '\n'.join(function(name) for name in (
    'wine_nx_unix_name_equal', 'wine_nx_load_static_unix_lib', 'wine_nx_unload_static_unix_lib'))
fixture += r'''
static void check(const char *text, int wow, const void *funcs)
{
    size_t count = strlen(text);
    WCHAR name[count]; /* Deliberately not terminated. */
    UNICODE_STRING string = {count * 2, count * 2, name};
    UINT64 res[2] = {0}, again[2] = {0};
    for (size_t i = 0; i < count; i++) name[i] = text[i];
    assert(!wine_nx_load_static_unix_lib(&string, wow, res));
    assert(res[1] == (UINT_PTR)funcs);
    assert(!wine_nx_unload_static_unix_lib(res[0]));
    assert(wine_nx_unload_static_unix_lib(res[0] + 1) == STATUS_INVALID_HANDLE);
    assert(!wine_nx_load_static_unix_lib(&string, wow, again));
    assert(!memcmp(res, again, sizeof(res)));
    string.Length--;
    assert(wine_nx_load_static_unix_lib(&string, wow, res) == STATUS_DLL_NOT_FOUND);
    string.Length++;
    name[0] = 0x177;
    assert(wine_nx_load_static_unix_lib(&string, wow, res) == STATUS_DLL_NOT_FOUND);
}
int main(void)
{
    UINT64 res[2] = {0};
    WCHAR win32u[] = {'w','i','n','3','2','u','.','d','l','l'};
    UNICODE_STRING string = {sizeof(win32u), sizeof(win32u), win32u};
    for (unsigned i = 0; i < ARRAY_SIZE(wine_nx_static_unix_libs); i++)
        check(wine_nx_static_unix_libs[i].name, 0, wine_nx_static_unix_libs[i].funcs);
    for (unsigned i = 0; i < ARRAY_SIZE(wine_nx_static_wow64_unix_libs); i++)
        check(wine_nx_static_wow64_unix_libs[i].name, 1, wine_nx_static_wow64_unix_libs[i].funcs);
    check("WineNXAudio.DRV", 0, wine_nx_audio_unix_funcs);
    check("WINENXAUDIO.DRV", 1, wine_nx_audio_wow64_unix_funcs);
    assert(initialized == 2);
    init_status = -1;
    assert(wine_nx_load_static_unix_lib(&string, 0, res) == -1);
    assert(!res[0] && !res[1]);
    assert(wine_nx_load_static_unix_lib(&string, 1, res) == STATUS_DLL_NOT_FOUND);
    string.Buffer = NULL;
    assert(wine_nx_load_static_unix_lib(&string, 0, res) == STATUS_DLL_NOT_FOUND);
    assert(wine_nx_load_static_unix_lib(NULL, 0, res) == STATUS_DLL_NOT_FOUND);
    assert(wine_nx_unload_static_unix_lib(0) == STATUS_INVALID_HANDLE);
    assert(wine_nx_unload_static_unix_lib((UINT_PTR)wine_nx_audio_unix_funcs) == STATUS_INVALID_HANDLE);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'unixlib.c'
    binary = Path(tmp) / 'unixlib'
    path.write_text(fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-g', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Static native/WoW64 Unix-library loading passed')
