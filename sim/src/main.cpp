// Desktop simulator: runs the real player UI (src/ui.cpp) in an SDL2 window with fake Spotify data.
// The mouse is the finger; close the window to quit.
#include "board.h"
#include "config.h"
#include "display.h"
#include "ui.h"
#include <SDL.h>
#include <lvgl.h>

void display_present();
bool display_snapshot(const char *path);
void fake_spotify_tick();

static String simInfo() {
    return "Simulator (no hardware)\nIP: 192.168.1.42\nSSID: Demo Wi-Fi (ch 6)\nSignal: -52 dBm (excellent)\n"
           "Up 0h 12m, reset: power\nSpotify: token ok, poll 180 ms";
}

int main(int, char **) {
    // A few pins so the playlist list starts populated, and the clock follows the Mac's own time zone.
    g_config.pins = {{"p0", "Morning Focus"}, {"p1", "Late Night Drive"}};
    if (getenv("SIM_IDLE_MINUTES")) g_config.idleMinutes = atoi(getenv("SIM_IDLE_MINUTES"));
    if (getenv("SIM_IDLE_CLOCK")) g_config.idleClock = true;

    display_init();
    display_set_brightness(g_config.screenBrightness);
    ui_init();
    ui_set_info_provider(simInfo);
    show_player_screen();

    // SIM_SNAPSHOT=file.bmp saves the screen after SIM_SNAPSHOT_MS (default 3000) and quits.
    const char *snapshot = getenv("SIM_SNAPSHOT");
    uint32_t snapshotAt = getenv("SIM_SNAPSHOT_MS") ? atoi(getenv("SIM_SNAPSHOT_MS")) : 3000;

    uint32_t lastTick = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return 0;
        }
        if (millis() - lastTick >= 250) {
            lastTick = millis();
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
        display_present();
        if (snapshot && millis() > snapshotAt) {
            bool ok = display_snapshot(snapshot);
            printf("Snapshot %s: %s\n", snapshot, ok ? "saved" : "failed");
            return ok ? 0 : 1;
        }
    }
}
