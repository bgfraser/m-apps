/* SPDX-License-Identifier: MIT */
/* Draws slots' screens on a computer, from the same code, into PPM files: slots_screens_host OUT_DIR. */
#include "canvas.h"
#include "screens.h"
#include "textentry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t screen[240][240];

void display_pixels(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *data) {
    if (x+w > 240 || y+h > 240 || !w || !h) {
        fprintf(stderr, "display_pixels out of the screen: %u %u %u %u\n", x, y, w, h);
        exit(2);
    }
    for (unsigned r = 0; r < h; ++r)
        for (unsigned c = 0; c < w; ++c, data += 2)
            screen[y+r][x+c] = (uint16_t)(data[0] << 8 | data[1]);
}

static void save(const char *dir, const char *name) {
    canvas_send_all();
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        exit(2);
    fprintf(f, "P6 240 240 255\n");
    for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 240; ++x) {
            uint16_t c = screen[y][x];
            uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3 | (c >> 13)), (uint8_t)(((c >> 5)&63) << 2 | ((c >> 9)&3)),
                              (uint8_t)((c&31) << 3 | ((c&31) >> 2))};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static struct slots s;

static void add(const char *name, uint32_t size, bool sound) {
    struct slot *slot = &s.list[s.count++];
    memset(slot, 0, sizeof *slot);
    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->file.size = size;
    slot->sound = sound;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const char *dir = argv[1];
    add("Felucca 1.8.2 beta", 812*1024+512, true);
    add("Snoop37 with my patches and a name too long to fit", 655*1024, true);
    add("SLOOP", 1024*1024, true);
    add("Broken one", 4096, false);
    for (unsigned board = 1; board <= 2; ++board) {
        char name[64];
        screen_list(&s, board, 0, 0, NO_SLOT, 0, 0);
        snprintf(name, sizeof name, "%u-0-list-save", board);
        save(dir, name);
        screen_list(&s, board, 2, 0, 2, "The last load stopped", CANVAS_ORANGE);
        snprintf(name, sizeof name, "%u-1-list-pending", board);
        save(dir, name);
        screen_list(&s, board, 5, 0, NO_SLOT, "Saved", CANVAS_GREEN);
        snprintf(name, sizeof name, "%u-2-list-end", board);
        save(dir, name);
        const uint8_t actions[] = {ACTION_LOAD, ACTION_DELETE, ACTION_BACK};
        screen_slot(&s.list[0], board, false, false, actions, 3, 0, 0);
        snprintf(name, sizeof name, "%u-3-slot", board);
        save(dir, name);
        screen_slot(&s.list[1], board, false, false, actions, 3, 1, 60);
        snprintf(name, sizeof name, "%u-4-slot-holding-delete", board);
        save(dir, name);
        screen_name(board, "", "Slot 1", TEXT_LOWER, 0, true, 0);
        snprintf(name, sizeof name, "%u-5-name-empty", board);
        save(dir, name);
        screen_name(board, "Felucca 1.8", "Slot 1", TEXT_UPPER, 1u << 5 | 1u << 12, true, 0);
        snprintf(name, sizeof name, "%u-6-name-caps", board);
        save(dir, name);
        screen_name(board, "My very long slot name that scrolls", "Slot 1", TEXT_SYMBOLS, 0, false,
                    "A name can't end in a space or a dot");
        snprintf(name, sizeof name, "%u-7-name-symbols", board);
        save(dir, name);
        screen_work("Loading the slot", "Writing the firmware and its settings", true);
        screen_work_progress(40, 100);
        snprintf(name, sizeof name, "%u-8-work", board);
        save(dir, name);
    }
    printf("OK\n");
    return 0;
}
