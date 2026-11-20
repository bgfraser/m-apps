/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* FM-1 (AC791N / WL82) radio bring-up, which the Wi-Fi side needs first, written from the clean-room fact sheet
 * docs/BLE-HW-FACTS.md (branch feat/ble-facts, 41b7374; §14-§17 from d907ce3): every register access names its section
 * there as "HW §n". m-apps keeps only what Wi-Fi uses of felucca-bsp's file: the BLE baseband, its link port and its
 * interrupts are left out (apps/wifi/bsp/README.md).
 *
 *   fm1_ble_rf_init(trims)      the radio in stock's second-boot order (HW §16): the captured Wi-Fi front end with
 *                               the stored trims (fm1_ble_rf.h), clocks and power, the BT analog block (HW §5.1,
 *                               §5.4); what stays TODO(hardware) is listed in fm1_ble_rf.h
 *   fm1_ble_rng32()             the random-number source (HW §1 0x13B00/04) */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"
#include "fm1_time.h"
#include "fm1_ble_rf.h"   /* the captured start-up program (build/gen/ble_rf_tables.h) */

/* ---- BT analog / configuration block (HW §2.2) */
#define FM1_BT_C00       (*(volatile uint32_t *)0x2FC00u)
#define FM1_BT_C04       (*(volatile uint32_t *)0x2FC04u)
#define FM1_BT_C08       (*(volatile uint32_t *)0x2FC08u)   /* [9:0], [19:10] stored I / Q trims */
#define FM1_BT_C0C       (*(volatile uint32_t *)0x2FC0Cu)
#define FM1_BT_C10       (*(volatile uint32_t *)0x2FC10u)   /* bytes 0..2 stored trims */
#define FM1_BT_C14       (*(volatile uint32_t *)0x2FC14u)
#define FM1_BT_C18       (*(volatile uint32_t *)0x2FC18u)
#define FM1_BT_C1C       (*(volatile uint32_t *)0x2FC1Cu)
#define FM1_BT_C20       (*(volatile uint32_t *)0x2FC20u)
#define FM1_BT_C24       (*(volatile uint32_t *)0x2FC24u)
#define FM1_BT_C28       (*(volatile uint32_t *)0x2FC28u)
#define FM1_BT_C40       (*(volatile uint32_t *)0x2FC40u)   /* BT block enable / config [I] */
#define FM1_BT_C48       (*(volatile uint32_t *)0x2FC48u)
#define FM1_BT_C78       (*(volatile uint32_t *)0x2FC78u)
#define FM1_BT_C7C       (*(volatile uint32_t *)0x2FC7Cu)
#define FM1_BT_PLL_TBL   (*(volatile uint32_t *)0x2FC88u)   /* the PLL channel table (an SRAM address) */
#define FM1_BT_C98       (*(volatile uint32_t *)0x2FC98u)
#define FM1_BT_C9C       (*(volatile uint32_t *)0x2FC9Cu)
#define FM1_BT_CA0       (*(volatile uint32_t *)0x2FCA0u)
#define FM1_BT_AGC0      (*(volatile uint32_t *)0x2FD80u)
#define FM1_BT_AGC(i)    (*(volatile uint32_t *)(0x2FD84u + 4u * (i)))   /* 0x2FD84 .. 0x2FD94 */
#define FM1_BT_BRCON     (*(volatile uint32_t *)0x20000u)   /* BR/EDR baseband control (HW §5.1 step 4) */
#define FM1_WL_BTEN      (*(volatile uint32_t *)0x14000u)   /* BT / RF enable (HW §5.1 step 3) */
#define FM1_CLK_CON1_BT  (*(volatile uint32_t *)0x10010u)   /* bits 14-15: BT domain power / reset (HW §5.1) */
#define FM1_RNG_LO       (*(volatile uint32_t *)0x13B00u)   /* random source (HW §1) */
#define FM1_RNG_HI       (*(volatile uint32_t *)0x13B04u)

/* Values the fact sheet does not settle, named so nobody mistakes them for measurements. */
#define FM1_BLE_STEP_DELAY_US 100u      /* HW §5.1 step 3: "a 240-unit delay" and "delays between steps", unit and
                                         * length unknown [M:s]; 100 us is a guess, generous for a power switch */

FM1_INLINE uint32_t fm1_ble_rng32(void) { return FM1_RNG_LO ^ (FM1_RNG_HI * 0x9E3779B9u); }   /* HW §1 */

/* ---- radio bring-up (HW §5.1 - §5.4, §16). Unverified on hardware in every step. */

/* HW §2.4: 81 entries x 3 words, one per 1 MHz channel; stock V15's form {i | i << 8, 0, 0} [M:d], meaning [I] */
static uint32_t fm1_ble_pll_tbl[81 * 3] __attribute__((aligned(4)));

#define FM1_BT_AGC_IDX   (*(volatile uint32_t *)0x2FD98u)   /* HW §2.2: the AGC table's write index (0) */
#define FM1_BT_AGC_DATA  (*(volatile uint32_t *)0x2FD9Cu)   /* HW §2.2: its data port, 128 words */

