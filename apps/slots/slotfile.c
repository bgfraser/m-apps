/* SPDX-License-Identifier: MIT */
#include "slotfile.h"
#include "core.h"
#include "platform.h"
#include <string.h>
#define SECTOR 0x1000u
#define CARD_SECTOR 512u
#define HEADER 512u
#define FIRST 0x4000u
#define END 0xff000u
#define FORMAT 1u
#define STATE_MAGIC "MSLT"
#define READ_ERROR "CARD READ ERROR"
#define WRITE_ERROR "CARD WRITE ERROR"
#define FLASH_ERROR "FLASH ERROR"
#define DAMAGED "SLOT IS DAMAGED"
#define PROGRESS_EVERY 8u
/* The board slots_open() was told: 1 FM-1, 2 SMK-37. */
static unsigned board;
#define MODEL_FOLDER (board == 2 ? "SMK37      " : "FM1        ")
void app_feed(void);
static uint8_t flash_data[SECTOR] __attribute__((aligned(4)));
static uint8_t file_data[SECTOR] __attribute__((aligned(4)));
static uint8_t header[HEADER] __attribute__((aligned(4)));
static uint8_t state_block[CARD_SECTOR] __attribute__((aligned(4)));
static struct fat_named named[MOST_SLOTS];

/* What a load writes: the firmware's part of the flash, without the SMK-37's Bluetooth and calibration sectors. */
static bool in_scope(uint32_t address) {
    if (address < FIRST || address >= END)
        return false;
    return board != 2 || address < 0xc0000u || address >= 0xc2000u;
}

static bool blank(const uint8_t *data) {
    for (unsigned i = 0; i < SECTOR; ++i)
        if (data[i] != 0xff)
            return false;
    return true;
}

static bool is_stored(const uint8_t stored[32], uint32_t address) {
    unsigned n = address/SECTOR;
    return stored[n/8] >> (n%8)&1u;
}

/* Where a stored sector is in its slot's file. */
static uint32_t file_offset(const uint8_t stored[32], uint32_t address) {
    unsigned n = address/SECTOR, before = 0;
    for (unsigned i = 0; i < n; ++i)
        before += stored[i/8] >> (i%8)&1u;
    return HEADER+before*SECTOR;
}

/* ---- the list ---- */

static int slot_with(const struct slots *s, const char id[8]) {
    if (id[0])
        for (unsigned i = 0; i < s->count; ++i)
            if (!memcmp(s->list[i].id, id, 8))
                return (int)i;
    return NO_SLOT;
}

int slots_pending(const struct slots *s) {
    return slot_with(s, s->pending);
}

static bool lower_equal(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if ((*a|(*a >= 'A' && *a <= 'Z' ? 32:0)) != (*b|(*b >= 'A' && *b <= 'Z' ? 32:0)))
            return false;
    return !*a && !*b;
}

/* The name to show: the long name without its extension. */
static void show_name(char *out, const char *from) {
    unsigned n = 0;
    while (from[n] && n < SLOT_NAME_MOST+4)
        ++n;
    if (n >= 4 && from[n-4] == '.' && lower_equal(from+n-3, "slt"))
        n -= 4;
    if (n > SLOT_NAME_MOST)
        n = SLOT_NAME_MOST;
    memcpy(out, from, n);
    out[n] = 0;
}

static bool name_before(const char *a, const char *b) {
    for (;; ++a, ++b) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        x |= x >= 'A' && x <= 'Z' ? 32u:0u;
        y |= y >= 'A' && y <= 'Z' ? 32u:0u;
        if (x != y || !x)
            return x < y;
    }
}

static bool read_header(struct slot *slot) {
    slot->sound = false;
    if (slot->file.size < HEADER || !fat_read(&slot->file, 0, header, HEADER))
        return false;
    if (memcmp(header, "SLOT", 4) || header[4] != FORMAT || header[5] != board || core_le16(header+6) != HEADER ||
        core_le32(header+508) != (core_crc32(0xffffffffu, header, 508)^0xffffffffu))
        return false;
    memcpy(slot->stored, header+16, 32);
    slot->sectors = core_le32(header+8);
    unsigned count = 0;
    for (uint32_t at = 0; at < SLOT_FLASH_SIZE; at += SECTOR)
        if (is_stored(slot->stored, at)) {
            if (!in_scope(at))
                return false;
            ++count;
        }
    if (count != slot->sectors || slot->file.size != HEADER+count*SECTOR || !is_stored(slot->stored, FIRST))
        return false;
    slot->sound = true;
    return true;
}

