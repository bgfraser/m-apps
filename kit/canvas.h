/* SPDX-License-Identifier: MIT */
/* The launcher's look for apps: a pale background, soft rounded shapes, Barlow text and the bar at the bottom, drawn
 * into a full 240 x 240 picture in RAM (115 KB). Draw, mark what changed, and canvas_send() sends it, at most a budget
 * at a time. It only draws into memory, so it also builds on a computer, with display_pixels() the computer's. */
#ifndef CANVAS_H
#define CANVAS_H
#include <stdbool.h>
#include <stdint.h>

#define CANVAS_N 240
#define CANVAS_RGB(r, g, b) ((uint16_t)(((r)&0xf8u) << 8 | ((g)&0xfcu) << 3 | (b) >> 3))
/* The launcher's colours. */
#define CANVAS_INK CANVAS_RGB(40, 52, 66)
#define CANVAS_DIM CANVAS_RGB(108, 120, 134)
#define CANVAS_FAINT CANVAS_RGB(150, 160, 172)
#define CANVAS_LINE CANVAS_RGB(196, 206, 218)
#define CANVAS_BLUE CANVAS_RGB(56, 136, 228)
#define CANVAS_WORDMARK CANVAS_RGB(70, 140, 215)
#define CANVAS_ORANGE CANVAS_RGB(236, 132, 40)
#define CANVAS_RED CANVAS_RGB(214, 72, 64)
#define CANVAS_GREEN CANVAS_RGB(52, 176, 104)
#define CANVAS_WHITE 0xffffu
#define CANVAS_SHADOW CANVAS_RGB(150, 166, 186)
#define CANVAS_BAR_Y 200

enum { CANVAS_SMALL, CANVAS_BIG };

extern uint16_t canvas_fb[CANVAS_N][CANVAS_N];

/* The background's colour at a pixel, and the background over a rectangle. */
uint16_t canvas_back(int x, int y);
void canvas_clear(int x, int y, int w, int h);
void canvas_fill(int x, int y, int w, int h, uint16_t c);
/* c at alpha a (out of 16) over what's there. */
void canvas_blend(int x, int y, uint16_t c, unsigned a);
/* A rounded rectangle in c at alpha a (out of 16), its edge soft pixels wide. */
void canvas_shape(int x, int y, int w, int h, int r, uint16_t c, unsigned a, int soft);
/* A white card with the launcher's soft shadow, and a blue ring round it if marked. */
void canvas_card(int x, int y, int w, int h, int r, bool marked);
/* RGB565 with 4-bit alpha, two pixels to a byte, the first in the high half. */
void canvas_sprite(int x, int y, int w, int h, const uint16_t *rgb, const uint8_t *alpha);
/* Text with the pen at x and the face's top at y, scale/8 its size. Returns the pen after it. */
int canvas_text(int x, int y, int font, const char *s, uint16_t c, int scale);
int canvas_text_w(int font, const char *s, int scale);
/* s cut to fit w at scale 8, ending in "...": a static copy if it had to be cut. */
const char *canvas_fit(int font, const char *s, int w);
/* The bar at the bottom, as the launcher's: the wordmark, then two lines on the right. */
void canvas_bar(const char *top, uint16_t top_colour, const char *bottom);

void canvas_mark(int x, int y, int w, int h);
/* Sends what's marked, oldest first, up to the budget. */
void canvas_send(void);
void canvas_send_all(void);
void canvas_budget(unsigned bytes);
/* Called around sending, as the card shares the screen's bus, and between the pieces sent. */
void canvas_hooks(void (*before)(void), void (*after)(void), void (*between)(void));
/* Forgets what's marked, as when all of it is drawn again. */
void canvas_forget(void);
#endif
