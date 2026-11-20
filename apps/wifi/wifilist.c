/* SPDX-License-Identifier: MIT */
#include "wifilist.h"
#include <string.h>

static const char HEAD[] = "# WiFi networks for the apps on this card. use= is the one they join.\n"
                           "# ssid= starts a network, password= is its password (none for an open one).\n";

static unsigned length(const char *s, unsigned most) {
    unsigned n = 0;
    while (n <= most && s[n])
        ++n;
    return n;
}

static bool same(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && !memcmp(a, b, n);
}

static void set(char *to, const char *from) {
    memcpy(to, from, strlen(from)+1);
}

static bool printable(const char *s) {
    for (; *s; ++s)
        if (*s == '\n' || *s == '\r')
            return false;
    return true;
}

bool wifi_ssid_ok(const char *ssid) {
    unsigned n = length(ssid, WIFI_SSID_MOST);
    return n && n <= WIFI_SSID_MOST && printable(ssid);
}

bool wifi_pass_ok(const char *pass) {
    unsigned n = length(pass, WIFI_PASS_MOST);
    if (!n)
        return true;
    if (n < WIFI_PASS_LEAST || n > WIFI_PASS_MOST)
        return false;
    /* WPA2's passphrase: printable ASCII. */
    for (; *pass; ++pass)
        if (*pass < 32 || *pass > 126)
            return false;
    return true;
}

static bool is(const char *line, unsigned n, const char *name, const char **value, unsigned *value_n) {
    unsigned k = (unsigned)strlen(name);
    if (n < k || memcmp(line, name, k))
        return false;
    *value = line+k;
    *value_n = n-k;
    return true;
}

static bool copy(char *to, unsigned most, const char *from, unsigned n) {
    if (n > most)
        return false;
    memcpy(to, from, n);
    to[n] = 0;
    return true;
}

void wifi_list_read(struct wifi_list *l, const char *text, unsigned n) {
    memset(l, 0, sizeof *l);
    char use[WIFI_SSID_MOST+1] = {0};
    bool have_use = false;
    /* The network being read, and whether it's being kept. */
    struct wifi_net *net = 0;
    bool bad = false;
    unsigned at = 0;
    if (n >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf)
        at = 3;
    while (at < n) {
        unsigned end = at;
        while (end < n && text[end] != '\n')
            ++end;
        const char *line = text+at;
        unsigned len = end-at;
        if (len && line[len-1] == '\r')
            --len;
        at = end+1;
        const char *value;
        unsigned value_n;
        if (!len || line[0] == '#')
            continue;
        if (is(line, len, "ssid=", &value, &value_n)) {
            if (net && bad)
                --l->count, ++l->skipped;
            net = 0;
            bad = false;
            if (l->count == WIFI_MOST) {
                ++l->skipped;
                bad = true;
                continue;
            }
            net = &l->net[l->count++];
            memset(net, 0, sizeof *net);
            bad = !copy(net->ssid, WIFI_SSID_MOST, value, value_n) || !wifi_ssid_ok(net->ssid);
        } else if (is(line, len, "password=", &value, &value_n)) {
            if (net && !bad)
                bad = !copy(net->pass, WIFI_PASS_MOST, value, value_n) || !wifi_pass_ok(net->pass);
        } else if (is(line, len, "use=", &value, &value_n)) {
            have_use = copy(use, WIFI_SSID_MOST, value, value_n);
        }
    }
    if (net && bad)
        --l->count, ++l->skipped;
    /* A name twice: the first counts. */
    for (unsigned i = 1; i < l->count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (same(l->net[i].ssid, l->net[j].ssid)) {
                wifi_list_remove(l, i--);
                ++l->skipped;
                break;
            }
    int found = have_use ? wifi_list_find(l, use) : -1;
    l->use = found > 0 ? (unsigned)found : 0;
}

static bool put(char *out, unsigned most, unsigned *n, const char *s) {
    unsigned k = (unsigned)strlen(s);
    if (*n+k > most)
        return false;
    memcpy(out+*n, s, k);
    *n += k;
    return true;
}

unsigned wifi_list_write(const struct wifi_list *l, char *out, unsigned most) {
    unsigned n = 0;
    bool ok = put(out, most, &n, HEAD);
    if (l->count) {
        ok = ok && put(out, most, &n, "use=") && put(out, most, &n, l->net[l->use < l->count ? l->use : 0].ssid)
             && put(out, most, &n, "\n");
    }
    for (unsigned i = 0; i < l->count; ++i)
        ok = ok && put(out, most, &n, "\nssid=") && put(out, most, &n, l->net[i].ssid) && put(out, most, &n, "\npassword=")
             && put(out, most, &n, l->net[i].pass) && put(out, most, &n, "\n");
    return ok ? n : 0;
}

int wifi_list_find(const struct wifi_list *l, const char *ssid) {
    for (unsigned i = 0; i < l->count; ++i)
        if (same(l->net[i].ssid, ssid))
            return (int)i;
    return -1;
}

int wifi_list_put(struct wifi_list *l, const char *ssid, const char *pass) {
    if (!wifi_ssid_ok(ssid) || !wifi_pass_ok(pass))
        return -1;
    int i = wifi_list_find(l, ssid);
    if (i < 0) {
        if (l->count == WIFI_MOST)
            return -1;
        i = (int)l->count++;
        set(l->net[i].ssid, ssid);
    }
    set(l->net[i].pass, pass);
    l->use = (unsigned)i;
    return i;
}

void wifi_list_remove(struct wifi_list *l, unsigned i) {
    if (i >= l->count)
        return;
    memmove(&l->net[i], &l->net[i+1], (l->count-i-1)*sizeof l->net[0]);
    --l->count;
    /* With the one to use gone, it's the first. */
    if (l->use == i || l->use > l->count)
        l->use = 0;
    else if (l->use > i)
        --l->use;
}
