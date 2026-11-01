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
// poll phase. For block-in-call resolution each mark is written *before* the
// potentially blocking call; the next section's entry mark overwrites it on
// the usual path.
namespace esphome {

enum BcStage : uint8_t {
    BC_NONE = 0,
    BC_IDLE = 1,          // loop cycling, nothing marked in progress
    BC_TX_ANALOG = 2,     // uart write + flush, analog poll (aux = batt)
    BC_WAIT_ANALOG = 3,   // analog rx/parse section
    BC_TX_ALARM = 4,      // uart write + flush, alarm poll (aux = batt)
    BC_WAIT_ALARM = 5,    // alarm rx/parse section
    BC_PUB_DATA = 6,      // hysteresis-gated value publish (publish_raw), any section
    BC_PUB_STACK = 7,     // push stack-level values
    BC_PUB_DISC = 8,      // discovery sweep (aux = entity-list index)
    BC_CHECK_STALE = 9,   // stale check + availability publishes
    BC_CAN_FRAME = 10,    // CAN frame handler publish path
    BC_CAN_DIAG = 11,     // CAN diagnostics publish
};

inline constexpr uint32_t BC_MAGIC = 0xBC0DEC5A;

// Live RTC record: sticky stage (the load-bearing crumb) plus the total
// mark count (rough loop-progress watermark). The live record is reset at
// every boot, after the previous session's values are snapshotted for
// reporting, so each session arms a fresh record.
RTC_NOINIT_ATTR inline uint32_t bc_magic_ = 0;
RTC_NOINIT_ATTR inline uint8_t bc_stage_ = BC_NONE;
RTC_NOINIT_ATTR inline uint8_t bc_aux_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_stage_ms_ = 0;
RTC_NOINIT_ATTR inline uint32_t bc_mark_count_ = 0;

// RAM snapshot of the previous session, taken at boot before the live
// record is reset; the broker pass reports the snapshot, not the
// overwritten live values.
inline bool bc_have_snapshot_ = false;
inline uint8_t bc_snap_stage_ = BC_NONE;
inline uint8_t bc_snap_aux_ = 0;
inline uint32_t bc_snap_mark_count_ = 0;
inline uint32_t bc_snap_stage_ms_ = 0;
inline int bc_snap_reset_reason_ = 0;

inline void bc_mark(uint8_t stage, uint8_t aux = 0) {
    if (bc_magic_ != BC_MAGIC) return;  // not set up (or garbage after power-on)
    bc_stage_ = stage;
    bc_aux_ = aux;
    bc_stage_ms_ = millis();
    bc_mark_count_++;
}

inline void bc_log_snapshot_() {
    const char *names[] = {"none", "idle", "tx_analog", "wait_analog",
                           "tx_alarm", "wait_alarm", "pub_data", "pub_stack",
                           "pub_disc", "check_stale", "can_frame", "can_diag"};
    const char *name = bc_snap_stage_ <= BC_CAN_DIAG ? names[bc_snap_stage_] : "?";
    ESP_LOGE("breadcrumbs",
             "prior session ended in stage %u(%s) aux=%u marks=%lu stage_ms=%lu reset_reason=%d",
             bc_snap_stage_, name, bc_snap_aux_, (unsigned long) bc_snap_mark_count_,
             (unsigned long) bc_snap_stage_ms_, bc_snap_reset_reason_);
    bc_have_snapshot_ = false;
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
        return;
    }
    bc_have_snapshot_ = true;
    bc_snap_stage_ = bc_stage_;
    bc_snap_aux_ = bc_aux_;
    bc_snap_mark_count_ = bc_mark_count_;
    bc_snap_stage_ms_ = bc_stage_ms_;
    bc_snap_reset_reason_ = (int) esp_reset_reason();
    bc_stage_ = BC_NONE;
    bc_aux_ = 0;
    bc_stage_ms_ = 0;
    bc_mark_count_ = 0;
    bc_log_snapshot_();
}

// Call from the 3s interval with the MQTT connection state. On first
// connect per boot, re-emit the snapshot through the logger so the mqtt
// log_topic carries it into the HA recorder (no log client required).
inline void bc_report_to_broker(bool mqtt_connected) {
    static bool attempted = false;
    if (attempted || !mqtt_connected || !bc_have_snapshot_) return;
    attempted = true;
    bc_log_snapshot_();
}

}  // namespace esphome
