#pragma once
#include "esphome.h"
#include "bms_link_sniffer_core.h"
#include "pylontech_protocol.h"

#include <cstring>

// Thin operator wrapper around linksniff::BmsLinkCore (part 1: listener).
// The core is pure and host-oracle tested; this header only moves bytes
// between the UART and the core and pushes event-driven text states into
// the bound native entities. Everything else is pulled by yaml template
// lambdas from core getters - there is deliberately no discovery code
// here: native ESPHome MQTT discovery covers this low-volume device.
//
// Phase 2 (issue #59): the guarded write client. The TX path is reachable
// only through WriteController's gates: the Link Write Enable switch
// (yaml restore_mode: ALWAYS_OFF) is re-checked at every state transition,
// the allowlist (param 0x4C, 3400-3500 mV, packs 01-04) lives in the pure
// core, each Send press is a one-shot (initial + single retry), and firing
// requires >=150 ms confirmed silence with the slot watchdog healthy.
// DE/RE keys only via the uart flow_control_pin during our own frame -
// nothing else in this tree writes to the UART.

class BmsLinkSniffer {
public:
    explicit BmsLinkSniffer(esphome::uart::UARTComponent* uart)
        : uart_(uart),
          write_(core_, &BmsLinkSniffer::enable_cb_, &BmsLinkSniffer::tx_cb_, this) {
        core_.set_ack_waiter(&write_);
    }

    void bind_entities(esphome::text_sensor::TextSensor* last_anomaly,
                       esphome::text_sensor::TextSensor* anomaly_dump,
                       esphome::text_sensor::TextSensor* write_status,
                       esphome::switch_::Switch* write_enable) {
        last_anomaly_ = last_anomaly;
        anomaly_dump_ = anomaly_dump;
        write_status_ = write_status;
        write_enable_ = write_enable;
    }

    // one-shot trigger from the Send Balance Voltage Write button. Pack 0
    // is the select's "None" (no restore, deliberate re-selection per
    // review round 2) - refuse rather than fall back to a default target.
    void arm_write(uint8_t pack, uint16_t value_mv) {
        if (!write_status_) return;
        if (!write_enable_ || !write_enable_->state) {
            write_status_->publish_state(
                "refused: Link Write Enable is off (turn it on deliberately)");
            return;
        }
        if (pack == 0) {
            write_status_->publish_state("refused: select a target pack first");
            return;
        }
        write_.request(pack, 0x4C, value_mv, esphome::millis());
        write_status_->publish_state(write_.status_line());
    }

    // 10 ms interval tick from yaml
    void tick() {
        uint32_t now = esphome::millis();
        uint8_t buf[512];
        size_t n = 0;
        uint8_t c;
        while (uart_->available() && n < sizeof buf && uart_->read_byte(&c)) {
            buf[n++] = c;
        }
        if (n) core_.feed(buf, n, now);
        core_.tick(now);
        write_.tick(now);
        for (const auto& e : core_.drain_events()) publish_event(e);
        if (write_status_ && write_.status_line() != last_write_line_) {
            last_write_line_ = write_.status_line();
            write_status_->publish_state(last_write_line_);
        }
        // review round 2: the enable gate auto-closes after every
        // operation completes (ack, no-ack, collision, abort, expiry) -
        // confirmation per press, matching the Mac tool's typed WRITE
        auto prev = write_status_prev_;
        write_status_prev_ = write_.status();
        if (write_enable_ && write_enable_->state && prev != write_.status() &&
            write_status_ && write_terminal(write_.status())) {
            write_enable_->turn_off();
            write_status_->publish_state(last_write_line_ + " [enable auto-off]");
        }
    }

    float writes_sent() const { return (float)write_.tx_count(); }
    float write_collisions() const { return (float)write_.collision_count(); }

    // Dump button: publish the ring and release a freeze. Auto-publish
    // (in publish_event, below) never resumes - the operator decides
    // when the incident window is no longer needed.
    void publish_dump() {
        if (anomaly_dump_) anomaly_dump_->publish_state(core_.dump_bounded());
        core_.resume_ring();
    }

