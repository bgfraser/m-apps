/* SPDX-License-Identifier: MIT */
#include "fat.h"
#include "card.h"
#include "core.h"
#include <string.h>
#define SECTOR 512u
#define ENTRY 32u
#define FAT_PER_SECTOR 128u
/* FAT32's limit for a directory: 2 MiB. */
#define MOST_ENTRIES 65536u
#define NO_SECTOR 0xffffffffu
#define READ_ERROR "CARD READ ERROR"
#define NOT_FAT32 "CARD IS NOT FAT32"
#define BAD_FOLDER "BAD FOLDER ON CARD"
#define WRITE_ERROR "CARD WRITE ERROR"
#define BAD_NAME "NAME NOT ALLOWED"
#define CARD_FULL "CARD IS FULL"
#define FSINFO_SIGNATURE 0x41615252u
#define FSINFO_SIGNATURE2 0x61417272u
#define NO_COUNT 0xffffffffu
#define END_OF_CHAIN 0x0fffffffu
/* No clock on the unit: 2026-01-01. */
#define DOS_DATE 0x5c21u
void app_feed(void);
static uint8_t block[SECTOR] __attribute__((aligned(4)));
static uint8_t fat_block[SECTOR] __attribute__((aligned(4)));
static uint32_t fat_at, fat_cached, data_at, clusters, root, fat_sectors, info_sector, first_free;
static unsigned per_cluster, fat_copies;
static bool fat_dirty;
static uint32_t cursor_start, cursor_index, cursor_cluster;
static int freed;
/* Where sectors come from: the card, unless an app gave another volume (fat_source()). */
static bool (*sector_read)(uint32_t sector, uint8_t *data) = card_read;
static bool (*sector_write)(uint32_t sector, const uint8_t *data) = card_write;

void fat_source(bool (*read)(uint32_t sector, uint8_t *data), bool (*write)(uint32_t sector, const uint8_t *data)) {
    sector_read = read ? read : card_read;
    sector_write = write ? write : card_write;
}

static bool looks_fat32(const uint8_t *b) {
    unsigned per = b[13];
    return core_le16(b+510) == 0xaa55 && core_le16(b+11) == SECTOR && per && !(per&(per-1)) && core_le16(b+14)
           && (b[16] == 1 || b[16] == 2) && !core_le16(b+17) && !core_le16(b+19) && !core_le16(b+22) && core_le32(b+36);
}

const char *fat_mount(uint32_t sectors) {
    uint32_t base = 0;
    if (!sector_read(0, block))
        return READ_ERROR;
    if (!looks_fat32(block)) {
        if (core_le16(block+510) != 0xaa55)
            return NOT_FAT32;
        base = core_le32(block+454);
        if (base >= sectors)
            return NOT_FAT32;
        if (!sector_read(base, block))
            return READ_ERROR;
        if (!looks_fat32(block))
            return NOT_FAT32;
    }
    uint32_t total = core_le32(block+32), reserved = core_le16(block+14), fat_size = core_le32(block+36), fats = block[16];
    if (total > sectors-base || fat_size > total/fats || reserved+fats*fat_size >= total)
        return NOT_FAT32;
    per_cluster = block[13];
    fat_at = base+reserved;
    data_at = fat_at+fats*fat_size;
    clusters = (total-reserved-fats*fat_size)/per_cluster;
    /* Past FAT32's 28 bits, a cluster number can't be told from its end-of-chain mark. */
    if (!clusters || clusters > 0x0ffffff0u || fat_size > 0xffffffffu/FAT_PER_SECTOR || fat_size*FAT_PER_SECTOR < clusters+2)
        return NOT_FAT32;
    root = core_le32(block+44);
    fat_sectors = fat_size;
    fat_copies = fats;
    info_sector = core_le16(block+48) ? base+core_le16(block+48):0;
    fat_cached = NO_SECTOR;
    fat_dirty = false;
    cursor_cluster = 0;
    first_free = 2;
    freed = 0;
    if (info_sector) {
        if (!sector_read(info_sector, block))
            return READ_ERROR;
        uint32_t hint = core_le32(block+492);
        if (core_le32(block) == FSINFO_SIGNATURE && core_le32(block+484) == FSINFO_SIGNATURE2 && hint >= 2 && hint-2 < clusters)
            first_free = hint;
    }
    return 0;
}

