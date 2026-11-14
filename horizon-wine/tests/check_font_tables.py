#!/usr/bin/env python3
"""The font file reader of the Switch's font scan (dlls/win32u/font_tables_switch.h)
against opentype.c reading whole files.

For every face of every font: what the parser gets from the tables read on
their own (the rest of the buffer filled with 0x00 and again with 0xff) has to
be what it gets from the whole file: the face count, version, FONTSIGNATURE,
flags, weight and every family, style and full name. Wine's own fonts, the
system's when there are any, a collection, an OpenType CFF font, a file that is
not an sfnt, and those fonts cut short and with broken bytes, under ASan and
UBSan (which also see the parser's bounds checks hold for broken files).
"""
from pathlib import Path
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]

test = r'''
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define WINE_NO_TRACE_MSGS
#define WINE_NO_DEBUG_MSGS
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"

unsigned char __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
const char *__wine_dbg_strdup( const char *str ) { return str; }
int __wine_dbg_output( const char *str ) { return 0; }
int __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function ) { return 0; }

#include "opentype.c"

static int fill;
static void *test_alloc( size_t size )
{
    void *data = malloc( size );
    if (data) memset( data, fill, size );
    return data;
}
#define FONT_TABLES_ALLOC(size) test_alloc( size )
#include "font_tables_switch.h"

/* The name table as far as the parser's records and strings reach, past the
 * end of the file: zeros, as the reader leaves them. */
#define PAST_END (6 + 12 * 65536 + 3 * 65536)

struct result
{
    BOOL sfnt, name, properties;
    DWORD count, version, ntm_flags;
    UINT weight;
    FONTSIGNATURE fs;
    char names[65536];
    size_t names_len;
};

static BOOL CDECL note_name( LANGID langid, struct opentype_name *name, void *user )
{
    struct result *result = user;
    size_t need = 3 * sizeof(UINT) + name->length;

    if (result->names_len + need <= sizeof(result->names))
    {
        UINT head[3] = { langid, name->codepage, name->length };
        memcpy( result->names + result->names_len, head, sizeof(head) );
        memcpy( result->names + result->names_len + sizeof(head), name->bytes, name->length );
        result->names_len += need;
    }
    return FALSE; /* all of them: more than unix_face_create looks at */
}

/* What unix_face_create asks the parser. */
static void parse( const void *data, size_t size, DWORD index, struct result *result )
{
    const struct ttc_sfnt_v1 *sfnt;
    const struct tt_name_v0 *name;

    memset( result, 0, sizeof(*result) );
    result->sfnt = opentype_get_ttc_sfnt_v1( data, size, index, &result->count, &sfnt );
    if (!result->sfnt) return;
    result->name = opentype_get_tt_name_v0( data, size, sfnt, &name );
    if (!result->name) return;
    result->properties = opentype_get_properties( data, size, sfnt, &result->version, &result->fs,
                                                  &result->ntm_flags, &result->weight );
    if (!result->properties) return;
    opentype_enum_family_names( name, note_name, result );
    opentype_enum_style_names( name, note_name, result );
    opentype_enum_full_names( name, note_name, result );
}

static int same( const struct result *a, const struct result *b )
{
    return a->sfnt == b->sfnt && a->name == b->name && a->properties == b->properties &&
           a->count == b->count && a->version == b->version && a->ntm_flags == b->ntm_flags &&
           a->weight == b->weight && !memcmp( &a->fs, &b->fs, sizeof(a->fs) ) &&
           a->names_len == b->names_len && !memcmp( a->names, b->names, a->names_len );
}

static struct result whole, sparse;

int main( int argc, char **argv )
{
    unsigned long long total = 0, read_total = 0;
    unsigned int faces = 0, parsed = 0;
    int quiet = argc > 1 && !strcmp( argv[1], "-q" );

    for (int i = 1 + quiet; i < argc; i++)
    {
        struct stat st;
        unsigned char *data;
        size_t size;
        DWORD index = 0, count;
        int fd = open( argv[i], O_RDONLY );

        assert( fd != -1 && !fstat( fd, &st ) );
        size = st.st_size;
        data = calloc( 1, size + PAST_END );
        assert( data && pread( fd, data, size, 0 ) == (ssize_t)size );
        total += size;
        do
        {
            parse( data, size, index, &whole );
            count = whole.sfnt || whole.count > 1 ? whole.count : 1;
            for (fill = 0; fill < 0x100; fill += 0xff)
            {
                struct font_tables tables;

                if (!font_tables_load( fd, size, index, &tables ))
                {
                    /* Only a file too short to have a type is read whole. */
                    assert( size < 4 );
                    continue;
                }
                assert( tables.size >= size );
                parse( tables.data, size, index, &sparse );
                if (!same( &whole, &sparse ))
                {
                    fprintf( stderr, "%s face %u (fill %#x): the parser read something else\n",
                             argv[i], (unsigned int)index, fill );
                    return 1;
                }
                if (fill) read_total += tables.read;
                free( tables.data );
            }
            faces++;
            parsed += whole.properties;
        } while (++index < count && index < 64);
        free( data );
        close( fd );
    }
    if (!quiet)
        printf( "%d files, %u faces (%u parsed): %llu KB read of %llu KB for their names\n",
                argc - 1 - quiet, faces, parsed, read_total / 1024, total / 1024 );
    return 0;
}
'''


