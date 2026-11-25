/* SPDX-License-Identifier: MIT */
#include "files.h"
#include <string.h>

static struct fat_named found[FILES_MOST];

static bool ends_with(const char *name, const char *tail) {
    size_t n = strlen(name), t = strlen(tail);
    if (n <= t)
        return false;
    for (size_t i = 0; i < t; ++i) {
        char c = name[n-t+i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c-32);
        if (c != tail[i])
            return false;
    }
    return true;
}

/* A .fwsc file's short name has the extension FWS, as the long one's is four letters; a .fws file is skipped. */
static const char *list(struct file_list *l, uint32_t folder, const char *folder_name, const char extension[3],
                        const char *tail, enum pkg_kind kind) {
    unsigned count = 0;
    bool more = false;
    const char *error = fat_list_named(folder, extension, found, FILES_MOST, &count, &more);
    if (error)
        return error;
    l->more |= more;
    for (unsigned i = 0; i < count; ++i) {
        if (!ends_with(found[i].name, tail))
            continue;
        if (l->count == FILES_MOST) {
            l->more = true;
            break;
        }
        struct item *it = l->items+l->count++;
        it->file = found[i].file;
        it->kind = kind;
        it->folder = folder_name;
        memcpy(it->name, found[i].name, FAT_NAME_BYTES);
    }
    return 0;
}

const char *files_scan(struct file_list *l) {
    static const char *const names[] = {"/", "/FIRMWARE", "/DOCK"};
    static const char short_names[][12] = {"", "FIRMWARE   ", "DOCK       "};
    l->count = 0;
    l->more = false;
    for (unsigned f = 0; f < 3; ++f) {
        uint32_t folder = fat_root();
        /* A folder that isn't there is fine; fat_folder() says so with the text given, which isn't shown. */
        if (f && fat_folder(&folder, short_names[f], "missing"))
            continue;
        const char *error = list(l, folder, names[f], "FWS", ".FWSC", PKG_FWSC);
        if (!error)
            error = list(l, folder, names[f], "BIN", ".BIN", PKG_BIN);
        if (error)
            return error;
    }
    return 0;
}
