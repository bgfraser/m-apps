/* SPDX-License-Identifier: MIT */
/* Plays the MP3 files in the card's root, one after another, polled, in the look of a 90s desktop player (ui.c).
 * OCT- and OCT+ (< and > on the SMK-37) move through the playlist, and a track moved to starts at once; held while
 * playing, they rewind and fast-forward. PLAY starts and pauses, and on "back" it leaves, as REC (SEQ REC) does from
 * anywhere.
 *
 * Decoded sound goes into a ring of about 190 ms, and the DMA's half buffers are filled from it (pump()) between every
 * card read, decode and piece of the screen. Decoding keeps the ring full, and the screen is drawn when it is, so
 * nothing has to fit inside one half buffer's 26 ms. */
#include "app.h"
#include "audio.h"
#include "card.h"
#include "fat.h"
#include "spectrum.h"
#include "pixel.h"
#include "ui.h"
#include <string.h>
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define MOST_FILES 64u
#define CHUNK 4096u
#define INPUT (16u*1024u)
/* Samples are 16-bit; the DAC takes 24. */
#define SAMPLE_SHIFT 7
#define TIMER_CON REG(0x10800u)
#define TIMER_COUNT REG(0x10804u)
#define TIMER_PERIOD REG(0x10808u)
#define TICKS_PER_US 24u
#define TICKS_PER_MS 24000u
#define REPEAT_AFTER_MS 400u
#define REPEAT_EVERY_MS 120u
/* Holding OCT-/OCT+ while playing jumps this far, this often: about thirteen times as fast. */
#define SEEK_STEP_S 2u
#define SEEK_EVERY_MS 150u
/* PREV goes back to the start of the track after this long, and to the one before it sooner. */
#define RESTART_AFTER_S 3u
#define IDLE_POLL_US 10000u
/* The ring, in stereo frames: a power of two. */
#define RING 8192u
/* The most one decoded frame can make: 1152 samples, or 576 at 8 kHz made into 44.1 kHz. */
#define MOST_PER_FRAME 3200u
/* How often the screen and keys are looked at while playing. */
#define PLAY_UI_MS 15u
/* What the screen may send at a time while playing: about 10 ms at 96 MHz, then decoding catches up. */
#define PLAY_SCREEN_BYTES 12288u

static struct fat_named files[MOST_FILES];
static unsigned count, lsb;
static bool more;
static mp3dec_t decoder;
static uint8_t chunk[CHUNK] __attribute__((aligned(4)));
static uint8_t input[INPUT];
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static uint8_t levels[SPECTRUM_BANDS];
static struct ui_model model;

struct stats {
    uint32_t frames, short_halves, most_decode, most_read, most_draw, idle, busy;
};
static struct stats stats;

static void timer_start(void) {
    if ((TIMER_CON&0xfu) == ((2u << 2) | 1u))
        return;
    TIMER_CON = 0;
    TIMER_PERIOD = 0xffffffffu;
    TIMER_COUNT = 0;
    TIMER_CON = (2u << 2) | 1u | (1u << 14);
}

/* Milliseconds, from a timer that wraps every 179 s, so it has to be read more often than that. */
static uint32_t clock_ms, clock_ticks;

static uint32_t now_ms(void) {
    uint32_t t = TIMER_COUNT, whole = (t-clock_ticks)/TICKS_PER_MS;
    clock_ticks += whole*TICKS_PER_MS;
    clock_ms += whole;
    return clock_ms;
}

/* The ring: written at head, read at tail, both counting up for ever. */
static int16_t ring[RING][2];
static uint32_t ring_head, ring_tail;
/* Muted while paused. Primed once a half's worth has come after a clear, so the gap while a track starts isn't
 * counted as running short. */
static bool muted, primed;

static unsigned ring_level(void) {
    return ring_head-ring_tail;
}

static void ring_clear(void) {
    ring_tail = ring_head;
    primed = false;
}

static void ring_put(int32_t left, int32_t right) {
    ring[ring_head%RING][0] = (int16_t)left;
    ring[ring_head%RING][1] = (int16_t)right;
    ++ring_head;
}

