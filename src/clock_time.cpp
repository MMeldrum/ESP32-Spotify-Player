#include "clock_time.h"
#include "config.h"

const TimeZoneInfo TIME_ZONES[] = {
    {"UTC", "UTC0"},
    {"London, Dublin", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow, Istanbul", "<+03>-3"},
    {"Dubai", "<+04>-4"},
    {"India", "IST-5:30"},
    {"Singapore, Perth", "<+08>-8"},
    {"China", "CST-8"},
    {"Japan, Korea", "JST-9"},
    {"Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Honolulu", "HST10"},
    {"Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"Arizona", "MST7"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic Canada", "AST4ADT,M3.2.0,M11.1.0"},
    {"Sao Paulo", "<-03>3"},
    {"Johannesburg", "SAST-2"},
};
const int TIME_ZONE_COUNT = sizeof(TIME_ZONES) / sizeof(TIME_ZONES[0]);

namespace {
bool started = false;

const char *zoneRule(int index) {
    return TIME_ZONES[constrain(index, 0, TIME_ZONE_COUNT - 1)].posix;
}
} // namespace

void time_begin() {
    if (started) return;
    started = true;
    configTzTime(zoneRule(g_config.timeZone), "pool.ntp.org", "time.google.com");
}

void time_set_zone(int index) {
    setenv("TZ", zoneRule(index), 1);
    tzset();
}

bool time_now(struct tm &out) {
    time_t now = time(nullptr);
    // Before the first sync the clock starts at 1970.
    if (now < 1700000000) return false;
    localtime_r(&now, &out);
    return true;
}
