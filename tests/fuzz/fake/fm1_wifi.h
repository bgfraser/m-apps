/* SPDX-License-Identifier: MIT */
/* apps/wifi/bsp/fm1_wifi.h on a computer, for the station's fuzz test: the register places it names, and its writes to
 * the MAC done nowhere. */
#pragma once
#include <stdint.h>
#define FM1_WIFI_ADDR_OWN 0x1008u
#define FM1_WIFI_ADDR_BSSID 0x1010u
#define FM1_WIFI_RX_FILTER 0x1400u
#define FM1_WIFI_RX_FILTER_ALL 0x1u
#define FM1_WIFI_WCID_ADDR(w) (0x1800u + 8u * (w))
#define FM1_WIFI_PKEY(w) (0x4000u + 32u * (w))
#define FM1_WIFI_IVEIV(w) (0x6000u + 8u * (w))
#define FM1_WIFI_WCID_ATTR(w) (0x6800u + 4u * (w))
#define FM1_WIFI_SKEY(i) (0x6C00u + 32u * (i))
#define FM1_WIFI_SKEY_MODE 0x7000u
#define FM1_WIFI_CIPHER_CCMP 4u
static inline uint32_t fm1_wifi_mac_rd(uint32_t r) { (void)r; return 0; }
static inline void fm1_wifi_mac_wr(uint32_t r, uint32_t v) { (void)r, (void)v; }
static inline void fm1_wifi_key_wr(uint32_t r, const uint8_t *key) { (void)r, (void)key; }
static inline void fm1_wifi_addr_wr(uint32_t r, const uint8_t *mac, uint32_t hi_or) { (void)r, (void)mac, (void)hi_or; }
static inline void fm1_wifi_set_channel(uint32_t ch) { (void)ch; }
