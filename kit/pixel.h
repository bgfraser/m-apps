/* SPDX-License-Identifier: MIT */
/* The apps' 90s desktop look, shared: a 120 x 120 picture shown at twice that, brushed metal, ridged title bars, black
 * insets with green text, and two pixel fonts (pixel_font.py). Draw into pix_fb, mark what changed, and pix_send() sends it,
 * at most a budget at a time. */
#ifndef PIXEL_H
#define PIXEL_H
#include <stdbool.h>
#include <stdint.h>

#define PIX_W 120
#define PIX_H 120
#define PIX_RGB(r, g, b) ((uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | (b) >> 3))

/* The palette. */
#define SK_BODY PIX_RGB(0x3c, 0x43, 0x58)
#define SK_BODY_ALT PIX_RGB(0x37, 0x3e, 0x52)
#define SK_EDGE_LIGHT PIX_RGB(0x9a, 0xa4, 0xbe)
#define SK_EDGE_DARK PIX_RGB(0x12, 0x15, 0x1f)
#define SK_INSET_DARK PIX_RGB(0x0e, 0x10, 0x18)
#define SK_INSET_LIGHT PIX_RGB(0x7c, 0x86, 0xa0)
#define SK_TITLE_BG PIX_RGB(0x19, 0x1e, 0x2d)
#define SK_RIDGE_LIGHT PIX_RGB(0x5c, 0x68, 0x8a)
#define SK_RIDGE_DARK PIX_RGB(0x0d, 0x10, 0x18)
#define SK_GOLD PIX_RGB(0xf2, 0xc8, 0x5c)
#define SK_GOLD_SHADOW PIX_RGB(0x5a, 0x40, 0x10)
#define SK_LCD PIX_RGB(0x00, 0x00, 0x00)
#define SK_LCD_ON PIX_RGB(0x10, 0xf0, 0x40)
#define SK_LCD_OFF PIX_RGB(0x06, 0x1c, 0x0c)
#define SK_LABEL PIX_RGB(0xcc, 0xd2, 0xe2)
#define SK_BUTTON PIX_RGB(0x5a, 0x64, 0x80)
#define SK_BUTTON_LIGHT PIX_RGB(0xb0, 0xba, 0xd4)
#define SK_BUTTON_DARK PIX_RGB(0x18, 0x1c, 0x27)
#define SK_BUTTON_DOWN PIX_RGB(0x48, 0x50, 0x68)
#define SK_ICON PIX_RGB(0x10, 0x13, 0x1b)

extern uint16_t pix_fb[PIX_H][PIX_W];

void pix_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c);
void pix_pixel(int x, int y, uint16_t c);
/* A raised or sunken edge, one pixel wide. */
void pix_edge(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t top_left, uint16_t bottom_right);
/* Brushed metal. */
void pix_body(unsigned x, unsigned y, unsigned w, unsigned h);
/* 5 x 7 text, 6 apart, clipped to [clip_x0, clip_x1). Returns where the next character would go. */
int pix_big_text(int x, int y, const char *s, uint16_t c, int clip_x0, int clip_x1);
/* 3 x 5 text (M, N and W wider), and its width. */
unsigned pix_small_width(const char *s);
unsigned pix_small_text(unsigned x, unsigned y, const char *s, uint16_t c);
/* A title bar 7 high across the picture, with the title in gold between ridges, or ridges right across for "". */
void pix_title_bar(unsigned y, const char *title);
/* A sprite of w x h colours, skipping the transparent one. */
void pix_sprite(int x, int y, unsigned w, unsigned h, const uint16_t *data, uint16_t transparent);

/* Marks part of the picture to be sent. */
void pix_mark(unsigned x, unsigned y, unsigned w, unsigned h);
/* Sends what's marked, oldest first, up to the budget, if there is one. */
void pix_send(void);
/* Sends all that's marked, whatever the budget. */
void pix_send_all(void);
/* The most bytes one pix_send() sends, or 0 for all. What's left goes next time, so a big redraw doesn't hold up
 * anything that has to keep time. */
void pix_budget(unsigned bytes);
/* Called around sending, as the card shares the screen's bus, and between the pieces sent. */
void pix_hooks(void (*before)(void), void (*after)(void), void (*between)(void));
/* Forgets what was marked, as after drawing everything again. */
void pix_forget(void);
#endif
