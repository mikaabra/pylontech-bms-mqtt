#pragma once
// ---------------------------------------------------------------------------
// BMS073 Link-bus sniffer core — pure logic, zero ESPHome types.
//
// Feeds a timestamped byte stream (batches share one arrival timestamp, the
// same model the 10 ms wrapper tick produces on-device), classifies
// YD/T 1363.3 ASCII-hex frames, keeps bus stats, and raises anomalies.
// Host-oracle driven: tests/test_bms_link_sniffer_core.cpp (listener) and
// tests/test_bms_link_sniffer_inject.cpp (guarded write client) — change
// together. Framing/checksum reuse from includes/pylontech_protocol.h; no
// framing is re-implemented here.
//
// Bus model (docs/PROTOCOL_REFERENCE.md "Link-bus capture", 2026-10-31):
//   - master polls slaves with ~8050D5{42|44}E002<pack 01..04>, slaves answer
//     ~8000D5<RTN><LENID><INFO><CHK> (responses carry RTN in CID2, "00" = OK)
//   - one poll slot every 624 ms (occasional 1264 ms double-slot),
//     ~436 ms idle per slot, response turnaround < 16 ms
//   - steady-state traffic contains ONLY CID2 42/44 requests — anything else
//     on this bus is config-plane activity or damage, by definition
//
// Classification rules (keep the oracle and this comment in sync):
//   CID2 "42"/"44"            -> telemetry poll request; pack from INFO[0:2]
//   CID2 "00"                 -> response (RTN 00); paired to the open slot
//                                 (LENID length 0 = the bare ack — special)
//   anything else             -> config-plane traffic -> anomaly
//   ADR/VER are recorded but not classified on: third-party client requests
//   (ADR 00, the proven in-stack probe shape) use the same CID2 space.
// Pass attribution is by the REQUEST's pack index, never by response
// content — the half-duplex bus answers exactly one responder per slot,
// and response INFO is not decoded in v1.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pylontech_protocol.h"  // parse_hex, rs485_calc_chksum, rs485_verify_checksum

