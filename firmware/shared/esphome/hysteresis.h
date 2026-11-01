#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// #53 pacing note: every heartbeat-bearing instance gets a FIXED slot —
// a periodic occurrence time assigned once (assign_slot, k*H/N spread)
// that no publish, commit or outage can move. A value re-sends at the
// first pass at/after each of its occurrences, so regrouping mechanisms
// (a shared commit instant, the poll walk committing a cohort together)
// cannot rebuild CADENCE — steady passes stay flat (idle: ~18-19 dues).
// An outage still owes one occurrence per missed instance: the first
// pass back carries the accumulated backlog (60 s gap ~114 dues, 360 s
// ~239) and the producer-side pass stop paces it away over the following
// passes. What survives the outage unregrouped is the slot SPACING, not
// the absence of a catch-up burst. Changes and crossings publish
// immediately and never touch slot state.
//
// Slot mode coexists with the legacy path: without assign_slot, decide()
// and check() behave exactly as rack-solar/epever consumed them before
// #53 (change/first immediate, elapsed-since-commit heartbeat). The deye
// handler uses slots for boot wave AND heartbeat.
//
// In slot mode the FIRST publish of a numeric instance waits for its slot
// (boot spread); change-type firsts/changes stay immediate (alarm
// latency). commit-on-acceptance throughout: a refused publish leaves the
// slot occurrence unconsumed and retries next pass; a sustained pool
// squeeze defers the backlog across passes, bounded by the same pass stop.
// Slot arithmetic is modulo 2^32: millis() wraps at 49.7 days and the
// occurrence indices then reset — accepted, devices power-cycle far more
// often.

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
    float pending_val_ = NAN;
    uint32_t slot_origin_ = 0, slot_phase_ = 0, slot_period_ = 0;
    uint32_t slot_last_occurrence_ = 0;

    // would-publish decision without side effects; pairs with commit()
    bool decide(float new_val, float threshold, float min_val = -INFINITY, float max_val = INFINITY,
                uint32_t heartbeat_ms = 60000) {
        if (std::isnan(new_val) || !std::isfinite(new_val)) return false;
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (slot_period_) {
            // fixed-slot heartbeat: once per period at the window position,
            // independent of any publish/commit/outage timing
            if (slot_due_(now)) {
                pending_val_ = new_val;
                return true;
            }
            if (first) return false;   // boot spread: first publish rides its slot
            if (fabs(new_val - last_val) >= threshold) {
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
        slot_mark_fired_(millis());
    }

    // #53 fixed slot: window position assigned ONCE; refires the value at
    // that position every period_ms, immune to commit timing and outages
    void assign_slot(uint32_t now_ms, uint32_t phase_ms, uint32_t period_ms) {
        slot_origin_ = now_ms;
        slot_phase_ = phase_ms;
        slot_period_ = period_ms;
        slot_last_occurrence_ = 0xFFFFFFFF;   // no occurrence consumed yet
    }

    bool slot_due_(uint32_t now) const {
        // occurrence-indexed: the k-th slot occurrence happens at
        // origin + phase + k*period; fire on the FIRST SAMPLE at or
        // after an unconsumed occurrence. Position-within-window
        // comparison (pos >= phase) is WRONG under sampling: a 10 s
        // pass grid leaves the last sampling gap of every window
        // unreachable, killing every instance slotted there.
        if (now - slot_origin_ < slot_phase_) return false;   // before first occurrence
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        return (uint32_t) (occ - slot_last_occurrence_) >= 1u;
    }

    void slot_mark_fired_(uint32_t now) {
        if (!slot_period_) return;
        // consume the LATEST occurrence when the commit landed at or
        // after it — a change before the occurrence leaves it to fire at
        // its time (idempotent re-send); nothing ever moves
        if (now - slot_origin_ < slot_phase_) return;
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        if ((uint32_t) (occ - slot_last_occurrence_) >= 1u) slot_last_occurrence_ = occ;
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
    }
};

struct HysteresisInt {
    int last_val = -2147483647;
    uint32_t last_publish = 0;
    bool first = true;
    int pending_val_ = -2147483647;
    uint32_t slot_origin_ = 0, slot_phase_ = 0, slot_period_ = 0;
    uint32_t slot_last_occurrence_ = 0;

    bool decide(int new_val, int threshold = 1, int min_val = -2147483647, int max_val = 2147483647,
               uint32_t heartbeat_ms = 60000) {
        if (new_val < min_val || new_val > max_val) return false;
        uint32_t now = millis();
        if (slot_period_) {
            if (slot_due_(now)) {
                pending_val_ = new_val;
                return true;
            }
            if (first) return false;   // boot spread: first publish rides its slot
            if (std::abs(new_val - last_val) >= threshold) {
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
        slot_mark_fired_(millis());
    }

    void assign_slot(uint32_t now_ms, uint32_t phase_ms, uint32_t period_ms) {
        slot_origin_ = now_ms;
        slot_phase_ = phase_ms;
        slot_period_ = period_ms;
        slot_last_occurrence_ = 0xFFFFFFFF;   // no occurrence consumed yet
    }

    bool slot_due_(uint32_t now) const {
        // occurrence-indexed: the k-th slot occurrence happens at
        // origin + phase + k*period; fire on the FIRST SAMPLE at or
        // after an unconsumed occurrence. Position-within-window
        // comparison (pos >= phase) is WRONG under sampling: a 10 s
        // pass grid leaves the last sampling gap of every window
        // unreachable, killing every instance slotted there.
        if (now - slot_origin_ < slot_phase_) return false;   // before first occurrence
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        return (uint32_t) (occ - slot_last_occurrence_) >= 1u;
    }

    void slot_mark_fired_(uint32_t now) {
        if (!slot_period_) return;
        if (now - slot_origin_ < slot_phase_) return;
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        if ((uint32_t) (occ - slot_last_occurrence_) >= 1u) slot_last_occurrence_ = occ;
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
    }
};

// Change-plus-heartbeat types. Two modes:
//  - legacy (no assign_slot): heartbeat_ms == 0 preserves the change-only
//    semantics rack-solar consumed before #53; a nonzero heartbeat_ms (the
//    deye round-2 call sites) allows the elapsed-since-commit republish.
//  - slot mode (assign_slot): every instance re-sends its value once per
//    period at its FIXED window position — the mechanism that repairs a
//    value the mqtt drain task DISCARDED after acceptance (backend pops and
//    releases regardless of is_connected_) AND keeps the change-type cohort
//    spread forever (a shared commit instant cannot regroup them). Changes
//    and first publishes stay immediate (alarm latency matters); they never
//    touch slot state. Slot mode ignores the heartbeat_ms parameter.

struct HysteresisBool {
    bool last_val = false;
    bool first = true;
    uint32_t last_publish = 0;
    bool pending_val_ = false;
    uint32_t slot_origin_ = 0, slot_phase_ = 0, slot_period_ = 0;
    uint32_t slot_last_occurrence_ = 0;

    bool decide(bool new_val, uint32_t heartbeat_ms = 0) {
        if (slot_period_) {
            if (slot_due_(millis())) {
                pending_val_ = new_val;
                return true;
            }
        }
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
        slot_mark_fired_(millis());
    }

    void assign_slot(uint32_t now_ms, uint32_t phase_ms, uint32_t period_ms) {
        slot_origin_ = now_ms;
        slot_phase_ = phase_ms;
        slot_period_ = period_ms;
        slot_last_occurrence_ = 0xFFFFFFFF;   // no occurrence consumed yet
    }

    bool slot_due_(uint32_t now) const {
        // occurrence-indexed: the k-th slot occurrence happens at
        // origin + phase + k*period; fire on the FIRST SAMPLE at or
        // after an unconsumed occurrence. Position-within-window
        // comparison (pos >= phase) is WRONG under sampling: a 10 s
        // pass grid leaves the last sampling gap of every window
        // unreachable, killing every instance slotted there.
        if (now - slot_origin_ < slot_phase_) return false;   // before first occurrence
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        return (uint32_t) (occ - slot_last_occurrence_) >= 1u;
    }

    void slot_mark_fired_(uint32_t now) {
        if (!slot_period_) return;
        if (now - slot_origin_ < slot_phase_) return;
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        if ((uint32_t) (occ - slot_last_occurrence_) >= 1u) slot_last_occurrence_ = occ;
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
    uint32_t slot_origin_ = 0, slot_phase_ = 0, slot_period_ = 0;
    uint32_t slot_last_occurrence_ = 0;

    bool decide(const std::string& new_val, uint32_t heartbeat_ms = 0) {
        if (slot_period_) {
            if (slot_due_(millis())) {
                pending_val_ = new_val;
                return true;
            }
        }
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
        slot_mark_fired_(millis());
    }

    void assign_slot(uint32_t now_ms, uint32_t phase_ms, uint32_t period_ms) {
        slot_origin_ = now_ms;
        slot_phase_ = phase_ms;
        slot_period_ = period_ms;
        slot_last_occurrence_ = 0xFFFFFFFF;   // no occurrence consumed yet
    }

    bool slot_due_(uint32_t now) const {
        // occurrence-indexed: the k-th slot occurrence happens at
        // origin + phase + k*period; fire on the FIRST SAMPLE at or
        // after an unconsumed occurrence. Position-within-window
        // comparison (pos >= phase) is WRONG under sampling: a 10 s
        // pass grid leaves the last sampling gap of every window
        // unreachable, killing every instance slotted there.
        if (now - slot_origin_ < slot_phase_) return false;   // before first occurrence
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        return (uint32_t) (occ - slot_last_occurrence_) >= 1u;
    }

    void slot_mark_fired_(uint32_t now) {
        if (!slot_period_) return;
        if (now - slot_origin_ < slot_phase_) return;
        uint32_t occ = (now - (slot_origin_ + slot_phase_)) / slot_period_;
        if ((uint32_t) (occ - slot_last_occurrence_) >= 1u) slot_last_occurrence_ = occ;
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
