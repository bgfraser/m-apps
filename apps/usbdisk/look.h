/* SPDX-License-Identifier: MIT */
/* What usbdisk shows, in the launcher's look: an SD card cabled to a laptop, with dots running along the cable while
 * data moves, and what's happening in a panel below. The same on both boards, but for the name of the key to leave. */
#ifndef LOOK_H
#define LOOK_H
#include <stdint.h>

enum { LOOK_STARTING, LOOK_NO_CARD, LOOK_WAITING, LOOK_CONNECTED, LOOK_EJECTED, LOOK_LEAVING };

struct look_model {
    unsigned state;
    uint32_t card_mib;
    unsigned read_kbs, write_kbs;       /* lately, in KB/s */
    uint32_t read_kib, written_kib;     /* since it started */
};

void look_start(unsigned board);
/* Redraws what changed, ms after the last call, and sends it, up to the budget. */
void look_update(const struct look_model *model, unsigned ms);
/* The most bytes one look_update() sends, or 0 for all; what's left goes next time. */
void look_budget(unsigned bytes);
/* Called around sending, as the card shares the screen's bus. */
void look_hooks(void (*before)(void), void (*after)(void));
#endif
