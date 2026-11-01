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

class WriteController;

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

// ---- guarded write client (issue #59) ---------------------------------------
// The allowlist is data with exactly one entry, mirroring
// docs/analysis/bms073-tools/rs485_write_param.py (the proven Mac tool):
// parameter 0x4C "Start Balance Voltage", 3400-3500 mV - "3.3 V is normal
// resting voltage; the 3.55 V cell alarm is the ceiling." Identical bounds
// on both write surfaces is deliberate: extending either is its own
// reviewed change; there is no force-path.
struct WriteAllowEntry {
    uint8_t   param;
    uint16_t  min_mv;
    uint16_t  max_mv;
    const char* name;
};
static const WriteAllowEntry kWriteAllowlist[] = {
    {0x4C, 3400, 3500, "Start Balance Voltage"},
};

// Observed Link slave addresses only: 00 is the standalone self-address,
// FF the broadcast probe address - both untested write territory.
static constexpr uint8_t kLinkPackFirst = 0x01;
static constexpr uint8_t kLinkPackLast  = 0x04;

inline const WriteAllowEntry* write_allowlist_find(uint8_t param) {
    for (const auto& e : kWriteAllowlist)
        if (e.param == param) return &e;
    return nullptr;
}

// Validation pass/fail with a reason line (goes to Write Status).
inline bool write_request_valid(uint8_t pack, uint8_t param, uint16_t value_mv,
                                std::string* why) {
    if (pack < kLinkPackFirst || pack > kLinkPackLast) {
        if (why) *why = "pack must be 01-04 (observed Link addresses)";
        return false;
    }
    const WriteAllowEntry* e = write_allowlist_find(param);
    if (!e) {
        if (why) *why = "parameter not on the write allowlist (0x4C only)";
        return false;
    }
    if (value_mv < e->min_mv || value_mv > e->max_mv) {
        if (why) *why = std::string(e->name) + ": " + std::to_string(value_mv) +
                        " mV outside " + std::to_string(e->min_mv) + "-" +
                        std::to_string(e->max_mv) + " mV";
        return false;
    }
    return true;
}

// CID2 0x49 int-parameter write frame:
//   ~ VER(80) ADR(00) CID1(D5) CID2(49) LENID(8008) [pack][param][u16 BE] CHK CR
// INFO is 4 bytes = 8 ASCII chars; LENID length 8 -> LCHK nibble 8 (same
// formula rs485_make_cmd uses). Byte-exact against both captured frames in
// the inject oracle - change them together.
inline std::string build_write_frame(uint8_t pack, uint8_t param, uint16_t value) {
    char info[9];
    snprintf(info, sizeof info, "%02X%02X%04X", (unsigned)pack, (unsigned)param,
             (unsigned)value);
    int len = 8;
    int digit_sum = (len / 256) + ((len / 16) % 16) + (len % 16);
    int lchk = (~digit_sum + 1) & 0xF;
    char body[25];
    snprintf(body, sizeof body, "8000D549%X%03X%s", lchk, len, info);
    std::string result = "~";
    result += body;
    result += rs485_calc_chksum(body);
    result += "\r";
    return result;
}

