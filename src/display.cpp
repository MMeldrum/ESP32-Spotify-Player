#include "display.h"
#include <algorithm>
#include "board.h"
#include <lvgl.h>

#if defined(BOARD_ESP32_4848S040)
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_timer.h>
#else
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#endif

namespace {

lv_disp_draw_buf_t drawBuf;
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

int rotation = 0;  // quarter turns clockwise

#if defined(BOARD_ESP32_4848S040)

constexpr int PIN_BL = 38;
constexpr int TOUCH_SDA = 19;
constexpr int TOUCH_SCL = 45;
constexpr uint8_t GT911_ADDRS[2] = {0x5D, 0x14};
constexpr uint16_t GT911_STATUS = 0x814E;
constexpr uint16_t GT911_POINT1 = 0x8150;
constexpr uint32_t TOUCH_STALE_MS = 400;
constexpr uint32_t PCLK_HZ = 12000000;
constexpr bool DIRECT_MODE = true;

// The ST7701S is initialised over 3-wire SPI; pixels then stream over the 16-bit RGB bus.
Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED /* DC */, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

// The panel is scanned continuously from one PSRAM framebuffer. LVGL draws complete frames into one of
// its two buffers, and each finished frame is copied over by a task on the other core, timed against the
// scan-out, so partial redraws never show. LVGL draws the next frame into its other buffer meanwhile.
esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t vsyncSem = nullptr;
volatile uint32_t lastVsyncUs = 0;

// A full-frame copy (~26 ms) is slightly slower than one scan-out (~22 ms of rows, ~24 ms per frame at
// 12 MHz). A copy started 6-14 ms after the end of a frame starts behind the scan-out, which then keeps
// ahead of it until the copy is done, so it never shows half-copied rows. Outside that window the copy
// waits for the next frame.
constexpr uint32_t COPY_EARLIEST_US = 6000;
constexpr uint32_t COPY_LATEST_US = 14000;
constexpr int MAX_DIRTY = 16;

struct RowRange {
    int y1, y2;
};
RowRange dirty[MAX_DIRTY];
int dirtyCount = 0;

void addDirtyRows(int y1, int y2) {
    for (int i = 0; i < dirtyCount; i++) {
        if (y1 <= dirty[i].y2 + 1 && y2 >= dirty[i].y1 - 1) {
            dirty[i].y1 = min(dirty[i].y1, y1);
            dirty[i].y2 = max(dirty[i].y2, y2);
            return;
        }
    }
    if (dirtyCount < MAX_DIRTY) {
        dirty[dirtyCount++] = {y1, y2};
    } else {
        dirty[MAX_DIRTY - 1].y1 = min(dirty[MAX_DIRTY - 1].y1, y1);
        dirty[MAX_DIRTY - 1].y2 = max(dirty[MAX_DIRTY - 1].y2, y2);
    }
}

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, esp_lcd_rgb_panel_event_data_t *, void *) {
    BaseType_t woken = pdFALSE;
    lastVsyncUs = (uint32_t)esp_timer_get_time();
    xSemaphoreGiveFromISR(vsyncSem, &woken);
    return woken == pdTRUE;
}

void panelInit() {
    bus->begin();
    bus->sendCommand(0x01); // software reset
    delay(120);
    // Some panel batches need st7701_type1_init_operations instead; type9 matches the library's board definition.
    bus->batchOperation((uint8_t *)st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

    vsyncSem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz = PCLK_HZ;
    cfg.timings.h_res = SCREEN_W;
    cfg.timings.v_res = SCREEN_H;
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 50;
    cfg.timings.hsync_front_porch = 10;
    cfg.timings.vsync_pulse_width = 8;
    cfg.timings.vsync_back_porch = 20;
    cfg.timings.vsync_front_porch = 10;
    cfg.data_width = 16;
    cfg.sram_trans_align = 8;
    cfg.psram_trans_align = 64;
    cfg.hsync_gpio_num = 16;
    cfg.vsync_gpio_num = 17;
    cfg.de_gpio_num = 18;
    cfg.pclk_gpio_num = 21;
    const int dataPins[16] = {4, 5, 6, 7, 15,         // B0-B4
                              8, 20, 3, 46, 9, 10,    // G0-G5
                              11, 12, 13, 14, 0};     // R0-R4
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = dataPins[i];
    cfg.disp_gpio_num = GPIO_NUM_NC;
    cfg.on_frame_trans_done = onVsync;
    cfg.flags.fb_in_psram = 1;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
}

constexpr int BL_PIN = PIN_BL;
constexpr bool BL_ACTIVE_HIGH = true;

uint8_t touchAddr = 0;

bool gt911Write(uint16_t reg, const uint8_t *data, size_t len) {
    Wire.beginTransmission(touchAddr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    for (size_t i = 0; i < len; i++) Wire.write(data[i]);
    return Wire.endTransmission() == 0;
}

bool gt911Read(uint16_t reg, uint8_t *data, size_t len) {
    if (!gt911Write(reg, nullptr, 0)) return false;
    if (Wire.requestFrom(touchAddr, (uint8_t)len) != len) return false;
    for (size_t i = 0; i < len; i++) data[i] = Wire.read();
    return true;
}

void touchInit() {
    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
    for (uint8_t addr : GT911_ADDRS) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            touchAddr = addr;
            break;
        }
    }
    if (!touchAddr) Serial.println("GT911 touch controller not found");
}

