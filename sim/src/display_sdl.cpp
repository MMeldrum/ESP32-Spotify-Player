// SDL2 window standing in for the panel, touch controller and backlight.
#include "display.h"
#include "board.h"
#include <Arduino.h>
#include <SDL.h>
#include <lvgl.h>

namespace {

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
uint16_t frame[SCREEN_W * SCREEN_H];
lv_color_t drawPixels[SCREEN_W * 60];
lv_disp_draw_buf_t drawBuf;
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

int brightness = 100;
bool screenOn = true;
bool swallowTouch = false;
void (*touchWakeCb)() = nullptr;

void applyBacklight() {
    // Same curve as the PWM on the board: perceived brightness ~ duty squared.
    int level = screenOn ? 255 * brightness * brightness / 10000 : 0;
    SDL_SetTextureColorMod(texture, level, level, level);
}

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&frame[y * SCREEN_W + area->x1], &pixels[(y - area->y1) * w], w * sizeof(uint16_t));
    }
    lv_disp_flush_ready(drv);
}

void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    int x, y;
    uint32_t buttons = SDL_GetMouseState(&x, &y);
    int ww, wh;
    SDL_GetWindowSize(window, &ww, &wh);
    data->point.x = constrain(x * SCREEN_W / ww, 0, SCREEN_W - 1);
    data->point.y = constrain(y * SCREEN_H / wh, 0, SCREEN_H - 1);
    bool pressed = (buttons & SDL_BUTTON_LMASK) && (SDL_GetMouseFocus() == window);
    data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    // As on the board: while the screen is off, the first touch only wakes it.
    if (!screenOn && pressed) {
        display_set_screen_on(true);
        swallowTouch = true;
        lv_disp_trig_activity(nullptr);
        if (touchWakeCb) touchWakeCb();
    }
    if (swallowTouch) {
        if (!pressed) swallowTouch = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

} // namespace

void display_init() {
    SDL_Init(SDL_INIT_VIDEO);
    // SIM_ZOOM scales the window (e.g. 2 for a sharper recording on a large screen).
    const char *zoomEnv = getenv("SIM_ZOOM");
    int zoom = zoomEnv ? constrain(atoi(zoomEnv), 1, 4) : 1;
    window = SDL_CreateWindow("Spotify Player simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W * zoom,
                              SCREEN_H * zoom, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");  // crisp pixels at SIM_ZOOM 2-4
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    applyBacklight();

    lv_init();
    lv_disp_draw_buf_init(&drawBuf, drawPixels, nullptr, SCREEN_W * 60);
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = SCREEN_W;
    dispDrv.ver_res = SCREEN_H;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchReadCb;
    indevDrv.long_press_time = 700;
    lv_indev_drv_register(&indevDrv);
}

void display_present() {
    SDL_UpdateTexture(texture, nullptr, frame, SCREEN_W * sizeof(uint16_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

// Saves what the window shows (backlight included) as a 24-bit BMP, for stills and checks.
bool display_snapshot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int level = screenOn ? 255 * brightness * brightness / 10000 : 0;
    const uint32_t rowBytes = SCREEN_W * 3, imageBytes = rowBytes * SCREEN_H;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
    put32(2, 54 + imageBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, SCREEN_W);
    put32(22, (uint32_t)-SCREEN_H);  // top-down rows
    header[26] = 1;
    header[28] = 24;
    put32(34, imageBytes);
    fwrite(header, 1, sizeof(header), f);
    uint8_t row[SCREEN_W * 3];
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t p = frame[y * SCREEN_W + x];
            uint8_t r = ((p >> 11) & 0x1F) * 255 / 31, g = ((p >> 5) & 0x3F) * 255 / 63, b = (p & 0x1F) * 255 / 31;
            row[x * 3] = b * level / 255;
            row[x * 3 + 1] = g * level / 255;
            row[x * 3 + 2] = r * level / 255;
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    return true;
}

void display_set_brightness(int percent) {
    brightness = constrain(percent, DISPLAY_MIN_BRIGHTNESS, 100);
    applyBacklight();
}

void display_set_screen_on(bool on) {
    screenOn = on;
    applyBacklight();
}

bool display_screen_on() { return screenOn; }

void display_on_touch_wake(void (*callback)()) { touchWakeCb = callback; }