static const char *refresh(struct slots *s) {
    s->count = 0;
    s->more = false;
    unsigned n = 0;
    bool more = false;
    if (s->has_folder) {
        const char *error = fat_list_named(s->folder, "SLT", named, MOST_SLOTS, &n, &more);
        if (error)
            return error;
        for (unsigned i = 0; i < n; ++i) {
            struct slot *slot = &s->list[s->count++];
            memset(slot, 0, sizeof *slot);
            slot->file = named[i].file;
            memcpy(slot->id, named[i].file.name, 8);
            show_name(slot->name, named[i].name);
            app_feed();
            read_header(slot);
        }
        s->more = more;
        /* In the order of the names shown, ignoring ASCII case, rather than the files' with .slt after them. */
        for (unsigned i = 1; i < s->count; ++i)
            for (unsigned j = i; j > 0 && name_before(s->list[j].name, s->list[j-1].name); --j) {
                struct slot t = s->list[j];
                s->list[j] = s->list[j-1];
                s->list[j-1] = t;
            }
    }
    return 0;
}

/* ---- reading a slot as if it were flash ---- */

/* The flash as it would be after loading the slot: its sectors, blank where it has none, and the unit's own flash where
 * a load doesn't write. The firmware check reads the boot area's header to find the firmware, so it needs that. */
static bool slot_read(void *ctx, uint32_t address, void *out, uint32_t size) {
    const struct slot *slot = ctx;
    app_feed();
    uint8_t *to = out;
    while (size) {
        uint32_t in = SECTOR-address%SECTOR, n = size < in ? size:in;
        if (address >= SLOT_FLASH_SIZE)
            return false;
        if (is_stored(slot->stored, address)) {
            if (!fat_read(&slot->file, file_offset(slot->stored, address)+address%SECTOR, to, n))
                return false;
        } else if (!in_scope(address)) {
            if (!flash_read(0, address, to, n))
                return false;
        } else {
            memset(to, 0xff, n);
        }
        to += n;
        address += n;
        size -= n;
    }
    return true;
}

/* A sector of the firmware's part as the slot holds it, into file_data. */
static bool slot_sector(const struct slot *slot, uint32_t address) {
    return slot_read((void *)slot, address, file_data, SECTOR);
}

/* The stored sectors' CRC, as the header says. */
static const char *check_data(struct slots *s, const struct slot *slot) {
    if (!slot->sound)
        return DAMAGED;
    if (!fat_read(&slot->file, 0, header, HEADER))
        return READ_ERROR;
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < slot->sectors; ++i) {
        if (!(i%PROGRESS_EVERY) && s->progress)
            s->progress("CHECKING THE SLOT", i, slot->sectors);
        app_feed();
        if (!fat_read(&slot->file, HEADER+i*SECTOR, file_data, SECTOR))
            return READ_ERROR;
        crc = core_crc32(crc, file_data, SECTOR);
    }
    return (crc^0xffffffffu) == core_le32(header+112) ? 0:DAMAGED;
}

/* ---- this unit's state ---- */

static void hex_name(char out[11], uint32_t value, const char *extension) {
    memset(out, ' ', 11);
    for (unsigned i = 0; i < 8; ++i)
        out[i] = "0123456789ABCDEF"[value >> (28-4*i)&15];
    if (extension)
        memcpy(out+8, extension, 3);
}

