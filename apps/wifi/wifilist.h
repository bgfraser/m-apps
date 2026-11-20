/* SPDX-License-Identifier: MIT */
/* The WiFi networks an app may join, in /MBOOT/WIFI.TXT, a text file to edit on a computer or with the WiFi app:
 *
 *     # WiFi networks for the apps on this card. use= is the one they join.
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
 * A name is 1 to 32 bytes and a password 8 to 63 characters (WPA2-PSK), or none for an open network. Pure data, so it
 * builds on a computer too (tests/wifi_list_test.py). */
#ifndef WIFILIST_H
#define WIFILIST_H
#include <stdbool.h>

#define WIFI_SSID_MOST 32u
#define WIFI_PASS_LEAST 8u
#define WIFI_PASS_MOST 63u
#define WIFI_MOST 12u
/* What a file this module writes can take, at most: room for the comment, use= and WIFI_MOST networks. */
#define WIFI_TEXT_MOST 2048u

struct wifi_net {
    char ssid[WIFI_SSID_MOST+1];
    char pass[WIFI_PASS_MOST+1];
};
struct wifi_list {
    struct wifi_net net[WIFI_MOST];
    unsigned count;
    unsigned use;               /* which one apps join; 0 with none */
    unsigned skipped;           /* networks in the file that weren't read: too many, or a name or password that can't be */
};

/* Reads a file's text, n bytes, which needn't end in a zero. */
void wifi_list_read(struct wifi_list *l, const char *text, unsigned n);
/* The file's text for the list, in out with room for most: its length, or 0 if it doesn't fit. */
unsigned wifi_list_write(const struct wifi_list *l, char *out, unsigned most);
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
