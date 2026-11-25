/* SPDX-License-Identifier: MIT */
/* A firmware file on the drive, read as the flash image it holds, and installed with the rules of m-bootloader's MIDI
 * mode (its docs/SPEC.md §11.6): everything is read and checked before anything is erased, only [0x4000, 0xFF000) is
 * written (never the boot area or the key area), and on the SMK-37 not its Bluetooth pairing and calibration.
 *
 * Two kinds of file:
 * - .fwsc, an M-UPGRADE package (SPEC §5.6): its flash.bin, type 0, a raw flash image from address 0, encrypted for the
 *   chip key. This is what Felucca-family installers carry.
 * - .bin, a raw flash image from address 0, such as an M-Flasher backup: the same thing without the package.
 *
 * Nothing here touches hardware: the file and the flash come in as callbacks, so tests/usbflash_host.c runs it all. */
#ifndef USBFLASH_PACKAGE_H
#define USBFLASH_PACKAGE_H
#include <stdbool.h>
#include <stdint.h>
#define PKG_SECTOR 0x1000u
#define PKG_FIRST 0x4000u
#define PKG_END 0xff000u
#define PKG_CAPACITY 0x100000u
#define PKG_MAX_SECTORS (PKG_END/PKG_SECTOR)
#define PKG_IDENTITY 32u

enum pkg_kind { PKG_FWSC, PKG_BIN };

/* The file's bytes: read n bytes at offset into to. */
struct pkg_file {
    void *ctx;
    uint32_t size;
    bool (*read)(void *ctx, uint32_t offset, void *to, uint32_t n);
};

struct package {
    struct pkg_file file;
    enum pkg_kind kind;
    /* .fwsc: 48-byte blocks at the start that carry a byte of the identity each (SPEC §5.6). */
    uint32_t prefix;
    /* Where the flash image starts in the package's image (with the identity bytes taken out), and its size. */
    uint32_t image_offset, size;
    /* .fwsc: flash.bin's CRC16 over its raw bytes. */
    uint16_t crc;
    bool has_crc;
    /* .fwsc: what M-UPGRADE would read as the firmware's identity, such as FM-1_015. */
    char identity[PKG_IDENTITY];
};

/* Reads the file's header. 0 on success, otherwise what to show. */
const char *pkg_open(struct package *p, enum pkg_kind kind, const struct pkg_file *file);
/* n bytes of the flash image at flash address `address`. Past the image's end they read as erased (0xFF). */
bool pkg_read(const struct package *p, uint32_t address, void *to, uint32_t n);

/* The unit's flash. Sectors are PKG_SECTOR bytes, pages 256. */
struct pkg_flash {
    bool (*read)(uint32_t address, void *to, uint32_t n);
    bool (*erase)(uint32_t address);
    bool (*program)(uint32_t address, const void *from, uint32_t n);
};

/* What an install keeps between the check and the write. */
struct pkg_install {
    uint16_t key;
    bool smk;
    /* The last address written, exclusive: the image's size, at most PKG_END. */
    uint32_t end;
    /* Each sector's CRC16 as the check read it, so the write can tell if the drive gives something else. */
    uint16_t crcs[PKG_MAX_SECTORS];
    /* Sectors that differ from the flash, of `sectors` in scope. */
    uint32_t differ, sectors;
    /* While writing: whether anything has been erased yet, so the caller knows if the firmware may be incomplete. */
    bool written;
};

/* Stages of progress: 0 checking the file, 1 writing, 2 verifying the flash. */
enum { PKG_CHECKING, PKG_WRITING, PKG_VERIFYING };
struct pkg_hooks {
    void (*progress)(unsigned stage, uint32_t done, uint32_t total);
    /* Called often, for the watchdog. */
    void (*feed)(void);
    /* A 4 KiB workspace for core_app_check(). */
    uint8_t *work;
};

/* Reads the whole image and checks it: its size, that it's for this chip key, that its firmware passes the check the
 * bootloader makes before starting it, and the package's own CRC. Compares it with the flash. Writes nothing. */
const char *pkg_check(const struct package *p, uint16_t key, bool smk, const struct pkg_flash *flash,
                      const struct pkg_hooks *hooks, struct pkg_install *st);
/* After pkg_check(): writes what differs, the firmware's first sector last, and verifies each sector as it goes, then
 * checks the installed firmware as the bootloader will. st->written says whether the flash was changed, if it fails. */
const char *pkg_write(const struct package *p, const struct pkg_flash *flash, const struct pkg_hooks *hooks,
                      struct pkg_install *st);
/* Whether address is one an install may write. */
bool pkg_in_scope(uint32_t address, bool smk);
#endif
