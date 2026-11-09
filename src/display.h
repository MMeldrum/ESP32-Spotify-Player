#pragma once

void display_init();

// Switches the backlight. While it is off, the first touch only switches it back on: LVGL never sees it.
void display_set_screen_on(bool on);
bool display_screen_on();
// Called (from the LVGL thread) when a touch has switched the screen back on.
void display_on_touch_wake(void (*callback)());
