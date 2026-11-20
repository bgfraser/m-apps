# From w0ts/fm1-base

The radio's start-up and WiFi path (`bsp/`, header-only) and the WPA2 station, its crypto and the IP layer
(`wifi_sta`, `wifi_crypto`, `wifi_ip`), cut down as below, from [w0ts/fm1-base](https://github.com/w0ts/fm1-base) at
`0f36ac350a59f8c857d0e96b0e33995d0bf88d09` and its `bsp` submodule
([hugelton/felucca-bsp](https://github.com/hugelton/felucca-bsp)) at `d83230fbdd2d36794024e5a37008258fe63ac7d1`.
`ble_rf_tables.h` is `chip/ble_rf_tables_v15.h`.

Licences, as their headers say: MPL-2.0; "GPL-3.0-only OR MPL-2.0", taken here under MPL-2.0; and Apache-2.0 for
`fm1_rf_cal.h`, `fm1_wifi_wlc.h` and `ble_rf_tables.h` (from JieLi's SDK). Copyright (C) 2026 Henri David
(hdavid), Optimist (github.com/w0ts/optimist); Leo Kuroshita (@kurogedelic), Hügelton Instruments; Charles Vestal;
Zhuhai Jieli Technology Co., Ltd.; Zhuhai Shengke / M-VAVE: each file's header says which.

## What's changed here

Kept small, for a station that sets up WiFi and, next, a store that fetches over HTTP: what neither uses is cut. To
take a newer upstream, start from its files and cut the same again; what was cut is in this repo's history (the
commit "wifi: only what a station uses of the copied stack").

- **`wifi_crypto.c`:** its SHA-1 block function is `wc_sha1_block_sw()`, and with `WC_SHA1_HW` defined, `sha1_block()`
  first offers each block to that function (`sha1hw.c`, the chip's SHA unit), marked "m-apps". Cut: `wc_pmk()` (the
  PMK in one go: the sliced job is used), AES encryption, the AES key wrap and the software CCMP (an access point's,
  and the host's tests').
- **`wifi_ip.c`, `wifi_ip.h`:** the station's side only. Cut: the access point and ad-hoc modes (FromDS and IBSS
  frames, the DHCP server's hook, the ping reply), IGMP and multicast, `wifi_ip_reply_udp()`, `wifi_ip_arp_learn()`,
  the `on_addr` and `send` hooks. Kept for the store: ARP, the DHCP client, UDP ports, `wifi_ip_send_udp()`.
- **`wifi_sta.c`, `wifi_sta.h`:** cut `wifi_sta_set_creds()` and `wifi_sta_get_mac()` (the MAC comes from the flash's
  unique ID, `net.c`).
- **`bsp/fm1_ble.h`:** the radio's bring-up only. Cut: the BLE baseband, its link-column port and interrupts.
- **`bsp/fm1_ble_rf.h`:** cut the breadcrumb (a `.noinit` record for felucca's console, written on every start-up
  op) and `fm1_rf_cal_wifi_iq()`.
- **`bsp/fm1_irq.h`:** only the global interrupt enable the start-up uses.
- **`bsp/fm1_wifi.h`, `bsp/fm1_time.h`:** cut helpers nothing calls.
- **Left out:** `fm1_sys.h` and `fm1_cpu1_stop.h` (firmware's P33, watchdog and second core).
