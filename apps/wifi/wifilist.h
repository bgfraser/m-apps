/* SPDX-License-Identifier: MIT */
/* The WiFi networks to join, in /WIFI.TXT at the card's root, for any firmware or app on the unit: a text file to edit
 * on a computer or with the WiFi app. docs/WIFI-TXT.md is the format:
 *
 *     # WiFi networks on this card. use= is the one to join.
 *     use=Home
 *
 *     ssid=Home
 *     password=correct horse
 *
 *     ssid=Workshop
 *     password=shed-key-2041
 *
 * One setting a line, as name=value: the value is everything after the first =, spaces and all, so a name or
 * password may have = and spaces in it. ssid= starts a network and password= is its password. use= names the network
 * to join; without it, or naming none of them, it's the first. Lines starting with # and blank lines are skipped, and
 * so are settings it doesn't know. Line ends may be LF or CRLF, and a UTF-8 byte order mark at the start is skipped.
 *
 * A name is 1 to 32 bytes and a password 8 to 63 characters (WPA2-PSK), or none for an open network. A name given
 * twice: the first that can be joined counts. Pure data, so it builds on a computer too (tests/wifi_list_test.py). */
#ifndef WIFILIST_H
#define WIFILIST_H
#include <stdbool.h>

#define WIFI_SSID_MOST 32u
#define WIFI_PASS_LEAST 8u
#define WIFI_PASS_MOST 63u
#define WIFI_MOST 12u
/* The largest file this module reads and changes. */
#define WIFI_TEXT_MOST 4096u

struct wifi_net {
    char ssid[WIFI_SSID_MOST+1];
    char pass[WIFI_PASS_MOST+1];
    bool in_file;               /* read from the file, so removing it takes it out of the file */
};
struct wifi_list {
    struct wifi_net net[WIFI_MOST];
    unsigned count;
    unsigned use;               /* the one to join; 0 with none */
    unsigned skipped;           /* networks in the file that weren't read: too many, or a name or password that can't be */
    /* The names removed since it was read, so that writing drops them from the file and nothing else. */
    char removed[WIFI_MOST][WIFI_SSID_MOST+1];
    unsigned removed_count;
};

/* Reads a file's text, n bytes, which needn't end in a zero. */
void wifi_list_read(struct wifi_list *l, const char *text, unsigned n);
/* The file's text for the list, in to with room for most bytes: false if it doesn't fit, and otherwise its length in *n,
 * which may be 0. It's the old text, old_n bytes (or none, 0 and 0), changed as little as can be (docs/WIFI-TXT.md):
 * comments, blank lines, settings it doesn't know and networks it skipped stay as they were, with their line ends. A
 * network's password= is rewritten in place, or added just after its ssid=; a removed one's settings go, in every
 * network of its name, with a blank line just before it (other blank lines and comments stay); use= is set where it is
 * (or added before the first network); and new networks go at the end. */
bool wifi_list_write(const struct wifi_list *l, const char *old, unsigned old_n, char *to, unsigned most, unsigned *n);
/* Whether a name, or a password, can go in the file and be joined: no line ends, and the lengths above. */
bool wifi_ssid_ok(const char *ssid);
bool wifi_pass_ok(const char *pass);
/* Which network has this name, or -1. */
int wifi_list_find(const struct wifi_list *l, const char *ssid);
/* Adds a network, or changes the password of the one with its name, and makes it the one to use. Its index, or -1 if
 * the name or password can't be or the list is full. */
int wifi_list_put(struct wifi_list *l, const char *ssid, const char *pass);
void wifi_list_remove(struct wifi_list *l, unsigned i);
#endif
