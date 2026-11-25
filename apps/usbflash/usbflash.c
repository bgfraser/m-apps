/* SPDX-License-Identifier: MIT */
/* Installs firmware from a USB flash drive, with no computer: the unit becomes a USB host, lists the .fwsc packages and
 * raw .bin flash images on a FAT32 drive, and writes the one chosen, by the rules of m-bootloader's MIDI mode
 * (package.h). The drive needs 5 V from a Y-cable with a power injector. Once anything is written it resets rather than
 * returning, so the bootloader checks what's in flash: firmware left incomplete opens its menu and MIDI mode. */
#include "app.h"
#include "core.h"
#include "fat.h"
#include "files.h"
#include "package.h"
#include "stick.h"
#include <string.h>
#define WHITE 0xffffu
#define GREY 0x8410u
#define GREEN 0x07e0u
#define YELLOW 0xffe0u
#define RED 0xf800u
#define CYAN 0x07ffu
#define SELECTED 0x001fu
#define ROWS 15u
#define COLUMNS 20u
#define LIST_TOP 2u
#define LIST_ROWS 10u
#define ATTACH_MS 3000u
#define HOLD_MS 2000u
#define POLL_MS 10u

static struct file_list files;
static struct package package;
static struct pkg_install install;
static uint8_t work[CORE_WORK_SIZE] __attribute__((aligned(4)));
static uint16_t chip_key;
static unsigned keys_down;
static unsigned last_percent;

/* --------------------------------------------------------------------------------------------------- the screen */

/* The screen has A-Z 0-9 - . / : < > + ? and space; lowercase shows as capitals. */
static char screen_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')
        return c;
    if (c == '-' || c == '.' || c == '/' || c == ':' || c == '<' || c == '>' || c == '+' || c == '?')
        return c;
    return '-';
}

static void row(unsigned r, uint16_t fg, uint16_t bg, const char *text) {
    char line[COLUMNS];
    unsigned n = 0;
    for (; n < COLUMNS && text[n]; ++n)
        line[n] = screen_char(text[n]);
    display_row(r, fg, bg, line, n);
}

static void text(unsigned r, uint16_t fg, const char *s) {
    row(r, fg, 0, s);
}

static void clear(unsigned from, unsigned to) {
    for (unsigned r = from; r < to; ++r)
        text(r, WHITE, "");
}

static void title(const char *s) {
    text(0, GREEN, s);
    clear(1, ROWS);
}

static char *append(char *at, const char *s) {
    while (*s)
        *at++ = *s++;
    *at = 0;
    return at;
}

static char *append_number(char *at, uint32_t v) {
    char digits[10];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0'+v%10);
        v /= 10;
    } while (v);
    while (n)
        *at++ = digits[--n];
    *at = 0;
    return at;
}

static char *append_hex4(char *at, unsigned v) {
    for (int shift = 12; shift >= 0; shift -= 4)
        *at++ = "0123456789ABCDEF"[(v >> shift)&15u];
    *at = 0;
    return at;
}

/* A long name over up to `rows` rows. */
static void wrapped(unsigned r, unsigned rows, uint16_t fg, const char *s) {
    size_t n = strlen(s);
    for (unsigned i = 0; i < rows; ++i)
        text(r+i, fg, n > i*COLUMNS ? s+i*COLUMNS : "");
}

static void progress(unsigned stage, uint32_t done, uint32_t total) {
    unsigned percent = total ? (unsigned)(done*100u/total) : 0;
    (void)stage;
    if (percent == last_percent)
        return;
    last_percent = percent;
    display_progress(percent, stage == PKG_WRITING ? YELLOW : CYAN);
}

static void start_progress(unsigned stage) {
    last_percent = 101;
    progress(stage, 0, 1);
}

/* ------------------------------------------------------------------------------------------------------ keys */

/* The next key to go down: KEY_PREV (OCT-), KEY_NEXT (OCT+), KEY_SELECT (PLAY) or KEY_RECOVERY (REC). */
static unsigned next_key(void) {
    for (;;) {
        unsigned now = board_keys(), pressed = now&~keys_down;
        keys_down = now;
        if (pressed)
            return pressed&KEY_RECOVERY ? KEY_RECOVERY : pressed&KEY_SELECT ? KEY_SELECT : pressed&KEY_PREV ? KEY_PREV
                                                                                                              : KEY_NEXT;
        hal_delay_us(POLL_MS*1000u);
        app_feed();
    }
}

