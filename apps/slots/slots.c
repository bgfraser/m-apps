/* SPDX-License-Identifier: MIT */
/* Keeps firmware and its settings on the card, and swaps between them, with no computer (slotfile.h). Slots are named
 * on the piano keys (kit/textentry.c). It writes flash, so after a load it resets, and doesn't return. */
#include "app.h"
#include "canvas.h"
#include "card.h"
#include "core.h"
#include "keys.h"
#include "screens.h"
#include "slotfile.h"
#include "textentry.h"
#include <string.h>
#define POLL_US 10000u
/* How long PLAY is held to confirm: a load can be undone by loading back, a delete can't. */
#define LOAD_HOLD_MS 1000u
#define DELETE_HOLD_MS 2000u
#define BLINK_MS 500u
static struct slots slots;
static unsigned board;
static unsigned lsb;
static uint16_t chip_key;
static uint8_t work[CORE_WORK_SIZE] __attribute__((aligned(4)));
static struct core_app checked;
static struct keys keys;

/* Milliseconds, from the 24 MHz timer the player and usbdisk use. It wraps every 179 s, so differences only. */
#define TIMER_CON REG(0x10800u)
#define TIMER_COUNT REG(0x10804u)
#define TIMER_PERIOD REG(0x10808u)
#define TICKS_PER_MS 24000u

static void timer_start(void) {
    if ((TIMER_CON&0xfu) == ((2u << 2) | 1u))
        return;
    TIMER_CON = 0;
    TIMER_PERIOD = 0xffffffffu;
    TIMER_COUNT = 0;
    TIMER_CON = (2u << 2) | 1u | (1u << 14);
}
static const char *working;
static unsigned work_shown;

/* The card and the panel share SPI1: drawing steps off the card, and back on. */
static void card_on(void) {
    card_open(lsb);
    card_fast(lsb);
}

static bool same(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && !memcmp(a, b, n);
}

/* What slotfile is doing, in words, and how far it's got. */
static void show_progress(const char *what, unsigned done, unsigned total) {
    app_feed();
    if (what != working) {
        working = what;
        bool writing = same(what, "WRITING");
        const char *title = same(what, "SAVING") ? "Saving what's on the unit"
                          : same(what, "CHECKING") ? "Checking what was saved"
                          : same(what, "CHECKING THE SLOT") ? "Checking the slot"
                          : writing ? "Loading the slot" : what;
        screen_work(title, writing ? "Writing the firmware and its settings" : "On the card", writing);
        canvas_send_all();
        work_shown = 0;
    }
    unsigned percent = total ? done*100/total : 0;
    if (percent != work_shown) {
        work_shown = percent;
        screen_work_progress(done, total);
        canvas_send();
    }
}

/* The same check the bootloader makes before starting firmware. */
static bool image_valid(bool (*read)(void *, uint32_t, void *, uint32_t), void *ctx) {
    struct core_flash flash = {.ctx = ctx, .capacity = SLOT_FLASH_SIZE, .read = read};
    return core_app_mount(&flash, chip_key, &checked) && core_app_check(&flash, &checked, work) == CORE_APP_OK;
}

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

static const char NO_CARD[] = "No card found";

/* Errors come from below in capitals; the screen has room for them as they are. */
static const char *open_slots(int *here) {
    card_open(lsb);
    uint32_t sectors = card_start(lsb);
    const char *error = NO_CARD;
    uint8_t uid[8];
    flash_uid(uid);
    if (sectors) {
        error = slots_open(&slots, sectors, uid, flash_jedec(), board);
        if (!error)
            *here = slots_on_unit(&slots);
    }
    card_close();
    return error;
}

/* Runs work on the card, with the screen able to show its progress. */
static const char *on_card(const char *(*work_on)(unsigned), unsigned index) {
    card_on();
    canvas_hooks(card_close, card_on, 0);
    working = 0;
    const char *error = work_on(index);
    canvas_hooks(0, 0, 0);
    card_close();
    return error;
}