// Rotated frames are turned into this PSRAM buffer before the vsync wait, so the copy after it stays a
// plain memcpy that keeps ahead of the scan-out. Allocated on the first rotation.
lv_color_t *rotBuf = nullptr;

// Packs logical rows y1-y2 of the frame, turned, into rotBuf and returns the panel rectangle they cover.
// A quarter turn makes the rows a band of panel columns, packed column band row by row.
void rotateRows(int rotation, const lv_color_t *src, int y1, int y2, int &px1, int &py1, int &px2, int &py2) {
    constexpr int N = SCREEN_W;  // square panel
    const int rows = y2 - y1 + 1;
    if (rotation == 2) {
        px1 = 0, px2 = N, py1 = N - 1 - y2, py2 = N - y1;
        for (int py = py1; py < py2; py++) {
            const lv_color_t *in = src + (N - 1 - py) * N + N - 1;
            lv_color_t *out = rotBuf + (py - py1) * N;
            for (int px = 0; px < N; px++) *out++ = *in--;
        }
    } else if (rotation == 1) {
        // Logical (lx, ly) shows at panel (N-1-ly, lx).
        px1 = N - 1 - y2, px2 = N - y1, py1 = 0, py2 = N;
        for (int py = 0; py < N; py++) {
            lv_color_t *out = rotBuf + py * rows;
            for (int px = px1; px < px2; px++) *out++ = src[(N - 1 - px) * N + py];
        }
    } else {
        // Logical (lx, ly) shows at panel (ly, N-1-lx).
        px1 = y1, px2 = y2 + 1, py1 = 0, py2 = N;
        for (int py = 0; py < N; py++) {
            lv_color_t *out = rotBuf + py * rows;
            for (int px = px1; px < px2; px++) *out++ = src[px * N + N - 1 - py];
        }
    }
}

void waitForCopySlot() {
    uint32_t since = (uint32_t)esp_timer_get_time() - lastVsyncUs;
    if (since > COPY_LATEST_US) {
        xSemaphoreTake(vsyncSem, 0);
        xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(50));
        since = (uint32_t)esp_timer_get_time() - lastVsyncUs;
    }
    if (since < COPY_EARLIEST_US) delayMicroseconds(COPY_EARLIEST_US - since);
}

// One finished frame for the copy task: the LVGL buffer it is in and its dirty rows.
struct CopyJob {
    const lv_color_t *pixels;
    RowRange rows[MAX_DIRTY];
    int count;
    int rotation;
};
CopyJob job;
SemaphoreHandle_t jobReady = nullptr;  // given by the flush, taken by the copy task
SemaphoreHandle_t jobDone = nullptr;   // the previous job's buffer is free again

void copyFrame(const CopyJob &j) {
    if (j.rotation && rotBuf) {
        // One band covering every dirty row keeps the packing simple; rotated frames are rarer anyway.
        int y1 = SCREEN_H, y2 = -1;
        for (int i = 0; i < j.count; i++) {
            y1 = min(y1, j.rows[i].y1);
            y2 = max(y2, j.rows[i].y2);
        }
        int px1, py1, px2, py2;
        rotateRows(j.rotation, j.pixels, y1, y2, px1, py1, px2, py2);
        waitForCopySlot();
        esp_lcd_panel_draw_bitmap(panel, px1, py1, px2, py2, rotBuf);
        return;
    }
    waitForCopySlot();
    for (int i = 0; i < j.count; i++) {
        esp_lcd_panel_draw_bitmap(panel, 0, j.rows[i].y1, SCREEN_W, j.rows[i].y2 + 1, j.pixels + j.rows[i].y1 * SCREEN_W);
    }
}