uint32_t fat_root(void) {
    return root;
}

static bool cluster_ok(uint32_t c) {
    return c >= 2 && c-2 < clusters;
}

static bool is_end(uint32_t c) {
    return c >= 0x0ffffff8u;
}

/* The cached sector, in every copy of the FAT. */
static bool flush_fat(void) {
    if (!fat_dirty)
        return true;
    for (unsigned copy = 0; copy < fat_copies; ++copy)
        if (!sector_write(fat_cached+copy*fat_sectors, fat_block))
            return false;
    fat_dirty = false;
    return true;
}

/* The next cluster, a value that isn't one, or 0 if that can't be read. */
static uint32_t next(uint32_t c) {
    uint32_t sector = fat_at+c/FAT_PER_SECTOR;
    if (sector != fat_cached) {
        if (!flush_fat())
            return 0;
        fat_cached = NO_SECTOR;
        if (!sector_read(sector, fat_block))
            return 0;
        fat_cached = sector;
    }
    return core_le32(fat_block+c%FAT_PER_SECTOR*4)&0x0fffffffu;
}

/* Keeps the top four bits, which FAT32 reserves. */
static bool set_next(uint32_t c, uint32_t value) {
    if (c == 0 || (next(c) == 0 && fat_cached != fat_at+c/FAT_PER_SECTOR))
        return false;
    uint8_t *e = fat_block+c%FAT_PER_SECTOR*4;
    core_put_le32(e, (core_le32(e)&0xf0000000u)|value);
    fat_dirty = true;
    return true;
}

#define LONG_PIECE 13u
#define MOST_LONG_PIECES 20u
#define LONG_FLAG_LAST 0x40u
#define LONG_ATTRIBUTES 0x0fu
static const uint8_t long_places[LONG_PIECE] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
static uint16_t long_units[LONG_PIECE*MOST_LONG_PIECES+1];

struct walk {
    const char *match;
    uint32_t found, size, sector, previous;
    unsigned at;
    bool is_found;
    struct fat_entry *entries;
    struct fat_named *named;
    unsigned max, count;
    bool more;
    /* The long name being read: pieces come last first, each with the short name's checksum. */
    unsigned long_next;
    uint8_t long_sum;
    bool long_whole;
};

static uint32_t entry_start(const uint8_t *e) {
    return core_le16(e+26) | (uint32_t)core_le16(e+20) << 16;
}

static void visit_folder(struct walk *w, const uint8_t *e) {
    if (e[11]&FAT_ATTR_FOLDER && !memcmp(e, w->match, 11)) {
        w->found = entry_start(e);
        w->is_found = true;
    }
}

/* A file of that name: never a folder or the volume's label, which fat_swap() and fat_remove() would wreck. */
static void visit_name(struct walk *w, const uint8_t *e) {
    if (!(e[11]&(FAT_ATTR_FOLDER|FAT_ATTR_LABEL)) && !memcmp(e, w->match, 11)) {
        w->found = entry_start(e);
        w->size = core_le32(e+28);
        w->is_found = true;
    }
}

/* e into the list, in name order, keeping the first max by name */
static void insert(struct walk *w, const uint8_t *e) {
    unsigned at = 0;
    while (at < w->count && memcmp(w->entries[at].name, e, 8) < 0)
        ++at;
    if (w->count == w->max) {
        w->more = true;
        if (at == w->max)
            return;
        --w->count;
    }
    memmove(w->entries+at+1, w->entries+at, (w->count-at)*sizeof(struct fat_entry));
    memcpy(w->entries[at].name, e, 8);
    w->entries[at].start = entry_start(e);
    w->entries[at].size = core_le32(e+28);
    ++w->count;
}

