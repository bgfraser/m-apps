/* SPDX-License-Identifier: MPL-2.0 */
/* The join follows w0ts/fm1-base's device/wifi_own.c (GPL-3.0-only OR MPL-2.0, taken here under MPL-2.0): Copyright
 * (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist). */
#include "net.h"
#include "frames.h"
#include "app.h"
#include "sha1hw.h"
#include "fm1_ble.h"
#include "fm1_wifi.h"
#include "wifi_crypto.h"
#include "wifi_ip.h"
#include "wifi_sta.h"
#include <string.h>
#define LISTEN_MS 150u
#define FIND_PASSES 3u
#define STEP_MS 400u
#define STEP_TRIES 6u
#define ATTEMPTS 3u
#define HANDSHAKE_MS 6000u
#define DHCP_MS 15000u
#define RX_MOST 1600u

/* The radio's DMA reaches only RAM below 0x01C20000: the kit's .dma region (kit/app.ld.S). */
static uint32_t rxb[(RX_MOST+8u)/4u+1u] __attribute__((section(".dma")));
static uint32_t txb[(FM1_WIFI_TX_HDR+1600u)/4u] __attribute__((section(".dma")));
/* The SDK board's constant records, as Optimist passes them; 187 is measured (fm1_rf_cal_on). */
static const uint8_t x106[2] = {0x0B, 0x0B}, x107[7] = {1, 7, 4, 7, 11, 1, 7};
static const uint8_t x108[20], x187[68];

static struct wifi_ip ip;
static struct wifi_sta sta;
static struct wc_pmk_job job;
static uint8_t pmk[32], mac[6];
static bool started, joined, open_net;

/* What the radio's start-up changes outside the MAC, as it was found, for net_off(): the RF / BT enables (0x14000) and
 * 0x14028, the Wi-Fi clock / reset block (0x14040-0x1405C) and the radio's analog settings (0x11900-0x11968). */
#define WL_EN 0x14000u
#define WL_28 0x14028u
#define WL_CLOCK 0x14040u
#define WL_CLOCK_N 8u
#define WL_ANALOG 0x11900u
#define WL_ANALOG_N 27u
static struct { uint32_t en, w28, clock[WL_CLOCK_N], analog[WL_ANALOG_N]; } found_regs;

static uint32_t t_last;
static int64_t t_us;
static int64_t now_us(void) {
    uint32_t t = fm1_ticks(), d = (t-t_last)/FM1_TICKS_PER_US;
    t_last += d*FM1_TICKS_PER_US;
    t_us += d;
    return t_us;
}
static uint32_t now_ms(void) { return (uint32_t)(now_us()/1000); }

static void rand_bytes(uint8_t *out, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i)
        out[i] = (uint8_t)(fm1_ble_rng32() ^ fm1_ticks() >> (i&7));
}

/* What went in and out, for the log when an address doesn't come. */
static struct { unsigned frames, data, ours, ip, sent, send_errors; } count;

static uint8_t *frame_out(void) { return (uint8_t *)txb+FM1_WIFI_TX_HDR; }
/* The frame at frame_out(), len bytes, out at 1 Mb/s; ack: the MAC waits for the receiver's ACK, and retries. */
static void transmit(uint32_t len, uint32_t ack) {
    if (!len)
        return;
    fm1_wifi_tx_prepare_rate(txb, len, 1u, ack, FM1_WIFI_RATE_CCK1);
    if (fm1_wifi_tx(txb) == 0)
        ++count.sent;
    else
        ++count.send_errors;
}
static void send(uint32_t len) { transmit(len, 1u); }

/* Up to max frames waiting, each to f as its 802.11 frame, without the radio's header. */
static void receive(void (*f)(const uint8_t *, uint32_t, int8_t), unsigned max) {
    for (unsigned i = 0; i < max; ++i) {
        int32_t n = fm1_wifi_rx(rxb, RX_MOST);
        if (n <= 0)
            return;
        const uint8_t *fr = (const uint8_t *)rxb+4;
        uint32_t mpdu = fm1_wifi_rx_mpdu_len(fr);
        ++count.frames;
        if (mpdu+FM1_WIFI_RXWI <= (uint32_t)n)
            f(fr+FM1_WIFI_RXWI, mpdu, (int8_t)fm1_wifi_rx_rssi(fr));
    }
}

static void drop(const uint8_t *f, uint32_t len, int8_t rssi) { (void)f, (void)len, (void)rssi; }

