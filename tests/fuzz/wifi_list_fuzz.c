/* SPDX-License-Identifier: MIT */
/* libFuzzer: apps/wifi's WIFI.TXT reader and writer on any text. Reading must stay inside it; writing it back, after a
 * change the input picks, must give a file that reads as the list it was written from. */
#include "wifilist.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* b, read back, is a. Or, when a file held more networks than fit (full), a first: a network removed makes room for
 * one that didn't fit before. */
static void same_list(const struct wifi_list *a, const struct wifi_list *b, int full) {
    assert(full ? b->count >= a->count : b->count == a->count);
    for (unsigned i = 0; i < a->count; ++i)
        assert(!strcmp(a->net[i].ssid, b->net[i].ssid) && !strcmp(a->net[i].pass, b->net[i].pass));
    if (a->count)
        assert(!strcmp(a->net[a->use].ssid, b->net[b->use].ssid));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    if (n > WIFI_TEXT_MOST)
        return 0;
    char *text = malloc(n ? n : 1);
    memcpy(text, data, n);
    static struct wifi_list l, again;
    wifi_list_read(&l, text, (unsigned)n);
    assert(l.count <= WIFI_MOST && (!l.count || l.use < l.count));
    int full = l.count == WIFI_MOST && l.skipped;
    /* A change, from the last byte: none, remove one, put one (new, or an existing one's password). */
    unsigned pick = n ? (unsigned char)data[n-1] : 0;
    if (pick%3 == 1 && l.count)
        wifi_list_remove(&l, pick/3%l.count);
    else if (pick%3 == 2)
        wifi_list_put(&l, l.count && pick&8 ? l.net[pick/3%l.count].ssid : "fuzz net", pick&4 ? "" : "fuzz-pass-1");
    static char out[WIFI_TEXT_MOST*2];
    unsigned len;
    if (wifi_list_write(&l, text, (unsigned)n, out, sizeof out, &len)) {
        wifi_list_read(&again, out, len);
        same_list(&l, &again, full);
    }
    free(text);
    return 0;
}
