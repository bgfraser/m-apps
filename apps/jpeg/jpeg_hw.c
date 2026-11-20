/* SPDX-License-Identifier: MIT */
/* The AC79's JPEG decoder (JL_JPG at 0x41900, IRQ 55), polled. The register sequence was worked out from how JieLi's
 * AC79 SDK drives it; this is written fresh, and handles the tables its own way.
 *
 * The decoder takes:
 *  - the Huffman tables as 258 16-bit words at table address 0: DC0, AC0, DC1, AC1, each as the smallest code of each
 *    length (12 lengths for DC, 16 for AC), then the index of its first symbol per length, then the symbols. Luma
 *    uses table 0, chroma table 1.
 *  - the quantisation tables at table address 0x180: 64 words for luma, then 64 for chroma, in natural order, each
 *    multiplied by the AAN scale factor and shifted right 6.
 *  - YPTR, UPTR and VPTR for a row of MCUs, BADDR and BCNT for the coded data, MCUCNT for the MCUs in a row, PTR_NUM
 *    for the lines in that row and YUVLINE for the width.
 * CON0: bits 0-1 on, bits 4-5 the format (0 4:4:4, 1 4:2:2, 3 4:2:0), bit 7 starts a row, bit 15 says a row is done
 * and bit 14 clears it, bit 13 says the coded data ran out and bit 12 clears it. Bits 2-3 would turn on its
 * interrupts, which we leave off. */
#include "jpeg_hw.h"
#include <string.h>
/* JPEG_PARSE_ONLY: the headers' parser alone, which builds on a computer too (tests/fuzz/jpeg_fuzz.c). */
#ifndef JPEG_PARSE_ONLY
#include "platform.h"

#define JPG ((volatile uint32_t *)0x41900u)
enum { CON0, CON1, CON2, YUVLINE, YDCVAL, UDCVAL, VDCVAL, YPTR, UPTR, VPTR, BADDR, BCNT, MCUCNT, PRECNT, HQTBLADR,
       HQTBLDAT, PTR_NUM, HAS_PTR, MCU_BUF, REGISTERS };

#define CON0_ON 3u
#define CON0_START BIT(7)
#define CON0_CLEAR_DATA BIT(12)
#define CON0_DATA_OUT BIT(13)
#define CON0_CLEAR_ROW BIT(14)
#define CON0_ROW_DONE BIT(15)
/* Set by JieLi's driver with the clears before each row; their meaning isn't known. */
#define CON0_ROW_SETUP (BIT(6) | BIT(10))
#define CON2_ROW BIT(3)
/* About 2 s at 96 MHz, longer at 360. */
#define ROW_TIMEOUT_POLLS 20000000u

static inline void sync(void) {
    __asm__ volatile("csync" ::: "memory");
}

uint32_t jpeg_register(unsigned index) {
    return index < REGISTERS ? JPG[index] : 0;
}
#endif

/* What the headers held. Baseline allows tables 0 and 1 of each kind; 2 and 3 are refused. */
static struct {
    uint8_t q[2][64];            /* zigzag order, as in the file */
    bool q_seen[2];
    uint8_t counts[2][2][16];    /* [ac][id] */
    uint8_t symbols[2][2][162];
    bool h_seen[2][2];
    uint8_t component_q[3];
    uint8_t component_dc[3], component_ac[3];
} t;

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63};

/* round(8192 * a(r) * a(c)), with a(0) = 1 and a(k) = sqrt(2) cos(k pi / 16): the AAN IDCT's scale factors. */
static const uint16_t aan[64] = {
    8192, 11363, 10703, 9633, 8192, 6436, 4433, 2260, 11363, 15760, 14846, 13361, 11363, 8928, 6149, 3135,
    10703, 14846, 13985, 12586, 10703, 8410, 5793, 2953, 9633, 13361, 12586, 11327, 9633, 7568, 5213, 2658,
    8192, 11363, 10703, 9633, 8192, 6436, 4433, 2260, 6436, 8928, 8410, 7568, 6436, 5057, 3483, 1776,
    4433, 6149, 5793, 5213, 4433, 3483, 2399, 1223, 2260, 3135, 2953, 2658, 2260, 1776, 1223, 624};

