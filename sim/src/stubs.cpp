// The firmware modules the UI talks to, reduced to what a desktop demo needs.
#include "app_log.h"
#include "config.h"
#include "led.h"
#include "web_portal.h"
#include <cstdarg>
#include <chrono>
#include <thread>

// In demo recording mode time is virtual: it advances exactly one video frame per frame, so the recording
// is smooth however long rendering takes.
bool g_virtualClock = false;
uint32_t g_virtualMs = 0;

extern "C" uint32_t millis(void) {
    if (g_virtualClock) return g_virtualMs;
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

extern "C" void delay(uint32_t ms) {
    if (g_virtualClock) {
        g_virtualMs += ms;
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

AppConfig g_config;
volatile bool g_internetOk = true;

void app_log(LogLevel level, const char *fmt, ...) {
    static const char LEVELS[] = {'D', 'I', 'W', 'E'};
    va_list args;
    va_start(args, fmt);
    // stderr: stdout may carry the recorded video frames.
    fprintf(stderr, "[%c] ", LEVELS[level]);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// Settings live only for the session.
bool config_save() { return true; }
std::vector<Pin> config_get_pins() { return g_config.pins; }

void led_set_color(uint32_t) {}

bool web_portal_update_progress(int &percent) {
    percent = 0;
    return false;
}
