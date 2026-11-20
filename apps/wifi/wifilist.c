/* SPDX-License-Identifier: MIT */
#include "wifilist.h"
#include <string.h>

static const char HEAD[] = "# WiFi networks on this card. use= is the one to join.\n"
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

/* Takes network i out of the list, keeping use on the same network, or the first. */
static void drop(struct wifi_list *l, unsigned i) {
    memmove(&l->net[i], &l->net[i+1], (l->count-i-1)*sizeof l->net[0]);
    --l->count;
    if (l->use == i || l->use > l->count)
        l->use = 0;
    else if (l->use > i)
        --l->use;
}

static int removed_at(const struct wifi_list *l, const char *ssid) {
    for (unsigned i = 0; i < l->removed_count; ++i)
        if (same(l->removed[i], ssid))
            return (int)i;
    return -1;
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
            net->in_file = true;
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
                drop(l, i--);
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

static bool put_n(char *out, unsigned most, unsigned *n, const char *s, unsigned k) {
    if (*n+k > most)
        return false;
    memcpy(out+*n, s, k);
    *n += k;
    return true;
}

/* A line: name, its value and the line end. */
static bool put_line(char *out, unsigned most, unsigned *n, const char *name, const char *value, const char *nl) {
    return put(out, most, n, name) && put(out, most, n, value) && put(out, most, n, nl);
}

unsigned wifi_list_write(const struct wifi_list *l, const char *old, unsigned old_n, char *out, unsigned most) {
    unsigned n = 0, at = 0;
    bool ok = true, written[WIFI_MOST] = {false}, use_set = false;
    const char *use = l->count ? l->net[l->use < l->count ? l->use : 0].ssid : 0;
    /* The file's line end, from its first line. */
    const char *nl = "\n";
    for (unsigned i = 0; i < old_n; ++i)
        if (old[i] == '\n') {
            if (i && old[i-1] == '\r')
                nl = "\r\n";
            break;
        }
    if (!old_n)
        ok = put(out, most, &n, HEAD);
    /* A use= anywhere is set where it is; with none, one goes before the first network. */
    for (unsigned i = 0; i < old_n && !use_set; ) {
        unsigned e = i;
        while (e < old_n && old[e] != '\n')
            ++e;
        const char *value;
        unsigned value_n;
        if (is(old+i, e-i, "use=", &value, &value_n))
            use_set = true;
        i = e+1;
    }
    bool had_use = use_set;
    use_set = false;
    if (old_n >= 3 && (unsigned char)old[0] == 0xef && (unsigned char)old[1] == 0xbb && (unsigned char)old[2] == 0xbf)
        ok = put_n(out, most, &n, old, 3), at = 3;
    /* The network whose lines these are: keep them as they are, rewrite its password (pending: none written yet), or
     * drop them. */
    enum { KEEP, OURS, DROP } mode = KEEP;
    int ours = -1;
    bool pending = false;
    /* Where the blank line just copied began, and where it ended: a removed network takes it too, so adding and removing
     * one leaves the file as it was. */
    unsigned blank_at = 0, after_blank = (unsigned)-1;
    while (ok && at < old_n) {
        unsigned end = at;
        while (end < old_n && old[end] != '\n')
            ++end;
        const char *line = old+at;
        unsigned len = end-at, whole = end < old_n ? end-at+1 : end-at;
        if (len && line[len-1] == '\r')
            --len;
        at = end+1;
        const char *value;
        unsigned value_n;
        bool ssid = is(line, len, "ssid=", &value, &value_n);
        /* A network's password with none in the file goes at the end of its settings. */
        if (pending && (!len || line[0] == '#' || ssid)) {
            ok = put_line(out, most, &n, "password=", l->net[ours].pass, nl);
            pending = false;
        }
        if (!len || line[0] == '#') {
            unsigned before = n;
            ok = ok && put_n(out, most, &n, line, whole);
            if (!len)
                blank_at = before, after_blank = n;
        } else if (ssid) {
            if (use && !use_set && !had_use) {
                ok = ok && put_line(out, most, &n, "use=", use, nl) && put(out, most, &n, nl);
                use_set = true;
            }
            char name[WIFI_SSID_MOST+1];
            int i = copy(name, WIFI_SSID_MOST, value, value_n) ? wifi_list_find(l, name) : -1;
            if (i >= 0 && !written[i]) {
                mode = OURS;
                ours = i;
                written[i] = true;
                pending = l->net[i].pass[0] != 0;
                ok = ok && put_n(out, most, &n, line, len) && put(out, most, &n, nl);
            } else if (i < 0 && value_n <= WIFI_SSID_MOST && removed_at(l, name) >= 0) {
                mode = DROP;
                if (n == after_blank)
                    n = blank_at;
            } else {
                mode = KEEP;
                ok = ok && put_n(out, most, &n, line, whole);
            }
        } else if (is(line, len, "password=", &value, &value_n) && mode != KEEP) {
            if (mode == OURS) {
                ok = ok && put_line(out, most, &n, "password=", l->net[ours].pass, nl);
                pending = false;
            }
        } else if (is(line, len, "use=", &value, &value_n)) {
            if (use)
                ok = ok && put_line(out, most, &n, "use=", use, nl);
            use_set = true;
        } else if (mode != DROP) {
            ok = ok && put_n(out, most, &n, line, whole);
        }
        /* A last line with no end gets one, so what's added after starts on a line of its own. */
        if (ok && at > old_n && n && out[n-1] != '\n')
            ok = put(out, most, &n, nl);
    }
    if (pending)
        ok = ok && put_line(out, most, &n, "password=", l->net[ours].pass, nl);
    if (use && !use_set && !had_use)
        ok = ok && put_line(out, most, &n, "use=", use, nl);
    for (unsigned i = 0; i < l->count; ++i)
        if (!written[i])
            ok = ok && put(out, most, &n, nl) && put_line(out, most, &n, "ssid=", l->net[i].ssid, nl)
                 && put_line(out, most, &n, "password=", l->net[i].pass, nl);
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
        memset(&l->net[i], 0, sizeof l->net[i]);
        set(l->net[i].ssid, ssid);
        /* Removed and put back: it's still in the file, where its password is changed. */
        int r = removed_at(l, ssid);
        if (r >= 0) {
            memmove(l->removed[r], l->removed[r+1], (l->removed_count-(unsigned)r-1)*sizeof l->removed[0]);
            --l->removed_count;
            l->net[i].in_file = true;
        }
    }
    set(l->net[i].pass, pass);
    l->use = (unsigned)i;
    return i;
}

void wifi_list_remove(struct wifi_list *l, unsigned i) {
    if (i >= l->count)
        return;
    /* At most WIFI_MOST were read, and each is recorded once. */
    if (l->net[i].in_file && l->removed_count < WIFI_MOST)
        set(l->removed[l->removed_count++], l->net[i].ssid);
    drop(l, i);
}
