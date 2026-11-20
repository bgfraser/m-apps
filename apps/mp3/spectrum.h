/* SPDX-License-Identifier: MIT */
/* Levels for the analyser's bars, from a frame of samples: a 512-point FFT, Hann-windowed, in fixed point. */
#ifndef SPECTRUM_H
#define SPECTRUM_H
#include <stdint.h>

#define SPECTRUM_BANDS 14
#define SPECTRUM_TOP 14

/* pcm holds frames of 16-bit samples, interleaved if there are two channels. It uses the first 512 frames, and
 * treats missing ones as silence. levels gets SPECTRUM_BANDS values from 0 to SPECTRUM_TOP, low notes first. */
void spectrum_run(const int16_t *pcm, unsigned channels, unsigned frames, uint8_t *levels);
#endif
