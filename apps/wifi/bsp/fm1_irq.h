/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 Charles Vestal */
/* FM-1 interrupt HAL: of felucca-bsp's file, only the global enable that the radio start-up turns off around a burst
 * (fm1_ble_rf.h). The vectors, attach, exceptions and crash records are left out (apps/wifi/bsp/README.md). */
#pragma once
#include <stdint.h>

static inline uint32_t fm1_icfg(void) { uint32_t v; __asm__ volatile("%0 = icfg" : "=r"(v)); return v; }
static inline void fm1_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void fm1_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }
