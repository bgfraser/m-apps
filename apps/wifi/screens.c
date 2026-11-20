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
#define GREY CANVAS_RGB(170, 180, 194)

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

/* The title big, or smaller and cut to fit, as a network's name may need. */
static void header(const char *title, const char *right) {
    canvas_clear(0, 0, N, CANVAS_BAR_Y);
    int room = N-32;
    if (right && *right) {
        right = canvas_fit(CANVAS_SMALL, right, 110);
        int w = canvas_text_w(CANVAS_SMALL, right, 8);
        canvas_text(N-16-w, 17, CANVAS_SMALL, right, CANVAS_DIM, 8);
        room -= w+8;
    }
    if (canvas_text_w(CANVAS_BIG, title, 11) <= room)
        canvas_text(16, 12, CANVAS_BIG, title, CANVAS_INK, 11);
    else
        canvas_text(16, 15, CANVAS_BIG, canvas_fit(CANVAS_BIG, title, room), CANVAS_INK, 8);
}

/* Four bars, as strong as the signal, white on a colour. bars: 0 to 4. */
static void signal_icon(int x, int y, unsigned bars, uint16_t colour) {
    canvas_shape(x, y, ICON, ICON, 9, colour, 16, 1);
    for (unsigned i = 0; i < 4; ++i) {
        int h = 6+(int)i*5, bx = x+7+(int)i*6;
        canvas_shape(bx, y+ICON-7-h, 4, h, 1, CANVAS_WHITE, i < bars ? 16 : 6, 1);
    }
}

static unsigned bars(int rssi) {
    return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
}

static void plus_icon(int x, int y) {
    canvas_shape(x, y, ICON, ICON, 9, CANVAS_BLUE, 16, 1);
    canvas_shape(x+ICON/2-2, y+9, 4, ICON-18, 1, CANVAS_WHITE, 16, 1);
    canvas_shape(x+9, y+ICON/2-2, ICON-18, 4, 1, CANVAS_WHITE, 16, 1);
}

static void back_icon(int x, int y) {
    canvas_shape(x, y, ICON, ICON, 9, GREY, 16, 1);
    canvas_text(x+11, y+7, CANVAS_BIG, "<", CANVAS_WHITE, 10);
}

static void pen_icon(int x, int y) {
    canvas_shape(x, y, ICON, ICON, 9, CANVAS_RGB(120, 96, 210), 16, 1);
    canvas_text(x+(ICON-canvas_text_w(CANVAS_BIG, "Aa", 8))/2, y+8, CANVAS_BIG, "Aa", CANVAS_WHITE, 8);
}

/* A small chevron: more this way. */
static void more_mark(int y, bool up) {
    for (int i = 0; i < 4; ++i) {
        int w = up ? 1+2*i : 7-2*i;
        canvas_fill(N/2-w/2, y+i, w, 1, CANVAS_FAINT);
    }
}

static void tile(unsigned row, bool marked) {
    canvas_card(TILE_X, TILE_Y0+(int)row*TILE_STEP, TILE_W, TILE_H, 12, marked);
}

static void tile_text(unsigned row, const char *name, const char *detail, uint16_t detail_colour) {
    int y = TILE_Y0+(int)row*TILE_STEP;
    canvas_text(TILE_X+48, y+5, CANVAS_BIG, canvas_fit(CANVAS_BIG, name, TILE_W-56), CANVAS_INK, 8);
    canvas_text(TILE_X+48, y+24, CANVAS_SMALL, canvas_fit(CANVAS_SMALL, detail, TILE_W-56), detail_colour, 8);
}

/* The rows shown with the selection in view, one above it when there is one. */
static unsigned first_row(unsigned selection, unsigned items) {
    unsigned top = selection > 0 ? selection-1 : 0;
    if (top+ROWS > items)
        top = items > ROWS ? items-ROWS : 0;
    return top;
}

static void more_marks(unsigned top, unsigned items) {
    if (top)
        more_mark(TILE_Y0-8, true);
    if (top+ROWS < items)
        more_mark(TILE_Y0+(int)ROWS*TILE_STEP-6, false);
}

static char *put_number(char *out, int v) {
    char tmp[12];
    unsigned n = 0, u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0)
        *out++ = '-';
    do
        tmp[n++] = (char)('0'+u%10);
    while (u /= 10);
    while (n)
        *out++ = tmp[--n];
    *out = 0;
    return out;
}

