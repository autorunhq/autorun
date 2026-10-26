#!/usr/bin/env python3
"""Exercise FEX unixlib lookup/unload using the actual tables and code in virtual.c.

No Switch or PE compiler is needed. Only the unixlib implementations and current
TEB are stubbed; Wine's types, tables, name lookup, module-list lookup and unload
are the real ones. Test with/without FEX and the other optional CPU/GPU modules.
Pass --source to run the same regression cases against an older virtual.c.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'dlls/ntdll/unix/virtual.c')
args = parser.parse_args()
source = args.source.read_text()


def table(name):
    at = source.index('} ' + name + '[]')
    start = source.rfind('static const struct\n', 0, at)
    end = source.index('\n};', at) + len('\n};')
    return source[start:end]


# Include the #ifdefs surrounding optional entries (and the FEX variant table).
native = table('wine_nx_static_unix_libs')
if '} wine_nx_variant_unix_libs[]' in source:
    native += '\n#ifdef WINE_NX_FEX\n' + table('wine_nx_variant_unix_libs') + '\n#endif\n'
tables = native + '\n' + table('wine_nx_static_wow64_unix_libs')
start = source.index('static BOOL wine_nx_unix_name_equal(')
end = source.index('\n#endif\n\n/***********************************************************************\n *           load_builtin_unixlib', start)
helpers = source[start:end]

# Supply only the implementations the real table refers to. Their distinct
# addresses let the tests check which table (native vs WoW64) was returned.
stubs = []
for name in sorted(set(re.findall(r'\b(wine_nx_\w+_unix_funcs)\b', tables))):
    definition = f'const unixlib_entry_t {name}[] = {{ test_unix_call }};'
    if name == 'wine_nx_fex_unix_funcs':
        definition = '#ifdef WINE_NX_FEX\n' + definition + '\n#endif'
    stubs.append(definition)
for name in sorted(set(re.findall(r'&(wine_nx_\w+_unix_count)\b', tables))):
    stubs.append(f'const unsigned int {name} = 1;')

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "horizon_unixlib_names.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ERR(...) fprintf( stderr, __VA_ARGS__ )

static TEB test_teb;
#define NtCurrentTeb() (&test_teb)
static NTSTATUS init_status;
static unsigned int init_calls;
static NTSTATUS test_unix_call( void *args ) { (void)args; return STATUS_SUCCESS; }
static NTSTATUS wine_nx_win32u_unix_init(void) { init_calls++; return init_status; }
@STUBS@
@TABLES@
@HELPERS@

/* Deliberately not NUL terminated: UNICODE_STRING.Length is authoritative. */
static UNICODE_STRING make_name( const char *text )
{
    UNICODE_STRING name;
    size_t i, length = strlen( text );
    name.Length = name.MaximumLength = length * sizeof(WCHAR);
    name.Buffer = malloc( (length ? length : 1) * sizeof(WCHAR) );
    assert( name.Buffer );
    for (i = 0; i < length; i++) name.Buffer[i] = (unsigned char)text[i];
    return name;
}

static NTSTATUS load_name( const char *text, BOOL wow, UINT64 result[2] )
{
    UNICODE_STRING name = make_name( text );
    NTSTATUS status = wine_nx_load_static_unix_lib( &name, wow, result );
    free( name.Buffer );
    return status;
}

static void expect_missing( const char *text, BOOL wow )
{
    UINT64 result[2] = { 123, 456 };
    assert( load_name( text, wow, result ) == STATUS_DLL_NOT_FOUND );
    assert( result[0] == 123 && result[1] == 456 );
}

static void expect_match( const char *text, BOOL wow, const unixlib_entry_t *funcs )
{
    UINT64 result[2] = { 0, 0 };
    assert( load_name( text, wow, result ) == STATUS_SUCCESS );
    assert( result[0] && result[1] == (UINT_PTR)funcs );
    assert( wine_nx_unload_static_unix_lib( result[0] ) == STATUS_SUCCESS );
}

/* This is MemoryWineUnixFuncs' native path: the loader's BaseDllName, not
 * the (unchanged by the payload rename) PE export-directory name. */
static void check_module( const char *text, BOOL fex )
{
    PEB peb = {0};
    PEB_LDR_DATA ldr = {0};
    LDR_DATA_TABLE_ENTRY module = {0};
    const void *funcs = NULL;
    module.BaseDllName = make_name( text );
    module.DllBase = &module;
    InitializeListHead( &ldr.InLoadOrderModuleList );
    InsertTailList( &ldr.InLoadOrderModuleList, &module.InLoadOrderLinks );
    peb.LdrData = &ldr;
    test_teb.Peb = &peb;
    if (fex)
    {
#ifdef WINE_NX_FEX
        assert( wine_nx_lookup_static_unix_funcs( module.DllBase, &funcs ) == STATUS_SUCCESS );
        assert( funcs == wine_nx_fex_unix_funcs );
#endif
    }
    else
        assert( wine_nx_lookup_static_unix_funcs( module.DllBase, &funcs ) == STATUS_DLL_NOT_FOUND );
    funcs = (void *)(UINT_PTR)123;
    assert( wine_nx_lookup_static_unix_funcs( &peb, &funcs ) == STATUS_DLL_NOT_FOUND );
    assert( funcs == (void *)(UINT_PTR)123 );
    peb.LdrData = NULL;
    assert( wine_nx_lookup_static_unix_funcs( module.DllBase, &funcs ) == STATUS_DLL_NOT_FOUND );
    test_teb.Peb = NULL;
    assert( wine_nx_lookup_static_unix_funcs( module.DllBase, &funcs ) == STATUS_DLL_NOT_FOUND );
    free( module.BaseDllName.Buffer );
}

#ifdef WINE_NX_FEX
static void check_fex( const char *stem )
{
    static const char *suffixes[] = { "", ".dll", "-opt", "-opt.dll", "-tune_2.dll" };
    static const char *bad[] = { "2.dll", ".dll.bak", ".so", "-", "-.dll", "-opt.exe", " opt.dll", "-opt/../x.dll" };
    char name[128];
    UINT64 canonical[2], result[2];
    size_t i, j;

    /* FEXUnixLib::Init's first, by-name method. */
    assert( load_name( stem, FALSE, canonical ) == STATUS_SUCCESS );
    assert( canonical[1] == (UINT_PTR)wine_nx_fex_unix_funcs );
    assert( wine_nx_unload_static_unix_lib( canonical[0] ) == STATUS_SUCCESS );
    for (i = 0; i < ARRAY_SIZE(suffixes); i++)
    {
        snprintf( name, sizeof(name), "%s%s", stem, suffixes[i] );
        assert( load_name( name, FALSE, result ) == STATUS_SUCCESS );
        assert( result[0] == canonical[0] && result[1] == canonical[1] );
        expect_match( name, FALSE, wine_nx_fex_unix_funcs );
        check_module( name, TRUE );
        for (j = 0; name[j]; j++)
            if (name[j] >= 'a' && name[j] <= 'z') name[j] += 'A' - 'a';
        expect_match( name, FALSE, wine_nx_fex_unix_funcs );
        check_module( name, TRUE );
        /* A WoW64 CPU DLL itself is a *native* DLL, not an x86 unixlib. */
        expect_missing( name, TRUE );
    }
    for (i = 0; i < ARRAY_SIZE(bad); i++)
    {
        snprintf( name, sizeof(name), "%s%s", stem, bad[i] );
        expect_missing( name, FALSE );
    }
}
#endif

int main(void)
{
    UINT64 result[2] = { 123, 456 };
    UNICODE_STRING invalid = {0};
    /* Include an extra byte: floor(Length / sizeof(WCHAR)) must not pair it
     * with a valid stem. This also tests that the new matcher remains bounded. */
    WCHAR odd[] = { 'l','i','b','w','o','w','6','4','f','e','x',0xffff };

    expect_match( "ws2_32.dll", FALSE, wine_nx_ws2_32_unix_funcs );
    expect_match( "WS2_32.DLL", TRUE, wine_nx_ws2_32_wow64_unix_funcs );
    expect_missing( "ws2_32", FALSE );
    expect_missing( "ws2_32-opt.dll", FALSE );
    expect_missing( "ws2_32-opt.dll", TRUE );
    expect_missing( "", FALSE );
    expect_missing( "missing.dll", FALSE );
    assert( wine_nx_load_static_unix_lib( NULL, FALSE, result ) == STATUS_DLL_NOT_FOUND );
    assert( wine_nx_load_static_unix_lib( &invalid, FALSE, result ) == STATUS_DLL_NOT_FOUND );
    invalid.Buffer = odd;
    invalid.Length = invalid.MaximumLength = sizeof(odd) - 1;
    assert( wine_nx_load_static_unix_lib( &invalid, FALSE, result ) == STATUS_DLL_NOT_FOUND );
    assert( wine_nx_unload_static_unix_lib( 0 ) == STATUS_INVALID_HANDLE );
    assert( wine_nx_unload_static_unix_lib( 123 ) == STATUS_INVALID_HANDLE );

    /* Existing unixlib initializers still run and propagate their failures. */
    init_status = STATUS_NO_MEMORY;
    assert( load_name( "win32u.dll", FALSE, result ) == STATUS_NO_MEMORY );
    assert( result[0] == 123 && result[1] == 456 && init_calls == 1 );
    init_status = STATUS_SUCCESS;
    expect_match( "win32u.dll", FALSE, NULL );
    assert( init_calls == 2 );

#ifdef WINE_NX_BOX64_INTERPRETER
    expect_match( "winebox64.dll", FALSE, wine_nx_winebox64_unix_funcs );
    expect_missing( "winebox64-opt.dll", FALSE );
#endif
#ifdef WINE_NX_AMD64
    expect_match( "winebox64ec.dll", FALSE, wine_nx_winebox64ec_unix_funcs );
#endif
#ifdef WINE_NX_MESA_SWITCH
    expect_match( "winevulkan.dll", FALSE, wine_nx_winevulkan_unix_funcs );
    expect_match( "winevulkan.dll", TRUE, wine_nx_winevulkan_wow64_unix_funcs );
#endif
#ifdef WINE_NX_FEX
    /* First reproduce the stock/variant difference in the old fallback path.
     * The stock BaseDllName used to match; the -opt one did not. */
    check_module( "libwow64fex.dll", TRUE );
    check_module( "libwow64fex-opt.dll", TRUE );
    check_module( "libarm64ecfex.dll", TRUE );
    check_module( "libarm64ecfex-opt.dll", TRUE );
    check_fex( "libwow64fex" );
    check_fex( "libarm64ecfex" );
#else
    expect_missing( "libwow64fex", FALSE );
    expect_missing( "libwow64fex.dll", FALSE );
    expect_missing( "libwow64fex-opt.dll", FALSE );
    expect_missing( "libarm64ecfex-opt.dll", FALSE );
    check_module( "libwow64fex-opt.dll", FALSE );
#endif
    puts( "lookup by canonical name and module base, stable handles, unload and feature guards: ok" );
    return 0;
}
'''
fixture = fixture.replace('@STUBS@', '\n'.join(stubs)).replace('@TABLES@', tables).replace('@HELPERS@', helpers)
compiler = shlex.split(os.environ.get('CC', 'cc'))
with tempfile.TemporaryDirectory(prefix='wine-nx-fex-unixlib-') as temporary:
    temporary = Path(temporary)
    path = temporary / 'test.c'
    path.write_text(fixture)
    for fex in (False, True):
        for extras in (False, True):
            binary = temporary / f'test-{int(fex)}-{int(extras)}'
            flags = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                     '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64',
                     '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix')]
            if fex:
                flags.append('-DWINE_NX_FEX')
            if extras:
                flags += ['-DWINE_NX_BOX64_INTERPRETER', '-DWINE_NX_AMD64', '-DWINE_NX_MESA_SWITCH']
            subprocess.run(compiler + flags + [str(path), '-o', str(binary)], check=True)
            completed = subprocess.run([str(binary)], capture_output=True, text=True)
            if completed.returncode:
                raise RuntimeError(f'FEX={fex}, extras={extras}: {completed.stdout}\n{completed.stderr}')
            print(f'FEX={int(fex)}, other optional modules={int(extras)}: {completed.stdout.strip()}')