// Injection timing (docs/PROTOCOL_REFERENCE.md measured Link bus):
//   624 ms slots leave ~436 ms idle; the accepted-silence window sits at
//   150..250 ms after the last RX - deep enough to be real idle, far
//   enough from the master's next poll (~436 ms) that our ~27 ms frame +
//   ack turnaround (~15 ms measured) land inside the same slot. The
//   150 ms ack deadline keeps the no-ack/collision verdict BEFORE the
//   master's next poll can arrive (worst case: fire at 250, +27 frame,
//   +150 verdict = 427 < 436): a plain no-ack can never be mislabeled
//   "collision" by the next slot's traffic (review round 3).
//   ONE FRAME PER PRESS - the Mac tool's posture, reviewer-ruled: no ack
//   never means unwritten, and a retry could write the same value twice
//   against unknown flash endurance. A late ack is still LISTENED for
//   until the cooldown passes and reported, but it never triggers
//   another transmit. kTxDueCooldownMs rate-limits presses across
//   operations; kArmExpiryMs mirrors the Mac tool's give-up-when-no-gap
//   behavior; kCadenceFreshMs requires a live poll window.
static constexpr uint32_t kGapConfirmMs    = 150;
static constexpr uint32_t kGapMaxMs        = 250;
static constexpr uint32_t kAckDeadlineMs   = 150;
static constexpr int32_t  kCadenceFreshMs  = 1500;
static constexpr uint32_t kArmExpiryMs    = 6000;
static constexpr uint32_t kTxDueCooldownMs = 4000;

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
    AckForWrite,      // bare ack consumed by a pending injection wait (not an anomaly)
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

    // Fire-discipline gate for the write client: a poll cadence seen
    // recently. No request ever seen, or a stale cadence, both read as
    // not-established - silence alone is not a window.
    bool bus_established(uint32_t now) const {
        return has_requests_ && (int32_t)(now - last_request_start_ms_) <= kCadenceFreshMs;
    }

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
        last_pair_ms_ = frame_end_t_ms_;  // clean request/response pair completed
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

    // Defined after WriteController (ack-waiter consult)
    void on_bare_ack(uint32_t t_ms);

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

    // ---- ack-waiter hookup (set only by the guarded write client) ---------
public:
    void set_ack_waiter(WriteController* w) { ack_waiter_ = w; }
    uint32_t dropped_events() const { return dropped_events_; }
    uint32_t last_frame_end_ms() const { return last_frame_end_ms_; }
    uint32_t last_rx_ms() const { return last_rx_t_ms_; }
    uint32_t last_pair_ms() const { return last_pair_ms_; }

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
    uint32_t last_pair_ms_ = 0;
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

    WriteController* ack_waiter_ = nullptr;
};

// ---------------------------------------------------------------------------
// Guarded injection controller, review round 3 (issue #59).
//
// The external reviewer's scenario harness found the round-1 controller
// could out-wait an outage indefinitely, reset on a second press, flurry
// frames under button spam, fire with no established cadence, and treat
// a late ack as no-ack (duplicate-write risk). All blocked; every rule
// below maps to one of those findings and to the Mac tool's posture
// (docs/analysis/bms073-tools/rs485_write_param.py):
//   - ONE FRAME PER PRESS (review round 3 ruling): the Mac tool's
//     posture - no retry. No ack never means the value was not written,
//     and an unknown-flash retry could write twice. A late ack is
//     LISTENED for until the cooldown passes and REPORTED, but never
//     triggers another transmit.
//   - presses while an operation is in flight are IGNORED, never reset
//   - every TX of ours demands one observed clean request/response pair
//     before the next fire, across operations (sticky need_clean_slot_)
//   - kTxDueCooldownMs rate-limits consecutive frames across presses
//   - fire needs BOTH a fresh poll cadence (kCadenceFreshMs) and a
//     150..250 ms window after the last RX; the 150 ms verdict deadline
//     keeps the no-ack decision ahead of the next master poll
//   - an armed write expires after kArmExpiryMs if no window appears,
//     including while waiting out the watchdog or a clean slot
//   - every TX-ending path sets the clean-pair requirement, opens the
//     late-ack listen window, and every wording states that
//     "no ack"/"collision"/"aborted after TX" never means the value was
//     not written
// ---------------------------------------------------------------------------
class WriteController {
public:
    enum class Status { Idle, Denied, Armed, WaitAck, Acked, NoAck, Collision, Aborted, Expired };

    using EnableFn = bool (*)(void*);
    using TxFn = bool (*)(const std::string&, void*);

    WriteController(BmsLinkCore& core, EnableFn enable, TxFn tx, void* ctx)
        : core_(core), enable_(enable), tx_(tx), ctx_(ctx) {}