static char *put(char *out, const char *s) {
    size_t n = strlen(s);
    memcpy(out, s, n+1);
    return out+n;
}

/* ---- the saved networks ---- */

unsigned screen_list_items(const struct wifi_list *l) {
    return l->count+2;
}

static void network_detail(const struct wifi_list *l, unsigned i, char *line, uint16_t *colour) {
    *colour = CANVAS_DIM;
    if (i == l->use) {
        put(line, "Apps join this one");
        *colour = CANVAS_GREEN;
    } else {
        put(line, l->net[i].pass[0] ? "Saved" : "Saved, open");
    }
}

void screen_list(const struct wifi_list *l, unsigned board, unsigned selection, const char *note, uint16_t note_colour) {
    unsigned items = screen_list_items(l), top = first_row(selection, items);
    char right[24];
    char *p = put_number(right, (int)l->count);
    put(p, l->count == 1 ? " network" : " networks");
    header("WiFi", right);
    for (unsigned r = 0; r < ROWS && top+r < items; ++r) {
        unsigned item = top+r;
        int y = TILE_Y0+(int)r*TILE_STEP;
        tile(r, item == selection);
        if (item == SCREEN_ADD) {
            plus_icon(TILE_X+6, y+5);
            tile_text(r, "Add a network", "From those heard here", CANVAS_DIM);
        } else if (item == items-1) {
            back_icon(TILE_X+6, y+5);
            tile_text(r, "Back", "To the launcher", CANVAS_DIM);
        } else {
            unsigned i = item-1;
            char line[32];
            uint16_t colour;
            signal_icon(TILE_X+6, y+5, 4, i == l->use ? CANVAS_GREEN : CANVAS_BLUE);
            network_detail(l, i, line, &colour);
            tile_text(r, l->net[i].ssid, line, colour);
        }
    }
    more_marks(top, items);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    canvas_bar(note && *note ? note : back_key(board), note && *note ? note_colour : CANVAS_DIM, keys_line(board, "OPEN"));
}

/* ---- a saved network ---- */

static const char *const labels[] = {"Test the connection", "Make apps join it", "Change the password", "Delete it"};

#define ACTION_Y 98
#define ACTION_STEP 25

static void draw_action(unsigned action, unsigned i, bool on, unsigned held_percent) {
    int y = ACTION_Y+(int)i*ACTION_STEP, w = N-24;
    uint16_t fill = action == ACTION_DELETE ? CANVAS_RED : CANVAS_BLUE;
    canvas_clear(10, y-2, w+4, ACTION_STEP);
    canvas_shape(12, y, w, 22, 11, on ? fill : CANVAS_WHITE, 16, 1);
    if (!on) {
        canvas_shape(12, y, w, 22, 11, CANVAS_LINE, 8, 1);
        canvas_shape(13, y+1, w-2, 20, 10, CANVAS_WHITE, 16, 1);
    }
    if (on && held_percent) {
        int lit = w*(int)(held_percent > 100 ? 100 : held_percent)/100;
        canvas_shape(12, y, lit, 22, 11, CANVAS_WHITE, 7, 1);
    }
    const char *label = labels[action];
    canvas_text(12+(w-canvas_text_w(CANVAS_SMALL, label, 8))/2, y+4, CANVAS_SMALL, label, on ? CANVAS_WHITE : CANVAS_INK, 8);
    canvas_mark(10, y-2, w+4, ACTION_STEP);
}

void screen_network_hold(const uint8_t *actions, unsigned selected, unsigned held_percent) {
    draw_action(actions[selected], selected, true, held_percent);
}

void screen_network(const struct wifi_list *l, unsigned index, unsigned board, const uint8_t *actions, unsigned count,
                    unsigned selected, unsigned held_percent, const char *note, uint16_t note_colour) {
    header("Network", "");
    canvas_card(12, 40, N-24, 50, 14, false);
    signal_icon(22, 48, 4, index == l->use ? CANVAS_GREEN : CANVAS_BLUE);
    canvas_text(66, 46, CANVAS_BIG, canvas_fit(CANVAS_BIG, l->net[index].ssid, N-24-66), CANVAS_INK, 8);
    char line[32];
    uint16_t colour;
    network_detail(l, index, line, &colour);
    canvas_text(66, 66, CANVAS_SMALL, line, colour, 8);
    for (unsigned i = 0; i < count; ++i)
        draw_action(actions[i], i, i == selected, i == selected ? held_percent : 0);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    unsigned a = actions[selected];
    const char *top = a == ACTION_DELETE ? (board == 2 ? "Hold SEQ PLAY to delete" : "Hold PLAY to delete") : back_key(board);
    if (note && *note)
        top = note;
    canvas_bar(top, note && *note ? note_colour : a == ACTION_DELETE ? CANVAS_RED : CANVAS_DIM,
               keys_line(board, a == ACTION_DELETE ? "HOLD" : "OK"));
}

