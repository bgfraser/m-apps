/* SPDX-License-Identifier: MIT */
/* An original skin in the style of the 90s desktop players: brushed metal, ridged title bars, a black SK_LCD with
 * segment digits and an analyser, and a green-on-black playlist. Everything is drawn here, nothing is a bitmap. */
#include "ui.h"
#include "pixel.h"
#include <string.h>

#define W PIX_W
#define H PIX_H

/* The palette. */
#define GROOVE PIX_RGB(0x10, 0x13, 0x1b)
#define LIST_TEXT PIX_RGB(0x10, 0xe8, 0x30)
#define LIST_PLAYING PIX_RGB(0xff, 0xff, 0xff)
#define LIST_SELECTED PIX_RGB(0x10, 0x28, 0xc8)
#define LIST_BACK PIX_RGB(0x80, 0x90, 0xa8)
#define SCROLL_THUMB PIX_RGB(0x7c, 0x86, 0xa0)
#define PEAK PIX_RGB(0xd8, 0xd8, 0xe0)

/* Where things are, in the 120 x 120 picture. */
#define MAIN_H 58
#define LCD_X 3
#define LCD_Y 9
#define LCD_W 48
#define LCD_H 31
#define ICON_X 5
#define ICON_Y 13
#define DIGITS_Y 11
#define BARS_X 6
#define BARS_Y 24
#define BAR_W 2
#define BAR_PITCH 3
#define MARQUEE_X 53
#define MARQUEE_Y 9
#define MARQUEE_W 64
#define MARQUEE_H 11
#define INFO_Y 22
#define LIGHTS_Y 31
#define POS_X 3
#define POS_Y 42
#define POS_W 114
#define POS_H 6
#define THUMB_W 10
#define BUTTONS_Y 49
#define BUTTON_H 8
#define LIST_TOP 58
#define LIST_X 3
#define LIST_Y 66
#define LIST_W 114
#define LIST_H 47
#define ROW_H 9
#define ROWS 5
#define ROW_CHARS 17
#define FOOTER_Y 114

static unsigned board;


/* What was drawn, to find what changed. */
static struct {
    unsigned mode, elapsed, total, kbps, khz, channels, selected, top, count, thumb, buttons;
    int playing;
    bool digits_shown;
    const char *note;
    bool started;
} last;
static char marquee[200];
static unsigned marquee_length, marquee_offset, marquee_ms, blink_ms, bars_ms;
static uint8_t bars[SPECTRUM_BANDS], peaks[SPECTRUM_BANDS], drawn_bars[SPECTRUM_BANDS], drawn_peaks[SPECTRUM_BANDS];
static uint16_t peak_hold[SPECTRUM_BANDS];

/* Triangles 5 tall: three wide (small) or five (large), pointing right or left. */
static void triangle(unsigned x, unsigned y, bool large, bool left, uint16_t c) {
    static const uint8_t small_cols[3][2] = {{0, 5}, {1, 3}, {2, 1}};
    static const uint8_t large_cols[5][2] = {{0, 5}, {0, 5}, {1, 3}, {1, 3}, {2, 1}};
    unsigned n = large ? 5 : 3;
    for (unsigned k = 0; k < n; ++k) {
        const uint8_t *col = large ? large_cols[k] : small_cols[k];
        pix_fill(left ? x+n-1-k : x+k, y+col[0], 1, col[1], c);
    }
}

/* Segment digits in a 7 x 11 cell. */
static void digit(unsigned x, unsigned y, int d, bool shown) {
    static const uint8_t segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    unsigned on = d >= 0 && d <= 9 && shown ? segments[d] : 0;
    pix_fill(x, y, 7, 11, SK_LCD);
    struct { uint8_t x, y, w, h; } const seg[7] = {
        {1, 0, 5, 1}, {6, 1, 1, 4}, {6, 6, 1, 4}, {1, 10, 5, 1}, {0, 6, 1, 4}, {0, 1, 1, 4}, {1, 5, 5, 1}};
    for (unsigned s = 0; s < 7; ++s)
        pix_fill(x+seg[s].x, y+seg[s].y, seg[s].w, seg[s].h, on&(1u << s) ? SK_LCD_ON : SK_LCD_OFF);
}