static void visit_file(struct walk *w, const uint8_t *e) {
    if (!(e[11]&FAT_ATTR_FOLDER) && !memcmp(e+8, w->match, 3))
        insert(w, e);
}

static void visit_subfolder(struct walk *w, const uint8_t *e) {
    if (e[11]&FAT_ATTR_FOLDER && e[0] != '.')
        insert(w, e);
}

static void long_piece(struct walk *w, const uint8_t *e) {
    unsigned piece = e[0]&(LONG_FLAG_LAST-1u);
    if (e[0]&LONG_FLAG_LAST) {
        w->long_whole = false;
        w->long_next = piece;
        w->long_sum = e[13];
        if (piece >= 1 && piece <= MOST_LONG_PIECES)
            long_units[piece*LONG_PIECE] = 0;
        else
            w->long_next = 0;
    }
    if (!w->long_next || piece != w->long_next || e[13] != w->long_sum) {
        w->long_next = 0;
        return;
    }
    for (unsigned i = 0; i < LONG_PIECE; ++i)
        long_units[(piece-1)*LONG_PIECE+i] = core_le16(e+long_places[i]);
    if (!--w->long_next)
        w->long_whole = true;
}

static uint8_t short_sum(const uint8_t *e) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i)
        sum = (uint8_t)((sum >> 1 | sum << 7)+e[i]);
    return sum;
}

static unsigned utf8_length(uint32_t c) {
    return c < 0x80 ? 1:c < 0x800 ? 2:c < 0x10000 ? 3:4;
}

/* Stops between characters, and leaves a terminator. */
static void long_name_utf8(char *out) {
    unsigned at = 0;
    for (unsigned i = 0; long_units[i] && long_units[i] != 0xffffu && i < LONG_PIECE*MOST_LONG_PIECES; ++i) {
        uint32_t c = long_units[i];
        if (c >= 0xd800 && c < 0xdc00 && long_units[i+1] >= 0xdc00 && long_units[i+1] < 0xe000)
            c = 0x10000u+((c-0xd800u) << 10)+(long_units[++i]-0xdc00u);
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffdu;
        unsigned n = utf8_length(c);
        if (at+n >= FAT_NAME_BYTES)
            break;
        static const uint8_t lead[5] = {0, 0, 0xc0, 0xe0, 0xf0};
        if (n == 1)
            out[at++] = (char)c;
        else {
            out[at] = (char)(lead[n] | c >> (6*(n-1)));
            for (unsigned b = 1; b < n; ++b)
                out[at+b] = (char)(0x80u | (c >> (6*(n-1-b))&0x3fu));
            at += n;
        }
    }
    out[at] = 0;
}

/* A computer may keep a short name's case in two bits of its entry, rather than a long name. Bytes past ASCII are in a
 * code page the name doesn't say, so they can't be turned into UTF-8. */
static char short_char(uint8_t c, bool lower) {
    if (c >= 0x80)
        return '?';
    return (char)(lower && c >= 'A' && c <= 'Z' ? c+32:c);
}

static void short_name(char *out, const uint8_t *e) {
    unsigned at = 0;
    for (unsigned i = 0; i < 8 && e[i] != ' '; ++i)
        out[at++] = short_char(e[i], e[12]&0x08);
    if (e[8] != ' ')
        out[at++] = '.';
    for (unsigned i = 8; i < 11 && e[i] != ' '; ++i)
        out[at++] = short_char(e[i], e[12]&0x10);
    out[at] = 0;
}

static int name_order(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = (uint8_t)*a, y = (uint8_t)*b;
        x = x >= 'a' && x <= 'z' ? x-32:x;
        y = y >= 'a' && y <= 'z' ? y-32:y;
        if (x != y || !x)
            return x-y;
    }
}

