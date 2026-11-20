/* SPDX-License-Identifier: MIT */
#include "pixel.h"
#include "display.h"
#include "pixel_font.h"
#include <string.h>

#define W PIX_W
#define H PIX_H

uint16_t pix_fb[PIX_H][PIX_W];
static void (*hook_before)(void), (*hook_after)(void), (*hook_between)(void);

/* What's waiting to be sent. */
#define MOST_DIRTY 24
static struct { uint8_t x, y, w, h; } dirty[MOST_DIRTY];
static unsigned dirty_count, budget;

void pix_forget(void) {
    dirty_count = 0;
}

void pix_hooks(void (*before)(void), void (*after)(void), void (*between)(void)) {
    hook_before = before;
    hook_after = after;
    hook_between = between;
}

void pix_mark(unsigned x, unsigned y, unsigned w, unsigned h) {
    for (unsigned i = 0; i < dirty_count; ++i)
        if (dirty[i].x == x && dirty[i].y == y && dirty[i].w == w && dirty[i].h == h)
            return;
    if (dirty_count == MOST_DIRTY) {
        dirty[0].x = 0;
        dirty[0].y = 0;
        dirty[0].w = W;
        dirty[0].h = H;
        dirty_count = 1;
        return;
    }
    dirty[dirty_count].x = (uint8_t)x;
    dirty[dirty_count].y = (uint8_t)y;
    dirty[dirty_count].w = (uint8_t)w;
    dirty[dirty_count].h = (uint8_t)h;
    ++dirty_count;
}

void pix_send_all(void) {
    unsigned kept = budget;
    budget = 0;
    pix_send();
    budget = kept;
}

void pix_budget(unsigned bytes) {
    budget = bytes;
}

/* Sends what's waiting, oldest first, up to the budget; the rest goes on the next call. */
void pix_send(void) {
    if (!dirty_count)
        return;
    static uint8_t out[240*2*2*8];
    unsigned spent = 0, done = 0;
    if (hook_before)
        hook_before();
    while (done < dirty_count && (!budget || spent < budget)) {
        unsigned x = dirty[done].x, y = dirty[done].y, w = dirty[done].w, h = dirty[done].h;
        unsigned per = sizeof out/(8*w);
        unsigned rows = h < per ? h : per;
        uint8_t *p = out;
        for (unsigned r = 0; r < rows; ++r) {
            const uint16_t *src = pix_fb[y+r]+x;
            uint8_t *line = p;
            for (unsigned c = 0; c < w; ++c) {
                uint8_t hi = (uint8_t)(src[c] >> 8), lo = (uint8_t)src[c];
                *p++ = hi;
                *p++ = lo;
                *p++ = hi;
                *p++ = lo;
            }
            memcpy(p, line, 4*w);
            p += 4*w;
        }
        display_pixels(2*x, 2*y, 2*w, 2*rows, out);
        if (hook_between)
            hook_between();
        spent += 8*w*rows;
        if (rows == h) {
            ++done;
        } else {
            dirty[done].y = (uint8_t)(y+rows);
            dirty[done].h = (uint8_t)(h-rows);
        }
    }
    memmove(dirty, dirty+done, (dirty_count-done)*sizeof dirty[0]);
    dirty_count -= done;
    if (hook_after)
        hook_after();
}

void pix_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c) {
    for (unsigned r = y; r < y+h && r < H; ++r)
        for (unsigned k = x; k < x+w && k < W; ++k)
            pix_fb[r][k] = c;
}

void pix_pixel(int x, int y, uint16_t c) {
    if (x >= 0 && y >= 0 && x < W && y < H)
        pix_fb[y][x] = c;
}

/* A raised or sunken edge, one pixel wide. */
void pix_edge(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t top_left, uint16_t bottom_right) {
    pix_fill(x, y, w, 1, top_left);
    pix_fill(x, y, 1, h, top_left);
    pix_fill(x, y+h-1, w, 1, bottom_right);
    pix_fill(x+w-1, y, 1, h, bottom_right);
}

void pix_body(unsigned x, unsigned y, unsigned w, unsigned h) {
    for (unsigned r = y; r < y+h; ++r)
        pix_fill(x, r, w, 1, (r&1) ? SK_BODY_ALT : SK_BODY);
}

/* Text, clipped to [clip_x0, clip_x1). Returns where the next character would go. */
int pix_big_text(int x, int y, const char *s, uint16_t c, int clip_x0, int clip_x1) {
    for (; *s; ++s, x += 6) {
        unsigned ch = (uint8_t)*s;
        const uint8_t *g = big_font[(ch < 32 || ch > 126 ? '?' : ch)-32];
        for (int r = 0; r < 7; ++r)
            for (int k = 0; k < 5; ++k)
                if (g[r]&(1u << k) && x+k >= clip_x0 && x+k < clip_x1)
                    pix_pixel(x+k, y+r, c);
    }
    return x;
}

unsigned pix_small_width(const char *s) {
    unsigned w = 0;
    for (; *s; ++s) {
        unsigned ch = (uint8_t)*s;
        unsigned gw = ch >= 32 && ch <= 126 ? small_font[ch-32][0] : 0;
        w += (gw ? gw : 3)+1;
    }
    return w ? w-1 : 0;
}

unsigned pix_small_text(unsigned x, unsigned y, const char *s, uint16_t c) {
    for (; *s; ++s) {
        unsigned ch = (uint8_t)*s;
        const uint8_t *g = ch >= 32 && ch <= 126 ? small_font[ch-32] : small_font[0];
        unsigned gw = g[0] ? g[0] : 3;
        if (g[0])
            for (unsigned r = 0; r < 5; ++r)
                for (unsigned k = 0; k < gw; ++k)
                    if (g[1+r]&(1u << k))
                        pix_pixel((int)(x+k), (int)(y+r), c);
        x += gw+1;
    }
    return x;
}

/* A title bar with ridges either side of the title, or right across without one. */
void pix_title_bar(unsigned y, const char *title) {
    pix_fill(1, y, W-2, 7, SK_TITLE_BG);
    unsigned tw = pix_small_width(title), tx = (W-tw)/2;
    for (unsigned r = 1; r < 6; ++r) {
        uint16_t c = (r&1) ? SK_RIDGE_LIGHT : SK_RIDGE_DARK;
        if (!tw) {
            pix_fill(10, y+r, W-24, 1, c);
            continue;
        }
        pix_fill(10, y+r, tx-14, 1, c);
        pix_fill(tx+tw+4, y+r, W-14-(tx+tw+4), 1, c);
    }
    pix_small_text(tx, y+1, title, SK_GOLD);
    /* A menu box on the left and two on the right, as the windows of the time had. */
    static const unsigned boxes[] = {3, 104, 111};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned bx = boxes[i];
        pix_fill(bx, y+1, 5, 5, SK_BUTTON);
        pix_edge(bx, y+1, 5, 5, SK_BUTTON_LIGHT, SK_BUTTON_DARK);
    }
    pix_pixel(5, (int)y+3, SK_ICON);
    pix_fill(106, y+4, 1, 1, SK_ICON);
    pix_fill(113, y+3, 1, 1, SK_ICON);
}

void pix_sprite(int x, int y, unsigned w, unsigned h, const uint16_t *data, uint16_t transparent) {
    for (unsigned r = 0; r < h; ++r)
        for (unsigned c = 0; c < w; ++c, ++data)
            if (*data != transparent)
                pix_pixel(x+(int)c, y+(int)r, *data);
}
