/* SPDX-License-Identifier: MIT */
/* libFuzzer: apps/jpeg's header parser on any bytes. It must not read outside them, and what it accepts must point
 * inside them. */
#define JPEG_PARSE_ONLY
#include "../../apps/jpeg/jpeg_hw.c"
#include <assert.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    /* A copy of exactly n bytes, so that AddressSanitizer sees a read past the end. */
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, data, n);
    struct jpeg_image im;
    if (!jpeg_parse(copy, (unsigned)n, &im)) {
        assert(im.entropy >= copy && im.entropy_size <= n && (size_t)(im.entropy-copy) <= n-im.entropy_size);
        assert(im.width && im.height && im.lines);
    }
    free(copy);
    return 0;
}