static void visit_named(struct walk *w, const uint8_t *e) {
    if (e[11]&0x10 || memcmp(e+8, w->match, 3))
        return;
    static char name[FAT_NAME_BYTES];
    if (w->long_whole && w->long_sum == short_sum(e))
        long_name_utf8(name);
    else
        short_name(name, e);
    unsigned at = 0;
    while (at < w->count && name_order(w->named[at].name, name) < 0)
        ++at;
    if (w->count == w->max) {
        w->more = true;
        if (at == w->max)
            return;
        --w->count;
    }
    memmove(w->named+at+1, w->named+at, (w->count-at)*sizeof(struct fat_named));
    memcpy(w->named[at].file.name, e, sizeof(w->named[at].file.name));
    w->named[at].file.start = entry_start(e);
    w->named[at].file.size = core_le32(e+28);
    memcpy(w->named[at].name, name, FAT_NAME_BYTES);
    ++w->count;
}

static const char *walk(uint32_t c, void (*visit)(struct walk *, const uint8_t *), struct walk *w) {
    unsigned seen = 0;
    if (!cluster_ok(c))
        return BAD_FOLDER;
    for (;;) {
        for (unsigned s = 0; s < per_cluster; ++s) {
            w->previous = w->sector;
            w->sector = data_at+(c-2)*per_cluster+s;
            if (!sector_read(w->sector, block))
                return READ_ERROR;
            for (unsigned at = 0; at < SECTOR; at += ENTRY) {
                w->at = at;
                const uint8_t *e = block+at;
                if (!e[0])
                    return 0;
                if (++seen > MOST_ENTRIES)
                    return BAD_FOLDER;
                if (e[0] != 0xe5 && e[11] == LONG_ATTRIBUTES) {
                    long_piece(w, e);
                    continue;
                }
                /* Deleted, or a volume label. */
                if (e[0] != 0xe5 && !(e[11]&0x08)) {
                    visit(w, e);
                    if (w->is_found)
                        return 0;
                }
                /* A long name belongs only to the entry right after it. */
                w->long_next = 0;
                w->long_whole = false;
            }
        }
        app_feed();
        uint32_t following = next(c);
        if (!following)
            return READ_ERROR;
        if (is_end(following))
            return 0;
        if (!cluster_ok(following))
            return BAD_FOLDER;
        c = following;
    }
}

const char *fat_folder(uint32_t *cluster, const char name[11], const char *missing) {
    struct walk w = {.match = name};
    const char *error = walk(*cluster, visit_folder, &w);
    if (error)
        return error;
    if (!w.is_found)
        return missing;
    *cluster = w.found;
    return 0;
}

const char *fat_list(uint32_t cluster, const char extension[3], struct fat_entry *entries, unsigned max, unsigned *count,
                     bool *more) {
    struct walk w = {.match = extension, .entries = entries, .max = max};
    const char *error = walk(cluster, visit_file, &w);
    *count = w.count;
    *more = w.more;
    return error;
}

const char *fat_list_folders(uint32_t cluster, struct fat_entry *entries, unsigned max, unsigned *count, bool *more) {
    struct walk w = {.entries = entries, .max = max};
    const char *error = walk(cluster, visit_subfolder, &w);
    *count = w.count;
    *more = w.more;
    return error;
}

const char *fat_list_named(uint32_t cluster, const char extension[3], struct fat_named *entries, unsigned max, unsigned *count,
                           bool *more) {
    struct walk w = {.match = extension, .named = entries, .max = max};
    const char *error = walk(cluster, visit_named, &w);
    *count = w.count;
    *more = w.more;
    return error;
}

