#pragma once

void display_init();

// Switches the backlight. While it is off, the first touch only switches it back on: LVGL never sees it.
void display_set_screen_on(bool on);
bool display_screen_on();
// Backlight brightness while the screen is on, DISPLAY_MIN_BRIGHTNESS-100 %. The minimum keeps the
// screen readable, so the brightness slider can always be found again.
constexpr int DISPLAY_MIN_BRIGHTNESS = 35;
void display_set_brightness(int percent);
// Called (from the LVGL thread) when a touch has switched the screen back on.
void display_on_touch_wake(void (*callback)());
