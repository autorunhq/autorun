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
# A game's 32-bit forwarder: a loader of its own in a 32-bit address space
# without the alias region (the NPDM's type 2), filed under a title id hashed
# with that address space -- never Autorun's, so the low-window patch never
# applies to it, and never the game's 39-bit one, which it sits beside.
assert re.search(r'#define NPDM_ADDRESS_SPACE_32_BIT_NO_ALIAS 2\b', forwarder)
assert 'address-space=%u\\nautorun-forwarder=%d' in forwarder
def game_title(game, space):
    key = f'{path}{path} --library-game={game}\naddress-space={space}\nautorun-forwarder={generation}'
    value = int.from_bytes(hashlib.sha256(key.encode()).digest()[:8], 'little')
    return 0x0500000000000000 | (value & 0x00FFFFFFFFFFF000)
for game in (1, 42, 4294967295):
    assert game_title(game, 3) != game_title(game, 2) and title not in (game_title(game, 3), game_title(game, 2))
# Its loader's heap fits the heap region, 2 GiB there and less than the memory
# an application has: svcSetHeapSize refuses a heap larger than its region.
hbl = (root / 'hbl/source/main.c').read_text()
heap = hbl[hbl.index('static u64 calculateMaxHeapSize(void)'):]
heap = heap[:heap.index('\n}\n')]
assert 'InfoType_HeapRegionSize' in heap and 'size = region & ~0x1FFFFFull;' in heap
assert heap.index('size -= 0x6000000;') < heap.index('InfoType_HeapRegionSize')
# In a 32-bit address space the loader keeps the NRO as high as it finds room,
# out of the way of images linked at low fixed bases.
place = hbl[hbl.index('void* map_addr = virtmemFindCodeMemory(total_size, 0);'):]
place = place[:place.index('rc = svcMapProcessCodeMemory(')]
assert 'aslr_base + aslr_size <= 0x100000000ull' in place
assert 'if ((uintptr_t)other > (uintptr_t)map_addr)' in place
# Wine there: the kernel's heap (or alias) region always covers 0x7ffe0000, so
# the shared user data goes where Wine can map a page, below 2 GB if it can;
# ntdll exports it to the PE side, and the program's own reads of 0x7ffe0000
# fault into the redirect once the heap page there is out of the runtime's heap
# and inaccessible, until the program ends. Thread data, which goes above 4 GB,
# takes the top of what there is, as native stacks already did. An update of
# Wine had lost all this, and a 32-bit forwarder's game stopped at "failed to
# map the shared user data: c0000018".
wine = (root.parent / 'dlls/ntdll/unix/virtual.c').read_text()
first = wine[wine.index('struct thread_data *virtual_alloc_first_thread_data(void)'):]
first = first[:first.index('\n}\n')]
assert 'if (status == (unsigned int)STATUS_CONFLICTING_ADDRESSES) status = relocate_user_shared_data( &view );' in first
assert 'thread_data_limit_low( &alloc_type )' in first
relocate = wine[wine.index('static NTSTATUS relocate_user_shared_data('):]
relocate = relocate[:relocate.index('\n}\n')]
assert 'limit_2g - 1' in relocate and 'limit_4g - 1' in relocate
assert relocate.index('user_shared_data = (*view)->base;') < relocate.index('horizon_hide_user_shared_data_address(')
limit = wine[wine.index('static ULONG_PTR thread_data_limit_low('):]
limit = limit[:limit.index('\n}\n')]
assert 'if ((void *)limit_4g < host_addr_space_limit) return limit_4g;' in limit and 'MEM_TOP_DOWN' in limit
data = wine[wine.index('struct thread_data *virtual_alloc_thread_data(void)'):]
data = data[:data.index('\n}\n')]
assert 'thread_data_limit_low( &alloc_type )' in data
horizon = (root.parent / 'dlls/ntdll/unix/horizon.c').read_text()
hide = horizon[horizon.index('const char *horizon_hide_user_shared_data_address( ULONG_PTR address )'):]
hide = hide[:hide.index('\n}\n')]
assert hide.index('wine_nx_native_heap_keep_out_leaving( address, granule, small_room )') < \
    hide.index('svcSetMemoryPermission( (void *)address, granule, Perm_None )')
assert 'if (horizon_redirect_user_shared_data( ctx )) horizon_resume_exception( ctx );' in horizon
for name in ('wine_nx_user_shared_data',):
    assert name in (root.parent / 'dlls/ntdll/ntdll.spec').read_text()
# And the runtime takes the game it names, --library-game=ID, as a forwarded one.
runtime = (root / 'source/runtime.c').read_text()
assert 'if (!forwarded_game) forwarded_game = runtime_command_line_game( argc, argv, target, sizeof(target) );' in runtime
back = runtime[runtime.index('static int return_to_launcher( void )'):]
back = back[:back.index('\n}\n')]
assert back.index('horizon_restore_user_shared_data_address();') < back.index('release_lent_memory();')
command_line = runtime[runtime.index('static int runtime_command_line_game('):]
command_line = command_line[:command_line.index('\n}\n')]
assert 'WINE_NX_FORWARDER_GAME_ARG' in command_line and 'return runtime_library_game( id, target, size );' in command_line

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
    /* Started from a game's 32-bit forwarder: a 32-bit game fits wherever its
     * image was linked, with or without the patch; a 64-bit one never does. */
    o.address_space_bits = 32;
    assert(address_space_fits(&l, &p));
    o.low_window = 0;
    assert(address_space_fits(&l, &p));
    p.machine = 0x8664;
    detected = LAUNCHER_ADDRESS_ANY;
    assert(!address_space_fits(&l, &p));
    p.machine = 0x014c;
    detected = LAUNCHER_ADDRESS_LOW;
    o.low_window = 1;
    o.address_space_bits = 0;
    assert(!address_space_fits(&l, &p));
    o.address_space_bits = 39;
    o.low_window = 0;
    detected = LAUNCHER_ADDRESS_ANY;
    assert(address_space_fits(&l, &p));
    puts("Low window: verified forwarder ID, 39-bit capability gating and 32-bit forwarders passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-low-window-routing-') as directory:
    directory = Path(directory)
    (directory / 'routing.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                    str(directory / 'routing.c'), '-o', str(directory / 'routing')], check=True)
    subprocess.run([str(directory / 'routing')], check=True)
