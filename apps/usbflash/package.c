/* SPDX-License-Identifier: MIT */
#include "package.h"
#include "core.h"
#include <string.h>
#define BLOCK 48u
#define KEPT 47u
#define MIN_PREFIX 20u
#define SENTINEL 0x7du
#define HEAD 64u
#define ENTRY 80u
#define MOST_ENTRIES 11u
#define PAGE 256u
/* How often a sector the drive gave differently from the check is read again, before giving up. */
#define TRIES 3u
#define NOT_PACKAGE "NOT A .FWSC PACKAGE"
#define BAD_PACKAGE "PACKAGE IS DAMAGED"
#define NO_FIRMWARE "NO FIRMWARE IN IT"
#define BAD_SIZE "WRONG FIRMWARE SIZE"
#define NOT_IMAGE "NOT A FLASH IMAGE"
#define WRONG_KEY "FOR ANOTHER CHIP KEY"
#define BAD_FIRMWARE "FIRMWARE FAILS CHECK"
#define READ_ERROR "DRIVE READ ERROR"
#define CHANGED "FILE READ CHANGED"
#define FLASH_ERROR "FLASH WRITE ERROR"
#define VERIFY_ERROR "FLASH VERIFY FAILED"
#define NOT_INSTALLED "INSTALLED FW FAILS"

static uint8_t sector[PKG_SECTOR] __attribute__((aligned(4)));
static uint8_t in_flash[PKG_SECTOR] __attribute__((aligned(4)));

/* The package's image is the file with byte 47 of each of the first `prefix` 48-byte blocks taken out. */
static uint32_t file_offset(const struct package *p, uint32_t at) {
    return at < p->prefix*KEPT ? at/KEPT*BLOCK+at%KEPT : at+p->prefix;
}

static bool image_read(const struct package *p, uint32_t at, uint8_t *to, uint32_t n) {
    while (n) {
        uint32_t run = n;
        if (at < p->prefix*KEPT && run > KEPT-at%KEPT)
            run = KEPT-at%KEPT;
        uint32_t from = file_offset(p, at);
        if (from > p->file.size || run > p->file.size-from || !p->file.read(p->file.ctx, from, to, run))
            return false;
        at += run;
        to += run;
        n -= run;
    }
    return true;
}

bool pkg_read(const struct package *p, uint32_t address, void *to, uint32_t n) {
    uint8_t *d = to;
    if (address > PKG_CAPACITY || n > PKG_CAPACITY-address)
        return false;
    uint32_t real = address < p->size ? p->size-address : 0;
    if (real > n)
        real = n;
    if (real && !image_read(p, p->image_offset+address, d, real))
        return false;
    memset(d+real, 0xff, n-real);
    return true;
}

static bool size_ok(uint32_t size) {
    return size > PKG_FIRST && size <= PKG_CAPACITY && !(size%PKG_SECTOR);
}

