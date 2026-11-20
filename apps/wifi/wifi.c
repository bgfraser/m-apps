/* SPDX-License-Identifier: MIT */
/* Keeps the WiFi networks the card's apps join, in /MBOOT/WIFI.TXT (wifilist.h), and tests them: adding one from those
 * heard, or by its name, with its password typed on the piano keys (kit/textentry.c), choosing the one to use, and
 * joining it to see it works (net.c). */
#include "app.h"
#include "canvas.h"
#include "card.h"
#include "fat.h"
#include "keys.h"
#include "net.h"
#include "screens.h"
#include "textentry.h"
#include "wifilist.h"
#include <string.h>
#define POLL_US 10000u
#define DELETE_HOLD_MS 1500u
#define BLINK_MS 500u
#define MOST_HEARD 24u
#define TIMER_COUNT REG(0x10804u)
#define TICKS_PER_MS 24000u

static const char FOLDER[11] = {'M', 'B', 'O', 'O', 'T', ' ', ' ', ' ', ' ', ' ', ' '};
static const char FILE_NAME[11] = {'W', 'I', 'F', 'I', ' ', ' ', ' ', ' ', 'T', 'X', 'T'};
static const char NO_CARD[] = "No card found";

static struct wifi_list list;
static struct net_ap heard[MOST_HEARD];
static unsigned heard_count;
static char text[4096] __attribute__((aligned(4)));
static struct keys keys;
static unsigned board, lsb;
static bool card_ok;

/* ---- the clock ---- */

/* The datasheet rates the AC7911B for 320 MHz. m-bootloader, as M-VAVE's FM-1 firmware up to V15, starts an app from
 * the card at 360 (PLL1 2/3 of 540), and there, with the radio on, the key's hashing went wrong in the store
 * experiment. V16 went back to 320. So from 360, the system clock moves to 2/3 of PLL0's 480 MHz, 320, with LSB 53, and
 * back before returning. */
static unsigned clock_byte, given_lsb;

static void clock_set(unsigned byte) {
    REG(0x1000c) &= ~BIT(8);
    sync_cpu();
    REG(0x10014) = (REG(0x10014)&~255u)|byte;
    sync_cpu();
    REG(0x1000c) |= BIT(8);
    sync_cpu();
}

static unsigned clock_320(unsigned lsb_mhz) {
    given_lsb = lsb_mhz;
    clock_byte = REG(0x10014)&255u;
    if (clock_byte != 6u)
        return lsb_mhz;
    clock_set(2u);
    hal_timer_clock(53);
    return 53;
}

static void clock_back(void) {
    if (clock_byte != 6u)
        return;
    clock_set(6u);
    hal_timer_clock(given_lsb);
}

/* ---- the card ---- */

/* The card and the panel share SPI1: drawing steps off the card, and back on. */
static void card_on(void) {
    card_open(lsb);
    card_fast(lsb);
}

/* Reads WIFI.TXT, if there is one: what to say about it, or 0. */
static const char *read_list(void) {
    memset(&list, 0, sizeof list);
    card_open(lsb);
    uint32_t sectors = card_start(lsb);
    const char *note = NO_CARD;
    if (sectors) {
        note = fat_mount(sectors);
        if (!note) {
            card_ok = true;
            uint32_t folder = fat_root();
            struct fat_entry file;
            bool found = false;
            if (!fat_folder(&folder, FOLDER, "") && !fat_find(folder, FILE_NAME, &file, &found) && found) {
                uint32_t n = file.size < sizeof text ? file.size : sizeof text;
                if (!fat_read(&file, 0, text, n))
                    note = "WIFI.TXT couldn't be read";
                else
                    wifi_list_read(&list, text, n);
            } else {
                note = "No WIFI.TXT yet";
            }
        }
    }
    card_close();
    if (!note && list.skipped)
        note = "Some of WIFI.TXT was left out";
    app_log("WIFI.TXT: %u networks, use %u, %u left out: %s", list.count, list.use, list.skipped, note ? note : "ok");
    return note;
}

/* Writes the list to WIFI.TXT, the old one replaced only once the new one is whole: 0, or what went wrong. */
static const char *save_list(void) {
    unsigned n = wifi_list_write(&list, text, WIFI_TEXT_MOST);
    if (!n)
        return "Too much to save";
    unsigned whole = (n+511u)&~511u;
    memset(text+n, 0, whole-n);
    card_on();
    uint32_t folder = fat_root(), start = 0;
    struct fat_entry old;
    bool found = false;
    const char *error = fat_make_folder(&folder, FOLDER);
    if (!error)
        error = fat_find(folder, FILE_NAME, &old, &found);
    if (!error)
        error = fat_reserve(n, &start);
    if (!error) {
        if (!fat_write(start, 0, text, whole))
            error = "The card didn't take it";
        else
            error = found ? fat_swap(folder, FILE_NAME, start, n) : fat_add(folder, FILE_NAME, FAT_ATTR_ARCHIVE, start, n);
        if (error)
            fat_release(start);
    }
    if (!error)
        error = fat_commit();
    card_close();
    app_log("saved WIFI.TXT, %u bytes: %s", n, error ? error : "ok");
    return error;
}