/* The radio in stock's second-boot order (HW §16.1), with the stored trims (the data of VM 106, 107, 108, 187:
 * src/ble/ble_vm.c found them in the VM or in Optimist's copy; without them the caller never gets here, §15.4):
 *   HW §16.1 groups 2-13   the Wi-Fi front end the BT RF init runs first (HW §5.2), from the captured program
 *                          (hal/fm1_ble_rf.h, build/gen/ble_rf_tables.h), the trim fields byte by byte (§16.4),
 *                          our own VCO scan (group 7);
 *   HW §5.1 steps 3, 4     the BT domain;
 *   HW §5.4 / §16.1 g. 14  the PLL channel table, the AGC table (captured) and configuration, the BT analog words.
 * The clock words of HW §16.1 group 1 (0x10010 bit 10 = our UART's clock, hal/fm1_uart.h; 0x10008 bit 3 = the
 * second core's start, hal/fm1_dual.h) are not the radio's: left as the firmware set them. */
static void fm1_ble_rf_init(const uint8_t *x106, const uint8_t *x107, const uint8_t *x108, const uint8_t *x187)
{
    uint32_t i;
    fm1_ble_rf_run(x106, x107, x108, x187);                /* HW §16.1 groups 2-13 */
    fm1_ble_rf_section(FM1_RF_SECT_BT);                    /* HW §16.1 group 14 */
    /* HW §5.1 step 3: BT domain power / reset, then the BT / RF enable */
    FM1_CLK_CON1_BT &= ~(3u << 14);                        /* bits 14-15 cleared [M:s] */
    fm1_delay_us(FM1_BLE_STEP_DELAY_US);
    FM1_WL_BTEN |= 1u << 7;                                /* -> 0x000C0081 in three steps [M:t] */
    fm1_delay_us(FM1_BLE_STEP_DELAY_US);
    FM1_WL_BTEN = (FM1_WL_BTEN & ~(0x1Fu << 16)) | 0x0Cu << 16;
    fm1_delay_us(FM1_BLE_STEP_DELAY_US);
    FM1_WL_BTEN |= 1u;
    fm1_delay_us(FM1_BLE_STEP_DELAY_US);
    /* HW §5.1 step 4 [M:t] */
    FM1_BT_C40 |= 1u << 10;
    FM1_BT_BRCON = 0xC0000000u;
    FM1_BT_C40 = 0xFFFFu;
    FM1_BT_C40 = 0xFCFDu;
    FM1_BT_C78 = 0x0Fu;
    FM1_BT_C78 = 0x3FFu;
    /* (VM 110, the carrier offset -> PLL_COMP: V15 writes no 110 and a hand-made one changed nothing in the
     * emulator, HW §14.5; U14) */
    /* HW §5.4: the PLL channel table, then the AGC */
    for (i = 0; i < 81u; i++) {
        fm1_ble_pll_tbl[3u * i] = i | i << 8;
        fm1_ble_pll_tbl[3u * i + 1u] = 0;
        fm1_ble_pll_tbl[3u * i + 2u] = 0;
    }
    FM1_BT_PLL_TBL = (uint32_t)(uintptr_t)fm1_ble_pll_tbl;
    FM1_BT_AGC_IDX = 0;                                    /* HW §2.2, §5.4: the AGC table (captured, §17) */
    for (i = 0; i < 128u; i++)
        FM1_BT_AGC_DATA = ble_rf_agc[i];
    FM1_BT_C20 = 0;                                        /* HW §5.4 AGC config [M:t] */
    FM1_BT_AGC0 = 0x000F0000u;
    FM1_BT_AGC(0) = 0x1872BF14u;
    FM1_BT_AGC(1) = 0x1872BF55u;
    FM1_BT_AGC(2) = 0x1872BF00u;
    FM1_BT_AGC(3) = 0;
    FM1_BT_AGC(4) = 0;
    FM1_BT_AGC0 = 0x000F0001u;
    FM1_BT_C7C |= 1u;
    FM1_BT_C48 = 0xB9u;                                    /* HW §2.2 (0xAD after the BR/EDR init, not done here) */
    FM1_BT_C00 = 0x00FFD144u;                              /* HW §2.2, §5.4: 0x2FC00 - 0x2FC28 [M:t] */
    FM1_BT_C04 = 0x4E143CDFu;
    {   /* the BT TX IQ / DC trims: bt_rf_analog_init's base values, with our calibration the compensation from the
         * measured record (fm1_rf_cal.h, RF-CALIBRATION.md §5) */
        uint32_t c08 = 0x03100000u, c10 = 0x80008080u;
        if (fm1_rf_cal_on && fm1_rfc_trimmed)
            fm1_rf_cal_bt_trims(fm1_rfc_x187, &c08, &c10);
        FM1_BT_C08 = c08;
        FM1_BT_C0C = 0x80808080u;
        FM1_BT_C10 = c10;
    }
    FM1_BT_C14 = 0x80u;
    FM1_BT_C18 = 0;
    FM1_BT_C1C = 0;
    FM1_BT_C20 = 0x36555537u;
    FM1_BT_C24 = 0x00034041u;
    FM1_BT_C28 = 0x0005C000u;
    FM1_BT_C98 = 0x02360236u;                              /* HW §2.2: constants [M:s] */
    FM1_BT_C9C = 0x36360008u;
    FM1_BT_CA0 = 0x00003636u;
    /* HW §5.4 then initialises the BR/EDR baseband (0x2FC44, 0x2000C ... 0x20168) and the slot timer. Not done: a
     * BLE-only link layer probably needs neither (HW §10), but whether the engine needs the BT clock running is U12 */
    fm1_ble_rf_section(FM1_RF_SECT_DONE);
}
