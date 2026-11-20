/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist) */
/* A small IP layer on the FM-1's Wi-Fi, a station's (no TCP, no DNS):
 * - ARP: answering for our address, and the addresses of those that send us one
 * - DHCP client state machine (Discover -> Offer -> Request -> Ack / Bound)
 * - UDP: up to WIFI_IP_PORTS ports bound, their datagrams (unicast, broadcast) to one receive hook, and datagrams sent
 * - an early drop filter right after the RX DMA
 * From Optimist's wifi/wifi_ip.c, without its access point, ad-hoc and multicast (IGMP) modes: apps/wifi/bsp/README.md. */
#ifndef WIFI_IP_H
#define WIFI_IP_H
#include <stdint.h>

#define WIFI_IP_80211_HDR 24u
#define WIFI_IP_LLC_SNAP 8u
#define WIFI_IP_IP_HDR 20u
#define WIFI_IP_UDP_HDR 8u
#define WIFI_IP_TOTAL_HDR (WIFI_IP_80211_HDR + WIFI_IP_LLC_SNAP + WIFI_IP_IP_HDR + WIFI_IP_UDP_HDR) /* 60 bytes */

#define WIFI_IP_DHCP_PORT_SERVER 67u
#define WIFI_IP_DHCP_PORT_CLIENT 68u
#define WIFI_IP_PORTS 4u                    /* UDP ports bound at once */

enum wifi_ip_dhcp_state {
    WIFI_IP_DHCP_IDLE = 0,
    WIFI_IP_DHCP_DISCOVERING,
    WIFI_IP_DHCP_REQUESTING,
    WIFI_IP_DHCP_BOUND,
    WIFI_IP_DHCP_FAILED
};

struct wifi_arp_entry {
    uint32_t ip;                           /* host order */
    uint8_t mac[6];
    uint8_t valid;
};

struct wifi_ip {
    uint8_t mac[6];                        /* our station MAC */
    uint8_t bssid[6];                      /* AP BSSID */
    uint32_t ip;                           /* our IPv4 address (host order), 0 = unassigned */
    uint32_t netmask;                      /* subnet mask (host order) */
    uint32_t gw;                           /* gateway IP (host order) */
    uint32_t dns;                          /* DNS server (host order) */
    uint32_t dhcp_server_ip;               /* DHCP server IP (host order) */
    uint32_t dhcp_offered_ip;              /* DHCP offered IP (host order) */
    uint32_t dhcp_xid;                     /* DHCP transaction ID */
    enum wifi_ip_dhcp_state dhcp_state;
    int64_t dhcp_t0;                       /* host time (us) of current state */
    uint8_t dhcp_retries;
    uint16_t ports[WIFI_IP_PORTS];         /* the UDP ports bound (0: a free slot) */
    /* a datagram to a bound port (unicast to us, or broadcast), with its arrival time */
    void (*udp_rx)(void *ctx, int64_t now_us, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                   const uint8_t *p, uint32_t n);
    void *ctx;                             /* for udp_rx */
    struct wifi_arp_entry arp_tbl[4];
    uint16_t tx_seq;                       /* 802.11 sequence counter */
    uint8_t protect;                       /* 1: our data frames carry the Protected bit (keys installed: the MAC encrypts) */
    void (*rand)(uint8_t *out, uint8_t n);
};

/* Initialize IP layer state */
void wifi_ip_init(struct wifi_ip *wip, const uint8_t sta_mac[6], const uint8_t bssid[6], void (*rand_fn)(uint8_t *out, uint8_t n));

/* bind a UDP port (its datagrams to wip->udp_rx): 0, or -1 (no free slot, port 0); unbind lets it go */
int wifi_ip_bind(struct wifi_ip *wip, uint16_t port);
void wifi_ip_unbind(struct wifi_ip *wip, uint16_t port);
int wifi_ip_bound(const struct wifi_ip *wip, uint16_t port);

/* Start DHCP client negotiation. Returns frame length written to tx_buf (or 0). */
uint32_t wifi_ip_dhcp_start(struct wifi_ip *wip, uint8_t *tx_buf);

/* Periodic timer polling (DHCP timeouts / retransmissions). Returns frame length written to tx_buf if transmit needed. */
uint32_t wifi_ip_poll(struct wifi_ip *wip, int64_t now_us, uint8_t *tx_buf);

/* Fast early drop filter (Optimist's docs/LINK-DESIGN.md §2.7).
 * Call immediately after RX DMA (or after decryption).
 * Returns 1 to keep and process frame, 0 to drop immediately. */
int wifi_ip_early_filter(const struct wifi_ip *wip, const uint8_t *frame, uint32_t len);

/* Process an incoming 802.11 Data frame (LLC/SNAP encapsulated ARP, IPv4/UDP).
 * Delivers the datagrams of bound ports to wip->udp_rx. If a response frame (ARP reply, DHCP req, etc.) is generated,
 * writes to tx_buf and sets tx_len, returning 1; returns 0 if consumed, -1 if ignored/error. */
int wifi_ip_rx_frame(struct wifi_ip *wip, const uint8_t *frame, uint32_t len, int64_t now_us, uint8_t *tx_buf, uint32_t *tx_len);

/* A UDP datagram from src_port. Encapsulates in 802.11 Data + LLC/SNAP + IPv4 + UDP.
 * Returns the total frame length written to tx_buf. It goes to the address's MAC if known (ARP), else the gateway's,
 * else the access point's. */
uint32_t wifi_ip_send_udp(struct wifi_ip *wip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *payload, uint32_t payload_len, uint8_t *tx_buf);

#endif