def tables(data):
    """(tag, offset, length) of a TrueType font's directory."""
    count = struct.unpack_from('>H', data, 4)[0]
    return [struct.unpack_from('>4sII', data, 12 + 16 * i)[0:1] + struct.unpack_from('>II', data, 12 + 16 * i + 8)
            for i in range(count)]


def collection(fonts):
    """A .ttc of the fonts: each one's tables where they were, after a header."""
    header = 12 + 4 * len(fonts)
    offsets, body = [], b''
    for font in fonts:
        base = header + len(body)
        font = bytearray(font)
        for i in range(struct.unpack_from('>H', font, 4)[0]):
            at = 12 + 16 * i + 8
            struct.pack_into('>I', font, at, struct.unpack_from('>I', font, at)[0] + base)
        offsets.append(base)
        body += bytes(font) + b'\0' * (-len(font) % 4)
    return b'ttcf' + struct.pack('>II', 0x00010000, len(fonts)) + b''.join(struct.pack('>I', o) for o in offsets) + body


def cff(font):
    """The font as an OpenType CFF one, as far as the parser sees: OTTO and a CFF table."""
    font = bytearray(font)
    font[0:4] = b'OTTO'
    for i in range(struct.unpack_from('>H', font, 4)[0]):
        if font[12 + 16 * i:16 + 16 * i] == b'glyf':
            font[12 + 16 * i:16 + 16 * i] = b'CFF '
    return bytes(font)


