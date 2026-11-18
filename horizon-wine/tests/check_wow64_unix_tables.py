#!/usr/bin/env python3
"""Check that the Switch's WoW64 unixlib tables cover every call of the Wine DLLs they serve.

The call gate refuses a code at or above the table's declared count, so a count
larger than the table, or a table shorter than the DLL's enum, would jump to an
arbitrary address. ws2_32 is served by a stub table here; opengl32 and
winenxaudio.drv link their real tables, whose counts come from the DLL's own
enum."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
stubs = (root / 'horizon-wine/source/ws2_32_unix_stub.c').read_text()

def enum_count(header, enum, last):
    text = (root / header).read_text()
    body = re.search(r'enum %s\n\{(.*?)\};' % enum, text, re.S).group(1)
    names = [n for n in re.findall(r'^\s*(\w+)\s*,?\s*$', body, re.M)]
    assert names[-1] == last, (header, names[-1])
    return len(names) - 1

def table_size(name):
    return int(re.search(r'const unixlib_entry_t %s\[(\d+)\]' % name, stubs).group(1))

checks = [
    ('wine_nx_ws2_32_wow64_unix_funcs', 'dlls/ws2_32/ws2_32_private.h', 'ws_unix_funcs', 'ws_unix_funcs_count'),
]
for table, header, enum, last in checks:
    count = enum_count(header, enum, last)
    size = table_size(table)
    assert size == count, f'{table} has {size} entries, {header} defines {count}'
    name = table.replace('_funcs', '_count')
    assert f'const unsigned int {name} = ARRAY_SIZE({table});' in stubs, f'{name} must be the size of {table}'

# crypt32's tables are its own (crypt32_roots.c): they are sized by their
# entries and held against the DLL's enum where they are compiled.
crypt32 = (root / 'horizon-wine/source/crypt32_roots.c').read_text()
crypt32_count = enum_count('dlls/crypt32/crypt32_private.h', 'unix_funcs', 'unix_funcs_count')
for table in ('wine_nx_crypt32_unix_funcs', 'wine_nx_crypt32_wow64_unix_funcs'):
    body = re.search(r'const unixlib_entry_t %s\[\] =\n\{(.*?)\};' % table, crypt32, re.S).group(1)
    size = len([entry for entry in body.replace('\n', ' ').split(',') if entry.strip()])
    assert size == crypt32_count, f'{table} has {size} entries, crypt32_private.h defines {crypt32_count}'
    assert f'C_ASSERT(ARRAY_SIZE({table}) == unix_funcs_count);' in crypt32, f'{table} must cover the crypt32 enum'
assert 'const unsigned int wine_nx_crypt32_wow64_unix_count = ARRAY_SIZE(wine_nx_crypt32_wow64_unix_funcs);' in crypt32, \
    'the crypt32 call count must be the size of its table'

# opengl32 and winenxaudio.drv have their real unix tables linked in, so their
# bound has to come from the DLL's own enum rather than a length written here.
opengl = (root / 'horizon-wine/source/opengl32_unix.c').read_text()
assert 'const unsigned int wine_nx_opengl32_wow64_unix_count = funcs_count;' in opengl, \
    'opengl32 call count must be the DLL enum count'
audio = (root / 'horizon-wine/source/audio_unix.c').read_text()
assert 'C_ASSERT(ARRAY_SIZE(wine_nx_audio_wow64_unix_funcs) == funcs_count);' in audio, \
    'the audio table must cover mmdevapi enum'
assert 'const unsigned int wine_nx_audio_wow64_unix_count = ARRAY_SIZE(wine_nx_audio_wow64_unix_funcs);' in audio, \
    'the audio call count must be the size of its table'
xinput = (root / 'horizon-wine/source/xinput_unix.c').read_text()
for table in ('wine_nx_xinput_unix_funcs', 'wine_nx_xinput_wow64_unix_funcs'):
    assert f'C_ASSERT( ARRAY_SIZE({table}) == nx_xinput_funcs_count );' in xinput, \
        f'{table} must cover the XInput enum'

# The x86 unix call gate (loader.c) and the static table dispatch (virtual.c),
# compiled against small tables: build 18 refused every handle but ntdll's,
# so opengl32's process_attach failed with STATUS_INVALID_HANDLE.
def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

virtual = (root / 'dlls/ntdll/unix/virtual.c').read_text()
loader = (root / 'dlls/ntdll/unix/loader.c').read_text()
libs = virtual[virtual.index('static const struct\n{\n    const char             *name;'):]
libs = libs[:libs.index('};') + 2]
fixture = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int NTSTATUS;
typedef unsigned int ULONG;
typedef unsigned long UINT_PTR;
typedef unsigned long long unixlib_handle_t;
typedef NTSTATUS (*unixlib_entry_t)( void * );
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xc0000008)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ULongToPtr(ul) ((void *)(UINT_PTR)(ul))
#define CHECK(x) do { if (!(x)) { printf( "FAIL line %d: %s\n", __LINE__, #x ); exit( 1 ); } } while (0)
static int last;
static NTSTATUS ntdll_call( void *args ) { last = 1; return (NTSTATUS)(UINT_PTR)args; }
static NTSTATUS ws2_call( void *args ) { last = 2; return (NTSTATUS)(UINT_PTR)args; }
static NTSTATUS gl_call( void *args ) { last = 3; return 0; }
static NTSTATUS audio_call( void *args ) { last = 4; return 0; }
static NTSTATUS crypt_call( void *args ) { last = 5; return 0; }
static NTSTATUS xinput_call( void *args ) { last = 6; return 0; }
static const unixlib_entry_t unix_call_wow64_funcs[] = { ntdll_call, ntdll_call };
const unixlib_entry_t wine_nx_ws2_32_wow64_unix_funcs[5] = { ws2_call, ws2_call, ws2_call, ws2_call, ws2_call };
const unixlib_entry_t wine_nx_opengl32_wow64_unix_funcs[3] = { gl_call, gl_call, gl_call };
const unixlib_entry_t wine_nx_audio_wow64_unix_funcs[2] = { audio_call, audio_call };
const unixlib_entry_t wine_nx_crypt32_wow64_unix_funcs[2] = { crypt_call, crypt_call };
const unixlib_entry_t wine_nx_xinput_wow64_unix_funcs[2] = { xinput_call, xinput_call };
/* The other tables of the list, which the calls below do not reach. */
static NTSTATUS other_call( void *args ) { last = 7; return 0; }
const unixlib_entry_t wine_nx_dnsapi_wow64_unix_funcs[1] = { other_call };
const unixlib_entry_t wine_nx_wma_unix_funcs[1] = { other_call };
const unixlib_entry_t wine_nx_winedmo_wow64_unix_funcs[1] = { other_call };
const unixlib_entry_t wine_nx_media_wow64_unix_funcs[1] = { other_call };
const unixlib_entry_t wine_nx_secur32_wow64_unix_funcs[2] = { other_call, other_call };
const unsigned int wine_nx_dnsapi_wow64_unix_count = 1;
const unsigned int wine_nx_wma_unix_count = 1;
const unsigned int wine_nx_winedmo_unix_count = 1;
const unsigned int wine_nx_media_unix_count = 1;
const unsigned int wine_nx_secur32_wow64_unix_count = 2;
const unsigned int wine_nx_ws2_32_wow64_unix_count = 5;
const unsigned int wine_nx_opengl32_wow64_unix_count = 3;
const unsigned int wine_nx_audio_wow64_unix_count = 2;
const unsigned int wine_nx_crypt32_wow64_unix_count = 2;
const unsigned int wine_nx_xinput_wow64_unix_count = 2;
/* opengl32's calls are timed as they are dispatched (wine_nx_gl_profile). */
struct wine_nx_gl_profile_entry { unsigned long long time; unsigned int calls; };
struct wine_nx_gl_profile_entry wine_nx_gl_profile_entries[8];
unsigned long long wine_nx_gl_call_time;
unsigned int wine_nx_gl_calls;
static unsigned long long ticks;
static unsigned long long horizon_interrupt_time( void ) { return ++ticks; }
''' + libs + '\n' + function(virtual, 'NTSTATUS wine_nx_call_static_wow64_unix') + '\n' + \
    function(loader, 'NTSTATUS wine_nx_call_ntdll_wow64') + r'''
#define H(t) ((unixlib_handle_t)(UINT_PTR)(t))
int main(void)
{
    CHECK( wine_nx_call_ntdll_wow64( H(unix_call_wow64_funcs), 1, 7 ) == 7 && last == 1 );
    CHECK( wine_nx_call_ntdll_wow64( H(unix_call_wow64_funcs), 2, 7 ) == STATUS_INVALID_PARAMETER );
    last = 0;
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_opengl32_wow64_unix_funcs), 0, 0x1000 ) == 0 && last == 3 );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_ws2_32_wow64_unix_funcs), 4, 9 ) == 9 && last == 2 );
    last = 0;
    CHECK( wine_nx_gl_calls == 1 && wine_nx_gl_profile_entries[0].calls == 1 && wine_nx_gl_call_time );
    last = 0;
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_audio_wow64_unix_funcs), 1, 0 ) == 0 && last == 4 );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_audio_wow64_unix_funcs), 2, 0 ) == STATUS_INVALID_PARAMETER );
    last = 0;
    /* A 32-bit crypt32 whose unix side is missing fails its DllMain, and with
     * it every program that imports it. */
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_crypt32_wow64_unix_funcs), 0, 0 ) == 0 && last == 5 );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_crypt32_wow64_unix_funcs), 2, 0 ) == STATUS_INVALID_PARAMETER );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_xinput_wow64_unix_funcs), 1, 0 ) == 0 && last == 6 );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_xinput_wow64_unix_funcs), 2, 0 ) == STATUS_INVALID_PARAMETER );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_secur32_wow64_unix_funcs), 1, 0 ) == 0 && last == 7 );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_secur32_wow64_unix_funcs), 2, 0 ) == STATUS_INVALID_PARAMETER );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_dnsapi_wow64_unix_funcs), 1, 0 ) == STATUS_INVALID_PARAMETER );
    last = 0;  /* the refused calls below must reach no table at all */
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_opengl32_wow64_unix_funcs), 3, 0 ) == STATUS_INVALID_PARAMETER );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_ws2_32_wow64_unix_funcs), 5, 0 ) == STATUS_INVALID_PARAMETER );
    CHECK( wine_nx_call_ntdll_wow64( H(wine_nx_ws2_32_wow64_unix_funcs + 1), 0, 0 ) == STATUS_INVALID_HANDLE );
    CHECK( wine_nx_call_ntdll_wow64( 0, 0, 0 ) == STATUS_INVALID_HANDLE );
    CHECK( wine_nx_call_ntdll_wow64( 0xdeadbeef, 0, 0 ) == STATUS_INVALID_HANDLE && !last );
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / 'gate.c', Path(tmp) / 'gate'
    source.write_text(fixture)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: the ws2_32 (%d) and crypt32 (%d) tables match their DLL enums, audio and XInput '
      'count their native and WoW64 tables; the x86 unix call gate reaches ntdll and every static table '
      'within its size, times opengl32 calls and refuses other handles'
      % (table_size(checks[0][0]), crypt32_count))
