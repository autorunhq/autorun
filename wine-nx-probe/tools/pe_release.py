"""Keep release DLLs free of DWARF without changing executable sections or RVAs."""
import struct
import subprocess


def runtime_image(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Invalid DOS image')
    pe, = struct.unpack_from('<I', data, 60)
    if pe > len(data) - 24 or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE image')
    machine, count, timestamp, symbols, symbol_count, optional_size, flags = struct.unpack_from('<HHIIIHH', data, pe + 4)
    optional = bytearray(data[pe + 24:pe + 24 + optional_size])
    if len(optional) != optional_size or len(optional) < 96:
        raise ValueError('Invalid PE optional header')
    sections = pe + 24 + optional_size
    if sections + count * 40 > len(data):
        raise ValueError('Invalid PE section table')
    alignment, = struct.unpack_from('<I', optional, 32)
    image_size, header_size = struct.unpack_from('<II', optional, 56)
    if (not alignment or alignment & (alignment - 1) or image_size % alignment or
            header_size < sections + count * 40 or header_size > len(data)):
        raise ValueError('Invalid PE image bounds')
    for offset in (8, 56, 60, 64):
        struct.pack_into('<I', optional, offset, 0)
    result = []
    for i in range(count):
        header = sections + i * 40
        name = data[header:header + 8].split(b'\0', 1)[0]
        if name.startswith(b'/'):
            start = symbols + symbol_count * 18 + int(name[1:])
            if start >= len(data) or b'\0' not in data[start:]:
                raise ValueError('Invalid PE section name')
            name = data[start:data.index(b'\0', start)]
        virtual_size, address, size, start = struct.unpack_from('<4I', data, header + 8)
        characteristics, = struct.unpack_from('<I', data, header + 36)
        if address % alignment or address + virtual_size > image_size:
            raise ValueError('Invalid PE section bounds')
        if name.startswith(b'.debug'):
            continue
        if start + size > len(data):
            raise ValueError('Truncated PE section')
        result.append((name, virtual_size, address, characteristics,
                       data[start:start + min(size, virtual_size or size)]))
    return data[:pe], machine, timestamp, flags & ~0x200, bytes(optional), result


def verify_release(original, release):
    if original != release and runtime_image(original) != runtime_image(release):
        raise ValueError('Release stripping changed a runtime section or PE interface')


def stage_release(source, destination, strip):
    subprocess.run([strip, '--strip-debug', '-o', str(destination), str(source)], check=True)
    try:
        verify_release(source.read_bytes(), destination.read_bytes())
    except ValueError as error:
        raise ValueError(f'{source}: {error}') from error
