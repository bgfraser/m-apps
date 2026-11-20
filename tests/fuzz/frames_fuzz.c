/* SPDX-License-Identifier: MIT */
/* libFuzzer: what the radio hears, on any bytes, as apps/wifi/net.c hands it on: a beacon or probe response
 * (frames.c), a data frame through the length check to the IP layer (wifi_ip.c: ARP, DHCP, UDP), and a WPA2 key
 * message to the station (wifi_sta.c). Nothing may read outside the frame. The first byte picks the station's state. */
#include "frames.h"
#include "net.h"
#include "wifi_ip.h"
#include "wifi_sta.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t mac[6] = {2, 0x11, 0x22, 0x33, 0x44, 0x55}, ap[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

static void zeros(uint8_t *out, uint8_t n) {
    memset(out, 0, n);
}

static void udp_rx(void *ctx, int64_t now_us, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                   const uint8_t *p, uint32_t n) {
    (void)ctx, (void)now_us, (void)src_ip, (void)src_port, (void)dst_ip, (void)dst_port;
    volatile uint8_t sum = 0;
    for (uint32_t i = 0; i < n; ++i)
        sum ^= p[i];
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    if (!n || n > 2048)
        return 0;
    unsigned state = data[0];
    uint8_t *f = malloc(n-1 ? n-1 : 1);
    memcpy(f, data+1, n-1);
    uint32_t len = (uint32_t)(n-1);
    static uint8_t tx[2048];
    uint32_t tx_len = 0;

    struct frame_heard h;
    if (frame_beacon(f, len, 6, &h)) {
        assert(h.channel >= 1 && h.channel <= 13 && h.security <= NET_OTHER);
        assert(!h.ssid || (h.ssid >= f && h.ssid_len <= 32 && h.ssid+h.ssid_len <= f+len));
        if (h.ssid)
            frame_hidden(h.ssid, h.ssid_len);
    }

    /* net.c's on_data */
    static struct wifi_ip ip;
    wifi_ip_init(&ip, mac, ap, zeros);
    ip.dhcp_xid = 0x12345678u;
    ip.dhcp_state = (enum wifi_ip_dhcp_state)(state%5);
    ip.ip = state&8 ? 0xc0a80105u : 0;
    ip.udp_rx = udp_rx;
    wifi_ip_bind(&ip, 5353);
    if (len >= 32u && (f[0]&0x0Cu) == 0x08u) {
        uint32_t header = (f[0]&0x80u) ? 26u : 24u;
        if (len >= header+8u && wifi_ip_early_filter(&ip, f, len) && frame_lengths_fit(f+header+6u, len-header-6u))
            wifi_ip_rx_frame(&ip, f, len, 0, tx, &tx_len);
    }

    /* the join's on_join: a station waiting for the first or third key message */
    static struct wifi_sta sta;
    static const uint8_t pmk[32] = {1, 2, 3};
    wifi_sta_init(&sta, mac, zeros);
    wifi_sta_set_pmk(&sta, pmk, (const uint8_t *)"fuzz", 4);
    wifi_sta_set_ap(&sta, ap);
    sta.state = state&16 ? WIFI_STA_STATE_WAIT_M3 : WIFI_STA_STATE_WAIT_M1;
    wifi_sta_rx_frame(&sta, f, len, tx, &tx_len);
    free(f);
    return 0;
}
