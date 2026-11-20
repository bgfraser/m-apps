/* SPDX-License-Identifier: MIT */
#include "screens.h"
#include "canvas.h"
#include "textentry.h"
#include <string.h>

#define N CANVAS_N
#define TILE_X 8
#define TILE_W 224
#define TILE_H 44
#define TILE_Y0 46
#define TILE_STEP 50
#define ROWS 3u
#define ICON 34

static const char *keys_line(unsigned board, const char *play) {
    static char line[48];
    const char *move = board == 2 ? "<> MOVE  SEQ PLAY " : "-/+ MOVE  PLAY ";
    unsigned n = 0;
    for (const char *p = move; *p; ++p)
        line[n++] = *p;
    for (const char *p = play; *p && n < sizeof line-1; ++p)
        line[n++] = *p;
    line[n] = 0;
    return line;
}

static const char *back_key(unsigned board) {
    return board == 2 ? "SEQ REC BACK" : "REC BACK";
}

static void header(const char *title, const char *right) {
    canvas_clear(0, 0, N, CANVAS_BAR_Y);
    canvas_text(16, 12, CANVAS_BIG, title, CANVAS_INK, 11);
    if (right && *right)
        canvas_text(N-16-canvas_text_w(CANVAS_SMALL, right, 8), 17, CANVAS_SMALL, right, CANVAS_DIM, 8);
}

/* A letter on a colour, the slot's own from its name, as the launcher shows an app with no icon. */
static void letter_icon(int x, int y, const char *name, uint16_t colour) {
    char letter[2] = {name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0]-32) : name[0], 0};
    canvas_shape(x, y, ICON, ICON, 9, colour, 16, 1);
    canvas_text(x+(ICON-canvas_text_w(CANVAS_BIG, letter, 10))/2, y+7, CANVAS_BIG, letter, CANVAS_WHITE, 10);
}

static uint16_t name_colour(const char *name) {
    static const uint16_t colours[] = {CANVAS_RGB(84, 132, 220), CANVAS_RGB(232, 120, 72), CANVAS_RGB(120, 96, 210),
                                       CANVAS_RGB(46, 160, 140), CANVAS_RGB(214, 84, 120), CANVAS_RGB(90, 150, 70)};
    unsigned h = 5381;
    for (; *name; ++name)
        h = h*33u+(unsigned char)(*name|32);
    return colours[h%(sizeof colours/sizeof colours[0])];
}

static void plus_icon(int x, int y) {
    canvas_shape(x, y, ICON, ICON, 9, CANVAS_BLUE, 16, 1);
    canvas_shape(x+ICON/2-2, y+9, 4, ICON-18, 1, CANVAS_WHITE, 16, 1);
    canvas_shape(x+9, y+ICON/2-2, ICON-18, 4, 1, CANVAS_WHITE, 16, 1);
}

static void back_icon(int x, int y) {
    canvas_shape(x, y, ICON, ICON, 9, CANVAS_RGB(170, 180, 194), 16, 1);
    canvas_text(x+11, y+7, CANVAS_BIG, "<", CANVAS_WHITE, 10);
}

/* KB, or MB with a tenth. */
static void put_size(char *out, uint32_t bytes) {
    char tmp[12];
    unsigned n = 0, kb = bytes/1024;
    bool mb = kb >= 1024;
    unsigned v = mb ? kb*10/1024 : kb;
    do {
        tmp[n++] = (char)('0'+v%10);
        v /= 10;
        if (mb && n == 1)
            tmp[n++] = '.';
    } while (v || (mb && n < 3));
    while (n)
        *out++ = tmp[--n];
    memcpy(out, mb ? " MB" : " KB", 4);
}

static void slot_detail(const struct slot *slot, bool here, bool pending, char *line, uint16_t *colour) {
    *colour = CANVAS_DIM;
    if (pending) {
        memcpy(line, "Load stopped: load it again", 28);
        *colour = CANVAS_ORANGE;
    } else if (!slot->sound) {
        memcpy(line, "Damaged", 8);
        *colour = CANVAS_RED;
    } else if (here) {
        memcpy(line, "On the unit now", 16);
        *colour = CANVAS_GREEN;
    } else {
        put_size(line, slot->file.size);
    }
}

unsigned screen_items(const struct slots *s) {
    return s->count+2;
}

/* A small chevron: more this way. */
static void more_mark(int y, bool up) {
    for (int i = 0; i < 4; ++i) {
        int w = up ? 1+2*i : 7-2*i;
        canvas_fill(N/2-w/2, y+i, w, 1, CANVAS_FAINT);
    }
}

