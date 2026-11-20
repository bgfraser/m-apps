/* SPDX-License-Identifier: MIT */
/* The AC79's JPEG decoder, polled: baseline JPEG with three components, 4:2:0, 4:2:2 or 4:4:4. It decodes a row of
 * MCUs at a time into planar YCbCr, and hands each row to a callback before starting the next. */
#ifndef JPEG_HW_H
#define JPEG_HW_H
#include <stdbool.h>
#include <stdint.h>

#define JPEG_444 1
#define JPEG_422 2
#define JPEG_420 3

struct jpeg_image {
    unsigned width, height, format;
    unsigned lines;              /* lines in a row of MCUs: 16 for 4:2:0, otherwise 8 */
    unsigned chroma_width;       /* width of the Cb and Cr planes */
    unsigned chroma_lines;       /* lines of Cb and Cr in a row of MCUs */
    unsigned restart;            /* restart interval, in MCUs, or 0 */
    const uint8_t *entropy;      /* the scan's coded data */
    unsigned entropy_size;
};

/* A decoded row of MCUs: lines of y (stride width), and chroma_lines of cb and cr (stride chroma_width). The last
 * row may hold fewer real lines than the buffer: first_line + lines can pass the height. Return false to stop. */
typedef bool (*jpeg_row)(void *priv, const struct jpeg_image *image, unsigned first_line,
                         const uint8_t *y, const uint8_t *cb, const uint8_t *cr);

struct jpeg_timing {
    uint32_t setup_ticks, hw_ticks, callback_ticks, rows;
};

/* Reads the headers. Returns 0, or what's wrong. */
const char *jpeg_parse(const uint8_t *data, unsigned size, struct jpeg_image *image);
/* Bytes the row buffers need. */
unsigned jpeg_work_size(const struct jpeg_image *image);
/* Decodes a parsed image. work must be 4-byte aligned and jpeg_work_size() long. ticks_now() is for timing, and may
 * be 0. Returns 0, or what went wrong. */
const char *jpeg_decode(const struct jpeg_image *image, uint8_t *work, jpeg_row row, void *priv,
                        uint32_t (*ticks_now)(void), struct jpeg_timing *timing);
/* The decoder's registers, for the log. */
uint32_t jpeg_register(unsigned index);
#endif