/* ---- listening, and what was heard ---- */

#define WORK_X 12
#define WORK_Y 64
#define WORK_W (N-24)
#define BAR_X (WORK_X+16)
#define BAR_Y (WORK_Y+58)
#define BAR_W (WORK_W-32)

void screen_scanning(unsigned done, unsigned total) {
    if (!done) {
        header("Add a network", "");
        canvas_card(WORK_X, WORK_Y, WORK_W, 84, 14, false);
        canvas_text(WORK_X+16, WORK_Y+12, CANVAS_BIG, "Listening", CANVAS_INK, 8);
        canvas_text(WORK_X+16, WORK_Y+33, CANVAS_SMALL, "For networks on channels 1 to 13", CANVAS_DIM, 8);
        canvas_mark(0, 0, N, CANVAS_BAR_Y);
        canvas_bar("", CANVAS_DIM, "");
    }
    int w = total ? (int)(BAR_W*(done > total ? total : done)/total) : 0;
    canvas_fill(BAR_X-2, BAR_Y-2, BAR_W+4, 12, CANVAS_WHITE);
    canvas_shape(BAR_X, BAR_Y, BAR_W, 8, 4, CANVAS_RGB(222, 230, 240), 16, 1);
    if (w >= 8)
        canvas_shape(BAR_X, BAR_Y, w, 8, 4, CANVAS_BLUE, 16, 1);
    canvas_mark(BAR_X-2, BAR_Y-2, BAR_W+4, 12);
}

unsigned screen_heard_items(unsigned count) {
    return count+2;
}

void screen_heard(const struct net_ap *aps, unsigned count, const struct wifi_list *l, unsigned board, unsigned selection) {
    unsigned items = screen_heard_items(count), top = first_row(selection, items);
    char right[24];
    char *p = put_number(right, (int)count);
    put(p, " heard");
    header("Add a network", right);
    for (unsigned r = 0; r < ROWS && top+r < items; ++r) {
        unsigned item = top+r;
        int y = TILE_Y0+(int)r*TILE_STEP;
        tile(r, item == selection);
        if (item == count) {
            pen_icon(TILE_X+6, y+5);
            tile_text(r, "Type a name", "For one not heard, or hidden", CANVAS_DIM);
        } else if (item == count+1) {
            back_icon(TILE_X+6, y+5);
            tile_text(r, "Back", "To the saved networks", CANVAS_DIM);
        } else {
            const struct net_ap *ap = &aps[item];
            bool ok = ap->security != NET_OTHER;
            char line[48];
            uint16_t colour = CANVAS_DIM;
            signal_icon(TILE_X+6, y+5, bars(ap->rssi), ok ? CANVAS_BLUE : GREY);
            if (!ok) {
                put(line, "WPA3 or other: can't join it");
                colour = CANVAS_ORANGE;
            } else {
                p = line;
                if (ap->security == NET_OPEN)
                    p = put(p, "Open, ");
                p = put_number(p, ap->rssi);
                p = put(p, " dBm, channel ");
                p = put_number(p, ap->channel);
                if (wifi_list_find(l, ap->ssid) >= 0) {
                    put(p, ", saved");
                    colour = CANVAS_GREEN;
                }
            }
            tile_text(r, ap->ssid, line, colour);
        }
    }
    more_marks(top, items);
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    canvas_bar(count ? back_key(board) : "Nothing heard", count ? CANVAS_DIM : CANVAS_ORANGE, keys_line(board, "CHOOSE"));
}

/* ---- typing ---- */