void copyTask(void *) {
    for (;;) {
        xSemaphoreTake(jobReady, portMAX_DELAY);
        copyFrame(job);
        xSemaphoreGive(jobDone);
    }
}

// Direct mode: LVGL has already drawn into the full-screen buffer, so only the dirty rows are collected
// here and handed to the copy task once the last area of the frame is done. LVGL then switches to its
// other buffer, which the previous copy has finished with: the flush waits for that before it returns.
void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    addDirtyRows(area->y1, area->y2);
    if (lv_disp_flush_is_last(drv)) {
        xSemaphoreTake(jobDone, portMAX_DELAY);
        std::sort(dirty, dirty + dirtyCount, [](const RowRange &a, const RowRange &b) { return a.y1 < b.y1; });
        job.pixels = pixels;
        job.count = dirtyCount;
        job.rotation = rotation;
        memcpy(job.rows, dirty, sizeof(RowRange) * dirtyCount);
        dirtyCount = 0;
        xSemaphoreGive(jobReady);
    }
    lv_disp_flush_ready(drv);
}

bool applyRotation() {
    if (rotation && !rotBuf) {
        rotBuf = (lv_color_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (!rotBuf) {
            Serial.println("No memory for the rotation buffer");
            rotation = 0;
            return false;
        }
    }
    return true;
}

void readTouch(lv_indev_data_t *data) {
    static int16_t lastX = 0, lastY = 0;
    static bool lastPressed = false;
    static uint32_t lastReportMs = 0;
    data->point.x = lastX;
    data->point.y = lastY;
    if (!touchAddr) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // Bit 7 is set once a new report is ready; between reports keep the previous state. While a finger is
    // down the GT911 reports every ~10 ms, so a long silence means the release report was missed: without
    // this, LVGL would see a press that never ends (no fading controls, no idle clock, no screen off).
    uint8_t status = 0;
    if (!gt911Read(GT911_STATUS, &status, 1) || !(status & 0x80)) {
        if (lastPressed && millis() - lastReportMs > TOUCH_STALE_MS) lastPressed = false;
        data->state = lastPressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        return;
    }
    lastReportMs = millis();
    uint8_t buf[4];
    lastPressed = (status & 0x0F) && gt911Read(GT911_POINT1, buf, sizeof(buf));
    uint8_t clear = 0;
    gt911Write(GT911_STATUS, &clear, 1);
    if (!lastPressed) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    int px = constrain(buf[0] | (buf[1] << 8), 0, SCREEN_W - 1);
    int py = constrain(buf[2] | (buf[3] << 8), 0, SCREEN_H - 1);
    // Panel to logical coordinates: the inverse of rotateRows().
    switch (rotation) {
        case 1: lastX = py, lastY = SCREEN_W - 1 - px; break;
        case 2: lastX = SCREEN_W - 1 - px, lastY = SCREEN_H - 1 - py; break;
        case 3: lastX = SCREEN_H - 1 - py, lastY = px; break;
        default: lastX = px, lastY = py; break;
    }
    data->point.x = lastX;
    data->point.y = lastY;
    data->state = LV_INDEV_STATE_PR;
}

void hardwareInit() {
    panelInit();
    touchInit();

    size_t bufPixels = SCREEN_W * SCREEN_H;
    auto *buf1 = (lv_color_t *)heap_caps_calloc(bufPixels, sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    auto *buf2 = (lv_color_t *)heap_caps_calloc(bufPixels, sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&drawBuf, buf1, buf2, bufPixels);

    jobReady = xSemaphoreCreateBinary();
    jobDone = xSemaphoreCreateBinary();
    xSemaphoreGive(jobDone);
    // Core 0, away from LVGL on core 1 (the Arduino loop), so drawing and copying overlap.
    xTaskCreatePinnedToCore(copyTask, "lcd_copy", 4096, nullptr, 5, nullptr, 0);
}

#else

constexpr int DRAW_BUF_LINES = 10;
constexpr bool DIRECT_MODE = false;

constexpr int TS_CS = 33;
constexpr int TS_IRQ = 36;
constexpr int TS_CLK = 25;
constexpr int TS_MOSI = 32;
constexpr int TS_MISO = 39;

TFT_eSPI tft;

constexpr int BL_PIN = TFT_BL;
constexpr bool BL_ACTIVE_HIGH = TFT_BACKLIGHT_ON == HIGH;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TS_CS, TS_IRQ);

lv_color_t drawPixels[SCREEN_W * DRAW_BUF_LINES];

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&pixels->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

void readTouch(lv_indev_data_t *data) {
    if (touch.tirqTouched() && touch.touched()) {
        TS_Point p = touch.getPoint();
        int x = constrain(map(p.x, 200, 3700, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
        int y = constrain(map(p.y, 240, 3800, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
        // Upside down the panel is turned by the controller, the touch layer here.
        data->point.x = rotation == 2 ? SCREEN_W - 1 - x : x;
        data->point.y = rotation == 2 ? SCREEN_H - 1 - y : y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void hardwareInit() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(TS_CLK, TS_MISO, TS_MOSI, TS_CS);
    touch.begin(touchSPI);
    touch.setRotation(1);

    lv_disp_draw_buf_init(&drawBuf, drawPixels, nullptr, SCREEN_W * DRAW_BUF_LINES);
}

// The ILI9341 turns the picture itself: landscape (1) or landscape upside down (3).
bool applyRotation() {
    rotation &= 2;
    tft.setRotation(rotation ? 3 : 1);
    return true;
}

#endif

// The backlight is dimmed with PWM. Channel 7 is clear of the CYD's RGB LED channels (2-4).
constexpr int BL_CHANNEL = 7;
constexpr int BL_BITS = 10;
constexpr int BL_MAX = (1 << BL_BITS) - 1;
int brightness = 100;

bool screenOn = true;
bool swallowTouch = false;

void setBacklight(bool on) {
    // Perceived brightness is roughly the square of the duty cycle.
    uint32_t duty = on ? (uint32_t)BL_MAX * brightness * brightness / 10000 : 0;
    ledcWrite(BL_CHANNEL, BL_ACTIVE_HIGH ? duty : BL_MAX - duty);
}

void backlightInit() {
    // The backlight drivers are slow: at 20 kHz short pulses never light the LEDs and brightness only
    // changed near the top of the range. 1 kHz dims smoothly and is too fast to see flicker.
    ledcSetup(BL_CHANNEL, 1000, BL_BITS);
    ledcAttachPin(BL_PIN, BL_CHANNEL);
    setBacklight(true);
}
void (*touchWakeCb)() = nullptr;

void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    readTouch(data);
    bool pressed = data->state == LV_INDEV_STATE_PR;
    if (!screenOn && pressed) {
        display_set_screen_on(true);
        swallowTouch = true;
        lv_disp_trig_activity(nullptr);
        if (touchWakeCb) touchWakeCb();
    }
    // Hide the waking touch from LVGL until the finger lifts, so it does not press anything.
    if (swallowTouch) {
        if (!pressed) swallowTouch = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

} // namespace

void display_set_brightness(int percent) {
    brightness = constrain(percent, DISPLAY_MIN_BRIGHTNESS, 100);
    if (screenOn) setBacklight(true);
}

void display_set_screen_on(bool on) {
    if (on == screenOn) return;
    screenOn = on;
    setBacklight(on);
}

bool display_screen_on() { return screenOn; }

void display_on_touch_wake(void (*callback)()) { touchWakeCb = callback; }

void display_set_rotation(int quarterTurns) {
    quarterTurns &= 3;
    if (!DISPLAY_CAN_ROTATE_90) quarterTurns &= 2;
    if (quarterTurns == rotation) return;
    rotation = quarterTurns;
    applyRotation();
    // Redraw everything: the whole panel has to be turned, not just what changes next.
    lv_area_t all = {0, 0, SCREEN_W - 1, SCREEN_H - 1};
    _lv_inv_area(lv_disp_get_default(), &all);
}

int display_rotation() { return rotation; }

void display_init() {
    lv_init();
    hardwareInit();
    backlightInit();

    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = SCREEN_W;
    dispDrv.ver_res = SCREEN_H;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
    dispDrv.direct_mode = DIRECT_MODE;
    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchReadCb;
    indevDrv.long_press_time = 700;
    lv_indev_drv_register(&indevDrv);
}
