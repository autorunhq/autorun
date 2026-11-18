#!/usr/bin/env python3
"""The launcher's editor for a game's text files (horizon-wine/source/
launcher_text.h, launcher_text_edit.c): a file read in each encoding a Windows
game writes its settings in -- UTF-8 with a byte order mark and without,
UTF-16 in both byte orders, Windows-1252 -- with each kind of line end, comes
back byte for byte when no line changed, and with only the changed line
different when one did; an .ini setting's key and value are found; a binary
file is refused. Then that a game's menu offers the editor and another
program of the game's folder, and that both browse from that folder."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / 'horizon-wine/source'


def c_bytes(data):
    return '"' + ''.join(f'\\x{b:02x}' for b in data) + '"', len(data)


cases = []
samples = {
    'utf8': ('[Display]\nWidth=1280\nName=Žluťoučký kůň\n', 'utf-8', b''),
    'utf8-bom-crlf': ('[Display]\r\nWidth=1280\r\n; € comment\r\n', 'utf-8', b'\xef\xbb\xbf'),
    'utf16le': ('[Display]\r\nWidth=1280\r\nName=日本 𝄞\r\n', 'utf-16-le', b'\xff\xfe'),
    'utf16be-no-end': ('Width=1280\r\nHeight=720', 'utf-16-be', b'\xfe\xff'),
    'cp1252': ('Width=1280\r\nName=Café € \u0081\r\n', 'cp1252-passthrough', b''),
    'cr': ('a=1\rb=2\r', 'utf-8', b''),
    'empty': ('', 'utf-8', b''),
    'one-newline': ('\n', 'utf-8', b''),
}


def encode(text, codec):
    if codec == 'cp1252-passthrough':
        out = bytearray()
        for ch in text:
            if ord(ch) in (0x81, 0x8d, 0x8f, 0x90, 0x9d):
                out.append(ord(ch))
            else:
                out += ch.encode('cp1252')
        return bytes(out)
    return text.encode(codec)


body = []
for name, (text, codec, bom) in samples.items():
    data = bom + encode(text, codec)
    literal, size = c_bytes(data)
    body.append(f'    roundtrip( "{name}", (const unsigned char *){literal}, {size} );')

# One line changed: the rest stays as it was, in the file's encoding.
changed_before = b'\xff\xfe' + '[A]\r\nWidth=1280\r\nHeight=720\r\n'.encode('utf-16-le')
changed_after = b'\xff\xfe' + '[A]\r\nWidth=1920\r\nHeight=720\r\n'.encode('utf-16-le')
ansi_before = 'Name=Café\nX=1\n'.encode('cp1252')
ansi_after = 'Name=Český €\nX=1\n'.encode('cp1252', errors='replace')

test = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "launcher_text.h"

static void roundtrip( const char *name, const unsigned char *data, size_t size )
{
    struct launcher_text text;
    unsigned char *out;
    size_t out_size;

    assert( launcher_text_parse( &text, data, size, 100 ) == LAUNCHER_TEXT_OK );
    assert( text.count >= 1 );
    out = launcher_text_build( &text, &out_size );
    if (out_size != size || memcmp( out, data, size ))
    {
        fprintf( stderr, "%s: %u bytes back from %u\n", name, (unsigned int)out_size, (unsigned int)size );
        assert( 0 );
    }
    free( out );
    launcher_text_free( &text );
}

static void change( const unsigned char *before, size_t before_size, int line, const char *value,
                    const unsigned char *after, size_t after_size )
{
    struct launcher_text text;
    size_t key, key_len, start, out_size;
    unsigned char *out;
    char edited[200];

    assert( launcher_text_parse( &text, before, before_size, 100 ) == LAUNCHER_TEXT_OK );
    assert( launcher_text_setting( text.lines[line], &key, &key_len, &start ) );
    snprintf( edited, sizeof(edited), "%.*s%s", (int)start, text.lines[line], value );
    assert( launcher_text_set( &text, line, edited ) );
    out = launcher_text_build( &text, &out_size );
    assert( out_size == after_size && !memcmp( out, after, after_size ) );
    free( out );
    launcher_text_free( &text );
}

int main( void )
{
    struct launcher_text text;
    size_t key, key_len, value;
    unsigned char *out;
    size_t size;

''' + '\n'.join(body) + r'''

    /* Lines: ends of any kind, the first one kept; the last one's own end. */
    assert( launcher_text_parse( &text, (const unsigned char *)"a\r\nb\nc", 6, 100 ) == LAUNCHER_TEXT_OK );
    assert( text.count == 3 && !strcmp( text.lines[1], "b" ) && !strcmp( text.newline, "\r\n" ) && !text.final_newline );
    out = launcher_text_build( &text, &size );
    assert( size == 7 && !memcmp( out, "a\r\nb\r\nc", 7 ) );
    free( out );
    /* Added and removed lines. */
    assert( launcher_text_insert( &text, 3, "d" ) );
    launcher_text_delete( &text, 0 );
    out = launcher_text_build( &text, &size );
    assert( size == 7 && !memcmp( out, "b\r\nc\r\nd", 7 ) );
    free( out );
    launcher_text_free( &text );

    /* Too many lines, and what is not text. */
    assert( launcher_text_parse( &text, (const unsigned char *)"1\n2\n3\n", 6, 2 ) == LAUNCHER_TEXT_TOO_LONG );
    assert( !text.count && !text.lines );
    assert( launcher_text_parse( &text, (const unsigned char *)"MZ\0\0\x90", 5, 100 ) == LAUNCHER_TEXT_BINARY );
    assert( launcher_text_parse( &text, (const unsigned char *)"\xff\xfe" "a\0\0\0", 6, 100 ) == LAUNCHER_TEXT_BINARY );
    assert( launcher_text_parse( &text, (const unsigned char *)"\xff\xfe" "a", 3, 100 ) == LAUNCHER_TEXT_BINARY );

    /* Settings: key and value, with what stands round the '='. */
    assert( launcher_text_setting( "  Height = 720", &key, &key_len, &value ) );
    assert( key == 2 && key_len == 6 && value == 11 );
    assert( launcher_text_setting( "Empty=", &key, &key_len, &value ) && value == 6 );
    assert( !launcher_text_setting( "[Display]", &key, &key_len, &value ) );
    assert( !launcher_text_setting( "; Width=1", &key, &key_len, &value ) );
    assert( !launcher_text_setting( "# Width=1", &key, &key_len, &value ) );
    assert( !launcher_text_setting( "// Width=1", &key, &key_len, &value ) );
    assert( !launcher_text_setting( "=1", &key, &key_len, &value ) );
    assert( !launcher_text_setting( "seta r_mode \"4\"", &key, &key_len, &value ) );

    /* Characters as the keyboard counts its 500: UTF-16 code units, not bytes. */
    assert( launcher_text_utf16_length( "" ) == 0 );
    assert( launcher_text_utf16_length( "Width=1280" ) == 10 );
    assert( launcher_text_utf16_length( "\xc5\xbelu\xc5\xa5ou\xc4\x8dk\xc3\xbd" ) == 9 );     /* žluťoučký: 13 bytes */
    assert( launcher_text_utf16_length( "\xe6\x97\xa5\xe6\x9c\xac" ) == 2 );                 /* 日本: 6 bytes */
    assert( launcher_text_utf16_length( "\xf0\x9d\x84\x9e" ) == 2 );                          /* 𝄞: a surrogate pair */
    assert( launcher_text_utf16_length( "a\xff" "b" ) == 3 );                                   /* a byte that is not UTF-8 */

    {
        static const unsigned char before[] = ''' + c_bytes(changed_before)[0] + r''', after[] = ''' + c_bytes(changed_after)[0] + r''';
        static const unsigned char ansi_before[] = ''' + c_bytes(ansi_before)[0] + r''', ansi_after[] = ''' + c_bytes(ansi_after)[0] + r''';

        change( before, sizeof(before) - 1, 1, "1920", after, sizeof(after) - 1 );
        /* Windows-1252 keeps what it has a byte for; the rest is '?'. */
        change( ansi_before, sizeof(ansi_before) - 1, 0, "\xc4\x8c" "esk\xc3\xbd \xe2\x82\xac", ansi_after, sizeof(ansi_after) - 1 );
    }
    puts( "launcher_text: ok" );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / 'text.c'
    exe = Path(temp) / 'text'
    src.write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(source), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The editor offers text files, and leaves Autorun's own settings to the menu.
edit = (source / 'launcher_text_edit.c').read_text()
for extension in ('".ini"', '".cfg"', '".txt"'):
    assert extension in edit, extension
assert '".wine-nx.txt"' in edit
# A line is too long for the keyboard by its characters, and what the keyboard
# gives back has room for 500 of any kind.
assert 'launcher_text_utf16_length( text ) <= TEXT_EDIT_PROMPT' in edit
assert edit.count('[TEXT_EDIT_BUFFER]') == 2 and 'TEXT_EDIT_PROMPT + 1]' not in edit
# Saved beside the file first, then put in its place.
assert '%s.autorun-new' in edit and 'rename( fresh, path )' in edit

# A game's menu: both rows, browsing from the game's folder.
launcher = (source / 'launcher.c').read_text()
assert 'ADD_ROW( ROW_OTHER_PROGRAM, SECTION_GENERAL, "Run another program",' in launcher
assert 'ADD_ROW( ROW_EDIT_FILE, SECTION_GENERAL, "Edit a text file",' in launcher
assert 'file_browser_from( l, dir, BROWSE_PROGRAMS, "Run another program", picked, sizeof(picked) )' in launcher
assert 'file_browser_from( l, dir, BROWSE_TEXT, "Edit a text file", picked, sizeof(picked) )' in launcher
assert 'launcher_text_edit( ui, picked, file_name( picked ) );' in launcher
assert 'if (start_path( l, picked, "Run another program", target, size )) return 1;' in launcher
# B in the folder it opened in closes the browser.
assert 'if (action == UI_ACTION_BACK && start && !strcmp( dir, start )) return 0;' in launcher

# The runtime and the host test build the editor.
cmake = (root / 'horizon-wine/CMakeLists.txt').read_text()
assert cmake.count('source/launcher_text_edit.c') == 2
assert 'source/launcher_text_edit.c' in (root / 'horizon-wine/tests/check-launcher-host.sh').read_text()

print('Text editor: every encoding and line end written back as read; '
      'a game\'s menu edits its text files and runs its other programs')
