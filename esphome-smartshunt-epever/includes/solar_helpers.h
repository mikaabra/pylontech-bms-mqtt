// solar_helpers.h - Helper functions for rack-solar-bridge
// Provides threshold-based publishing with heartbeat for SmartShunt and EPEVER sensors

#pragma once

#include <cmath>
#include <string>
#include <cstdarg>

inline bool safe_snprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vsnprintf(buf, size, fmt, args);
  va_end(args);
  if (ret < 0 || (size_t)ret >= size) {
    ESP_LOGW("safe_snprintf", "Buffer truncation detected! Needed %d bytes, have %zu", ret, size);
    return false;
  }
  return true;
}

inline void publish_solar(int &publish_count) {
    publish_count++;
    if (publish_count % 20 == 0) {
        delay(10);
    }
}

inline bool check_threshold_float(float new_val, float& last_val,
                                   uint32_t& last_publish,
                                   float threshold,
                                   float min_val = -INFINITY,
                                   float max_val = INFINITY,
                                   uint32_t heartbeat_ms = 60000) {
    if (std::isnan(new_val) || !std::isfinite(new_val)) return false;
    if (new_val < min_val || new_val > max_val) return false;

    uint32_t now = millis();

    if (last_publish == 0 || last_val < min_val || last_val > max_val) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    if (fabs(new_val - last_val) >= threshold) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    if ((now - last_publish) >= heartbeat_ms) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    return false;
}

inline bool check_threshold_float_robust(float new_val, float& last_val,
                                          uint32_t& last_publish,
                                          float threshold,
                                          float max_rate_per_sec,
                                          float min_val,
                                          float max_val,
                                          uint32_t heartbeat_ms = 60000) {
    if (std::isnan(new_val) || !std::isfinite(new_val)) return false;
    if (new_val < min_val || new_val > max_val) return false;

    uint32_t now = millis();

    if (last_publish == 0 || last_val < min_val || last_val > max_val) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    uint32_t time_delta_ms = now - last_publish;
    if (time_delta_ms > 0) {
        float change = fabs(new_val - last_val);
        float rate = change / (time_delta_ms / 1000.0f);
        if (rate > max_rate_per_sec) {
            ESP_LOGW("validation", "Rate limit: %.2f/sec (max %.2f/sec)", rate, max_rate_per_sec);
            return false;
        }
    }

    if (fabs(new_val - last_val) >= threshold) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    if (time_delta_ms >= heartbeat_ms) {
        last_val = new_val;
        last_publish = now;
        return true;
    }

    return false;
}

inline bool check_threshold_int(int new_val, int &last_val, uint32_t &last_publish, int threshold = 1) {
    uint32_t now = millis();
    bool publish = false;

    if (last_publish == 0 || last_val == -1) {
        publish = true;
    } else if (std::abs(new_val - last_val) >= threshold) {
        publish = true;
    } else if ((now - last_publish) >= 60000) {
        publish = true;
    }

    if (publish) {
        last_val = new_val;
        last_publish = now;
    }

    return publish;
}

inline bool check_threshold_bool(bool new_val, bool &last_val, uint32_t &last_change_time, bool &pending_val, bool &has_pending) {
    uint32_t now = millis();
    const uint32_t DEBOUNCE_MS = 2000;

    if (new_val != last_val) {
        if (!has_pending) {
            pending_val = new_val;
            last_change_time = now;
            has_pending = true;
            return false;
        } else if (new_val == pending_val) {
            if ((now - last_change_time) >= DEBOUNCE_MS) {
                last_val = new_val;
                has_pending = false;
                return true;
            }
            return false;
        } else {
            pending_val = new_val;
            last_change_time = now;
            return false;
        }
    }

    if (has_pending && new_val != pending_val) {
        has_pending = false;
    }

    return false;
}