static unsigned be16(const uint8_t *p) {
    return (unsigned)p[0] << 8 | p[1];
}

const char *jpeg_parse(const uint8_t *data, unsigned size, struct jpeg_image *image) {
    memset(&t, 0, sizeof t);
    memset(image, 0, sizeof *image);
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return "NOT A JPEG";
    const uint8_t *p = data+2, *end = data+size;
    bool frame = false;
    unsigned samp = 0;
    for (;;) {
        while (p < end && *p == 0xff)
            ++p;
        if (end-p < 3)
            return "CUT SHORT";
        unsigned marker = *p++;
        unsigned length = be16(p);
        if (length < 2 || length > (unsigned)(end-p))
            return "BAD SEGMENT";
        const uint8_t *s = p+2, *s_end = p+length;
        p = s_end;
        if (marker == 0xc0 || marker == 0xc1) {
            if (length < 17 || s[0] != 8)
                return "NOT 8 BIT";
            if (s[5] != 3)
                return s[5] == 1 ? "GREYSCALE" : "NOT 3 COMPONENTS";
            image->height = be16(s+1);
            image->width = be16(s+3);
            for (unsigned c = 0; c < 3; ++c) {
                const uint8_t *k = s+6+3*c;
                if (k[0] != c+1)
                    return "COMPONENT IDS";
                if (k[2] > 1)
                    return "QUANT TABLE ID";
                t.component_q[c] = k[2];
                if (c && k[1] != 0x11)
                    return "CHROMA SAMPLING";
            }
            samp = s[7];
            frame = true;
        } else if (marker >= 0xc2 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            return marker == 0xc2 ? "PROGRESSIVE" : "NOT BASELINE";
        } else if (marker == 0xdb) {
            while (s < s_end) {
                unsigned id = s[0]&15;
                if (s[0] >> 4)
                    return "16 BIT QUANT";
                if (id > 1 || s_end-s < 65)
                    return "BAD QUANT TABLE";
                memcpy(t.q[id], s+1, 64);
                t.q_seen[id] = true;
                s += 65;
            }
        } else if (marker == 0xc4) {
            while (s < s_end) {
                unsigned ac = s[0] >> 4, id = s[0]&15, total = 0;
                if (ac > 1 || id > 1 || s_end-s < 17)
                    return "BAD HUFFMAN TABLE";
                for (unsigned i = 0; i < 16; ++i)
                    total += s[1+i];
                if (total > (ac ? 162u : 12u) || (unsigned)(s_end-s) < 17+total)
                    return "BAD HUFFMAN TABLE";
                if (!ac)
                    for (unsigned i = 12; i < 16; ++i)
                        if (s[1+i])
                            return "LONG DC CODES";
                memcpy(t.counts[ac][id], s+1, 16);
                memcpy(t.symbols[ac][id], s+17, total);
                t.h_seen[ac][id] = true;
                s += 17+total;
            }
        } else if (marker == 0xdd) {
            if (length < 4)
                return "BAD SEGMENT";
            image->restart = be16(s);
        } else if (marker == 0xda) {
            if (!frame)
                return "NO FRAME";
            /* The component count, three components of two bytes, and three bytes of spectral selection. */
            if (length < 2+1+6+3)
                return "BAD SEGMENT";
            if (s[0] != 3)
                return "NOT ONE SCAN";
            for (unsigned c = 0; c < 3; ++c) {
                if (s[1+2*c] != c+1)
                    return "SCAN COMPONENTS";
                t.component_dc[c] = s[2+2*c] >> 4;
                t.component_ac[c] = s[2+2*c]&15;
                if (t.component_dc[c] > 1 || t.component_ac[c] > 1)
                    return "HUFFMAN TABLE ID";
            }
            image->entropy = s_end;
            image->entropy_size = (unsigned)(end-s_end);
            break;
        } else if (marker == 0xd9) {
            return "NO SCAN";
        }
    }
    if (t.component_q[1] != t.component_q[2] || t.component_dc[1] != t.component_dc[2] ||
        t.component_ac[1] != t.component_ac[2])
        return "CB AND CR DIFFER";
    for (unsigned c = 0; c < 3; ++c)
        if (!t.q_seen[t.component_q[c]] || !t.h_seen[0][t.component_dc[c]] || !t.h_seen[1][t.component_ac[c]])
            return "MISSING TABLE";
    if (!image->width || !image->height)
        return "NO SIZE";
    if (samp == 0x11) {
        image->format = JPEG_444;
        image->lines = 8;
        image->chroma_width = image->width;
        image->chroma_lines = 8;
    } else if (samp == 0x21) {
        image->format = JPEG_422;
        image->lines = 8;
        image->chroma_width = image->width/2;
        image->chroma_lines = 8;
    } else if (samp == 0x22) {
        image->format = JPEG_420;
        image->lines = 16;
        image->chroma_width = image->width/2;
        image->chroma_lines = 8;
    } else {
        return "LUMA SAMPLING";
    }
    unsigned mcu_width = image->format == JPEG_444 ? 8 : 16;
    if (image->width%mcu_width)
        return image->format == JPEG_444 ? "WIDTH NOT x8" : "WIDTH NOT x16";
    if (image->restart && image->restart%(image->width/mcu_width))
        return "RESTART NOT PER ROW";
    if (image->restart)
        return "RESTART MARKERS";     /* not handled yet */
    return 0;
}

