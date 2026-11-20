/* SPDX-License-Identifier: MIT */
/* The shapes, the background and the text are drawn as apps/picker's scene.c draws them. */
#include "canvas.h"
#include "display.h"
#include "font.h"
#include <string.h>

#define N CANVAS_N

uint16_t canvas_fb[N][N];
static void (*hook_before)(void), (*hook_after)(void), (*hook_between)(void);

/* ---- sending ---- */

#define MOST_DIRTY 24
static struct { uint8_t x, y; uint16_t w, h; } dirty[MOST_DIRTY];
static unsigned dirty_count, budget;

void canvas_budget(unsigned bytes) {
    budget = bytes;
}

void canvas_hooks(void (*before)(void), void (*after)(void), void (*between)(void)) {
    hook_before = before;
    hook_after = after;
    hook_between = between;
}

void canvas_forget(void) {
    dirty_count = 0;
}

void canvas_mark(int x, int y, int w, int h) {
    if (x < 0)
        w += x, x = 0;
    if (y < 0)
        h += y, y = 0;
    if (x+w > N)
        w = N-x;
    if (y+h > N)
        h = N-y;
    if (w <= 0 || h <= 0)
        return;
    for (unsigned i = 0; i < dirty_count; ++i)
        if (dirty[i].x == x && dirty[i].y == y && dirty[i].w == w && dirty[i].h == h)
            return;
    if (dirty_count == MOST_DIRTY) {
        dirty[0].x = dirty[0].y = 0;
        dirty[0].w = dirty[0].h = N;
        dirty_count = 1;
        return;
    }
    dirty[dirty_count].x = (uint8_t)x;
    dirty[dirty_count].y = (uint8_t)y;
    dirty[dirty_count].w = (uint16_t)w;
    dirty[dirty_count].h = (uint16_t)h;
    ++dirty_count;
}

void canvas_send(void) {
    if (!dirty_count)
        return;
    static uint8_t out[N*2*16];
    unsigned spent = 0, done = 0;
    if (hook_before)
        hook_before();
    while (done < dirty_count && (!budget || spent < budget)) {
        unsigned x = dirty[done].x, y = dirty[done].y, w = dirty[done].w, h = dirty[done].h;
        unsigned per = sizeof out/(2*w), rows = h < per ? h : per;
        uint8_t *p = out;
        for (unsigned r = 0; r < rows; ++r)
            for (unsigned c = 0; c < w; ++c) {
                uint16_t v = canvas_fb[y+r][x+c];
                *p++ = (uint8_t)(v >> 8);
                *p++ = (uint8_t)v;
            }
        display_pixels(x, y, w, rows, out);
        if (hook_between)
            hook_between();
        spent += 2*w*rows;
        if (rows == h) {
            ++done;
        } else {
            dirty[done].y = (uint8_t)(y+rows);
            dirty[done].h = (uint16_t)(h-rows);
        }
    }
    memmove(dirty, dirty+done, (dirty_count-done)*sizeof dirty[0]);
    dirty_count -= done;
    if (hook_after)
        hook_after();
}

void canvas_send_all(void) {
    unsigned kept = budget;
    budget = 0;
    canvas_send();
    budget = kept;
}

/* ---- colour and shapes ---- */

static uint16_t mix(uint16_t under, uint16_t over, unsigned a, unsigned of) {
    unsigned r = under >> 11, g = (under >> 5)&63u, b = under&31u;
    unsigned R = over >> 11, G = (over >> 5)&63u, B = over&31u;
    r = (r*(of-a)+R*a+of/2)/of;
    g = (g*(of-a)+G*a+of/2)/of;
    b = (b*(of-a)+B*a+of/2)/of;
    return (uint16_t)(r << 11 | g << 5 | b);
}

void canvas_blend(int x, int y, uint16_t c, unsigned a) {
    if ((unsigned)x >= N || (unsigned)y >= N || !a)
        return;
    canvas_fb[y][x] = a >= 16 ? c : mix(canvas_fb[y][x], c, a, 16);
}

