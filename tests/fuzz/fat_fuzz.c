/* SPDX-License-Identifier: MIT */
/* libFuzzer: kit/fat.c on any card image, and the launcher's catalog on it (apps/picker/catalog.c): mounting, folders,
 * long names, reading a file, META.TXT, then a file written and replaced, as an app saving does. What's read must stay
 * inside the card, and nothing may hang (a loop in the FAT has to end in an error). */
#include "catalog.h"
#include "fat.h"
#include "load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *image;
static uint32_t sectors;

void app_feed(void) {
}

bool card_read(uint32_t sector, uint8_t *data) {
    if (sector >= sectors)
        return false;
    memcpy(data, image+sector*512ull, 512);
    return true;
}

bool card_write(uint32_t sector, const uint8_t *data) {
    if (sector >= sectors)
        return false;
    memcpy(image+sector*512ull, data, 512);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    if (n > 96*1024)
        return 0;
    sectors = (uint32_t)((n+511)/512);
    image = calloc(sectors ? sectors : 1, 512);
    memcpy(image, data, n);
    if (!fat_mount(sectors)) {
        uint32_t root = fat_root(), folder = root;
        static struct fat_named named[8];
        static struct fat_entry entries[8];
        unsigned count;
        bool more, found = false;
        fat_list_named(root, "TXT", named, 8, &count, &more);
        fat_list_folders(root, entries, 8, &count, &more);
        fat_list(root, "TXT", entries, 8, &count, &more);
        struct fat_entry file;
        if (!fat_find(root, "WIFI    TXT", &file, &found) && found) {
            static uint8_t text[4096];
            fat_read(&file, 0, text, file.size < sizeof text ? file.size : sizeof text);
        }
        if (!fat_folder(&folder, "MBOOT      ", "") && !fat_folder(&folder, "APPS       ", "")) {
            static struct catalog c;
            catalog_read(folder, &c);
        }
        /* A file written, as an app saves: a new one that reports success must read back as written. (A replaced one is
         * written too, but not checked: on a damaged FAT that had the old file's clusters free, the new file can be
         * given them and then lose them as the old chain is freed, which kit/fat.c only partly guards against.) */
        uint32_t start;
        if (!fat_reserve(1000, &start)) {
            static uint8_t block[1024], back[1000];
            for (unsigned i = 0; i < sizeof block; ++i)
                block[i] = (uint8_t)(i*7+1);
            /* Replaced if it's there (fat_add() is for a name the folder doesn't have), as the WiFi app saves. */
            const char *name = found ? "WIFI    TXT" : "NEW     TXT";
            bool exists = found;
            if (!found && fat_find(root, name, 0, &exists))
                exists = true;
            if (fat_write(start, 0, block, sizeof block) &&
                !(exists ? fat_swap(root, name, start, 1000) : fat_add(root, name, FAT_ATTR_ARCHIVE, start, 1000)) &&
                !fat_commit() && !exists) {
                struct fat_entry written;
                bool there = false;
                if (fat_find(root, name, &written, &there) || !there || written.size != 1000 ||
                    !fat_read(&written, 0, back, sizeof back) || memcmp(back, block, sizeof back)) {
                    fprintf(stderr, "a file written isn't read back as written\n");
                    abort();
                }
            }
        }
    }
    free(image);
    return 0;
}
