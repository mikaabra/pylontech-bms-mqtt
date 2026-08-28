#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

inline uint32_t safe_elapsed(uint32_t now, uint32_t last) {
    return now - last;
}

inline bool check_threshold_float(float new_val, float& last_val, uint32_t& last_publish,
                                   float threshold,
                                   float min_val = -INFINITY, float max_val = INFINITY,
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

inline bool check_threshold_int(int new_val, int& last_val, uint32_t& last_publish,
                                  int threshold = 1,
                                  int min_val = -2147483647, int max_val = 2147483647,
                                  uint32_t heartbeat_ms = 60000) {
    if (new_val < min_val || new_val > max_val) return false;
    uint32_t now = millis();
    if (last_publish == 0 || last_val < min_val || last_val > max_val) {
        last_val = new_val;
        last_publish = now;
        return true;
    }
    if (std::abs(new_val - last_val) >= threshold) {
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

inline bool check_threshold_bool(bool new_val, bool& last_val, bool& first_publish) {
    if (first_publish || new_val != last_val) {
        last_val = new_val;
        first_publish = false;
        return true;
    }
    return false;
}

inline bool check_threshold_string(const char* new_val, const char*& last_val, bool& first_publish) {
    if (first_publish || strcmp(new_val, last_val) != 0) {
        last_val = new_val;
        first_publish = false;
        return true;
    }
    return false;
}

inline bool check_threshold_string(const std::string& new_val, std::string& last_val, bool& first_publish) {
    if (first_publish || new_val != last_val) {
        last_val = new_val;
        first_publish = false;
        return true;
    }
    return false;
}

struct HysteresisFloat {
    float last_val = NAN;
    uint32_t last_publish = 0;
    bool first = true;

    bool check(float new_val, float threshold, float min_val = -INFINITY, float max_val = INFINITY, uint32_t heartbeat_ms = 60000) {
        if (std::isnan(new_val) || !std::isfinite(new_val)) return false;
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (first || last_val < min_val || last_val > max_val) {
            last_val = new_val;
            last_publish = now;
            first = false;
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

    void reset() {
        last_val = NAN;
        last_publish = 0;
        first = true;
    }
};

struct HysteresisInt {
    int last_val = -2147483647;
    uint32_t last_publish = 0;
    bool first = true;

    bool check(int new_val, int threshold = 1, int min_val = -2147483647, int max_val = 2147483647, uint32_t heartbeat_ms = 60000) {
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (first || last_val < min_val || last_val > max_val) {
            last_val = new_val;
            last_publish = now;
            first = false;
            return true;
        }
        if (std::abs(new_val - last_val) >= threshold) {
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

    void reset() {
        last_val = -2147483647;
        last_publish = 0;
        first = true;
    }
};

struct HysteresisBool {
    bool last_val = false;
    bool first = true;

    bool check(bool new_val) {
        if (first || new_val != last_val) {
            last_val = new_val;
            first = false;
            return true;
        }
        return false;
    }

    void reset() {
        first = true;
    }
};

struct HysteresisString {
    std::string last_val;
    bool first = true;

    bool check(const std::string& new_val) {
        if (first || new_val != last_val) {
            last_val = new_val;
            first = false;
            return true;
        }
        return false;
    }

    void reset() {
        last_val.clear();
        first = true;
    }
};
