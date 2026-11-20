/* SPDX-License-Identifier: MIT */
/* The player's look: a 90s desktop player, the main window over its playlist, drawn at 120 x 120 and shown at twice
 * that. It keeps what it last drew, and only redraws and sends what changed. */
#ifndef UI_H
#define UI_H
#include <stdbool.h>
#include <stdint.h>
#include "spectrum.h"

#define UI_STOPPED 0
#define UI_PLAYING 1
#define UI_PAUSED 2

struct ui_model {
    unsigned mode;
    unsigned elapsed, total;            /* seconds */
    unsigned kbps, hz, channels;        /* 0 until a file says */
    unsigned count;                     /* tracks; the playlist's last row, count, is "back" */
    unsigned selected;
    int playing;                        /* the track playing or paused, or -1 */
    /* A track's name for the screen, without .MP3. It may use one buffer for every call. */
    const char *(*name)(unsigned track);
    const char *note;                   /* shown instead of the list, and in the title, if set */
    unsigned keys;                      /* held, as board_keys() says */
    const uint8_t *levels;              /* SPECTRUM_BANDS bars, or 0 for none */
};

/* Draws all of it and sends it. board is APP_BOARD, for the key names. */
void ui_start(unsigned board);
/* Redraws what changed, ms after the last call (for scrolling, blinking and the bars falling), and sends it. */
void ui_update(const struct ui_model *model, unsigned ms);
/* What it sends at a time, and what's called around it, are pix_budget() and pix_hooks() in kit/pixel.h. */
#endif