    // ---- getters for template-sensor lambdas ----
    float rate_per_s() { return core_.take_rate_per_s(esphome::millis()); }
    float frames_total() const { return (float)core_.frames_total(); }
    float checksum_errors() const { return (float)core_.checksum_errors(); }
    float anomalies_total() const { return (float)core_.anomalies_total(); }
    float noise_bytes() const { return (float)core_.noise_bytes_total(); }
    float cadence_ms() const { return core_.cadence_ema_ms() ? (float)core_.cadence_ema_ms() : NAN; }
    float idle_gap_min_ms() const {
        uint32_t g = core_.idle_gap_min_ms(esphome::millis());
        return g ? (float)g : NAN;
    }
    float idle_gap_avg_ms() const {
        uint32_t g = core_.idle_gap_avg_ms(esphome::millis());
        return g ? (float)g : NAN;
    }
    bool watchdog() const { return core_.watchdog_tripped(); }
    bool pack_responding(int p) const { return core_.pack(p).responding; }
    int  pack_latency(int p) const { return core_.pack(p).last_latency_ms; }
    float pack_latency_ms(int p) const {
        int v = core_.pack(p).last_latency_ms;
        return v >= 0 ? (float)v : NAN;
    }
    float pack_age_s(int p) const {
        uint32_t last = core_.pack(p).last_response_ms;
        return last ? (float)(esphome::millis() - last) / 1000.0f : NAN;
    }

private:
    void publish_event(const linksniff::LinkEvent& e) {
        if (!last_anomaly_) return;
        bool anomaly_like = true;
        const char* kind = "frame";
        switch (e.kind) {
            case linksniff::EventKind::FrameOk:        anomaly_like = false; break;
            case linksniff::EventKind::AnomalyConfig:  kind = "config-traffic"; break;
            case linksniff::EventKind::AnomalyChecksum: kind = "checksum"; break;
            case linksniff::EventKind::AnomalyLenid:   kind = "lenid"; break;
            case linksniff::EventKind::AnomalyGarbled:  kind = "garbled"; break;
            case linksniff::EventKind::AnomalyNoise:    kind = "noise"; break;
            case linksniff::EventKind::AnomalyAck:      kind = "bare-ack"; break;
            case linksniff::EventKind::AnomalyUnpaired: kind = "unpaired-response"; break;
            case linksniff::EventKind::WatchdogTrip:    kind = "slot-watchdog-trip"; break;
            case linksniff::EventKind::WatchdogRecover: kind = "slot-watchdog-recover"; break;
            case linksniff::EventKind::PackMute:       kind = "pack-mute"; break;
            case linksniff::EventKind::PackRecover:    kind = "pack-recover"; break;
            case linksniff::EventKind::RingReleased:
                // incident window re-opened by recovery; the dump below
                // publishes what the window covered: the frozen head
                // (pre-incident context + trigger) plus the post-recovery
                // entries that fit (~11 s on the healthy cadence)
                kind = "ring-released";
                break;
            case linksniff::EventKind::AckForWrite:
                // healthy expected event: Write Status already reports the
                // ack; it must not appear on the Last Anomaly feed
                anomaly_like = false;
                break;
        }
        if (!anomaly_like) return;
        char line[160];
        if (e.pack)
            snprintf(line, sizeof line, "[%lu.%03lu] pack %d %s: %s",
                     (unsigned long)(e.t_ms / 1000), (unsigned long)(e.t_ms % 1000),
                     e.pack, kind, e.detail.c_str());
        else
            snprintf(line, sizeof line, "[%lu.%03lu] %s: %s",
                     (unsigned long)(e.t_ms / 1000), (unsigned long)(e.t_ms % 1000),
                     kind, e.detail.c_str());
        // full formatted line to the WARN stream (bms_link/logs -> Link Log
        // Tail), per review round 3: the pack number and kind, not a bare
        // detail fragment
        ESP_LOGW("link", "%s", line);
        last_anomaly_->publish_state(line);
        if (anomaly_dump_ && (e.kind == linksniff::EventKind::WatchdogTrip ||
                              e.kind == linksniff::EventKind::PackMute ||
                              e.kind == linksniff::EventKind::RingReleased)) {
            // trip/mute: the freshly frozen window; release: the window as
            // covered - the core re-opened the ring at recovery (only when
            // every trigger cleared), so post-recovery entries that fit are
            // included; a re-trip cancels the release and the newest
            // incident owns the capture
            anomaly_dump_->publish_state(core_.dump_bounded());
        }
    }

    esphome::uart::UARTComponent* uart_;
    linksniff::BmsLinkCore core_;
    linksniff::WriteController write_;
    esphome::text_sensor::TextSensor* last_anomaly_ = nullptr;
    esphome::text_sensor::TextSensor* anomaly_dump_ = nullptr;
    esphome::text_sensor::TextSensor* write_status_ = nullptr;
    esphome::switch_::Switch* write_enable_ = nullptr;
    std::string last_write_line_;
    linksniff::WriteController::Status write_status_prev_ = linksniff::WriteController::Status::Idle;

    static bool write_terminal(linksniff::WriteController::Status s) {
        using S = linksniff::WriteController::Status;
        return s == S::Acked || s == S::NoAck || s == S::Collision ||
               s == S::Aborted || s == S::Expired || s == S::Denied;
    }

    static bool enable_cb_(void* ctx) {
        BmsLinkSniffer* self = (BmsLinkSniffer*)ctx;
        return self->write_enable_ && self->write_enable_->state;
    }
    static bool tx_cb_(const std::string& frame, void* ctx) {
        // review round 2 nit: the flush result is checked - a timeout or
        // driver failure after the write is a real, reported abort, not
        // dead code. ASSUMED_SUCCESS (platform cannot confirm) also
        // returns true; FAILED/TIMEOUT return false and the controller
        // treats the attempt as sent-unknown (bytes may be on the wire).
        BmsLinkSniffer* self = (BmsLinkSniffer*)ctx;
        self->uart_->write_array(reinterpret_cast<const uint8_t*>(frame.data()),
                                 (size_t)frame.size());
        auto r = self->uart_->flush();
        return r == esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS ||
               r == esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
    }
};
