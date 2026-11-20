/* SPDX-License-Identifier: MIT */
/* SHA-1 blocks on the chip's SHA unit (JL_SHA at 0x11700), as JieLi's AC79 SDK drives it (lib/net/mbedtls_2_26_0/todo/
 * algorithm_alt.c, mbedtls_sha1_process, Apache-2.0): the block read by DMA from 32-byte aligned RAM, the state in
 * RESULT7..RESULT3, CON bit 3 SHA-1, bit 1 a block from that state, bit 15 done, bit 14 to clear it. A WPA2 key is
 * 4096 rounds of HMAC-SHA-1, about 33 000 blocks, so this is most of a join's time. It isn't trusted until
 * sha1_hw_check() has seen it give what the software does. */
#include "sha1hw.h"
#include "app.h"
#include <string.h>
#define SHA_CON REG(0x11700u)
#define SHA_ADR REG(0x11704u)
#define SHA_RESULT(n) REG(0x11708u+4u*(n))
#define WAIT 200000u

void wc_sha1_block_sw(uint32_t h[5], const uint8_t *p);

/* In the low RAM other DMA reaches (kit/app.ld.S), aligned as the unit wants. */
static uint8_t block[64] __attribute__((aligned(32), section(".dma")));
static enum { UNTRIED, WORKS, FAILS } state;

static bool run(uint32_t h[5], const uint8_t *p) {
    memcpy(block, p, 64);
    for (unsigned i = 0; i < 5; ++i)
        SHA_RESULT(7-i) = h[i];
    SHA_RESULT(2) = 0;
    SHA_RESULT(1) = 0;
    SHA_RESULT(0) = 0;
    SHA_ADR = (uint32_t)(uintptr_t)block;
    sync_cpu();
    SHA_CON |= BIT(14);
    SHA_CON |= BIT(3);
    SHA_CON |= BIT(1);
    unsigned n = WAIT;
    while (!(SHA_CON&BIT(15)))
        if (!--n)
            return false;
    SHA_CON |= BIT(14);
    for (unsigned i = 0; i < 5; ++i)
        h[i] = SHA_RESULT(7-i);
    return true;
}

int sha1_hw_block(uint32_t h[5], const uint8_t *p) {
    return state == WORKS && run(h, p);
}

bool sha1_hw_check(void) {
    if (state != UNTRIED)
        return state == WORKS;
    static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t data[64];
    uint32_t seed = 0x2545F491u, hw[5], sw[5];
    bool same = true;
    /* From the start, and from a state of no hash's. */
    for (unsigned t = 0; t < 4 && same; ++t) {
        for (unsigned i = 0; i < 64; ++i) {
            seed = seed*1664525u+1013904223u;
            data[i] = (uint8_t)(seed >> 24);
        }
        for (unsigned i = 0; i < 5; ++i) {
            seed = seed*1664525u+1013904223u;
            hw[i] = sw[i] = t == 0 ? iv[i] : seed;
        }
        wc_sha1_block_sw(sw, data);
        same = run(hw, data) && !memcmp(hw, sw, sizeof hw);
    }
    state = same ? WORKS : FAILS;
    if (same) {
        /* How much faster: 256 blocks each way, on the 24 MHz timer. */
        uint32_t t0 = REG(0x10804u);
        for (unsigned i = 0; i < 256; ++i)
            run(hw, data);
        uint32_t t1 = REG(0x10804u);
        for (unsigned i = 0; i < 256; ++i)
            wc_sha1_block_sw(sw, data);
        uint32_t t2 = REG(0x10804u);
        app_log("SHA-1 unit works: 256 blocks in %u us, %u us in software", (unsigned)((t1-t0)/24u),
                (unsigned)((t2-t1)/24u));
    } else {
        app_log("SHA-1 unit gave something else, or didn't finish: software it is");
    }
    return same;
}
