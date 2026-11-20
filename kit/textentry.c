/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Adapted from fm1-radio's password page (app/app.c: pw_char, pw_layer, pw_key, ui_piano), drawn with the kit's
 * canvas, and with symbols a file name may have. */
#include "textentry.h"
#include "canvas.h"
#include "keys.h"

/* With OCT+ held: digits on F3..D4, then these on D#4..F#5. A file name can't have \ / : * ? " < > |. */
static const char symbols[] = " -_.,!&()+=#@$%'";

char text_char(unsigned key, unsigned layer) {
    if (key >= 26)
        return 0;
    if (layer == TEXT_SYMBOLS)
        return key < 10 ? (char)('0'+key) : symbols[key-10];
    return (char)((layer == TEXT_UPPER ? 'A' : 'a')+key);
}

unsigned text_layer(uint32_t buttons) {
    return buttons&KEYS_OCT_UP ? TEXT_SYMBOLS : buttons&KEYS_OCT_DOWN ? TEXT_UPPER : TEXT_LOWER;
}

int text_type(char *text, unsigned most, uint32_t pressed, unsigned layer) {
    unsigned n = 0;
    int changed = 0;
    while (text[n])
        ++n;
    for (unsigned k = 0; k < KEYS_PIANO; ++k) {
        if (!(pressed >> k&1u))
            continue;
        char c = text_char(k, layer);
        if (!c) {
            if (n)
                text[--n] = 0, changed = 1;
        } else if (n < most) {
            text[n++] = c;
            text[n] = 0;
            changed = 1;
        }
    }
    return changed;
}

/* F3..G5 as a little piano: 16 white keys and 11 black, each with what it types now. */
void text_piano(int y, unsigned layer, uint32_t held) {
    static const uint8_t black[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};   /* from F */
    const int kw = 14, x0 = 8, wh = TEXT_PIANO_H-2, bh = 27;
    canvas_clear(0, y, CANVAS_N, TEXT_PIANO_H);
    int wi = 0;
    for (unsigned k = 0; k < KEYS_PIANO; ++k)                /* white keys first, then the black ones over them */
        if (!black[k%12]) {
            int x = x0+wi*kw;
            bool lit = held >> k&1u;
            char t[2] = {text_char(k, layer), 0};
            if (!t[0])
                t[0] = '<';

            canvas_shape(x, y, kw-1, wh, 2, lit ? CANVAS_BLUE : CANVAS_WHITE, 16, 1);
            canvas_shape(x, y, kw-1, wh, 2, CANVAS_LINE, 6, 1);
            canvas_shape(x+1, y, kw-3, wh-1, 2, lit ? CANVAS_BLUE : CANVAS_WHITE, 16, 1);
            canvas_text(x+(kw-1-canvas_text_w(CANVAS_SMALL, t, 8))/2, y+wh-17, CANVAS_SMALL, t,
                        lit ? CANVAS_WHITE : CANVAS_INK, 8);
            ++wi;
        }
    wi = 0;
    for (unsigned k = 0; k < KEYS_PIANO; ++k) {
        if (!black[k%12]) {
            ++wi;
            continue;
        }
        int x = x0+wi*kw-6;
        bool lit = held >> k&1u;
        char t[2] = {text_char(k, layer), 0};
        canvas_shape(x, y, 12, bh, 2, lit ? CANVAS_BLUE : CANVAS_INK, 16, 1);
        canvas_text(x+(12-canvas_text_w(CANVAS_SMALL, t, 8))/2, y+5, CANVAS_SMALL, t, CANVAS_WHITE, 8);
    }
    canvas_mark(0, y, CANVAS_N, TEXT_PIANO_H);
}