/* Fills a free half buffer, if there is one, from the ring; or with silence while paused or stopped. A ring that runs
 * short is counted, and the rest of that half is silent. */
static void pump(void) {
    int32_t *h = audio_free_half();
    if (!h)
        return;
    unsigned n = muted ? 0 : ring_level();
    if (n >= AUDIO_HALF_FRAMES) {
        n = AUDIO_HALF_FRAMES;
        primed = true;
    } else if (!muted && primed && model.mode == UI_PLAYING) {
        ++stats.short_halves;
    }
    for (unsigned i = 0; i < n; ++i, ++ring_tail) {
        h[2*i] = (int32_t)ring[ring_tail%RING][0] << SAMPLE_SHIFT;
        h[2*i+1] = (int32_t)ring[ring_tail%RING][1] << SAMPLE_SHIFT;
    }
    memset(h+2*n, 0, 2*(AUDIO_HALF_FRAMES-n)*sizeof(int32_t));
    audio_filled();
}

static uint32_t ui_last_ms;

static void ui_now(void) {
    uint32_t now = now_ms();
    ui_update(&model, now-ui_last_ms);
    ui_last_ms = now;
}

/* The card and the screen share SPI1. */
static void card_off(void) {
    card_close();
}

static void card_on(void) {
    card_open(lsb);
    card_fast(lsb);
}

static const char *read_files(void) {
    card_open(lsb);
    uint32_t sectors = card_start(lsb);
    const char *error = "NO CARD FOUND";
    count = 0;
    if (sectors) {
        error = fat_mount(sectors);
        if (!error)
            error = fat_list_named(fat_root(), "MP3", files, MOST_FILES, &count, &more);
        if (!error && !count)
            error = "NO MP3 FILES";
    }
    card_close();
    for (unsigned i = 0; i < count; ++i)
        app_log("%s %u", files[i].name, (unsigned)files[i].file.size);
    return error;
}

/* The name for the screen, without the .MP3. The font has ASCII, so anything past it shows as '?'. */
static const char *track_name(unsigned track) {
    static char out[FAT_NAME_BYTES];
    unsigned n = 0;
    for (const uint8_t *p = (const uint8_t *)files[track].name; *p && n+1 < sizeof out; ++p) {
        if (*p < 0x80)
            out[n++] = (char)*p;
        else if (*p >= 0xc0)                    /* the first byte of a character */
            out[n++] = '?';
    }
    if (n >= 4 && out[n-4] == '.' && (out[n-3]|32) == 'm' && (out[n-2]|32) == 'p' && out[n-1] == '3')
        n -= 4;
    out[n] = 0;
    return out;
}

/* Keys, as events. Stopped, OCT-/OCT+ move when pressed, and repeat when held. Playing, a tap moves when let go, and
 * holding seeks instead. PLAY counts when pressed. */
#define EV_PREV 1u
#define EV_NEXT 2u
#define EV_SELECT 4u
#define EV_REWIND 8u
#define EV_FORWARD 16u
#define EV_LEAVE 32u

static struct {
    unsigned held;
    uint32_t repeat_at;
    bool long_press;
} keys;

static void keys_reset(void) {
    keys.held = board_keys();
    keys.long_press = true;                     /* a key held from before doesn't count */
}

static unsigned key_events(bool playing) {
    unsigned now = board_keys(), pressed = now&~keys.held, released = keys.held&~now, ev = 0;
    uint32_t t = now_ms();
    static const unsigned key[2] = {KEY_PREV, KEY_NEXT}, move[2] = {EV_PREV, EV_NEXT}, seek[2] = {EV_REWIND, EV_FORWARD};
    for (unsigned i = 0; i < 2; ++i) {
        if (pressed&key[i]) {
            keys.long_press = false;
            keys.repeat_at = t+REPEAT_AFTER_MS;
            if (!playing)
                ev |= move[i];
        } else if (now&key[i] && (int32_t)(t-keys.repeat_at) >= 0) {
            keys.long_press = true;
            ev |= playing ? seek[i] : move[i];
            keys.repeat_at = t+(playing ? SEEK_EVERY_MS : REPEAT_EVERY_MS);
        } else if (released&key[i] && playing && !keys.long_press) {
            ev |= move[i];
        }
    }
    if (pressed&KEY_SELECT)
        ev |= EV_SELECT;
    if (pressed&KEY_RECOVERY)
        ev |= EV_LEAVE;
    keys.held = now;
    model.keys = now;
    return ev;
}

