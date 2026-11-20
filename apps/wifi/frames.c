/* SPDX-License-Identifier: MPL-2.0 */
/* What the radio hears, read: no hardware here, so it builds on a computer too, for its fuzz test
 * (tests/fuzz/frames_fuzz.c). Moved from net.c (MPL-2.0). */
#include "frames.h"
#include "net.h"
#include <string.h>

/* A beacon or probe response: its BSSID at 16, fixed fields to 36 (capabilities at 34), then the elements. */
bool frame_beacon(const uint8_t *m, uint32_t len, unsigned heard_on, struct frame_heard *h) {
    if (len < 36 || (m[0] != 0x80 && m[0] != 0x50))
        return false;
    memset(h, 0, sizeof *h);
    h->bssid = m+16;
    h->channel = heard_on;
    bool privacy = (m[34]&0x10) != 0, rsn = false, wpa2 = false;
    for (uint32_t at = 36; at+2 <= len; ) {
        unsigned id = m[at], l = m[at+1];
        const uint8_t *b = m+at+2;
        if (at+2+l > len)
            break;
        if (id == 0 && l <= 32) {
            h->ssid = b;
            h->ssid_len = l;
        } else if (id == 3 && l == 1) {
            h->channel = b[0];
        } else if (id == 48 && l >= 2) {
            /* RSN: version 1, group CCMP, a CCMP pairwise and a PSK key management among those offered, PMF not
             * required (capabilities bit 6). */
            static const uint8_t ccmp[4] = {0x00, 0x0f, 0xac, 0x04}, psk[4] = {0x00, 0x0f, 0xac, 0x02};
            rsn = true;
            if (l >= 8 && b[0] == 1 && b[1] == 0 && !memcmp(b+2, ccmp, 4)) {
                unsigned p = 6, pairs = b[p] | b[p+1] << 8, keys;
                bool pair_ok = false, key_ok = false;
                p += 2;
                for (unsigned i = 0; i < pairs && p+4 <= l; ++i, p += 4)
                    pair_ok |= !memcmp(b+p, ccmp, 4);
                if (p+2 <= l) {
                    keys = b[p] | b[p+1] << 8;
                    p += 2;
                    for (unsigned i = 0; i < keys && p+4 <= l; ++i, p += 4)
                        key_ok |= !memcmp(b+p, psk, 4);
                }
                bool pmf_required = p+2 <= l && (b[p]&0x40);
                wpa2 = pair_ok && key_ok && !pmf_required;
            }
        }
        at += 2+l;
    }
    h->security = wpa2 ? NET_WPA2 : !privacy && !rsn ? NET_OPEN : NET_OTHER;
    /* A channel said that can't be one: the one it was heard on. */
    if (h->channel < 1 || h->channel > 13)
        h->channel = heard_on;
    return true;
}

bool frame_hidden(const uint8_t *ssid, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        if (ssid[i])
            return false;
    return true;
}

bool frame_lengths_fit(const uint8_t *type_and_packet, uint32_t n) {
    const uint8_t *p = type_and_packet+2;
    if (type_and_packet[0] != 0x08 || type_and_packet[1] != 0x00)
        return true;
    n -= 2;
    if (n < 20u || p[0] >> 4 != 4u)
        return false;
    uint32_t header = (p[0]&15u)*4u, total = (uint32_t)p[2] << 8 | p[3];
    if (header < 20u || total < header || total > n)
        return false;
    if (p[9] != 17u)
        return true;
    if (total-header < 8u)
        return false;
    uint32_t udp = (uint32_t)p[header+4] << 8 | p[header+5];
    return udp >= 8u && udp <= total-header;
}