namespace linksniff {

// ---- constants ------------------------------------------------------------
static constexpr int      kNumPacks          = 4;     // Link slaves 01..04
static constexpr uint32_t kWatchdogTripMs   = 2000;  // over 624 & 1264 ms slots
static constexpr int      kPackMuteSlots    = 3;     // unanswered consecutive polls
static constexpr uint32_t kByteGapGarbleMs  = 100;   // silence inside a frame = garbled
static constexpr size_t   kMaxFrameChars    = 600;   // A222 block responses ~590 + slack
static constexpr size_t   kRingSlots        = 32;
// incident-window release delay after a recovery event (review round 3:
// a freeze must not outlive the incident - one month-1 mute must not eat
// the month-3 lockup's capture)
static constexpr uint32_t kRingReleaseDelayMs = 5000;
static constexpr uint32_t kStatsWindowMs    = 60000;

// The captured bare ack (RTN 00, empty INFO), interior without SOI/EOI.
static const char kRawBareAck[] = "8000D5000000FD9F";

// ---- frame-level helpers ---------------------------------------------------

// LENID validation: 4 hex chars = LCHK nibble + 12-bit length (the number of
// INFO ASCII characters). Same formula the vendor session and
// pylontech_protocol.h's rs485_make_cmd use; asserted on real frames in the
// oracle ("8008" len-8, "E002", "B07E", "9052", "A222", "0000").
inline bool lenid4_valid(const std::string& lenid) {
    if (lenid.size() != 4) return false;
    unsigned long v;
    if (!parse_hex(lenid, v)) return false;
    int lchk = (int)((v >> 12) & 0xF);
    int len  = (int)(v & 0xFFF);
    int d0 = (len >> 8) & 0xF, d1 = (len >> 4) & 0xF, d2 = len & 0xF;
    int calc = (~(d0 + d1 + d2) + 1) & 0xF;
    return lchk == calc;
}
// zero-length LENID 0000: special-cased by the caller (bare ack / knock).
inline int lenid4_len(const std::string& lenid) {
    unsigned long v;
    if (!parse_hex(lenid, v)) return -1;
    return (int)(v & 0xFFF);
}

// ---- events -----------------------------------------------------------------

enum class EventKind {
    FrameOk,          // well-formed frame classified (see role below) — no entity legs
    AnomalyConfig,    // request with CID2 outside {42,44} — config-plane traffic
    AnomalyChecksum,  // envelope-consistent but checksum failed
    AnomalyLenid,     // LCHK/length mismatch — malformed
    AnomalyGarbled,   // frame aborted: byte-silence, embedded '~', overlong
    AnomalyNoise,     // bytes outside any frame
    AnomalyAck,       // bare ack outside an expected injection wait
    AnomalyUnpaired,  // response with no open request slot (after first request seen)
    WatchdogTrip,     // no request start within kWatchdogTripMs after cadence established
    WatchdogRecover,
    PackMute,         // pack hit kPackMuteSlots unanswered polls
    PackRecover,
    RingReleased,     // incident window re-opened after recovery + delay (wrapper publishes)
};

struct LinkEvent {
    EventKind   kind;
    uint32_t    t_ms    = 0;
    int         pack    = 0;    // 1..4 where applicable, else 0
    std::string detail;        // short human-readable line (goes to Last Anomaly)
};

// Per-frame record as handed to the ring recorder (hex, bounded).
struct RingEntry {
    uint32_t    t_ms = 0;
    std::string hex;   // frame interior, truncated with "..." + "(+N)" suffix if over budget
};

// ---- per-pack stats -----------------------------------------------------------

struct PackStats {
    uint32_t polls          = 0;
    uint32_t responses      = 0;
    int      unanswered     = 0;   // consecutive polls without a VALID response
    int      last_latency_ms = -1; // request-frame-end -> response-frame-start
    uint32_t last_response_ms = 0;
    bool     responding     = false;
    bool     muting         = false;  // PackMute notified, awaiting first answer
};

// gap ring for idle-window stats: {request start, gap since previous frame end}
struct GapSample { uint32_t t_ms; uint32_t gap_ms; };

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------
class BmsLinkCore {
public:
    void feed(const uint8_t* data, size_t n, uint32_t t_ms) {
        last_rx_t_ms_ = t_ms;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = data[i];
            if (!in_frame_) {
                if (b == '~') {
                    in_frame_ = true;
                    frame_.clear();
                    frame_start_t_ms_ = t_ms;
                    frame_end_t_ms_ = t_ms;
                    noise_open_ = false;
                } else {
                    noise_bytes_++;
                    if (!noise_open_) {
                        noise_open_ = true;
                        push_event(EventKind::AnomalyNoise, t_ms, 0, "bus noise outside frame");
                        anomalies_++;
                        ring_now(t_ms, "(noise)");
                    }
                }
                continue;
            }
            if (b == '~') {
                // frames never embed SOI mid-body: the old one is garbled
                garble(t_ms, 0);
                in_frame_ = true;
                frame_.clear();
                frame_start_t_ms_ = t_ms;
                frame_end_t_ms_ = t_ms;
                noise_open_ = false;
                continue;
            }
            if (b == '\r') {
                frame_end_t_ms_ = t_ms;
                process_frame(t_ms);
                in_frame_ = false;
                noise_open_ = false;
                continue;
            }
            if (frame_.size() >= kMaxFrameChars) {
                garble(t_ms, frame_.size());
                in_frame_ = false;  // stay out until next '~'; bytes until then = noise
                continue;
            }
            frame_ += (char)b;
            frame_end_t_ms_ = t_ms;
        }
    }