static int find_here(void) {
    card_on();
    int here = slots_on_unit(&slots);
    card_close();
    return here;
}

static char typed[SLOT_NAME_MOST+1];

static const char *do_save(unsigned ignored) {
    (void)ignored;
    return slots_save_new(&slots, typed);
}

static const char *do_load(unsigned index) {
    return slots_load(&slots, index);
}

static const char *do_delete(unsigned index) {
    return slots_delete(&slots, index);
}

static void leave_card(void) {
    card_open(lsb);
    card_finish();
    card_close();
}

/* ---- naming ---- */

static unsigned blink_ms;
static char placeholder[SLOT_NAME_MOST+1];
static const char *name_note;

static void draw_name(void) {
    screen_name(board, typed, placeholder, text_layer(keys.buttons), keys.piano, blink_ms < BLINK_MS, name_note);
    canvas_send_all();
}

/* Redraws while the layer keys change, which are held rather than pressed, and blinks the cursor. */
static void name_tick(unsigned ms) {
    static uint32_t shown;
    uint32_t layers = keys.buttons&(KEYS_OCT_DOWN | KEYS_OCT_UP);
    bool was = blink_ms < BLINK_MS;
    blink_ms = (blink_ms+ms)%(2*BLINK_MS);
    if (layers != shown || (blink_ms < BLINK_MS) != was) {
        shown = layers;
        draw_name();
    }
}

/* A name typed on the keys into typed, or false if cancelled. Left empty, it's the placeholder. */
static bool ask_name(void) {
    typed[0] = 0;
    name_note = 0;
    slots_free_name(&slots, "Slot", placeholder);
    draw_name();
    for (;;) {
        next_press(name_tick);
        if (keys.buttons_pressed&KEYS_REC)
            return false;
        name_note = 0;
        if (keys.buttons_pressed&KEYS_PLAY) {
            if (!typed[0])
                memcpy(typed, placeholder, sizeof typed);
            if (slots_name_ok(typed))
                return true;
            name_note = "A name can't end in a space or a dot";
        }
        text_type(typed, SLOT_NAME_MOST, keys.piano_pressed, text_layer(keys.buttons));
        blink_ms = 0;
        draw_name();
    }
}

/* ---- a slot's page ---- */

/* Held long enough, it's confirmed; let go sooner, it isn't. */
static bool hold(const uint8_t *actions, unsigned at) {
    unsigned needed = actions[at] == ACTION_DELETE ? DELETE_HOLD_MS : LOAD_HOLD_MS, shown = 0;
    uint32_t started = TIMER_COUNT;
    for (;;) {
        unsigned ms = (TIMER_COUNT-started)/TICKS_PER_MS;
        if (ms >= needed)
            return true;
        app_feed();
        keys_scan(&keys);
        if (!(keys.buttons&KEYS_PLAY))
            return false;
        unsigned percent = ms*100/needed+1;
        if (percent >= shown+4) {
            shown = percent;
            screen_slot_hold(actions, at, percent);
            canvas_send();
        }
        hal_delay_us(POLL_US/2);
    }
}

/* What to do with a slot: ACTION_LOAD, ACTION_DELETE or ACTION_BACK. */
static unsigned slot_page(unsigned index, bool here, bool pending) {
    uint8_t actions[3];
    unsigned count = 0, at = 0;
    if (!here)
        actions[count++] = ACTION_LOAD;
    actions[count++] = ACTION_DELETE;
    actions[count++] = ACTION_BACK;
    const struct slot *slot = &slots.list[index];
    for (;;) {
        screen_slot(slot, board, here, pending, actions, count, at, 0);
        canvas_send_all();
        next_press(0);
        if (keys.buttons_pressed&KEYS_REC)
            return ACTION_BACK;
        if (keys.buttons_pressed&KEYS_PREV)
            at = at ? at-1 : count-1;
        else if (keys.buttons_pressed&KEYS_NEXT)
            at = at+1 < count ? at+1 : 0;
        else if (keys.buttons_pressed&KEYS_PLAY) {
            if (actions[at] == ACTION_BACK)
                return ACTION_BACK;
            if (hold(actions, at))
                return actions[at];
        }
    }
}