static void draw_time(unsigned seconds, bool shown) {
    unsigned m = seconds/60, s = seconds%60;
    if (m > 99)
        m = 99;
    digit(12, DIGITS_Y, (int)(m/10), shown);
    digit(20, DIGITS_Y, (int)(m%10), shown);
    pix_fill(28, DIGITS_Y, 1, 11, SK_LCD);
    pix_pixel(28, DIGITS_Y+3, shown ? SK_LCD_ON : SK_LCD_OFF);
    pix_pixel(28, DIGITS_Y+7, shown ? SK_LCD_ON : SK_LCD_OFF);
    digit(30, DIGITS_Y, (int)(s/10), shown);
    digit(38, DIGITS_Y, (int)(s%10), shown);
    pix_mark(12, DIGITS_Y, 33, 11);
}

static void draw_state(unsigned mode) {
    pix_fill(ICON_X, ICON_Y, 5, 5, SK_LCD);
    if (mode == UI_PLAYING) {
        triangle(ICON_X, ICON_Y, true, false, SK_LCD_ON);
    } else if (mode == UI_PAUSED) {
        pix_fill(ICON_X, ICON_Y, 2, 5, PIX_RGB(0xf0, 0xd0, 0x30));
        pix_fill(ICON_X+3, ICON_Y, 2, 5, PIX_RGB(0xf0, 0xd0, 0x30));
    } else {
        pix_fill(ICON_X, ICON_Y, 5, 5, PIX_RGB(0xe0, 0x40, 0x30));
    }
    pix_mark(ICON_X, ICON_Y, 5, 5);
}

/* Bar colours, top row first: red, orange, yellow, then greens. */
static uint16_t bar_colour(unsigned row_from_top) {
    static const uint16_t c[SPECTRUM_TOP] = {
        PIX_RGB(0xff, 0x30, 0x20), PIX_RGB(0xff, 0x50, 0x20), PIX_RGB(0xff, 0x80, 0x18), PIX_RGB(0xff, 0xa8, 0x18),
        PIX_RGB(0xff, 0xd0, 0x20), PIX_RGB(0xf0, 0xf0, 0x20), PIX_RGB(0xc8, 0xf0, 0x20), PIX_RGB(0xa0, 0xf0, 0x20),
        PIX_RGB(0x78, 0xf0, 0x20), PIX_RGB(0x50, 0xe8, 0x20), PIX_RGB(0x30, 0xe0, 0x28), PIX_RGB(0x20, 0xd0, 0x28),
        PIX_RGB(0x18, 0xc0, 0x28), PIX_RGB(0x10, 0xb0, 0x28)};
    return c[row_from_top < SPECTRUM_TOP ? row_from_top : SPECTRUM_TOP-1];
}

static void draw_bars(void) {
    for (unsigned b = 0; b < SPECTRUM_BANDS; ++b) {
        unsigned x = BARS_X+b*BAR_PITCH;
        for (unsigned r = 0; r < SPECTRUM_TOP; ++r) {
            unsigned level = SPECTRUM_TOP-r;   /* the level this row shows */
            uint16_t c = level <= bars[b] ? bar_colour(r) : level == peaks[b] && peaks[b] ? PEAK : SK_LCD;
            pix_fill(x, BARS_Y+r, BAR_W, 1, c);
        }
        drawn_bars[b] = bars[b];
        drawn_peaks[b] = peaks[b];
    }
    pix_mark(BARS_X, BARS_Y, SPECTRUM_BANDS*BAR_PITCH, SPECTRUM_TOP);
}

static void draw_marquee(void) {
    pix_fill(MARQUEE_X+1, MARQUEE_Y+1, MARQUEE_W-2, MARQUEE_H-2, SK_LCD);
    int x0 = MARQUEE_X+2, x1 = MARQUEE_X+MARQUEE_W-2;
    unsigned width = marquee_length*6;
    if (width <= (unsigned)(x1-x0)) {
        pix_big_text(x0, MARQUEE_Y+2, marquee, SK_LCD_ON, x0, x1);
    } else {
        /* It goes round: the text, a gap, the text again. */
        int x = x0-(int)marquee_offset;
        x = pix_big_text(x, MARQUEE_Y+2, marquee, SK_LCD_ON, x0, x1);
        x = pix_big_text(x, MARQUEE_Y+2, " *** ", SK_LCD_ON, x0, x1);
        pix_big_text(x, MARQUEE_Y+2, marquee, SK_LCD_ON, x0, x1);
    }
    pix_mark(MARQUEE_X+1, MARQUEE_Y+1, MARQUEE_W-2, MARQUEE_H-2);
}