    void tick(uint32_t t_ms) {
        // frame-level silence abort
        if (in_frame_ && t_ms - last_rx_t_ms_ >= kByteGapGarbleMs) {
            garble(t_ms, frame_.size());
            in_frame_ = false;
        }
        // incident window release: recovery re-opened the ring; after the
        // delay the wrapper publishes what the window covered: the frozen
        // head (pre-incident context + trigger note) plus the post-recovery
        // entries that fit (~11 s of traffic on the healthy 624 ms cadence
        // - 18 of 32 slots measured on a live dump). A new trip during the
        // delay cancels this via freeze_ring().
        if (release_pending_ && (int32_t)(t_ms - release_at_ms_) >= 0) {
            release_pending_ = false;
            push_event(EventKind::RingReleased, t_ms, 0, "incident window released");
        }
        // slot watchdog
        if (!watchdog_tripped_ && has_requests_ &&
            t_ms - last_request_start_ms_ > kWatchdogTripMs) {
            watchdog_tripped_ = true;
            push_event(EventKind::WatchdogTrip, t_ms, 0, "no request for >2 s");
            anomalies_++;
            ring_now(t_ms, "(watchdog)");
            freeze_ring();  // after the marker: the pre-incident window + freeze note is the frozen head
        }
    }

    // -- accessors (template-sensor lambdas poll these) ----------------------
    uint32_t frames_total() const { return frames_total_; }
    uint32_t checksum_errors() const { return checksum_errors_; }
    uint32_t anomalies_total() const { return anomalies_; }
    uint32_t noise_bytes_total() const { return noise_bytes_; }
    int      cadence_ema_ms() const { return cadence_ema_ms_; }
    bool     watchdog_tripped() const { return watchdog_tripped_; }
    bool     saw_traffic() const { return frames_total_ > 0 || noise_bytes_ > 0; }
    uint32_t since_last_rx(uint32_t now) const { return saw_traffic() ? now - last_rx_t_ms_ : 0; }

    const PackStats& pack(int p) const { return packs_[p]; }  // p in 0..4

    // idle-gap stats over the last kStatsWindowMs of request starts
    uint32_t idle_gap_min_ms(uint32_t now) const {
        uint32_t best = 0;
        for (size_t i = 0; i < gap_ring_.size(); i++) {
            if (now - gap_ring_[i].t_ms > kStatsWindowMs) continue;
            if (best == 0 || gap_ring_[i].gap_ms < best) best = gap_ring_[i].gap_ms;
        }
        return best;
    }
    uint32_t idle_gap_avg_ms(uint32_t now) const {
        uint64_t sum = 0; uint32_t cnt = 0;
        for (size_t i = 0; i < gap_ring_.size(); i++) {
            if (now - gap_ring_[i].t_ms > kStatsWindowMs) continue;
            sum += gap_ring_[i].gap_ms; cnt++;
        }
        return cnt ? (uint32_t)(sum / cnt) : 0;
    }

    // frames/s over the window since the previous call (rate sensor)
    float take_rate_per_s(uint32_t now) {
        if (rate_anchor_ms_ == 0) { rate_anchor_ms_ = now; rate_anchor_total_ = frames_total_; return 0.0f; }
        uint32_t dt = now - rate_anchor_ms_;
        if (dt == 0) return 0.0f;
        float r = (float)(frames_total_ - rate_anchor_total_) * 1000.0f / (float)dt;
        rate_anchor_ms_ = now;
        rate_anchor_total_ = frames_total_;
        return r;
    }

    // Incident capture: the ring freezes on the two events we exist to
    // catch (slot-watchdog trip = master-side outage, pack mute = the
    // every-couple-months lockup). While frozen, new entries do NOT
    // overwrite the pre-incident window; they are counted instead. The
    // Dump button (wrapper) publishes the frozen window and releases.
    void reopen_ring_after_incident_(uint32_t t_ms) {
        // round 4 (reviewer): a single recovery must not release the
        // window while another fault is still active - pack 1 answering
        // with pack 2 still mute, or the watchdog clearing with a pack
        // still muting, would overwrite the pre-incident window before
        // the incident is over. Re-open ONLY when every trigger cleared.
        if (any_incident_active()) return;
        ring_frozen_ = false;
        release_pending_ = true;
        release_at_ms_ = t_ms + kRingReleaseDelayMs;
    }
    // true while any incident trigger still holds: watchdog tripped or
    // any pack muting
    bool any_incident_active() const {
        if (watchdog_tripped_) return true;
        for (int p = 1; p <= 4; p++)
            if (packs_[p].muting) return true;
        return false;
    }
    void freeze_ring() { ring_frozen_ = true; release_pending_ = false; }
    // operator override for the ring-dump button
    void resume_ring() { ring_frozen_ = false; release_pending_ = false; }
    bool ring_frozen() const { return ring_frozen_; }
    uint32_t ring_dropped() const { return ring_dropped_; }

