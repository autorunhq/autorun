from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='autorun-boot-tests-') as directory:
    executable = Path(directory) / 'setup-boot-test'
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        str(root / 'tests/setup_boot.c'), '-lcrypto', '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
