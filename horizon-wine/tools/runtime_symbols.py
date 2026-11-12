#!/usr/bin/env python3
"""The runtime's function names, for its own log: a symbol file beside each NRO
on the card, read by source/runtime_symbols.c when a fault or a stall report
names an address in the runtime's code.

The NRO carries no symbols, and the ELF it is made from stays with the build,
so without this a crash in the runtime -- in Mesa above all -- is a bare
offset. The file is small and sorted, and read by binary search straight from
the card: a fault handler cannot count on the heap.

Layout, little-endian:
    "ARSYM1\\0\\0"
    u32 count, u32 names_size
    count x { u32 address, u32 size, u32 name_offset }, by address
    names_size bytes of NUL-terminated names

    runtime_symbols.py ELF OUT
"""
from pathlib import Path
import struct
import sys

MAGIC = b'ARSYM1\0\0'
STT_FUNC = 2


def functions(elf):
    """(address, size, name) of every function the ELF's symbol table names."""
    assert elf[:4] == b'\x7fELF' and elf[4] == 2 and elf[5] == 1, 'not a little-endian ELF64'
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', elf, 0x3a)
    sections = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize) for i in range(shnum)]
    found = {}
    for name, kind, flags, addr, offset, size, link, info, align, entsize in sections:
        if kind != 2:  # SHT_SYMTAB
            continue
        strtab = sections[link]
        strings = elf[strtab[4]:strtab[4] + strtab[5]]
        for i in range(size // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from('<IBBHQQ', elf, offset + i * 24)
            if st_info & 0xf != STT_FUNC or not st_shndx or not st_value or st_value >= 1 << 32:
                continue
            end = strings.index(b'\0', st_name)
            symbol = strings[st_name:end]
            # A global name wins over a local one at the same address.
            if st_value not in found or (st_info >> 4) == 1:
                found[st_value] = (min(st_size, 0xffffffff), symbol)
    return sorted((address, size, name) for address, (size, name) in found.items())


def write(elf_path, out_path):
    entries = functions(Path(elf_path).read_bytes())
    names = bytearray()
    table = bytearray()
    offsets = {}
    for address, size, name in entries:
        if name not in offsets:
            offsets[name] = len(names)
            names += name[:200] + b'\0'
        table += struct.pack('<III', address, size, offsets[name])
    Path(out_path).write_bytes(MAGIC + struct.pack('<II', len(entries), len(names)) + table + names)
    return len(entries)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.split('\n\n')[-1].strip())
    print(f'{sys.argv[2]}: {write(sys.argv[1], sys.argv[2])} functions')
