#pragma once
#include <Arduino.h>
#include <time.h>

// Internet time (SNTP) in the time zone chosen on the Settings screen.

struct TimeZoneInfo {
    const char *name;
    const char *posix;  // POSIX TZ rule, including daylight saving
};

extern const TimeZoneInfo TIME_ZONES[];
extern const int TIME_ZONE_COUNT;

// Starts syncing; call once Wi-Fi is up. Safe to call again.
void time_begin();
// Switches to TIME_ZONES[index].
void time_set_zone(int index);
// Local time; false until the first sync has completed.
bool time_now(struct tm &out);
