#pragma once
#include <cstdint>
#include "esp_attr.h"
#include "esp_system.h"
#include "esphome/core/application.h"

// Forensic breadcrumbs for the loop-blocking watchdog panics (see the
// 2026-11-01 incident): the task watchdog watches the ESPHome loop task and
// fires after 5 s of continuous block. RTC noinit RAM survives the panic
// reboot, so a sticky "stage entered" marker written by the loop names the
// section that never returned — publish sweep, discovery, UART flush, or a
// poll phase. Each section marks on entry and marks BC_IDLE on completion,
// so a panic either shows the blocking section's entry mark (blocked inside
// it) or BC_IDLE with a stale timestamp (blocked in unmarked code after the
// last section returned).
namespace esphome {

enum BcStage : uint8_t {
    BC_NONE = 0,
    BC_IDLE = 1,          // last marked section completed
    BC_TX_ANALOG = 2,     // uart write + flush, analog poll (aux = batt)
    BC_WAIT_ANALOG = 3,   // analog rx/parse section
    BC_TX_ALARM = 4,      // uart write + flush, alarm poll (aux = alarm batt)
    BC_WAIT_ALARM = 5,    // alarm rx/parse section (aux = alarm batt)
    BC_PUB_DATA = 6,      // hysteresis-gated value publish (publish_raw)
    BC_PUB_STACK = 7,     // stack-level value publishes
    BC_PUB_DISC = 8,      // discovery sweep (aux = entity-list index)
    BC_CHECK_STALE = 9,   // stale check + availability publishes
    BC_CAN_FRAME = 10,    // CAN frame handler (aux = low arb-id byte)
    BC_CAN_DIAG = 11,     // CAN diagnostics publish
    BC_PUB_AVAIL = 12,    // channel availability stamp publishes
    BC_SWEEP_BATT = 13,   // 30s sweep, per-battery section (aux = batt)
};

inline constexpr uint32_t BC_MAGIC = 0xBC0DEC5A;

// Live RTC record: sticky stage (the load-bearing crumb), total mark count
// (loop-progress watermark), and publish durations — the most recent one
// that returned plus the slowest since boot with its stage/aux, so a run of
// slow-but-completing publishes leaves a signature even when the last one
// before a wedge happened to be fast.
// The live record is reset at every boot, after the previous session's
// values are snapshotted for reporting, so each session arms a fresh record.
RTC_NOINIT_ATTR inline uint32_t bc_magic_ = 0;
RTC_NOINIT_ATTR inline uint8_t bc_stage_ = BC_NONE;
RTC_NOINIT_ATTR inline uint8_t bc_aux_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_stage_ms_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_mark_count_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_last_pub_ms_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_pub_start_ms_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_max_pub_ms_ = 0;
RTC_NOINIT_ATTR inline uint8_t bc_max_pub_stage_ = BC_NONE;
RTC_NOINIT_ATTR inline uint8_t bc_max_pub_aux_ = 0;

// RAM (per-boot, not RTC): the section the loop is running in. Any
// non-publish mark updates it, and publish marks carry it as their aux so a
// wedge inside a publish still shows which section called it — without
// touching every publish call site.
inline uint8_t bc_section_ = 0;

// RAM snapshot of the previous session, taken at boot before the live record
// is reset; the broker pass reports the snapshot, not the overwritten live
// values. bc_have_snapshot_ is cleared ONLY in bc_report_to_broker(), so
// the report still flows after MQTT connects.
inline bool bc_have_snapshot_ = false;
inline uint8_t bc_snap_stage_ = BC_NONE;
inline uint8_t bc_snap_aux_ = 0;
inline uint32_t bc_snap_mark_count_ = 0;
inline uint32_t bc_snap_stage_ms_ = 0;
inline uint32_t bc_snap_last_pub_ms_ = 0;
inline uint32_t bc_snap_max_pub_ms_ = 0;
inline uint8_t bc_snap_max_pub_stage_ = BC_NONE;
inline uint8_t bc_snap_max_pub_aux_ = 0;
inline int bc_snap_reset_reason_ = 0;

// Per-boot guard for the broker pass; reset by bc_setup() so each session
// gets exactly one delivery attempt.
inline bool bc_broker_attempted_ = false;

inline void bc_mark(uint8_t stage, uint8_t aux = 0) {
    if (bc_magic_ != BC_MAGIC) return;  // not set up (garbage after power-on)
    bc_stage_ = stage;
    bc_aux_ = aux;
    bc_stage_ms_ = millis();
    bc_mark_count_++;
    if (stage != BC_PUB_DATA && stage != BC_PUB_AVAIL && stage != BC_IDLE) bc_section_ = stage;
}

// Scope guard for section-style loop callbacks such as tick(): marks
// BC_IDLE on every scope exit including early returns — unless the loop
// never comes back, which leaves the section's entry mark as the panic
// answer. Use one per callback scope, after the entry mark.
struct BcScopeGuard {
    ~BcScopeGuard() { bc_mark(BC_IDLE); }
};

// Called by the publish chokepoints just before the blocking socket write;
// a publish that never returns leaves this mark as the crash-site answer.
inline void bc_pub_begin(uint8_t stage = BC_PUB_DATA) {
    bc_pub_start_ms_ = millis();
    bc_mark(stage, bc_section_);
}

inline void bc_pub_end() {
    uint32_t dur = millis() - bc_pub_start_ms_;
    bc_last_pub_ms_ = dur;
    if (dur > bc_max_pub_ms_) {
        bc_max_pub_ms_ = dur;
        bc_max_pub_stage_ = bc_stage_;
        bc_max_pub_aux_ = bc_aux_;
    }
    bc_mark(BC_IDLE);
}

// Routine means provably benign: OTA/software restart, power-on, or
// deep-sleep wake. Everything else — including unknown reasons and power
// glitches — reports at ERROR severity so nothing crash-shaped hides
// behind the routine label.
inline bool bc_reason_is_routine_() {
    return bc_snap_reset_reason_ == ESP_RST_SW || bc_snap_reset_reason_ == ESP_RST_POWERON ||
           bc_snap_reset_reason_ == ESP_RST_DEEPSLEEP;
}

// Emit the boot snapshot. Serial pass at boot keeps the snapshot pending so
// the broker pass can deliver it to the recorder once MQTT connects;
// non-routine resets log at ERROR, routine resets (OTA, manual) at
// INFO/WARN so they stay distinguishable in HA despite both landing there.
inline void bc_log_snapshot_(bool to_broker) {
    const char *names[] = {"none", "idle", "tx_analog", "wait_analog",
                           "tx_alarm", "wait_alarm", "pub_data", "pub_stack",
                           "pub_disc", "check_stale", "can_frame", "can_diag",
                           "pub_avail", "sweep_batt"};
    const char *name = bc_snap_stage_ <= BC_SWEEP_BATT ? names[bc_snap_stage_] : "?";
    const char *max_name = bc_snap_max_pub_stage_ <= BC_SWEEP_BATT ? names[bc_snap_max_pub_stage_] : "?";
    const bool routine = bc_reason_is_routine_();
    if (!routine) {
        ESP_LOGE("breadcrumbs",
                 "prior session ended in stage %u(%s) aux=%u marks=%lu last_pub_ms=%lu max_pub_ms=%lu(max stage %u(%s) aux %u) stage_ms=%lu reset_reason=%d",
                 bc_snap_stage_, name, bc_snap_aux_, (unsigned long) bc_snap_mark_count_,
                 (unsigned long) bc_snap_last_pub_ms_, (unsigned long) bc_snap_max_pub_ms_,
                 bc_snap_max_pub_stage_, max_name, bc_snap_max_pub_aux_,
                 (unsigned long) bc_snap_stage_ms_, bc_snap_reset_reason_);
    } else if (to_broker) {
        ESP_LOGW("breadcrumbs",
                 "prior session ended in stage %u(%s) aux=%u marks=%lu last_pub_ms=%lu max_pub_ms=%lu(max stage %u(%s) aux %u) stage_ms=%lu reset_reason=%d (routine reset, not a crash)",
                 bc_snap_stage_, name, bc_snap_aux_, (unsigned long) bc_snap_mark_count_,
                 (unsigned long) bc_snap_last_pub_ms_, (unsigned long) bc_snap_max_pub_ms_,
                 bc_snap_max_pub_stage_, max_name, bc_snap_max_pub_aux_,
                 (unsigned long) bc_snap_stage_ms_, bc_snap_reset_reason_);
    } else {
        ESP_LOGI("breadcrumbs",
                 "prior session ended in stage %u(%s) aux=%u marks=%lu last_pub_ms=%lu max_pub_ms=%lu(max stage %u(%s) aux %u) stage_ms=%lu reset_reason=%d (routine reset, not a crash)",
                 bc_snap_stage_, name, bc_snap_aux_, (unsigned long) bc_snap_mark_count_,
                 (unsigned long) bc_snap_last_pub_ms_, (unsigned long) bc_snap_max_pub_ms_,
                 bc_snap_max_pub_stage_, max_name, bc_snap_max_pub_aux_,
                 (unsigned long) bc_snap_stage_ms_, bc_snap_reset_reason_);
    }
}

// Call once from on_boot before the handlers run. A valid RTC record means
// the previous session ended in a reboot (panic, OTA, manual): snapshot it,
// log it on serial now, and reset the live record for the fresh session.
// A power-on boot (garbage noinit RAM) just initializes the record.
inline void bc_setup() {
    if (bc_magic_ != BC_MAGIC) {
        bc_magic_ = BC_MAGIC;
        bc_stage_ = BC_NONE;
        bc_aux_ = 0;
        bc_stage_ms_ = 0;
        bc_mark_count_ = 0;
        bc_last_pub_ms_ = 0;
        bc_pub_start_ms_ = 0;
        bc_max_pub_ms_ = 0;
        bc_max_pub_stage_ = BC_NONE;
        bc_max_pub_aux_ = 0;
        return;
    }
    bc_have_snapshot_ = true;
    bc_snap_stage_ = bc_stage_;
    bc_snap_aux_ = bc_aux_;
    bc_snap_mark_count_ = bc_mark_count_;
    bc_snap_stage_ms_ = bc_stage_ms_;
    bc_snap_last_pub_ms_ = bc_last_pub_ms_;
    bc_snap_max_pub_ms_ = bc_max_pub_ms_;
    bc_snap_max_pub_stage_ = bc_max_pub_stage_;
    bc_snap_max_pub_aux_ = bc_max_pub_aux_;
    bc_snap_reset_reason_ = (int) esp_reset_reason();
    bc_stage_ = BC_NONE;
    bc_aux_ = 0;
    bc_stage_ms_ = 0;
    bc_mark_count_ = 0;
    bc_last_pub_ms_ = 0;
    bc_pub_start_ms_ = 0;
    bc_max_pub_ms_ = 0;
    bc_max_pub_stage_ = BC_NONE;
    bc_max_pub_aux_ = 0;
    bc_section_ = 0;
    bc_log_snapshot_(false);
    bc_broker_attempted_ = false;
}

// Call from the 3s interval with the MQTT connection state. On first
// connect per boot, emit the snapshot through the logger so the mqtt
// log_topic carries it into the HA recorder — the only place the snapshot
// flag is consumed.
inline void bc_report_to_broker(bool mqtt_connected) {
    if (bc_broker_attempted_ || !mqtt_connected || !bc_have_snapshot_) return;
    bc_broker_attempted_ = true;
    bc_have_snapshot_ = false;
    bc_log_snapshot_(true);
}

}  // namespace esphome
