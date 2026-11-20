# SPDX-License-Identifier: MIT
"""Fuzzes the parsers that read what comes from a card or a file, with libFuzzer, AddressSanitizer and UBSan, for a few
seconds each (FUZZ_SECONDS). Needs clang with libFuzzer; without it, says so and passes. A crash leaves its input in
build/fuzz/<name>/, and the run fails. tests/fuzz/found/<name>/ keeps the inputs that found a bug, which every run
starts from.

  fuzz_test.py [SECONDS] [NAME...]"""
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'build'/'fuzz'
CC = os.environ.get('FUZZ_CC', 'clang')
FLAGS = ['-g', '-O1', '-std=gnu99', '-fsanitize=fuzzer,address,undefined', '-fno-sanitize-recover=all']

def jpeg_seeds(folder):
    """The sample pictures in apps/jpeg/images.h."""
    text = (ROOT/'apps/jpeg/images.h').read_text()
    for i, m in enumerate(re.finditer(r'\{([0-9,\s]+)\}', text)):
        data = bytes(int(x) for x in m.group(1).split(',') if x.strip())
        if data[:2] == b'\xff\xd8':
            (folder/f'image{i}.jpg').write_bytes(data)

def wifi_seeds(folder):
    (folder/'plain.txt').write_text('# mine\nuse=Home\n\nssid=Home\npassword=correct horse\n\nssid=Cafe\npassword=\n')
    (folder/'crlf.txt').write_bytes(b'\xef\xbb\xbf# x\r\nssid=A\r\n# its password\r\npassword=aaaaaaaa\r\nband=5\r\nuse=A')

