# SPDX-License-Identifier: MIT
"""apps/wifi/wifilist.c on the computer (tests/wifi_list_host.c), with the sanitizers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as folder:
    host = Path(folder)/'host'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c99', '-D_GNU_SOURCE', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-iquote', ROOT/'apps/wifi', ROOT/'tests/wifi_list_host.c',
                    ROOT/'apps/wifi/wifilist.c', '-o', host], check=True)
    result = subprocess.run([host], capture_output=True, text=True, timeout=60)
    print(result.stdout, end='')
    if result.returncode:
        sys.exit(result.stderr)