static void start(void) {
    if (started)
        return;
    fm1_time_init();
    t_last = fm1_ticks();
    /* A locally administered address of the unit's own, from its flash's unique ID. */
    uint8_t uid[8] = {0};
    if (flash_init(1, 1))
        flash_uid(uid);
    for (unsigned i = 0; i < 6; ++i)
        mac[i] = (uint8_t)(uid[i]^uid[(i+6)&7]^0x5a);
    mac[0] = (uint8_t)((mac[0]&0xfc)|0x02);
    found_regs.en = REG(WL_EN);
    found_regs.w28 = REG(WL_28);
    for (unsigned i = 0; i < WL_CLOCK_N; ++i)
        found_regs.clock[i] = REG(WL_CLOCK+4u*i);
    for (unsigned i = 0; i < WL_ANALOG_N; ++i)
        found_regs.analog[i] = REG(WL_ANALOG+4u*i);
    fm1_rf_cal_on = 1;
    uint32_t t0 = fm1_ticks();
    fm1_ble_rf_init(x106, x107, x108, x187);
    fm1_wifi_up();
    app_log("radio up in %u ms, calibration %d %d", (unsigned)((fm1_ticks()-t0)/FM1_TICKS_PER_US/1000u), fm1_rfc_scan_rc,
            fm1_rfc_trim_rc);
    started = true;
}

/* ---- the networks heard ---- */

static struct net_ap *scan_list;
static unsigned scan_most, scan_count;

static void on_scan(const uint8_t *m, uint32_t len, int8_t rssi) {
    struct frame_heard h;
    if (!frame_beacon(m, len, fm1_wifi_channel(), &h) || !h.ssid || frame_hidden(h.ssid, h.ssid_len))
        return;
    struct net_ap *ap = 0;
    for (unsigned i = 0; i < scan_count && !ap; ++i)
        if (strlen(scan_list[i].ssid) == h.ssid_len && !memcmp(scan_list[i].ssid, h.ssid, h.ssid_len))
            ap = &scan_list[i];
    if (!ap) {
        if (scan_count == scan_most)
            return;
        ap = &scan_list[scan_count++];
        memset(ap, 0, sizeof *ap);
        memcpy(ap->ssid, h.ssid, h.ssid_len);
        ap->rssi = -128;
    }
    if (rssi > ap->rssi) {
        ap->rssi = rssi;
        memcpy(ap->bssid, h.bssid, 6);
        ap->channel = (uint8_t)h.channel;
        ap->security = (uint8_t)h.security;
    }
}

static void listen(unsigned channel, void (*f)(const uint8_t *, uint32_t, int8_t)) {
    fm1_wifi_set_channel(channel);
    uint32_t t0 = now_ms();
    while (now_ms()-t0 < LISTEN_MS)
        receive(f, 24);
    app_feed();
}

unsigned net_scan(struct net_ap *list, unsigned most, void (*progress)(unsigned done, unsigned total)) {
    start();
    net_leave();
    fm1_wifi_mac_wr(FM1_WIFI_RX_FILTER, FM1_WIFI_RX_FILTER_ALL);
    scan_list = list;
    scan_most = most;
    scan_count = 0;
    for (unsigned ch = 1; ch <= 13; ++ch) {
        listen(ch, on_scan);
        if (progress)
            progress(ch, 13);
    }
    for (unsigned i = 1; i < scan_count; ++i)
        for (unsigned j = i; j && list[j].rssi > list[j-1].rssi; --j) {
            struct net_ap t = list[j];
            list[j] = list[j-1];
            list[j-1] = t;
        }
    app_log("scan: %u networks", scan_count);
    return scan_count;
}

/* ---- joining ---- */

static const char *want;
static unsigned want_len;
static struct net_ap found;
static unsigned answers;            /* probe responses naming it: whether asking by name is heard */

static void on_find(const uint8_t *m, uint32_t len, int8_t rssi) {
    struct frame_heard h;
    if (!frame_beacon(m, len, fm1_wifi_channel(), &h) || h.ssid_len != want_len || memcmp(h.ssid, want, want_len))
        return;
    answers += m[0] == 0x50;
    if (rssi > found.rssi) {
        found.rssi = rssi;
        memcpy(found.bssid, h.bssid, 6);
        found.channel = (uint8_t)h.channel;
        found.security = (uint8_t)h.security;
    }
}

static void on_join(const uint8_t *f, uint32_t len, int8_t rssi) {
    (void)rssi;
    uint32_t tx_len = 0;
    if (sta.state != WIFI_STA_STATE_CONNECTED && wifi_sta_rx_frame(&sta, f, len, frame_out(), &tx_len) == 1 && tx_len)
        send(tx_len);
}

