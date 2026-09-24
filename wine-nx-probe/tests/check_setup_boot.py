from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
crypto = root.parent / 'libs/tomcrypt/src'
with tempfile.TemporaryDirectory(prefix='autorun-boot-tests-') as directory:
    executable = Path(directory) / 'setup-boot-test'
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-DLTC_NOTHING', '-DLTC_SHA256', '-DLTC_NO_TEST', '-DARGTYPE=4',
        '-I' + str(crypto / 'headers'), str(root / 'tests/setup_boot.c'),
        str(crypto / 'hashes/sha2/sha256.c'), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