/* ---- the app ---- */

int app_main(unsigned lsb_mhz) {
    lsb = lsb_mhz;
    slots.progress = show_progress;
    slots.valid = image_valid;
    timer_start();
    board = app_board();
    if (!board) {
        /* Its pins aren't known, so nothing is touched. */
        app_log("which board this is isn't known");
        return 1;
    }
    board_init(board == 2);
    keys_init(board == 2);
    display_init(board);
    canvas_forget();
    screen_work("Reading the card", "", false);
    canvas_send_all();
    if (!flash_init(1, 1)) {
        screen_work("Flash error", "The unit's flash didn't answer", true);
        canvas_send_all();
        hal_delay_us(5000000);
        board_restore();
        return 1;
    }
    chip_key = (uint16_t)(hal_efuse(1, 3) >> 8);
    int here = NO_SLOT;
    const char *note = open_slots(&here);
    bool card_ok = note != NO_CARD;
    uint16_t note_colour = CANVAS_RED;
    if (!note && slots_pending(&slots) != NO_SLOT) {
        note = "The last load stopped";
        note_colour = CANVAS_ORANGE;
    }
    if (note)
        app_log("%s", note);
    app_log("%u slots, on the unit: %d", slots.count, here);
    int pending = slots_pending(&slots);
    unsigned selection = pending != NO_SLOT ? (unsigned)pending+1 : here != NO_SLOT ? (unsigned)here+1 : 0;
    /* The key that started this, still held, doesn't count. */
    keys_scan(&keys);
    keys_scan(&keys);
    for (;;) {
        unsigned items = screen_items(&slots);
        if (selection >= items)
            selection = items-1;
        pending = slots_pending(&slots);
        screen_list(&slots, board, selection, here, pending, note, note_colour);
        canvas_send_all();
        note = 0;
        note_colour = CANVAS_RED;
        next_press(0);
        bool back = keys.buttons_pressed&KEYS_REC;
        if (keys.buttons_pressed&KEYS_PREV)
            selection = selection ? selection-1 : items-1;
        else if (keys.buttons_pressed&KEYS_NEXT)
            selection = selection+1 < items ? selection+1 : 0;
        else if (back || keys.buttons_pressed&KEYS_PLAY) {
            if (back || selection == items-1) {
                if (card_ok)
                    leave_card();
                board_restore();
                return 0;
            }
            if (!card_ok)
                continue;
            if (selection == SCREEN_SAVE) {
                if (!ask_name())
                    continue;
                note = on_card(do_save, 0);
                app_log("save: %s", note ? note : "ok");
                if (!note) {
                    here = find_here();
                    note = "Saved";
                    note_colour = CANVAS_GREEN;
                    selection = here != NO_SLOT ? (unsigned)here+1 : 0;
                }
                continue;
            }
            unsigned index = selection-1;
            unsigned action = slot_page(index, (int)index == here, (int)index == pending);
            if (action == ACTION_DELETE) {
                note = on_card(do_delete, index);
                app_log("delete: %s", note ? note : "ok");
                if (!note) {
                    note = "Deleted";
                    note_colour = CANVAS_GREEN;
                }
                here = find_here();
            } else if (action == ACTION_LOAD) {
                /* The guard stays on: each sector feeds it, so only a real hang reaches it, and it ends in download mode
                 * rather than a unit that needs switching off. Off only for the reset, which should start the firmware. */
                note = on_card(do_load, index);
                app_log("load: %s", note ? note : "ok");
                if (!note) {
                    leave_card();
                    screen_work("Done", "Starting the firmware", false);
                    canvas_send_all();
                    hal_delay_us(1000000);
                    board_restore();
                    app_guard(false);
                    sync_cpu();
                    hal_reset();
                }
                /* It may have saved, so the list is read again. It's safe to try the load again from here. */
                here = find_here();
            }
        }
    }
}
