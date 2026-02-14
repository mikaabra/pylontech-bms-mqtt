#pragma once

#include <cmath>
#include <string>
#include <cstdarg>
#include <algorithm>

inline uint32_t safe_elapsed(uint32_t now, uint32_t last) {
    return now - last;
}

// Simple bitflip rate tracking using a fixed 10-minute window
// When window expires, counter resets
inline void record_bitflip_event(uint32_t& count, uint32_t& window_start, uint32_t now) {
    const uint32_t WINDOW_MS = 600000; // 10 minutes
    
    if (window_start == 0) {
        window_start = now;
        count = 1;
        return;
    }
    
    uint32_t elapsed = safe_elapsed(now, window_start);
    if (elapsed >= WINDOW_MS) {
        // Window expired, start new window
        count = 1;
        window_start = now;
    } else {
        count++;
    }
}

inline float get_bitflip_rate_per_minute(uint32_t count, uint32_t window_start, uint32_t now) {
    if (count == 0 || window_start == 0) {
        return 0.0f;
    }
    
    const uint32_t WINDOW_MS = 600000; // 10 minutes
    uint32_t elapsed = safe_elapsed(now, window_start);
    
    if (elapsed >= WINDOW_MS) {
        // Window expired, rate is 0
        return 0.0f;
    }
    
    // Rate = events / elapsed_minutes
    float elapsed_minutes = elapsed / 60000.0f;
    if (elapsed_minutes < 0.1f) {
        elapsed_minutes = 0.1f; // Minimum 6 seconds to avoid division by near-zero
    }
    return count / elapsed_minutes;
}

inline bool is_valid_printable(const std::string& s) {
    for (char c : s) {
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return true;
}

inline bool validate_model_description(const std::string& s) {
    if (s.empty() || s.length() > 64) return false;
    if (!is_valid_printable(s)) return false;
    return (s.find("SmartShunt") != std::string::npos ||
            s.find("BMV") != std::string::npos);
}

inline bool validate_device_type(const std::string& s) {
    if (s.empty() || s.length() > 32) return false;
    if (!is_valid_printable(s)) return false;
    return std::isalnum(s[0]);
}

inline bool validate_firmware_version(const std::string& s) {
    if (s.empty() || s.length() > 16) return false;
    if (!is_valid_printable(s)) return false;
    return std::any_of(s.begin(), s.end(), ::isdigit);
}

inline bool validate_serial_number(const std::string& s) {
    if (s.empty() || s.length() < 4 || s.length() > 32) return false;
    for (char c : s) {
        if (!std::isalnum(c) && c != '-') {
            return false;
        }
    }
    return true;
}

inline bool validate_dc_monitor_mode(const std::string& s) {
    if (s.empty() || s.length() > 64) return false;
    if (!is_valid_printable(s)) return false;
    static const char* valid_modes[] = {"charger", "load", "dual", "bmv", "smartshunt", "battery", "monitor", "-1", "0", "1", "2"};
    std::string lower;
    for (char c : s) lower += std::tolower(c);
    for (const char* mode : valid_modes) {
        if (lower.find(mode) != std::string::npos) return true;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isdigit(c) || c == '-';
    });
}

inline bool validate_alarm_condition(const std::string& s) {
    if (s.empty() || s.length() > 16) return false;
    if (!is_valid_printable(s)) return false;
    std::string lower;
    for (char c : s) lower += std::tolower(c);
    if (lower.length() <= 3) {
        return (lower[0] == 'o' || lower.find("alarm") != std::string::npos);
    }
    return (lower == "on" || lower == "off" ||
            lower.find("alarm") != std::string::npos ||
            lower.find("ok") != std::string::npos);
}

inline bool validate_alarm_reason(const std::string& s) {
    if (s.empty() || s.length() > 64) return false;
    if (!is_valid_printable(s)) return false;
    return true;
}

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

    if (safe_elapsed(now, last_publish) >= heartbeat_ms) {
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

    uint32_t time_delta_ms = safe_elapsed(now, last_publish);
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

inline bool check_threshold_int(int new_val, int &last_val, uint32_t &last_publish, int threshold = 1, int min_val = INT_MIN, int max_val = INT_MAX) {
    uint32_t now = millis();
    bool publish = false;

    if (new_val < min_val || new_val > max_val) return false;

    if (last_publish == 0 || last_val < min_val || last_val > max_val) {
        publish = true;
    } else if (std::abs(new_val - last_val) >= threshold) {
        publish = true;
    } else if (safe_elapsed(now, last_publish) >= 60000) {
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
