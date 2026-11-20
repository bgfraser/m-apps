/* SPDX-License-Identifier: MIT */
/* C start of an app: the two ways in, from m-bootloader's docs/SPEC.md §11.8, and the guard that keeps the log
 * readable if the app hangs or crashes. */
#include "app.h"
#include "usb_midi.h"
#include <string.h>
/* From here up, the entry's argument is the ROM's pointer, not a clock. */
#define ROM_POINTER 0x01000000u
#define RETAINED ((uint8_t *)(uintptr_t)0x01c7fd80)
#define DOWNLOAD_MODE "usb_update_mode"
#define DOWNLOAD_MODE_SIZE 16u
/* P33: the watchdog. Bits 3:0 are the time, 0xf is 32 s; bit 4 enables, bit 6 clears. Clearing bit 6 of the other
 * register makes it a plain reset. */
#define WDT_CON 0x80u
#define WDT_32S 0x0fu
#define WDT_ENABLE 0x10u
#define WDT_CLEAR 0x40u
#define VLD_KEEP 0x17u
#define VLD_WDT_EXCEPTION 0x40u
/* The CPU exception: vector 1, enabled at priority 7 in bits 7:4 of the first interrupt config word. It's taken with
 * interrupts off. */
#define VECTOR_1 (*(volatile uint32_t *)0x01c7fe04u)
#define ICFG_0 (*(volatile uint32_t *)0x01eef100u)
#define ICFG_EXCEPTION 0xf0u
#define EMU_MSG (*(volatile uint32_t *)0x01eef0d4u)
#define CRASH_SHOW_S 8u
extern uint8_t __payload_end[], __bss_start[], __bss_end[];
extern const struct app_launch *app_launch_given;
static struct app_launch launch;
extern void app_crash_entry(void);

static struct {
    bool on;
    uint8_t watchdog, keep;
    uint8_t retained[DOWNLOAD_MODE_SIZE];
    uint32_t vector, icfg;
} guard;

uint32_t app_folder(void) {
    return launch.folder;
}

void app_feed(void) {
    if (guard.on)
        p33_or(WDT_CON, WDT_CLEAR);
}

void app_guard(bool on) {
    if (on == guard.on)
        return;
    guard.on = on;
    if (on) {
        memcpy(guard.retained, RETAINED, DOWNLOAD_MODE_SIZE);
        guard.watchdog = p33_read(WDT_CON);
        guard.keep = p33_read(VLD_KEEP);
        guard.vector = VECTOR_1;
        guard.icfg = ICFG_0;
        /* Any reset from here lands in download mode, which keeps RAM. */
        memcpy(RETAINED, DOWNLOAD_MODE, DOWNLOAD_MODE_SIZE);
        EMU_MSG = 0xffffffffu;
        VECTOR_1 = (uint32_t)(uintptr_t)app_crash_entry;
        ICFG_0 = guard.icfg | ICFG_EXCEPTION;
        p33_write(WDT_CON, 0);
        p33_and(VLD_KEEP, (uint8_t)~VLD_WDT_EXCEPTION);
        p33_write(WDT_CON, WDT_32S | WDT_CLEAR);
        p33_or(WDT_CON, WDT_ENABLE);
    } else {
        p33_write(WDT_CON, 0);
        p33_write(VLD_KEEP, guard.keep);
        p33_write(WDT_CON, (uint8_t)(guard.watchdog & ~WDT_ENABLE));
        ICFG_0 = guard.icfg;
        VECTOR_1 = guard.vector;
        memcpy(RETAINED, guard.retained, DOWNLOAD_MODE_SIZE);
    }
    sync_cpu();
}

/* The app's stacks may be what broke, but there's nothing better to run on. A second fault or a hang here ends with
 * the watchdog, and the log still says what happened. */
static unsigned detected;

unsigned app_detect_board(void) {
    if (detected)
        return detected;
    uint8_t mark[8];
    if (!flash_init(1, 1))
        return 0;
    if (flash_read(0, 0xc1224u, mark, 8) && !memcmp(mark, "trimdat", 8))
        detected = 2;
    else if (flash_read(0, 0xff000u, mark, 8) && !memcmp(mark, "TRIMCOPY", 8))
        detected = 2;
    else {
        uint32_t id = flash_jedec()&0xffffffu;
        if (id == 0x856014u || id == 0xb36014u)
            detected = 1;
    }
    app_log("board %u, from the flash (chip %06x)", detected, (unsigned)(flash_jedec()&0xffffffu));
    return detected;
}

void app_crash(void) __attribute__((noreturn));
void app_crash(void) {
    uint32_t cause = EMU_MSG, near, called_from;
    __asm__ volatile("%0 = reti\n\t%1 = rets" : "=r"(near), "=r"(called_from));
    /* The exception is taken a little late, so "near": seen one call after the bad read. */
    app_log("CRASH: cause %08x near %08x, last call from %08x", (unsigned)cause, (unsigned)near, (unsigned)called_from);
    app_log_crashed();
    /* The reset turns the screen off, so the message has to come first. */
    /* Only a board already known: a crash is no time to read flash. */
    if (display_init(APP_BOARD ? APP_BOARD : detected)) {
        for (unsigned row = 0; row < 15; ++row)
            display_text(row, 0xffff, "");
        display_text(1, 0xf800, "APP CRASHED");
        display_text(3, 0xffff, "SWITCH OFF AND ON");
        display_text(4, 0xffff, "TO START AGAIN");
        for (unsigned second = 0; second < CRASH_SHOW_S; ++second) {
            hal_delay_us(1000000);
            app_feed();
        }
    }
    memcpy(RETAINED, DOWNLOAD_MODE, DOWNLOAD_MODE_SIZE);
    sync_cpu();
    hal_reset();
}

int app_start(uint32_t argument);
int app_start(uint32_t argument) {
    bool from_rom = argument >= ROM_POINTER;
    /* The file's trailer is about to be cleared with .bss. It says which file the log is from. */
    uint8_t trailer[8];
    memcpy(trailer, __payload_end, sizeof trailer);
    memset(__bss_start, 0, (size_t)(__bss_end-__bss_start));
    app_log_begin(trailer);
    /* The launcher's, copied: it stays in the launcher's RAM, but the app needn't rely on that. */
    if (app_launch_given && !memcmp(app_launch_given->magic, "LNCH", 4) && app_launch_given->format == APP_LAUNCH_FORMAT)
        launch = *app_launch_given;
    if (from_rom) {
        hal_early();
        usb_midi_stop();
        hal_clock_b();
        argument = 48;
    } else
        hal_timer_clock(argument);
    app_guard(true);
    app_log("started by the %s, LSB %u MHz", from_rom ? "ROM":"caller", (unsigned)argument);
    int result = app_main(argument);
    app_log("app_main returned %d", result);
    app_log_end(result);
    app_guard(false);
    if (from_rom) {
        /* The SMK-37's panel keeps its picture through the reset, so the app's last screen would stay up as if it were
         * still running: say what it is now. Only a board already known. */
        unsigned board = APP_BOARD ? APP_BOARD : detected;
        if (board && display_init(board)) {
            for (unsigned row = 0; row < 15; ++row)
                display_text(row, 0xffff, "");
            display_text(1, 0x07e0, "THE APP HAS ENDED");
            display_text(3, 0xffff, "IN DOWNLOAD MODE");
        }
        /* Download mode keeps RAM, where firmware would overwrite the log. */
        memcpy(RETAINED, DOWNLOAD_MODE, DOWNLOAD_MODE_SIZE);
        sync_cpu();
        hal_reset();
    }
    return result;
}