    // last anomaly line (Last Anomaly entity) — built on drain by the wrapper
    std::vector<LinkEvent> drain_events() {
        std::vector<LinkEvent> out;
        out.swap(events_);
        return out;
    }

    // bounded ring dump, newest first, <= max_bytes with explicit elision;
    // the elision marker's own bytes are reserved up front so the bound
    // holds even when the last (dropped) line is the one that overflowed
    std::string dump_bounded(size_t max_bytes = 255) const {
        std::string out;
        size_t keep = max_bytes > 3 ? max_bytes - 3 : 0;
        for (int i = 0; i < (int)ring_count_ && i < (int)kRingSlots; i++) {
            size_t idx = (ring_head_ + kRingSlots - 1 - (size_t)i) % kRingSlots;
            std::string piece = ring_[idx].hex;
            char stamp[24];
            snprintf(stamp, sizeof stamp, "[%lu.%03lu]", (unsigned long)(ring_[idx].t_ms / 1000),
                     (unsigned long)(ring_[idx].t_ms % 1000));
            std::string line = std::string(stamp) + piece;
            if (!out.empty()) line = "|" + line;
            if (out.size() + line.size() > keep) {
                if (out.empty()) {
                    // newest entry alone blows the budget: truncate it explicitly
                    return line.substr(0, max_bytes - 4) + "…+N";
                }
                return out + "…";
            }
            out += line;
        }
        return out;
    }

private:
    // ---- frame processing ---------------------------------------------------
    void process_frame(uint32_t t_ms) {
        frames_total_++;
        const std::string raw = frame_;  // interior: VER..CHK

        // envelope: VER(2) ADR(2) CID1(2) CID2(2) LENID(4) INFO(len) CHK(4)
        if (raw.size() < 16) return malformed(t_ms, raw, "short");
        std::string lenid = raw.substr(8, 4);
        int len = lenid4_len(lenid);
        if (len < 0) return malformed(t_ms, raw, "lenid hex");
        size_t expect = (size_t)(12 + len + 4);
        if (raw.size() != expect) return malformed_lenid(t_ms, raw, "len mismatch");
        if (!lenid4_valid(lenid)) return malformed_lenid(t_ms, raw, "lchk");

        std::string full = "~" + raw + "\r";
        if (!rs485_verify_checksum(full)) {
            push_event(EventKind::AnomalyChecksum, t_ms, 0,
                       "checksum fail cid2=" + raw.substr(6, 2));
            anomalies_++;
            checksum_errors_++;
            ring_now(t_ms, raw.substr(0, kRingHexBudget));
            return;
        }

        std::string cid2 = raw.substr(6, 2);
        std::string info = raw.substr(12, (size_t)len);

        if (cid2 == "42" || cid2 == "44") {
            on_request(t_ms, cid2, info);
            return;
        }
        if (cid2 == "00") {
            if (len == 0) return on_bare_ack(t_ms);
            on_response(t_ms);
            return;
        }
        // config-plane traffic: the single most important anomaly class
        std::string d = "cid2=" + cid2 + " len=" + std::to_string(len);
        push_event(EventKind::AnomalyConfig, t_ms, 0, d);
        anomalies_++;
        ring_now(t_ms, raw.substr(0, kRingHexBudget));
    }