static const char *read_state(struct slots *s) {
    char name[11];
    memcpy(name, s->unit_name, 8);
    memcpy(name+8, "DAT", 3);
    bool found = false;
    const char *error = fat_find(s->folder, name, &s->state_file, &found);
    if (error || !found || s->state_file.size != CARD_SECTOR)
        return error;
    if (!fat_read(&s->state_file, 0, state_block, CARD_SECTOR))
        return READ_ERROR;
    s->has_state = true;
    if (!memcmp(state_block, STATE_MAGIC, 4) && state_block[4] == 2 &&
        core_crc16(0, state_block, 26) == core_le16(state_block+26)) {
        memcpy(s->active, state_block+8, 8);
        memcpy(s->pending, state_block+16, 8);
    }
    return 0;
}

/* One sector of the card, so it's either as it was or as it now is. */
static const char *write_state(struct slots *s) {
    memset(state_block, 0, CARD_SECTOR);
    memcpy(state_block, STATE_MAGIC, 4);
    state_block[4] = 2;
    memcpy(state_block+8, s->active, 8);
    memcpy(state_block+16, s->pending, 8);
    core_put_le16(state_block+26, core_crc16(0, state_block, 26));
    if (s->has_state)
        return fat_write(s->state_file.start, 0, state_block, CARD_SECTOR) ? 0:WRITE_ERROR;
    uint32_t start = 0;
    const char *error = fat_reserve(CARD_SECTOR, &start);
    if (error)
        return error;
    if (!fat_write(start, 0, state_block, CARD_SECTOR))
        return WRITE_ERROR;
    char name[11];
    memcpy(name, s->unit_name, 8);
    memcpy(name+8, "DAT", 3);
    error = fat_add(s->folder, name, FAT_ATTR_ARCHIVE, start, CARD_SECTOR);
    if (error)
        return error;
    s->state_file.start = start;
    s->state_file.size = CARD_SECTOR;
    s->has_state = true;
    return 0;
}

const char *slots_open(struct slots *s, uint32_t card_sectors, const uint8_t uid[8], uint32_t jedec, unsigned which) {
    board = which;
    s->count = 0;
    s->more = false;
    s->has_folder = false;
    s->has_state = false;
    s->jedec = jedec;
    memset(s->active, 0, 8);
    memset(s->pending, 0, 8);
    hex_name(s->unit_name, core_crc32(0xffffffffu, uid, 8)^0xffffffffu, 0);
    const char *error = fat_mount(card_sectors);
    if (error)
        return error;
    /* Nothing's made until there's something to save. */
    static const char missing[] = "MISSING";
    uint32_t c = fat_root();
    error = fat_folder(&c, "MBOOT      ", missing);
    if (!error)
        error = fat_folder(&c, "SLOTS      ", missing);
    if (!error)
        error = fat_folder(&c, MODEL_FOLDER, missing);
    if (error == missing)
        return 0;
    if (error)
        return error;
    s->folder = c;
    s->has_folder = true;
    error = read_state(s);
    return error ? error:refresh(s);
}

static const char *make_folders(struct slots *s) {
    if (s->has_folder)
        return 0;
    uint32_t c = fat_root();
    const char *error = fat_make_folder(&c, "MBOOT      ");
    if (!error)
        error = fat_make_folder(&c, "SLOTS      ");
    if (!error)
        error = fat_make_folder(&c, MODEL_FOLDER);
    if (error)
        return error;
    s->folder = c;
    s->has_folder = true;
    return 0;
}

static bool on_flash(void *ctx, uint32_t address, void *out, uint32_t size) {
    app_feed();
    return flash_read(ctx, address, out, size);
}

/* ---- comparing ---- */

/* Whether a slot and the flash hold the same thing, as far as a load is concerned. */
static const char *same_as_flash(const struct slot *slot, bool *same) {
    *same = false;
    if (!slot->sound)
        return 0;
    for (uint32_t at = FIRST; at < END; at += SECTOR) {
        if (!in_scope(at))
            continue;
        if (!flash_read(0, at, flash_data, SECTOR))
            return FLASH_ERROR;
        if (!is_stored(slot->stored, at)) {
            app_feed();
            if (!blank(flash_data))
                return 0;
            continue;
        }
        if (!slot_sector(slot, at))
            return READ_ERROR;
        if (memcmp(file_data, flash_data, SECTOR))
            return 0;
    }
    *same = true;
    return 0;
}

