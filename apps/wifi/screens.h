/* SPDX-License-Identifier: MIT */
/* What the WiFi app shows, in the launcher's look, drawn on the canvas only, so they also draw on a computer
 * (tests/wifi_screens_host.c). */
#ifndef SCREENS_H
#define SCREENS_H
#include "net.h"
#include "wifilist.h"
#include <stdbool.h>
#include <stdint.h>

/* The saved networks: "Add a network" first, then the networks, then "Back". */
#define SCREEN_ADD 0u
unsigned screen_list_items(const struct wifi_list *l);
void screen_list(const struct wifi_list *l, unsigned board, unsigned selection, const char *note, uint16_t note_colour);

/* A saved network's page: its actions, the one selected, and how far a hold to confirm has gone (0..100). */
enum { ACTION_TEST, ACTION_USE, ACTION_PASSWORD, ACTION_DELETE };
void screen_network(const struct wifi_list *l, unsigned index, unsigned board, const uint8_t *actions, unsigned count,
                    unsigned selected, unsigned held_percent, const char *note, uint16_t note_colour);
void screen_network_hold(const uint8_t *actions, unsigned selected, unsigned held_percent);

/* Listening for networks, then what was heard: the networks, then "Type a name", then "Back". */
void screen_scanning(unsigned done, unsigned total);
unsigned screen_heard_items(unsigned count);
void screen_heard(const struct net_ap *aps, unsigned count, const struct wifi_list *l, unsigned board, unsigned selection);

/* Typing a name or a password: the title, the network's name (or 0), the text so far, what's shown when it's empty,
 * the layer, the piano keys held, the cursor's blink, and a note (in red) or 0. */
void screen_type(unsigned board, const char *title, const char *network, const char *text, const char *placeholder,
                 unsigned layer, uint32_t held, bool cursor, const char *note);

/* A test: the network, the step it's on (NET_STEPS when done), and then why it failed, or 0 if it joined. */
void screen_test(unsigned board, const char *ssid, unsigned step, bool done, const char *why, const struct net_result *r,
                 const char *bar_top, uint16_t bar_colour, const char *bar_keys);
#endif