static void number(char *out, unsigned v, unsigned digits) {
    for (unsigned i = digits; i-- > 0; v /= 10)
        out[i] = (char)('0'+v%10);
    out[digits] = 0;
}

static void draw_info(unsigned kbps, unsigned khz, unsigned channels) {
    char t[4];
    pix_fill(MARQUEE_X+1, INFO_Y+1, 13, 5, SK_LCD);
    if (kbps) {
        number(t, kbps > 999 ? 999 : kbps, 3);
        pix_small_text(MARQUEE_X+1, INFO_Y+1, t, SK_LCD_ON);
    }
    pix_fill(89, INFO_Y+1, 9, 5, SK_LCD);
    if (khz) {
        number(t, khz > 99 ? 99 : khz, 2);
        pix_small_text(90, INFO_Y+1, t, SK_LCD_ON);
    }
    pix_small_text(MARQUEE_X, LIGHTS_Y, "MONO", channels == 1 ? SK_LCD_ON : SK_LCD_OFF);
    pix_small_text(74, LIGHTS_Y, "STEREO", channels == 2 ? SK_LCD_ON : SK_LCD_OFF);
    pix_mark(MARQUEE_X, INFO_Y, 64, 14);
}

static unsigned thumb_at(unsigned elapsed, unsigned total) {
    if (!total)
        return 0;
    if (elapsed > total)
        elapsed = total;
    return elapsed*(POS_W-THUMB_W)/total;
}

static void draw_position(unsigned at) {
    pix_fill(POS_X, POS_Y, POS_W, POS_H, GROOVE);
    pix_edge(POS_X, POS_Y, POS_W, POS_H, SK_INSET_DARK, SK_INSET_LIGHT);
    unsigned x = POS_X+at;
    pix_fill(x, POS_Y, THUMB_W, POS_H, SK_BUTTON);
    pix_edge(x, POS_Y, THUMB_W, POS_H, SK_BUTTON_LIGHT, SK_BUTTON_DARK);
    pix_fill(x+4, POS_Y+2, 2, 2, SK_BUTTON_DARK);
    pix_mark(POS_X, POS_Y, POS_W, POS_H);
}

enum { B_PREV, B_PLAY, B_PAUSE, B_STOP, B_NEXT, B_EJECT, BUTTONS };
static const uint8_t button_x[BUTTONS] = {3, 15, 27, 39, 51, 66};
static const uint8_t button_w[BUTTONS] = {12, 12, 12, 12, 12, 11};

static void draw_icon(unsigned which, unsigned x, unsigned y, uint16_t c) {
    switch (which) {
    case B_PREV:
        pix_fill(x, y, 1, 5, c);
        triangle(x+1, y, false, true, c);
        triangle(x+4, y, false, true, c);
        break;
    case B_NEXT:
        triangle(x, y, false, false, c);
        triangle(x+3, y, false, false, c);
        pix_fill(x+6, y, 1, 5, c);
        break;
    case B_PLAY:
        triangle(x+1, y, true, false, c);
        break;
    case B_PAUSE:
        pix_fill(x+1, y, 2, 5, c);
        pix_fill(x+4, y, 2, 5, c);
        break;
    case B_STOP:
        pix_fill(x+1, y, 5, 5, c);
        break;
    case B_EJECT:
        for (unsigned k = 0; k < 3; ++k)
            pix_fill(x+2-k, y+k, 1+2*k, 1, c);
        pix_fill(x, y+4, 5, 1, c);
        break;
    }
}

static void draw_buttons(unsigned down) {
    for (unsigned b = 0; b < BUTTONS; ++b) {
        unsigned x = button_x[b], w = button_w[b];
        bool d = down&(1u << b);
        pix_fill(x, BUTTONS_Y, w, BUTTON_H, d ? SK_BUTTON_DOWN : SK_BUTTON);
        pix_edge(x, BUTTONS_Y, w, BUTTON_H, d ? SK_BUTTON_DARK : SK_BUTTON_LIGHT, d ? SK_BUTTON_LIGHT : SK_BUTTON_DARK);
        unsigned iw = b == B_EJECT ? 5 : 7;
        draw_icon(b, x+(w-iw)/2+(d ? 1 : 0), BUTTONS_Y+1+(d ? 1 : 0)+(BUTTON_H-2-5)/2, SK_ICON);
    }
    pix_mark(3, BUTTONS_Y, 74, BUTTON_H);
}