/* A VBR file says its frame count in its first frame. Otherwise, the size and the bitrate say about how long it is. */
static uint32_t frames_said(const uint8_t *frame, unsigned size) {
    static const unsigned places[] = {36, 21, 13};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned at = places[i];
        if (at+12 > size || (memcmp(frame+at, "Xing", 4) && memcmp(frame+at, "Info", 4)))
            continue;
        if (frame[at+7]&1)
            return (uint32_t)frame[at+8] << 24 | (uint32_t)frame[at+9] << 16 | (uint32_t)frame[at+10] << 8 | frame[at+11];
    }
    return 0;
}

enum { PLAY_END, PLAY_SWITCH, PLAY_LEAVE, PLAY_FAILED };

/* The playlist's cursor, moved while playing. A different track starts at once; "back" changes nothing until PLAY. */
static int move_cursor(unsigned ev) {
    if (!(ev&(EV_PREV | EV_NEXT)))
        return -1;
    unsigned at = model.selected;
    if (ev&EV_PREV && at == (unsigned)model.playing && model.elapsed >= RESTART_AFTER_S)
        return PLAY_SWITCH;                     /* back to the start of this one */
    model.selected = ev&EV_PREV ? (at ? at-1 : count) : (at < count ? at+1 : 0);
    return model.selected < count && model.selected != (unsigned)model.playing ? PLAY_SWITCH : -1;
}

/* Paused: silence goes out, what's in the ring waits, the time blinks, and the cursor still moves. */
static int pause(void) {
    muted = true;
    model.mode = UI_PAUSED;
    for (;;) {
        pump();
        unsigned ev = key_events(true);
        if (ev&EV_LEAVE)
            return PLAY_LEAVE;
        if (ev&EV_SELECT) {
            if (model.selected == count)
                return PLAY_LEAVE;
            muted = false;
            model.mode = UI_PLAYING;
            return -1;
        }
        int action = move_cursor(ev);
        if (action >= 0) {
            muted = false;
            return action;
        }
        ui_now();
        app_feed();
        hal_delay_us(IDLE_POLL_US);
    }
}

/* Reads and decodes a track into the ring, with the audio running, until it ends or a key says otherwise. The end of
 * one track runs into the next, as the ring keeps what's left of it. */