void screen_list(const struct slots *s, unsigned board, unsigned selection, int here, int pending, const char *note,
                 uint16_t note_colour) {
    unsigned items = screen_items(s), top = selection > 0 ? selection-1 : 0;
    if (top+ROWS > items)
        top = items > ROWS ? items-ROWS : 0;
    char right[24];
    unsigned n = 0, count = s->count;
    if (count >= 10)
        right[n++] = (char)('0'+count/10%10);
    right[n++] = (char)('0'+count%10);
    memcpy(right+n, count == 1 ? " slot" : " slots", 7);
    header("Slots", right);
    for (unsigned r = 0; r < ROWS && top+r < items; ++r) {
        unsigned item = top+r;
        int y = TILE_Y0+(int)r*TILE_STEP;
        canvas_card(TILE_X, y, TILE_W, TILE_H, 12, item == selection);
        const char *name, *detail;
        char line[48];
        uint16_t detail_colour = CANVAS_DIM;
        if (item == SCREEN_SAVE) {
            plus_icon(TILE_X+6, y+5);
            name = "Save what's on the unit";
            detail = "As a new slot, named on the keys";
        } else if (item == items-1) {
            back_icon(TILE_X+6, y+5);
            name = "Back";
            detail = "To the launcher";
        } else {
            const struct slot *slot = &s->list[item-1];
            letter_icon(TILE_X+6, y+5, slot->name, slot->sound ? name_colour(slot->name) : CANVAS_RGB(170, 180, 194));
            name = slot->name;
            slot_detail(slot, (int)item-1 == here, (int)item-1 == pending, line, &detail_colour);
            detail = line;
        }
        canvas_text(TILE_X+48, y+5, CANVAS_BIG, canvas_fit(CANVAS_BIG, name, TILE_W-56), CANVAS_INK, 8);
        canvas_text(TILE_X+48, y+24, CANVAS_SMALL, canvas_fit(CANVAS_SMALL, detail, TILE_W-56), detail_colour, 8);
    }
    if (top)
        more_mark(TILE_Y0-8, true);
    if (top+ROWS < items)
        more_mark(TILE_Y0+(int)ROWS*TILE_STEP-6, false);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    canvas_bar(note && *note ? note : back_key(board), note && *note ? note_colour : CANVAS_DIM,
               keys_line(board, "OPEN"));
}

static const char *const labels[] = {"Load it onto the unit", "Delete it", "Back"};

/* An action's button: white, or filled when selected, with a hold filling it with white from the left. */
static void draw_action(unsigned action, unsigned i, bool on, unsigned held_percent) {
    int y = 114+(int)i*28, w = N-24;
    uint16_t fill = action == ACTION_DELETE ? CANVAS_RED : CANVAS_BLUE;
    canvas_clear(10, y-2, w+4, 28);
    canvas_shape(12, y, w, 24, 12, on ? fill : CANVAS_WHITE, 16, 1);
    if (!on) {
        canvas_shape(12, y, w, 24, 12, CANVAS_LINE, 8, 1);
        canvas_shape(13, y+1, w-2, 22, 11, CANVAS_WHITE, 16, 1);
    }
    if (on && held_percent) {
        int lit = w*(int)(held_percent > 100 ? 100 : held_percent)/100;
        canvas_shape(12, y, lit, 24, 12, CANVAS_WHITE, 7, 1);
    }
    const char *label = labels[action];
    canvas_text(12+(w-canvas_text_w(CANVAS_SMALL, label, 8))/2, y+5, CANVAS_SMALL, label, on ? CANVAS_WHITE : CANVAS_INK, 8);
    canvas_mark(10, y-2, w+4, 28);
}

void screen_slot_hold(const uint8_t *actions, unsigned selected, unsigned held_percent) {
    draw_action(actions[selected], selected, true, held_percent);
}

