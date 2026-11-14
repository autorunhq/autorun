#!/usr/bin/env python3
"""The settings menu and Carafe's builds of DXVK and FEX.

The launcher lists, for a program, the options its DXVK build and its FEX
build read, and has the rows Carafe's parts came with. The settings menu
over a running game has them too (tools/make-carafe-menu.py): "DXVK build"
offers Carafe's, "FEX build" chooses between Autorun's and Carafe's, an
option shows only while the build chosen in the menu reads it, and the three
switches from Carafe (shared server threads, thread orchestrator, lock wait
boost) are rows of their own.

The menu's own reader (source/carafe_settings.c, through
tests/menu_languages.c) is asked which rows it shows for each choice; what
each build reads is asked of the code the launcher and the runtime use
(dxvk_option_supported for every version a release can have,
nx_fex_option_carafe), and the launcher's reader of a program's file says
what each new row's value turns on.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
wine = root / 'horizon-wine'

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    menu, exe = temp / 'menu.txt', temp / 'menu_languages'
    subprocess.run([sys.executable, str(wine / 'tools/make-carafe-menu.py'), str(menu)], check=True)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-D_GNU_SOURCE', '-D__SWITCH__', '-Wall', '-Wextra',
                    '-Werror', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-I', str(wine / 'source'), '-I', str(wine / 'tests/controls-shims'),
                    '-I', str(wine / 'tests/controls-shims/SDL2'), str(wine / 'tests/menu_languages.c'),
                    str(wine / 'source/input_profile.c'), str(wine / 'source/dxvk_options.c'), '-lm',
                    '-o', str(exe)], check=True)

    def ask(*arguments):
        out = subprocess.run([str(exe), str(menu)] + list(arguments), check=True, stderr=subprocess.DEVNULL,
                             stdout=subprocess.PIPE).stdout.decode('utf-8')
        found = {}
        for line in out.split('\n'):
            if line:
                fields = line.split('\t')
                found.setdefault(fields[0], []).append(fields[1:])
        return found

    plain = ask()
    shown = lambda found: {key for key, visible, _ in found['row'] if visible == '1'}
    lit = lambda found: {key for key, _, on in found['row'] if on == '1'}
    rows = [key for key, _, _ in plain['row']]
    assert len(rows) == len(set(rows)) == 141, len(rows)

    # DXVK. An option is one row; it shows while the menu's "DXVK build" is on
    # a build of which some version reads it (two builds read some options
    # from different versions on, hence the union over the table's entries).
    builds = ['-', 'sarek', 'gplasync', 'carafe']
    reads = {}
    for name, build, read in plain['dxvkreads']:
        reads.setdefault(name, set())
        if read == '1':
            reads[name].add(build)
    assert len(reads) == 81 and all(reads.values()), [name for name, found in reads.items() if not found]
    assert [text for key, text, _ in plain['value'] if key == 'dxvk-source'] == builds
    counts = {}
    for build in builds:
        found = ask('set:dxvk-source=' + build)
        visible = shown(found)
        for name, by in reads.items():
            assert (name in visible) == (build in by), (name, build, sorted(by))
        counts[build] = sum(name in visible for name in reads)
        # Nothing but DXVK's options depends on the DXVK build.
        assert visible - set(reads) == shown(plain) - set(reads), build
    assert counts['carafe'] > counts['-'] > counts['sarek'] > 0, counts
    assert reads['d3d9.effectThread'] == {'carafe'} and reads['dxvk.gplAsyncCache'] == {'gplasync'}
    assert reads['dxvk.enableDyasync'] == {'sarek'} and reads['dxvk.shaderCompilationMethod'] == {'sarek', 'carafe'}
    assert reads['dxvk.maxMemoryBudget'] == set(builds)
    assert sum(by == {'carafe'} for by in reads.values()) == 9, sorted(n for n, by in reads.items() if by == {'carafe'})

    # FEX. What only the build with Carafe's patches reads shows with "FEX
    # build" on Carafe, and that row is there while the program runs on FEX.
    only_carafe = {name for name, carafe in plain['fexcarafe'] if carafe == '1'}
    every_fex = {name for name, _ in plain['fexcarafe']}
    assert len(only_carafe) == 12 and len(every_fex) == 31 and every_fex <= set(rows)
    assert [text for key, text, _ in plain['value'] if key == 'fex-build'] == ['-', 'carafe', 'experimental']
    assert shown(plain) & every_fex == every_fex - only_carafe
    with_carafe = ask('set:fex-build=carafe')
    assert shown(with_carafe) & every_fex == every_fex
    assert shown(with_carafe) - every_fex == shown(plain) - every_fex
    # The experimental build is Carafe's with more: it reads the same options.
    assert shown(ask('set:fex-build=experimental')) == shown(with_carafe)
    assert 'fex-build' in shown(plain) and ['fex', '1'] in plain['grouprow']
    box64 = ask('set:cpu=box64')
    assert 'fex-build' not in shown(box64) and ['fex', '0'] in box64['grouprow']

    # The server's shared threads need Horizon synchronization: greyed out without.
    assert 'server-pool' in shown(plain) and 'server-pool' not in lit(plain)
    assert 'server-pool' in lit(ask('set:sync=horizon'))
    assert {'orchestrator', 'lock-boost'} <= lit(plain) & shown(plain)

    # What a row's value is to the launcher and the runtime, which read the
    # same file: orchestrator, lock boost, server pool, Carafe's FEX, DXVK build.
    nothing = ['0', '0', '0', '0', '0']
    for key, stored, want in (('orchestrator', '1', ['1', '0', '0', '0', '0']),
                              ('lock-boost', '1', ['0', '1', '0', '0', '0']),
                              ('server-pool', '1', ['0', '0', '1', '0', '0']),
                              ('fex-build', 'carafe', ['0', '0', '0', '1', '0']),
                              ('fex-build', 'experimental', ['0', '0', '0', '2', '0']),
                              ('dxvk-source', 'carafe', ['0', '0', '0', '0', '3']),
                              ('dxvk-source', 'gplasync', ['0', '0', '0', '0', '2']),
                              ('dxvk-source', 'sarek', ['0', '0', '0', '0', '1'])):
        found = ask('%s=%s' % (key, stored))
        assert found['stored'] == [[key, stored, stored]], found['stored']
        assert found['launcher'] == [[key, stored] + want], (found['launcher'], want)
        assert [text for row, text, _ in plain['value'] if row == key][0] == '-', key
    assert ask('cpu=box64')['launcher'] == [['cpu', 'box64'] + nothing]

print('settings menu: DXVK options by build %s, 12 FEX options with Carafe\'s build, 3 Carafe switches'
      % ' '.join('%s=%d' % (build.replace('-', 'official'), counts[build]) for build in builds))
