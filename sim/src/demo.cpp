// Scripted demo recording: drives the UI with timed taps, swipes and drags, writes every frame as raw RGB24
// (480x480, 30 fps) and the captions as "start_ms end_ms text" lines. sim/make_demo_video.py turns them into
// a video. Coordinates are screen pixels on the 480x480 ESP32-4848S040 layout.
#include "board.h"
#include "config.h"
#include "ui.h"
#include <cmath>
#include <ctime>
#include <functional>
#include <lvgl.h>
#include <string>
#include <vector>

extern bool g_virtualClock;
extern uint32_t g_virtualMs;
void fake_spotify_tick();
void display_set_scripted_touch(bool (*touch)(int *x, int *y));
void display_frame_rgb(uint8_t *out);
void display_present();

namespace {

constexpr int FPS = 30;

struct Gesture {
    uint32_t start, end;  // finger down during [start, end)
    int x1, y1, x2, y2;   // moves linearly over moveMs, then stays
    uint32_t moveMs;
};
struct Action {
    uint32_t at;
    std::function<void()> fn;
};
struct Caption {
    uint32_t start, end;
    std::string text;
};
struct Skip {
    uint32_t at, ms;
};

std::vector<Gesture> gestures;
std::vector<Action> actions;
std::vector<Caption> captions;
std::vector<Skip> skips;
uint32_t t = 0;        // script time = video time
uint32_t videoMs = 0;  // current frame's video time

void caption(const std::string &text) {
    if (!captions.empty() && captions.back().end == 0) captions.back().end = t;
    captions.push_back({t, 0, text});
}
void wait(uint32_t ms) { t += ms; }
void act(std::function<void()> fn) { actions.push_back({t, fn}); }
void press(int x1, int y1, int x2, int y2, uint32_t moveMs, uint32_t downMs) {
    gestures.push_back({t, t + downMs, x1, y1, x2, y2, moveMs});
    t += downMs;
}
void tap(int x, int y, uint32_t after = 900) {
    press(x, y, x, y, 0, 110);
    wait(after);
}
void hold(int x, int y, uint32_t ms) { press(x, y, x, y, 0, ms); }
// A quick flick (for LVGL's swipe gesture).
void swipe(int x1, int y1, int x2, int y2) { press(x1, y1, x2, y2, 220, 240); }
// A slow drag that pauses before lifting, so nothing flings.
void drag(int x1, int y1, int x2, int y2, uint32_t moveMs) { press(x1, y1, x2, y2, moveMs, moveMs + 250); }
// Jump the clock forward without recording (e.g. waiting for the idle clock).
void skip(uint32_t ms) { skips.push_back({t, ms}); }

bool fingerAt(uint32_t ms, int *x, int *y) {
    for (const Gesture &g : gestures) {
        if (ms < g.start || ms >= g.end) continue;
        float f = g.moveMs ? std::min(1.0f, (ms - g.start) / (float)g.moveMs) : 1.0f;
        *x = (int)lroundf(g.x1 + (g.x2 - g.x1) * f);
        *y = (int)lroundf(g.y1 + (g.y2 - g.y1) * f);
        return true;
    }
    return false;
}

bool scriptedTouch(int *x, int *y) { return fingerAt(videoMs, x, y); }

// Player layout (480x480): top bar y 28; extra row y 357; progress y 396; controls y 439.
constexpr int TOP_Y = 28, ROW_Y = 357, BAR_Y = 396, CTRL_Y = 439;
constexpr int SETTINGS_X = 250, DEVICES_X = 337, LISTS_X = 424;
constexpr int SHUFFLE_X = 150, LIKE_X = 240, REPEAT_X = 330;
constexpr int VOLDN_X = 62, PREV_X = 145, PLAY_X = 240, NEXT_X = 334, VOLUP_X = 417;
constexpr int BACK_X = 48, BACK_Y = 30;
constexpr int ROW0_Y = 87, ROW1_Y = 138;  // list rows
constexpr int EMPTY_X = 240, EMPTY_Y = 110;  // art only: swipes and taps go to the screen
constexpr int LONG_X = 120, LONG_Y = 70;     // below the top bar, above the text: a long press reaches the screen

void buildScript() {
    caption("ESP32 Spotify Player\nThe 4.0\" 480x480 UI, shown in the desktop simulator with demo music");
    wait(4500);
    caption("Now playing: title, artist and album art.\nThe colours follow the art.");
    wait(4500);

    caption("Play and pause");
    tap(EMPTY_X, EMPTY_Y, 700);  // the controls faded during the intro: the first tap brings them back
    tap(PLAY_X, CTRL_Y, 1600);
    tap(PLAY_X, CTRL_Y, 1200);

    caption("Skip to the next track, or back");
    tap(NEXT_X, CTRL_Y, 2400);
    tap(PREV_X, CTRL_Y, 2400);

    caption("Tap the progress bar to jump to that point");
    tap(360, BAR_Y, 1600);
    tap(150, BAR_Y, 1600);

    caption("Volume up and down; the level shows top left");
    tap(VOLUP_X, CTRL_Y, 700);
    tap(VOLUP_X, CTRL_Y, 700);
    tap(VOLDN_X, CTRL_Y, 1600);

    caption("Swipe left for the next track, right for the previous one");
    wait(600);
    swipe(400, EMPTY_Y, 110, EMPTY_Y);
    wait(2400);
    swipe(90, EMPTY_Y, 380, EMPTY_Y);
    wait(2400);

    caption("Shuffle, like and repeat light up in the album colour when on");
    tap(SHUFFLE_X, ROW_Y, 900);
    tap(LIKE_X, ROW_Y, 900);
    tap(REPEAT_X, ROW_Y, 900);
    caption("Repeat cycles: off, whole album or playlist, this track");
    tap(REPEAT_X, ROW_Y, 1300);
    tap(REPEAT_X, ROW_Y, 900);
    tap(SHUFFLE_X, ROW_Y, 1400);

    caption("The previous track sits above the title, the next one below");
    wait(3000);
    caption("Tap \"Next\" to see the queue");
    tap(240, 315, 3200);
    tap(EMPTY_X, EMPTY_Y, 900);
    caption("Tap \"Previous\" for recently played,\nthen tap a track to play it again");
    tap(240, 200, 2600);
    tap(240, ROW1_Y, 2800);

    caption("Tap the title for track details, with Play album");
    tap(240, 240, 3000);
    tap(333, 424, 2800);

    caption("Choose where Spotify plays");
    tap(DEVICES_X, TOP_Y, 2200);
    caption("Phones don't allow remote volume,\nso the volume buttons grey out");
    tap(240, ROW1_Y, 3800);
    tap(DEVICES_X, TOP_Y, 1600);
    tap(240, ROW0_Y, 1500);

    caption("Playlists: pinned ones first; tap one to play it");
    tap(LISTS_X, TOP_Y, 2600);
    tap(240, ROW1_Y, 2600);

    caption("While music plays, the controls fade after 5 seconds\nand the text moves down");
    wait(7500);
    caption("A tap brings the controls back");
    tap(EMPTY_X, EMPTY_Y, 2200);

    caption("Long-press for device and network info");
    hold(LONG_X, LONG_Y, 1100);
    wait(3000);
    tap(LONG_X, LONG_Y, 900);

    // Prepared off camera: night mode covers the current hour, and a 1-minute idle time for the clock.
    act([] {
        time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        g_config.nightFrom = local.tm_hour;
        g_config.nightTo = (local.tm_hour + 1) % 24;
        g_config.nightBrightness = 45;
        g_config.idleMinutes = 1;
    });
    caption("Settings (gear button): brightness changes as you drag");
    tap(SETTINGS_X, TOP_Y, 1500);
    drag(432, 132, 150, 132, 1300);
    wait(700);
    drag(150, 132, 432, 132, 1100);
    wait(1000);

    caption("Night mode: a dimmer screen between two hours you choose");
    tap(406, 201, 3500);
    tap(406, 201, 1500);

    caption("Pick your time zone; summer time is automatic");
    tap(240, 450, 2600);
    tap(240, 450, 1200);

    caption("When idle: switch the screen off, or show a clock");
    drag(240, 430, 240, 110, 800);
    wait(1200);
    tap(240, 360, 1400);  // the list opens upwards: "Show the clock" is its second row
    tap(240, 308, 1800);

    caption("The previous and next lines can be hidden");
    tap(406, 435, 1800);
    drag(240, 191, 240, 470, 800);  // starts on the "Time zone" label, not on a slider
    wait(800);
    tap(BACK_X, BACK_Y, 3000);

    caption("After the idle time (here 1 minute), the clock appears over the art");
    wait(400);
    skip(61000);
    wait(4500);
    caption("A tap goes back to the player");
    tap(240, 240, 3000);

    caption("Also on the board: Wi-Fi setup, Spotify login, playlist pins\nand firmware updates from its web page");
    wait(5500);
    caption("");
    captions.back().end = t;
}

// A soft circle where the finger is, and a short ring after it lifts.
void drawFinger(uint8_t *rgb) {
    int x, y;
    static int lastX = -1, lastY = -1;
    static uint32_t releasedAt = 0;
    bool down = fingerAt(videoMs, &x, &y);
    float alpha;
    int radius;
    if (down) {
        lastX = x;
        lastY = y;
        releasedAt = 0;
        alpha = 0.45f;
        radius = 24;
    } else {
        if (lastX < 0) return;
        if (!releasedAt) releasedAt = videoMs;
        uint32_t since = videoMs - releasedAt;
        if (since > 300) return;
        x = lastX;
        y = lastY;
        alpha = 0.45f * (1 - since / 300.0f);
        radius = 24 + since / 15;
    }
    for (int py = y - radius - 2; py <= y + radius + 2; py++) {
        for (int px = x - radius - 2; px <= x + radius + 2; px++) {
            if (px < 0 || py < 0 || px >= SCREEN_W || py >= SCREEN_H) continue;
            float d = hypotf(px - x, py - y);
            float a = d <= radius - 2 ? alpha : d <= radius + 1 ? std::min(1.0f, alpha * 2) : 0;
            if (a <= 0) continue;
            uint8_t *p = rgb + (py * SCREEN_W + px) * 3;
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (1 - a) + 255 * a);
        }
    }
}

} // namespace