bool fat_read(const struct fat_entry *file, uint32_t offset, void *to, uint32_t n) {
    if (offset > file->size || n > file->size-offset)
        return false;
    uint8_t *out = to;
    uint32_t c = file->start, cluster_bytes = per_cluster*SECTOR, index = offset/cluster_bytes, skip = index;
    if (!cluster_ok(c))
        return !n;
    /* Sequential reads go on from the last, as walking a long file's chain each time would get slower and slower. */
    if (cursor_start == file->start && cursor_index <= index && cursor_cluster) {
        c = cursor_cluster;
        skip = index-cursor_index;
    }
    for (; skip; --skip) {
        c = next(c);
        if (!cluster_ok(c))
            return false;
    }
    cursor_start = file->start;
    cursor_index = index;
    cursor_cluster = c;
    offset %= cluster_bytes;
    while (n) {
        uint32_t sector = data_at+(c-2)*per_cluster+offset/SECTOR;
        uint32_t in_sector = offset%SECTOR, part = SECTOR-in_sector;
        if (part > n)
            part = n;
        if (part == SECTOR && !((uintptr_t)out&3)) {
            /* The card's DMA goes straight to the target. */
            if (!sector_read(sector, out))
                return false;
        } else {
            if (!sector_read(sector, block))
                return false;
            memcpy(out, block+in_sector, part);
        }
        out += part;
        offset += part;
        n -= part;
        if (offset == cluster_bytes && n) {
            app_feed();
            offset = 0;
            c = next(c);
            if (!cluster_ok(c))
                return false;
        }
    }
    return true;
}

const char *fat_reserve(uint32_t size, uint32_t *start) {
    uint32_t cluster_bytes = per_cluster*SECTOR, need = size/cluster_bytes+(size%cluster_bytes != 0);
    /* An empty file has no chain. */
    *start = 0;
    if (!need)
        return 0;
    /* First, without changing anything, whether there's room. */
    uint32_t found = 0, c = first_free;
    for (uint32_t looked = 0; looked < clusters && found < need; ++looked) {
        if (!(looked&0xfff))
            app_feed();
        uint32_t value = next(c);
        if (!value && fat_cached != fat_at+c/FAT_PER_SECTOR)
            return READ_ERROR;
        if (!value)
            ++found;
        if (++c >= clusters+2)
            c = 2;
    }
    if (found < need)
        return CARD_FULL;
    uint32_t first = 0, last = 0;
    c = first_free;
    uint32_t looked = 0;
    for (found = 0; found < need; ) {
        if (!(looked++&0xfff))
            app_feed();
        uint32_t value = next(c);
        if (!value && fat_cached != fat_at+c/FAT_PER_SECTOR)
            return READ_ERROR;
        if (!value) {
            if (!set_next(c, END_OF_CHAIN) || (last && !set_next(last, c)))
                return WRITE_ERROR;
            if (!first)
                first = c;
            last = c;
            ++found;
        }
        if (++c >= clusters+2)
            c = 2;
    }
    first_free = c;
    freed -= (int)need;
    if (!flush_fat())
        return WRITE_ERROR;
    *start = first;
    return 0;
}

bool fat_write(uint32_t start, uint32_t offset, const void *from, uint32_t n) {
    if (offset%SECTOR || n%SECTOR || !cluster_ok(start))
        return false;
    const uint8_t *in = from;
    uint32_t c = start, cluster_bytes = per_cluster*SECTOR;
    for (uint32_t skip = offset/cluster_bytes; skip; --skip) {
        c = next(c);
        if (!cluster_ok(c))
            return false;
    }
    offset %= cluster_bytes;
    while (n) {
        if (!sector_write(data_at+(c-2)*per_cluster+offset/SECTOR, in))
            return false;
        in += SECTOR;
        offset += SECTOR;
        n -= SECTOR;
        if (offset == cluster_bytes && n) {
            app_feed();
            offset = 0;
            c = next(c);
            if (!cluster_ok(c))
                return false;
        }
    }
    return true;
}

