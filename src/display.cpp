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
#else
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#endif

namespace {

lv_disp_draw_buf_t drawBuf;
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

#if defined(BOARD_ESP32_4848S040)

constexpr int PIN_BL = 38;
constexpr int TOUCH_SDA = 19;
constexpr int TOUCH_SCL = 45;
constexpr uint8_t GT911_ADDRS[2] = {0x5D, 0x14};
constexpr uint16_t GT911_STATUS = 0x814E;
constexpr uint16_t GT911_POINT1 = 0x8150;
constexpr uint32_t PCLK_HZ = 12000000;
constexpr bool DIRECT_MODE = true;

// The ST7701S is initialised over 3-wire SPI; pixels then stream over the 16-bit RGB bus.
Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED /* DC */, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

// The panel is scanned continuously from one PSRAM framebuffer. LVGL draws complete frames into its own
// buffer, and each finished frame is copied over right after a vsync, so partial redraws never show.
esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t vsyncSem = nullptr;

// A full-frame copy (~26 ms) is slightly slower than one scan-out (~24 ms at 12 MHz). Starting the copy a
// few ms after vsync lets the scan-out get ahead first, so it never catches up with the copy.
constexpr uint32_t COPY_DELAY_MS = 6;
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

// Direct mode: LVGL has already drawn into the full-screen buffer, so only the dirty rows are collected
// here and copied to the panel, top to bottom, once the last area of the frame is done.
void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    addDirtyRows(area->y1, area->y2);
    if (lv_disp_flush_is_last(drv)) {
        std::sort(dirty, dirty + dirtyCount, [](const RowRange &a, const RowRange &b) { return a.y1 < b.y1; });
        xSemaphoreTake(vsyncSem, 0);
        xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(COPY_DELAY_MS));
        for (int i = 0; i < dirtyCount; i++) {
            esp_lcd_panel_draw_bitmap(panel, 0, dirty[i].y1, SCREEN_W, dirty[i].y2 + 1, pixels + dirty[i].y1 * SCREEN_W);
        }
        dirtyCount = 0;
    }
    lv_disp_flush_ready(drv);
}

void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    static int16_t lastX = 0, lastY = 0;
    static bool lastPressed = false;
    data->point.x = lastX;
    data->point.y = lastY;
    data->state = lastPressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    if (!touchAddr) return;

    // Bit 7 is set once a new report is ready; between reports keep the previous state.
    uint8_t status = 0;
    if (!gt911Read(GT911_STATUS, &status, 1) || !(status & 0x80)) return;
    uint8_t buf[4];
    lastPressed = (status & 0x0F) && gt911Read(GT911_POINT1, buf, sizeof(buf));
    uint8_t clear = 0;
    gt911Write(GT911_STATUS, &clear, 1);
    if (!lastPressed) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    lastX = constrain(buf[0] | (buf[1] << 8), 0, SCREEN_W - 1);
    lastY = constrain(buf[2] | (buf[3] << 8), 0, SCREEN_H - 1);
    data->point.x = lastX;
    data->point.y = lastY;
    data->state = LV_INDEV_STATE_PR;
}

void hardwareInit() {
    panelInit();
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);
    touchInit();

    size_t bufPixels = SCREEN_W * SCREEN_H;
    auto *buf = (lv_color_t *)heap_caps_calloc(bufPixels, sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&drawBuf, buf, nullptr, bufPixels);
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

void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    if (touch.tirqTouched() && touch.touched()) {
        TS_Point p = touch.getPoint();
        data->point.x = constrain(map(p.x, 200, 3700, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
        data->point.y = constrain(map(p.y, 240, 3800, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
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

#endif

} // namespace

void display_init() {
    lv_init();
    hardwareInit();

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
