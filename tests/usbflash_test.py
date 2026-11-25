# SPDX-License-Identifier: MIT
"""Run apps/usbflash's file scan, package reader and installer on FAT32 drive images, against a simulated flash chip.

The drive images, flash images and packages are built here from m-bootloader's SPEC (§5.1, §5.2, §5.4, §5.6), not with
the code under test: a FAT32 volume with long names, firmware encrypted for a chip key, and .fwsc packages with their
identity woven in."""
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
SIZE, FIRST, END, SECTOR = 0x100000, 0x4000, 0xff000, 0x1000
KEY = 0x980f
rng = random.Random(7)


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xffff
    return crc


def enc(data, key):
    out, state = bytearray(data), key
    for i in range(len(out)):
        out[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff
    return bytes(out)


def record(name, address, length, attribute, data_crc=0xffff, index=0):
    r = bytearray(32)
    struct.pack_into('<HIIBBH', r, 2, data_crc, address, length, attribute, 0, index)
    r[16:16 + len(name)] = name
    struct.pack_into('<H', r, 0, crc16(r[2:]))
    return bytes(r)


def flash_image(key=KEY, app_length=0x30000, size=0x93000, settings=None):
    """A flash image from address 0: boot area, then firmware encrypted for `key`, then raw settings."""
    image = bytearray(rng.randbytes(size))
    header = bytearray(32)
    struct.pack_into('<HI', header, 2, 0x0220, 0x31302e30)
    struct.pack_into('<IBB', header, 8, 0xff000, 0x10, 16)
    header[16:28] = b'AC791N_STORY'
    struct.pack_into('<H', header, 0, crc16(header[2:]))
    image[0:32] = enc(header, 0xffff)
    for i, r in enumerate([record(b'uboot.boot', 0xa0, 0x100, 0), record(b'isd_config.ini', 0x3fa0, 96, 2),
                           record(b'app_dir_head', FIRST, 0xffffffff, 0x81), record(b'key_mac', 0xff000, 0x1000, 0x12)]):
        image[0x20 + 32 * i:0x40 + 32 * i] = enc(r, 0xffff)
    code = rng.randbytes(app_length)
    extent = (0x120 + app_length + 31) & ~31
    table = (record(b'app_area_head', 0x02000120, extent, 0x83) + record(b'app.bin', 0x120, app_length, 0x82, crc16(code))
             + record(b'VM', 0x93000, 0x56000, 0x12))
    plain = table + b'\xff' * (0x120 - len(table)) + code
    plain += b'\xff' * (-len(plain) % 32)
    area = b''.join(enc(plain[q:q + 32], key ^ ((q >> 2) & 0xffff)) for q in range(0, len(plain), 32))
    image[FIRST:FIRST + len(area)] = area
    if settings is not None:
        image[0x93000:0x94000] = bytes([settings]) * SECTOR
    return bytes(image)


def weave(identity, prefix, image):
    name = identity.encode('ascii')
    markers = [(c + i + 1) & 255 for i, c in enumerate(name)] + [0x7d] * (prefix - len(name))
    assert 0x7d not in markers[:len(name)]
    out = b''.join(image[i * 47:(i + 1) * 47] + bytes([m]) for i, m in enumerate(markers)) + image[prefix * 47:]
    assert out[prefix * 48 + 47] != 0x7d
    return out


def fwsc(flash_bin, identity='FM-1_901', prefix=20):
    """An M-UPGRADE package: flash.bin (type 0) and an ota.bin, as Felucca-family packages list them."""
    members = [(0, b'flash.bin', flash_bin), (0x64, b'ota.bin', rng.randbytes(5000))]
    image = bytearray(0x400)
    entries = []
    for kind, name, data in members:
        offset = len(image)
        image += data + b'\0' * (-len(data) % 32)
        e = bytearray(80)
        struct.pack_into('<HHHxxII', e, 0, kind, len(entries), crc16(data), offset, len(data))
        e[64:64 + len(name)] = name
        entries.append(enc(e, 0xffff))
    stored = b''.join(entries)
    head = bytearray(64)
    struct.pack_into('<HIHHH', head, 2, crc16(stored), len(image), len(members), 4, 0x200)
    head[16:22] = b'AC791N'
    struct.pack_into('<H', head, 0, crc16(head[2:]))
    image[0:64] = enc(head, 0xffff)
    image[64:64 + len(stored)] = stored
    return weave(identity, prefix, bytes(image))


class Drive:
    """A FAT32 volume with long names, optionally behind an MBR."""
    def __init__(self, sectors=0x8000, per_cluster=8, partition=False):
        self.base = 2048 if partition else 0
        self.total = sectors - self.base
        self.per_cluster, self.reserved, self.fats = per_cluster, 32, 2
        clusters = self.total // per_cluster
        self.fat_size = (clusters + 2) * 4 // 512 + 1
        self.data = self.base + self.reserved + self.fats * self.fat_size
        self.image = bytearray(sectors * 512)
        self.fat = [0x0ffffff8, 0x0fffffff]
        self.next = 2
        self.dirs = {}
        if partition:
            mbr = self.image
            struct.pack_into('<BBBBBBBBII', mbr, 446, 0, 0, 0, 0, 0x0c, 0, 0, 0, self.base, self.total)
            mbr[510:512] = b'\x55\xaa'
        self.root = self.mkdir(None, None)
        self.serial = 0

    def alloc(self, n):
        count = max(1, -(-n // (self.per_cluster * 512)))
        first = self.next
        for c in range(first, first + count):
            self.fat.append(c + 1 if c + 1 < first + count else 0x0fffffff)
        self.next += count
        return first

    def write(self, cluster, data):
        at = (self.data + (cluster - 2) * self.per_cluster) * 512
        self.image[at:at + len(data)] = data

    def mkdir(self, parent, name):
        cluster = self.alloc(4 * self.per_cluster * 512)
        entries = []
        if parent is not None:
            entries.append(b'.          ' + bytes([0x10]) + bytes(8) + struct.pack('<H', cluster >> 16) + bytes(4)
                           + struct.pack('<HI', cluster & 0xffff, 0))
            p = 0 if parent == self.root else parent
            entries.append(b'..         ' + bytes([0x10]) + bytes(8) + struct.pack('<H', p >> 16) + bytes(4)
                           + struct.pack('<HI', p & 0xffff, 0))
            # As a computer names it: the 8.3 name in capitals, and the long one only if that differs.
            self.entry(parent, name if name != name.upper() else None, name.upper().ljust(11).encode(), cluster, 0, 0x10)
        self.dirs[cluster] = entries
        return cluster

    def entry(self, folder, long_name, short, cluster, size, attribute=0x20):
        if short is None:
            self.serial += 1
            stem, _, ext = long_name.rpartition('.')
            short = (f'F{self.serial:05d}~1'.ljust(8) + ext.upper()[:3].ljust(3)).encode()
        assert len(short) == 11
        out = []
        if long_name is not None:
            check = 0
            for b in short:
                check = (((check & 1) << 7) + (check >> 1) + b) & 0xff
            units = [ord(c) for c in long_name]
            if len(units) % 13:
                units.append(0)
            units += [0xffff] * (-len(units) % 13)
            pieces = [units[i:i + 13] for i in range(0, len(units), 13)]
            for n in range(len(pieces), 0, -1):
                u = pieces[n - 1]
                e = bytearray(32)
                e[0] = n | (0x40 if n == len(pieces) else 0)
                for i, at in enumerate(list(range(1, 11, 2)) + list(range(14, 26, 2)) + list(range(28, 32, 2))):
                    struct.pack_into('<H', e, at, u[i])
                e[11], e[13] = 0x0f, check
                out.append(bytes(e))
        e = bytearray(32)
        e[0:11] = short
        e[11] = attribute
        struct.pack_into('<H', e, 20, cluster >> 16)
        struct.pack_into('<HI', e, 26, cluster & 0xffff, size)
        out.append(bytes(e))
        self.dirs[folder].extend(out)

    def add(self, folder, name, data, short=None):
        cluster = self.alloc(len(data))
        self.write(cluster, data)
        self.entry(folder, None if short else name, short, cluster, len(data))

    def finish(self):
        for cluster, entries in self.dirs.items():
            data = b''.join(entries)
            assert len(data) <= 4 * self.per_cluster * 512
            self.write(cluster, data)
        b = bytearray(512)
        b[0:3] = b'\xeb\x58\x90'
        b[3:11] = b'MSWIN4.1'
        struct.pack_into('<HBHBHHBHHHII', b, 11, 512, self.per_cluster, self.reserved, self.fats, 0, 0, 0xf8, 0, 63, 255,
                         self.base, self.total)
        struct.pack_into('<IHHIHH', b, 36, self.fat_size, 0, 0, self.root, 1, 6)
        b[66], b[82:90], b[510:512] = 0x29, b'FAT32   ', b'\x55\xaa'
        at = self.base * 512
        self.image[at:at + 512] = b
        info = bytearray(512)
        struct.pack_into('<I', info, 0, 0x41615252)
        struct.pack_into('<III', info, 484, 0x61417272, 0xffffffff, 0xffffffff)
        info[508:512] = b'\0\0\x55\xaa'
        self.image[at + 512:at + 1024] = info
        fat = b''.join(struct.pack('<I', v) for v in self.fat)
        assert len(fat) <= self.fat_size * 512
        for copy in range(self.fats):
            at = (self.base + self.reserved + copy * self.fat_size) * 512
            self.image[at:at + len(fat)] = fat
        return bytes(self.image)


def build_host(tmp):
    host = tmp / 'usbflash_host'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DAPP_BOARD=1',
                    f'-I{ROOT}/apps/usbflash', f'-I{ROOT}/kit', '-idirafter', f'{ROOT}/m-bootloader/include',
                    ROOT / 'tests/usbflash_host.c', ROOT / 'apps/usbflash/package.c', ROOT / 'apps/usbflash/files.c',
                    ROOT / 'kit/fat.c', ROOT / 'm-bootloader/src/core.c', '-o', host], check=True)
    return host


def run(host, drive, flash, *args, board='fm1', key=KEY, faults=None):
    r = subprocess.run([host, drive, flash, f'{key:04x}', board, *map(str, args)], capture_output=True, text=True,
                       env={**ENV, **(faults or {})})
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    return r.stdout


def result(out):
    """The lines of an install, as {'open': ..., 'check': ..., 'write': ...}, each its text and its fields."""
    lines = {}
    for line in out.splitlines():
        stage, _, rest = line.partition(': ')
        words = rest.split(' ')
        fields = dict(w.split('=', 1) for w in words if '=' in w)
        lines[stage] = (' '.join(w for w in words if '=' not in w), fields)
    return lines


def main():
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        host = build_host(tmp)
        old = bytearray(flash_image(size=SIZE, settings=1))
        old[END:] = rng.randbytes(SIZE - END)      # the unit's key area: never written
        new_bin = flash_image(app_length=0x40000)
        backup = bytearray(flash_image(size=SIZE, settings=2))
        damaged_code = bytearray(fwsc(new_bin))
        damaged_code[0x400 + 20 + FIRST + 0x200] ^= 1  # in app.bin: the firmware's own CRC fails
        damaged_vm = bytearray(fwsc(flash_image(app_length=0x20000)))
        damaged_vm[0x400 + 20 + 0x90000] ^= 1          # past the firmware, in flash.bin: only the package CRC fails
        smk_bin = flash_image(size=0xc4000)

        d = Drive()
        d.add(d.root, 'Felucca_1.8.2.fwsc', fwsc(new_bin))
        d.add(d.root, 'OLD.BIN', bytes(backup), short=b'OLD     BIN')
        d.add(d.root, 'notes.txt', b'hello')
        d.add(d.root, 'nope.fws', b'x' * 4096)
        d.add(d.root, 'other key.fwsc', fwsc(flash_image(key=0x1234)))
        d.add(d.root, 'damaged code.fwsc', bytes(damaged_code))
        d.add(d.root, 'damaged vm.fwsc', bytes(damaged_vm))
        fw = d.mkdir(d.root, 'FIRMWARE')
        d.add(fw, 'SLOOP 2.8.5 for the FM-1.fwsc', fwsc(flash_image(), identity='FM-1_928'))
        d.add(fw, 'random.bin', rng.randbytes(0x20000))
        dock = d.mkdir(d.root, 'Dock')
        d.add(dock, 'smk.fwsc', fwsc(smk_bin, identity='SMK-37 Pro_901', prefix=36))
        drive = tmp / 'drive.img'
        drive.write_bytes(d.finish())
        flash = tmp / 'flash.bin'

        def fresh(image=old):
            flash.write_bytes(bytes(image))

        # The list: / then /FIRMWARE then /DOCK, packages before images, names as a computer shows them.
        fresh()
        listed = run(host, drive, flash, 'list').splitlines()
        names = [line.split('|')[2] for line in listed[:-1]]
        assert names == ['damaged code.fwsc', 'damaged vm.fwsc', 'Felucca_1.8.2.fwsc', 'other key.fwsc', 'OLD.BIN',
                         'SLOOP 2.8.5 for the FM-1.fwsc', 'random.bin', 'smk.fwsc'], names
        assert listed[0].startswith('/|fwsc|') and listed[4].startswith('/|bin|') and listed[5].startswith('/FIRMWARE|')
        assert listed[7].startswith('/DOCK|') and listed[-1] == 'more|0', listed
        index = {n: i for i, n in enumerate(names)}

        # A package over other firmware: only [0x4000, its size) changes.
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc']))
        assert r['open'][1]['identity'] == 'FM-1_901', r
        assert r['check'][0] == 'ok' and r['write'][0] == 'ok' and r['write'][1]['written'] == '1', r
        after = flash.read_bytes()
        assert after[FIRST:len(new_bin)] == new_bin[FIRST:], 'the firmware as packaged'
        assert after[:FIRST] == old[:FIRST] and after[len(new_bin):] == old[len(new_bin):], 'the rest as it was'
        installed = after
        print('package installed')

        # The same again: nothing to write, nothing erased.
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc']))
        assert r['check'][1]['differ'] == '0' and r['write'][1]['erases'] == '0', r
        assert flash.read_bytes() == installed
        print('already installed: nothing written')

        # Refusals: nothing in flash changes.
        for name, why in [('other key.fwsc', 'FOR ANOTHER CHIP KEY'), ('damaged code.fwsc', 'FIRMWARE FAILS CHECK'),
                          ('damaged vm.fwsc', 'PACKAGE IS DAMAGED'), ('random.bin', 'NOT A FLASH IMAGE')]:
            fresh()
            r = result(run(host, drive, flash, 'install', index[name]))
            got = r['open'][0] if r['open'][0] != 'ok' else r['check'][0]
            assert got == why, (name, r)
            assert 'write' not in r and flash.read_bytes() == old, name
            print(f'{name}: refused, {why}')

        # A whole-flash backup: up to the key area, which stays the unit's.
        fresh()
        r = result(run(host, drive, flash, 'install', index['OLD.BIN']))
        assert r['write'][0] == 'ok', r
        after = flash.read_bytes()
        assert after[FIRST:END] == backup[FIRST:END] and after[:FIRST] == old[:FIRST] and after[END:] == old[END:]
        print('backup image installed, key area kept')

        # The SMK-37: 36 identity blocks, so flash.bin starts among them; its Bluetooth and calibration stay.
        smk_old = bytearray(flash_image(size=SIZE))
        fresh(smk_old)
        r = result(run(host, drive, flash, 'install', index['smk.fwsc'], board='smk37'))
        assert r['open'][1]['identity'] == 'SMK-37' and r['write'][0] == 'ok', r
        after = flash.read_bytes()
        assert after[FIRST:0xc0000] == smk_bin[FIRST:0xc0000] and after[0xc0000:0xc2000] == smk_old[0xc0000:0xc2000]
        assert after[0xc2000:0xc4000] == smk_bin[0xc2000:0xc4000] and after[0xc4000:] == smk_old[0xc4000:]
        print('SMK-37 package installed around its calibration')

        # The drive pulled while writing: the firmware's first sector is left erased, so the bootloader won't start it,
        # and installing again finishes it.
        fresh()
        clean = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc']))
        checked_reads = int(clean['check'][1]['reads'])
        fresh()
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc'],
                       faults={'USBF_READ_FAIL_AFTER': str(checked_reads + 200)}))
        assert r['write'][0] == 'DRIVE READ ERROR' and r['write'][1]['written'] == '1', r
        after = flash.read_bytes()
        assert after[FIRST:FIRST + SECTOR] == b'\xff' * SECTOR and after != old
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc']))
        assert r['write'][0] == 'ok' and flash.read_bytes() == installed, r
        print('pulled drive: firmware left unstartable, then finished by installing again')

        # A sector that reads differently once is read again.
        fresh()
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc'],
                       faults={'USBF_FLIP_AT': str(checked_reads + 100)}))
        assert r['write'][0] == 'ok' and flash.read_bytes() == installed, r
        print('a bad read during the write: read again')

        # The flash failing to program.
        fresh()
        r = result(run(host, drive, flash, 'install', index['Felucca_1.8.2.fwsc'], faults={'USBF_PROGRAM_FAIL': '0x8000'}))
        assert r['write'][0] == 'FLASH WRITE ERROR' and r['write'][1]['written'] == '1', r
        assert flash.read_bytes()[FIRST:FIRST + SECTOR] == b'\xff' * SECTOR
        print('flash failure: stopped, firmware left unstartable')

        # Behind an MBR, and a drive that isn't FAT32.
        p = Drive(partition=True)
        p.add(p.root, 'one.fwsc', fwsc(new_bin))
        drive.write_bytes(p.finish())
        assert run(host, drive, flash, 'list').splitlines()[0] == '/|fwsc|one.fwsc|%d' % len(fwsc(new_bin))
        drive.write_bytes(bytes(0x100000))
        assert run(host, drive, flash, 'list').strip() == 'drive: CARD IS NOT FAT32'
        print('partitioned drive listed; blank drive refused')
    print('usbflash OK')


main()