def broken(fonts, out, rng):
    """Fonts cut short and with broken bytes where the parser looks."""
    made = []
    for n, (name, data) in enumerate(fonts):
        spots = [4, 8, 12, 16]
        if data[:4] != b'ttcf':
            for tag, offset, length in tables(data):
                if tag in (b'head', b'OS/2', b'name'):
                    spots += [offset, offset + 2, offset + 4, offset + length - 1]
        for cut in sorted({1, 3, 4, 11, 12, 13, 20, 27, 28, len(data) // 3, len(data) // 2, len(data) - 1}):
            path = out / f'{n}-cut{cut}-{name}'
            path.write_bytes(data[:cut])
            made.append(path)
        for k in range(24):
            data2 = bytearray(data)
            for _ in range(rng.randint(1, 4)):
                at = rng.choice(spots + [rng.randrange(min(len(data), 512))])
                at = min(max(at, 0), len(data) - 1)
                data2[at] = rng.randrange(256)
            # And the directory's offsets and lengths, now and then out of the file or wrapping.
            if k % 3 == 0 and len(data2) > 12:
                count = min(struct.unpack_from('>H', data2, 4)[0], (len(data2) - 12) // 16)
                if count:
                    at = 12 + 16 * rng.randrange(count) + rng.choice((8, 12))
                    struct.pack_into('>I', data2, at, rng.choice((0xffffffff, 0xfffffff0, len(data2), len(data2) + 1)))
            path = out / f'{n}-broken{k}-{name}'
            path.write_bytes(bytes(data2))
            made.append(path)
        # Names the parser reads outside the name table: in the file and past its end.
        name_table = [(offset, length) for tag, offset, length in tables(data) if tag == b'name'] \
            if data[:4] != b'ttcf' else []
        for offset, length in name_table:
            count = struct.unpack_from('>H', data, offset + 2)[0]
            strings = struct.unpack_from('>H', data, offset + 4)[0]
            changes = {
                'strings-in-file': [(offset + 4, max(strings, min(len(data) - offset - 64, 0xffff)))],
                'strings-past-end': [(offset + 4, 0xffff)],
                'records-past-end': [(offset + 2, 0xffff)],
                'records-past-table': [(offset + 2, count + 8)],
            }
            for i in range(min(count, 64)):
                record = offset + 6 + 12 * i
                changes.setdefault('record-offset', []).append((record + 10, 0xfff0 - strings % 0x10))
                changes.setdefault('record-length', []).append((record + 8, 0xffff))
            for label, edits in changes.items():
                data2 = bytearray(data)
                for at, value in edits:
                    struct.pack_into('>H', data2, at, value)
                path = out / f'{n}-{label}-{name}'
                path.write_bytes(bytes(data2))
                made.append(path)
    return made


def main():
    cc = os.environ.get('WINE_NX_HOST_CC') or shutil.which('clang') or 'cc'
    wine = sorted((root / 'fonts').glob('*.ttf'))
    assert wine, 'fonts/*.ttf'
    system = []
    for pattern in ('*.ttf', '*.ttc', '*.otf', '*.otc'):
        system += sorted(Path('/usr/share/fonts').rglob(pattern))[:40]
    with tempfile.TemporaryDirectory(prefix='wine-nx-font-tables-') as directory:
        path = Path(directory)
        source = path / 'test.c'
        source.write_text(test)
        subprocess.run([cc, '-std=gnu11', '-g', '-O1', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-sanitize=alignment',
                        '-D__WINESRC__', '-D_WIN64', '-DWINE_UNIX_LIB',
                        '-I' + str(root / 'include'), '-I' + str(root / 'dlls/win32u'), str(source),
                        '-o', str(path / 'test')], check=True)
        made = path / 'made'
        made.mkdir()
        fonts = [(p.name, p.read_bytes()) for p in wine]
        (made / 'wine.ttc').write_bytes(collection([data for _, data in fonts[:4]]))
        (made / 'cff.otf').write_bytes(cff(fonts[0][1]))
        (made / 'bitmap.fon').write_bytes(b'MZ' + bytes(range(256)) * 8)
        extra = [made / 'wine.ttc', made / 'cff.otf', made / 'bitmap.fon']
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        run = [str(path / 'test')]
        result = subprocess.run(run + [str(p) for p in wine + extra + system], env=env, timeout=600,
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        assert result.returncode == 0
        assert ' 0 faces' not in result.stdout

        targets = [(p.name, p.read_bytes()) for p in wine[:6] + [made / 'wine.ttc', made / 'cff.otf']]
        cases = broken(targets, made, random.Random(1))
        result = subprocess.run(run + ['-q'] + [str(p) for p in cases], env=env, timeout=600,
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        assert result.returncode == 0
    print(f'Font tables: the parser reads the same from the tables alone, '
          f'{len(wine)} Wine fonts, {len(system)} system fonts, a collection, CFF, '
          f'a bitmap font and {len(cases)} cut and broken files passed')


main()
