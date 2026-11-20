# SPDX-License-Identifier: MIT
"""Draw the MP3 player's screens on the computer (tests/mp3_ui_host.c), with the sanitizers. It checks the analyser's
levels on tones, and that the screen built up from updates matches one drawn at once. With an argument, it keeps the
pictures there, as PPM."""
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
                    '-fsanitize=address,undefined', '-iquote', ROOT/'apps/mp3', '-iquote', ROOT/'kit', '-iquote', ROOT/'m-bootloader/include',
                    ROOT/'tests/mp3_ui_host.c', ROOT/'apps/mp3/ui.c', ROOT/'apps/mp3/spectrum.c', ROOT/'kit/pixel.c', '-lm', '-o', host],
                   check=True)
    result = subprocess.run([host, out], capture_output=True, text=True, timeout=120)
    if result.returncode:
        sys.exit(result.stdout+result.stderr)
    assert len(list(out.glob('*.ppm'))) == 7
print('mp3 ui OK')
