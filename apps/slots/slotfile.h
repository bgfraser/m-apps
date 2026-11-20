/* SPDX-License-Identifier: MIT */
#ifndef SLOTFILE_H
#define SLOTFILE_H
#include "fat.h"
/* Firmware and its settings on the card, one file a slot, shared by every unit of the model:
 * /MBOOT/SLOTS/FM1/ or /MBOOT/SLOTS/SMK37/, each slot SLOTnnnn.SLT with its name as the long name, NAME.slt.
 *
 * A slot holds the firmware's part of the flash only: 0x4000 up to the key area at 0xFF000, and on the SMK-37 not its
 * Bluetooth and calibration sectors at 0xC0000-0xC1FFF. The boot area and the unit's own data are never in a slot, and
 * a load never writes them (as clean-flash's firmware scope). A 512-byte header says which of those sectors the file
 * holds, in order after it; the others are blank (all 0xFF).
 *
 * Header: "SLOT", format 1, board (1 FM-1, 2 SMK-37), header size (u16), sectors stored (u32), the flash chip's JEDEC
 * ID (u32), at 16 a bitmap of 256 bits (bit n: flash sector n is stored), at 48 the name (64 bytes, NUL-padded), at
 * 112 the CRC32 of the stored sectors, at 508 the CRC32 of the 508 bytes before it. Little-endian.
 *
 * Each unit keeps which slot it holds, and a load it hasn't finished, in <8 hex digits of its unique ID>.DAT there. */
#define SLOT_FLASH_SIZE 0x100000u
#define SLOT_NAME_MOST 40u
#define MOST_SLOTS 40u
#define NO_SLOT (-1)
struct slot {
    char id[8];                         /* the short name, padded with spaces */
    char name[SLOT_NAME_MOST+1];        /* what to show: the long name without .slt, in UTF-8 */
    struct fat_entry file;
    bool sound;                         /* its header is whole */
    uint8_t stored[32];
    uint32_t sectors;
};
struct slots {
    uint32_t folder;
    bool has_folder;
    struct slot list[MOST_SLOTS];
    unsigned count;
    bool more;
    /* Ids, with no slot as all zeros. */
    char active[8], pending[8];
    bool has_state;
    struct fat_entry state_file;
    char unit_name[11];
    uint32_t jedec;
    void (*progress)(const char *what, unsigned done, unsigned total);
    /* Whether an image holds firmware that would start: read reads it like flash. */
    bool (*valid)(bool (*read)(void *ctx, uint32_t address, void *out, uint32_t size), void *ctx);
};
/* Mounts, lists the slots, and reads what this unit's last load left. board is 1 FM-1 or 2 SMK-37; jedec goes in
 * what's saved. */
const char *slots_open(struct slots *s, uint32_t card_sectors, const uint8_t uid[8], uint32_t jedec, unsigned board);
/* The slot holding what's on the unit, or NO_SLOT. It leaves out what a load doesn't write. */
int slots_on_unit(struct slots *s);
/* Whether a name can be a slot's: what a computer allows in a file name, at most SLOT_NAME_MOST characters. */
bool slots_name_ok(const char *name);
/* A name no slot has: base, or base with a number after it. out holds SLOT_NAME_MOST+1. */
void slots_free_name(const struct slots *s, const char *base, char *out);
/* Saves what's on the unit as a new slot with this name (made unique if a slot has it already). */
const char *slots_save_new(struct slots *s, const char *name);
/* Saves what's on the unit, then writes this slot to it. Only returns with no error once it's all done. */
const char *slots_load(struct slots *s, unsigned index);
const char *slots_delete(struct slots *s, unsigned index);
/* The slot an interrupted load was writing, or NO_SLOT. */
int slots_pending(const struct slots *s);
#endif
