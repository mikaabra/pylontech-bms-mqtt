// solar_helpers.h - Helper functions for rack-solar-bridge
// Provides threshold-based publishing with heartbeat for SmartShunt and EPEVER sensors

#pragma once

#include <cmath>
#include <string>
#include <cstdarg>
#include <algorithm>

// ============================================================================
// MILLIS() ROLLOVER-SAFE ELAPSED TIME CALCULATION
// millis() rolls over every ~49.7 days (2^32 ms at 1ms resolution)
// This function correctly calculates elapsed time across rollover boundaries
// ============================================================================
inline uint32_t safe_elapsed(uint32_t now, uint32_t last) {
    // Standard subtraction works correctly even across rollover due to unsigned arithmetic
    // Example: now=0x00000010, last=0xFFFFFFF0 -> result=0x00000020 (32 ticks, correct!)
    return now - last;
}

// ============================================================================
// TEXT SENSOR VALIDATION HELPERS
// Detect and reject corrupted/bitflipped text values
// ============================================================================

// Check if string contains only printable ASCII characters (32-126)
inline bool is_valid_printable(const std::string& s) {
    for (char c : s) {
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return true;
}

// Validate SmartShunt model description (should contain "SmartShunt" or "BMV")
inline bool validate_model_description(const std::string& s) {
    if (s.empty() || s.length() > 64) return false;
    if (!is_valid_printable(s)) return false;
    // Should contain known device identifiers
    return (s.find("SmartShunt") != std::string::npos || 
            s.find("BMV") != std::string::npos);
}

// Validate device type (should be alphanumeric, reasonable length)
inline bool validate_device_type(const std::string& s) {
    if (s.empty() || s.length() > 32) return false;
    if (!is_valid_printable(s)) return false;
    // Device type typically starts with letters/numbers
    return std::isalnum(s[0]);
}

// Validate firmware version (typical format: "v1.23" or "1.23")
inline bool validate_firmware_version(const std::string& s) {
    if (s.empty() || s.length() > 16) return false;
    if (!is_valid_printable(s)) return false;
    // Should contain at least one digit
    return std::any_of(s.begin(), s.end(), ::isdigit);
}

// Validate serial number (alphanumeric, no spaces, reasonable length)
inline bool validate_serial_number(const std::string& s) {
    if (s.empty() || s.length() < 4 || s.length() > 32) return false;
    for (char c : s) {
        if (!std::isalnum(c) && c != '-') {
            return false;
        }
    }
    return true;
}

// Validate monitor mode (known values: -1, 0, 1, 2 typically)
inline bool validate_dc_monitor_mode(const std::string& s) {
    if (s.empty() || s.length() > 32) return false;
    if (!is_valid_printable(s)) return false;
    // Should contain known mode descriptions or numeric values
    static const char* valid_modes[] = {"charger", "load", "dual", "-1", "0", "1", "2"};
    std::string lower;
    for (char c : s) lower += std::tolower(c);
    for (const char* mode : valid_modes) {
        if (lower.find(mode) != std::string::npos) return true;
    }
    // If it looks like a number, accept it
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isdigit(c) || c == '-';
    });
}

// Validate alarm condition (ON/OFF or similar binary states)
inline bool validate_alarm_condition(const std::string& s) {
    if (s.empty() || s.length() > 16) return false;
    if (!is_valid_printable(s)) return false;
    std::string lower;
    for (char c : s) lower += std::tolower(c);
    // Known good alarm states
    return (lower == "on" || lower == "off" || 
            lower.find("alarm") != std::string::npos ||
            lower.find("ok") != std::string::npos);
}

// Validate alarm reason (should be descriptive text)
inline bool validate_alarm_reason(const std::string& s) {
    if (s.empty() || s.length() > 64) return false;
    if (!is_valid_printable(s)) return false;
    return true;  // Alarm reasons can vary, just check printable
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

    // Use rollover-safe elapsed time calculation
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