static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* Near white at the top, a cool pale blue-grey at the bottom, dithered. */
uint16_t canvas_back(int x, int y) {
    unsigned d = bayer[(y&3)*4+(x&3)];
    unsigned r = 246u-(unsigned)y*22u/N+d/2, g = 248u-(unsigned)y*16u/N+d/4, b = 252u-(unsigned)y*6u/N+d/2;
    return (uint16_t)((r > 255 ? 255 : r) >> 3 << 11 | (g > 255 ? 255 : g) >> 2 << 5 | (b > 255 ? 255 : b) >> 3);
}

void canvas_clear(int x, int y, int w, int h) {
    for (int py = y; py < y+h; ++py)
        for (int px = x; px < x+w; ++px)
            if ((unsigned)px < N && (unsigned)py < N)
                canvas_fb[py][px] = canvas_back(px, py);
}

void canvas_fill(int x, int y, int w, int h, uint16_t c) {
    for (int py = y; py < y+h; ++py)
        for (int px = x; px < x+w; ++px)
            if ((unsigned)px < N && (unsigned)py < N)
                canvas_fb[py][px] = c;
}

static unsigned isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r+bit) {
            v -= r+bit;
            r = (r >> 1)+bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* The distance from a pixel's centre to a rounded rectangle's edge, in 1/16 pixel, negative inside. */
static int distance(int px, int py, int x, int y, int w, int h, int r) {
    int qx = px*16+8-(x*16+w*8), qy = py*16+8-(y*16+h*8);
    qx = (qx < 0 ? -qx : qx)-(w*8-r*16);
    qy = (qy < 0 ? -qy : qy)-(h*8-r*16);
    if (qx <= 0 || qy <= 0)
        return (qx > qy ? qx : qy)-r*16;
    return (int)isqrt((uint32_t)(qx*qx+qy*qy))-r*16;
}

static unsigned cover(int d, int soft) {
    int f = soft*16, a = (f/2-d)*16/f;
    return a < 0 ? 0u : a > 16 ? 16u : (unsigned)a;
}

void canvas_shape(int x, int y, int w, int h, int r, uint16_t c, unsigned a, int soft) {
    int m = soft/2+1;
    for (int py = y-m; py < y+h+m; ++py)
        for (int px = x-m; px < x+w+m; ++px) {
            /* Well inside, with no edge to soften: just the colour. */
            if (a == 16 && px >= x+r+soft && px < x+w-r-soft && py >= y+soft && py < y+h-soft) {
                if ((unsigned)px < N && (unsigned)py < N)
                    canvas_fb[py][px] = c;
                continue;
            }
            canvas_blend(px, py, c, a*cover(distance(px, py, x, y, w, h, r), soft)/16u);
        }
}

void canvas_card(int x, int y, int w, int h, int r, bool marked) {
    canvas_shape(x, y+2, w, h, r, CANVAS_SHADOW, 5, 6);
    canvas_shape(x, y, w, h, r, CANVAS_WHITE, 16, 1);
    if (marked) {
        /* A ring: the blue, then white inside it again. */
        canvas_shape(x, y, w, h, r, CANVAS_BLUE, 16, 1);
        canvas_shape(x+2, y+2, w-4, h-4, r-2 > 0 ? r-2:0, CANVAS_WHITE, 16, 1);
    }
}

void canvas_sprite(int x, int y, int w, int h, const uint16_t *rgb, const uint8_t *alpha) {
    for (int i = 0; i < w*h; ++i) {
        unsigned a = i&1 ? alpha[i/2]&15u : alpha[i/2] >> 4;
        canvas_blend(x+i%w, y+i/w, rgb[i], (a*17u+8u) >> 4);
    }
}

/* ---- text ---- */

static const font_t *face(int font) {
    return font == CANVAS_BIG ? &FONT_B : &FONT_XS;
}

static unsigned glyph(const font_t *f, unsigned ch) {
    if (ch < f->first || ch > f->last)
        ch = '?';
    return ch-f->first;
}

int canvas_text_w(int font, const char *s, int scale) {
    const font_t *f = face(font);
    int w = 0;
    for (; *s; ++s)
        w += f->adv[glyph(f, (uint8_t)*s)];
    return w*scale/8;
}

static unsigned lit(const uint8_t *rows, int w, int h, int gx, int gy) {
    if (gx < 0 || gy < 0 || gx >= w || gy >= h)
        return 0;
    unsigned a = rows[gy*((w+1)/2)+gx/2];
    return gx&1 ? a&15u : a >> 4;
}

int canvas_text(int x, int y, int font, const char *s, uint16_t c, int scale) {
    const font_t *f = face(font);
    int pen = x*8;
    for (; *s; ++s) {
        unsigned gi = glyph(f, (uint8_t)*s);
        const uint8_t *gd = f->data+f->off[gi];
        int w = f->bw[gi], h = gd[2];
        int bx = (pen+(gd[0]-f->pad)*scale)/8, by = y+gd[1]*scale/8;
        int ow = (w*scale+7)/8+1, oh = (h*scale+7)/8+1;
        for (int oy = 0; oy < oh; ++oy)
            for (int ox = 0; ox < ow; ++ox) {
                unsigned a;
                if (scale == 8) {
                    a = lit(gd+3, w, h, ox, oy);
                } else {
                    /* Where the pixel's centre falls in the glyph, in 1/16, and the four around it. */
                    int sx = ((2*ox+1)*64/scale-8), sy = ((2*oy+1)*64/scale-8);
                    int gx = sx >> 4, gy = sy >> 4, fx = sx&15, fy = sy&15;
                    if (sx < 0)
                        gx = -1, fx = sx+16;
                    if (sy < 0)
                        gy = -1, fy = sy+16;
                    unsigned top = lit(gd+3, w, h, gx, gy)*(16u-(unsigned)fx)+lit(gd+3, w, h, gx+1, gy)*(unsigned)fx;
                    unsigned bottom = lit(gd+3, w, h, gx, gy+1)*(16u-(unsigned)fx)+lit(gd+3, w, h, gx+1, gy+1)*(unsigned)fx;
                    a = (top*(16u-(unsigned)fy)+bottom*(unsigned)fy) >> 8;
                }
                canvas_blend(bx+ox, by+oy, c, (a*17u+8u) >> 4);
            }
        pen += f->adv[gi]*scale;
    }
    return pen/8;
}

const char *canvas_fit(int font, const char *s, int w) {
    static char out[64];
    if (canvas_text_w(font, s, 8) <= w)
        return s;
    unsigned n = 0;
    while (s[n] && n < sizeof out-4)
        out[n] = s[n], ++n;
    out[n] = 0;
    while (n && canvas_text_w(font, out, 8)+canvas_text_w(font, "...", 8) > w)
        out[--n] = 0;
    while (n && out[n-1] == ' ')
        out[--n] = 0;
    memcpy(out+n, "...", 4);
    return out;
}

void canvas_bar(const char *top, uint16_t top_colour, const char *bottom) {
    canvas_clear(0, CANVAS_BAR_Y, N, N-CANVAS_BAR_Y);
    for (int y = CANVAS_BAR_Y; y < N; ++y)
        for (int x = 0; x < N; ++x)
            canvas_blend(x, y, CANVAS_WHITE, 9);
    canvas_fill(0, CANVAS_BAR_Y, N, 1, CANVAS_LINE);
    int mark_end = canvas_text(12, CANVAS_BAR_Y+7, CANVAS_BIG, "m-apps", CANVAS_WORDMARK, 12);
    if (top && *top) {
        const char *t = canvas_fit(CANVAS_SMALL, top, N-12-mark_end-10);
        canvas_text(N-12-canvas_text_w(CANVAS_SMALL, t, 8), CANVAS_BAR_Y+5, CANVAS_SMALL, t, top_colour, 8);
    }
    if (bottom && *bottom) {
        const char *b = canvas_fit(CANVAS_SMALL, bottom, N-12-mark_end-10);
        canvas_text(N-12-canvas_text_w(CANVAS_SMALL, b, 8), CANVAS_BAR_Y+21, CANVAS_SMALL, b, CANVAS_FAINT, 8);
    }
    canvas_mark(0, CANVAS_BAR_Y, N, N-CANVAS_BAR_Y);
}