    // explicit one-shot trigger (the Send button). A press while an
    // operation is in flight is IGNORED - it never resets the pending
    // one-shot's ack window. need_clean_slot_ is deliberately sticky: any
    // TX of ours (this press or a previous one) demands one clean
    // request/response pair before anything may fire next.
    bool request(uint8_t pack, uint8_t param, uint16_t value_mv, uint32_t now) {
        if (status_ == Status::Armed || status_ == Status::WaitAck) {
            line_ = "ignored: previous write still in flight";
            return false;
        }
        std::string why;
        if (!write_request_valid(pack, param, value_mv, &why)) {
            deny(why);
            return false;
        }
        pending_ = Pending{pack, param, value_mv};
        has_tx_ = false;  // per-operation: a later op's wordings must not
                          // claim a frame this op never sent (round 3 must-fix)
        armed_at_ms_ = now;
        status_ = Status::Armed;
        line_ = "armed: pack " + hex2(pack) + " param " + hex2(param) +
                " (" + std::to_string(value_mv) + " mV); one frame on the next window, expires in 6 s";
        return true;
    }

    void tick(uint32_t t_ms) {
        switch (status_) {
            case Status::Armed:    tick_armed(t_ms); break;
            case Status::WaitAck:  tick_wait_ack(t_ms); break;
            default: break;  // terminal: nothing fires without a fresh press
        }
    }

    void on_ack(uint32_t t_ms) {
        if (status_ == Status::WaitAck) {
            status_ = Status::Acked;
            last_ack_latency_ms_ = (int)(t_ms - tx_started_ms_);
            line_ = "ack ok in " + std::to_string(last_ack_latency_ms_) +
                    " ms (bare RTN-00 ack, pack not identified: ack-level verification only)";
            end_tx_op_(t_ms);
        } else if (ack_listen_active_(t_ms)) {
            // report-only: the operation already ended; the status
            // unchanged, the straggler fact appended for the operator
            line_ += " | late bare ack " + std::to_string(t_ms - tx_started_ms_) +
                     " ms after our frame: likely the answer - value probably written"
                     " (ack-level only, pack not identified)";
        }
    }

    // the core consults this on every bare ack: true while an ack belongs
    // to our write - in-window, or the report-only late-ack listen window
    // left open by a just-ended operation (bounded by the cooldown)
    bool ack_expected(uint32_t now) const {
        return status_ == Status::WaitAck || ack_listen_active_(now);
    }

    Status status() const { return status_; }
    const std::string& status_line() const { return line_; }
    uint32_t tx_count() const { return tx_count_; }
    uint32_t collision_count() const { return collision_count_; }

private:
    struct Pending { uint8_t pack; uint8_t param; uint16_t value_mv; };

    void deny(const std::string& why) {
        status_ = Status::Denied;
        line_ = "denied: " + why;
    }

    // any TX of ours demands one clean observed pair before the next fire
    void require_clean_(uint32_t t_ms) {
        need_clean_slot_ = true;
        failed_at_ms_ = t_ms;
    }
    // every TX-ending outcome also opens the late-ack listen window,
    // bounded by the cooldown after our frame
    void end_tx_op_(uint32_t t_ms) {
        require_clean_(t_ms);
        late_ack_until_ms_ = last_tx_ms_ + kTxDueCooldownMs;
    }
    bool ack_listen_active_(uint32_t now) const {
        return late_ack_until_ms_ != 0 &&
               (status_ == Status::NoAck || status_ == Status::Collision ||
                (status_ == Status::Aborted && has_tx_)) &&
               (int32_t)(late_ack_until_ms_ - now) > 0;
    }