/* The badge: a gold M with its shadow, in a dark square. */
static void draw_badge(void) {
    unsigned x = 104, y = BUTTONS_Y-1;
    pix_fill(x, y, 13, 10, SK_TITLE_BG);
    pix_edge(x, y, 13, 10, SK_INSET_DARK, SK_INSET_LIGHT);
    pix_big_text((int)x+5, (int)y+2, "M", SK_GOLD_SHADOW, 0, W);
    pix_big_text((int)x+4, (int)y+1, "M", SK_GOLD, 0, W);
}

static const char *list_hint(const struct ui_model *m) {
    if (m->selected >= m->count)
        return m->mode == UI_STOPPED || !m->count ? "PLAY: LEAVE" : "PLAY: STOP AND LEAVE";
    if (m->mode != UI_STOPPED)
        return "TAP: TRACK HOLD: SEEK";
    return board == 2 ? "< > PICK  PLAY GO" : "OCT-/+ PICK PLAY GO";
}

static void draw_footer(const struct ui_model *m) {
    pix_body(LIST_X, FOOTER_Y, LIST_W, 5);
    char t[8];
    unsigned shown = m->selected < m->count ? m->selected+1 : m->count;
    number(t, shown > 99 ? 99 : shown, 2);
    t[2] = '/';
    number(t+3, m->count > 99 ? 99 : m->count, 2);
    pix_small_text(LIST_X+1, FOOTER_Y, t, SK_LABEL);
    const char *hint = list_hint(m);
    pix_small_text(LIST_X+LIST_W-pix_small_width(hint), FOOTER_Y, hint, SK_LABEL);
    pix_mark(LIST_X, FOOTER_Y, LIST_W, 5);
}

static void draw_list(const struct ui_model *m, unsigned top) {
    unsigned x0 = LIST_X+1, y0 = LIST_Y+1, inner_w = LIST_W-2;
    pix_fill(x0, y0, inner_w, LIST_H-2, SK_LCD);
    if (m->note && !m->count) {
        unsigned n = (unsigned)strlen(m->note);
        unsigned w = n*6-1;
        pix_big_text((int)(x0+(inner_w-4-w)/2), (int)(y0+2*ROW_H+1), m->note, SK_LCD_ON, x0, x0+inner_w-4);
    }
    for (unsigned i = 0; i < ROWS && !(m->note && !m->count); ++i) {
        unsigned item = top+i, y = y0+i*ROW_H;
        if (item > m->count)
            break;
        bool selected = item == m->selected;
        if (selected)
            pix_fill(x0, y, inner_w-5, ROW_H, LIST_SELECTED);
        char line[ROW_CHARS+1];
        uint16_t c;
        if (item == m->count) {
            memcpy(line, "<< BACK", 8);
            c = LIST_BACK;
        } else {
            unsigned n = 0, number_of = item+1;
            if (number_of >= 10)
                line[n++] = (char)('0'+number_of/10%10);
            line[n++] = (char)('0'+number_of%10);
            line[n++] = '.';
            line[n++] = ' ';
            const char *name = m->name ? m->name(item) : "";
            while (*name && n < ROW_CHARS)
                line[n++] = *name++;
            line[n] = 0;
            c = (int)item == m->playing ? LIST_PLAYING : LIST_TEXT;
        }
        pix_big_text((int)x0+1, (int)y+1, line, c, x0, x0+inner_w-5);
    }
    /* The scroll bar, on the right. */
    unsigned bx = x0+inner_w-4, bh = LIST_H-2, items = m->count+1;
    pix_fill(bx, y0, 4, bh, PIX_RGB(0x1a, 0x1e, 0x2a));
    unsigned th = items > ROWS ? bh*ROWS/items : bh, ty = items > ROWS ? (bh-th)*top/(items-ROWS) : 0;
    if (th < 4)
        th = 4;
    pix_fill(bx, y0+ty, 4, th, SCROLL_THUMB);
    pix_edge(bx, y0+ty, 4, th, SK_BUTTON_LIGHT, SK_BUTTON_DARK);
    pix_mark(LIST_X+1, LIST_Y+1, LIST_W-2, LIST_H-2);
}