static void leave_card(void) {
    card_open(lsb);
    card_finish();
    card_close();
}

/* ---- keys ---- */

/* Waits for a key, scanning the matrix. tick, if given, runs each poll with the ms since the last. */
static void next_press(void (*tick)(unsigned ms)) {
    for (;;) {
        hal_delay_us(POLL_US);
        app_feed();
        keys_scan(&keys);
        if (tick)
            tick(POLL_US/1000);
        if (keys.piano_pressed || keys.buttons_pressed)
            return;
    }
}

static bool pressed(uint32_t key) {
    return keys.buttons_pressed&key;
}

/* ---- typing ---- */

static char typed[WIFI_PASS_MOST+1];
static unsigned typed_most, blink_ms;
static const char *type_title, *type_network, *type_placeholder, *type_note;

static void draw_type(void) {
    screen_type(board, type_title, type_network, typed, type_placeholder, text_layer(keys.buttons), keys.piano,
                blink_ms < BLINK_MS, type_note);
    canvas_send_all();
}

/* Redraws while the layer keys change, which are held rather than pressed, and blinks the cursor. */
static void type_tick(unsigned ms) {
    static uint32_t shown;
    uint32_t layers = keys.buttons&(KEYS_OCT_DOWN | KEYS_OCT_UP);
    bool was = blink_ms < BLINK_MS;
    blink_ms = (blink_ms+ms)%(2*BLINK_MS);
    if (layers != shown || (blink_ms < BLINK_MS) != was) {
        shown = layers;
        draw_type();
    }
}

/* Typing into typed, which holds the text to start from: false if cancelled. ok() says what's wrong, or 0. */
static bool type(const char *title, const char *network, const char *placeholder, unsigned most,
                 const char *(*ok)(const char *)) {
    type_title = title;
    type_network = network;
    type_placeholder = placeholder;
    type_note = 0;
    typed_most = most;
    blink_ms = 0;
    draw_type();
    for (;;) {
        next_press(type_tick);
        if (pressed(KEYS_REC))
            return false;
        type_note = 0;
        if (pressed(KEYS_PLAY)) {
            type_note = ok(typed);
            if (!type_note)
                return true;
        }
        text_type(typed, typed_most, keys.piano_pressed, text_layer(keys.buttons));
        blink_ms = 0;
        draw_type();
    }
}

static const char *name_ok(const char *s) {
    return wifi_ssid_ok(s) ? 0 : "Type its name";
}

/* Empty for an open network. */
static const char *pass_ok(const char *s) {
    unsigned n = (unsigned)strlen(s);
    return n && n < WIFI_PASS_LEAST ? "At least 8 characters" : n > WIFI_PASS_MOST ? "At most 63 characters" : 0;
}

/* ---- testing ---- */

static const char *test_ssid;

static void show_step(unsigned step) {
    screen_test(board, test_ssid, step, false, 0, &(struct net_result){0}, "Testing", CANVAS_DIM, "");
    canvas_send_all();
}

/* Joins, showing each step: why it failed, or 0. r->step is where it got to. */
static const char *test(const char *ssid, const char *pass, struct net_result *r) {
    test_ssid = ssid;
    const char *why = net_join(ssid, pass, show_step, r);
    net_leave();
    app_log("test %s: %s", ssid, why ? why : "joined");
    return why;
}

/* ---- adding, and changing a password ---- */

/* The password typed for ssid, tested, and saved: true once saved. In typed: the password to start from. An open
 * network heard (ask false) has none to type. */
