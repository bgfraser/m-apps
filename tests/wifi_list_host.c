/* SPDX-License-Identifier: MIT */
/* apps/wifi/wifilist.c on a computer: reading WIFI.TXT as people write it, and writing it back. */
#include "wifilist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failed = 1; } } while (0)

static void read_text(struct wifi_list *l, const char *t) { wifi_list_read(l, t, (unsigned)strlen(t)); }

int main(void) {
    struct wifi_list l;
    /* As a person writes it: CRLF, a BOM, comments, spaces and = kept in values, use= after the networks. */
    read_text(&l, "\xef\xbb\xbf# mine\r\n\r\nssid=Home Net\r\npassword=a = b c d\r\n\r\nssid=Shed\r\npassword=12345678\r\nuse=Shed\r\n");
    CHECK(l.count == 2 && l.use == 1 && !l.skipped);
    CHECK(!strcmp(l.net[0].ssid, "Home Net") && !strcmp(l.net[0].pass, "a = b c d"));
    CHECK(!strcmp(l.net[1].pass, "12345678"));
    /* No use=, or one naming nothing: the first. An open network. No newline at the end. */
    read_text(&l, "ssid=Cafe\nssid=Two\npassword=abcdefgh\nuse=Nowhere");
    CHECK(l.count == 2 && l.use == 0 && !l.net[0].pass[0]);
    /* What can't be joined is skipped and counted: a short password, a long name, a password before any name, a name
     * twice, an empty name. */
    read_text(&l, "password=orphaned1\nssid=Short\npassword=1234567\nssid=0123456789012345678901234567890123\n"
                  "ssid=Ok\npassword=abcdefgh\nssid=Ok\npassword=zzzzzzzz\nssid=\nunknown=thing\n");
    CHECK(l.count == 1 && l.skipped == 4 && !strcmp(l.net[0].pass, "abcdefgh"));
    /* Too many. */
    char big[4096];
    unsigned n = 0;
    for (unsigned i = 0; i < WIFI_MOST+3; ++i)
        n += (unsigned)sprintf(big+n, "ssid=net%u\npassword=password%u\n", i, i);
    wifi_list_read(&l, big, n);
    CHECK(l.count == WIFI_MOST && l.skipped == 3);
    /* Writing it back and reading it again gives the same list, with the longest names and passwords too. */
    memset(&l, 0, sizeof l);
    char ssid[WIFI_SSID_MOST+1], pass[WIFI_PASS_MOST+1];
    for (unsigned i = 0; i < WIFI_MOST; ++i) {
        memset(ssid, 'a'+i, WIFI_SSID_MOST);
        ssid[WIFI_SSID_MOST] = 0;
        memset(pass, '!'+i, WIFI_PASS_MOST);
        pass[WIFI_PASS_MOST] = 0;
        CHECK(wifi_list_put(&l, ssid, pass) == (int)i);
    }
    CHECK(wifi_list_put(&l, "one more", "abcdefgh") == -1);
    l.use = 5;
    char text[WIFI_TEXT_MOST];
    unsigned len = wifi_list_write(&l, text, sizeof text);
    CHECK(len > 0);
    struct wifi_list again;
    wifi_list_read(&again, text, len);
    CHECK(again.count == l.count && again.use == 5 && !again.skipped && !memcmp(again.net, l.net, sizeof l.net));
    CHECK(!wifi_list_write(&l, text, 100));
    /* Put changes a password in place and uses it; remove keeps use on the same network, or the first. */
    memset(&l, 0, sizeof l);
    wifi_list_put(&l, "A", "aaaaaaaa");
    wifi_list_put(&l, "B", "bbbbbbbb");
    wifi_list_put(&l, "C", "");
    CHECK(l.count == 3 && l.use == 2);
    CHECK(wifi_list_put(&l, "A", "AAAAAAAA") == 0 && l.use == 0 && !strcmp(l.net[0].pass, "AAAAAAAA"));
    l.use = 2;
    wifi_list_remove(&l, 0);
    CHECK(l.count == 2 && l.use == 1 && !strcmp(l.net[l.use].ssid, "C"));
    wifi_list_remove(&l, 1);
    CHECK(l.count == 1 && l.use == 0);
    wifi_list_remove(&l, 0);
    CHECK(l.count == 0 && l.use == 0);
    len = wifi_list_write(&l, text, sizeof text);
    CHECK(len && !memmem(text, len, "\nuse=", 5));
    /* Names and passwords that can't be. */
    CHECK(!wifi_ssid_ok("") && !wifi_ssid_ok("a\nb") && wifi_ssid_ok("caf\xc3\xa9"));
    CHECK(wifi_pass_ok("") && !wifi_pass_ok("1234567") && !wifi_pass_ok("caf\xc3\xa9 1234") && wifi_pass_ok("~!@#$%^&*()"));
    puts(failed ? "FAILED" : "wifi list OK");
    return failed;
}