    void tick_armed(uint32_t t_ms) {
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = has_tx_ ? "sent; result unknown (aborted after TX - write enable switched off)"
                           : "aborted: write enable switched off";
            if (has_tx_) require_clean_(t_ms);
            return;
        }
        if ((int32_t)(t_ms - armed_at_ms_) > (int32_t)kArmExpiryMs) {
            status_ = Status::Expired;
            line_ = "expired: no safe window within " + std::to_string(kArmExpiryMs / 1000) +
                    " s (watchdog, cadence, clean-slot or silence waits); press Send again" +
                    (has_tx_ ? std::string(" - result unknown") : std::string());
            if (has_tx_) require_clean_(t_ms);
            return;
        }
        if (core_.watchdog_tripped()) return;           // bus in unknown state: hold
        if (!core_.bus_established(t_ms)) return;       // no fresh poll cadence: hold
        if (need_clean_slot_) {
            if ((int32_t)(core_.last_pair_ms() - failed_at_ms_) > 0)
                need_clean_slot_ = false;               // clean pair observed AFTER the TX/failure
            else
                return;
        }
        if (has_tx_ && (int32_t)(t_ms - last_tx_ms_) < (int32_t)kTxDueCooldownMs) return;
        uint32_t since = core_.since_last_rx(t_ms);
        if (since < kGapConfirmMs || since > kGapMaxMs) return;  // land mid-idle, not at the next poll's edge

        std::string frame = build_write_frame(pending_.pack, pending_.param, pending_.value_mv);
        tx_started_ms_ = t_ms;
        bool ok = tx_(frame, ctx_);
        has_tx_ = true;
        last_tx_ms_ = t_ms;
        armed_at_ms_ = t_ms;  // expiry clock restarts at each TX
        tx_count_++;  // every TX attempt counts, flush failures included:
                      // bytes may be on the wire (round 3: never undercount)
        if (!ok) {
            status_ = Status::Aborted;
            line_ = "aborted: uart flush failed - bytes may be on the wire; result unknown";
            end_tx_op_(t_ms);
            return;
        }
        status_ = Status::WaitAck;
        line_ = "sent, waiting for ack (frame " + std::to_string(frame.size()) + " chars)";
    }

    void tick_wait_ack(uint32_t t_ms) {
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = "sent; result unknown (aborted after TX - write enable switched off)";
            end_tx_op_(t_ms);
            return;
        }
        if ((int32_t)(t_ms - tx_started_ms_) < (int32_t)kAckDeadlineMs) return;
        bool traffic = (int32_t)(core_.last_rx_ms() - tx_started_ms_) > 0;
        if (traffic) collision_count_++;
        const char* verdict = traffic ? "collision" : "no ack";
        status_ = traffic ? Status::Collision : Status::NoAck;
        line_ = std::string(verdict) + " after the single frame; disarmed - result unknown"
                " (the pack may still have taken the value). Listening for a late ack until the"
                " cooldown passes; press Send for a new one-shot";
        end_tx_op_(t_ms);
    }

    static std::string hex2(uint8_t v) {
        char b[3];
        snprintf(b, sizeof b, "%02X", (unsigned)v);
        return b;
    }

    BmsLinkCore& core_;
    EnableFn enable_;
    TxFn tx_;
    void* ctx_;

    Pending pending_{0, 0, 0};
    Status status_ = Status::Idle;
    bool has_tx_ = false;
    bool need_clean_slot_ = false;
    uint32_t failed_at_ms_ = 0;
    uint32_t armed_at_ms_ = 0;
    uint32_t last_tx_ms_ = 0;
    uint32_t tx_started_ms_ = 0;
    uint32_t late_ack_until_ms_ = 0;
    uint32_t tx_count_ = 0;
    uint32_t collision_count_ = 0;
    int last_ack_latency_ms_ = 0;
    std::string line_ = "idle";
};

// Bare-ack routing: a pending injection wait consumes it (AckForWrite,
// not an anomaly); anything else stays unexpected config-plane oddity.
inline void BmsLinkCore::on_bare_ack(uint32_t t_ms) {
    last_frame_end_ms_ = frame_end_t_ms_;
    if (ack_waiter_ && ack_waiter_->ack_expected(t_ms)) {
        push_event(EventKind::AckForWrite, t_ms, 0, "bare ack for injected write");
        ack_waiter_->on_ack(t_ms);
        return;
    }
    push_event(EventKind::AnomalyAck, t_ms, 0, "bare ack with no write pending");
    anomalies_++;
    ring_now(t_ms, kRawBareAck);
}

}  // namespace linksniff
