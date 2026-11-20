/* SPDX-License-Identifier: MIT */
/* What slots shows, in the launcher's look, drawn on the canvas only, so they also draw on a computer
 * (tests/slots_screens_host.c). */
#ifndef SCREENS_H
#define SCREENS_H
#include "slotfile.h"
#include <stdbool.h>
#include <stdint.h>

/* The list: "save" first, then the slots, then "back". */
#define SCREEN_SAVE 0u
unsigned screen_items(const struct slots *s);
void screen_list(const struct slots *s, unsigned board, unsigned selection, int here, int pending, const char *note,
                 uint16_t note_colour);

/* A slot's page: its actions, the one selected, and how far a hold to confirm has gone (0..100). */
enum { ACTION_LOAD, ACTION_DELETE, ACTION_BACK };
void screen_slot(const struct slot *slot, unsigned board, bool here, bool pending, const uint8_t *actions, unsigned count,
                 unsigned selected, unsigned held_percent);
/* Only the selected action's button, as a hold goes on. */
void screen_slot_hold(const uint8_t *actions, unsigned selected, unsigned held_percent);

/* Naming a slot: the text so far, what it'll be if left empty, the layer, the piano keys held, the cursor's blink. */
void screen_name(unsigned board, const char *text, const char *placeholder, unsigned layer, uint32_t held, bool cursor,
                 const char *note);

/* Work that takes a while: the whole screen, then only the bar as it moves. */
void screen_work(const char *title, const char *line, bool warn);
void screen_work_progress(unsigned done, unsigned total);
#endif
