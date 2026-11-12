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
// core, each Send press is ONE deliberate frame (no retry - a late ack is
// listened for and reported, but never triggers a transmit), and firing
// requires >=150 ms confirmed silence with the slot watchdog healthy.
// DE/RE keys only via the uart flow_control_pin during our own frame -
// nothing else in this tree writes to the UART.

class BmsLinkSniffer {
public:
    explicit BmsLinkSniffer(esphome::uart::UARTComponent* uart)
        : uart_(uart),
          write_(core_, gate_, &BmsLinkSniffer::enable_cb_, &BmsLinkSniffer::tx_cb_, this),
          read_(core_, gate_, &BmsLinkSniffer::enable_cb_, &BmsLinkSniffer::tx_cb_, this),
          teleop_(core_, gate_, &BmsLinkSniffer::enable_cb_, &BmsLinkSniffer::tx_cb_, this) {
        core_.set_ack_waiter(&write_);
        core_.set_read_waiter(&read_);
        core_.set_telecom_waiter(&teleop_);
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

    void bind_read_entities(esphome::text_sensor::TextSensor* read_status,
                            esphome::sensor::Sensor* balance_mv_pack1,
                            esphome::sensor::Sensor* balance_mv_pack2,
                            esphome::sensor::Sensor* balance_mv_pack3,
                            esphome::sensor::Sensor* balance_mv_pack4) {
        read_status_ = read_status;
        balance_mv_[1] = balance_mv_pack1;
        balance_mv_[2] = balance_mv_pack2;
        balance_mv_[3] = balance_mv_pack3;
        balance_mv_[4] = balance_mv_pack4;
    }

    void bind_debug_entities(esphome::sensor::Sensor* debug_value) {
        debug_value_ = debug_value;
    }

    void bind_teleop_entities(esphome::text_sensor::TextSensor* teleop_status) {
        teleop_status_ = teleop_status;
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
        auto rs = read_.status();
        if (rs == linksniff::ReadController::Status::Armed ||
            rs == linksniff::ReadController::Status::WaitBlock) {
            write_status_->publish_state(
                "refused: a read is in flight - wait for its verdict before writing");
            return;
        }
        if (teleop_in_flight_()) {
            write_status_->publish_state(
                "refused: a teleControl sequence is in flight - wait for its verdict");
            return;
        }
        bool accepted = write_.request(pack, 0x4C, value_mv, esphome::millis());
        write_status_->publish_state(write_.status_line());
        // record only an ACCEPTED request (review fix): a refused or
        // ignored press carries no written value, and a read-back must
        // never grade against a value the bus never received
        if (accepted) {
            last_written_valid_[pack] = true;
            last_written_param_[pack] = 0x4C;
            last_written_mv_[pack] = value_mv;
        }
    }

    // generic debug write (the allowlist still gates the param; each new
    // writable param lands as its own allowlist entry + select option in
    // its own reviewed change - there is deliberately no force path)
    void arm_debug_write(uint8_t pack, uint8_t param, uint16_t raw_value) {
        if (!write_status_) return;
        if (!write_enable_ || !write_enable_->state) {
            write_status_->publish_state(
                "refused: Link Operate Enable is off (turn it on deliberately)");
            return;
        }
        if (pack == 0) {
            write_status_->publish_state("refused: select a target pack first");
            return;
        }
        if (param == 0) {
            write_status_->publish_state("refused: select a write parameter first");
            return;
        }
        auto rs = read_.status();
        if (rs == linksniff::ReadController::Status::Armed ||
            rs == linksniff::ReadController::Status::WaitBlock) {
            write_status_->publish_state(
                "refused: a read is in flight - wait for its verdict before writing");
            return;
        }
        if (teleop_in_flight_()) {
            write_status_->publish_state(
                "refused: a teleControl sequence is in flight - wait for its verdict");
            return;
        }
        bool accepted = write_.request(pack, param, raw_value, esphome::millis());
        write_status_->publish_state(write_.status_line());
        // only an ACCEPTED request records (the return value, not the
        // Armed status: an ignored in-flight press keeps the OLD op armed
        // while this press's value never reached the wire)
        if (accepted) {
            last_written_valid_[pack] = true;
            last_written_param_[pack] = param;
            last_written_mv_[pack] = raw_value;
        }
    }

    // one-shot trigger from the Read Balance Voltage button. The read is
    // the step-4 acceptance proof producer: after a write, ReadOk with a
    // matching value is the write-verify; without a recorded write the
    // same press is a baseline read of the stored 0x4C.
    void arm_read(uint8_t pack) {
        if (!read_status_) return;
        if (!read_gates_ok_(pack)) return;
        // the balance read only claims write-verify when the last accepted
        // write to this pack was the balance param itself
        bool expect_valid =
            last_written_valid_[pack] && last_written_param_[pack] == linksniff::kReadBalanceParam;
        read_.request(pack, linksniff::kReadBalanceParam, expect_valid,
                      last_written_mv_[pack], esphome::millis(), true);
        read_status_->publish_state(read_.status_line());
    }

    // generic parameter read (u16 band 0x07-0x7C): the debug select names
    // the param; the verdict line + Debug Parameter Value carry the raw
    // reading (name/scale/unit live in the select label, from the vendor
    // XML via gen_param_menus.py)
    void arm_debug_read(uint8_t pack, uint8_t param) {
        if (!read_status_) return;
        if (!read_gates_ok_(pack)) return;
        if (param == 0) {
            read_status_->publish_state("refused: select a read parameter first");
            return;
        }
        bool expect_valid =
            last_written_valid_[pack] && last_written_param_[pack] == param;
        read_.request(pack, param, expect_valid, last_written_mv_[pack],
                      esphome::millis(), true);
        read_status_->publish_state(read_.status_line());
    }

    // one-shot trigger from the Fire Variant Probe button (CID2-47
    // short-read evidence probes, variant path merged in the core,
    // PR #102). shape is the Variant Probe Shape select index 0-3 ->
    // kShort4C/kShort004C/kShort4C01/kShort4C4C (option order mirrors
    // the core's allowlisted enum order). One press = ONE frame: pack
    // and gate handling are identical to the block read (the shared
    // Target Pack select, Link Operate Enable, in-flight write/teleop
    // refusals), the core allowlist denies packs 01/02 fail-closed,
    // and expectations are forced off - the answer (or silence) is
    // graded EVIDENCE-ONLY and never becomes a value. The armed line
    // names the shape+pack; the verdict lands on Link Read Status.
    void arm_variant_probe(uint8_t pack, uint8_t shape) {
        if (!read_status_) return;
        if (!read_gates_ok_(pack)) return;
        if (shape > 3) {
            read_status_->publish_state("refused: select a probe shape first");
            return;
        }
        linksniff::ReadVariant variant;
        switch (shape) {
            case 0:  variant = linksniff::ReadVariant::kShort4C; break;
            case 1:  variant = linksniff::ReadVariant::kShort004C; break;
            case 2:  variant = linksniff::ReadVariant::kShort4C01; break;
            default: variant = linksniff::ReadVariant::kShort4C4C; break;
        }
        // param is ignored for variants (INFO is fixed by the allowlist)
        // but stays the block read's 0x4C for consistency; has_expect
        // false - evidence never grades against expectations; the same
        // corridor aim as the block read (aim_long_gap = true)
        read_.request(pack, linksniff::kReadBalanceParam, false, 0,
                      esphome::millis(), true, variant);
        read_status_->publish_state(read_.status_line());
    }

    // one-shot trigger from the SYS_OFF button. One press = probe, then the
    // D545 SYS_OFF write - the controller refuses to fire the write unless
    // the probed pack answered. Packs 01-04 only (00 is the master battery:
    // rebooting it takes the Link bus and inverter comms down).
    void arm_sys_off(uint8_t pack) {
        if (!teleop_status_) return;
        if (!write_enable_ || !write_enable_->state) {
            teleop_status_->publish_state(
                "refused: Link Operate Enable is off (turn it on deliberately)");
            return;
        }
        if (pack == 0) {
            teleop_status_->publish_state("refused: select a target pack first");
            return;
        }
        if (!tx_plane_clear_()) {
            teleop_status_->publish_state(
                "refused: a write or read is in flight - wait for its verdict");
            return;
        }
        teleop_.request_sys_off(pack, esphome::millis());
        teleop_status_->publish_state(teleop_.status_line());
    }

    // one-shot trigger from the LMCHG wake-pair button (the captured
    // recovery sequence: probe, LMCHG disable, LMCHG enable). New-gen
    // packs only - the controller refuses old-gen targets (no captured
    // old-gen disable; the physical button is that recovery path).
    void arm_wake_pair(uint8_t pack) {
        if (!teleop_status_) return;
        if (!write_enable_ || !write_enable_->state) {
            teleop_status_->publish_state(
                "refused: Link Operate Enable is off (turn it on deliberately)");
            return;
        }
        if (pack == 0) {
            teleop_status_->publish_state("refused: select a target pack first");
            return;
        }
        if (!tx_plane_clear_()) {
            teleop_status_->publish_state(
                "refused: a write or read is in flight - wait for its verdict");
            return;
        }
        teleop_.request_wake_pair(pack, esphome::millis());
        teleop_status_->publish_state(teleop_.status_line());
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
        read_.tick(now);
        teleop_.tick(now);
        for (const auto& e : core_.drain_events()) publish_event(e);
        if (write_status_ && write_.status_line() != last_write_line_) {
            last_write_line_ = write_.status_line();
            write_status_->publish_state(last_write_line_);
        }
        // trusted read verdicts publish the per-pack 0x4C value; every
        // untrusted grade leaves sensors untouched (fail-closed)
        if (read_status_ && read_.status_line() != last_read_line_) {
            last_read_line_ = read_.status_line();
            read_status_->publish_state(last_read_line_);
        }
        if (teleop_status_ && teleop_.status_line() != last_teleop_line_) {
            last_teleop_line_ = teleop_.status_line();
            teleop_status_->publish_state(last_teleop_line_);
        }
        auto rstat = read_.status();
        auto prev_pair = op_status_prev_;
        if (rstat != prev_pair.r) {
            if (rstat == linksniff::ReadController::Status::ReadOk ||
                rstat == linksniff::ReadController::Status::Mismatch) {
                if (read_.last_param() == linksniff::kReadBalanceParam) {
                    if (balance_mv_[read_.last_pack()])
                        balance_mv_[read_.last_pack()]->publish_state((float)read_.value_raw());
                } else if (debug_value_) {
                    debug_value_->publish_state((float)read_.value_raw());
                }
            }
        }
        // review round 2: the enable gate auto-closes after every operation
        // completes. Review-vote fix: only when the client that WAS in
        // flight is the one that just reached its terminal state - a
        // finished write stays Acked until the next write, and that stale
        // Acked used to kill the read armed right after the write verdict
        bool op_done = linksniff::op_just_finished(prev_pair.w, prev_pair.r, prev_pair.t,
                                                   write_.status(), rstat, teleop_.status());
        op_status_prev_ = {write_.status(), rstat, teleop_.status()};
        if (write_enable_ && write_enable_->state && op_done && write_status_) {
            write_enable_->turn_off();
            enable_on_ms_ = 0;
            write_status_->publish_state(last_write_line_ + " [enable auto-off]");
        }
        if (teleop_status_ && op_done && teleop_.status_line() != last_teleop_line_) {
            last_teleop_line_ = teleop_.status_line();
            teleop_status_->publish_state(last_teleop_line_ + " [enable auto-off]");
        }
        // review round 3: enable left on with no press and no operation
        // in flight must not stay armed forever - 5 minutes of idleness
        // closes the gate
        bool en = write_enable_ && write_enable_->state;
        if (en && !enable_prev_) enable_on_ms_ = now;
        enable_prev_ = en;
        bool write_in_flight = linksniff::write_status_in_flight(write_.status());
        bool read_in_flight = linksniff::read_status_in_flight(rstat);
        bool teleop_in_flight = teleop_in_flight_();
        if (en && enable_on_ms_ && now - enable_on_ms_ > kEnableTimeoutMs &&
            write_status_ && !write_in_flight && !read_in_flight && !teleop_in_flight) {
            write_enable_->turn_off();
            enable_on_ms_ = 0;
            write_status_->publish_state("enable timed out after 5 min of idleness - turn it on deliberately for the next operation");
        }
    }

    float writes_sent() const { return (float)write_.tx_count(); }
    float write_collisions() const { return (float)write_.collision_count(); }
    float reads_sent() const { return (float)read_.tx_count(); }
    float teleops_sent() const { return (float)teleop_.tx_count(); }

    // Dump button: publish the ring and release a freeze. The core also
    // releases automatically once every trigger has cleared (publish_event
    // below); so this stays what it always was - the operator override.
    void publish_dump() {
        if (anomaly_dump_) anomaly_dump_->publish_state(core_.dump_bounded());
        core_.resume_ring();
    }

    // ---- getters for template-sensor lambdas ----
    float rate_per_s() { return core_.take_rate_per_s(esphome::millis()); }
    std::string gap_report() const { return core_.gap_report(); }
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
            case linksniff::EventKind::BlockForRead:
                // healthy expected event: Link Read Status already carries
                // the graded verdict and the ring holds the compact record
                anomaly_like = false;
                break;
            case linksniff::EventKind::VariantAnswer:
                // healthy expected event (CID2-47 variant probes): Link
                // Read Status already carries the EVIDENCE-ONLY verdict
                // (VariantEvidence / NoAnswerMasterPoll) and the ring
                // holds the short record - the core marks it not-an-anomaly
                anomaly_like = false;
                break;
            case linksniff::EventKind::AckForTeleOp:
                // healthy expected event: Link TeleOp Status reports the
                // ack within its sequence verdict
                anomaly_like = false;
                break;
            case linksniff::EventKind::ProbeAnswer:
                // healthy expected event: Link TeleOp Status carries the
                // probe verdict; the ring holds the id record
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
    linksniff::SharedTxGate gate_;
    linksniff::WriteController write_;
    linksniff::ReadController read_;
    linksniff::TeleOpController teleop_;
    esphome::text_sensor::TextSensor* last_anomaly_ = nullptr;
    esphome::text_sensor::TextSensor* anomaly_dump_ = nullptr;
    esphome::text_sensor::TextSensor* write_status_ = nullptr;
    esphome::text_sensor::TextSensor* read_status_ = nullptr;
    esphome::text_sensor::TextSensor* teleop_status_ = nullptr;
    esphome::switch_::Switch* write_enable_ = nullptr;
    esphome::sensor::Sensor* balance_mv_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    esphome::sensor::Sensor* debug_value_ = nullptr;
    std::string last_write_line_;
    std::string last_read_line_;
    std::string last_teleop_line_;
    bool last_written_valid_[5] = {};
    uint8_t last_written_param_[5] = {};
    uint16_t last_written_mv_[5] = {};
    struct OpStatusPair {
        linksniff::WriteController::Status w;
        linksniff::ReadController::Status r;
        linksniff::TeleOpController::Status t;
    };
    OpStatusPair op_status_prev_{
        linksniff::WriteController::Status::Idle,
        linksniff::ReadController::Status::Idle,
        linksniff::TeleOpController::Status::Idle};
    bool enable_prev_ = false;
    uint32_t enable_on_ms_ = 0;
    static constexpr uint32_t kEnableTimeoutMs = 300000;

    bool tx_plane_clear_() const {
        auto ws = write_.status();
        if (ws == linksniff::WriteController::Status::Armed ||
            ws == linksniff::WriteController::Status::WaitAck) return false;
        auto rs = read_.status();
        if (rs == linksniff::ReadController::Status::Armed ||
            rs == linksniff::ReadController::Status::WaitBlock) return false;
        return true;
    }
    bool teleop_in_flight_() const {
        return linksniff::teleop_status_in_flight(teleop_.status());
    }

    // shared refusal gates for the three read-side arms (arm_read,
    // arm_debug_read, arm_variant_probe) - the same four checks were
    // copied between them, folded here per CLAUDE.md's third-copy rule
    // and the #107 review verdict. Each refusal publishes its
    // byte-identical line to Link Read Status and the arm returns;
    // true = every gate passed. Check order preserved from the folded
    // blocks; arm-local refusals (param/shape) sit after these gates.
    bool read_gates_ok_(uint8_t pack) {
        if (!write_enable_ || !write_enable_->state) {
            read_status_->publish_state(
                "refused: Link Operate Enable is off (turn it on deliberately)");
            return false;
        }
        if (pack == 0) {
            read_status_->publish_state("refused: select a target pack first");
            return false;
        }
        auto ws = write_.status();
        if (ws == linksniff::WriteController::Status::Armed ||
            ws == linksniff::WriteController::Status::WaitAck) {
            read_status_->publish_state("refused: a write is in flight - wait for its verdict");
            return false;
        }
        if (teleop_in_flight_()) {
            read_status_->publish_state(
                "refused: a teleControl sequence is in flight - wait for its verdict");
            return false;
        }
        return true;
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