int slots_on_unit(struct slots *s) {
    /* The active slot is the likely one, and differing slots stop at their first sector. */
    int active = slot_with(s, s->active);
    bool same = false;
    if (active != NO_SLOT && !same_as_flash(&s->list[active], &same) && same)
        return active;
    for (unsigned i = 0; i < s->count; ++i)
        if ((int)i != active && !same_as_flash(&s->list[i], &same) && same)
            return (int)i;
    return NO_SLOT;
}

/* ---- saving ---- */

bool slots_name_ok(const char *name) {
    unsigned n = 0;
    while (name[n])
        ++n;
    if (!n || n > SLOT_NAME_MOST)
        return false;
    char full[SLOT_NAME_MOST+5];
    memcpy(full, name, n);
    memcpy(full+n, ".slt", 5);
    return fat_long_name_ok(full) && name[0] != ' ' && name[n-1] != ' ' && name[n-1] != '.';
}

static bool name_taken(const struct slots *s, const char *name) {
    for (unsigned i = 0; i < s->count; ++i)
        if (lower_equal(s->list[i].name, name))
            return true;
    return false;
}

void slots_free_name(const struct slots *s, const char *base, char *out) {
    unsigned n = 0;
    while (base[n] && n < SLOT_NAME_MOST)
        out[n] = base[n], ++n;
    out[n] = 0;
    if (!name_taken(s, out))
        return;
    /* Room for " 999", and no space left before it. */
    if (n > SLOT_NAME_MOST-4)
        n = SLOT_NAME_MOST-4;
    while (n && out[n-1] == ' ')
        --n;
    for (unsigned k = 2; k < 1000; ++k) {
        unsigned m = n;
        out[m++] = ' ';
        if (k >= 100)
            out[m++] = (char)('0'+k/100);
        if (k >= 10)
            out[m++] = (char)('0'+k/10%10);
        out[m++] = (char)('0'+k%10);
        out[m] = 0;
        if (!name_taken(s, out))
            return;
    }
}

/* What's on the unit, the firmware's part of it, into a new chain: the header, then the sectors that aren't blank. It's
 * read back before anything names it. On error the chain is gone again. */
static const char *copy_flash(struct slots *s, const char *name, uint32_t *start, uint32_t *size) {
    uint8_t stored[32];
    uint32_t count = 0;
    memset(stored, 0, sizeof stored);
    for (uint32_t at = FIRST; at < END; at += SECTOR) {
        if (!in_scope(at))
            continue;
        app_feed();
        if (!flash_read(0, at, flash_data, SECTOR))
            return FLASH_ERROR;
        if (at == FIRST || !blank(flash_data)) {
            stored[at/SECTOR/8] |= (uint8_t)(1u << (at/SECTOR%8));
            ++count;
        }
    }
    *size = HEADER+count*SECTOR;
    const char *error = fat_reserve(*size, start);
    if (error)
        return error;
    uint32_t crc = 0xffffffffu, done = 0;
    for (uint32_t at = FIRST; at < END && !error; at += SECTOR) {
        if (!is_stored(stored, at))
            continue;
        if (!(done%PROGRESS_EVERY) && s->progress)
            s->progress("SAVING", done, count);
        app_feed();
        if (!flash_read(0, at, flash_data, SECTOR))
            error = FLASH_ERROR;
        else if (!fat_write(*start, HEADER+done*SECTOR, flash_data, SECTOR))
            error = WRITE_ERROR;
        crc = core_crc32(crc, flash_data, SECTOR);
        ++done;
    }
    if (!error) {
        memset(header, 0, HEADER);
        memcpy(header, "SLOT", 4);
        header[4] = FORMAT;
        header[5] = (uint8_t)board;
        core_put_le16(header+6, HEADER);
        core_put_le32(header+8, count);
        core_put_le32(header+12, s->jedec);
        memcpy(header+16, stored, 32);
        for (unsigned i = 0; name[i] && i < 63; ++i)
            header[48+i] = (uint8_t)name[i];
        core_put_le32(header+112, crc^0xffffffffu);
        core_put_le32(header+508, core_crc32(0xffffffffu, header, 508)^0xffffffffu);
        if (!fat_write(*start, 0, header, HEADER))
            error = WRITE_ERROR;
    }
    struct slot copy = {.file = {.start = *start, .size = *size}};
    memcpy(copy.stored, stored, 32);
    copy.sectors = count;
    if (!error && !read_header(&copy))
        error = "CARD WROTE WRONGLY";
    done = 0;
    for (uint32_t at = FIRST; at < END && !error; at += SECTOR) {
        if (!is_stored(stored, at))
            continue;
        if (!(done%PROGRESS_EVERY) && s->progress)
            s->progress("CHECKING", done, count);
        ++done;
        if (!slot_sector(&copy, at))
            error = READ_ERROR;
        else if (!flash_read(0, at, flash_data, SECTOR))
            error = FLASH_ERROR;
        else if (memcmp(file_data, flash_data, SECTOR))
            error = "CARD WROTE WRONGLY";
    }
    if (error) {
        fat_release(*start);
        fat_commit();
    }
    return error;
}

