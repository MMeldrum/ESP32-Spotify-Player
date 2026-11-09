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
// No RGB LED, and GPIO 0 (BOOT) is a data line of the RGB panel.
#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_BOOT_BUTTON 0
// Downloads are buffered in PSRAM, not LittleFS: a flash write stalls PSRAM, which the RGB panel scans
// its framebuffer from, and the picture slips sideways.
#define BOARD_TEMP_IN_PSRAM 1

#else

#define BOARD_NAME "ESP32-2432S028R"
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int UI_SCALE_NUM = 1;
constexpr int UI_SCALE_DEN = 1;
#define FONT_UI_SMALL font_ui_14
#define FONT_UI_LARGE font_ui_20
#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_BOOT_BUTTON 1
#define BOARD_TEMP_IN_PSRAM 0

#endif