static void on_data(const uint8_t *f, uint32_t len, int8_t rssi) {
    (void)rssi;
    uint32_t tx_len = 0;
    if (len < 32u || (f[0]&0x0Cu) != 0x08u)
        return;
    ++count.data;
    uint32_t header = (f[0]&0x80u) ? 26u : 24u;
    if (len < header+8u || !wifi_ip_early_filter(&ip, f, len) || !frame_lengths_fit(f+header+6u, len-header-6u))
        return;
    ++count.ours;
    /* EAPOL: a new group key. */
    if (f[header+6u] == 0x88u && f[header+7u] == 0x8Eu) {
        if (wifi_sta_rx_frame(&sta, f, len, frame_out(), &tx_len) == 1)
            send(tx_len);
        return;
    }
    int got = wifi_ip_rx_frame(&ip, f, len, now_us(), frame_out(), &tx_len);
    if (got >= 0)
        ++count.ip;
    if (got == 1)
        send(tx_len);
}

/* The station's association request, which is WPA2's: for an open network, without its RSN element, which it puts
 * last, and without the Privacy capability. */
static uint32_t build_assoc(struct wifi_sta *s, uint8_t *b) {
    uint32_t n = wifi_sta_build_assoc_req(s, b);
    if (open_net && n >= 24u+4u+22u && b[n-22] == 0x30) {
        b[24] &= (uint8_t)~0x10u;
        n -= 22;
    }
    return n;
}

/* A probe request naming the network: one that doesn't say its name in its beacons (a hidden one) answers it with a
 * probe response that does. Broadcast, so nothing acknowledges it. */
static void probe(const char *ssid, unsigned n) {
    static const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c};
    static uint16_t seq;
    uint8_t *b = frame_out();
    memset(b, 0, 24);
    b[0] = 0x40;
    memset(b+4, 0xff, 6);
    memcpy(b+10, mac, 6);
    memset(b+16, 0xff, 6);
    b[22] = (uint8_t)(seq << 4);
    b[23] = (uint8_t)(seq >> 4);
    ++seq;
    uint32_t at = 24;
    b[at++] = 0;
    b[at++] = (uint8_t)n;
    memcpy(b+at, ssid, n);
    at += n;
    memcpy(b+at, rates, sizeof rates);
    transmit(at+sizeof rates, 0u);
}

/* Sends a request until the state moves on from state, a few times. */
static bool step(uint8_t state, uint32_t (*build)(struct wifi_sta *, uint8_t *)) {
    send(build(&sta, frame_out()));
    unsigned tries = 1;
    uint32_t t0 = now_ms();
    while (sta.state == state) {
        receive(on_join, 24);
        if (now_ms()-t0 >= STEP_MS) {
            if (tries++ >= STEP_TRIES)
                return false;
            t0 = now_ms();
            send(build(&sta, frame_out()));
        }
        app_feed();
    }
    return true;
}

/* 0, or what failed. */
static const char *join(void) {
    const char *why = "It didn't answer";
    memset(&count, 0, sizeof count);
    for (unsigned attempt = 1; attempt <= ATTEMPTS; ++attempt) {
        wifi_sta_set_mac_regs(mac, found.bssid, found.channel);
        wifi_sta_init(&sta, mac, rand_bytes);
        wifi_sta_set_pmk(&sta, pmk, (const uint8_t *)want, want_len);
        wifi_sta_set_ap(&sta, found.bssid);
        sta.state = WIFI_STA_STATE_AUTH;
        send(wifi_sta_build_deauth(&sta, frame_out()));
        uint32_t t0 = now_ms();
        while (now_ms()-t0 < 150)
            receive(drop, 24);
        if (!step(WIFI_STA_STATE_AUTH, wifi_sta_build_auth) || sta.state != WIFI_STA_STATE_ASSOC) {
            app_log("no auth answer: %u frames heard, %u sent, %u send errors", count.frames, count.sent, count.send_errors);
            continue;
        }
        if (!step(WIFI_STA_STATE_ASSOC, build_assoc) || sta.state < WIFI_STA_STATE_WAIT_M1) {
            why = "It turned the unit away";
            continue;
        }
        /* Open: associated is joined. (The station now waits for WPA2's first key message, which won't come.) */
        if (open_net)
            return 0;
        t0 = now_ms();
        while (sta.state != WIFI_STA_STATE_CONNECTED && sta.state != WIFI_STA_STATE_FAILED && now_ms()-t0 < HANDSHAKE_MS) {
            receive(on_join, 24);
            app_feed();
        }
        app_log("join attempt %u: state %u", attempt, (unsigned)sta.state);
        if (sta.state == WIFI_STA_STATE_CONNECTED)
            return 0;
        /* The AP checks our M2 with the key from the password: with the wrong one, it never sends M3. */
        why = sta.state == WIFI_STA_STATE_WAIT_M3 ? "Wrong password" : "It didn't start the handshake";
    }
    /* Associated, perhaps, but not joined: say so, rather than leave the router waiting. */
    send(wifi_sta_build_deauth(&sta, frame_out()));
    return why;
}