    void on_request(uint32_t t_ms, const std::string& cid2, const std::string& info) {
        unsigned long pk;
        int pack = 0;
        if (info.size() >= 2 && parse_hex(info.substr(0, 2), pk) && pk <= 4) pack = (int)pk;

        // cadence bookkeeping
        if (has_requests_ && t_ms > last_request_start_ms_) {
            uint32_t dt = t_ms - last_request_start_ms_;
            cadence_ema_ms_ = cadence_ema_ms_ ? (cadence_ema_ms_ * 7 + (int)dt) / 8 : (int)dt;
            if (last_frame_end_ms_ && t_ms > last_frame_end_ms_) {
                GapSample s{t_ms, t_ms - last_frame_end_ms_};
                record_gap(s);
            }
        }
        last_request_start_ms_ = t_ms;
        has_requests_ = true;
        if (watchdog_tripped_) {
            watchdog_tripped_ = false;
            reopen_ring_after_incident_(t_ms);
            push_event(EventKind::WatchdogRecover, t_ms, pack, "cadence resumed");
        }

        // pairing: a new request closes the previous slot as unanswered
        if (open_req_.valid) close_open_slot_unanswered();
        open_req_ = OpenReq{pack, cid2, t_ms, frame_end_t_ms_, true};

        if (pack >= 1 && pack <= 4) {
            PackStats& s = packs_[pack];
            s.polls++;
            s.unanswered++;
        }
    }

    void on_response(uint32_t t_ms) {
        if (!open_req_.valid) {
            if (has_requests_) {
                push_event(EventKind::AnomalyUnpaired, t_ms, 0, "response with no open request");
                anomalies_++;
                ring_now(t_ms, "(unpaired resp)");
            }
            last_frame_end_ms_ = frame_end_t_ms_;  // still counts as bus activity
            return;
        }
        int pack = open_req_.pack;
        std::string pair_cid = open_req_.cid2;
        int latency = (int)(frame_start_t_ms_ - open_req_.req_end_ms);
        open_req_.valid = false;
        last_frame_end_ms_ = frame_end_t_ms_;
        ring_now(frame_end_t_ms_, pair_cid + " p" + std::to_string(pack));  // compact normal-traffic context entry
        if (pack >= 1 && pack <= 4) {
            PackStats& s = packs_[pack];
            s.responses++;
            s.unanswered = 0;
            s.last_latency_ms = latency;
            s.last_response_ms = t_ms;
            if (s.muting) {
                s.muting = false;
                reopen_ring_after_incident_(t_ms);
                push_event(EventKind::PackRecover, t_ms, pack, "pack answered");
            }
            s.responding = true;
        }
    }

    void on_bare_ack(uint32_t t_ms) {
        last_frame_end_ms_ = frame_end_t_ms_;
        if (bare_ack_hook_) {
            bare_ack_hook_(t_ms, bare_ack_ctx_);  // injection wait decides; v1: never set
            return;
        }
        push_event(EventKind::AnomalyAck, t_ms, 0, "bare ack with no write pending");
        anomalies_++;
        ring_now(t_ms, kRawBareAck);
    }

    // ---- helpers -------------------------------------------------------------
    void close_open_slot_unanswered() {
        ring_now(last_request_start_ms_,
                 open_req_.cid2 + " p" + std::to_string(open_req_.pack) + " -");  // unanswered poll close
        if (open_req_.pack >= 1 && open_req_.pack <= 4) {
            PackStats& s = packs_[open_req_.pack];
            if (s.unanswered >= kPackMuteSlots && !s.muting) {
                s.muting = true;
                s.responding = false;
                freeze_ring();
                push_event(EventKind::PackMute, last_request_start_ms_, open_req_.pack,
                           "mute after " + std::to_string(s.unanswered) + " polls");
                anomalies_++;
            }
        }
        open_req_.valid = false;
    }

    void garble(uint32_t t_ms, size_t len) {
        std::string d = "garbled partial " + std::to_string(len) + "ch";
        push_event(EventKind::AnomalyGarbled, t_ms, 0, d);
        anomalies_++;
        ring_now(t_ms, std::string("~") + frame_.substr(0, kRingHexBudget));
    }

