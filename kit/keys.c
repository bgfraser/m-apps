/* SPDX-License-Identifier: MPL-2.0 */
/* The key matrix, polled. The pins, the words and the tables are felucca-bsp's (boards/fm1/fm1_input.h, Copyright (C)
 * 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, and Charles Vestal; boards/smk37/fm1_input.h, Copyright (C)
 * 2026 Kerem Kilic (Ellic Studio)); the shifting is m-bootloader's board.c's. Both MPL-2.0 here.
 *
 * FM-1: 11 columns x 6 rows behind two 74HC595s (PA4 data, PA3 clock, PA1 latch), the high byte first; a column is
 * one bit low. The rows are PA0, PA5..PA8 and PB7 with pull-ups, low = closed; the keys and buttons are on PA5..PA8.
 * SMK-37: 16 columns x 8 rows behind four 74HC595s on the same pins, the low byte first; column c is bit 24+c
 * (c < 8) or 8+c low, and the low 16 bits are the button LEDs, off when high. The rows are PA2, PA5, PA6, PA7, PA9,
 * PH6, PH8 and PH9. The cells of column p are read with column p-1's word latched, as the stock firmware scans. */
#include "keys.h"
#include "platform.h"

#define PA 0u
#define PH 7u
#define SETTLE_US 10u
#define FM_COLUMNS 11u
#define SMK_COLUMNS 16u
#define NONE 0xffu

static bool smk;

/* FM-1: the id at each column of rows PA5..PA8; 0..13 buttons, 14..40 the piano keys. */
static const uint8_t fm_map[4][FM_COLUMNS] = {
    {5, 11, 4, 10, 3, 9, 2, 8, NONE, NONE, NONE},
    {34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    {0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
};
static const uint8_t fm_rows[4] = {5, 6, 7, 8};

/* SMK-37: each key's second contact (it closes once the key is down), column << 4 | row, for keys 5..31, F3..G5. */
static const uint8_t smk_keys[KEYS_PIANO] = {
    0xf2, 0xe2, 0xc2, 0xa2, 0x82, 0x62, 0x42, 0x22, 0x12, 0x32,
    0x33, 0x43, 0x53, 0x63, 0x73, 0x83, 0x93, 0xa3, 0xb3, 0xc3, 0xd3, 0xe3, 0x23, 0x13, 0x14,
    0x24, 0x34};
/* SMK-37 buttons: OCTAVE-, OCTAVE+, SEQ PLAY, SEQ REC, <, >. */
static const uint8_t smk_buttons[6] = {0x66, 0x36, 0x16, 0x46, 0x96, 0x56};
static const uint8_t smk_rows[8][2] = {{PA, 2}, {PA, 5}, {PA, 6}, {PA, 7}, {PA, 9}, {PH, 6}, {PH, 8}, {PH, 9}};

static void shift(uint32_t word) {
    for (unsigned b = 0; b < (smk ? 4u:2u); ++b) {
        unsigned shift_by = smk ? b*8:(1-b)*8;
        uint8_t byte = (uint8_t)(word >> shift_by);
        for (unsigned bit = 8; bit; --bit) {
            gpio_level(PA, 4, !!(byte&(1u << (bit-1))));
            gpio_level(PA, 3, true);
            sync_cpu();
            gpio_level(PA, 3, false);
        }
    }
    gpio_level(PA, 1, true);
    sync_cpu();
    gpio_level(PA, 1, false);
}

void keys_init(bool board_smk) {
    smk = board_smk;
    if (smk)
        for (unsigned r = 0; r < 8; ++r)
            gpio_input(smk_rows[r][0], smk_rows[r][1], true, false);
    else
        for (unsigned r = 0; r < 4; ++r)
            gpio_input(PA, fm_rows[r], true, false);
}

/* What the matrix says now: piano keys and buttons closed. */
static void read_matrix(uint32_t *piano, uint32_t *buttons) {
    *piano = *buttons = 0;
    if (!smk) {
        for (unsigned p = 0; p < FM_COLUMNS; ++p) {
            shift(0xffffu ^ (1u << p) ^ (p < 2 ? 1u << (11+p):0u));
            hal_delay_us(SETTLE_US);
            for (unsigned r = 0; r < 4; ++r) {
                uint8_t id = fm_map[r][p];
                if (id == NONE || gpio_read(PA, fm_rows[r]))
                    continue;
                if (id >= 14)
                    *piano |= 1u << (id-14);
                else if (id == 0)
                    *buttons |= KEYS_PREV | KEYS_OCT_DOWN;
                else if (id == 1)
                    *buttons |= KEYS_NEXT | KEYS_OCT_UP;
                else if (id == 12)
                    *buttons |= KEYS_PLAY;
                else if (id == 13)
                    *buttons |= KEYS_REC;
            }
        }
        shift(0xffffu);
        return;
    }
    uint8_t cells[SMK_COLUMNS];
    for (unsigned p = 0; p < SMK_COLUMNS; ++p) {
        unsigned c = (p+SMK_COLUMNS-1)%SMK_COLUMNS;
        shift((0xffff0000u ^ (c < 8 ? 0x01000000u << c : 0x00010000u << (c-8))) | 0xffffu);
        hal_delay_us(SETTLE_US);
        uint8_t closed = 0;
        for (unsigned r = 0; r < 8; ++r)
            if (!gpio_read(smk_rows[r][0], smk_rows[r][1]))
                closed |= (uint8_t)(1u << r);
        cells[p] = closed;
    }
    shift(UINT32_MAX);
    for (unsigned k = 0; k < KEYS_PIANO; ++k)
        if (cells[smk_keys[k] >> 4] >> (smk_keys[k]&15)&1u)
            *piano |= 1u << k;
    static const uint32_t bits[6] = {KEYS_OCT_DOWN, KEYS_OCT_UP, KEYS_PLAY, KEYS_REC, KEYS_PREV, KEYS_NEXT};
    for (unsigned b = 0; b < 6; ++b)
        if (cells[smk_buttons[b] >> 4] >> (smk_buttons[b]&15)&1u)
            *buttons |= bits[b];
}

void keys_scan(struct keys *k) {
    static uint32_t last_piano, last_buttons;
    uint32_t piano, buttons;
    read_matrix(&piano, &buttons);
    /* Two scans the same, either way. */
    uint32_t piano_now = (k->piano & (piano | last_piano)) | (piano & last_piano);
    uint32_t buttons_now = (k->buttons & (buttons | last_buttons)) | (buttons & last_buttons);
    last_piano = piano;
    last_buttons = buttons;
    k->piano_pressed = piano_now & ~k->piano;
    k->buttons_pressed = buttons_now & ~k->buttons;
    k->piano = piano_now;
    k->buttons = buttons_now;
}