int run_demo() {
    buildScript();
    const char *outPath = getenv("SIM_RECORD");
    FILE *out = !outPath || strcmp(outPath, "-") == 0 ? stdout : fopen(outPath, "wb");
    const char *capPath = getenv("SIM_CAPTIONS") ? getenv("SIM_CAPTIONS") : "demo_captions.txt";
    FILE *caps = fopen(capPath, "w");
    if (!out || !caps) {
        fprintf(stderr, "Cannot open the output files\n");
        return 1;
    }
    for (const Caption &c : captions) {
        std::string text = c.text;
        for (char &ch : text) {
            if (ch == '\n') ch = '|';
        }
        fprintf(caps, "%u %u %s\n", c.start, c.end, text.c_str());
    }
    fclose(caps);

    display_set_scripted_touch(scriptedTouch);
    std::vector<uint8_t> rgb(SCREEN_W * SCREEN_H * 3);
    uint32_t offset = 0, lastTick = 0;
    size_t nextAction = 0, nextSkip = 0;
    for (uint32_t frame = 0;; frame++) {
        videoMs = frame * 1000 / FPS;
        if (videoMs > t) break;
        while (nextSkip < skips.size() && skips[nextSkip].at <= videoMs) offset += skips[nextSkip++].ms;
        g_virtualMs = videoMs + offset;
        while (nextAction < actions.size() && actions[nextAction].at <= videoMs) actions[nextAction++].fn();

        if (g_virtualMs - lastTick >= 250) {
            lastTick = g_virtualMs;
            fake_spotify_tick();
            PlayerState state;
            spotify_get_state(state);
            ui_update_player(state);
            if (spotify_art_begin_read()) {
                ui_show_art();
                spotify_art_end_read();
            }
        }
        lv_timer_handler();
        if (frame % 2 == 0) display_present();
        display_frame_rgb(rgb.data());
        drawFinger(rgb.data());
        fwrite(rgb.data(), 1, rgb.size(), out);
        if (frame % (FPS * 10) == 0) fprintf(stderr, "Recorded %us of %us\n", videoMs / 1000, t / 1000);
    }
    if (out != stdout) fclose(out);
    fprintf(stderr, "Done: %u ms of video, captions in %s\n", t, capPath);
    return 0;
}
