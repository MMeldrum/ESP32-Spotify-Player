#pragma once
// lv_conf.h puts the LVGL heap in PSRAM on the 4848S040; on the desktop that is plain malloc.
#include <stdlib.h>
#define ps_malloc malloc
