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
// Phase 2 (issue #59, guarded write client) adds the TX path; this header
// contains no write call, and the v1 yaml declares no tx_pin, so DE/RE
// (GPIO21, held low by an internal ALWAYS_OFF switch pin) can never key.

class BmsLinkSniffer {
public:
    explicit BmsLinkSniffer(esphome::uart::UARTComponent* uart)
        : uart_(uart) {}

    void bind_entities(esphome::text_sensor::TextSensor* last_anomaly,
              esphome::text_sensor::TextSensor* anomaly_dump) {
        last_anomaly_ = last_anomaly;
        anomaly_dump_ = anomaly_dump;
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
        for (const auto& e : core_.drain_events()) publish_event(e);
    }

    void publish_dump() {
        if (anomaly_dump_) anomaly_dump_->publish_state(core_.dump_bounded());
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
        last_anomaly_->publish_state(line);
    }

    esphome::uart::UARTComponent* uart_;
    linksniff::BmsLinkCore core_;
    esphome::text_sensor::TextSensor* last_anomaly_ = nullptr;
    esphome::text_sensor::TextSensor* anomaly_dump_ = nullptr;
};