const char *fat_release(uint32_t start) {
    uint32_t c = start;
    cursor_cluster = 0;
    /* Bounded by the volume, in case the chain loops. It stops at the first cluster already free. */
    for (uint32_t n = 0; n < clusters && cluster_ok(c); ++n) {
        if (!(n&0xfff))
            app_feed();
        uint32_t following = next(c);
        if (!following)
            break;
        if (!set_next(c, 0))
            return WRITE_ERROR;
        ++freed;
        if (c < first_free)
            first_free = c;
        c = following;
    }
    return flush_fat() ? 0:WRITE_ERROR;
}

static void put_entry(uint8_t *e, const char name[11], unsigned attributes, uint32_t start, uint32_t size) {
    memset(e, 0, ENTRY);
    memcpy(e, name, 11);
    e[11] = (uint8_t)attributes;
    core_put_le16(e+16, DOS_DATE);
    core_put_le16(e+18, DOS_DATE);
    core_put_le16(e+24, DOS_DATE);
    core_put_le16(e+20, (uint16_t)(start >> 16));
    core_put_le16(e+26, (uint16_t)start);
    core_put_le32(e+28, size);
}

/* A cluster for a directory: all zeros, as a zero entry ends a directory. */
static const char *blank_cluster(uint32_t c) {
    memset(block, 0, SECTOR);
    for (unsigned s = 0; s < per_cluster; ++s)
        if (!sector_write(data_at+(c-2)*per_cluster+s, block))
            return WRITE_ERROR;
    return 0;
}

/* Entries to add: up to FAT_LONG_PIECES long-name pieces, then the short entry. */
#define MOST_ADDED (FAT_LONG_PIECES+1u)
static uint8_t added[MOST_ADDED*ENTRY];

/* Writes n entries from added into one directory sector, so one write makes them all appear at once. They go in a run
 * of free entries in a sector; or, where the directory ends without room, at the start of the next sector, or of a
 * new cluster, before the old end's free entries are marked deleted so the directory goes on to them. */
static const char *add_entries(uint32_t directory, unsigned n) {
    if (!cluster_ok(directory))
        return BAD_FOLDER;
    uint32_t c = directory, seen = 0, end_sector = 0;
    unsigned end_at = 0;
    for (;;) {
        app_feed();
        for (unsigned s = 0; s < per_cluster; ++s) {
            uint32_t sector = data_at+(c-2)*per_cluster+s;
            if (end_sector) {
                /* Past the end, whatever is here was never part of the directory. */
                memset(block, 0, SECTOR);
                memcpy(block, added, n*ENTRY);
                if (!sector_write(sector, block))
                    return WRITE_ERROR;
                if (!sector_read(end_sector, block))
                    return READ_ERROR;
                for (unsigned at = end_at; at < SECTOR; at += ENTRY)
                    block[at] = 0xe5;
                return sector_write(end_sector, block) ? 0:WRITE_ERROR;
            }
            if (!sector_read(sector, block))
                return READ_ERROR;
            unsigned run = 0;
            for (unsigned at = 0; at < SECTOR; at += ENTRY) {
                /* As walk() does, so a chain that loops ends with an error, and its FAT stays as it is. */
                if (++seen > MOST_ENTRIES)
                    return BAD_FOLDER;
                if (block[at] && block[at] != 0xe5) {
                    run = 0;
                    continue;
                }
                bool was_end = !block[at];
                unsigned first = at-run*ENTRY;
                if (was_end) {
                    /* A zero entry ends the directory: all after it is free, though what follows in this sector may
                     * never have been written. */
                    if (first+n*ENTRY > SECTOR) {
                        end_sector = sector;
                        end_at = at;
                        break;
                    }
                    memcpy(block+first, added, n*ENTRY);
                    if (first+n*ENTRY < SECTOR)
                        memset(block+first+n*ENTRY, 0, SECTOR-first-n*ENTRY);
                    return sector_write(sector, block) ? 0:WRITE_ERROR;
                }
                if (++run == n) {
                    memcpy(block+first, added, n*ENTRY);
                    return sector_write(sector, block) ? 0:WRITE_ERROR;
                }
            }
        }
        uint32_t following = next(c);
        if (!following)
            return READ_ERROR;
        if (is_end(following))
            break;
        if (!cluster_ok(following))
            return BAD_FOLDER;
        c = following;
    }
    /* Full, or ending in the last sector of its last cluster: a blank cluster goes on the end first, so the directory
     * is never without its end, then the entries, then the old end's free entries are marked deleted. */
    uint32_t more = 0;
    const char *error = fat_reserve(per_cluster*SECTOR, &more);
    if (!error)
        error = blank_cluster(more);
    if (error)
        return error;
    if (!set_next(c, more) || !flush_fat())
        return WRITE_ERROR;
    memset(block, 0, SECTOR);
    memcpy(block, added, n*ENTRY);
    if (!sector_write(data_at+(more-2)*per_cluster, block))
        return WRITE_ERROR;
    if (!end_sector)
        return 0;
    if (!sector_read(end_sector, block))
        return READ_ERROR;
    for (unsigned at = end_at; at < SECTOR; at += ENTRY)
        block[at] = 0xe5;
    return sector_write(end_sector, block) ? 0:WRITE_ERROR;
}

