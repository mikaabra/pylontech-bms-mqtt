#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct LogRecord {
    char level[8];
    char tag[32];
    char msg[512];
};
inline std::vector<LogRecord> g_logs;
inline uint32_t g_ms = 0;

inline uint32_t millis() { return g_ms; }

// Parameter names end in _ so member access (rec_.tag) is never
// token-substituted by the preprocessor.
#define BC_CAPTURE_LOG_(lvl_, tag_, ...)                               \
    do {                                                                \
        LogRecord rec_;                                                 \
        snprintf(rec_.level, sizeof(rec_.level), "%s", lvl_);           \
        snprintf(rec_.tag, sizeof(rec_.tag), "%s", tag_);               \
        snprintf(rec_.msg, sizeof(rec_.msg), __VA_ARGS__);              \
        g_logs.push_back(rec_);                                         \
    } while (0)

#define ESP_LOGE(tag, ...) BC_CAPTURE_LOG_("ERROR", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) BC_CAPTURE_LOG_("WARN", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) BC_CAPTURE_LOG_("INFO", tag, __VA_ARGS__)

namespace esphome {
// ESPHome's App is a global OBJECT, not a type; publish paths call
// App.feed_wdt() on it unqualified
inline struct AppStub {
  void feed_wdt() {}
} App;
inline void yield() {}
}  // namespace esphome

// The firmware's handlers see these symbols unqualified via esphome.h's
// using-directive; mirror that for the host-side headers.
using namespace esphome;
