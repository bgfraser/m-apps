/* SPDX-License-Identifier: MIT */
/* apps/jpeg's header parser on a computer (JPEG_PARSE_ONLY): the sample pictures parse, and malformed headers don't. */
#define JPEG_PARSE_ONLY
#include "../apps/jpeg/jpeg_hw.c"
#include "../apps/jpeg/images.h"
#include <stdio.h>
#include <stdlib.h>

static int failed;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failed = 1; } } while (0)

/* A copy of n bytes of p, so that a read past them is caught. */
static const char *parse(const uint8_t *p, unsigned n, struct jpeg_image *im) {
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, p, n);
    const char *e = jpeg_parse(copy, n, im);
    free(copy);
    return e;
}

int main(void) {
    static const struct { const uint8_t *data; unsigned size; } pictures[] = {
        {image0, sizeof image0}, {image1, sizeof image1}, {image2, sizeof image2}, {image3, sizeof image3},
        {image4, sizeof image4},
    };
    struct jpeg_image im;
    for (unsigned i = 0; i < sizeof pictures/sizeof pictures[0]; ++i) {
        const char *e = parse(pictures[i].data, pictures[i].size, &im);
        CHECK(!e);
        if (e)
            printf("picture %u: %s\n", i, e);
    }
    const uint8_t *p = image0;
    unsigned n = sizeof image0;
    /* The first marker without its FF. */
    uint8_t *bad = malloc(n);
    memcpy(bad, p, n);
    bad[2] = 0;
    CHECK(parse(bad, n, &im) != 0);
    /* Cut right after the scan's header: no coded data. */
    unsigned sos = 2;
    while (sos+3 < n && !(p[sos] == 0xff && p[sos+1] == 0xda))
        ++sos;
    unsigned header_end = sos+2+((unsigned)p[sos+2] << 8 | p[sos+3]);
    CHECK(!strcmp(parse(p, header_end, &im), "NO DATA"));
    /* A scan that isn't baseline's: spectral selection 1..63. */
    memcpy(bad, p, n);
    bad[header_end-3] = 1;
    CHECK(!strcmp(parse(bad, n, &im), "NOT BASELINE"));
    /* An end of image before any scan, a restart marker in the headers, the found inputs. */
    static const uint8_t eoi[] = {0xff, 0xd8, 0xff, 0xd9}, rst[] = {0xff, 0xd8, 0xff, 0xd0, 0, 4, 0, 0};
    static const uint8_t dri[] = {0xff, 0xd8, 0xff, 0xdd, 0x00, 0x02};
    CHECK(!strcmp(parse(eoi, sizeof eoi, &im), "NO SCAN"));
    CHECK(!strcmp(parse(rst, sizeof rst, &im), "BAD MARKER"));
    CHECK(!strcmp(parse(dri, sizeof dri, &im), "BAD SEGMENT"));
    free(bad);
    puts(failed ? "FAILED" : "jpeg parse OK");
    return failed;
}
