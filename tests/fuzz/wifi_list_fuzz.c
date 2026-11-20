/* SPDX-License-Identifier: MIT */
/* libFuzzer: apps/wifi's WIFI.TXT reader and writer on any text. Reading must stay inside it; writing it back, after
 * changes the input picks, must give a file that reads as the list it was written from. */
#include "wifilist.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CHANGES 4u

/* b, read back, is a, in the same order. When the file held more networks than fit (full), the reader may now take one
 * it didn't before, one changed in place can come back earlier, and a new one, at the end of the file, may be past the
 * 12 it takes (docs/WIFI-TXT.md): each of a's from the file is there, as it was. */
static void same_list(const struct wifi_list *a, const struct wifi_list *b, int full) {
    assert(full || b->count == a->count);
    for (unsigned i = 0; i < a->count; ++i) {
        int j = full ? wifi_list_find(b, a->net[i].ssid) : (int)i;
        if (full && j < 0 && !a->net[i].in_file)
            continue;
        assert(j >= 0 && !strcmp(a->net[i].ssid, b->net[j].ssid) && !strcmp(a->net[i].pass, b->net[j].pass));
    }
    if (a->count && (!full || wifi_list_find(b, a->net[a->use].ssid) >= 0))
        assert(!strcmp(a->net[a->use].ssid, b->net[b->use].ssid));
}

/* The k-th ssid= line's value in text, whether the reader took it or not: names to put that the file has. */
static int nth_name(const char *text, size_t n, unsigned k, char *name) {
    unsigned seen = 0;
    for (size_t at = 0; at < n; ) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            ++end;
        size_t len = end-at;
        if (len && text[end-1] == '\r')
            --len;
        if (len >= 5 && !memcmp(text+at, "ssid=", 5) && len-5 <= WIFI_SSID_MOST && seen++ == k) {
            memcpy(name, text+at+5, len-5);
            name[len-5] = 0;
            return 1;
        }
        at = end+1;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    /* The last CHANGES bytes pick the changes; the rest is the file. */
    if (n < CHANGES || n-CHANGES > WIFI_TEXT_MOST)
        return 0;
    size_t text_n = n-CHANGES;
    char *text = malloc(text_n ? text_n : 1);
    memcpy(text, data, text_n);
    static struct wifi_list l, again;
    wifi_list_read(&l, text, (unsigned)text_n);
    assert(l.count <= WIFI_MOST && (!l.count || l.use < l.count));
    int full = l.count == WIFI_MOST && l.skipped;
    for (unsigned c = 0; c < CHANGES; ++c) {
        unsigned pick = data[text_n+c], k = pick >> 3;
        char name[WIFI_SSID_MOST+1];
        const char *pass = pick&4 ? "" : "fuzz-pass-1";
        switch (pick%6) {
        case 1:                                     /* remove one */
            if (l.count)
                wifi_list_remove(&l, k%l.count);
            break;
        case 2:                                     /* a password changed */
            if (l.count) {
                memcpy(name, l.net[k%l.count].ssid, sizeof name);
                wifi_list_put(&l, name, pass);
            }
            break;
        case 3:                                     /* a new one */
            wifi_list_put(&l, k&1 ? "fuzz net" : "another", pass);
            break;
        case 4:                                     /* a name the file has, taken or skipped */
            if (nth_name(text, text_n, k%16, name))
                wifi_list_put(&l, name, pass);
            break;
        case 5:                                     /* removed, and put back */
            if (l.count) {
                unsigned i = k%l.count;
                memcpy(name, l.net[i].ssid, sizeof name);
                wifi_list_remove(&l, i);
                wifi_list_put(&l, name, pass);
            }
            break;
        }
        assert(l.count <= WIFI_MOST && (!l.count || l.use < l.count));
    }
    static char out[WIFI_TEXT_MOST*2];
    unsigned len;
    if (wifi_list_write(&l, text, (unsigned)text_n, out, sizeof out, &len)) {
        wifi_list_read(&again, out, len);
        same_list(&l, &again, full);
    }
    free(text);
    return 0;
}
