/* SPDX-License-Identifier: MIT */
/* A USB flash drive on USB0, with the unit as the host: M-Unbricker's host driver and bulk-only transport
 * (m-unbricker/src/husb.c, bot.c), and the few SCSI commands a drive needs to be read. The drive needs 5 V from
 * elsewhere, such as a Y-cable with a power injector, as the unit doesn't supply VBUS. Polled, read-only. */
#ifndef USBFLASH_STICK_H
#define USBFLASH_STICK_H
#include <stdbool.h>
#include <stdint.h>
#define STICK_SECTOR 512u
/* Starts the controller as a host and waits up to wait_ms for a drive, then makes it ready. 0 on success, otherwise
 * what to show. */
const char *stick_open(uint32_t wait_ms);
/* The drive's size in 512-byte sectors. */
uint32_t stick_sectors(void);
/* One sector, for kit/fat.c (fat_source()). */
bool stick_read(uint32_t sector, uint8_t *data);
/* The drive is never written: kit/fat.c gets this as its writer. */
bool stick_refuse_write(uint32_t sector, const uint8_t *data);
/* Takes the controller down, so whatever runs next starts USB from scratch. */
void stick_close(void);
/* Milliseconds, for the app: the timer the host driver runs on. Differences only, as it wraps. */
uint32_t stick_ms(void);
#endif
