/* SPDX-License-Identifier: MIT */
#include "stick.h"
#include "bot.h"
#include "husb.h"
#define USB_CON0 0x11800u
#define USB_IO_CON0 0x51000u
/* The 24 MHz timer the kit's apps keep time with (slots, usbdisk), not timer 0, which hal_delay_us() takes over. */
#define TIMER_CON 0x10800u
#define TIMER_COUNT 0x10804u
#define TIMER_PERIOD 0x10808u
#define TICKS_PER_S 24000000u
#define READY_MS 10000u
#define RETRIES 3u
#define REG32(a) (*(volatile uint32_t *)(a))
void app_feed(void);
void app_log(const char *format, ...);

/* For husb.c and bot.c, which keep time with these (m-unbricker/src/usbio.h). */
uint32_t tb_tps = TICKS_PER_S;
uint32_t tb_now(void) {
    return REG32(TIMER_COUNT);
}
uint32_t tb_ms_since(uint32_t t0) {
    return (tb_now()-t0)/(TICKS_PER_S/1000u);
}

static void timer_start(void) {
    if ((REG32(TIMER_CON)&0xfu) == ((2u << 2) | 1u))
        return;
    REG32(TIMER_CON) = 0;
    REG32(TIMER_PERIOD) = 0xffffffffu;
    REG32(TIMER_COUNT) = 0;
    REG32(TIMER_CON) = (2u << 2) | 1u | (1u << 14);
}

uint32_t stick_ms(void) {
    timer_start();
    return tb_now()/(TICKS_PER_S/1000u);
}

static struct dev_info dev;
static uint32_t sectors;
static uint8_t data[64] __attribute__((aligned(4)));

static void wait_ms(uint32_t ms) {
    uint32_t t0 = tb_now();
    while (tb_ms_since(t0) < ms)
        ;
}

static enum bot_res command(const uint8_t *cdb, unsigned cdb_len, uint8_t *buf, uint32_t len, uint32_t *moved) {
    uint32_t got = 0;
    enum bot_res r = bot_execute(cdb, cdb_len, buf, len, len != 0, &got);
    if (moved)
        *moved = got;
    return r;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* TEST UNIT READY until the drive says so, asking what's wrong after each refusal (some drives want that). */
static bool ready(void) {
    static const uint8_t test[6] = {0x00};
    static const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    uint32_t t0 = tb_now();
    while (tb_ms_since(t0) < READY_MS) {
        app_feed();
        enum bot_res r = command(test, 6, 0, 0, 0);
        if (r == B_OK)
            return true;
        if (r == B_REFUSED)
            command(sense, 6, data, 18, 0);
        else if (!husb_attached())
            return false;
        wait_ms(100);
    }
    return false;
}

const char *stick_open(uint32_t wait_ms_most) {
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    static const uint8_t capacity[10] = {0x25};
    timer_start();
    sectors = 0;
    husb_init();
    if (!husb_wait_attach(wait_ms_most))
        return "NO USB DRIVE";
    int res = 0, step = dev_enumerate(&dev, &res);
    app_log("usb: enumerate step %d result %d, %04x:%04x, endpoints %u/%u of %u", step, res, dev.vid, dev.pid,
            dev.ep_in, dev.ep_out, dev.mps);
    if (step)
        return step == 5 ? "NOT A USB DRIVE" : "USB ENUM ERROR";
    uint32_t got = 0;
    /* Direct access, such as a flash drive or a card reader with a card in it. */
    if (command(inquiry, 6, data, 36, &got) != B_OK || got < 1 || (data[0]&0x1f) != 0)
        return "NOT A USB DRIVE";
    if (!ready())
        return "DRIVE NOT READY";
    for (unsigned i = 0;; ++i) {
        if (command(capacity, 10, data, 8, &got) == B_OK && got == 8)
            break;
        if (i == RETRIES)
            return "DRIVE NOT READY";
        ready();
    }
    if (be32(data+4) != STICK_SECTOR)
        return "SECTORS NOT 512 B";
    sectors = be32(data)+1;
    app_log("usb: %u sectors", (unsigned)sectors);
    return sectors > 1 ? 0 : "DRIVE IS EMPTY";
}

uint32_t stick_sectors(void) {
    return sectors;
}

bool stick_read(uint32_t sector, uint8_t *out) {
    uint8_t cdb[10] = {0x28};
    put_be32(cdb+2, sector);
    cdb[8] = 1;
    for (unsigned i = 0; i < RETRIES; ++i) {
        uint32_t got = 0;
        if (sector < sectors && command(cdb, 10, out, STICK_SECTOR, &got) == B_OK && got == STICK_SECTOR)
            return true;
        if (!husb_attached())
            return false;
        app_log("usb: read %u failed: %s", (unsigned)sector, bot_err);
    }
    return false;
}

bool stick_refuse_write(uint32_t sector, const uint8_t *in) {
    (void)sector;
    (void)in;
    return false;
}

void stick_close(void) {
    REG32(USB_CON0) = 0;
    REG32(USB_IO_CON0) = 0;
}
