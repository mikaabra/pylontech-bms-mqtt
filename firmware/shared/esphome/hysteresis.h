#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// #53 pacing note: value heartbeats are phase-staggered by arming each
// instance's last_publish at (now + phase - heartbeat). Unsigned wrap makes
// the arithmetic safe at boot (millis() < phase). The staggering therefore
// lives ENTIRELY in initialization: the due predicate is unchanged, so
// phases survive resets and reconnect re-arms by design.
//
// armed instances (fresh from arm()) suppress the first/threshold branches:
// their initial publish waits for its phase slot, which replaces the old
// all-at-once first-publish burst with a paced wave. commit() clears the
// state; refused publishes leave the instance armed/due and it retries on
// the next pass — refusals cost at most one extra heartbeat interval of
// staleness (commit-on-acceptance).

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
    bool armed = false;
    float pending_val_ = NAN;

    // would-publish decision without side effects; pairs with commit()
    bool decide(float new_val, float threshold, float min_val = -INFINITY, float max_val = INFINITY,
                uint32_t heartbeat_ms = 60000) {
        if (std::isnan(new_val) || !std::isfinite(new_val)) return false;
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (armed) {
            // phased first publish: only the slotted heartbeat path fires
            if (safe_elapsed(now, last_publish) >= heartbeat_ms) {
                pending_val_ = new_val;
                return true;
            }
            return false;
        }
        if (first || last_val < min_val || last_val > max_val) {
            pending_val_ = new_val;
            return true;
        }
        if (fabs(new_val - last_val) >= threshold) {
            pending_val_ = new_val;
            return true;
        }
        if (safe_elapsed(now, last_publish) >= heartbeat_ms) {
            pending_val_ = new_val;
            return true;
        }
        return false;
    }

    // apply the pending decision (call only after the publish was accepted)
    void commit() {
        last_val = pending_val_;
        last_publish = millis();
        first = false;
        armed = false;
    }

    // phase-staggered (re)arm: first publish fires ~phase_ms from now_ms,
    // spread across one heartbeat window. Usable at boot and on reconnect —
    // armed state communicates only through decide/commit.
    void arm(uint32_t now_ms, uint32_t phase_ms, uint32_t heartbeat_ms) {
        armed = true;
        last_publish = now_ms + phase_ms - heartbeat_ms;  // uint wrap-safe
        pending_val_ = NAN;
    }

    // legacy decide+assume-success; kept for consumers not yet on
    // commit-on-acceptance (rack-solar handlers adopt decide/commit in
    // their own rounds — see issue #53)
    bool check(float new_val, float threshold, float min_val = -INFINITY, float max_val = INFINITY,
               uint32_t heartbeat_ms = 60000) {
        if (!decide(new_val, threshold, min_val, max_val, heartbeat_ms)) return false;
        commit();
        return true;
    }

    void reset() {
        last_val = NAN;
        pending_val_ = NAN;
        last_publish = 0;
        first = true;
        armed = false;
    }
};

struct HysteresisInt {
    int last_val = -2147483647;
    uint32_t last_publish = 0;
    bool first = true;
    bool armed = false;
    int pending_val_ = -2147483647;

    bool decide(int new_val, int threshold = 1, int min_val = -2147483647, int max_val = 2147483647,
               uint32_t heartbeat_ms = 60000) {
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (armed) {
            // phased first publish; the armed gate itself suppresses the
            // first/threshold branches until the first slot commit (relevant
            // whether last_val is still the sentinel or a valid re-arm value)
            if (safe_elapsed(now, last_publish) >= heartbeat_ms) {
                pending_val_ = new_val;
                return true;
            }
            return false;
        }
        if (first || last_val < min_val || last_val > max_val) {
            pending_val_ = new_val;
            return true;
        }
        if (std::abs(new_val - last_val) >= threshold) {
            pending_val_ = new_val;
            return true;
        }
        if (safe_elapsed(now, last_publish) >= heartbeat_ms) {
            pending_val_ = new_val;
            return true;
        }
        return false;
    }

    void commit() {
        last_val = pending_val_;
        last_publish = millis();
        first = false;
        armed = false;
    }

    void arm(uint32_t now_ms, uint32_t phase_ms, uint32_t heartbeat_ms) {
        armed = true;
        last_publish = now_ms + phase_ms - heartbeat_ms;  // uint wrap-safe
        pending_val_ = -2147483647;
    }

    bool check(int new_val, int threshold = 1, int min_val = -2147483647, int max_val = 2147483647,
               uint32_t heartbeat_ms = 60000) {
        if (!decide(new_val, threshold, min_val, max_val, heartbeat_ms)) return false;
        commit();
        return true;
    }

    void reset() {
        last_val = -2147483647;
        pending_val_ = -2147483647;
        last_publish = 0;
        first = true;
        armed = false;
    }
};

// Change-plus-heartbeat types. heartbeat_ms == 0 preserves the legacy
// change-only semantics (rack-solar consumers pass no heartbeat); the deye
// alarm/state/cell-list sites pass a slow (300 s) heartbeat so a value that
// was committed after an ACCEPTED publish can still be DISCARDED by the
// mqtt drain task on disconnect (backend pops and releases regardless of
// is_connected_) — the heartbeat re-sends it within one window instead of
// waiting for the next value change. No arming: changes still publish
// immediately (alarm latency matters); the first publish rides the cap-paced
// boot wave.

struct HysteresisBool {
    bool last_val = false;
    bool first = true;
    uint32_t last_publish = 0;
    bool pending_val_ = false;

    bool decide(bool new_val, uint32_t heartbeat_ms = 0) {
        if (first || new_val != last_val) {
            pending_val_ = new_val;
            return true;
        }
        if (heartbeat_ms == 0) return false;
        if (safe_elapsed(millis(), last_publish) >= heartbeat_ms) {
            pending_val_ = new_val;
            return true;
        }
        return false;
    }

    void commit() {
        last_val = pending_val_;
        last_publish = millis();
        first = false;
    }

    bool check(bool new_val) {
        if (!decide(new_val)) return false;
        commit();
        return true;
    }

    void reset() {
        first = true;
        pending_val_ = false;
        last_publish = 0;
    }
};

struct HysteresisString {
    std::string last_val;
    bool first = true;
    uint32_t last_publish = 0;
    std::string pending_val_;

    bool decide(const std::string& new_val, uint32_t heartbeat_ms = 0) {
        if (first || new_val != last_val) {
            pending_val_ = new_val;
            return true;
        }
        if (heartbeat_ms == 0) return false;
        if (safe_elapsed(millis(), last_publish) >= heartbeat_ms) {
            pending_val_ = new_val;
            return true;
        }
        return false;
    }

    void commit() {
        last_val = pending_val_;
        last_publish = millis();
        first = false;
    }

    bool check(const std::string& new_val) {
        if (!decide(new_val)) return false;
        commit();
        return true;
    }

    void reset() {
        last_val.clear();
        pending_val_.clear();
        first = true;
        last_publish = 0;
    }
};
