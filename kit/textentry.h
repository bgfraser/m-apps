/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Typing on the piano keys, as fm1-radio types a WiFi password (app/app.c): the 27 keys F3..G5 type a..z, and G5
 * deletes. Holding OCT- types capitals, and holding OCT+ digits (F3..D4) and symbols (D#4..F#5). The symbols here are
 * those a file name may have; with text_all_symbols() on, as for a password, holding both types the rest (F3..A4). The keyboard is drawn as a little piano, each key with what it types now. */
#ifndef TEXTENTRY_H
#define TEXTENTRY_H
#include <stdint.h>

enum { TEXT_LOWER, TEXT_UPPER, TEXT_SYMBOLS, TEXT_MORE };

/* Every printable ASCII character, or only those a file name may have (the default). */
void text_all_symbols(int on);

/* What piano key k (0 = F3 .. 26 = G5) types in a layer, 0 for delete, or TEXT_NOTHING. */
#define TEXT_NOTHING '\1'
char text_char(unsigned key, unsigned layer);
/* The layer the held buttons (KEYS_* from keys.h) choose. */
unsigned text_layer(uint32_t buttons);
/* Applies the piano keys pressed to text, which holds room for most characters and a zero. Returns whether it changed. */
int text_type(char *text, unsigned most, uint32_t pressed, unsigned layer);
/* The piano at y on the canvas, 240 wide and TEXT_PIANO_H high, with held keys lit. */
#define TEXT_PIANO_H 46
void text_piano(int y, unsigned layer, uint32_t held);
#endif
