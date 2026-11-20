/* SPDX-License-Identifier: MIT */
/* Draws usbdisk's screens on a computer, from the same code, into PPM files: usbdisk_look_host OUT_DIR. */
#include "look.h"
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

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const char *dir = argv[1];
    for (unsigned board = 1; board <= 2; ++board) {
        char name[64];
        struct look_model m = {.state = LOOK_STARTING};
        look_start(board);
        look_update(&m, 0);
        snprintf(name, sizeof name, "%u-0-starting", board);
        save(dir, name);
        m.state = LOOK_WAITING;
        m.card_mib = 7452;
        look_update(&m, 600);
        snprintf(name, sizeof name, "%u-1-waiting", board);
        save(dir, name);
        m.state = LOOK_CONNECTED;
        look_update(&m, 600);
        snprintf(name, sizeof name, "%u-2-connected", board);
        save(dir, name);
        m.read_kbs = 870;
        m.read_kib = 52000;
        for (unsigned k = 0; k < 7; ++k)
            look_update(&m, 50);
        snprintf(name, sizeof name, "%u-3-reading", board);
        save(dir, name);
        m.read_kbs = 0;
        m.write_kbs = 440;
        m.written_kib = 3100;
        for (unsigned k = 0; k < 7; ++k)
            look_update(&m, 50);
        snprintf(name, sizeof name, "%u-4-writing", board);
        save(dir, name);
        m.write_kbs = 0;
        m.state = LOOK_EJECTED;
        look_update(&m, 50);
        snprintf(name, sizeof name, "%u-5-ejected", board);
        save(dir, name);
        struct look_model none = {.state = LOOK_NO_CARD};
        look_start(board);
        look_update(&none, 0);
        snprintf(name, sizeof name, "%u-6-no-card", board);
        save(dir, name);
    }
    printf("OK\n");
    return 0;
}
