#!/usr/bin/env python3
"""Stage the opt-in dynarec NRO with the verified baseline PE payload."""
from pathlib import Path
import shutil
import subprocess
import sys
from zipfile import ZipFile, ZIP_DEFLATED

probe = Path(__file__).resolve().parents[1]
baseline = probe / 'build-switch-wow64/sd-card/switch/wine'
build = probe / 'build-switch-wow64-dynarec'
stage = build / 'sd-card/switch/wine'
verify = probe / 'tools/verify-wow64-package.py'
subprocess.run([sys.executable, str(verify), str(baseline)], check=True)
shutil.copytree(baseline, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns('.DS_Store', '*.log'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
(stage / 'README.txt').write_text('''Experimental dynarec build nx-wow64-dynarec-1.
Uses the same 7zr.exe b 1 -mmt2 -md18 benchmark as the interpreter package.
Expected: Avr:/Tot: benchmark rows, lifecycle verdict, exit_code=0x00000000.
[DYNAREC] native_entries must increase to establish generated-code execution.
Emitted bytes measure code arena allocation, not guest instruction count.
If initial JIT allocation fails, execution falls back to the interpreter.

Standalone dynarec passed hardware; this full Wine integration is unconfirmed.
Precise guest fault contexts, instruction budgets, code arena reclamation and
concurrent self-modifying code are unfinished. Use the interpreter package to
restore the previous runtime. Both packages use sdmc:/switch/wine.

The package also includes pe32-functional.exe, pe32-threads.exe and
pe32-lifecycle.exe. Select one in target.txt and remove args.txt to run it.
For archive integrity testing, args.txt can contain:
C:\\7zr.exe t C:\\7zr-sample.7z
For extraction: C:\\7zr.exe x C:\\7zr-tree.7z -oC:\\7zr-out -y
Logs: sdmc:/switch/wine/logs/autorun_runtime.log and horizon-trace.log.
''')
subprocess.run([sys.executable, str(verify), str(stage)], check=True)
archive = build / 'wine-nx-dynarec-1.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as output:
    for file in sorted(stage.rglob('*')):
        if file.is_file() and file.name != '.DS_Store' and file.suffix != '.log':
            output.write(file, file.relative_to(build / 'sd-card'))
with ZipFile(archive) as output:
    assert output.testzip() is None
print(archive)