unsigned jpeg_work_size(const struct jpeg_image *image) {
    return image->width*image->lines + 2*image->chroma_width*image->chroma_lines;
}

/* One table in the decoder's layout: the smallest code of each length, the index of its first symbol, then the
 * symbols. The code is shifted on only while it fits, which is how JieLi's driver builds it. */
#ifndef JPEG_PARSE_ONLY
static void huffman(uint8_t *out, const uint8_t *counts, const uint8_t *symbols, bool ac) {
    unsigned n = ac ? 16 : 12, code = 0, index = 0;
    memset(out, 0, ac ? 210 : 48);
    for (unsigned i = 1; i <= n; ++i) {
        out[2*(i-1)] = (uint8_t)code;
        out[2*(i-1)+1] = (uint8_t)(code >> 8);
        out[2*n+i-1] = (uint8_t)index;
        unsigned count = counts[i-1];
        if (!count) {
            code <<= 1;
        } else {
            code += count;
            if (!(code&(1u << (i+1))))
                code <<= 1;
            index += count;
        }
    }
    memcpy(out+3*n, symbols, index);
}

static void load_tables(void) {
    static uint8_t h[516] __attribute__((aligned(4)));
    huffman(h, t.counts[0][t.component_dc[0]], t.symbols[0][t.component_dc[0]], false);
    huffman(h+48, t.counts[1][t.component_ac[0]], t.symbols[1][t.component_ac[0]], true);
    huffman(h+258, t.counts[0][t.component_dc[1]], t.symbols[0][t.component_dc[1]], false);
    huffman(h+306, t.counts[1][t.component_ac[1]], t.symbols[1][t.component_ac[1]], true);
    JPG[HQTBLADR] = 0;
    sync();
    for (unsigned i = 0; i < 258; ++i)
        JPG[HQTBLDAT] = (uint32_t)h[2*i] | (uint32_t)h[2*i+1] << 8;
    JPG[HQTBLADR] = 0x180;
    sync();
    for (unsigned table = 0; table < 2; ++table) {
        const uint8_t *q = t.q[t.component_q[table]];
        int16_t natural[64];
        for (unsigned i = 0; i < 64; ++i)
            natural[zigzag[i]] = (int16_t)(((uint32_t)q[i]*aan[zigzag[i]]) >> 6);
        for (unsigned i = 0; i < 64; ++i)
            JPG[HQTBLDAT] = (uint32_t)(int32_t)natural[i];
        sync();
    }
}

