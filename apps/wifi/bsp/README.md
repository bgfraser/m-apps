# From w0ts/fm1-base

The radio's start-up and WiFi path (`bsp/`, header-only) and the WPA2 station, its crypto and the IP layer
(`wifi_sta`, `wifi_crypto`, `wifi_ip`), from [w0ts/fm1-base](https://github.com/w0ts/fm1-base) at
`0f36ac350a59f8c857d0e96b0e33995d0bf88d09` and its `bsp` submodule
([hugelton/felucca-bsp](https://github.com/hugelton/felucca-bsp)) at `d83230fbdd2d36794024e5a37008258fe63ac7d1`.
`ble_rf_tables.h` is `chip/ble_rf_tables_v15.h`.

Licences, as their headers say: MPL-2.0; "GPL-3.0-only OR MPL-2.0", taken here under MPL-2.0; and Apache-2.0 for
`fm1_rf_cal.h`, `fm1_wifi_wlc.h` and `ble_rf_tables.h` (from JieLi's SDK). Copyright (C) 2026 Henri David
(hdavid), Optimist (github.com/w0ts/optimist); Leo Kuroshita (@kurogedelic), Hügelton Instruments; Charles Vestal;
Kerem Kilic (Ellic Studio); Zhuhai Jieli Technology Co., Ltd.; Zhuhai Shengke / M-VAVE: each file's header says which.

One change, marked "m-apps" in `wifi_crypto.c`: its SHA-1 block function is `wc_sha1_block_sw()`, and with
`WC_SHA1_HW` defined, `sha1_block()` first offers each block to that function (`sha1hw.c`, the chip's SHA unit).