/* PLAY held for HOLD_MS, with the bar filling; false if it comes up first or REC goes down. */
static bool held(void) {
    start_progress(PKG_WRITING);
    for (unsigned ms = 0; ms < HOLD_MS; ms += POLL_MS) {
        keys_down = board_keys();
        if (!(keys_down&KEY_SELECT) || keys_down&KEY_RECOVERY)
            return false;
        progress(PKG_WRITING, ms, HOLD_MS);
        hal_delay_us(POLL_MS*1000u);
        app_feed();
    }
    return true;
}

/* -------------------------------------------------------------------------------------------- the drive's files */

static bool same(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && !memcmp(a, b, n);
}

/* kit/fat.c speaks of a card. */
static const char *drive_words(const char *error) {
    if (same(error, "CARD READ ERROR"))
        return "DRIVE READ ERROR";
    if (same(error, "CARD IS NOT FAT32"))
        return "NOT FAT32";
    if (same(error, "BAD FOLDER ON CARD"))
        return "BAD FOLDER ON DRIVE";
    return error;
}

static const char *connect(void) {
    title("USB FLASHER");
    text(2, WHITE, "LOOKING FOR A DRIVE");
    text(4, GREY, "FAT32 USB DRIVE WITH");
    text(5, GREY, "5V FROM A Y-CABLE");
    const char *error = stick_open(ATTACH_MS);
    if (error)
        return error;
    text(2, WHITE, "READING THE DRIVE");
    fat_source(stick_read, stick_refuse_write);
    error = fat_mount(stick_sectors());
    if (!error)
        error = files_scan(&files);
    return error ? drive_words(error) : 0;
}

/* -------------------------------------------------------------------------------------------------- the pages */

static void show_list(unsigned selection) {
    char line[COLUMNS+1], *at = line;
    title("USB FLASHER");
    at = append(at, "KEY ");
    at = append_hex4(at, chip_key);
    at = append(at, "  FILES ");
    at = append_number(at, files.count);
    if (files.more)
        append(at, "+");
    text(1, GREY, line);
    unsigned first = selection >= LIST_ROWS ? selection-LIST_ROWS+1 : 0;
    for (unsigned r = 0; r < LIST_ROWS && first+r < files.count; ++r) {
        bool on = first+r == selection;
        row(LIST_TOP+r, on ? WHITE : files.items[first+r].kind == PKG_FWSC ? WHITE : GREY, on ? SELECTED : 0,
            files.items[first+r].name);
    }
    if (!files.count) {
        text(3, YELLOW, "NO .FWSC OR .BIN IN");
        text(4, YELLOW, "/ /FIRMWARE /DOCK");
    }
    text(13, GREY, "OCT- OCT+: CHOOSE");
    text(14, GREY, files.count ? "PLAY: OPEN  REC: OUT" : "PLAY: AGAIN REC: OUT");
}

static void show_file(const struct item *it) {
    char line[COLUMNS+1], *at = line;
    title(it->kind == PKG_FWSC ? "FIRMWARE PACKAGE" : "RAW FLASH IMAGE");
    wrapped(2, 3, WHITE, it->name);
    at = append(at, "IN ");
    append(at, it->folder);
    text(5, GREY, line);
}

static void show_details(void) {
    char line[COLUMNS+1], *at = line;
    if (package.kind == PKG_FWSC && package.identity[0]) {
        at = append(at, "ID ");
        append(at, package.identity);
        text(6, WHITE, line);
        at = line;
    }
    at = append(at, "FIRMWARE ");
    at = append_number(at, package.size/1024u);
    append(at, " KB");
    text(7, WHITE, line);
}

/* What was written, if anything, decides how to leave: a changed flash is the bootloader's to check. */
static void leave(bool restart) {
    stick_close();
    if (restart) {
        text(12, WHITE, "RESTARTING");
        hal_delay_us(1500000);
    }
    board_restore();
    if (restart) {
        app_guard(false);
        sync_cpu();
        hal_reset();
    }
}

static bool unit_read(uint32_t address, void *to, uint32_t n) {
    return flash_read(0, address, to, n);
}
static bool unit_erase(uint32_t address) {
    return flash_erase(0, address, PKG_SECTOR);
}
static bool unit_program(uint32_t address, const void *from, uint32_t n) {
    return flash_program(0, address, from, n);
}
static const struct pkg_flash unit_flash = {.read = unit_read, .erase = unit_erase, .program = unit_program};

static bool file_read(void *ctx, uint32_t offset, void *to, uint32_t n) {
    return fat_read(ctx, offset, to, n);
}