const char *fat_add(uint32_t directory, const char name[11], unsigned attributes, uint32_t start, uint32_t size) {
    put_entry(added, name, attributes, start, size);
    return add_entries(directory, 1);
}

bool fat_long_name_ok(const char *long_name) {
    static const char refused[] = "\\/:*?\"<>|";
    unsigned length = 0;
    while (long_name[length]) {
        unsigned char ch = (unsigned char)long_name[length];
        if (ch < 0x20 || ch > 0x7e)
            return false;
        for (const char *r = refused; *r; ++r)
            if (ch == (unsigned char)*r)
                return false;
        ++length;
    }
    return length && length <= FAT_LONG_PIECES*LONG_PIECE && long_name[length-1] != ' ' && long_name[length-1] != '.';
}

const char *fat_add_named(uint32_t directory, const char name[11], const char *long_name, unsigned attributes,
                          uint32_t start, uint32_t size) {
    if (!fat_long_name_ok(long_name))
        return BAD_NAME;
    unsigned length = (unsigned)strlen(long_name);
    unsigned pieces = (length+LONG_PIECE-1)/LONG_PIECE;
    put_entry(added+pieces*ENTRY, name, attributes, start, size);
    uint8_t sum = short_sum(added+pieces*ENTRY);
    for (unsigned p = 0; p < pieces; ++p) {
        /* The last piece comes first. */
        unsigned piece = pieces-p;
        uint8_t *e = added+p*ENTRY;
        memset(e, 0, ENTRY);
        e[0] = (uint8_t)(piece | (p ? 0u:LONG_FLAG_LAST));
        e[11] = LONG_ATTRIBUTES;
        e[13] = sum;
        for (unsigned i = 0; i < LONG_PIECE; ++i) {
            unsigned at = (piece-1)*LONG_PIECE+i;
            /* The name, a zero after it if there's room, then 0xFFFF. */
            uint16_t unit = at < length ? (uint8_t)long_name[at] : at == length ? 0:0xffffu;
            core_put_le16(e+long_places[i], unit);
        }
    }
    return add_entries(directory, pieces+1);
}

const char *fat_find(uint32_t directory, const char name[11], struct fat_entry *entry, bool *found) {
    struct walk w = {.match = name};
    const char *error = walk(directory, visit_name, &w);
    *found = !error && w.is_found;
    if (*found && entry) {
        memcpy(entry->name, name, sizeof(entry->name));
        entry->start = w.found;
        entry->size = w.size;
    }
    return error;
}