void screen_slot(const struct slot *slot, unsigned board, bool here, bool pending, const uint8_t *actions, unsigned count,
                 unsigned selected, unsigned held_percent) {
    header("Slot", "");
    canvas_card(12, 40, N-24, 64, 14, false);
    letter_icon(24, 52, slot->name, slot->sound ? name_colour(slot->name) : CANVAS_RGB(170, 180, 194));
    canvas_text(68, 50, CANVAS_BIG, canvas_fit(CANVAS_BIG, slot->name, N-24-68), CANVAS_INK, 8);
    char line[48];
    uint16_t colour;
    slot_detail(slot, here, pending, line, &colour);
    if (colour == CANVAS_DIM) {
        /* With the size, how much of the flash it fills. */
        put_size(line, slot->file.size);
    }
    canvas_text(68, 70, CANVAS_SMALL, canvas_fit(CANVAS_SMALL, line, N-24-68), colour, 8);
    for (unsigned i = 0; i < count; ++i)
        draw_action(actions[i], i, i == selected, i == selected ? held_percent : 0);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    unsigned a = actions[selected];
    const char *top = a == ACTION_LOAD ? (board == 2 ? "Hold SEQ PLAY to load" : "Hold PLAY to load")
                    : a == ACTION_DELETE ? (board == 2 ? "Hold SEQ PLAY to delete" : "Hold PLAY to delete")
                    : back_key(board);
    canvas_bar(top, a == ACTION_DELETE ? CANVAS_RED : CANVAS_DIM, keys_line(board, a == ACTION_BACK ? "BACK" : "HOLD"));
}

void screen_name(unsigned board, const char *text, const char *placeholder, unsigned layer, uint32_t held, bool cursor,
                 const char *note) {
    header("Name the slot", "");
    canvas_card(12, 40, N-24, 40, 12, true);
    /* The end of the text, if it's longer than the field. */
    const char *shown = text;
    while (*shown && canvas_text_w(CANVAS_BIG, shown, 8) > N-24-30)
        ++shown;
    int end = 26;
    if (*text)
        end = canvas_text(26, 50, CANVAS_BIG, shown, CANVAS_INK, 8);
    else
        canvas_text(26, 50, CANVAS_BIG, placeholder, CANVAS_FAINT, 8);
    if (cursor)
        canvas_fill(end+1, 50, 2, 19, CANVAS_BLUE);
    static const char *const layers[] = {"abc", "ABC", "123 &()"};
    canvas_text(16, 88, CANVAS_SMALL, layers[layer], layer ? CANVAS_BLUE : CANVAS_DIM, 8);
    const char *hold = "Hold OCT- for ABC, OCT+ for 123";
    (void)board;
    canvas_text(N-16-canvas_text_w(CANVAS_SMALL, hold, 8), 88, CANVAS_SMALL, hold, CANVAS_FAINT, 8);
    canvas_mark(0, 0, N, 110);
    text_piano(110, layer, held);
    canvas_text(16, 160, CANVAS_SMALL, "The keys type. The last one deletes.", CANVAS_DIM, 8);
    canvas_mark(0, 156, N, CANVAS_BAR_Y-156);
    canvas_bar(note && *note ? note : (board == 2 ? "SEQ PLAY SAVE" : "PLAY SAVE"), note && *note ? CANVAS_RED : CANVAS_DIM,
               board == 2 ? "SEQ REC CANCEL" : "REC CANCEL");
}

#define WORK_X 12
#define WORK_Y 64
#define WORK_W (N-24)
#define BAR_X (WORK_X+16)
#define BAR_Y (WORK_Y+58)
#define BAR_W (WORK_W-32)

void screen_work(const char *title, const char *line, bool warn) {
    header("Slots", "");
    canvas_card(WORK_X, WORK_Y, WORK_W, 84, 14, false);
    canvas_text(WORK_X+16, WORK_Y+12, CANVAS_BIG, title, CANVAS_INK, 8);
    canvas_text(WORK_X+16, WORK_Y+33, CANVAS_SMALL, line, warn ? CANVAS_ORANGE : CANVAS_DIM, 8);
    canvas_shape(BAR_X, BAR_Y, BAR_W, 8, 4, CANVAS_RGB(222, 230, 240), 16, 1);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    canvas_bar(warn ? "Don't switch off" : "", warn ? CANVAS_ORANGE : CANVAS_DIM, "");
}

void screen_work_progress(unsigned done, unsigned total) {
    int w = total ? (int)(BAR_W*(done > total ? total : done)/total) : 0;
    canvas_fill(BAR_X-2, BAR_Y-2, BAR_W+4, 12, CANVAS_WHITE);
    canvas_shape(BAR_X, BAR_Y, BAR_W, 8, 4, CANVAS_RGB(222, 230, 240), 16, 1);
    if (w >= 8)
        canvas_shape(BAR_X, BAR_Y, w, 8, 4, CANVAS_BLUE, 16, 1);
    canvas_mark(BAR_X-2, BAR_Y-2, BAR_W+4, 12);
}
