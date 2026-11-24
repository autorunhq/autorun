#!/usr/bin/env python3
"""Exercise the launcher's UTF-8 paths and the runtime's packed UTF-16 strings."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def definition(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


source = r'''
#include <assert.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "dlls/ntdll/locale_private.h"
#include "horizon-wine/source/launcher_list.h"
'''
source += '\n' + definition('dlls/ntdll/unix/env.c', 'NTSTATUS WINAPI RtlUTF8ToUnicodeN(')
source += '\n' + definition('horizon-wine/source/runtime.c', 'static void put_process_string(')
source += r'''
int main(void)
{
    static const WCHAR expected[] = u"\"D:\\Café 日本 🎮\\Steam.exe\" -test";
    char dos[256], cmdline[512];
    WCHAR buffer[512], *cursor = buffer;
    UNICODE_STRING path, empty, ascii;

    assert(launcher_dos_path("ums0:/Café 日本 🎮/Steam.exe", dos, sizeof(dos)));
    assert(launcher_command_line(dos, "-test", cmdline, sizeof(cmdline)));
    put_process_string(&cursor, &path, cmdline);
    put_process_string(&cursor, &empty, "");
    put_process_string(&cursor, &ascii, "C:\\Steam.exe");

    assert(path.Buffer == buffer && path.Length == sizeof(expected) - sizeof(WCHAR));
    assert(path.MaximumLength == sizeof(expected) && !memcmp(path.Buffer, expected, sizeof(expected)));
    assert(empty.Buffer == buffer + sizeof(expected) / sizeof(WCHAR));
    assert(!empty.Length && empty.MaximumLength == sizeof(WCHAR) && !empty.Buffer[0]);
    assert(ascii.Buffer == empty.Buffer + 1 && ascii.Length == sizeof(u"C:\\Steam.exe") - sizeof(WCHAR));
    assert(ascii.MaximumLength == sizeof(u"C:\\Steam.exe"));
    assert(!memcmp(ascii.Buffer, u"C:\\Steam.exe", sizeof(u"C:\\Steam.exe")));
    assert(cursor == ascii.Buffer + ascii.MaximumLength / sizeof(WCHAR));
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'process_strings'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-fshort-wchar',
                    '-D__WINESRC__', '-DWINE_UNIX_LIB', '-I', str(root / 'include'), '-I', str(root),
                    '-x', 'c', '-', '-o', str(test)], input=source, text=True, check=True)
    subprocess.run([str(test)], check=True)