const char *fat_swap(uint32_t directory, const char name[11], uint32_t start, uint32_t size) {
    struct walk w = {.match = name};
    const char *error = walk(directory, visit_name, &w);
    if (error)
        return error;
    if (!w.is_found)
        return BAD_FOLDER;
    /* block still holds the entry's sector. This one write is what changes the file. */
    uint8_t *e = block+w.at;
    core_put_le16(e+20, (uint16_t)(start >> 16));
    core_put_le16(e+26, (uint16_t)start);
    core_put_le32(e+28, size);
    if (!sector_write(w.sector, block))
        return WRITE_ERROR;
    /* On a damaged card whose FAT had the old chain's first cluster free, fat_reserve() can have taken it for the new
     * one: freeing the old chain would free the new. (The old chain running into the new further on is possible on such
     * a card too; it isn't checked.) */
    if (w.found == start)
        return 0;
    return fat_release(w.found);
}

const char *fat_remove(uint32_t directory, const char name[11]) {
    struct walk w = {.match = name};
    const char *error = walk(directory, visit_name, &w);
    if (error)
        return error;
    if (!w.is_found)
        return BAD_FOLDER;
    uint8_t sum = short_sum(block+w.at);
    block[w.at] = 0xe5;
    /* A name given on a computer can have long-name pieces just before it. Those in this sector go with it. */
    unsigned at = w.at;
    for (; at >= ENTRY && block[at-ENTRY+11] == LONG_ATTRIBUTES && block[at-ENTRY] != 0xe5 && block[at-ENTRY+13] == sum;
         at -= ENTRY)
        block[at-ENTRY] = 0xe5;
    if (!sector_write(w.sector, block))
        return WRITE_ERROR;
    /* And those that start in the sector before, after the entry itself has gone: a cut between leaves pieces that
     * belong to nothing, which readers skip. */
    if (!at && w.previous && w.previous != w.sector) {
        if (!sector_read(w.previous, block))
            return READ_ERROR;
        bool changed = false;
        for (at = SECTOR; at >= ENTRY && block[at-ENTRY+11] == LONG_ATTRIBUTES && block[at-ENTRY] != 0xe5 &&
                          block[at-ENTRY+13] == sum; at -= ENTRY) {
            block[at-ENTRY] = 0xe5;
            changed = true;
        }
        if (changed && !sector_write(w.previous, block))
            return WRITE_ERROR;
    }
    return fat_release(w.found);
}

const char *fat_make_folder(uint32_t *cluster, const char name[11]) {
    struct walk w = {.match = name};
    const char *error = walk(*cluster, visit_folder, &w);
    if (error)
        return error;
    if (w.is_found) {
        *cluster = w.found;
        return 0;
    }
    uint32_t made = 0;
    error = fat_reserve(per_cluster*SECTOR, &made);
    if (!error)
        error = blank_cluster(made);
    if (error)
        return error;
    /* Its own entries, where a parent that's the root counts as cluster 0. */
    memset(block, 0, SECTOR);
    put_entry(block, ".          ", FAT_ATTR_FOLDER, made, 0);
    put_entry(block+ENTRY, "..         ", FAT_ATTR_FOLDER, *cluster == root ? 0:*cluster, 0);
    if (!sector_write(data_at+(made-2)*per_cluster, block))
        return WRITE_ERROR;
    error = fat_add(*cluster, name, FAT_ATTR_FOLDER, made, 0);
    if (error)
        return error;
    *cluster = made;
    return 0;
}

const char *fat_commit(void) {
    if (!flush_fat())
        return WRITE_ERROR;
    if (!info_sector)
        return 0;
    if (!sector_read(info_sector, block))
        return READ_ERROR;
    if (core_le32(block) != FSINFO_SIGNATURE || core_le32(block+484) != FSINFO_SIGNATURE2)
        return 0;
    uint32_t count = core_le32(block+488);
    if (count != NO_COUNT)
        core_put_le32(block+488, count+(uint32_t)freed);
    core_put_le32(block+492, first_free);
    freed = 0;
    return sector_write(info_sector, block) ? 0:WRITE_ERROR;
}
