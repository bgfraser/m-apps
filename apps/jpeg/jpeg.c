/* SPDX-License-Identifier: MIT */
/* Tries the AC79's JPEG decoder: decodes the pictures in images.h, shows them, and logs how long the decoder took.
 * One picture's planes are left in `dump` (built with jpeg_CFLAGS=-DDUMP=n), for checking on a computer. */
#include "app.h"
#include "images.h"
#include "jpeg_hw.h"
#include <string.h>

#define WHITE 0xffff
#define GREEN 0x07e0
#define YELLOW 0xffe0
#define RED 0xf800
#define BLACK 0x0000
#define TIMER_CON REG(0x10800u)
#define TIMER_COUNT REG(0x10804u)
#define TIMER_PERIOD REG(0x10808u)
#define TICKS_PER_US 24u
#define SHOW_MS 2500u
#define REPEATS 10u
#ifndef DUMP
#define DUMP 0
#endif
#define IMAGES (sizeof images/sizeof images[0])

static uint8_t work[480*16*2] __attribute__((aligned(4)));
uint8_t dump[240*240*3] __attribute__((aligned(4)));
static uint8_t line[240*2];

struct view {
    unsigned index, scale, out_width, x0, y0;
    bool show;
};

static void timer_start(void) {
    if ((TIMER_CON&0xfu) == ((2u << 2) | 1u))
        return;
    TIMER_CON = 0;
    TIMER_PERIOD = 0xffffffffu;
    TIMER_COUNT = 0;
    TIMER_CON = (2u << 2) | 1u | (1u << 14);
}

static uint32_t ticks(void) {
    return TIMER_COUNT;
}

static unsigned us(uint32_t t) {
    return t/TICKS_PER_US;
}

static uint8_t clamp(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* JFIF's YCbCr to RGB565, big-endian for display_pixels(). */
static void rgb(uint8_t *out, int y, int cb, int cr) {
    int d = cb-128, e = cr-128;
    uint8_t r = clamp(y+((91881*e) >> 16));
    uint8_t g = clamp(y-((22554*d+46802*e) >> 16));
    uint8_t b = clamp(y+((116130*d) >> 16));
    uint16_t c = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
    out[0] = (uint8_t)(c >> 8);
    out[1] = (uint8_t)c;
}

static bool row(void *priv, const struct jpeg_image *im, unsigned first, const uint8_t *y, const uint8_t *cb,
                const uint8_t *cr) {
    struct view *v = priv;
    if (v->index == DUMP && im->width*im->height*3 <= sizeof dump) {
        for (unsigned l = 0; l < im->lines && first+l < im->height; ++l)
            memcpy(dump+(first+l)*im->width, y+l*im->width, im->width);
        unsigned chroma_height = im->format == JPEG_420 ? (im->height+1)/2 : im->height;
        unsigned chroma_first = first/im->lines*im->chroma_lines;
        uint8_t *planes = dump+im->width*im->height;
        for (unsigned l = 0; l < im->chroma_lines && chroma_first+l < chroma_height; ++l) {
            memcpy(planes+(chroma_first+l)*im->chroma_width, cb+l*im->chroma_width, im->chroma_width);
            memcpy(planes+chroma_height*im->chroma_width+(chroma_first+l)*im->chroma_width, cr+l*im->chroma_width,
                   im->chroma_width);
        }
    }
    if (!v->show)
        return true;
    for (unsigned l = 0; l < im->lines && first+l < im->height; ++l) {
        unsigned sy = first+l;
        if (sy%v->scale)
            continue;
        const uint8_t *yl = y+l*im->width, *cbl, *crl;
        unsigned cl = im->format == JPEG_420 ? l/2 : l;
        cbl = cb+cl*im->chroma_width;
        crl = cr+cl*im->chroma_width;
        for (unsigned ox = 0; ox < v->out_width; ++ox) {
            unsigned sx = ox*v->scale, cx = im->format == JPEG_444 ? sx : sx/2;
            rgb(line+2*ox, yl[sx], cbl[cx], crl[cx]);
        }
        display_pixels(v->x0, v->y0+sy/v->scale, v->out_width, 1, line);
    }
    return true;
}

static const char *format_name(unsigned f) {
    return f == JPEG_420 ? "4:2:0" : f == JPEG_422 ? "4:2:2" : "4:4:4";
}

static void wait_ms(unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
        hal_delay_us(1000);
        if (i%500 == 0)
            app_feed();
    }
}

/* "NNNN.NN" milliseconds from microseconds, for the screen. */
static void put_ms(char *out, unsigned micro) {
    unsigned ms = micro/1000, frac = micro%1000/10;
    char tmp[8];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0'+ms%10);
        ms /= 10;
    } while (ms);
    while (n)
        *out++ = tmp[--n];
    *out++ = '.';
    *out++ = (char)('0'+frac/10);
    *out++ = (char)('0'+frac%10);
    *out = 0;
}