static bool password_and_save(const char *ssid, bool ask, const char **note) {
    for (;;) {
        text_all_symbols(1);
        if (ask && !type("Password", ssid, "None for an open network", WIFI_PASS_MOST, pass_ok))
            return false;
        struct net_result r;
        const char *why = test(ssid, typed, &r);
        if (!why) {
            int i = wifi_list_put(&list, ssid, typed);
            const char *error = i < 0 ? "The list is full" : save_list();
            screen_test(board, ssid, NET_STEPS, true, 0, &r, error ? error : "Saved: apps join it",
                        error ? CANVAS_RED : CANVAS_GREEN, board == 2 ? "SEQ PLAY OK" : "PLAY OK");
            canvas_send_all();
            do
                next_press(0);
            while (!pressed(KEYS_PLAY | KEYS_REC));
            *note = error ? error : "Saved: apps join it";
            return !error;
        }
        screen_test(board, ssid, r.step, true, why, &r, board == 2 ? "SEQ PLAY: save it anyway" : "PLAY: save it anyway",
                    CANVAS_RED, ask ? (board == 2 ? "SEQ REC EDIT" : "REC EDIT") : (board == 2 ? "SEQ REC BACK" : "REC BACK"));
        canvas_send_all();
        do
            next_press(0);
        while (!pressed(KEYS_PLAY | KEYS_REC));
        if (pressed(KEYS_PLAY)) {
            int i = wifi_list_put(&list, ssid, typed);
            const char *error = i < 0 ? "The list is full" : save_list();
            *note = error ? error : "Saved, not tested";
            return !error;
        }
        if (!ask)
            return false;
    }
}

static void progress(unsigned done, unsigned total) {
    screen_scanning(done, total);
    canvas_send();
}

/* Scans, and adds a network chosen from those heard or typed: what to say on the list, or 0. */
static const char *add_network(void) {
    if (list.count == WIFI_MOST)
        return "The list is full: delete one first";
    canvas_forget();
    screen_scanning(0, 13);
    canvas_send_all();
    heard_count = net_scan(heard, MOST_HEARD, progress);
    unsigned selection = 0;
    for (;;) {
        unsigned items = screen_heard_items(heard_count);
        if (selection >= items)
            selection = items-1;
        screen_heard(heard, heard_count, &list, board, selection);
        canvas_send_all();
        next_press(0);
        if (pressed(KEYS_REC))
            return 0;
        if (pressed(KEYS_PREV))
            selection = selection ? selection-1 : items-1;
        else if (pressed(KEYS_NEXT))
            selection = selection+1 < items ? selection+1 : 0;
        else if (pressed(KEYS_PLAY)) {
            if (selection == items-1)
                return 0;
            static char ssid[WIFI_SSID_MOST+1];
            if (selection == heard_count) {
                typed[0] = 0;
                text_all_symbols(1);
                if (!type("Network name", 0, "As it's spelt", WIFI_SSID_MOST, name_ok))
                    continue;
                memcpy(ssid, typed, sizeof ssid);
            } else {
                if (heard[selection].security == NET_OTHER)
                    continue;
                memcpy(ssid, heard[selection].ssid, sizeof ssid);
            }
            bool open = selection < heard_count && heard[selection].security == NET_OPEN;
            /* Saved already: start from its password. */
            int known = wifi_list_find(&list, ssid);
            typed[0] = 0;
            if (known >= 0 && !open)
                memcpy(typed, list.net[known].pass, sizeof list.net[known].pass);
            const char *note = 0;
            if (password_and_save(ssid, !open, &note))
                return note;
        }
    }
}

/* ---- a saved network ---- */

/* Held long enough, it's confirmed; let go sooner, it isn't. */
static bool hold(const uint8_t *actions, unsigned at) {
    unsigned shown = 0;
    uint32_t started = TIMER_COUNT;
    for (;;) {
        unsigned ms = (TIMER_COUNT-started)/TICKS_PER_MS;
        if (ms >= DELETE_HOLD_MS)
            return true;
        app_feed();
        keys_scan(&keys);
        if (!(keys.buttons&KEYS_PLAY))
            return false;
        unsigned percent = ms*100/DELETE_HOLD_MS+1;
        if (percent >= shown+4) {
            shown = percent;
            screen_network_hold(actions, at, percent);
            canvas_send();
        }
        hal_delay_us(POLL_US/2);
    }
}

