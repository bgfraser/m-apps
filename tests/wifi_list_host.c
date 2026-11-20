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
    unsigned len = wifi_list_write(&l, 0, 0, text, sizeof text);
    CHECK(len > 0);
    struct wifi_list again;
    wifi_list_read(&again, text, len);
    CHECK(again.count == l.count && again.use == 5 && !again.skipped);
    for (unsigned i = 0; i < l.count; ++i)
        CHECK(!strcmp(again.net[i].ssid, l.net[i].ssid) && !strcmp(again.net[i].pass, l.net[i].pass));
    CHECK(!wifi_list_write(&l, 0, 0, text, 100));
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
    len = wifi_list_write(&l, 0, 0, text, sizeof text);
    CHECK(len && !memmem(text, len, "\nuse=", 5));
    /* Changing a file keeps what it doesn't know: a byte order mark, CRLF, comments, blank lines, a setting it doesn't
     * know, a network it skipped (a short password), and where use= is. A password changes in place, a removed
     * network's settings go and its comment stays, a password the file didn't have goes at the end of its settings,
     * and a new network goes at the end. */
    const char *mine = "\xef\xbb\xbf# my networks\r\n\r\n# at home\r\nssid=Home\r\npassword=old-home-pw\r\nband=5\r\n\r\n"
                       "# the shed\r\nssid=Shed\r\npassword=shed-pw-1\r\nhidden=1\r\n\r\nssid=Short\r\npassword=1234\r\n\r\n"
                       "ssid=Cafe\r\n\r\nuse=Shed\r\n";
    read_text(&l, mine);
    CHECK(l.count == 3 && l.skipped == 1 && l.use == 1);
    CHECK(wifi_list_put(&l, "Home", "new-home-pw") == 0);
    wifi_list_remove(&l, (unsigned)wifi_list_find(&l, "Shed"));
    CHECK(wifi_list_put(&l, "Cafe", "cafe-pw-22") == 1);
    CHECK(wifi_list_put(&l, "New", "") == 2);
    len = wifi_list_write(&l, mine, (unsigned)strlen(mine), text, sizeof text);
    const char *want = "\xef\xbb\xbf# my networks\r\n\r\n# at home\r\nssid=Home\r\npassword=new-home-pw\r\nband=5\r\n\r\n"
                       "# the shed\r\n\r\nssid=Short\r\npassword=1234\r\n\r\nssid=Cafe\r\npassword=cafe-pw-22\r\n\r\n"
                       "use=New\r\n\r\nssid=New\r\npassword=\r\n";
    CHECK(len == strlen(want) && !memcmp(text, want, len));
    if (len != strlen(want) || memcmp(text, want, len))
        printf("got:\n%.*s\n", (int)len, text);
    /* Removed and put back: still where it was, with its new password. With no use= in the file, one goes before the
     * first network. A last line with no line end gets one. */
    const char *plain = "# x\nssid=A\npassword=aaaaaaaa\nssid=B\npassword=bbbbbbbb";
    read_text(&l, plain);
    wifi_list_remove(&l, 0);
    CHECK(wifi_list_put(&l, "A", "AAAAAAAA") == 1);
    CHECK(wifi_list_put(&l, "C", "cccccccc") == 2);
    len = wifi_list_write(&l, plain, (unsigned)strlen(plain), text, sizeof text);
    want = "# x\nuse=C\n\nssid=A\npassword=AAAAAAAA\nssid=B\npassword=bbbbbbbb\n\nssid=C\npassword=cccccccc\n";
    CHECK(len == strlen(want) && !memcmp(text, want, len));
    if (len != strlen(want) || memcmp(text, want, len))
        printf("got:\n%.*s\n", (int)len, text);
    /* Read back, it's the same list. */
    wifi_list_read(&again, text, len);
    CHECK(again.count == 3 && again.use == 2 && !strcmp(again.net[0].pass, "AAAAAAAA"));
    /* Adding a network and removing it again leaves the file as it was, but for use=. */
    const char *two = "use=One\n\nssid=One\npassword=11111111\n\nssid=Two\npassword=22222222\n";
    read_text(&l, two);
    char first[WIFI_TEXT_MOST];
    wifi_list_put(&l, "Three", "33333333");
    l.use = 0;
    unsigned first_n = wifi_list_write(&l, two, (unsigned)strlen(two), first, sizeof first);
    read_text(&l, "");
    wifi_list_read(&l, first, first_n);
    CHECK(l.count == 3 && l.net[2].in_file);
    wifi_list_remove(&l, 2);
    len = wifi_list_write(&l, first, first_n, text, sizeof text);
    CHECK(len == strlen(two) && !memcmp(text, two, len));
    if (len != strlen(two) || memcmp(text, two, len))
        printf("got:\n%.*s\n", (int)len, text);
    /* A file too big for out isn't cut short. */
    CHECK(!wifi_list_write(&l, plain, (unsigned)strlen(plain), text, 20));
    /* Names and passwords that can't be. */
    CHECK(!wifi_ssid_ok("") && !wifi_ssid_ok("a\nb") && wifi_ssid_ok("caf\xc3\xa9"));
    CHECK(wifi_pass_ok("") && !wifi_pass_ok("1234567") && !wifi_pass_ok("caf\xc3\xa9 1234") && wifi_pass_ok("~!@#$%^&*()"));
    puts(failed ? "FAILED" : "wifi list OK");
    return failed;
}
