#!/usr/bin/env python3
"""Run the Horizon server's PE image reader (horizon_server_read_pe_image_info)
on synthetic images and the ARM64X ntdll produced by the multi-arch build.

It must size images as wineserver's get_image_params does: SizeOfImage rounded
up to the section alignment, at least a page, and the header mapping ending at
the first section. NFS Most Wanted Black Edition's speed.exe has a SizeOfImage
that ends inside its last section's page, and loading it failed with
STATUS_INVALID_IMAGE_FORMAT."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
loadorder = (root / 'dlls/ntdll/unix/loadorder.c').read_text()
pe_loader = (root / 'dlls/ntdll/loader.c').read_text()


def block(marker, text=source):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


struct_start = source.index('struct horizon_pe_image_info\n{')
struct_text = source[struct_start:source.index('};', struct_start) + 2]
defines = '\n'.join(line for line in source.splitlines()
                    if re.match(r'#define HORIZON_(IMAGE_|COMIMAGE_|STATUS_)', line))

fixture = f'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define max(a,b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef int BOOL;
{defines}
{struct_text}
{block('struct horizon_pe_version')} ;
struct horizon_server_object
{{
    struct horizon_pe_image_info mapping_image;
    unsigned int type, mapping_has_image, refs;
}};
static unsigned short horizon_process_machine = HORIZON_IMAGE_FILE_MACHINE_I386;
static unsigned int horizon_server_errno_status( int error ) {{ return 0xc0000000u | (unsigned int)error; }}
{block('static unsigned int horizon_server_read_exact_at(')}
{block('static unsigned short horizon_get_le16(')}
{block('static unsigned int horizon_get_le32(')}
{block('static unsigned long long horizon_get_le64(')}
{block('static int horizon_pe_data_dir(')}
{block('static size_t horizon_server_read_pe_dir(')}
{block('static unsigned int horizon_server_find_resource(')}
{block('static unsigned int horizon_server_read_pe_version(')}
static unsigned int horizon_server_build_shared_image( int fd, const unsigned char *sections,
                                                       unsigned int section_count, unsigned int align_mask,
                                                       unsigned long long file_size,
                                                       struct horizon_server_object **shared_file )
{{
    *shared_file = NULL;
    return HORIZON_STATUS_SUCCESS;
}}
{block('static unsigned int horizon_server_read_pe_image_info(')}
struct horizon_server_connection {{ unsigned int pid; int reply_fd; }};
struct horizon_map_image_view_request
{{
    unsigned int mapping, size, entry, machine;
    unsigned long long base;
}};
struct horizon_server_handle_entry {{ struct horizon_server_object *object; }};
struct horizon_image_view
{{
    struct horizon_image_view *next;
    struct horizon_server_object *mapping;
    unsigned int pid;
    unsigned long long base;
}};
static struct horizon_image_view *horizon_image_views;
static struct horizon_server_handle_entry mapped_entry;
static int horizon_server_objects_mutex;
#define HORIZON_SERVER_OBJECT_MAPPING 1
static void test_lock( int *lock ) {{ assert(!*lock); *lock = 1; }}
static void test_unlock( int *lock ) {{ assert(*lock); *lock = 0; }}
#define pthread_mutex_lock test_lock
#define pthread_mutex_unlock test_unlock
static struct horizon_server_handle_entry *horizon_server_find_handle_locked( unsigned int handle )
{{ return handle == 1 ? &mapped_entry : NULL; }}
static void horizon_server_remove_image_view_locked( unsigned int pid, unsigned long long base )
{{ assert(!horizon_image_views); }}
static int horizon_server_write_status( int fd, unsigned int status ) {{ return status; }}
{block('static int horizon_server_handle_map_image_view(')}
static void test_managed_view(void)
{{
    struct horizon_server_object object = {{0}};
    struct horizon_server_connection connection = {{42, 0}};
    struct horizon_map_image_view_request request = {{1, 0x1000, 0, 0, 0x400000}};
    unsigned short saved = horizon_process_machine;
    const unsigned int machines[] = {{0x14c, 0x8664, 0xaa64}};
    object.type = HORIZON_SERVER_OBJECT_MAPPING;
    object.mapping_has_image = 1;
    mapped_entry.object = &object;
    horizon_process_machine = 0x8664;
    for (unsigned int i = 0; i < ARRAY_SIZE(machines); i++)
    {{
        unsigned int status;
        request.machine = machines[i];
        status = horizon_server_handle_map_image_view( &connection, (const unsigned char *)&request );
        assert(status == (machines[i] == 0x14c ? HORIZON_STATUS_IMAGE_MACHINE_TYPE_MISMATCH : 0));
        assert(horizon_image_views && horizon_image_views->mapping == &object);
        assert(horizon_image_views->base == request.base && object.mapping_image.machine == machines[i]);
        free(horizon_image_views);
        horizon_image_views = NULL;
    }}
    assert(object.refs == ARRAY_SIZE(machines));
    horizon_process_machine = saved;
}}
typedef uint16_t WORD, WCHAR;
typedef unsigned int ULONG;
typedef struct {{ unsigned int dwSignature, rest[12]; }} VS_FIXEDFILEINFO;
typedef struct {{ unsigned short Length, MaximumLength; WCHAR *Buffer; }} UNICODE_STRING;
#define FALSE 0
#define TRUE 1
#define TRACE(...) ((void)0)
#define VS_FFI_SIGNATURE 0xfeef04bd
enum loadorder {{ LO_INVALID, LO_DEFAULT, LO_BUILTIN, LO_NATIVE_BUILTIN }};
struct pe_mapping_info
{{
    struct horizon_pe_image_info image;
    unsigned int version_len;
    const void *version_res;
}};
static size_t wcslen( const WCHAR *s ) {{ size_t n = 0; while (s[n]) n++; return n; }}
static unsigned int fold( unsigned int c ) {{ return c >= 'A' && c <= 'Z' ? c + 32 : c; }}
static int wcsnicmp( const WCHAR *a, const WCHAR *b, size_t n )
{{
    while (n--)
    {{
        if (fold(*a) != fold(*b)) return fold(*a) - fold(*b);
        if (!*a) return 0;
        a++; b++;
    }}
    return 0;
}}
static int wcsicmp( const WCHAR *a, const WCHAR *b ) {{ return wcsnicmp(a, b, wcslen(a) + 1); }}
static WCHAR *wcsrchr( const WCHAR *s, WCHAR c )
{{ WCHAR *found = NULL; do {{ if (*s == c) found = (WCHAR *)s; }} while (*s++); return found; }}
static int wcsncmp( const WCHAR *a, const WCHAR *b, size_t n )
{{ while (n--) {{ if (*a != *b) return *a - *b; if (!*a++) break; b++; }} return 0; }}
static BOOL main_exe_loaded, init_done;
static void *std_key, *app_key;
static enum loadorder override;
static void init_load_order(void) {{ init_done = TRUE; }}
static void *open_app_key(const WCHAR *name) {{ return (void *)name; }}
static enum loadorder get_load_order_value(void *std, void *app, const WCHAR *name) {{ return override; }}
{block('static WCHAR *get_basename(', loadorder)}
{block('static inline void remove_dll_ext(', loadorder)}
{block('struct version_info', loadorder)};
{block('struct version_entry', loadorder)};
{block('static BOOL get_version_entry(', loadorder)}
{block('static BOOL version_find_key(', loadorder)}
{block('static enum loadorder version_heuristics(', loadorder)}
{block('void set_load_order_app_name(', loadorder)}
{block('enum loadorder get_load_order(', loadorder)}
int main( int argc, char **argv )
{{
    int i;
    test_managed_view();

    if (argc < 2) return 2;
    horizon_process_machine = strtoul( argv[1], NULL, 16 );
    for (i = 2; i < argc; i++)
    {{
        struct horizon_pe_image_info info;
        struct horizon_server_object *shared_file;
        struct horizon_pe_version *version;
        struct pe_mapping_info mapping;
        int fd = open( argv[i], O_RDONLY );
        unsigned int status = horizon_server_read_pe_image_info( fd, &info, &shared_file, &version );

        mapping.image = info;
        mapping.version_len = version ? version->size : 0;
        mapping.version_res = version ? version->data : NULL;

        WCHAR name[] = {{'D',':','\\\\','G','a','m','e','\\\\','x','i','n','p','u','t','.','d','l','l',0}};
        UNICODE_STRING nt_name = {{sizeof(name) - sizeof(WCHAR), sizeof(name), name}};
        main_exe_loaded = FALSE;
        override = LO_INVALID;
        if (get_load_order(&nt_name, FALSE, &mapping) != LO_NATIVE_BUILTIN) abort();
        set_load_order_app_name(name);
        if (!main_exe_loaded || app_key != name + 8) abort();
        enum loadorder expected = version_heuristics(NULL, &mapping);
        if (expected == LO_INVALID) expected = LO_DEFAULT;
        if (get_load_order(&nt_name, FALSE, &mapping) != expected) abort();
        override = LO_BUILTIN;
        if (get_load_order(&nt_name, FALSE, &mapping) != LO_BUILTIN) abort();

        printf( "%d %08x %x %x %x %x %x %x %x %x %x %x %x %x %x\\n", i - 2, status,
                info.map_size, info.header_map_size, info.image_flags, info.alignment, info.machine,
                info.is_hybrid, info.contains_code, info.loader_flags, info.header_size,
                info.wine_builtin, info.wine_fakedll, mapping.version_len,
                version_heuristics( NULL, &mapping ) );
        free( version );
        close( fd );
    }}
    return 0;
}}
'''


client_fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
static TEB test_teb;
static const USHORT current_machine = IMAGE_FILE_MACHINE_AMD64;
#undef NtCurrentTeb
#define NtCurrentTeb() (&test_teb)
#define GetProcessHeap() ((HANDLE)1)
#define RtlAllocateHeap(heap, flags, size) malloc(size)
#define RtlFreeHeap(heap, flags, ptr) free(ptr)
static NTSTATUS read_file(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, void *context,
                          IO_STATUS_BLOCK *io, void *buffer, ULONG size, LARGE_INTEGER *offset, ULONG *key)
{
    ssize_t count = pread((int)(intptr_t)file, buffer, size, offset->QuadPart);
    io->Information = count < 0 ? 0 : count;
    return count < 0 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
#define NtReadFile read_file
'''
for marker in ('static ULONG read_image_directory(', 'static BOOL is_com_ilonly(',
               'static BOOL has_chpe_metadata(', 'static BOOL is_valid_binary('):
    client_fixture += block(marker, pe_loader) + '\n'
client_fixture += r'''
int main(int argc, char **argv)
{
    SECTION_IMAGE_INFORMATION info = {0};
    assert(argc == 5);
    int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    info.Machine = strtoul(argv[2], NULL, 16);
    info.ImageFlags = strtoul(argv[3], NULL, 16);
    info.ImageContainsCode = strtoul(argv[4], NULL, 16);
    printf("%u\n", is_valid_binary((HANDLE)(intptr_t)fd, &info));
    close(fd);
}
'''


def pe32(path, size_of_image, sections, section_alignment=0x1000, size_of_headers=0x1000):
    """An i386 PE: sections are (virtual address, virtual size, raw pointer, raw size)."""
    opt = bytearray(224)
    struct.pack_into('<H', opt, 0, 0x10b)
    struct.pack_into('<I', opt, 16, 0x1000)                      # entry point
    struct.pack_into('<I', opt, 28, 0x400000)                    # image base
    struct.pack_into('<II', opt, 32, section_alignment, 0x200)  # section, file alignment
    struct.pack_into('<III', opt, 56, size_of_image, size_of_headers, 0)
    struct.pack_into('<H', opt, 68, 2)
    struct.pack_into('<I', opt, 92, 16)
    headers = bytearray(0x40)
    headers[0:2] = b'MZ'
    struct.pack_into('<I', headers, 0x3c, 0x40)
    headers += b'PE\0\0' + struct.pack('<HHIIIHH', 0x14c, len(sections), 0, 0, 0, len(opt), 0x10f) + opt
    for i, (va, vsize, raw, rawsize) in enumerate(sections):
        headers += struct.pack('<8sIIIIIIHHI', b'.s%d' % i, vsize, va, rawsize, raw, 0, 0, 0, 0,
                               0x60000020 if i == 0 else 0x40000040)
    end = max([0x1000] + [raw + rawsize for _, _, raw, rawsize in sections])
    path.write_bytes(bytes(headers) + bytes(end - len(headers)))


def pe64(path, machine, hybrid=False, truncate_cfg=False, builtin=False):
    """A PE32+ image with one section and optional ARM64X load-config metadata."""
    pe_offset = 0x80
    opt = bytearray(240)
    struct.pack_into('<H', opt, 0, 0x20b)
    struct.pack_into('<I', opt, 4, 0x100)
    struct.pack_into('<I', opt, 16, 0x1000)
    struct.pack_into('<Q', opt, 24, 0x180000000)
    struct.pack_into('<II', opt, 32, 0x1000, 0x200)
    struct.pack_into('<II', opt, 56, 0x2000, 0x400)
    struct.pack_into('<HH', opt, 68, 2, 0x40)
    struct.pack_into('<QQ', opt, 72, 0x100000, 0x1000)
    struct.pack_into('<I', opt, 108, 16)
    struct.pack_into('<II', opt, 112 + 5 * 8, 0x1200, 0x10)
    if hybrid:
        struct.pack_into('<II', opt, 112 + 10 * 8, 0x1100, 0xd0)

    headers = bytearray(pe_offset)
    headers[0:2] = b'MZ'
    struct.pack_into('<I', headers, 0x3c, pe_offset)
    if builtin:
        headers[0x40:0x51] = b'Wine builtin DLL\0'
    headers += b'PE\0\0' + struct.pack('<HHIIIHH', machine, 1, 0, 0, 0, len(opt), 0x2022) + opt
    headers += struct.pack('<8sIIIIIIHHI', b'.text', 0x1000, 0x1000, 0x400, 0x400,
                           0, 0, 0, 0, 0x60000020)
    image = headers + bytes(0x800 - len(headers))
    if hybrid:
        cfg = bytearray(0xd0)
        struct.pack_into('<I', cfg, 0, len(cfg))
        struct.pack_into('<Q', cfg, 0xc8, 0x180001800)
        image[0x500:0x500 + len(cfg)] = cfg
        if truncate_cfg:
            image = image[:0x540]
    path.write_bytes(image)


with tempfile.TemporaryDirectory(prefix='wine-nx-image-info-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(fixture)
    subprocess.run(['cc', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    (tmp / 'client.c').write_text(client_fixture)
    subprocess.run(['cc', '-g', '-Wall', '-Werror', '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64',
                    '-fsanitize=address,undefined', '-I' + str(root / 'include'),
                    str(tmp / 'client.c'), '-o', str(tmp / 'client')], check=True)

    # SizeOfImage 0x678e4e: the last section's page runs to 0x679000 (speed.exe's layout).
    pe32(tmp / 'unaligned.exe', 0x678e4e, [(0x1000, 0x48e2a5, 0x1000, 0x1000), (0x638000, 0x40e4e, 0x2000, 0x1000)])
    pe32(tmp / 'aligned.exe', 0x532000, [(0x1000, 0x1000, 0x1000, 0x1000)])
    pe32(tmp / 'align64k.exe', 0x21000, [(0x10000, 0x1000, 0x1000, 0x1000)], section_alignment=0x10000)
    pe32(tmp / 'late-section.exe', 0x4000, [(0x2000, 0x1000, 0x400, 0x200)], size_of_headers=0x400)
    pe32(tmp / 'flat.exe', 0x3000, [(0x400, 0x200, 0x400, 0x200)], section_alignment=0x200)
    pe32(tmp / 'wraps.exe', 0xfffff001, [(0x1000, 0x1000, 0x1000, 0x1000)])
    pe64(tmp / 'amd64.dll', 0x8664, builtin=True)
    pe64(tmp / 'arm64x.dll', 0xaa64, hybrid=True, builtin=True)
    pe64(tmp / 'truncated-cfg.dll', 0xaa64, hybrid=True, truncate_cfg=True)

    def run(machine, labels, paths):
        out = subprocess.run([str(tmp / 'test'), f'{machine:x}'] + [str(p) for p in paths], check=True,
                             capture_output=True, text=True).stdout.splitlines()
        return {labels[int(line.split()[0])]: [int(v, 16) for v in line.split()[1:]] for line in out}

    def client_accepts(path, row):
        assert row[0] == 0, row
        output = subprocess.check_output([str(tmp / 'client'), str(path),
                                          f'{row[5]:x}', f'{row[3]:x}', f'{row[7]:x}'], text=True)
        return bool(int(output))

    names = ['unaligned', 'aligned', 'align64k', 'late-section', 'flat', 'wraps']
    rows = run(0x14c, names, [tmp / (n + '.exe') for n in names])

    assert rows['unaligned'][:3] == [0, 0x679000, 0x1000], rows['unaligned']
    assert rows['aligned'][:3] == [0, 0x532000, 0x1000], rows['aligned']
    assert rows['align64k'][:3] == [0, 0x30000, 0x10000], rows['align64k']
    assert rows['late-section'][:3] == [0, 0x4000, 0x2000], rows['late-section']
    assert rows['flat'][0] == 0 and rows['flat'][3] & 0x08, rows['flat']
    assert rows['wraps'][0] == 0xc000007b, rows['wraps']

    rows = run(0x8664, ['amd64', 'arm64x', 'truncated'],
               [tmp / 'amd64.dll', tmp / 'arm64x.dll', tmp / 'truncated-cfg.dll'])
    assert rows['amd64'][0] == 0 and rows['amd64'][5] == 0x8664, rows['amd64']
    assert rows['amd64'][3] & 0x04 and rows['amd64'][10] == 1, rows['amd64']
    assert rows['arm64x'][0] == 0 and rows['arm64x'][5:8] == [0xaa64, 1, 1], rows['arm64x']
    assert rows['arm64x'][3] & 0x04 and rows['arm64x'][10] == 1, rows['arm64x']
    assert rows['truncated'][0] == 0 and rows['truncated'][6] == 0, rows['truncated']
    assert run(0x14c, ['amd64'], [tmp / 'amd64.dll'])['amd64'][0] == 0xc000007b

    def managed_image(path, flags=1, version=(2, 5), pe64_image=False, directory_size=72):
        if pe64_image:
            pe64(path, 0x8664)
        else:
            pe32(path, 0x2000, [(0x1000, 0x1000, 0x400, 0x400)], size_of_headers=0x400)
        image = bytearray(path.read_bytes())
        opt = struct.unpack_from('<I', image, 60)[0] + 24
        struct.pack_into('<II', image, opt + (112 if pe64_image else 96) + 14 * 8,
                         0x1100, directory_size)
        struct.pack_into('<IHHIII', image, 0x500, 72, *version, 0x1180, 0x40, flags)
        path.write_bytes(image)

    for flags, expected in ((1, 3), (9, 3), (0x20001, 0x23), (3, 2), (0x20003, 0x22), (0, 0)):
        path = tmp / 'managed.dll'
        managed_image(path, flags)
        row = run(0x14c, ['il'], [path])['il']
        assert row[0] == 0 and row[3] == expected and row[8] == 1, row
        row = run(0x8664, ['il'], [path])['il']
        assert row[0] == 0 and row[3] == expected, row
        assert client_accepts(path, row) == bool(flags & 1), row

    for version in ((1, 1), (2, 0), (2, 4)):
        managed_image(path, version=version)
        row = run(0x8664, ['old-clr'], [path])['old-clr']
        assert row[0] == 0 and row[3] == 0, row
        assert client_accepts(path, row), row
    managed_image(path, version=(4, 0))
    assert run(0x8664, ['new-clr'], [path])['new-clr'][3] == 3
    for directory_size in (0, 8, 16):
        managed_image(path, directory_size=directory_size)
        row = run(0x8664, ['short-clr'], [path])['short-clr']
        assert row[0] == 0 and row[3] == 0 and not client_accepts(path, row), row
    managed_image(path)
    path.write_bytes(path.read_bytes()[:0x510])
    row = run(0x8664, ['truncated-clr'], [path])['truncated-clr']
    assert row[0] == 0 and row[3] == 0 and not client_accepts(path, row), row
    path = tmp / 'aligned.exe'
    row = run(0x8664, ['native32'], [path])['native32']
    assert row[0] == 0 and not client_accepts(path, row), row
    path = tmp / 'managed.dll'
    managed_image(path, pe64_image=True)
    row = run(0x8664, ['managed64'], [path])['managed64']
    assert row[0] == 0 and row[3] == 2 and row[5] == 0x8664, row

    if real_managed := os.environ.get('WINE_NX_MANAGED_DLL'):
        for machine in (0x14c, 0x8664):
            row = run(machine, ['mscorlib'], [Path(real_managed)])['mscorlib']
            assert row[0] == 0 and row[8] == 1 and client_accepts(Path(real_managed), row), row
        print(f'{real_managed}: managed image accepted in Win32 and Win64')

    if mono_dir := os.environ.get('WINE_NX_MONO_DIR'):
        assemblies = sorted(Path(mono_dir).rglob('*.dll'))
        rows = run(0x8664, list(map(str, assemblies)), assemblies)
        for assembly in assemblies:
            row = rows[str(assembly)]
            assert row[0] == 0 and client_accepts(assembly, row), (assembly, row)
        print(f'Mono: all {len(assemblies)} packaged assemblies pass the Win64 section and DLL-loader checks')

    def version_node(key, value=b'', children=b'', text=False):
        data = bytearray(struct.pack('<HHH', 0, len(value) // (2 if text else 1), int(text)))
        data += (key + '\0').encode('utf-16le')
        data += bytes(-len(data) % 4)
        data += value
        data += bytes(-len(data) % 4)
        data += children
        struct.pack_into('<H', data, 0, len(data))
        return data

    def version_blob(vendor):
        company = version_node('CompanyName', (vendor + '\0').encode('utf-16le'), text=True)
        table = version_node('040904b0', children=company, text=True)
        strings = version_node('StringFileInfo', children=table, text=True)
        return version_node('VS_VERSION_INFO', struct.pack('<13I', 0xfeef04bd, *([0] * 12)), strings)

    def add_version(path, vendor, languages=(0x409,), named=False, odd=False):
        image = bytearray(path.read_bytes())
        pe = struct.unpack_from('<I', image, 60)[0]
        opt = pe + 24
        pe32 = struct.unpack_from('<H', image, opt)[0] == 0x10b
        section = opt + (224 if pe32 else 240)
        raw = struct.unpack_from('<I', image, section + 20)[0]
        va = struct.unpack_from('<I', image, section + 12)[0]
        blob = version_blob(vendor)
        if odd:
            blob += b'X'
        resource = bytearray(0x400)
        struct.pack_into('<HH', resource, 12, int(named), 1)
        if named:
            struct.pack_into('<II', resource, 16, 0x800000f0, 0x800000e0)
        struct.pack_into('<II', resource, 16 + int(named) * 8, 16, 0x80000030)
        struct.pack_into('<H', resource, 0x30 + 14, 1)
        struct.pack_into('<II', resource, 0x40, 1, 0x80000050)
        struct.pack_into('<H', resource, 0x50 + 14, len(languages))
        for i, language in enumerate(languages):
            struct.pack_into('<II', resource, 0x60 + i * 8, language, 0x90 + i * 16)
            selected = len(blob) if language == languages[-1] else 1
            struct.pack_into('<4I', resource, 0x90 + i * 16, va + 0x200, selected, 0, 0)
        resource[0x200:0x200 + len(blob)] = blob
        image[raw:raw + len(resource)] = resource
        struct.pack_into('<II', image, section + 16, len(resource), raw)
        struct.pack_into('<II', image, opt + (96 if pe32 else 112) + 16, va, len(resource))
        path.write_bytes(image)
        return raw, len(blob)

    for machine in (0x14c, 0x8664):
        def make(path):
            if machine == 0x14c:
                pe32(path, 0x2000, [(0x1000, 0x1000, 0x400, 0x400)], size_of_headers=0x400)
            else:
                pe64(path, machine)
        for vendor, expected in [('Microsoft Corporation', 1), ('Controller mod', 3), ('Twain Working Group', 2)]:
            path = tmp / 'version.dll'
            make(path)
            raw, length = add_version(path, vendor, languages=(0x411, 0x409), named=True, odd=True)
            row = run(machine, ['version'], [path])['version']
            assert row[0] == 0 and row[-2:] == [(length + 3) & ~3, expected], row
        for languages in ((0x411, 0), (0x411,)):
            make(path)
            _, length = add_version(path, 'Microsoft Corporation', languages=languages)
            assert run(machine, ['v'], [path])['v'][-2:] == [length, 1]
        original = path.read_bytes()
        for offset, value in ((raw + 20, 0x80001000), (raw + 0x94, 0xffffffff),
                              (raw + 0x90, 0xfffffff0), (raw + 12, 0xffffffff)):
            damaged = bytearray(original)
            struct.pack_into('<I', damaged, offset, value)
            path.write_bytes(damaged)
            row = run(machine, ['bad'], [path])['bad']
            assert row[0] == 0 and row[-2:] == [0, 3], row
        path.write_bytes(original[:raw + 0x205])
        assert run(machine, ['short'], [path])['short'][-2:] == [0, 3]

    if real_version := os.environ.get('WINE_NX_VERSION_INFO_DLL'):
        row = run(0x8664, ['vendor'], [Path(real_version)])['vendor']
        assert row[0] == 0 and row[-2] > 0 and row[-1] == 1, row
        print(f'{real_version}: {row[-2]} bytes of version metadata, Microsoft built-in preference restored')

    default_real = root / 'horizon-wine/toolchains/build-wine-amd64-pe/dlls/ntdll/aarch64-windows/ntdll.dll'
    real = Path(os.environ.get('WINE_NX_IMAGE_INFO_EXE', default_real))
    if real.is_file():
        row = run(0x8664, ['real'], [real])['real']
        status, map_size, header_map_size = row[:3]
        assert status == 0 and map_size % 0x1000 == 0 and header_map_size <= map_size, row
        assert row[3] & 0x04 and row[5:8] == [0xaa64, 1, 1] and row[10] == 1, row
        print(f'{real}: map size {map_size:#x}, header map {header_map_size:#x}, ARM64X metadata found')

print('Image info: ranges, ARM64X, version resources, language selection and load-order heuristics passed')

runtime = (root / 'horizon-wine/source/runtime.c').read_text()
init = runtime.index('set_load_order_app_name( params->ImagePathName.Buffer )')
assert runtime.index('runtime_init_peb_process( teb, module, params )') < init
assert init < runtime.index('status = runtime_prepare_arm64ec()', init)
