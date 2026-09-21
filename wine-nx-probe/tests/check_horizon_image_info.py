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


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


struct_start = source.index('struct horizon_pe_image_info\n{')
struct_text = source[struct_start:source.index('};', struct_start) + 2]
defines = '\n'.join(line for line in source.splitlines()
                    if re.match(r'#define HORIZON_(IMAGE_|STATUS_(SUCCESS|NO_MEMORY|INVALID_IMAGE_FORMAT))', line))

fixture = f'''
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define max(a,b) ((a) > (b) ? (a) : (b))
typedef int BOOL;
{defines}
{struct_text}
struct horizon_server_object {{ int unused; }};
static unsigned short horizon_process_machine = HORIZON_IMAGE_FILE_MACHINE_I386;
static unsigned int horizon_server_errno_status( int error ) {{ return 0xc0000000u | (unsigned int)error; }}
{block('static unsigned int horizon_server_read_exact_at(')}
{block('static unsigned short horizon_get_le16(')}
{block('static unsigned int horizon_get_le32(')}
{block('static unsigned long long horizon_get_le64(')}
{block('static int horizon_pe_data_dir(')}
{block('static size_t horizon_server_read_pe_dir(')}
static unsigned int horizon_server_build_shared_image( int fd, const unsigned char *sections,
                                                       unsigned int section_count, unsigned int align_mask,
                                                       unsigned long long file_size,
                                                       struct horizon_server_object **shared_file )
{{
    *shared_file = NULL;
    return HORIZON_STATUS_SUCCESS;
}}
{block('static unsigned int horizon_server_read_pe_image_info(')}
int main( int argc, char **argv )
{{
    int i;

    if (argc < 2) return 2;
    horizon_process_machine = strtoul( argv[1], NULL, 16 );
    for (i = 2; i < argc; i++)
    {{
        struct horizon_pe_image_info info;
        struct horizon_server_object *shared_file;
        int fd = open( argv[i], O_RDONLY );
        unsigned int status = horizon_server_read_pe_image_info( fd, &info, &shared_file );

        printf( "%d %08x %x %x %x %x %x %x %x %x %x %x %x\\n", i - 2, status,
                info.map_size, info.header_map_size, info.image_flags, info.alignment, info.machine,
                info.is_hybrid, info.contains_code, info.loader_flags, info.header_size,
                info.wine_builtin, info.wine_fakedll );
        close( fd );
    }}
    return 0;
}}
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

    default_real = root / 'wine-nx-probe/toolchains/build-wine-amd64-pe/dlls/ntdll/aarch64-windows/ntdll.dll'
    real = Path(os.environ.get('WINE_NX_IMAGE_INFO_EXE', default_real))
    if real.is_file():
        row = run(0x8664, ['real'], [real])['real']
        status, map_size, header_map_size = row[:3]
        assert status == 0 and map_size % 0x1000 == 0 and header_map_size <= map_size, row
        assert row[3] & 0x04 and row[5:8] == [0xaa64, 1, 1] and row[10] == 1, row
        print(f'{real}: map size {map_size:#x}, header map {header_map_size:#x}, ARM64X metadata found')

print('Image info: range, machine, ARM64X, signatures, truncation and overflow passed')
