/* SPDX-License-Identifier: MIT */
/* Runs apps/slots' logic on the computer, with a card image and a flash image. See tests/slots_test.py. */
#include "slotfile.h"
#include "core.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static uint8_t *card, *flash;
static uint32_t card_sectors;
static long ops, cut_after = -1;
static const uint8_t uid[8] = {1, 2, 3, 4, 5, 6, 7, 8};

/* Power goes before this change. */
static void op(void) {
    if (ops++ == cut_after)
        _exit(42);
}

void app_feed(void) {
}

bool card_read(uint32_t sector, uint8_t *data) {
    if (sector >= card_sectors)
        abort();
    memcpy(data, card+sector*512ull, 512);
    return true;
}

bool card_write(uint32_t sector, const uint8_t *data) {
    if (sector >= card_sectors)
        abort();
    op();
    memcpy(card+sector*512ull, data, 512);
    return true;
}

bool flash_read(void *ctx, uint32_t address, void *out, uint32_t size) {
    (void)ctx;
    if (address+size > SLOT_FLASH_SIZE)
        abort();
    memcpy(out, flash+address, size);
    return true;
}

bool flash_erase(void *ctx, uint32_t address, uint32_t size) {
    (void)ctx;
    /* Only the app area, and never what's kept. */
    if (address < 0x4000 || address >= 0xff000 || (address >= 0xc0000 && address < 0xc2000) || size != 0x1000)
        abort();
    op();
    memset(flash+address, 0xff, size);
    return true;
}

bool flash_program(void *ctx, uint32_t address, const void *data, uint32_t size) {
    (void)ctx;
    if (address < 0x4000 || address >= 0xff000 || (address >= 0xc0000 && address < 0xc2000))
        abort();
    /* A cut here leaves half a sector, as pages are written in turn. */
    if (ops == cut_after) {
        for (uint32_t i = 0; i < size/2; ++i)
            flash[address+i] &= ((const uint8_t *)data)[i];
        _exit(42);
    }
    ops++;
    for (uint32_t i = 0; i < size; ++i)
        flash[address+i] &= ((const uint8_t *)data)[i];
    return true;
}

/* As the bootloader's check does, it starts from the boot area's header, which says where the firmware is. */
static bool valid(bool (*read)(void *, uint32_t, void *, uint32_t), void *ctx) {
    uint8_t head[4], boot[4];
    return read(ctx, 0, boot, 4) && memcmp(boot, "\xff\xff\xff\xff", 4) && read(ctx, 0x4000, head, 4) &&
           !memcmp(head, "GOOD", 4);
}

static uint8_t *map(const char *path, size_t *size) {
    int fd = open(path, O_RDWR);
    struct stat st;
    if (fd < 0 || fstat(fd, &st))
        exit(2);
    void *p = mmap(0, (size_t)st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        exit(2);
    *size = (size_t)st.st_size;
    return p;
}

static struct slots s;

static int named(const char *name) {
    for (unsigned i = 0; i < s.count; ++i)
        if (!strcmp(s.list[i].name, name))
            return (int)i;
    printf("no slot %s\n", name);
    exit(4);
}

static const char *shown(int index) {
    return index == NO_SLOT ? "-" : s.list[index].name;
}

static int find_id(const char id[8]) {
    for (unsigned i = 0; i < s.count; ++i)
        if (id[0] && !memcmp(s.list[i].id, id, 8))
            return (int)i;
    return NO_SLOT;
}

/* card, flash, cut after this many changes or -1, then: list | save NAME | load NAME | delete NAME */
int main(int argc, char **argv) {
    if (argc < 5)
        return 2;
    size_t size;
    card = map(argv[1], &size);
    card_sectors = (uint32_t)(size/512);
    flash = map(argv[2], &size);
    cut_after = atol(argv[3]);
    s.valid = valid;
    const char *error = slots_open(&s, card_sectors, uid, 0x856014u, APP_BOARD);
    const char *what = argv[4];
    if (!error && !strcmp(what, "save"))
        error = slots_save_new(&s, argv[5]);
    else if (!error && !strcmp(what, "load"))
        error = slots_load(&s, (unsigned)named(argv[5]));
    else if (!error && !strcmp(what, "delete"))
        error = slots_delete(&s, (unsigned)named(argv[5]));
    if (error) {
        printf("error %s %ld\n", error, ops);
        return 3;
    }
    /* Read it all again, as the unit would after a restart. */
    memset(&s, 0, sizeof(s));
    s.valid = valid;
    error = slots_open(&s, card_sectors, uid, 0x856014u, APP_BOARD);
    if (error) {
        printf("error %s\n", error);
        return 3;
    }
    int here = slots_on_unit(&s), pending = slots_pending(&s);
    printf("ok %ld\there=%s\tpending=%s\tactive=%s\tslots=", ops, shown(here), shown(pending),
           shown(find_id(s.active)));
    for (unsigned i = 0; i < s.count; ++i)
        printf("%s|", s.list[i].name);
    printf("\n");
    return 0;
}