def fat_seeds(folder):
    """A small FAT32 volume: one reserved sector, one FAT, clusters of a sector; /WIFI.TXT, a long name, and an app in
    /MBOOT/APPS/HELLO with BOOT.APP, META.TXT and ICON.QOI."""
    clusters, sector = 24, 512
    image = bytearray((2+clusters)*sector)
    boot = image
    boot[0:3] = b'\xeb\x58\x90'
    struct.pack_into('<HBHB', boot, 11, sector, 1, 1, 1)
    struct.pack_into('<IIHHIH', boot, 32, len(image)//sector, 1, 0, 0, 2, 0)
    boot[82:90] = b'FAT32   '
    boot[510:512] = b'\x55\xaa'
    fat = [0x0ffffff8, 0x0fffffff] + [0]*(sector//4-2)
    def at(c):
        return (2+c-2)*sector
    def entry(name, attr, start, size):
        return name.encode() + bytes([attr, 0, 0]) + bytes(6) + struct.pack('<H', start >> 16) + bytes(4) + \
            struct.pack('<HI', start & 0xffff, size)
    def lfn(long_name, short):
        s = 0
        for ch in short.encode():
            s = ((s >> 1) | ((s & 1) << 7)) + ch & 0xff
        units = [ord(c) for c in long_name] + [0]
        units += [0xffff]*(-len(units) % 13)
        pieces = [units[i:i+13] for i in range(0, len(units), 13)]
        out = b''
        for i in reversed(range(len(pieces))):
            u = pieces[i]
            e = bytearray(32)
            e[0] = (i+1) | (0x40 if i == len(pieces)-1 else 0)
            for k, place in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
                struct.pack_into('<H', e, place, u[k])
            e[11], e[13] = 0x0f, s
            out += bytes(e)
        return out
    def put(c, data, chain=1):
        for k in range(chain):
            fat[c+k] = c+k+1 if k < chain-1 else 0x0fffffff
            image[at(c+k):at(c+k)+sector] = data[k*sector:(k+1)*sector].ljust(sector, b'\0')
    wifi = b'use=Home\n\nssid=Home\npassword=correct horse\n'
    put(2, entry('MBOOT      ', 0x10, 3, 0) + entry('WIFI    TXT', 0x20, 4, len(wifi)) +
        lfn('A long name.txt', 'ALONGN~1TXT') + entry('ALONGN~1TXT', 0x20, 4, len(wifi)))
    put(3, entry('.          ', 0x10, 3, 0) + entry('..         ', 0x10, 0, 0) + entry('APPS       ', 0x10, 5, 0))
    put(4, wifi)
    put(5, entry('.          ', 0x10, 5, 0) + entry('..         ', 0x10, 3, 0) + entry('HELLO      ', 0x10, 6, 0))
    meta = b'name=Hello\nabout=Says hello\nversion=1.0\n'
    icon = b'qoif' + struct.pack('>II', 2, 2) + bytes([4, 0]) + bytes([0xfe, 255, 0, 0, 0xc2]) + bytes(7) + b'\x01'
    app = b'\x00'*600
    put(6, entry('.          ', 0x10, 6, 0) + entry('..         ', 0x10, 5, 0) + entry('BOOT    APP', 0x20, 7, len(app)) +
        entry('META    TXT', 0x20, 9, len(meta)) + entry('ICON    QOI', 0x20, 10, len(icon)))
    put(7, app, 2)
    put(9, meta)
    put(10, icon)
    image[sector:2*sector] = struct.pack(f'<{sector//4}I', *fat)
    (folder/'card.img').write_bytes(bytes(image))

def qoi_seeds(folder):
    for path in sorted(ROOT.glob('apps/*/ICON.QOI')):
        (folder/f'{path.parent.name}.qoi').write_bytes(path.read_bytes())

def meta_seeds(folder):
    for path in sorted(ROOT.glob('apps/*/META.TXT')):
        (folder/f'{path.parent.name}.txt').write_bytes(path.read_bytes())

def frames_seeds(folder):
    """A beacon with a name, a channel and an RSN element; a DHCP offer; an ARP request; an EAPOL key message."""
    rsn = bytes([48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0])
    beacon = bytes([0x80, 0]) + bytes(14) + bytes([0x10, 0x20, 0x30, 0x40, 0x50, 0x60]) + bytes(12) + bytes([0x11, 0]) + \
        bytes([0, 4]) + b'Home' + bytes([3, 1, 6]) + rsn
    (folder/'beacon').write_bytes(b'\x00' + beacon)
    hdr = bytes([0x08, 0x02, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60]) + bytes(8)
    llc = bytes([0xaa, 0xaa, 3, 0, 0, 0])
    dhcp = bytearray(240+12)
    dhcp[0], dhcp[1], dhcp[2] = 2, 1, 6
    dhcp[4:8] = bytes.fromhex('12345678')
    dhcp[16:20] = bytes([192, 168, 1, 5])
    dhcp[236:240] = bytes([0x63, 0x82, 0x53, 0x63])
    dhcp[240:252] = bytes([53, 1, 2, 54, 4, 192, 168, 1, 1, 255, 0, 0])
    udp = struct.pack('>HHHH', 67, 68, 8+len(dhcp), 0) + bytes(dhcp)
    ipv4 = bytearray(struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20+len(udp), 0, 0, 64, 17, 0, bytes([192, 168, 1, 1]),
                                 bytes([255, 255, 255, 255]))) + udp
    (folder/'dhcp').write_bytes(b'\x01' + hdr + llc + b'\x08\x00' + bytes(ipv4))
    dns = struct.pack('>HHHH', 53, 5353, 8+12, 0) + bytes(12)
    ipv4 = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20+len(dns), 0, 0, 64, 17, 0, bytes([192, 168, 1, 1]),
                       bytes([192, 168, 1, 5])) + dns
    (folder/'udp').write_bytes(b'\x08' + hdr + llc + b'\x08\x00' + ipv4)
    arp = struct.pack('>HHBBH', 1, 0x800, 6, 4, 1) + bytes([0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 192, 168, 1, 1]) + \
        bytes(6) + bytes([192, 168, 1, 5])
    (folder/'arp').write_bytes(b'\x08' + hdr + llc + b'\x08\x06' + arp)
    key = bytes([2, 3, 0, 95, 2, 0x00, 0x8a, 0, 16]) + bytes(8) + bytes(range(32)) + bytes(16+8+8+16+2)
    (folder/'eapol').write_bytes(b'\x00' + hdr + llc + b'\x88\x8e' + key)

