#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The Carafe builds of DXVK and FEX (carafe/dlls), from the release list to
system32: tests/carafe_dlls.c, built for this machine and run.

Without arguments nothing leaves the machine. With --live the launcher's own
code fetches this repository's release list and its newest archives and
installs them, which is what a card does; a network that cannot be reached is
reported and does not fail the check.

It needs the headers and libraries of curl, zstd and zlib. Where they are not
in the compiler's own paths, CARAFE_DLLS_CFLAGS and CARAFE_DLLS_LIBS name them.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tests = root / 'horizon-wine/tests'
compiler = os.environ.get('WINE_NX_HOST_CC', 'cc')
cflags = shlex.split(os.environ.get('CARAFE_DLLS_CFLAGS', ''))
libs = shlex.split(os.environ.get('CARAFE_DLLS_LIBS', '-lcurl -lzstd -lz'))
live = '--live' in sys.argv[1:]

with tempfile.TemporaryDirectory() as temp:
    program = Path(temp) / 'carafe_dlls'
    subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(tests / 'graphics-shims'), *cflags,
                    str(tests / 'carafe_dlls.c'), *libs, '-o', str(program)], check=True)
    env = dict(os.environ, UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1',
               # curl and the TLS library keep what they set up once for the process.
               ASAN_OPTIONS='detect_leaks=0')
    run = subprocess.run([str(program), *(['live'] if live else [])], cwd=temp, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
status, lines = run.returncode, run.stdout.splitlines()
print(run.stdout, end='')

# A run's log cannot be read from everywhere its result can; an annotation
# can, so what was downloaded, or what went wrong, is said there too.
annotate = bool(os.environ.get('GITHUB_ACTIONS'))
if live and status == 77:
    message = "Carafe's DLL releases could not be reached from here, so the download was not checked"
    for line in lines[-6:] if annotate else []:
        print(f'::warning::{line.strip()}')
    print(f'::warning::{message}' if annotate else message)
    sys.exit(0)
if annotate and (live or status):
    for line in lines[-12:]:
        print(f"::{'error' if status else 'notice'}::{line.strip()}")
sys.exit(status)
