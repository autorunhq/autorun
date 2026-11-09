#!/usr/bin/env python3
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
forwarder = (root / 'source/forwarder.c').read_text()
generation = int(re.search(r'#define FORWARDER_GENERATION (\d+)', forwarder)[1])
path = 'sdmc:/switch/wine/wine-nx-runtime.nro'
key = f'{path}{path}\naddress-space=3\nautorun-forwarder={generation}'
title = int.from_bytes(hashlib.sha256(key.encode()).digest()[:8], 'little')
title = 0x0500000000000000 | (title & 0x00FFFFFFFFFFF000)
patch = (root / 'mesosphere/low-window.patch').read_text()
assert f'program_id == UINT64_C(0x{title:016X})' in patch
assert title == 0x0548EABB35576000

# The 32-bit forwarder is a second entry for the same NRO: its own address
# space in the name, its own title id, and no part of the patch's scope.
key32 = f'{path}{path}\naddress-space=2\nautorun-forwarder={generation}'
title32 = int.from_bytes(hashlib.sha256(key32.encode()).digest()[:8], 'little')
title32 = 0x0500000000000000 | (title32 & 0x00FFFFFFFFFFF000)
assert title32 != title
assert f'program_id == UINT64_C(0x{title32:016X})' not in patch
# And sphaira's plain name is the one it always was, so a forwarder it made for
# the same NRO is still recognised as somebody else's.
plain = int.from_bytes(hashlib.sha256(f'{path}{path}'.encode()).digest()[:8], 'little')
plain = 0x0500000000000000 | (plain & 0x00FFFFFFFFFFF000)
plain32 = int.from_bytes(hashlib.sha256(f'{path}{path}\naddress-space=2'.encode()).digest()[:8], 'little')
plain32 = 0x0500000000000000 | (plain32 & 0x00FFFFFFFFFFF000)
assert plain32 != plain

source = (root / 'source/launcher.c').read_text()
def function(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fixture = r'''
#include <assert.h>
#include <stdio.h>
enum launcher_address_space { LAUNCHER_ADDRESS_LOW, LAUNCHER_ADDRESS_ANY };
static enum launcher_address_space detected;
struct options { int address_space_bits, low_window; };
struct launcher { struct options *options; };
struct program { const char *path; unsigned short machine; };
static enum launcher_address_space launcher_program_address_space(const char *path)
{ (void)path; return detected; }
'''
fixture += function('static int address_space_fits(')
fixture += r'''
int main(void)
{
    struct options o = {39, 0};
    struct launcher l = {&o};
    struct program p = {"fixed.exe", 0x014c};
    detected = LAUNCHER_ADDRESS_LOW;
    assert(!address_space_fits(&l, &p));
    o.low_window = 1;
    assert(address_space_fits(&l, &p));
    /* 32-bit without aliases: the four gigabytes from zero are the window, so
     * a fixed-address image has its address and nothing needs the patch. */
    o.address_space_bits = 32;
    assert(address_space_fits(&l, &p));
    /* A 64-bit image wants to be above four gigabytes, so it cannot. */
    p.machine = 0x8664;
    assert(!address_space_fits(&l, &p));
    p.machine = 0x014c;
    o.address_space_bits = 0;
    assert(!address_space_fits(&l, &p));
    o.address_space_bits = 39;
    o.low_window = 0;
    assert(!address_space_fits(&l, &p));
    detected = LAUNCHER_ADDRESS_ANY;
    assert(address_space_fits(&l, &p));
    puts("Low window: verified forwarder IDs and address-space gating passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-low-window-routing-') as directory:
    directory = Path(directory)
    (directory / 'routing.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                    str(directory / 'routing.c'), '-o', str(directory / 'routing')], check=True)
    subprocess.run([str(directory / 'routing')], check=True)