void screen_type(unsigned board, const char *title, const char *network, const char *text, const char *placeholder,
                 unsigned layer, uint32_t held, bool cursor, const char *note) {
    header(title, network);
    canvas_card(12, 40, N-24, 40, 12, true);
    /* The end of the text, if it's longer than the field. */
    const char *shown = text;
    while (*shown && canvas_text_w(CANVAS_BIG, shown, 8) > N-24-30)
        ++shown;
    int end = 26;
    if (*text)
        end = canvas_text(26, 50, CANVAS_BIG, shown, CANVAS_INK, 8);
    else if (placeholder)
        canvas_text(26, 50, CANVAS_BIG, canvas_fit(CANVAS_BIG, placeholder, N-24-30), CANVAS_FAINT, 8);
    if (cursor)
        canvas_fill(end+1, 50, 2, 19, CANVAS_BLUE);
    static const char *const layers[] = {"abc", "ABC", "123 &()", "more #<>"};
    canvas_text(16, 88, CANVAS_SMALL, layers[layer], layer ? CANVAS_BLUE : CANVAS_DIM, 8);
    const char *hold = "OCT- ABC, OCT+ 123, both more";
    canvas_text(N-16-canvas_text_w(CANVAS_SMALL, hold, 8), 88, CANVAS_SMALL, hold, CANVAS_FAINT, 8);
    canvas_mark(0, 0, N, 110);
    text_piano(110, layer, held);
    char count[48];
    char *p = put_number(count, (int)strlen(text));
    put(p, " characters. The last key deletes.");
    canvas_text(16, 160, CANVAS_SMALL, count, CANVAS_DIM, 8);
    canvas_mark(0, 156, N, CANVAS_BAR_Y-156);
    canvas_bar(note && *note ? note : (board == 2 ? "SEQ PLAY DONE" : "PLAY DONE"), note && *note ? CANVAS_RED : CANVAS_DIM,
               board == 2 ? "SEQ REC CANCEL" : "REC CANCEL");
}

/* ---- a test ---- */

static const char *const steps[] = {"Starting the radio", "Finding the network", "Making the key", "Joining",
                                    "Getting an address"};

static char *put_ip(char *out, uint32_t ip) {
    for (int i = 3; i >= 0; --i) {
        out = put_number(out, (int)(ip >> (8*i)&255u));
        if (i)
            *out++ = '.';
    }
    *out = 0;
    return out;
}

void screen_test(unsigned board, const char *ssid, unsigned step, bool done, const char *why, const struct net_result *r,
                 const char *bar_top, uint16_t bar_colour, const char *bar_keys) {
    (void)board;
    header(ssid, "");
    canvas_card(12, 40, N-24, 152, 14, false);
    for (unsigned i = 0; i < NET_STEPS; ++i) {
        int y = 50+(int)i*17;
        bool failed = done && why && i == step, passed = i < step || (done && !why);
        uint16_t dot = failed ? CANVAS_RED : passed ? CANVAS_GREEN : i == step ? CANVAS_BLUE : CANVAS_LINE;
        canvas_shape(26, y+4, 8, 8, 4, dot, 16, 1);
        canvas_text(42, y, CANVAS_SMALL, steps[i], i <= step || done ? CANVAS_INK : CANVAS_FAINT, 8);
    }
    if (done) {
        char line[64];
        if (why) {
            canvas_text(26, 140, CANVAS_BIG, canvas_fit(CANVAS_BIG, why, N-24-28), CANVAS_RED, 8);
            if (r->channel) {
                char *p = put(line, "Heard at ");
                p = put_number(p, r->rssi);
                p = put(p, " dBm, channel ");
                put_number(p, r->channel);
                canvas_text(26, 164, CANVAS_SMALL, line, CANVAS_DIM, 8);
            }
        } else {
            char *p = put(line, "Connected: ");
            put_ip(p, r->ip);
            canvas_text(26, 138, CANVAS_BIG, canvas_fit(CANVAS_BIG, line, N-24-28), CANVAS_GREEN, 8);
            p = put_number(line, r->rssi);
            p = put(p, " dBm, channel ");
            p = put_number(p, r->channel);
            p = put(p, ", ");
            p = put_number(p, (int)(r->ms/1000));
            *p++ = '.';
            p = put_number(p, (int)(r->ms/100%10));
            put(p, " s");
            canvas_text(26, 160, CANVAS_SMALL, line, CANVAS_DIM, 8);
            p = put(line, "Gateway ");
            put_ip(p, r->gateway);
            canvas_text(26, 175, CANVAS_SMALL, line, CANVAS_DIM, 8);
        }
    }
    canvas_mark(0, 0, N, CANVAS_BAR_Y);
    canvas_bar(bar_top ? bar_top : "", bar_colour, bar_keys ? bar_keys : "");
}