static void (*told)(unsigned step);
static struct net_result *result;
static void at(unsigned step) {
    result->step = (uint8_t)step;
    told(step);
}

const char *net_join(const char *ssid, const char *pass, void (*status)(unsigned step), struct net_result *r) {
    memset(r, 0, sizeof *r);
    told = status;
    result = r;
    at(NET_STEP_RADIO);
    start();
    net_leave();
    uint32_t began = now_ms();
    want = ssid;
    want_len = (unsigned)strlen(ssid);
    memset(&found, 0, sizeof found);
    found.rssi = -128;
    answers = 0;
    at(NET_STEP_FINDING);
    fm1_wifi_mac_wr(FM1_WIFI_RX_FILTER, FM1_WIFI_RX_FILTER_ALL);
    for (unsigned pass_n = 0; pass_n < FIND_PASSES && found.rssi == -128; ++pass_n)
        for (unsigned ch = 1; ch <= 13; ++ch) {
            fm1_wifi_set_channel(ch);
            probe(ssid, want_len);
            listen(ch, on_find);
        }
    if (found.rssi == -128)
        return "Not heard here";
    r->rssi = found.rssi;
    r->channel = found.channel;
    app_log("%s: channel %u, %d dBm, security %u, %u probe responses", ssid, found.channel, found.rssi, found.security,
            answers);
    if (found.security == NET_OTHER)
        return "Not WPA2: can't join it";
    open_net = found.security == NET_OPEN;
    if (!open_net && !*pass)
        return "It needs a password";
    fm1_wifi_set_channel(found.channel);
    memset(pmk, 0, sizeof pmk);
    if (!open_net) {
        at(NET_STEP_KEY);
        sha1_hw_check();
        uint32_t t0 = now_ms();
        wc_pmk_begin(&job, pass, (const uint8_t *)ssid, want_len);
        while (!wc_pmk_step(&job, 256u, pmk))
            app_feed();
        app_log("key made in %u ms", (unsigned)(now_ms()-t0));
    }
    at(NET_STEP_JOINING);
    wifi_sta_clear_hw_keys();
    const char *why = join();
    if (why)
        return why;
    joined = true;
    uint32_t t0 = now_ms();
    while (now_ms()-t0 < 300)
        receive(on_join, 24);
    fm1_wifi_mac_wr(FM1_WIFI_RX_FILTER, FM1_WIFI_RX_FILTER_STA);
    at(NET_STEP_ADDRESS);
    wifi_ip_init(&ip, mac, found.bssid, rand_bytes);
    ip.protect = open_net ? 0 : 1;
    memset(&count, 0, sizeof count);
    send(wifi_ip_dhcp_start(&ip, frame_out()));
    ip.dhcp_t0 = now_us();
    t0 = now_ms();
    while (!ip.ip) {
        receive(on_data, 8);
        send(wifi_ip_poll(&ip, now_us(), frame_out()));
        if (now_ms()-t0 > DHCP_MS) {
            app_log("no address: %u frames, %u data, %u ours, %u to IP; %u sent, %u send errors; DHCP state %u, %u tries",
                    count.frames, count.data, count.ours, count.ip, count.sent, count.send_errors, (unsigned)ip.dhcp_state,
                    (unsigned)ip.dhcp_retries);
            return "Joined, but no address";
        }
        app_feed();
    }
    r->ip = ip.ip;
    r->gateway = ip.gw;
    r->dns = ip.dns;
    r->ms = now_ms()-began;
    r->step = NET_STEPS;
    app_log("address %08x gateway %08x dns %08x, %u ms", (unsigned)ip.ip, (unsigned)ip.gw, (unsigned)ip.dns, (unsigned)r->ms);
    return 0;
}

void net_leave(void) {
    if (joined) {
        send(wifi_sta_build_deauth(&sta, frame_out()));
        wifi_sta_clear_hw_keys();
    }
    joined = false;
}

void net_off(void) {
    if (!started)
        return;
    net_leave();
    /* The RF paths and BT off first, then the clock block and the analog settings as they were. */
    REG(WL_EN) = found_regs.en;
    sync_cpu();
    REG(WL_28) = found_regs.w28;
    for (unsigned i = 0; i < WL_CLOCK_N; ++i)
        REG(WL_CLOCK+4u*i) = found_regs.clock[i];
    for (unsigned i = 0; i < WL_ANALOG_N; ++i)
        REG(WL_ANALOG+4u*i) = found_regs.analog[i];
    sync_cpu();
    started = false;
    app_log("radio off");
}
