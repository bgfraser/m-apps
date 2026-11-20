/* SPDX-License-Identifier: MIT */
/* Draws the WiFi app's screens on a computer, from the same code, into PPM files: wifi_screens_host OUT_DIR. */
#include "canvas.h"
#include "screens.h"
#include "textentry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t screen[240][240];
static unsigned saved;

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
    ++saved;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const char *dir = argv[1];
    struct wifi_list l;
    memset(&l, 0, sizeof l);
    wifi_list_put(&l, "Home", "correct horse");
    wifi_list_put(&l, "Workshop with a name too long to fit", "shed-key-2041");
    wifi_list_put(&l, "Cafe", "");
    l.use = 0;
    struct net_ap aps[5];
    memset(aps, 0, sizeof aps);
    const struct { const char *ssid; int rssi; unsigned ch, sec; } heard[] = {
        {"Home", -42, 1, NET_WPA2}, {"Neighbours 5G", -61, 6, NET_WPA2}, {"Free Cafe WiFi", -70, 11, NET_OPEN},
        {"Office WPA3", -78, 3, NET_OTHER}, {"Far away", -88, 13, NET_WPA2}};
    for (unsigned i = 0; i < 5; ++i) {
        strcpy(aps[i].ssid, heard[i].ssid);
        aps[i].rssi = (int8_t)heard[i].rssi;
        aps[i].channel = (uint8_t)heard[i].ch;
        aps[i].security = (uint8_t)heard[i].sec;
    }
    struct net_result ok = {-46, 1, 0xc0a801b5u, 0xc0a80101u, 0xc0a80101u, 6230, NET_STEPS};
    struct net_result heard_only = {-71, 6, 0, 0, 0, 0, NET_STEP_JOINING};
    for (unsigned board = 1; board <= 2; ++board) {
        char name[64];
#define SAVE(what) do { snprintf(name, sizeof name, "%u-%s", board, what); save(dir, name); } while (0)
        screen_list(&l, board, 0, 0, 0);
        SAVE("list-add");
        screen_list(&l, board, 2, "Saved: apps join it", CANVAS_GREEN);
        SAVE("list-second");
        struct wifi_list empty;
        memset(&empty, 0, sizeof empty);
        screen_list(&empty, board, 0, "No WIFI.TXT yet", CANVAS_DIM);
        SAVE("list-empty");
        uint8_t actions[4] = {ACTION_TEST, ACTION_USE, ACTION_PASSWORD, ACTION_DELETE};
        screen_network(&l, 1, board, actions, 4, 0, 0, 0, 0);
        SAVE("network");
        screen_network(&l, 1, board, actions, 4, 3, 60, 0, 0);
        SAVE("network-delete");
        uint8_t used[3] = {ACTION_TEST, ACTION_PASSWORD, ACTION_DELETE};
        screen_network(&l, 0, board, used, 3, 0, 0, 0, 0);
        SAVE("network-used");
        screen_scanning(0, 13);
        screen_scanning(5, 13);
        SAVE("scanning");
        screen_heard(aps, 5, &l, board, 0);
        SAVE("heard");
        screen_heard(aps, 5, &l, board, 3);
        SAVE("heard-other");
        screen_heard(aps, 5, &l, board, 5);
        SAVE("heard-type");
        screen_heard(aps, 0, &l, board, 0);
        SAVE("heard-none");
        text_all_symbols(1);
        for (unsigned layer = 0; layer < 4; ++layer) {
            char what[16];
            snprintf(what, sizeof what, "type-%u", layer);
            screen_type(board, "Password", "Neighbours 5G", layer ? "secret#1" : "", "8 to 63 characters", layer,
                        layer == 2 ? 1u << 3 : 0, true, layer == 3 ? "At least 8 characters" : 0);
            SAVE(what);
        }
        screen_test(board, "Home", NET_STEP_KEY, false, 0, &ok, "Testing", CANVAS_DIM, "");
        SAVE("test-running");
        screen_test(board, "Home", NET_STEPS, true, 0, &ok, "Saved: apps join it", CANVAS_GREEN,
                    board == 2 ? "SEQ PLAY OK" : "PLAY OK");
        SAVE("test-ok");
        screen_test(board, "Neighbours 5G", NET_STEP_JOINING, true, "Wrong password", &heard_only,
                    board == 2 ? "SEQ PLAY: save it anyway" : "PLAY: save it anyway", CANVAS_RED,
                    board == 2 ? "SEQ REC EDIT" : "REC EDIT");
        SAVE("test-failed");
        screen_type(board, "Network name", 0, "My hidden network", 0, TEXT_LOWER, 0, false, 0);
        SAVE("type-name");
        screen_test(board, "Workshop with a name too long to fit", NET_STEP_FINDING, false, 0, &ok, "Testing", CANVAS_DIM, "");
        SAVE("test-long-name");
        screen_test(board, "Far away", NET_STEP_FINDING, true, "Not heard here", &(struct net_result){0}, "", CANVAS_DIM,
                    "");
        SAVE("test-not-heard");
    }
    printf("%u screens\n", saved);
    return 0;
}
