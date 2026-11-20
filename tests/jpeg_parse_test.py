# SPDX-License-Identifier: MIT
"""apps/jpeg's header parser on the computer (tests/jpeg_parse_host.c), with the sanitizers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as folder:
    host = Path(folder)/'host'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu99', '-g', '-Wall', '-Wextra', '-Wno-unused-const-variable',
                    '-fsanitize=address,undefined', ROOT/'tests/jpeg_parse_host.c', '-o', host], check=True)
    result = subprocess.run([host], capture_output=True, text=True, timeout=60)
    print(result.stdout, end='')
    if result.returncode:
        sys.exit(result.stderr)