FUZZERS = {
    'jpeg': (['tests/fuzz/jpeg_fuzz.c'], [], jpeg_seeds),
    'wifi_list': (['tests/fuzz/wifi_list_fuzz.c', 'apps/wifi/wifilist.c'], ['-Iapps/wifi'], wifi_seeds),
    'frames': (['tests/fuzz/frames_fuzz.c', 'apps/wifi/frames.c', 'apps/wifi/wifi_ip.c', 'apps/wifi/wifi_sta.c',
                'apps/wifi/wifi_crypto.c'], ['-Itests/fuzz/fake', '-Iapps/wifi'], frames_seeds),
    'qoi': (['tests/fuzz/qoi_fuzz.c', 'apps/picker/qoi.c'], ['-Iapps/picker'], qoi_seeds),
    'meta': (['tests/fuzz/meta_fuzz.c', 'kit/load.c', 'm-bootloader/src/core.c'],
             ['-Ikit', '-Iapps/picker', '-idirafter', 'm-bootloader/include', '-DPLATFORM_SIM'], meta_seeds),
    'fat': (['tests/fuzz/fat_fuzz.c', 'kit/fat.c', 'kit/load.c', 'apps/picker/catalog.c', 'm-bootloader/src/core.c'],
            ['-Ikit', '-Iapps/picker', '-idirafter', 'm-bootloader/include', '-DPLATFORM_SIM'], fat_seeds),
}

def main():
    args = sys.argv[1:]
    seconds = int(args.pop(0)) if args and args[0].isdigit() else int(os.environ.get('FUZZ_SECONDS', '10'))
    names = args or list(FUZZERS)
    probe = OUT/'probe'
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT/'probe.c').write_text('#include <stddef.h>\n#include <stdint.h>\nint LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { return 0; }\n')
    if not shutil.which(CC) or subprocess.run([CC, *FLAGS, OUT/'probe.c', '-o', probe], capture_output=True).returncode:
        print(f'fuzz: skipped, {CC} with libFuzzer not found')
        return
    failed = []
    for name in names:
        sources, flags, seeds = FUZZERS[name]
        folder = OUT/name
        corpus = folder/'corpus'
        corpus.mkdir(parents=True, exist_ok=True)
        seeds(corpus)
        # Inputs that found a bug once: run first, every time.
        for found in sorted((ROOT/'tests/fuzz/found'/name).glob('*')):
            shutil.copy(found, corpus/found.name)
        binary = folder/name
        subprocess.run([CC, *FLAGS, *flags, *[ROOT/s for s in sources], '-o', binary], check=True, cwd=ROOT)
        run = subprocess.run([binary, corpus, f'-max_total_time={seconds}', '-max_len=16384', '-timeout=10', f'-artifact_prefix={folder}/',
                              '-print_final_stats=1'], capture_output=True, text=True)
        stats = [l for l in run.stderr.splitlines() if l.startswith('stat::number_of_executed_units')]
        runs = stats[-1].split(':')[-1].strip() if stats else '?'
        if run.returncode:
            failed.append(name)
            print(f'fuzz {name}: FAILED after {runs} runs; the input is in {folder}/')
            print('\n'.join(l for l in run.stderr.splitlines() if 'ERROR' in l or 'SUMMARY' in l or ' #' in l[:6] or 'in ' in l[:40])[:3000])
        else:
            print(f'fuzz {name}: OK, {runs} runs in {seconds} s')
    if failed:
        sys.exit(1)

if __name__ == '__main__':
    main()