int app_main(unsigned lsb_mhz) {
    timer_start();
    bool screen = display_init(APP_BOARD);
    app_log("jpeg test, LSB %u MHz, board %u, screen %s", lsb_mhz, APP_BOARD, screen ? "yes" : "no");
    app_log("registers before: con0 %x con1 %x con2 %x mcucnt %x ptr_num %x", (unsigned)jpeg_register(0),
            (unsigned)jpeg_register(1), (unsigned)jpeg_register(2), (unsigned)jpeg_register(12),
            (unsigned)jpeg_register(16));
    /* Do the registers hold what's written? */
    REG(0x41900u+4*3) = 0x1f0;
    REG(0x41900u+4*16) = 0x10;
    app_log("yuvline wrote 1f0 read %x, ptr_num wrote 10 read %x", (unsigned)jpeg_register(3),
            (unsigned)jpeg_register(16));

    unsigned hw_us[IMAGES];
    const char *result[IMAGES];
    int failures = 0;
    for (unsigned i = 0; i < IMAGES; ++i) {
        struct jpeg_image im;
        hw_us[i] = 0;
        result[i] = jpeg_parse(images[i].data, images[i].size, &im);
        if (result[i]) {
            app_log("%s: parse: %s", images[i].name, result[i]);
            ++failures;
            continue;
        }
        app_log("%s: %ux%u %s, %u bytes coded, work %u", images[i].name, im.width, im.height, format_name(im.format),
                im.entropy_size, jpeg_work_size(&im));
        struct view v = {.index = i, .scale = 1, .show = screen};
        while (im.width/v.scale > 240 || im.height/v.scale > 240)
            v.scale *= 2;
        v.out_width = im.width/v.scale;
        v.x0 = (240-v.out_width)/2;
        v.y0 = (240-im.height/v.scale)/2;
        if (screen)
            display_rect(0, 0, 240, 240, BLACK);
        struct jpeg_timing t;
        uint32_t started = ticks();
        result[i] = jpeg_decode(&im, work, row, &v, ticks, &t);
        uint32_t total = ticks()-started;
        app_log("%s: shown: %s, %u rows, setup %u us, decoder %u us, drawing %u us, all %u us", images[i].name,
                result[i] ? result[i] : "ok", (unsigned)t.rows, us(t.setup_ticks), us(t.hw_ticks),
                us(t.callback_ticks), us(total));
        if (result[i]) {
            app_log("  con0 %x con2 %x bcnt %x mcucnt %x baddr %x", (unsigned)jpeg_register(0),
                    (unsigned)jpeg_register(2), (unsigned)jpeg_register(11), (unsigned)jpeg_register(12),
                    (unsigned)jpeg_register(10));
            ++failures;
            continue;
        }
        if (screen) {
            wait_ms(SHOW_MS);
        }
        /* The decoder alone, without drawing. */
        uint32_t best = 0xffffffffu, sum = 0;
        for (unsigned k = 0; k < REPEATS; ++k) {
            uint32_t s = ticks();
            const char *e = jpeg_decode(&im, work, 0, 0, 0, 0);
            uint32_t d = ticks()-s;
            if (e) {
                app_log("%s: repeat %u: %s", images[i].name, k, e);
                result[i] = e;
                ++failures;
                break;
            }
            sum += d;
            if (d < best)
                best = d;
        }
        hw_us[i] = us(best);
        app_log("%s: decode only: best %u us, mean %u us of %u", images[i].name, us(best), us(sum/REPEATS), REPEATS);
        app_feed();
    }
    if (DUMP < IMAGES) {
        struct jpeg_image im;
        if (!jpeg_parse(images[DUMP].data, images[DUMP].size, &im))
            app_log("dump: %s at %x, %ux%u %s", images[DUMP].name, (unsigned)(uintptr_t)dump, im.width, im.height,
                    format_name(im.format));
    }
    if (screen) {
        for (unsigned r = 0; r < 15; ++r)
            display_text(r, WHITE, "");
        display_text(0, GREEN, "JPEG DECODER");
        display_text(1, WHITE, "DECODE ONLY, MS");
        for (unsigned i = 0; i < IMAGES; ++i) {
            char text[21];
            unsigned n = (unsigned)strlen(images[i].name);
            if (n > 13)
                n = 13;
            memcpy(text, images[i].name, n);
            text[n++] = ' ';
            if (result[i])
                memcpy(text+n, "FAIL", 5);
            else
                put_ms(text+n, hw_us[i]);
            display_text(3+i, result[i] ? RED : WHITE, text);
        }
        display_text(13, failures ? RED : GREEN, failures ? "SOME FAILED" : "ALL DECODED");
        wait_ms(8000);
    }
    return failures ? 1 : 0;
}
