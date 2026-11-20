/* SPDX-License-Identifier: MIT */
/* libFuzzer: apps/picker's QOI decoder (the launcher's icons) on any bytes. */
#include "qoi.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define MAX 44u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);
    /* Exactly the pixels an icon of MAX x MAX can have, so that a write past them is caught. */
    uint8_t *out = malloc(MAX*MAX*4);
    unsigned w = 0, h = 0;
    if (qoi_decode(copy, (uint32_t)n, out, MAX, &w, &h))
        assert(w && h && w <= MAX && h <= MAX);
    free(out);
    free(copy);
    return 0;
}