static void draw_static(void) {
    pix_body(0, 0, W, H);
    pix_edge(0, 0, W, MAIN_H, SK_EDGE_LIGHT, SK_EDGE_DARK);
    pix_edge(0, LIST_TOP, W, H-LIST_TOP, SK_EDGE_LIGHT, SK_EDGE_DARK);
    pix_title_bar(1, "");
    pix_title_bar(LIST_TOP+1, "PLAYLIST");
    pix_fill(LCD_X, LCD_Y, LCD_W, LCD_H, SK_LCD);
    pix_edge(LCD_X, LCD_Y, LCD_W, LCD_H, SK_INSET_DARK, SK_INSET_LIGHT);
    pix_fill(MARQUEE_X, MARQUEE_Y, MARQUEE_W, MARQUEE_H, SK_LCD);
    pix_edge(MARQUEE_X, MARQUEE_Y, MARQUEE_W, MARQUEE_H, SK_INSET_DARK, SK_INSET_LIGHT);
    pix_fill(MARQUEE_X, INFO_Y, 15, 7, SK_LCD);
    pix_edge(MARQUEE_X, INFO_Y, 15, 7, SK_INSET_DARK, SK_INSET_LIGHT);
    pix_small_text(70, INFO_Y+1, "KBPS", SK_LABEL);
    pix_fill(88, INFO_Y, 11, 7, SK_LCD);
    pix_edge(88, INFO_Y, 11, 7, SK_INSET_DARK, SK_INSET_LIGHT);
    pix_small_text(101, INFO_Y+1, "KHZ", SK_LABEL);
    pix_fill(LIST_X, LIST_Y, LIST_W, LIST_H, SK_LCD);
    pix_edge(LIST_X, LIST_Y, LIST_W, LIST_H, SK_INSET_DARK, SK_INSET_LIGHT);
    draw_badge();
    pix_mark(0, 0, W, H);
}

void ui_start(unsigned which_board) {
    board = which_board;
    memset(&last, 0, sizeof last);
    memset(bars, 0, sizeof bars);
    memset(peaks, 0, sizeof peaks);
    memset(peak_hold, 0, sizeof peak_hold);
    marquee[0] = 0;
    marquee_length = marquee_offset = marquee_ms = blink_ms = bars_ms = 0;
    pix_forget();
    draw_static();
    draw_time(0, true);
    draw_state(UI_STOPPED);
    draw_bars();
    draw_info(0, 0, 0);
    draw_position(0);
    draw_buttons(0);
    pix_send_all();
}

static void put_time(char *out, unsigned seconds) {
    unsigned m = seconds/60;
    unsigned n = 0;
    if (m >= 10)
        out[n++] = (char)('0'+m/10%10);
    out[n++] = (char)('0'+m%10);
    out[n++] = ':';
    out[n++] = (char)('0'+seconds%60/10);
    out[n++] = (char)('0'+seconds%10);
    out[n] = 0;
}

static void make_marquee(const struct ui_model *m, char *out, unsigned room) {
    unsigned n = 0;
    const char *name = 0;
    unsigned track = 0;
    if (m->note) {
        name = m->note;
    } else if (m->playing >= 0) {
        track = (unsigned)m->playing+1;
        name = m->name ? m->name((unsigned)m->playing) : "";
    } else if (m->selected < m->count) {
        track = m->selected+1;
        name = m->name ? m->name(m->selected) : "";
    } else {
        name = m->count ? "BACK TO THE MENU" : "MP3 PLAYER";
    }
    if (track) {
        if (track >= 10)
            out[n++] = (char)('0'+track/10%10);
        out[n++] = (char)('0'+track%10);
        out[n++] = '.';
        out[n++] = ' ';
    }
    while (*name && n+12 < room)
        out[n++] = *name++;
    if (m->playing >= 0 && !m->note && m->total) {
        out[n++] = ' ';
        out[n++] = '(';
        put_time(out+n, m->total);
        n += (unsigned)strlen(out+n);
        out[n++] = ')';
    }
    out[n] = 0;
}

