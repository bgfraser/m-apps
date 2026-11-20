# SPDX-License-Identifier: MIT
"""Draw usbdisk's screens on the computer (tests/usbdisk_look_host.c), with the sanitizers, for both boards. With an
argument, it keeps the pictures there, as PPM."""
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
                    '-fsanitize=address,undefined', '-iquote', ROOT/'apps/usbdisk', '-iquote', ROOT/'kit', '-iquote',
                    ROOT/'m-bootloader/include', ROOT/'tests/usbdisk_look_host.c', ROOT/'apps/usbdisk/look.c', ROOT/'kit/canvas.c', '-o', host], check=True)
    result = subprocess.run([host, out], capture_output=True, text=True, timeout=120)
    if result.returncode:
        sys.exit(result.stdout+result.stderr)
    shots = sorted(out.glob('*.ppm'))
    assert len(shots) == 14, len(shots)
    # Reading and writing move the packets, so those screens differ from connected and from each other.
    data = {p.stem: p.read_bytes() for p in shots}
    for board in '12':
        assert len({data[f'{board}-2-connected'], data[f'{board}-3-reading'], data[f'{board}-4-writing']}) == 3
print('usbdisk look OK')
