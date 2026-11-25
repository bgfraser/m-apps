/* SPDX-License-Identifier: MIT */
/* Runs apps/usbflash's file scan, package reader and installer on the computer: a FAT32 drive image stands in for the
 * USB drive, and a NOR flash chip is simulated, which refuses anything an install must never do. See
 * tests/usbflash_test.py.
 *
 *   usbflash_host DRIVE FLASH KEY BOARD list
 *   usbflash_host DRIVE FLASH KEY BOARD install INDEX
 *
 * FLASH is read, and written back after an install. Faults, from the environment: USBF_READ_FAIL_AFTER=N (drive reads
 * fail after N sectors, as if it were pulled), USBF_FLIP_AT=N (the Nth sector read comes back with one byte changed),
 * USBF_PROGRAM_FAIL=ADDRESS (programming that page fails). */
#include "files.h"
#include "package.h"
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FLASH_SIZE 0x100000u
static uint8_t *drive;
static uint32_t drive_sectors, reads, fail_after = 0xffffffffu, flip_at = 0xffffffffu, program_fail = 0xffffffffu;
static uint8_t flash[FLASH_SIZE];
static bool smk;
static unsigned erases, programs;
static struct file_list files;
static struct package package;
static struct pkg_install install;
static uint8_t work[CORE_WORK_SIZE];

void app_feed(void) {
}

/* fat.c's default volume is the card, which this never uses. */
bool card_read(uint32_t sector, uint8_t *data) {
    (void)sector;
    (void)data;
    fprintf(stderr, "read from the card\n");
    abort();
}

bool card_write(uint32_t sector, const uint8_t *data) {
    (void)sector;
    (void)data;
    fprintf(stderr, "write to the card\n");
    abort();
}

static bool drive_read(uint32_t sector, uint8_t *data) {
    if (sector >= drive_sectors) {
        fprintf(stderr, "read beyond the drive: sector %u\n", sector);
        abort();
    }
    if (++reads > fail_after)
        return false;
    memcpy(data, drive+sector*512ull, 512);
    if (reads == flip_at)
        data[100] ^= 0x40;
    return true;
}

static bool drive_write(uint32_t sector, const uint8_t *data) {
    (void)sector;
    (void)data;
    fprintf(stderr, "write to the drive\n");
    abort();
}

/* What an install may touch, said again here rather than taken from package.c. */
static void must_be_in_scope(uint32_t address, uint32_t n) {
    if (address < 0x4000u || address+n > 0xff000u || (smk && address < 0xc2000u && address+n > 0xc0000u)) {
        fprintf(stderr, "write outside the firmware's part of the flash: %#x+%#x\n", address, n);
        abort();
    }
}

static bool nor_read(uint32_t address, void *to, uint32_t n) {
    if (address > FLASH_SIZE || n > FLASH_SIZE-address)
        return false;
    memcpy(to, flash+address, n);
    return true;
}

static bool nor_erase(uint32_t address) {
    if (address%PKG_SECTOR) {
        fprintf(stderr, "erase off a sector: %#x\n", address);
        abort();
    }
    must_be_in_scope(address, PKG_SECTOR);
    memset(flash+address, 0xff, PKG_SECTOR);
    ++erases;
    return true;
}

static bool nor_program(uint32_t address, const void *from, uint32_t n) {
    if (address%256u || n != 256u) {
        fprintf(stderr, "program off a page: %#x+%#x\n", address, n);
        abort();
    }
    must_be_in_scope(address, n);
    if (address == program_fail)
        return false;
    const uint8_t *b = from;
    for (uint32_t i = 0; i < n; ++i) {
        if (flash[address+i] != 0xff) {
            fprintf(stderr, "program over unerased flash at %#x\n", address+i);
            abort();
        }
        flash[address+i] &= b[i];
    }
    ++programs;
    return true;
}

static bool file_read(void *ctx, uint32_t offset, void *to, uint32_t n) {
    return fat_read(ctx, offset, to, n);
}

static uint8_t *load(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n)
        abort();
    fclose(f);
    *size = (uint32_t)n;
    return data;
}

static uint32_t env(const char *name, uint32_t otherwise) {
    const char *v = getenv(name);
    return v ? (uint32_t)strtoul(v, 0, 0) : otherwise;
}

int main(int argc, char **argv) {
    if (argc < 6)
        return 2;
    uint32_t size;
    drive = load(argv[1], &size);
    if (!drive || size%512u)
        return 2;
    drive_sectors = size/512u;
    uint32_t flash_size;
    uint8_t *f = load(argv[2], &flash_size);
    if (!f || flash_size != FLASH_SIZE)
        return 2;
    memcpy(flash, f, FLASH_SIZE);
    free(f);
    uint16_t key = (uint16_t)strtoul(argv[3], 0, 16);
    smk = !strcmp(argv[4], "smk37");
    fail_after = env("USBF_READ_FAIL_AFTER", fail_after);
    flip_at = env("USBF_FLIP_AT", flip_at);
    program_fail = env("USBF_PROGRAM_FAIL", program_fail);

    fat_source(drive_read, drive_write);
    const char *error = fat_mount(drive_sectors);
    if (!error)
        error = files_scan(&files);
    if (error) {
        printf("drive: %s\n", error);
        return 0;
    }
    if (!strcmp(argv[5], "list")) {
        for (unsigned i = 0; i < files.count; ++i)
            printf("%s|%s|%s|%u\n", files.items[i].folder, files.items[i].kind == PKG_FWSC ? "fwsc" : "bin",
                   files.items[i].name, files.items[i].file.size);
        printf("more|%d\n", files.more);
        return 0;
    }
    if (strcmp(argv[5], "install") || argc != 7)
        return 2;
    unsigned index = (unsigned)atoi(argv[6]);
    if (index >= files.count)
        return 2;
    struct item *it = files.items+index;
    struct pkg_file file = {.ctx = &it->file, .size = it->file.size, .read = file_read};
    const struct pkg_flash nor = {.read = nor_read, .erase = nor_erase, .program = nor_program};
    const struct pkg_hooks hooks = {.work = work};
    error = pkg_open(&package, it->kind, &file);
    if (error) {
        printf("open: %s\n", error);
        return 0;
    }
    printf("open: ok identity=%s size=%u\n", package.identity, package.size);
    error = pkg_check(&package, key, smk, &nor, &hooks, &install);
    printf("check: %s differ=%u sectors=%u reads=%u\n", error ? error : "ok", install.differ, install.sectors, reads);
    if (!error) {
        error = pkg_write(&package, &nor, &hooks, &install);
        printf("write: %s written=%d erases=%u programs=%u\n", error ? error : "ok", install.written, erases, programs);
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(flash, 1, FLASH_SIZE, out) != FLASH_SIZE)
        return 2;
    fclose(out);
    return 0;
}