static int play(unsigned track, const char **error) {
    const struct fat_entry *file = &files[track].file;
    uint32_t offset = 0, have = 0, step = 0, position = 0, samples = 0, total_frames = 0, per_frame = 0;
    uint32_t first_frame = 0, bytes_per_s = 0;
    unsigned hz = 0;
    int32_t before_left = 0, before_right = 0;
    bool first = true;
    int result = PLAY_END;
    memset(&stats, 0, sizeof stats);
    memset(&decoder, 0, sizeof(decoder));
    mp3dec_init(&decoder);
    model.mode = UI_PLAYING;
    model.playing = (int)track;
    model.elapsed = model.total = model.kbps = model.hz = model.channels = 0;
    *error = 0;
    card_on();
    uint32_t started = TIMER_COUNT, ui_at = now_ms();
    for (;;) {
        pump();
        if (RING-ring_level() >= MOST_PER_FRAME) {
            uint32_t a = TIMER_COUNT;
            /* Before a frame could run off the end of what's read. */
            while (have < INPUT-CHUNK && offset < file->size) {
                uint32_t n = file->size-offset < CHUNK ? file->size-offset : CHUNK;
                if (!fat_read(file, offset, chunk, n)) {
                    *error = "CARD READ ERROR";
                    break;
                }
                memcpy(input+have, chunk, n);
                have += n;
                offset += n;
                pump();
            }
            if (*error) {
                result = PLAY_FAILED;
                break;
            }
            uint32_t b = TIMER_COUNT;
            /* With no frame found, as in a tag bigger than input, it only sets frame_bytes. */
            mp3dec_frame_info_t info = {0};
            int decoded = mp3dec_decode_frame(&decoder, input, (int)have, pcm, &info);
            uint32_t c = TIMER_COUNT;
            pump();
            if (!info.frame_bytes)
                break;
            if (first && info.hz) {
                total_frames = frames_said(input+info.frame_offset, (unsigned)(info.frame_bytes-info.frame_offset));
                first_frame = offset-have+(uint32_t)info.frame_offset;
            }
            have -= (uint32_t)info.frame_bytes;
            memmove(input, input+info.frame_bytes, have);
            if ((b-a)/TICKS_PER_US > stats.most_read)
                stats.most_read = (b-a)/TICKS_PER_US;
            if ((c-b)/TICKS_PER_US > stats.most_decode)
                stats.most_decode = (c-b)/TICKS_PER_US;
            app_feed();
            if (first && info.hz) {
                first = false;
                hz = (unsigned)info.hz;
                per_frame = decoded ? (uint32_t)decoded : 1152u;
                uint32_t seconds = total_frames ? total_frames*per_frame/hz
                                   : info.bitrate_kbps ? (file->size-offset+have)/((unsigned)info.bitrate_kbps*125u) : 0;
                model.total = (unsigned)seconds;
                model.hz = hz;
                model.channels = (unsigned)info.channels;
                /* For seeking: about where a second starts, from the length if known, or else the bitrate. */
                bytes_per_s = seconds ? (file->size-first_frame)/seconds : (uint32_t)info.bitrate_kbps*125u;
                app_log("%u Hz, %d channels, %d kbps, %u s, %u frames said", hz, info.channels, info.bitrate_kbps,
                        model.total, (unsigned)total_frames);
                /* In 1/65536ths of a frame, from the rate the DAC really runs at. */
                step = hz == 44100u ? 0 : (uint32_t)(((uint64_t)hz*AUDIO_RATE_DEN << 16)/AUDIO_RATE_NUM);
            }
            if (!decoded)
                continue;
            ++stats.frames;
            samples += (uint32_t)decoded;
            for (int i = 0; i < decoded; ++i) {
                int32_t left = pcm[info.channels == 2 ? 2*i : i], right = pcm[info.channels == 2 ? 2*i+1 : i];
                if (!step) {
                    ring_put(left, right);
                    continue;
                }
                /* Linear, from the last frame to this one. */
                while (position < 65536u) {
                    int32_t t = (int32_t)position;
                    /* The difference can take 17 bits, so t gives up one to keep the product in 32. */
                    ring_put(before_left+(((left-before_left)*(t >> 1)) >> 15),
                             before_right+(((right-before_right)*(t >> 1)) >> 15));
                    position += step;
                }
                position -= 65536u;
                before_left = left;
                before_right = right;
            }
            pump();
            model.elapsed = hz ? samples/hz : 0;
            model.kbps = (unsigned)info.bitrate_kbps;
            spectrum_run(pcm, (unsigned)info.channels, (unsigned)decoded, levels);
            continue;
        }
        /* The ring is full: time for the keys and the screen. */
        uint32_t now = now_ms();
        if (now-ui_at < PLAY_UI_MS) {
            uint32_t idle = TIMER_COUNT;
            while (!audio_free_half() && now_ms()-ui_at < PLAY_UI_MS)
                ;
            stats.idle += (TIMER_COUNT-idle)/TICKS_PER_US;
            continue;
        }
        ui_at = now;
        unsigned ev = key_events(true);
        int action = -1;
        if (ev&EV_LEAVE)
            action = PLAY_LEAVE;
        else if (ev&EV_SELECT)
            action = model.selected == count ? PLAY_LEAVE : pause();
        else if (ev&(EV_REWIND | EV_FORWARD) && bytes_per_s) {
            /* A jump, then a moment of what's there, as a CD player does. */
            unsigned at = model.elapsed;
            unsigned to = ev&EV_FORWARD ? at+SEEK_STEP_S : at > SEEK_STEP_S ? at-SEEK_STEP_S : 0;
            uint32_t where = first_frame+to*bytes_per_s;
            if (where >= file->size) {
                action = PLAY_END;
            } else {
                offset = where;
                have = 0;
                mp3dec_init(&decoder);
                samples = to*hz;
                position = 0;
                before_left = before_right = 0;
                model.elapsed = to;
                ring_clear();
            }
        } else {
            action = move_cursor(ev);
        }
        if (action >= 0) {
            result = action;
            break;
        }
        uint32_t d = TIMER_COUNT;
        ui_now();
        uint32_t took = (TIMER_COUNT-d)/TICKS_PER_US;
        if (took > stats.most_draw)
            stats.most_draw = took;
    }
    stats.busy = (TIMER_COUNT-started)/TICKS_PER_US;
    card_off();
    /* Anything but the end drops what was waiting, so that it happens at once. */
    if (result != PLAY_END)
        ring_clear();
    return result;
}