static const char *open_fwsc(struct package *p) {
    uint8_t marker, head[HEAD], entry[ENTRY];
    const struct pkg_file *f = &p->file;
    /* At least 20 blocks carry the identity, then as many more as still end in the sentinel. */
    for (p->prefix = 0;; ++p->prefix) {
        if ((p->prefix+1)*BLOCK > f->size)
            return NOT_PACKAGE;
        if (!f->read(f->ctx, p->prefix*BLOCK+KEPT, &marker, 1))
            return READ_ERROR;
        if (p->prefix >= MIN_PREFIX && marker != SENTINEL)
            break;
        unsigned length = (unsigned)strlen(p->identity);
        if (marker != SENTINEL && length == p->prefix && length+1 < PKG_IDENTITY)
            p->identity[length] = (char)(marker-p->prefix-1);
    }
    uint32_t image_size = f->size-p->prefix;
    if (!image_read(p, 0, head, HEAD))
        return READ_ERROR;
    core_enc(0xffff, head, HEAD);
    unsigned count = core_le16(head+8);
    if (core_crc16(0, head+2, HEAD-2) != core_le16(head) || !count || count > MOST_ENTRIES || core_le32(head+4) > image_size
        || HEAD+count*ENTRY > image_size)
        return NOT_PACKAGE;
    /* The list's CRC is over the entries as stored, encrypted. */
    uint16_t list_crc = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!image_read(p, HEAD+i*ENTRY, entry, ENTRY))
            return READ_ERROR;
        list_crc = core_crc16(list_crc, entry, ENTRY);
    }
    if (list_crc != core_le16(head+2))
        return BAD_PACKAGE;
    for (unsigned i = 0; i < count; ++i) {
        if (!image_read(p, HEAD+i*ENTRY, entry, ENTRY))
            return READ_ERROR;
        core_enc(0xffff, entry, ENTRY);
        if (core_le16(entry))
            continue;
        p->image_offset = core_le32(entry+8);
        p->size = core_le32(entry+12);
        p->crc = core_le16(entry+4);
        p->has_crc = true;
        if (p->image_offset > image_size || p->size > image_size-p->image_offset)
            return BAD_PACKAGE;
        return size_ok(p->size) ? 0 : BAD_SIZE;
    }
    return NO_FIRMWARE;
}

const char *pkg_open(struct package *p, enum pkg_kind kind, const struct pkg_file *file) {
    memset(p, 0, sizeof(*p));
    p->file = *file;
    p->kind = kind;
    if (kind == PKG_FWSC)
        return open_fwsc(p);
    /* A raw image: from address 0, so it starts with a flash header that decodes. */
    p->size = file->size;
    if (!size_ok(p->size))
        return file->size < PKG_FIRST || file->size > PKG_CAPACITY ? NOT_IMAGE : BAD_SIZE;
    uint8_t header[32];
    if (!pkg_read(p, 0, header, 32))
        return READ_ERROR;
    core_enc(0xffff, header, 32);
    return core_crc16(0, header+2, 30) == core_le16(header) ? 0 : NOT_IMAGE;
}

bool pkg_in_scope(uint32_t address, bool smk) {
    if (address < PKG_FIRST || address >= PKG_END)
        return false;
    return !smk || address < 0xc0000u || address >= 0xc2000u;
}

/* For core_app_mount() and core_app_check(): the image as a flash chip. */
static bool image_as_flash(void *ctx, uint32_t address, void *to, uint32_t n) {
    return pkg_read(ctx, address, to, n);
}

static bool flash_as_flash(void *ctx, uint32_t address, void *to, uint32_t n) {
    const struct pkg_flash *flash = ctx;
    return flash->read(address, to, n);
}

static void tick(const struct pkg_hooks *h, unsigned stage, uint32_t done, uint32_t total) {
    if (h->feed)
        h->feed();
    if (h->progress)
        h->progress(stage, done, total);
}

static bool firmware_ok(const struct core_flash *chip, uint16_t key, uint8_t *work) {
    struct core_app app;
    return core_app_mount(chip, key, &app) && core_app_check(chip, &app, work) == CORE_APP_OK;
}

