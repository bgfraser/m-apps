/* SPDX-License-Identifier: MIT */
#ifndef SHA1HW_H
#define SHA1HW_H
#include <stdbool.h>
#include <stdint.h>
/* Whether the chip's SHA unit gives what the software does: checked once, and then wifi_crypto.c uses it. */
bool sha1_hw_check(void);
/* wifi_crypto.c's hook (WC_SHA1_HW): 1 if it did the block. */
int sha1_hw_block(uint32_t h[5], const uint8_t *p);
#endif
