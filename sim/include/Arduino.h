#pragma once
// Desktop stand-in for the parts of the Arduino core the UI uses (String, millis, delay, helpers).
// LVGL's C files include it too (lv_conf.h uses millis() as the tick), so the C part comes first.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t millis(void);
void delay(uint32_t ms);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <type_traits>

using std::max;
using std::min;

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))


// The ESP32 core sets the TZ rule and starts SNTP; the desktop already has the time.
inline void configTzTime(const char *tz, const char *, const char * = nullptr, const char * = nullptr) {
    setenv("TZ", tz, 1);
    tzset();
}

// Arduino's String, backed by std::string, with the members this project uses.
class String {
public:
    String() = default;
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    explicit String(char c) : s_(1, c) {}
    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value, int>::type = 0>
    explicit String(T v) : s_(std::to_string(v)) {}

    const char *c_str() const { return s_.c_str(); }
    unsigned int length() const { return (unsigned int)s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    void reserve(unsigned int n) { s_.reserve(n); }
    bool concat(const char *s, unsigned int n) {
        s_.append(s, n);
        return true;
    }
    char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    int indexOf(char c, unsigned int from = 0) const {
        auto p = s_.find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(const String &t, unsigned int from = 0) const {
        auto p = s_.find(t.s_, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    String substring(unsigned int from, unsigned int to = 0xFFFFFFFF) const {
        if (from > s_.size()) return String();
        return String(s_.substr(from, std::min<size_t>(to, s_.size()) - from));
    }
    bool startsWith(const String &p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    long toInt() const { return atol(s_.c_str()); }

    String &operator+=(const String &o) {
        s_ += o.s_;
        return *this;
    }
    String &operator+=(const char *o) {
        s_ += o ? o : "";
        return *this;
    }
    String &operator+=(char c) {
        s_ += c;
        return *this;
    }
    friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
    friend String operator+(const String &a, const char *b) { return String(a.s_ + (b ? b : "")); }
    friend String operator+(const char *a, const String &b) { return String((a ? a : "") + b.s_); }
    friend String operator+(const String &a, char b) { return String(a.s_ + b); }
    friend bool operator==(const String &a, const String &b) { return a.s_ == b.s_; }
    friend bool operator!=(const String &a, const String &b) { return a.s_ != b.s_; }
    friend bool operator==(const String &a, const char *b) { return a.s_ == (b ? b : ""); }
    friend bool operator!=(const String &a, const char *b) { return !(a == b); }

private:
    std::string s_;
};

#endif // __cplusplus