/* SLOT0001 and on, the first no file here has. */
static void free_id(const struct slots *s, char id[8]) {
    memcpy(id, "SLOT0000", 8);
    for (unsigned n = 1; n < 10000; ++n) {
        id[4] = (char)('0'+n/1000);
        id[5] = (char)('0'+n/100%10);
        id[6] = (char)('0'+n/10%10);
        id[7] = (char)('0'+n%10);
        if (slot_with(s, id) == NO_SLOT)
            return;
    }
}

const char *slots_save_new(struct slots *s, const char *wanted) {
    if (s->count >= MOST_SLOTS)
        return "TOO MANY SLOTS";
    if (!slots_name_ok(wanted))
        return "NAME NOT ALLOWED";
    char name[SLOT_NAME_MOST+1], full[SLOT_NAME_MOST+5], id[8], short_name[11];
    slots_free_name(s, wanted, name);
    unsigned n = (unsigned)strlen(name);
    memcpy(full, name, n);
    memcpy(full+n, ".slt", 5);
    const char *error = make_folders(s);
    if (error)
        return error;
    uint32_t start = 0, size = 0;
    error = copy_flash(s, name, &start, &size);
    if (error)
        return error;
    free_id(s, id);
    memcpy(short_name, id, 8);
    memcpy(short_name+8, "SLT", 3);
    error = fat_add_named(s->folder, short_name, full, FAT_ATTR_ARCHIVE, start, size);
    if (error) {
        /* Nothing names it, so it would only be lost. */
        fat_release(start);
        fat_commit();
        return error;
    }
    memcpy(s->active, id, 8);
    error = write_state(s);
    if (!error)
        error = fat_commit();
    return error ? error:refresh(s);
}

/* Settings go over the slot they came from: a new chain, then its entry points at it. Its name stays. */
static const char *save_over(struct slots *s, unsigned index) {
    const struct slot *slot = &s->list[index];
    uint32_t start = 0, size = 0;
    const char *error = copy_flash(s, slot->name, &start, &size);
    if (error)
        return error;
    char short_name[11];
    memcpy(short_name, slot->id, 8);
    memcpy(short_name+8, "SLT", 3);
    error = fat_swap(s->folder, short_name, start, size);
    if (!error)
        error = fat_commit();
    return error ? error:refresh(s);
}

/* What's on the unit, saved unless a slot holds it, or it isn't valid: an interrupted load can leave that, and the slot
 * being loaded then holds the only good copy. Settings go over the slot they came from, but firmware that isn't that
 * slot's gets its own. So does what's on the unit when that slot is the one being loaded, or there'd be nothing to go
 * back to. */
