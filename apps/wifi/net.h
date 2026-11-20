/* SPDX-License-Identifier: MPL-2.0 */
/* WiFi for an app, polled: the radio (bsp/, from fm1-base), a scan, a WPA2-PSK join and DHCP (Optimist's own stack,
 * wifi_sta, wifi_crypto, wifi_ip). The join follows w0ts/fm1-base's device/wifi_own.c (GPL-3.0-only OR MPL-2.0). */
#ifndef NET_H
#define NET_H
#include <stdbool.h>
#include <stdint.h>

/* What a network asks of a station: WPA2-PSK (CCMP, without required PMF) and open networks can be joined here, and
 * nothing else, WPA3-only among them. */
enum { NET_WPA2, NET_OPEN, NET_OTHER };
struct net_ap {
    char ssid[33];
    uint8_t bssid[6], channel, security;
    int8_t rssi;                /* dBm, the strongest heard */
};
/* The networks heard on channels 1 to 13, a name once (its strongest), strongest first, hidden ones left out. progress
 * is told after each channel, of 13. Starts the radio the first time. */
unsigned net_scan(struct net_ap *list, unsigned most, void (*progress)(unsigned done, unsigned total));

/* The steps of a join, for status(). */
enum { NET_STEP_RADIO, NET_STEP_FINDING, NET_STEP_KEY, NET_STEP_JOINING, NET_STEP_ADDRESS, NET_STEPS };
struct net_result {
    int8_t rssi;
    uint8_t channel;
    uint32_t ip, gateway, dns;  /* host order */
    uint32_t ms;                /* from the start to an address */
    uint8_t step;               /* the step it got to: where it stopped, if it failed */
};
/* Joins and asks DHCP for an address: 0, or why not, in words. status() is told each step as it starts. Whether it's
 * open or WPA2 is what the network says when heard; pass is ignored for an open one. */
const char *net_join(const char *ssid, const char *pass, void (*status)(unsigned step), struct net_result *r);
/* Leaves the network joined, if any. */
void net_leave(void);
#endif
