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

/* ---- the text, a line at a time ---- */

struct line {
    const char *s;
    unsigned len;               /* without its line end */
    unsigned whole;             /* with it: LF, CRLF, or none on a last line */
};

/* The line starting at *at, and *at moved past it; false at the end. */
static bool next_line(const char *text, unsigned n, unsigned *at, struct line *l) {
    if (*at >= n)
        return false;
    unsigned end = *at;
    while (end < n && text[end] != '\n')
        ++end;
    l->s = text+*at;
    l->len = end-*at;
    l->whole = (end < n ? end+1 : end)-*at;
    if (l->len && l->s[l->len-1] == '\r')
        --l->len;
    *at = end < n ? end+1 : n;
    return true;
}

static unsigned bom(const char *text, unsigned n) {
    return n >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf ? 3 : 0;
}

static bool skipped_line(const struct line *l) {
    return !l->len || l->s[0] == '#';
}

static bool is(const struct line *l, const char *name, const char **value, unsigned *value_n) {
    unsigned k = (unsigned)strlen(name);
    if (l->len < k || memcmp(l->s, name, k))
        return false;
    *value = l->s+k;
    *value_n = l->len-k;
    return true;
}

/* A value as a C string: false if it's too long, or has a zero byte, which a C string can't hold. */
static bool copy(char *to, unsigned most, const char *from, unsigned n) {
    if (n > most)
        return false;
    for (unsigned i = 0; i < n; ++i)
        if (!from[i])
            return false;
    memcpy(to, from, n);
    to[n] = 0;
    return true;
}

/* ---- the list ---- */

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
        if (same(l->removed[i].ssid, ssid))
            return (int)i;
    return -1;
}

void wifi_list_read(struct wifi_list *l, const char *text, unsigned n) {
    memset(l, 0, sizeof *l);
    char use[WIFI_SSID_MOST+1] = {0};
    bool have_use = false;
    /* The network being read (0 while skipping one), and whether its password can't be. */
    struct wifi_net *net = 0;
    bool bad = false;
    unsigned at = bom(text, n), start = at;
    struct line line;
    for (; next_line(text, n, &at, &line); start = at) {
        const char *value;
        unsigned value_n;
        if (skipped_line(&line))
            continue;
        if (is(&line, "ssid=", &value, &value_n)) {
            if (net && bad)
                --l->count, ++l->skipped;
            net = 0;
            bad = false;
            /* A name that can't be, one already read (the first that can be joined counts), or one too many. */
            char name[WIFI_SSID_MOST+1];
            if (!copy(name, WIFI_SSID_MOST, value, value_n) || !wifi_ssid_ok(name) || wifi_list_find(l, name) >= 0 ||
                l->count == WIFI_MOST) {
                ++l->skipped;
                continue;
            }
            net = &l->net[l->count++];
            memset(net, 0, sizeof *net);
            set(net->ssid, name);
            net->in_file = true;
            net->at = start;
        } else if (is(&line, "password=", &value, &value_n)) {
            if (net && !bad)
                bad = !copy(net->pass, WIFI_PASS_MOST, value, value_n) || !wifi_pass_ok(net->pass);
        } else if (is(&line, "use=", &value, &value_n)) {
            have_use = copy(use, WIFI_SSID_MOST, value, value_n);
        }
    }
    if (net && bad)
        --l->count, ++l->skipped;
    int found = have_use ? wifi_list_find(l, use) : -1;
    l->use = found > 0 ? (unsigned)found : 0;
}

/* ---- writing ---- */

struct out {
    char *s;
    unsigned n, most;
    bool ok;
};

static void put_n(struct out *o, const char *s, unsigned k) {
    if (!o->ok || o->n+k > o->most) {
        o->ok = false;
        return;
    }
    memcpy(o->s+o->n, s, k);
    o->n += k;
}

static void put(struct out *o, const char *s) {
    put_n(o, s, (unsigned)strlen(s));
}

/* name and value, then end, the line end. */
static void put_setting(struct out *o, const char *name, const char *value, const char *end, unsigned end_n) {
    put(o, name);
    put(o, value);
    put_n(o, end, end_n);
}

/* A line as it was. One that starts with a byte order mark is plain text where it is, but would be skipped as a mark at
 * the start of the file: one that wasn't first, but would be now, gets a line end before it. */
static void put_line(struct out *o, const struct line *l, const char *text, const char *nl) {
    if (!o->n && l->s != text && bom(l->s, l->len))
        put(o, nl);
    put_n(o, l->s, l->whole);
}

/* Whether the network whose ssid= ends at at has a password= among its lines. */
static bool has_password(const char *text, unsigned n, unsigned at) {
    struct line line;
    const char *value;
    unsigned value_n;
    while (next_line(text, n, &at, &line)) {
        if (is(&line, "ssid=", &value, &value_n))
            return false;
        if (is(&line, "password=", &value, &value_n))
            return true;
    }
    return false;
}

