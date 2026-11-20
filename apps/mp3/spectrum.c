/* SPDX-License-Identifier: MIT */
#include "spectrum.h"
#include "spectrum_tables.h"

#define LOG2_N 9
/* Each band's first bin; at 44.1 kHz a bin is 86 Hz. About two thirds of an octave each. */
static const uint16_t edges[SPECTRUM_BANDS+1] = {1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 37, 50, 68, 100, 256};
/* A full-scale sine comes out near 96, in eighths of an octave (0.75 dB). Each step of the bars is 5 of them. */
#define FULL_SCALE 96
#define PER_STEP 5

static int32_t re[SPECTRUM_N], im[SPECTRUM_N];

static unsigned reversed(unsigned n) {
    unsigned r = 0;
    for (unsigned i = 0; i < LOG2_N; ++i) {
        r = r << 1 | (n&1);
        n >>= 1;
    }
    return r;
}

/* log2 in eighths: the top bit's place, and the three bits after it. */
static int eighths(uint32_t v) {
    if (!v)
        return 0;
    int top = 31;
    while (!(v&(1u << top)))
        --top;
    unsigned frac = top >= 3 ? (v >> (top-3))&7u : (v << (3-top))&7u;
    return top*8+(int)frac;
}

void spectrum_run(const int16_t *pcm, unsigned channels, unsigned frames, uint8_t *levels) {
    for (unsigned n = 0; n < SPECTRUM_N; ++n) {
        int32_t s = 0;
        if (n < frames)
            s = channels == 2 ? ((int32_t)pcm[2*n]+pcm[2*n+1]) >> 1 : pcm[n];
        int32_t w = hann[n < SPECTRUM_N/2 ? n : SPECTRUM_N-1-n];
        unsigned r = reversed(n);
        /* Halved, so that sums of products stay inside 32 bits. */
        re[r] = (s*w) >> 16;
        im[r] = 0;
    }
    for (unsigned len = 2; len <= SPECTRUM_N; len <<= 1) {
        unsigned half = len/2, step = SPECTRUM_N/len;
        for (unsigned i = 0; i < SPECTRUM_N; i += len) {
            for (unsigned j = 0; j < half; ++j) {
                int32_t c = fft_cos[j*step], s = fft_sin[j*step];
                int32_t *ar = &re[i+j], *ai = &im[i+j], *br = &re[i+j+half], *bi = &im[i+j+half];
                int32_t tr = (*br*c+*bi*s) >> 15, ti = (*bi*c-*br*s) >> 15;
                int32_t ur = *ar, ui = *ai;
                *ar = (ur+tr) >> 1;
                *ai = (ui+ti) >> 1;
                *br = (ur-tr) >> 1;
                *bi = (ui-ti) >> 1;
            }
        }
    }
    for (unsigned b = 0; b < SPECTRUM_BANDS; ++b) {
        uint32_t most = 0;
        for (unsigned k = edges[b]; k < edges[b+1]; ++k) {
            uint32_t x = (uint32_t)(re[k] < 0 ? -re[k] : re[k]), y = (uint32_t)(im[k] < 0 ? -im[k] : im[k]);
            uint32_t m = x > y ? x+y/2 : y+x/2;
            if (m > most)
                most = m;
        }
        int level = (eighths(most)-(FULL_SCALE-SPECTRUM_TOP*PER_STEP))/PER_STEP;
        levels[b] = (uint8_t)(level < 0 ? 0 : level > SPECTRUM_TOP ? SPECTRUM_TOP : level);
    }
}
