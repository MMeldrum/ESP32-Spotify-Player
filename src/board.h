#pragma once

// Per-board constants. The board is selected by the PlatformIO environment (see platformio.ini).

#if defined(BOARD_ESP32_4848S040)

#define BOARD_NAME "ESP32-4848S040"
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 480;
// The UI is laid out for 320x240 and scaled by UI_SCALE_NUM / UI_SCALE_DEN.
constexpr int UI_SCALE_NUM = 3;
constexpr int UI_SCALE_DEN = 2;
#define FONT_UI_SMALL font_ui_22
#define FONT_UI_LARGE font_ui_30
#define FONT_CLOCK font_clock_120  // digits and colon only
// No RGB LED, and GPIO 0 (BOOT) is a data line of the RGB panel.
#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_BOOT_BUTTON 0
// Downloads are buffered in PSRAM, not LittleFS: a flash write stalls PSRAM, which the RGB panel scans
// its framebuffer from, and the picture slips sideways.
#define BOARD_TEMP_IN_PSRAM 1
// Album art is scaled to fill the screen 1:1 (480x480, kept in PSRAM); the 640 px image is the default source.
constexpr int ART_BUFFER_W = SCREEN_W;
constexpr int DEFAULT_ART_SIZE = 640;
// default_16MB.csv has two app slots, so firmware can be updated from the web page.
#define BOARD_HAS_OTA 1
// The square screen has room for shuffle/like/repeat and the previous/next track lines.
#define BOARD_HAS_EXTRA_CONTROLS 1

#else

#define BOARD_NAME "ESP32-2432S028R"
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int UI_SCALE_NUM = 1;
constexpr int UI_SCALE_DEN = 1;
#define FONT_UI_SMALL font_ui_14
#define FONT_UI_LARGE font_ui_20
#define FONT_CLOCK font_clock_64  // digits and colon only
#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_BOOT_BUTTON 1
#define BOARD_TEMP_IN_PSRAM 0
// Album art is decoded small and stretched by LVGL, which keeps it in internal RAM and softens it.
constexpr int ART_BUFFER_W = 120;
constexpr int DEFAULT_ART_SIZE = 300;
// huge_app.csv has a single app slot: no updates over Wi-Fi.
#define BOARD_HAS_OTA 0
#define BOARD_HAS_EXTRA_CONTROLS 0

#endif
