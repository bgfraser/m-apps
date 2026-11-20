/* SPDX-License-Identifier: MPL-2.0 */
/* All the keys, polled: the piano keys and every button, on the FM-1 and the SMK-37. board.h's board_keys() reads
 * only four; this reads the whole matrix. The tables come from felucca-bsp's boards/fm1/fm1_input.h and
 * boards/smk37/fm1_input.h (MPL-2.0): Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Charles
 * Vestal; Kerem Kilic (Ellic Studio). */
#ifndef KEYS_H
#define KEYS_H
#include <stdbool.h>
#include <stdint.h>

/* The 27 piano keys both boards have, F3 to G5: all of the FM-1's, and the SMK-37's keys 5 to 31 of C3..C6. */
#define KEYS_PIANO 27u

/* Buttons, as bits of buttons. PREV and NEXT move through a list: OCT- and OCT+ on the FM-1, which has no others,
 * and < and > on the SMK-37. PLAY is SEQ PLAY and REC is SEQ REC on the SMK-37. */
#define KEYS_PREV 0x1u
#define KEYS_NEXT 0x2u
#define KEYS_PLAY 0x4u
#define KEYS_REC 0x8u
#define KEYS_OCT_DOWN 0x10u
#define KEYS_OCT_UP 0x20u

struct keys {
    uint32_t piano;             /* bit k: piano key k held, 0 = F3 .. 26 = G5 */
    uint32_t piano_pressed;     /* went down since the last keys_scan() */
    uint32_t buttons;           /* KEYS_* held */
    uint32_t buttons_pressed;
};

/* After board_init(): the matrix's rows as inputs. board_restore() puts them back. */
void keys_init(bool smk);
/* One scan of the matrix, about 0.3 ms. Call it every few ms: a press counts after two scans closed, a release after
 * two open. */
void keys_scan(struct keys *k);
#endif