static unsigned pressed_buttons(const struct ui_model *m) {
    unsigned down = 0;
    if (m->keys&1u)
        down |= 1u << B_PREV;
    if (m->keys&2u)
        down |= 1u << B_NEXT;
    if (m->keys&4u)
        down |= 1u << (m->selected >= m->count ? B_EJECT : m->mode == UI_PLAYING ? B_PAUSE : B_PLAY);
    if (m->keys&8u)
        down |= 1u << B_EJECT;
    return down;
}

void ui_update(const struct ui_model *m, unsigned ms) {
    if (!last.started) {
        last.started = true;
        last.mode = 0xff;
        last.selected = last.count = last.top = 0xffff;
        last.thumb = last.buttons = last.elapsed = last.kbps = 0xffff;
    }
    /* The time, which blinks while paused. */
    blink_ms = m->mode == UI_PAUSED ? blink_ms+ms : 0;
    bool shown = m->mode != UI_PAUSED || (blink_ms/500)%2 == 0;
    unsigned elapsed = m->playing >= 0 ? m->elapsed : 0;
    if (elapsed != last.elapsed || shown != last.digits_shown) {
        draw_time(elapsed, shown);
        last.elapsed = elapsed;
        last.digits_shown = shown;
    }
    if (m->mode != last.mode) {
        draw_state(m->mode);
        draw_footer(m);
    }
    /* The bars rise at once, and fall a step at a time; the peaks wait, then fall. */
    bars_ms += ms;
    if (bars_ms >= 40) {
        unsigned steps = bars_ms/40;
        bars_ms %= 40;
        for (unsigned b = 0; b < SPECTRUM_BANDS; ++b) {
            unsigned want = m->levels && m->mode == UI_PLAYING ? m->levels[b] : 0;
            bars[b] = (uint8_t)(want >= bars[b] ? want : bars[b] > steps ? bars[b]-steps : 0);
            if (bars[b] >= peaks[b]) {
                peaks[b] = bars[b];
                peak_hold[b] = 400;
            } else if (peak_hold[b] > 40*steps) {
                peak_hold[b] = (uint16_t)(peak_hold[b]-40*steps);
            } else if (peaks[b]) {
                peaks[b]--;
                peak_hold[b] = 40;
            }
        }
        if (memcmp(bars, drawn_bars, sizeof bars) || memcmp(peaks, drawn_peaks, sizeof peaks))
            draw_bars();
    }
    /* The title, scrolling a pixel each 60 ms when it doesn't fit. */
    char text[sizeof marquee];
    make_marquee(m, text, sizeof text);
    unsigned length = (unsigned)strlen(text);
    if (length != marquee_length || memcmp(text, marquee, length)) {
        memcpy(marquee, text, length+1);
        marquee_length = length;
        marquee_offset = 0;
        marquee_ms = 0;
        draw_marquee();
    } else if (marquee_length*6 > MARQUEE_W-4) {
        marquee_ms += ms;
        if (marquee_ms >= 60) {
            marquee_offset = (marquee_offset+marquee_ms/60)%((marquee_length+5)*6);
            marquee_ms %= 60;
            draw_marquee();
        }
    }
    unsigned khz = (m->hz+500)/1000;
    if (m->kbps != last.kbps || khz != last.khz || m->channels != last.channels) {
        draw_info(m->kbps, khz, m->channels);
        last.kbps = m->kbps;
        last.khz = khz;
        last.channels = m->channels;
    }
    unsigned thumb = m->playing >= 0 ? thumb_at(m->elapsed, m->total) : 0;
    if (thumb != last.thumb) {
        draw_position(thumb);
        last.thumb = thumb;
    }
    unsigned down = pressed_buttons(m);
    if (down != last.buttons) {
        draw_buttons(down);
        last.buttons = down;
    }
    unsigned top = last.top == 0xffff ? 0 : last.top;
    if (m->selected < top)
        top = m->selected;
    if (m->selected >= top+ROWS)
        top = m->selected-ROWS+1;
    if (top != last.top || m->selected != last.selected || m->playing != last.playing || m->count != last.count ||
        m->note != last.note) {
        draw_list(m, top);
        draw_footer(m);
        last.top = top;
        last.selected = m->selected;
        last.playing = m->playing;
        last.count = m->count;
        last.note = m->note;
    }
    last.mode = m->mode;
    pix_send();
}