/* One file: check it, then write it if PLAY is held. *restart is set once writing starts; true means leave now. */
static bool open_file(struct item *it, bool *restart) {
    const struct pkg_hooks hooks = {.progress = progress, .feed = app_feed, .work = work};
    struct pkg_file file = {.ctx = &it->file, .size = it->file.size, .read = file_read};
    char line[COLUMNS+1], *at = line;
    show_file(it);
    const char *error = pkg_open(&package, it->kind, &file);
    if (!error) {
        show_details();
        text(8, CYAN, "CHECKING THE FILE");
        start_progress(PKG_CHECKING);
        error = pkg_check(&package, chip_key, APP_BOARD == 2, &unit_flash, &hooks, &install);
        app_log("check %s: %s, %u of %u sectors differ", it->name, error ? error : "ok", (unsigned)install.differ,
                (unsigned)install.sectors);
    }
    if (error) {
        text(8, RED, error);
        if (same(error, "FOR ANOTHER CHIP KEY")) {
            at = append(at, "THIS UNIT IS ");
            append_hex4(at, chip_key);
            text(11, GREY, line);
        }
        text(14, GREY, "ANY KEY: BACK");
        next_key();
        return false;
    }
    text(8, GREEN, "FILE IS GOOD");
    if (!install.differ) {
        text(11, GREEN, "ALREADY INSTALLED");
        text(14, GREY, "ANY KEY: BACK");
        next_key();
        return false;
    }
    at = append_number(at, install.differ);
    at = append(at, " OF ");
    at = append_number(at, install.sectors);
    append(at, " TO WRITE");
    text(11, WHITE, line);
    text(13, YELLOW, "HOLD PLAY: WRITE");
    text(14, GREY, "REC: BACK");
    for (;;) {
        unsigned key = next_key();
        if (key == KEY_RECOVERY)
            return false;
        if (key == KEY_SELECT && held())
            break;
        display_rect(0, 144, 240, 32, 0);
    }
    /* From here on, it resets rather than returns: nothing may cut a write short. */
    app_guard(false);
    *restart = true;
    for (;;) {
        title("WRITING FIRMWARE");
        wrapped(2, 2, WHITE, it->name);
        text(5, YELLOW, "DO NOT SWITCH OFF");
        text(6, YELLOW, "OR UNPLUG THE DRIVE");
        start_progress(PKG_WRITING);
        error = pkg_write(&package, &unit_flash, &hooks, &install);
        app_log("write: %s, %s", error ? error : "ok", install.written ? "flash changed" : "flash as it was");
        if (!error) {
            title("DONE");
            wrapped(2, 2, WHITE, it->name);
            text(5, GREEN, "INSTALLED, CHECKED");
            leave(true);
        }
        title("WRITE FAILED");
        text(2, RED, error);
        if (install.written) {
            text(4, YELLOW, "FIRMWARE INCOMPLETE");
            text(5, GREY, "TRY AGAIN, OR USE");
            text(6, GREY, "MIDI MODE/M-FLASHER");
        }
        text(13, YELLOW, "PLAY: TRY AGAIN");
        text(14, GREY, "REC: RESTART");
        unsigned key;
        while ((key = next_key()) != KEY_SELECT && key != KEY_RECOVERY)
            ;
        if (key == KEY_RECOVERY)
            return true;
    }
}

int app_main(unsigned lsb_mhz) {
    (void)lsb_mhz;
    if (APP_BOARD != 1 && APP_BOARD != 2)
        return 1;
    board_init(APP_BOARD == 2);
    if (!display_init(APP_BOARD)) {
        board_restore();
        return 1;
    }
    if (!flash_init(1, 1)) {
        title("USB FLASHER");
        text(2, RED, "FLASH ERROR");
        hal_delay_us(3000000);
        board_restore();
        return 1;
    }
    chip_key = (uint16_t)(hal_efuse(1, 3) >> 8);
    app_log("usbflash on board %d, chip key %04x", APP_BOARD, chip_key);
    /* The key that started this, if still held, doesn't count. */
    keys_down = board_keys();
    bool restart = false;
    for (;;) {
        const char *error = connect();
        app_log("drive: %s, %u files", error ? error : "ok", files.count);
        if (error) {
            text(8, RED, error);
            text(13, GREY, "PLAY: TRY AGAIN");
            text(14, GREY, "REC: OUT");
            if (next_key() == KEY_RECOVERY)
                break;
            continue;
        }
        unsigned selection = 0;
        bool again = false;
        while (!again) {
            show_list(selection);
            unsigned key = next_key();
            if (key == KEY_RECOVERY) {
                leave(restart);
                return 0;
            }
            if (key == KEY_PREV && files.count)
                selection = selection ? selection-1 : files.count-1;
            else if (key == KEY_NEXT && files.count)
                selection = selection+1 < files.count ? selection+1 : 0;
            else if (key == KEY_SELECT) {
                if (!files.count)
                    again = true;
                else if (open_file(files.items+selection, &restart) && restart)
                    leave(true);
            }
        }
    }
    leave(restart);
    return 0;
}