/* A network's page: what to say on the list, or 0. */
static const char *network_page(unsigned index) {
    unsigned at = 0;
    const char *note = 0;
    uint16_t note_colour = CANVAS_DIM;
    for (;;) {
        uint8_t actions[4];
        unsigned count = 0;
        actions[count++] = ACTION_TEST;
        if (index != list.use)
            actions[count++] = ACTION_USE;
        actions[count++] = ACTION_PASSWORD;
        actions[count++] = ACTION_DELETE;
        if (at >= count)
            at = count-1;
        screen_network(&list, index, board, actions, count, at, 0, note, note_colour);
        canvas_send_all();
        note = 0;
        next_press(0);
        if (pressed(KEYS_REC))
            return 0;
        if (pressed(KEYS_PREV))
            at = at ? at-1 : count-1;
        else if (pressed(KEYS_NEXT))
            at = at+1 < count ? at+1 : 0;
        else if (pressed(KEYS_PLAY)) {
            struct wifi_net *net = &list.net[index];
            if (actions[at] == ACTION_TEST) {
                struct net_result r;
                const char *why = test(net->ssid, net->pass, &r);
                screen_test(board, net->ssid, r.step, true, why, &r, why ? "Didn't join" : "It works",
                            why ? CANVAS_RED : CANVAS_GREEN, board == 2 ? "SEQ PLAY OK" : "PLAY OK");
                canvas_send_all();
                do
                    next_press(0);
                while (!pressed(KEYS_PLAY | KEYS_REC));
            } else if (actions[at] == ACTION_USE) {
                list.use = index;
                const char *error = save_list();
                note = error ? error : "Apps join it now";
                note_colour = error ? CANVAS_RED : CANVAS_GREEN;
            } else if (actions[at] == ACTION_PASSWORD) {
                static char ssid[WIFI_SSID_MOST+1];
                memcpy(ssid, net->ssid, sizeof ssid);
                memcpy(typed, net->pass, sizeof net->pass);
                const char *saved = 0;
                if (password_and_save(ssid, true, &saved))
                    return saved;
            } else if (actions[at] == ACTION_DELETE && hold(actions, at)) {
                wifi_list_remove(&list, index);
                const char *error = save_list();
                return error ? error : "Deleted";
            }
        }
    }
}

/* ---- the app ---- */

int app_main(unsigned lsb_mhz) {
    lsb = clock_320(lsb_mhz);
    board = app_board();
    if (!board) {
        /* Its pins aren't known, so nothing is touched. */
        app_log("which board this is isn't known");
        clock_back();
        return 1;
    }
    app_log("board %u, LSB %u MHz (given %u)", board, lsb, lsb_mhz);
    board_init(board == 2);
    keys_init(board == 2);
    display_init(board);
    canvas_forget();
    const char *note = read_list();
    uint16_t note_colour = note == NO_CARD ? CANVAS_RED : CANVAS_DIM;
    unsigned selection = list.count ? list.use+1 : SCREEN_ADD;
#ifdef WIFI_AUTOTEST
    /* make ... WIFI_TEST=-DWIFI_AUTOTEST: no keys, for a test from a computer. What's heard, then each saved network
     * joined, all in the log. */
    {
        canvas_forget();
        screen_scanning(0, 13);
        canvas_send_all();
        heard_count = net_scan(heard, MOST_HEARD, progress);
        for (unsigned i = 0; i < heard_count; ++i)
            app_log("heard '%s': %d dBm, channel %u, security %u", heard[i].ssid, heard[i].rssi, heard[i].channel,
                    heard[i].security);
        for (unsigned i = 0; i < list.count; ++i) {
            struct net_result r;
            const char *why = test(list.net[i].ssid, list.net[i].pass, &r);
            app_log("RESULT '%s': %s (step %u, %d dBm, ch %u, ip %08x, %u ms)", list.net[i].ssid, why ? why : "joined",
                    r.step, r.rssi, r.channel, (unsigned)r.ip, (unsigned)r.ms);
            screen_test(board, list.net[i].ssid, r.step, true, why, &r, "Test", CANVAS_DIM, "");
            canvas_send_all();
        }
        if (card_ok)
            leave_card();
        board_restore();
        clock_back();
        return 0;
    }
#endif
    /* The key that started this, still held, doesn't count. */
    keys_scan(&keys);
    keys_scan(&keys);
    for (;;) {
        unsigned items = screen_list_items(&list);
        if (selection >= items)
            selection = items-1;
        screen_list(&list, board, selection, note, note_colour);
        canvas_send_all();
        note = 0;
        note_colour = CANVAS_DIM;
        next_press(0);
        bool back = pressed(KEYS_REC);
        if (pressed(KEYS_PREV))
            selection = selection ? selection-1 : items-1;
        else if (pressed(KEYS_NEXT))
            selection = selection+1 < items ? selection+1 : 0;
        else if (back || pressed(KEYS_PLAY)) {
            if (back || selection == items-1)
                break;
            if (!card_ok) {
                note = NO_CARD;
                note_colour = CANVAS_RED;
                continue;
            }
            unsigned before = list.count;
            if (selection == SCREEN_ADD) {
                note = add_network();
                if (list.count != before || note)
                    selection = list.use+1;
            } else {
                note = network_page(selection-1);
            }
            note_colour = note && (!memcmp(note, "Deleted", 8) || !memcmp(note, "Saved", 5)) ? CANVAS_GREEN : CANVAS_DIM;
            if (list.count < before && selection > list.count)
                selection = list.count;
        }
    }
    net_leave();
    if (card_ok)
        leave_card();
    board_restore();
    clock_back();
    return 0;
}