bool wifi_list_write(const struct wifi_list *l, const char *old, unsigned old_n, char *to, unsigned most, unsigned *n) {
    struct out o = {to, 0, most, true};
    const char *use = l->count ? l->net[l->use < l->count ? l->use : 0].ssid : 0;
    /* For lines added: the file's own line end, from its first line. */
    const char *nl = "\n";
    for (unsigned i = 0; i < old_n; ++i)
        if (old[i] == '\n') {
            if (i && old[i-1] == '\r')
                nl = "\r\n";
            break;
        }
    unsigned nl_n = (unsigned)strlen(nl);
    if (!old_n)
        put(&o, HEAD);
    unsigned at = bom(old, old_n);
    put_n(&o, old, at);
    /* A use= anywhere is set where it is; with none, one goes before the first network. */
    bool had_use = false, use_added = false;
    struct line line;
    const char *value;
    unsigned value_n;
    for (unsigned i = at; next_line(old, old_n, &i, &line); )
        had_use |= is(&line, "use=", &value, &value_n);
    /* Whose lines these are: a network kept as it is, ours (its password rewritten), or one removed. */
    enum { KEEP, OURS, DROP } mode = KEEP;
    int ours = -1;
    bool written[WIFI_MOST] = {false};
    /* Where the blank line just copied began, and where it ended: a removed network takes it too, so adding and removing
     * one leaves the file as it was. */
    unsigned blank_at = 0, after_blank = (unsigned)-1;
    for (unsigned start = at; o.ok && next_line(old, old_n, &at, &line); start = at) {
        /* This line's own end, for a line rewritten in place. */
        const char *end = line.s+line.len;
        unsigned end_n = line.whole-line.len;
        if (skipped_line(&line)) {
            unsigned before = o.n;
            put_line(&o, &line, old, nl);
            if (!line.len)
                blank_at = before, after_blank = o.n;
        } else if (is(&line, "ssid=", &value, &value_n)) {
            if (use && !had_use && !use_added) {
                put_setting(&o, "use=", use, nl, nl_n);
                put(&o, nl);
                use_added = true;
            }
            char name[WIFI_SSID_MOST+1];
            bool named = copy(name, WIFI_SSID_MOST, value, value_n);
            int i = named ? wifi_list_find(l, name) : -1;
            /* The copy that was read, if it was; a new one's first copy. */
            if (i >= 0 && !written[i] && (!l->net[i].in_file || l->net[i].at == start)) {
                mode = OURS;
                ours = i;
                written[i] = true;
                /* Its name as it is; a last line with no end gets one, as its password may follow. */
                put_n(&o, line.s, line.len);
                if (end_n)
                    put_n(&o, end, end_n);
                else
                    put(&o, nl);
                /* A password the file didn't have goes just after its name. */
                if (l->net[i].pass[0] && !has_password(old, old_n, at))
                    put_setting(&o, "password=", l->net[i].pass, nl, nl_n);
            } else if (i < 0 && named && removed_at(l, name) >= 0) {
                /* Removed: every network of that name goes. */
                mode = DROP;
                if (o.n == after_blank)
                    o.n = blank_at;
            } else {
                mode = KEEP;
                put_line(&o, &line, old, nl);
            }
        } else if (is(&line, "password=", &value, &value_n) && mode != KEEP) {
            if (mode == OURS)
                put_setting(&o, "password=", l->net[ours].pass, end, end_n);
        } else if (is(&line, "use=", &value, &value_n)) {
            /* With no networks left, use= names none: it goes. */
            if (use)
                put_setting(&o, "use=", use, end, end_n);
        } else if (mode != DROP) {
            put_line(&o, &line, old, nl);
        }
    }
    /* What's added starts on a line of its own. */
    bool add_use = use && !had_use && !use_added, adding = add_use;
    for (unsigned i = 0; i < l->count; ++i)
        adding |= !written[i];
    if (adding && o.n && o.s[o.n-1] != '\n')
        put(&o, nl);
    if (add_use)
        put_setting(&o, "use=", use, nl, nl_n);
    for (unsigned i = 0; i < l->count; ++i)
        if (!written[i]) {
            if (o.n)
                put(&o, nl);
            put_setting(&o, "ssid=", l->net[i].ssid, nl, nl_n);
            put_setting(&o, "password=", l->net[i].pass, nl, nl_n);
        }
    *n = o.n;
    return o.ok;
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
            l->net[i].in_file = true;
            l->net[i].at = l->removed[r].at;
            memmove(&l->removed[r], &l->removed[r+1], (l->removed_count-(unsigned)r-1)*sizeof l->removed[0]);
            --l->removed_count;
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
    if (l->net[i].in_file && l->removed_count < WIFI_MOST) {
        set(l->removed[l->removed_count].ssid, l->net[i].ssid);
        l->removed[l->removed_count++].at = l->net[i].at;
    }
    drop(l, i);
}
