#if 1

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#if defined(BOARD_ESP32_4848S040)
// Internal RAM is needed by Wi-Fi and TLS, so the LVGL heap lives in PSRAM.
#define LV_MEM_SIZE (256U * 1024U)
#define LV_MEM_POOL_INCLUDE <esp32-hal-psram.h>
#define LV_MEM_POOL_ALLOC ps_malloc
#else
// Measured on the 4848S040 (same objects plus a few): ~22 KB at rest, ~33 KB with the Settings screen open.
#define LV_MEM_SIZE (40U * 1024U)
#endif

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_LOG 0
// newlib's memcpy/memset copy whole words; LVGL's own versions are slower for large image blits.
#define LV_MEMCPY_MEMSET_STD 1

#ifdef SIMULATOR
// The desktop simulator decodes album art with LVGL's copy of TJpgDec.
#define LV_USE_SJPG 1
#endif

// Montserrat only supplies the LV_SYMBOL glyphs; the UI fonts in src/fonts fall back to it.
#if defined(BOARD_ESP32_4848S040)
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_DEFAULT &lv_font_montserrat_22
#else
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#endif

#endif
#endif