/* Plays from a track on, until the list ends or PLAY on "back". Returns true for leaving the app. */
static bool play_from(unsigned track) {
    const char *error = 0;
    bool leave = false;
    ring_clear();
    muted = false;
    audio_start();
    pix_hooks(card_off, card_on, pump);
    pix_budget(PLAY_SCREEN_BYTES);
    keys_reset();
    for (;;) {
        int r = play(track, &error);
        app_log("track %u: %u frames in %u ms: %u short halves, longest read %u us, decode %u us, draw %u us, idle %u ms",
                track+1, (unsigned)stats.frames, (unsigned)(stats.busy/1000u), (unsigned)stats.short_halves,
                (unsigned)stats.most_read, (unsigned)stats.most_decode, (unsigned)stats.most_draw,
                (unsigned)(stats.idle/1000u));
        if (r == PLAY_END) {
            if (track+1 >= count)
                break;
            /* The cursor follows the music, unless it was put somewhere else. */
            if (model.selected == track)
                model.selected = track+1;
            ++track;
        } else if (r == PLAY_SWITCH) {
            track = model.selected;
        } else {
            leave = r == PLAY_LEAVE;
            if (error)
                model.note = error;
            break;
        }
    }
    /* What's left of the last track plays out, then silence in both halves, so that stopping doesn't leave the DAC
     * holding a sample. */
    while (ring_level() && !leave && !error) {
        pump();
        unsigned ev = key_events(true);
        if (ev&(EV_SELECT | EV_LEAVE)) {
            leave = ev&EV_LEAVE || model.selected == count;
            break;
        }
        ui_now();
    }
    ring_clear();
    muted = true;
    for (unsigned i = 0; i < 2; ++i) {
        while (!audio_free_half())
            ;
        pump();
    }
    audio_stop();
    pix_hooks(0, 0, 0);
    pix_budget(0);
    model.mode = UI_STOPPED;
    model.playing = -1;
    model.elapsed = 0;
    model.kbps = model.hz = model.channels = 0;
    keys_reset();
    return leave;
}

int app_main(unsigned lsb_mhz) {
    lsb = lsb_mhz;
    timer_start();
    now_ms();
    unsigned board = app_board();
    if (!board) {
        /* Its pins aren't known, so nothing is touched. */
        app_log("which board this is isn't known");
        return 1;
    }
    board_init(board == 2);
    display_init(board);
    memset(&model, 0, sizeof model);
    model.playing = -1;
    model.name = track_name;
    model.levels = levels;
    model.note = "READING THE CARD";
    ui_start(board);
    ui_now();
    model.note = read_files();
    model.count = count;
    app_log("%u files, LSB %u, audio clock stock %u", count, lsb_mhz, audio_clock_is_stock());
    keys_reset();
    for (;;) {
        unsigned ev = key_events(false);
        if (ev&EV_LEAVE)
            break;
        if (ev&EV_PREV)
            model.selected = model.selected ? model.selected-1 : count;
        else if (ev&EV_NEXT)
            model.selected = model.selected < count ? model.selected+1 : 0;
        else if (ev&EV_SELECT) {
            if (model.selected >= count || play_from(model.selected))
                break;
        }
        ui_now();
        app_feed();
        hal_delay_us(IDLE_POLL_US);
    }
    card_open(lsb);
    card_finish();
    card_close();
    board_restore();
    return 0;
}