static void point(uint8_t *y, uint8_t *cb, uint8_t *cr, unsigned mcus) {
    JPG[YPTR] = (uint32_t)(uintptr_t)y;
    JPG[UPTR] = (uint32_t)(uintptr_t)cb;
    JPG[VPTR] = (uint32_t)(uintptr_t)cr;
    JPG[MCUCNT] = mcus;
}

const char *jpeg_decode(const struct jpeg_image *image, uint8_t *work, jpeg_row row, void *priv,
                        uint32_t (*ticks_now)(void), struct jpeg_timing *timing) {
    struct jpeg_timing unused;
    if (!timing)
        timing = &unused;
    memset(timing, 0, sizeof *timing);
    uint32_t t0 = ticks_now ? ticks_now() : 0;
    uint8_t *y = work, *cb = y+image->width*image->lines, *cr = cb+image->chroma_width*image->chroma_lines;
    unsigned mcus = image->width/(image->format == JPEG_444 ? 8 : 16);
    unsigned rows = (image->height+image->lines-1)/image->lines;
    unsigned format = image->format == JPEG_444 ? 0 : image->format == JPEG_422 ? 1 : 3;

    JPG[CON0] = 0;
    JPG[CON1] = 0;
    JPG[CON2] = 0;
    sync();
    point(y, cb, cr, mcus);
    load_tables();
    JPG[CON0] |= CON0_ON;
    JPG[YDCVAL] = 0;
    JPG[UDCVAL] = 0;
    JPG[VDCVAL] = 0;
    sync();
    JPG[BADDR] = (uint32_t)(uintptr_t)image->entropy;
    JPG[BCNT] = image->entropy_size;
    JPG[PTR_NUM] = image->lines;
    JPG[MCUCNT] = mcus;
    JPG[CON0] |= CON0_ROW_SETUP | CON0_CLEAR_DATA | CON0_CLEAR_ROW;
    JPG[CON0] |= format << 4;
    sync();
    JPG[YUVLINE] = image->width;
    JPG[CON2] |= CON2_ROW;
    sync();
    if (ticks_now)
        timing->setup_ticks = ticks_now()-t0;
    JPG[CON0] |= CON0_START;

    const char *error = 0;
    for (unsigned r = 0; r < rows && !error; ++r) {
        uint32_t started = ticks_now ? ticks_now() : 0;
        unsigned waited = 0;
        for (;;) {
            uint32_t con = JPG[CON0];
            if (con&CON0_DATA_OUT)
                JPG[CON0] |= CON0_CLEAR_DATA;
            if (con&CON0_ROW_DONE) {
                JPG[CON0] |= CON0_CLEAR_ROW;
                break;
            }
            if (++waited > ROW_TIMEOUT_POLLS) {
                error = "DECODER TIMED OUT";
                break;
            }
        }
        if (error)
            break;
        uint32_t done = ticks_now ? ticks_now() : 0;
        timing->hw_ticks += done-started;
        ++timing->rows;
        if (row && !row(priv, image, r*image->lines, y, cb, cr))
            error = "STOPPED";
        if (ticks_now)
            timing->callback_ticks += ticks_now()-done;
        if (error || r+1 == rows)
            break;
        point(y, cb, cr, mcus);
        JPG[CON0] |= CON0_CLEAR_DATA;
        JPG[CON0] |= BIT(10);
        JPG[CON0] |= CON0_CLEAR_ROW;
        JPG[CON2] |= CON2_ROW;
        sync();
        JPG[CON0] |= CON0_START;
    }
    JPG[CON0] = 0;
    JPG[BCNT] = 0;
    return error;
}
#endif