    void malformed(uint32_t t_ms, const std::string& raw, const char* why) {
        push_event(EventKind::AnomalyGarbled, t_ms, 0, std::string("malformed: ") + why);
        anomalies_++;
        ring_now(t_ms, raw.substr(0, kRingHexBudget));
    }

    void malformed_lenid(uint32_t t_ms, const std::string& raw, const char* why) {
        push_event(EventKind::AnomalyLenid, t_ms, 0, std::string("lenid: ") + why);
        anomalies_++;
        ring_now(t_ms, raw.substr(0, kRingHexBudget));
    }

    void record_gap(const GapSample& s) {
        if (gap_ring_.size() < 32) {
            gap_ring_.push_back(s);              // growth phase: head trails size
        } else {
            gap_ring_[gap_head_] = s;            // steady state: ring overwrite
        }
        gap_head_ = (gap_head_ + 1) % 32;
    }

    void ring_now(uint32_t t_ms, const std::string& hex) {
        if (ring_frozen_) {
            ring_dropped_++;
            return;
        }
        RingEntry e;
        e.t_ms = t_ms;
        if (hex.size() > kRingHexBudget) {
            e.hex = hex.substr(0, kRingHexBudget) + "…(+" + std::to_string(hex.size() - kRingHexBudget) + ")";
        } else {
            e.hex = hex;
        }
        ring_[ring_head_] = e;
        ring_head_ = (ring_head_ + 1) % kRingSlots;
        if (ring_count_ < kRingSlots) ring_count_++;
    }

    void push_event(EventKind k, uint32_t t, int pack, const std::string& detail) {
        if (events_.size() >= 64) { dropped_events_++; return; }  // wrapper drains every 10 ms
        events_.push_back(LinkEvent{k, t, pack, detail});
    }

    // ---- hook (set only by the guarded write client, part 2) --------------
public:
    void set_bare_ack_hook(void (*hook)(uint32_t t_ms, void* ctx), void* ctx) {
        bare_ack_hook_ = hook;
        bare_ack_ctx_ = ctx;
    }
    uint32_t dropped_events() const { return dropped_events_; }
    uint32_t last_frame_end_ms() const { return last_frame_end_ms_; }

private:
    static constexpr size_t kRingHexBudget = 240;

    struct OpenReq { int pack; std::string cid2; uint32_t start_ms; uint32_t req_end_ms; bool valid; };
    OpenReq open_req_{0, "", 0, 0, false};

    // scanner state
    bool        in_frame_ = false;
    std::string frame_;
    uint32_t    frame_start_t_ms_ = 0;
    uint32_t    frame_end_t_ms_ = 0;
    bool        noise_open_ = false;
    uint32_t    last_rx_t_ms_ = 0;

    // stats
    uint32_t frames_total_ = 0;
    uint32_t checksum_errors_ = 0;
    uint32_t anomalies_ = 0;
    uint32_t noise_bytes_ = 0;
    uint32_t dropped_events_ = 0;
    int      cadence_ema_ms_ = 0;
    bool     has_requests_ = false;
    bool     watchdog_tripped_ = false;
    uint32_t last_request_start_ms_ = 0;
    uint32_t last_frame_end_ms_ = 0;
    uint32_t rate_anchor_ms_ = 0;
    uint32_t rate_anchor_total_ = 0;
    PackStats packs_[kNumPacks + 1];

    // ring buffers
    RingEntry   ring_[kRingSlots];
    size_t      ring_head_ = 0;
    size_t      ring_count_ = 0;
    bool        ring_frozen_ = false;
    bool        release_pending_ = false;
    uint32_t    release_at_ms_ = 0;
    uint32_t    ring_dropped_ = 0;
    std::vector<GapSample> gap_ring_;
    size_t      gap_head_ = 0;

    std::vector<LinkEvent> events_;

    void (*bare_ack_hook_)(uint32_t, void*) = nullptr;
    void* bare_ack_ctx_ = nullptr;
};

}  // namespace linksniff
