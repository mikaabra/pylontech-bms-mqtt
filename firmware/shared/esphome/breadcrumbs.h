#pragma once
#include <cstdint>
#include "esp_attr.h"
#include "esp_system.h"
#include "esphome/core/application.h"
#include "esphome/components/mqtt/mqtt_client.h"

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
// values; the snapshot stays pending until the report publish is accepted
// (it goes first on the connected tick, before budgeted discovery).
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

inline void bc_mark(uint8_t stage, uint8_t aux = 0) {
    if (bc_magic_ != BC_MAGIC) return;  // not set up (garbage after power-on)
    bc_stage_ = stage;
    bc_aux_ = aux;
    bc_stage_ms_ = millis();
    bc_mark_count_++;
    if (stage != BC_PUB_DATA && stage != BC_PUB_AVAIL && stage != BC_IDLE) bc_section_ = stage;
}

// Scope guard for section-style loop callbacks such as tick(): marks
// BC_IDLE and clears the section origin on every scope exit including
// early returns — unless the loop never comes back, which leaves the
// section's entry mark as the panic answer. bc_pub_end cannot do the
// clearing: it also runs mid-section for each publish.
struct BcScopeGuard {
    ~BcScopeGuard() {
        bc_mark(BC_IDLE);
        bc_section_ = 0;
    }
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

// Per-boot: build the snapshot report once. Shared by the serial pass at
// boot (ESP_LOG* below) and the broker report publish, which runs first on
// each connected tick. Returns false only if the format outgrew the buffer —
// callers must not publish a truncated report.
inline bool bc_build_report_(char *buf, size_t buf_size, bool routine) {
    const char *names[] = {"none", "idle", "tx_analog", "wait_analog",
                           "tx_alarm", "wait_alarm", "pub_data", "pub_stack",
                           "pub_disc", "check_stale", "can_frame", "can_diag",
                           "pub_avail", "sweep_batt"};
    const char *name = bc_snap_stage_ <= BC_SWEEP_BATT ? names[bc_snap_stage_] : "?";
    const char *max_name = bc_snap_max_pub_stage_ <= BC_SWEEP_BATT ? names[bc_snap_max_pub_stage_] : "?";
    const char *suffix = routine ? " (routine reset, not a crash)" : "";
    int n = snprintf(buf, buf_size,
                     "prior session ended in stage %u(%s) aux=%u marks=%lu last_pub_ms=%lu "
                     "max_pub_ms=%lu(max stage %u(%s) aux %u) stage_ms=%lu reset_reason=%d%s",
                     bc_snap_stage_, name, bc_snap_aux_, (unsigned long) bc_snap_mark_count_,
                     (unsigned long) bc_snap_last_pub_ms_, (unsigned long) bc_snap_max_pub_ms_,
                     bc_snap_max_pub_stage_, max_name, bc_snap_max_pub_aux_,
                     (unsigned long) bc_snap_stage_ms_, bc_snap_reset_reason_, suffix);
    return n >= 0 && (size_t) n < buf_size;
}

// Serial pass at boot: keep the snapshot pending so the broker report pass
// (first on the connected tick) can deliver it into the HA recorder;
// non-routine resets at ERROR, routine resets at INFO so they stay
// distinguishable.
inline void bc_log_snapshot_serial_() {
    char buf[288];
    bool fit = bc_build_report_(buf, sizeof(buf), bc_reason_is_routine_());
    if (!fit) {
        // serial pass still shows the (null-terminated) excerpt: a truncated
        // report is better than a missing one, and the flag names the bug
        ESP_LOGE("breadcrumbs", "report overgrew buffer (format regression), excerpt follows");
        ESP_LOGE("breadcrumbs", "%s", buf);
        return;
    }
    if (bc_reason_is_routine_()) {
        ESP_LOGI("breadcrumbs", "%s", buf);
    } else {
        ESP_LOGE("breadcrumbs", "%s", buf);
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
    bc_log_snapshot_serial_();
}

// Call from the 3s interval, FIRST on each connected tick (before budgeted
// discovery can fill the pool). Publishing to the log-tail topic through the
// normal MQTT client path (not the logger, which has no retry) is checked:
// under idf_send_async this enqueues rather than sends, and the snapshot
// stays pending until the publish is accepted into the queue, so a crash
// record cannot be lost to a full pool at the worst moment. Rides with the
// Log Tail entity into the recorder.
inline bool bc_publish_report() {
    if (!bc_have_snapshot_) return true;  // nothing pending
    if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return false;
    char buf[288];
    if (!bc_build_report_(buf, sizeof(buf), bc_reason_is_routine_())) {
        // never publish a truncated report; the serial pass already showed
        // the excerpt, so consume the record rather than retrying forever
        ESP_LOGE("breadcrumbs", "report overgrew buffer; discarding record (format regression)");
        bc_have_snapshot_ = false;
        return true;
    }
    if (!esphome::mqtt::global_mqtt_client->publish("deye_bms/logs", std::string(buf))) return false;
    bc_have_snapshot_ = false;
    return true;
}

}  // namespace esphome
