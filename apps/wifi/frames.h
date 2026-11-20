/* SPDX-License-Identifier: MPL-2.0 */
/* What the radio hears, read (frames.c). Every length in a frame is untrusted: these stay inside the n bytes given. */
#ifndef FRAMES_H
#define FRAMES_H
#include <stdbool.h>
#include <stdint.h>

/* A beacon or probe response's network: its name (ssid, ssid_len; 0 if it gave none), BSSID, channel and security
 * (NET_WPA2, NET_OPEN or NET_OTHER, net.h). */
struct frame_heard {
    const uint8_t *ssid, *bssid;
    unsigned ssid_len, channel, security;
};
/* m, len bytes, as one: false if it isn't. heard_on is the channel it was heard on, for one that names none, or one
 * that can't be. */
bool frame_beacon(const uint8_t *m, uint32_t len, unsigned heard_on, struct frame_heard *h);
/* A name of zeros, as a hidden network sends. */
bool frame_hidden(const uint8_t *ssid, unsigned n);
/* Whether an IPv4 packet's lengths fit the n bytes received (from its EtherType on), and a UDP one's too: the IP
 * layer (wifi_ip.c) trusts them. Anything but IPv4 is let through as it is. */
bool frame_lengths_fit(const uint8_t *type_and_packet, uint32_t n);
#endif
