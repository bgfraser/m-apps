/* SPDX-License-Identifier: MIT */
/* Drawn into a full-size picture, with soft edges, in the launcher's colours and font (apps/picker). Only what changed
 * is sent. */
#include "look.h"
#include "art.h"
#include "canvas.h"
#include <string.h>

#define SCREEN CANVAS_RGB(236, 243, 251)

/* Where things go. */
#define CARD_X 16
#define CARD_Y 50
#define LAPTOP_X 124
#define LAPTOP_Y (CARD_Y+SD_CARD_PORT_Y-LAPTOP_PORT_Y)
#define CABLE_Y (CARD_Y+SD_CARD_PORT_Y)
#define CABLE_X0 (CARD_X+SD_CARD_W+4)
#define CABLE_X1 (LAPTOP_X-4)
#define PANEL_X 12
#define PANEL_Y 130
#define PANEL_W 216
#define PANEL_H 58
#define DOT_GAP 10

static unsigned board;

void look_budget(unsigned bytes) {
    canvas_budget(bytes);
}

void look_hooks(void (*before)(void), void (*after)(void)) {
    canvas_hooks(before, after, 0);
}

/* ---- numbers ---- */

static char *put_text(char *p, const char *s) {
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_number(char *p, uint32_t v) {
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0'+v%10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

/* MB, or GB with a tenth. */
static char *put_size(char *p, uint32_t mib) {
    if (mib < 1024)
        return put_text(put_number(p, mib), " MB");
    p = put_number(p, mib/1024);
    *p++ = '.';
    *p++ = (char)('0'+mib%1024*10/1024);
    return put_text(p, " GB");
}

/* ---- the parts ---- */

static struct {
    unsigned state, screen_phase, read_kbs, write_kbs, read_mib, written_mib;
    int flow;
    bool started;
} last;
static unsigned dots_ms, screen_ms, phase;
static uint32_t card_mib;

/* The cable: a line, with dots moving along it while data does, towards the laptop when reading. */
static void draw_cable(int flow) {
    int x0 = CABLE_X0, x1 = CABLE_X1, y = CABLE_Y;
    canvas_clear(x0-3, y-4, x1-x0+7, 9);
    canvas_shape(x0, y-1, x1-x0, 2, 1, CANVAS_LINE, 16, 1);
    if (flow) {
        uint16_t c = flow > 0 ? CANVAS_BLUE : CANVAS_ORANGE;
        int shift = (int)(phase%DOT_GAP);
        for (int x = x0+(flow > 0 ? shift : DOT_GAP-1-shift); x < x1-2; x += DOT_GAP)
            canvas_shape(x, y-3, 6, 6, 3, c, 16, 1);
    }
    canvas_mark(x0-3, y-4, x1-x0+7, 9);
}

/* A bar under the folder on the laptop's screen, with a piece running along it while data moves. */
static void draw_bar(unsigned screen_phase, int flow) {
    int bx = LAPTOP_X+LAPTOP_SCREEN_X+LAPTOP_SCREEN_W/2-20, by = LAPTOP_Y+LAPTOP_SCREEN_Y+LAPTOP_SCREEN_H/2+12;
    for (int y = by-1; y < by+5; ++y)
        for (int x = bx-1; x < bx+41; ++x)
            canvas_fb[y][x] = SCREEN;
    if (flow) {
        canvas_shape(bx, by, 40, 4, 2, CANVAS_RGB(214, 224, 236), 16, 1);
        canvas_shape(bx+(int)(screen_phase%5)*7, by, 12, 4, 2, flow > 0 ? CANVAS_BLUE : CANVAS_ORANGE, 16, 1);
    }
    canvas_mark(bx-1, by-1, 42, 6);
}

/* The laptop's screen: what the computer sees. */
static void draw_screen(unsigned state) {
    int sx = LAPTOP_X+LAPTOP_SCREEN_X, sy = LAPTOP_Y+LAPTOP_SCREEN_Y;
    int cx = sx+LAPTOP_SCREEN_W/2, cy = sy+LAPTOP_SCREEN_H/2;
    canvas_shape(sx, sy, LAPTOP_SCREEN_W, LAPTOP_SCREEN_H, 2, SCREEN, 16, 1);
    switch (state) {
    case LOOK_STARTING:
    case LOOK_WAITING:
        canvas_sprite(cx-FOLDER_DIM_W/2, cy-FOLDER_DIM_H/2-2, FOLDER_DIM_W, FOLDER_DIM_H, folder_dim_rgb, folder_dim_alpha);
        break;
    case LOOK_NO_CARD:
        canvas_sprite(cx-CROSS_W/2, cy-CROSS_H/2, CROSS_W, CROSS_H, cross_rgb, cross_alpha);
        break;
    case LOOK_CONNECTED:
        canvas_sprite(cx-FOLDER_W/2, cy-FOLDER_H/2-4, FOLDER_W, FOLDER_H, folder_rgb, folder_alpha);
        break;
    default:
        canvas_sprite(cx-CHECK_W/2, cy-CHECK_H/2, CHECK_W, CHECK_H, check_rgb, check_alpha);
        break;
    }
    canvas_mark(sx, sy, LAPTOP_SCREEN_W, LAPTOP_SCREEN_H);
}

static const char *state_title(unsigned state, int flow) {
    switch (state) {
    case LOOK_STARTING: return "Starting";
    case LOOK_NO_CARD: return "No card found";
    case LOOK_WAITING: return "Connect USB";
    case LOOK_CONNECTED: return flow > 0 ? "Reading" : flow < 0 ? "Writing" : "Connected";
    case LOOK_EJECTED: return "Ejected";
    default: return "Leaving";
    }
}

/* The panel's words: what's happening, the speed, and a line about it. White goes under them, so the panel's edge
 * and shadow are drawn only when it's new. */
static void draw_words(const struct look_model *m, int flow) {
    int x = PANEL_X, y = PANEL_Y;
    for (int py = y+6; py < y+PANEL_H-6; ++py)
        for (int px = x+10; px < x+PANEL_W-10; ++px)
            canvas_fb[py][px] = CANVAS_WHITE;
    canvas_text(x+14, y+9, CANVAS_BIG, state_title(m->state, flow), m->state == LOOK_NO_CARD ? CANVAS_RED : CANVAS_INK, 8);
    char line[48];
    if (flow) {
        put_text(put_number(line, flow > 0 ? m->read_kbs : m->write_kbs), " KB/s");
        canvas_text(x+PANEL_W-14-canvas_text_w(CANVAS_BIG, line, 8), y+9, CANVAS_BIG, line, flow > 0 ? CANVAS_BLUE : CANVAS_ORANGE, 8);
    }
    const char *detail = line;
    switch (m->state) {
    case LOOK_STARTING:
        detail = "Getting the card ready";
        break;
    case LOOK_NO_CARD:
        detail = "Check the card, then try again";
        break;
    case LOOK_WAITING:
        put_text(put_size(line, m->card_mib), " card, ready to share");
        break;
    case LOOK_EJECTED:
        detail = "You can unplug the cable now";
        break;
    case LOOK_LEAVING:
        detail = "Back to the menu";
        break;
    default:
        if (m->read_kib >= 1024 || m->written_kib >= 1024)
            put_text(put_size(put_text(put_size(line, m->read_kib/1024), " read, "), m->written_kib/1024), " written");
        else
            put_text(put_size(line, m->card_mib), " card");
        break;
    }
    canvas_text(x+14, y+33, CANVAS_SMALL, detail, CANVAS_DIM, 8);
    canvas_mark(x+10, y+6, PANEL_W-20, PANEL_H-12);
}

static void draw_panel(void) {
    int x = PANEL_X, y = PANEL_Y;
    canvas_clear(x-4, y-4, PANEL_W+8, PANEL_H+10);
    canvas_card(x, y, PANEL_W, PANEL_H, 12, false);
    canvas_mark(x-4, y-4, PANEL_W+8, PANEL_H+10);
}

static void draw_static(void) {
    canvas_clear(0, 0, CANVAS_N, CANVAS_N);
    canvas_text(16, 14, CANVAS_BIG, "USB Disk", CANVAS_INK, 8);
    canvas_sprite(CARD_X, CARD_Y, SD_CARD_W, SD_CARD_H, sd_card_rgb, sd_card_alpha);
    canvas_sprite(LAPTOP_X, LAPTOP_Y, LAPTOP_W, LAPTOP_H, laptop_rgb, laptop_alpha);
    canvas_bar("Eject before unplugging", CANVAS_DIM, board == 2 ? "SEQ REC LEAVE" : "REC LEAVE");
    canvas_mark(0, 0, CANVAS_N, CANVAS_N);
}

static void draw_size(void) {
    canvas_clear(130, 12, 98, 20);
    if (card_mib) {
        char line[24];
        put_text(put_size(line, card_mib), " card");
        canvas_text(CANVAS_N-14-canvas_text_w(CANVAS_SMALL, line, 8), 17, CANVAS_SMALL, line, CANVAS_DIM, 8);
    }
    canvas_mark(130, 12, 98, 20);
}

void look_start(unsigned which_board) {
    board = which_board;
    memset(&last, 0, sizeof last);
    dots_ms = screen_ms = phase = 0;
    card_mib = 0;
    canvas_forget();
    draw_static();
    draw_panel();
    draw_cable(0);
    canvas_send_all();
}

void look_update(const struct look_model *m, unsigned ms) {
    if (!last.started) {
        last.started = true;
        last.state = 0xff;
    }
    int flow = m->state != LOOK_CONNECTED ? 0 : m->read_kbs > m->write_kbs ? 1 : m->write_kbs ? -1 : 0;
    /* The dots move a pixel each 40 ms, and the bar on the screen a step each 150 ms. */
    dots_ms += ms;
    bool moved = false;
    if (flow && dots_ms >= 40) {
        phase += dots_ms/40;
        dots_ms %= 40;
        moved = true;
    }
    screen_ms += ms;
    unsigned screen_phase = flow ? screen_ms/150 : 0;
    if (flow != last.flow || moved)
        draw_cable(flow);
    /* The bar is only there while connected, under the folder. */
    if (m->state != last.state) {
        draw_screen(m->state);
        if (m->state == LOOK_CONNECTED)
            draw_bar(screen_phase, flow);
    } else if (m->state == LOOK_CONNECTED && (flow != last.flow || screen_phase != last.screen_phase)) {
        draw_bar(screen_phase, flow);
    }
    if (m->card_mib != card_mib) {
        card_mib = m->card_mib;
        draw_size();
    }
    unsigned read_mib = m->read_kib/1024, written_mib = m->written_kib/1024;
    if (m->state != last.state || flow != last.flow || (flow > 0 && m->read_kbs != last.read_kbs) ||
        (flow < 0 && m->write_kbs != last.write_kbs) || read_mib != last.read_mib || written_mib != last.written_mib)
        draw_words(m, flow);
    last.state = m->state;
    last.flow = flow;
    last.screen_phase = screen_phase;
    last.read_kbs = m->read_kbs;
    last.write_kbs = m->write_kbs;
    last.read_mib = read_mib;
    last.written_mib = written_mib;
    canvas_send();
}
