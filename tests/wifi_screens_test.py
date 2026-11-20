# SPDX-License-Identifier: MIT
"""Draw the WiFi app's screens on the computer (tests/wifi_screens_host.c), with the sanitizers, for both boards. With
an argument, it keeps the pictures there, as PPM."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as folder:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(folder)
    out.mkdir(parents=True, exist_ok=True)
    host = Path(folder)/'host'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c99', '-D_DEFAULT_SOURCE', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-iquote', ROOT/'apps/wifi', '-iquote', ROOT/'kit', '-iquote',
                    ROOT/'m-bootloader/include', ROOT/'tests/wifi_screens_host.c', ROOT/'apps/wifi/screens.c',
                    ROOT/'apps/wifi/wifilist.c', ROOT/'kit/canvas.c', ROOT/'kit/textentry.c', '-o', host], check=True)
    result = subprocess.run([host, out], capture_output=True, text=True, timeout=120)
    if result.returncode:
        sys.exit(result.stdout+result.stderr)
    assert len(list(out.glob('*.ppm'))) == 42, len(list(out.glob('*.ppm')))
print('wifi screens OK')
