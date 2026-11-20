/* SPDX-License-Identifier: MIT */
/* Draws the MP3 player's screens on a computer, from the same code, into PPM files: mp3_ui_host OUT_DIR. Also checks
 * the analyser's levels on test tones, and that what's sent matches what's drawn. */
#include "pixel.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t screen[240][240];
static unsigned sent_bytes;

void display_pixels(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *data) {
    if (x+w > 240 || y+h > 240 || !w || !h) {
        fprintf(stderr, "display_pixels out of the screen: %u %u %u %u\n", x, y, w, h);
        exit(2);
    }
    for (unsigned r = 0; r < h; ++r)
        for (unsigned c = 0; c < w; ++c, data += 2)
            screen[y+r][x+c] = (uint16_t)(data[0] << 8 | data[1]);
    sent_bytes += w*h*2;
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
    printf("%s: %u bytes sent since the last\n", name, sent_bytes);
    sent_bytes = 0;
}

static const char *names[] = {
    "Daft Punk - Around the World", "Aphex Twin - Windowlicker", "Boards of Canada - Roygbiv",
    "The Prodigy - Firestarter (Extended Mix)", "Moby - Porcelain", "Air - La Femme d'Argent",
    "Fatboy Slim - Right Here, Right Now", "Underworld - Born Slippy .NUXX"};

static const char *name(unsigned track) {
    return names[track%8];
}

static int16_t pcm[1152*2];

static void tone(double hz, double amplitude) {
    for (unsigned i = 0; i < 1152; ++i) {
        int16_t v = (int16_t)(amplitude*32767*sin(2*M_PI*hz*i/44100));
        pcm[2*i] = pcm[2*i+1] = v;
    }
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const char *dir = argv[1];
    uint8_t levels[SPECTRUM_BANDS];

    /* The analyser on tones: a full-scale tone fills its band, a quiet one shows low, silence shows nothing. */
    static const double hz[] = {100, 1000, 10000};
    for (unsigned i = 0; i < 3; ++i) {
        for (double a = 1.0; a > 0.0005; a /= 10) {
            tone(hz[i], a);
            spectrum_run(pcm, 2, 1152, levels);
            unsigned most = 0, band = 0;
            for (unsigned b = 0; b < SPECTRUM_BANDS; ++b)
                if (levels[b] > most)
                    most = levels[b], band = b;
            printf("%5.0f Hz at %6.4f: band %2u level %2u\n", hz[i], a, band, most);
            if (a == 1.0 && most < SPECTRUM_TOP-1) {
                fprintf(stderr, "a full-scale tone shows only %u\n", most);
                return 1;
            }
        }
    }
    memset(pcm, 0, sizeof pcm);
    spectrum_run(pcm, 2, 1152, levels);
    for (unsigned b = 0; b < SPECTRUM_BANDS; ++b)
        if (levels[b]) {
            fprintf(stderr, "silence shows %u in band %u\n", levels[b], b);
            return 1;
        }

    struct ui_model m = {.mode = UI_STOPPED, .count = 8, .selected = 0, .playing = -1, .name = name};
    ui_start(1);
    save(dir, "0-start");
    ui_update(&m, 0);
    save(dir, "1-stopped");
    m.selected = 6;
    ui_update(&m, 20);
    save(dir, "2-selected-6");

    /* Playing, with a mix of tones for the bars. */
    m.mode = UI_PLAYING;
    m.playing = 3;
    m.selected = 3;
    m.elapsed = 83;
    m.total = 245;
    m.kbps = 192;
    m.hz = 44100;
    m.channels = 2;
    for (unsigned i = 0; i < 1152; ++i) {
        double t = i/44100.0, v = 0.5*sin(2*M_PI*80*t)+0.25*sin(2*M_PI*440*t)+0.12*sin(2*M_PI*2500*t)+
                                 0.05*sin(2*M_PI*9000*t);
        pcm[2*i] = pcm[2*i+1] = (int16_t)(v*32767);
    }
    spectrum_run(pcm, 2, 1152, levels);
    m.levels = levels;
    for (unsigned k = 0; k < 10; ++k)
        ui_update(&m, 30);
    save(dir, "3-playing");
    m.keys = 2;
    ui_update(&m, 30);
    save(dir, "4-next-held");
    m.keys = 0;
    m.mode = UI_PAUSED;
    for (unsigned k = 0; k < 8; ++k)
        ui_update(&m, 100);
    save(dir, "5-paused");

    struct ui_model empty = {.mode = UI_STOPPED, .count = 0, .playing = -1, .note = "NO MP3 FILES", .name = name};
    ui_start(2);
    ui_update(&empty, 0);
    save(dir, "6-no-files-smk37");

    /* What's on the screen has to be what was drawn: draw it all again from the start, and compare. */
    static uint16_t before[240][240];
    ui_start(1);
    m.mode = UI_PLAYING;
    m.levels = levels;
    /* Sent a little at a time, as while playing, with the track changing part way. */
    pix_budget(4096);
    for (unsigned k = 0; k < 50; ++k) {
        m.elapsed = 83+k/10;
        if (k == 20)
            m.playing = 5, m.selected = 5;
        ui_update(&m, 37);
    }
    for (unsigned k = 0; k < 40; ++k)
        ui_update(&m, 0);
    pix_budget(0);
    memcpy(before, screen, sizeof before);
    memset(screen, 0, sizeof screen);
    ui_start(1);
    struct ui_model same = m;
    ui_update(&same, 0);
    /* Animations differ (bars and scrolling), so compare the parts that don't move: the list and below. */
    for (unsigned y = 2*66; y < 240; ++y)
        if (memcmp(before[y], screen[y], sizeof screen[y])) {
            fprintf(stderr, "row %u differs after updates\n", y);
            return 1;
        }
    printf("OK\n");
    return 0;
}