const char *pkg_check(const struct package *p, uint16_t key, bool smk, const struct pkg_flash *flash,
                      const struct pkg_hooks *hooks, struct pkg_install *st) {
    memset(st, 0, sizeof(*st));
    st->key = key;
    st->smk = smk;
    st->end = p->size < PKG_END ? p->size : PKG_END;
    /* The firmware's first record must decrypt with this chip's key, as MIDI mode and Felucca's loader require. */
    uint8_t line[32];
    if (!pkg_read(p, PKG_FIRST, line, 32))
        return READ_ERROR;
    core_enc(key, line, 32);
    if (core_crc16(0, line+2, 30) != core_le16(line))
        return WRONG_KEY;
    /* The check the bootloader makes before starting it: every file of the firmware, by its CRC. */
    struct core_flash image = {.ctx = (void *)p, .capacity = PKG_CAPACITY, .read = image_as_flash};
    tick(hooks, PKG_CHECKING, 0, p->size);
    if (!firmware_ok(&image, key, hooks->work))
        return BAD_FIRMWARE;
    /* All of it, for the package's CRC, each sector's CRC and what differs from the flash. */
    uint16_t whole = 0;
    for (uint32_t at = 0; at < p->size; at += PKG_SECTOR) {
        if (!pkg_read(p, at, sector, PKG_SECTOR))
            return READ_ERROR;
        whole = core_crc16(whole, sector, PKG_SECTOR);
        if (at < st->end) {
            st->crcs[at/PKG_SECTOR] = core_crc16(0, sector, PKG_SECTOR);
            if (pkg_in_scope(at, smk)) {
                ++st->sectors;
                if (!flash->read(at, in_flash, PKG_SECTOR))
                    return FLASH_ERROR;
                if (memcmp(sector, in_flash, PKG_SECTOR))
                    ++st->differ;
            }
        }
        tick(hooks, PKG_CHECKING, at+PKG_SECTOR, p->size);
    }
    if (p->has_crc && whole != p->crc)
        return BAD_PACKAGE;
    return 0;
}

/* The sector at `at` from the file, as the check read it. */
static const char *fetch(const struct package *p, const struct pkg_install *st, uint32_t at) {
    const char *error = READ_ERROR;
    for (unsigned i = 0; i < TRIES; ++i) {
        if (!pkg_read(p, at, sector, PKG_SECTOR))
            continue;
        if (core_crc16(0, sector, PKG_SECTOR) == st->crcs[at/PKG_SECTOR])
            return 0;
        error = CHANGED;
    }
    return error;
}

static bool same_as_flash(const struct pkg_flash *flash, uint32_t at) {
    return flash->read(at, in_flash, PKG_SECTOR) && !memcmp(sector, in_flash, PKG_SECTOR);
}

static const char *write_sector(const struct package *p, const struct pkg_flash *flash, struct pkg_install *st,
                                uint32_t at, bool erased) {
    const char *error = fetch(p, st, at);
    if (error)
        return error;
    if (!erased && same_as_flash(flash, at))
        return 0;
    st->written = true;
    if (!erased && !flash->erase(at))
        return FLASH_ERROR;
    for (uint32_t page = 0; page < PKG_SECTOR; page += PAGE)
        if (!flash->program(at+page, sector+page, PAGE))
            return FLASH_ERROR;
    return same_as_flash(flash, at) ? 0 : VERIFY_ERROR;
}

const char *pkg_write(const struct package *p, const struct pkg_flash *flash, const struct pkg_hooks *hooks,
                      struct pkg_install *st) {
    const char *error;
    st->written = false;
    if (!st->differ)
        return 0;
    /* The first sector goes first and is written last: until the end, what's in flash can't pass as firmware. */
    st->written = true;
    if (!flash->erase(PKG_FIRST))
        return FLASH_ERROR;
    uint32_t done = 0;
    for (uint32_t at = PKG_FIRST+PKG_SECTOR; at < st->end; at += PKG_SECTOR) {
        if (!pkg_in_scope(at, st->smk))
            continue;
        if ((error = write_sector(p, flash, st, at, false)) != 0)
            return error;
        tick(hooks, PKG_WRITING, ++done, st->sectors);
    }
    if ((error = write_sector(p, flash, st, PKG_FIRST, true)) != 0)
        return error;
    tick(hooks, PKG_WRITING, st->sectors, st->sectors);
    /* And the firmware as the bootloader will see it. */
    struct core_flash chip = {.ctx = (void *)flash, .capacity = PKG_CAPACITY, .read = flash_as_flash};
    tick(hooks, PKG_VERIFYING, 0, 1);
    if (!firmware_ok(&chip, st->key, hooks->work))
        return NOT_INSTALLED;
    tick(hooks, PKG_VERIFYING, 1, 1);
    return 0;
}
