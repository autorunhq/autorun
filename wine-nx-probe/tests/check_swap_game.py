from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()


def function(name):
    start = re.search(rf'^.*\b{re.escape(name)}\([^\n]*\)\n\{{', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def block(start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


parts = [
    block('struct horizon_backing\n', '\n#include "horizon_pool.h"'),
    block('static struct horizon_swap_storage swap_storage;', '\n#endif'),
    function('compare_mapping'),
    'static struct rb_tree mappings = { compare_mapping, NULL };',
    function('find_overlap_mapping'),
    block('static void swap_free_memory(', '\n#endif'),
    function('swap_resident_locked'),
    function('horizon_swap_fault'),
]
fixture = (root / 'wine-nx-probe/tests/swap_game.c').read_text()
unit = fixture.replace('/* RUNTIME_IMPLEMENTATION */', '\n'.join(parts))
with tempfile.TemporaryDirectory(prefix='autorun-game-swap-') as directory:
    unit_path = Path(directory) / 'game.c'
    binary = Path(directory) / 'game'
    unit_path.write_text(unit)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g', '-pthread',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined',
                    '-I' + str(root / 'include'), '-I' + str(root / 'dlls/ntdll/unix'),
                    str(unit_path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
