/* SPDX-License-Identifier: MIT */
/* A modded unit's SD card as a USB disk. Eject it on the computer, or press REC (SEQ REC on the SMK-37), to leave. */
#include "app.h"
#include "card.h"
#include "look.h"
#include "msc.h"
#define TICK_US 100u
#define TIMER_CON REG(0x10800u)
#define TIMER_COUNT REG(0x10804u)
#define TIMER_PERIOD REG(0x10808u)
#define TICKS_PER_MS 24000u
/* The screen and the keys, between USB commands: often enough to look alive, seldom enough not to slow it. */
#define LOOK_EVERY_MS 50u
#define SPEED_EVERY_MS 500u
#define NO_HOST_MS 120000u
#define AFTER_EJECT_US 300000u
#define SHOW_END_MS 1200u
/* What the screen may send at a time while serving the card, so the host isn't kept waiting long. */
#define SCREEN_BYTES 4096u

static unsigned lsb;
static bool screen;
static struct look_model model;

static void timer_start(void) {
    if ((TIMER_CON&0xfu) == ((2u << 2) | 1u))
        return;
    TIMER_CON = 0;
    TIMER_PERIOD = 0xffffffffu;
    TIMER_COUNT = 0;
    TIMER_CON = (2u << 2) | 1u | (1u << 14);
}

/* Milliseconds, from a timer that wraps every 179 s, so it has to be read more often than that. */
static uint32_t clock_ms, clock_ticks;

static uint32_t now_ms(void) {
    uint32_t t = TIMER_COUNT, whole = (t-clock_ticks)/TICKS_PER_MS;
    clock_ticks += whole*TICKS_PER_MS;
    clock_ms += whole;
    return clock_ms;
}

static uint32_t look_at;

static void look(void) {
    if (!screen)
        return;
    uint32_t now = now_ms();
    look_update(&model, now-look_at);
    look_at = now;
}

/* The card and the screen share SPI1: between commands the card is idle, so it can be let go. */
static void card_off(void) {
    card_close();
}

static void card_on(void) {
    card_open(lsb);
    card_fast(lsb);
}

static void wait_ms(unsigned ms) {
    uint32_t until = now_ms()+ms;
    while ((int32_t)(now_ms()-until) < 0) {
        look();
        app_feed();
        hal_delay_us(10000);
    }
}

int app_main(unsigned lsb_mhz) {
    lsb = lsb_mhz;
    timer_start();
    now_ms();
    unsigned board = app_board();
    if (!board) {
        /* Its pins aren't known, so nothing is touched. */
        app_log("which board this is isn't known");
        return 1;
    }
    board_init(board == 2);
    screen = display_init(board);
    model.state = LOOK_STARTING;
    if (screen) {
        look_start(board);
        look();
    }
    card_open(lsb_mhz);
    uint32_t sectors = card_start(lsb_mhz);
    card_close();
    if (!sectors) {
        app_log("NO CARD FOUND");
        model.state = LOOK_NO_CARD;
        wait_ms(5000);
        board_restore();
        return 1;
    }
    app_log("card: %u sectors, %u MiB", (unsigned)sectors, (unsigned)(sectors/2048));
    model.card_mib = sectors/2048;
    model.state = LOOK_WAITING;
    look();
    /* A press that opened this doesn't count as leaving. */
    while (board_keys()&KEY_RECOVERY)
        hal_delay_us(10000);
    card_on();
    look_hooks(card_off, card_on);
    look_budget(SCREEN_BYTES);
    msc_start(sectors);
    uint32_t started = now_ms(), looked = started, measured = started, read_before = 0, written_before = 0;
    bool was_configured = false, pressed = false, no_host = false;
    const struct msc_stats *stats = msc_stats();
    while (!msc_ejected()) {
        msc_poll();
        hal_delay_us(TICK_US);
        uint32_t now = now_ms();
        if (now-looked < LOOK_EVERY_MS)
            continue;
        looked = now;
        app_feed();
        if (msc_configured() && !was_configured) {
            app_log("host connected");
            was_configured = true;
            model.state = LOOK_CONNECTED;
        }
        if (now-measured >= SPEED_EVERY_MS) {
            uint32_t ms = now-measured;
            model.read_kbs = (stats->sectors_read-read_before)*500u/ms;
            model.write_kbs = (stats->sectors_written-written_before)*500u/ms;
            read_before = stats->sectors_read;
            written_before = stats->sectors_written;
            model.read_kib = stats->sectors_read/2;
            model.written_kib = stats->sectors_written/2;
            measured = now;
        }
        if (board_keys()&KEY_RECOVERY) {
            pressed = true;
            break;
        }
        if (!was_configured && now-started >= NO_HOST_MS) {
            no_host = true;
            break;
        }
        look();
    }
    /* Let the host hear the end of its last command. */
    for (unsigned n = 0; n < AFTER_EJECT_US/TICK_US; ++n) {
        msc_poll();
        hal_delay_us(TICK_US);
    }
    msc_stop();
    card_finish();
    card_close();
    look_hooks(0, 0);
    look_budget(0);
    app_log("%s: read %u KiB, wrote %u KiB, %u errors", pressed ? "REC pressed" : no_host ? "no host" : "ejected",
            (unsigned)(stats->sectors_read/2), (unsigned)(stats->sectors_written/2), (unsigned)stats->errors);
    model.read_kbs = model.write_kbs = 0;
    model.read_kib = stats->sectors_read/2;
    model.written_kib = stats->sectors_written/2;
    model.state = pressed || no_host ? LOOK_LEAVING : LOOK_EJECTED;
    wait_ms(SHOW_END_MS);
    board_restore();
    return 0;
}