static const char *save_first(struct slots *s, const char target[8]) {
    int here = slots_on_unit(s);
    if (here != NO_SLOT || !s->valid(on_flash, 0))
        return 0;
    int active = slot_with(s, s->active);
    if (active != NO_SLOT && memcmp(s->active, target, 8) && s->list[active].sound) {
        if (!slot_sector(&s->list[active], FIRST))
            return READ_ERROR;
        if (!flash_read(0, FIRST, flash_data, SECTOR))
            return FLASH_ERROR;
        if (!memcmp(file_data, flash_data, SECTOR))
            return save_over(s, (unsigned)active);
    }
    char name[SLOT_NAME_MOST+1];
    slots_free_name(s, "Saved", name);
    return slots_save_new(s, name);
}

/* ---- loading ---- */

/* A sector of the slot onto the flash: what differs is erased and written, and a blank sector only erased. */
static const char *write_sector(const struct slot *slot, uint32_t at, bool erased) {
    if (!slot_sector(slot, at))
        return READ_ERROR;
    if (!flash_read(0, at, flash_data, SECTOR))
        return FLASH_ERROR;
    if (!memcmp(file_data, flash_data, SECTOR) && !erased)
        return 0;
    app_feed();
    if (!erased && !flash_erase(0, at, SECTOR))
        return FLASH_ERROR;
    if (!blank(file_data) && !flash_program(0, at, file_data, SECTOR))
        return FLASH_ERROR;
    if (!flash_read(0, at, flash_data, SECTOR) || memcmp(file_data, flash_data, SECTOR))
        return FLASH_ERROR;
    return 0;
}

const char *slots_load(struct slots *s, unsigned index) {
    if (index >= s->count)
        return DAMAGED;
    /* Ids, as saving can move things in the list. */
    char target[8];
    memcpy(target, s->list[index].id, 8);
    if (s->progress)
        s->progress("CHECKING THE SLOT", 0, 1);
    if (!s->list[index].sound)
        return DAMAGED;
    const char *error = check_data(s, &s->list[index]);
    if (error)
        return error;
    if (!s->valid(slot_read, &s->list[index]))
        return DAMAGED;
    error = save_first(s, target);
    if (error)
        return error;
    int moved = slot_with(s, target);
    if (moved == NO_SLOT)
        return DAMAGED;
    const struct slot slot = s->list[moved];
    memcpy(s->pending, target, 8);
    error = write_state(s);
    if (error)
        return error;
    /* Only what differs, with nothing erased until something does. */
    bool any = false;
    for (uint32_t at = FIRST; at < END && !any; at += SECTOR) {
        if (!in_scope(at))
            continue;
        if (!slot_sector(&slot, at))
            return READ_ERROR;
        if (!flash_read(0, at, flash_data, SECTOR))
            return FLASH_ERROR;
        any = memcmp(file_data, flash_data, SECTOR) != 0;
    }
    if (any) {
        /* The first firmware sector is erased first and written last, so what's interrupted can't pass as firmware. */
        app_feed();
        if (!flash_erase(0, FIRST, SECTOR))
            return FLASH_ERROR;
        for (uint32_t at = FIRST+SECTOR; at < END; at += SECTOR) {
            if (!in_scope(at))
                continue;
            if (!(at/SECTOR%PROGRESS_EVERY) && s->progress)
                s->progress("WRITING", at/SECTOR, END/SECTOR);
            error = write_sector(&slot, at, false);
            if (error)
                return error;
        }
        error = write_sector(&slot, FIRST, true);
        if (error)
            return error;
    }
    memcpy(s->active, target, 8);
    memset(s->pending, 0, 8);
    error = write_state(s);
    return error ? error:fat_commit();
}

const char *slots_delete(struct slots *s, unsigned index) {
    if (index >= s->count)
        return DAMAGED;
    const struct slot *slot = &s->list[index];
    char id[8], short_name[11];
    memcpy(id, slot->id, 8);
    memcpy(short_name, id, 8);
    memcpy(short_name+8, "SLT", 3);
    const char *error = fat_remove(s->folder, short_name);
    if (error)
        return error;
    if (!memcmp(s->active, id, 8))
        memset(s->active, 0, 8);
    if (!memcmp(s->pending, id, 8))
        memset(s->pending, 0, 8);
    error = s->has_state ? write_state(s):0;
    if (!error)
        error = fat_commit();
    return error ? error:refresh(s);
}
