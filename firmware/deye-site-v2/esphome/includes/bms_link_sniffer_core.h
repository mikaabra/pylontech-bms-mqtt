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
class ReadController;
class TeleOpController;

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

// ---- guarded block-read client (dongle PoC productionized) -----------------
// The D547 parameter-block read (docs/analysis/bms073-tools/rs485_verify_
// write.py): the pack answers a ~8000D547E002<pack> read with the full
// A222 block — 546 INFO chars, ~590 ms on the wire at 9600 — which
// overruns the ~436 ms idle window and collides with the master's next
// poll. Collision economics from the 2026-10-31 in-stack read-back: one
// garbled master slot, immediate recovery - accepted per press, never
// routine (a retry is a new deliberate press; reads are idempotent, so
// retry-by-press replaces the Mac tool's reference-based reconstruction
// proof: in firmware the trust rule is strictly whole-frame-checksum-
// valid + battery-number match, everything else is fail-closed.
static constexpr uint32_t kBlockWindowMs     = 1500;  // the tool's 1.5 s collect window
static constexpr int      kBlockMinInfoChars = 200;   // only the A222 block qualifies:
                                                    // B07E=126, 9052=164, C022=68 all below;
                                                    // 546 above - pinned, not guessed
// Parameter-block INFO layout (PROTOCOL_REFERENCE.md, fully decoded and
// fixture-pinned): [battery number: 1 byte][array: 271 bytes]; array =
// u16 idx 0x00-0x06 (14 B) + ONE stray byte at array offset 14 (observed
// 0x76, meaning unknown, required for alignment) + u16 idx 0x07-0x7C.
// int-parameter entry INFO char offset = 4*idx + 4 (checked: 0x4C -> 308,
// byte offset 154, exactly where both captured reads agree).
static constexpr size_t   kBlockBatteryChars = 2;    // INFO chars 0..1
static constexpr size_t   kBlockParamBaseChar = 4;    // 4*idx + this
// the balance param stays pinned: the per-pack sensors + the step-4
// write-verify semantics key on it
static constexpr uint8_t  kReadBalanceParam  = 0x4C;
// the generic read surface covers exactly the u16 entry band the A222
// block exposes (0x07..0x7C); 0x00..0x06 are 4-byte entries and 0x7D+
// are bit-params/tail - neither is decoded, so neither is readable here
static constexpr uint8_t  kReadParamFirst    = 0x07;
static constexpr uint8_t  kReadParamLast     = 0x7C;

inline bool read_param_valid(uint8_t param) {
    return param >= kReadParamFirst && param <= kReadParamLast;
}

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
        if (why) *why = "parameter not on the write allowlist";
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
//   150 ms ack deadline runs from TX START and already covers the ~27 ms
//   frame, keeping the verdict BEFORE the master's next poll can arrive
//   (worst case: fire at 250 + 150 verdict = 400 < 436): a plain no-ack
//   can never be mislabeled "collision" by the next slot's traffic
//   (review rounds 3-4).
//   ONE FRAME PER PRESS - the Mac tool's posture, reviewer-ruled: no ack
//   never means unwritten, and a retry could write the same value twice
//   against unknown flash endurance. A late ack is still LISTENED for
//   until the cooldown passes and reported, but it never triggers
//   another transmit. kTxDueCooldownMs rate-limits presses across
//   operations; kArmExpiryMs mirrors the Mac tool's give-up-when-no-gap
//   behavior; kCadenceFreshMs requires a live poll window.
static constexpr uint32_t kGapConfirmMs    = 150;
static constexpr uint32_t kGapMaxMs        = 250;

// Long-gap aim, measured live 2026-11-11 (Link Gap Report) + the
// reviewer's runway equation: regular idle runs 470-530 ms, one ~1100 ms
// rotation gap every 3rd slot. The A222 block answer needs ~585 ms of
// wire + the pack's turnaround (probe-family answers measured 275-277 ms
// live; 300 ms budgeted) => a fire at T ends its answer at
// T + 300 + 590. The latest safe fire into a measured ~1100 ms long gap
// is therefore ~210 ms; kAimLatestFireMs sits at 180 with jitter
// headroom. The old detected-mid-gap band (600-1000 ms) can never
// satisfy the equation (600 + 300 + 590 > any observed long gap) - a
// detected-long fire was a guaranteed 1-3 ms tail race, so the detected
// path is gone. Missed prediction costs one rotation of latency, not a
// damaged frame.
static constexpr uint32_t kLongGapMs         = 700;
static constexpr uint32_t kLongGapEverySlots = 3;
static constexpr uint32_t kAimGapConfirmMs   = 115;  // above the 100 ms frame-pause ceiling
static constexpr uint32_t kAimLatestFireMs   = 180;  // reviewer runway cap: fire + turnaround + wire <= gap

// Pre-long corridor v2 (2026-11-12 re-derivation, reviewer-ruling round):
// the rotation = [42->p | ans ~122-150 ms | quiet ~455] / [44->p | ans
// ~104 ms | quiet ~500] pairs at ~628 ms, plus ONE ~1080 ms long silence
// per ~2511 ms. The 562-char block is ~1094 ms head-to-tail - longer
// than every quiet, so no fire point inside any gap carries it
// (wire-proven: all four sweep reads cut at bytes 417-425). The only
// collision-free corridor is the single quiet that closes right before
// a long gap: the closing exchange transits our request-to-block turn,
// then the whole silence carries the block. Row 1 of the verification
// session was a natural corridor read - our request at ~133 ms into the
// pre-long quiet rode the silence and arrived WHOLE at the master end;
// it failed at the sniffer end with pack-02's answer tail ending ~0 ms
// from our block head. HEAD MARGIN is the deciding variable: each extra
// ms of fire offset buys a ms of clearance. Band re-derived against the
// wire-measured worst cases (closing answers 117 B/~122 ms and 144
// B/~150 ms, 10-25 ms answer-start delay, block turnaround 506-532 ms),
// the reviewer-checked arithmetic:
//   block start .............. quiet-open + 756-832 ms
//   closing answer ends ..... quiet-open + 653-696 ms
//   head margin ............. +60 to +179 ms across all variants
//   block end ............... quiet-open + 1341-1417 ms
//   tail margin to silence .. >= 316 ms
//   our ~21 ms request clears the closing poll by >=134 ms (the 455-kind
//   quiet; 154 ms on the 500-kind)
// Deliberately NOT capped by kAimLatestFireMs - that constant is the
// runway cap for firing INSIDE a long gap and has no meaning here.
// Band width 50 ms = 5+ feed ticks: real ticks at odd phases (e.g.
// since=257) always land inside (v1's 175-180 band could be straddled
// by 10 ms ticks at 171/181 and never fire - oracle case pins the
// jittered landing).
// The predictor: every observed >=kLongGapMs quiet resyncs the phase;
// stale predictions slide forward one period at a time, and a
// prediction older than kPreLongStaleMs unlocks the aim entirely - the
// rotation may have changed shape, and bounded slides keep the loop
// trivial (<=4 iterations at the stale bound). The lead window's upper
// bound is 700: the worst-case corridor lead is ~696 ms (144 B closing
// answer + 25 ms start delay) - those corridors are inside the window;
// a wider lead is not a corridor and costs latency, not safety.
static constexpr uint32_t kLongGapPeriodMs    = 2511;   // long-to-long quiet-open period (wire)
static constexpr uint32_t kPreLongLeadMinMs   = 630;     // corridor lead lower bound (long-open - quiet-open)
static constexpr uint32_t kPreLongLeadMaxMs   = 700;     // corridor lead upper bound (worst-case lead ~696)
static constexpr uint32_t kPreLongStaleMs     = 10000;   // unlock after this long without an observed long
static constexpr uint32_t kPreLongFireFromMs  = 250;     // re-derived fire band, lower bound
static constexpr uint32_t kPreLongFireToMs    = 300;     // re-derived fire band, upper bound
static constexpr uint32_t kAckDeadlineMs   = 150;
static constexpr int32_t  kCadenceFreshMs  = 1500;
static constexpr uint32_t kArmExpiryMs    = 6000;
static constexpr uint32_t kTxDueCooldownMs = 4000;

// One shared TX gate for BOTH tx clients (review: the cooldown used to be
// one-directional - the read waited on the write's latch but not vice
// versa, and the clean-pair baseline moved only on the writer's own TX).
// Every frame of ours, from either client, prices into the same 4 s
// cooldown latch AND sets the same clean-pair baseline: nothing of ours
// fires until one clean master exchange has been observed after our most
// recent frame. Latch hygiene (expiry sweeping) runs in both controllers'
// ticks; clearing is idempotent.
struct SharedTxGate {
    bool     cooldown_active = false;
    uint32_t last_tx_ms = 0;
    bool     need_clean_slot = false;
    uint32_t clean_pair_baseline = 0;
};

// Ring entries and event details cross publish boundaries that require
// valid UTF-8: native-API protobuf rejects bad sequences (found on-device
// 2026-11-11 when garbled UART noise — bytes >= 0x80 — rode text-sensor
// updates and killed every subscribed consumer). The Link dialect's frames
// are ASCII-hex text, so printable bytes stay verbatim; anything else
// escapes as \xNN.
inline std::string sanitize_publish_text(const std::string& s) {
    static const char* kHexDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        unsigned char b = (unsigned char)ch;
        if (b >= 0x20 && b <= 0x7E) {
            out += ch;
        } else {
            out += "\\x";
            out += kHexDigits[b >> 4];
            out += kHexDigits[b & 0xF];
        }
    }
    return out;
}

// The captured bare ack (RTN 00, empty INFO), interior without SOI/EOI.
static const char kRawBareAck[] = "8000D5000000FD9F";

// CID2 0x47 int-parameter block-read frame:
//   ~ VER(80) ADR(00) CID1(D5) CID2(47) LENID(E002) [pack] CHK CR
// Byte-exact against bms073.build(cid2=0x47, info=f"{pack:02X}") — the
// captured request for pack 01 is ~8000D547E00201FD1C (pinned in the
// read oracle against the Python builder's output).
inline std::string build_read_frame(uint8_t pack) {
    int len = 2;  // INFO = the pack address, 2 ASCII-hex chars
    int digit_sum = (len / 256) + ((len / 16) % 16) + (len % 16);
    int lchk = (~digit_sum + 1) & 0xF;
    char body[20];
    snprintf(body, sizeof body, "8000D547%X%03X%02X", lchk, len, (unsigned)pack);
    std::string result = "~";
    result += body;
    result += rs485_calc_chksum(body);
    result += "\r";
    return result;
}

// ---- guarded teleControl client (SYS_OFF restart + LMCHG wake pair) --------
// All frames below are byte-exact against the 2026-11-11 vendor-tool
// wire captures (PROTOCOL_REFERENCE "teleControl captured on the wire");
// the telecontrol oracle pins every literal.
//
// teleControl gates (the XML teleControl_Group bit numbers, verified on
// the wire): 00 DISCHG, 01 CHG, 02 LMCHG, 04 SYS_OFF.
static constexpr uint8_t kTeleGateLmchg  = 0x02;
static constexpr uint8_t kTeleGateSysOff = 0x04;

// D5C9 presence probe (the addressing mechanism: the D545 write carries
// no target — the probe selects the pack, the pack answers with a LENID
// 9052 identity block, and the next write lands on the freshly-probed
// pack). CHK family = the shared ASCII-complement function: the oracle
// pins probes for every address against the captured class (whole-frame
// byte-sum 1033 = 0x409, CHK = FD (0x0C - addr)).
inline std::string build_probe_frame(uint8_t addr) {
    char body[20];
    snprintf(body, sizeof body, "8000D5C9E002%02X", (unsigned)addr);
    std::string result = "~";
    result += body;
    result += rs485_calc_chksum(body);
    result += "\r";
    return result;
}

// D545 teleControl enable frame: LENID A006, INFO = [variant][gate][10],
// computed CHK — reproduces every captured enable literal in the catalog
// (SYS_OFF both variants, CHG, DISCHG, LMCHG; class 938 = 0x3AA).
inline std::string build_telecontrol_enable(uint8_t variant, uint8_t gate) {
    char body[24];
    snprintf(body, sizeof body, "8000D545A006%02X%02X10", (unsigned)variant, (unsigned)gate);
    std::string result = "~";
    result += body;
    result += rs485_calc_chksum(body);
    result += "\r";
    return result;
}

// Captured new-generation LMCHG disable, byte-exact literal
// (~ 80 00 D5 45 A0 06 04 02 01 FF C2). Disable frames from the vendor
// tool carry an off-formula CHK (FF C(4-gate)) that the packs demonstrably
// accept battery-side (this exact sequence woke pack 04) — replicate the
// wire, not the formula. No old-gen disable was ever captured: the wake
// pair is new-gen-only and refuses old-gen targets rather than guess.
static const char kLmchgDisableNewGen[] = "~8000D545A006040201FFC2\r";

// The probe answer's LENID (identity block, INFO = 00 [id] ...; captured
// literal "80 00 D5 00 90 52 00 [id] ..."). Routing matches LENID exactly.
static const char kProbeAnswerLenid[] = "9052";

// Identity-answer content gate (reviewer-prescribed): after the leading
// 00, the identity block carries a printable-ASCII ID (captured
// 4A473235 = "JG25"); telemetry answers carry the battery number there
// (0001... -> bytes 0x00 0x01, not printable). Generic - no
// battery-specific data. A candidate that passes shape + structure but
// fails this content test is a telemetry answer in disguise and is
// rejected: the FET-control path never fires on it.
inline bool probe_identity_content(const std::string& info) {
    if (info.size() < 6) return false;
    unsigned long v;
    if (!parse_hex(info.substr(2, 4), v)) return false;
    uint8_t b0 = (uint8_t)((v >> 8) & 0xFF);
    uint8_t b1 = (uint8_t)(v & 0xFF);
    return b0 >= 0x20 && b0 <= 0x7E && b1 >= 0x20 && b1 <= 0x7E;
}

// Tool-observed variant byte per Link address (PROTOCOL_REFERENCE
// teleControl catalog: the tool sent 01 for packs at addresses 01-02
// and 04 for 03-04; both landed). Wire fact, not a behavior claim.
inline bool telecontrol_variant(uint8_t pack, uint8_t& variant) {
    if (pack == 1 || pack == 2) { variant = 0x01; return true; }
    if (pack == 3 || pack == 4) { variant = 0x04; return true; }
    return false;
}

// Battery number from a block INFO (chars 0..1). Also the pack identity
// check: a trusted read must name the pack we asked (RTN-00 responses
// carry no requester echo — the reference-data identify is all we get).
inline bool block_battery(const std::string& info, uint8_t& battery) {
    unsigned long v;
    if (!parse_hex(info.substr(0, kBlockBatteryChars), v)) return false;
    battery = (uint8_t)v;
    return true;
}

// u16 int-parameter entry from a block INFO by index (0x07-0x7C band;
// the 0x00-0x06 band is 4-byte and not surfaced). Returns false when the
// INFO is short (collision-truncated) or the entry chars are not hex.
inline bool block_param_u16(const std::string& info, uint8_t idx, uint16_t& value) {
    size_t off = kBlockParamBaseChar + (size_t)idx * 4;
    if (info.size() < off + 4) return false;
    unsigned long v;
    if (!parse_hex(info.substr(off, 4), v)) return false;
    value = (uint16_t)v;
    return true;
}

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
    BlockForRead,     // D547 block (or its damaged remnant) consumed by a pending read (not an anomaly)
    AckForTeleOp,     // bare ack consumed by a pending teleControl frame (not an anomaly)
    ProbeAnswer,      // 9052 identity block routed to a pending teleControl probe (not an anomaly)
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
    bool long_gap_due() const { return slots_since_long_gap_ >= kLongGapEverySlots - 1; }

    // Pre-long-corridor query (see the kPreLongLead* comment block).
    // Stale check FIRST: a prediction older than kPreLongStaleMs unlocks
    // the aim (the rotation changed shape) and keeps the catch-up loop
    // bounded without a separate iteration cap.
    bool pre_long_quiet(uint32_t quiet_open_ms, uint32_t now_ms) const {
        if (!long_open_seen_) return false;
        if ((int32_t)(now_ms - last_long_open_ms_) > (int32_t)kPreLongStaleMs) return false;
        uint32_t p = last_long_open_ms_ + kLongGapPeriodMs;
        while ((int32_t)(now_ms - (p + kLongGapMs)) > 0) p += kLongGapPeriodMs;
        int32_t lead = (int32_t)(p - quiet_open_ms);
        return lead >= (int32_t)kPreLongLeadMinMs && lead <= (int32_t)kPreLongLeadMaxMs;
    }
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

    std::string gap_report() const {
        std::string seq;
        uint64_t sum = 0; uint32_t cnt = 0, mn = 0, mx = 0;
        // ring order: growth phase is already oldest->newest; once full,
        // gap_head_ marks the next overwrite slot, i.e. the oldest entry
        size_t n = gap_ring_.size();
        for (size_t k = 0; k < n; k++) {
            const GapSample& s = gap_ring_[(n < 32) ? k : (gap_head_ + k) % 32];
            sum += s.gap_ms; cnt++;
            if (mn == 0 || s.gap_ms < mn) mn = s.gap_ms;
            if (s.gap_ms > mx) mx = s.gap_ms;
            if (!seq.empty()) seq += ",";
            seq += std::to_string(s.gap_ms);
        }
        std::string out = "n=" + std::to_string(cnt);
        if (cnt) {
            out += " min=" + std::to_string(mn)
                 + " avg=" + std::to_string((uint32_t)(sum / cnt))
                 + " max=" + std::to_string(mx)
                 + " gaps=" + seq;
        }
        return out;
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
    // window auto-releases once EVERY trigger has cleared (recovery
    // re-opens recording, the publish follows after 5 s); the Dump button
    // (wrapper) stays as the operator's manual release.
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
        // Our own D547 block read's answer (or its collision-damaged
        // remnant) routes to the read controller before any pairing,
        // size or checksum verdict: the reader grades the frame itself
        // and the bus record keeps it as a read artifact, not a pack or
        // master fault.
        if (len >= kBlockMinInfoChars && read_waiter_ &&
            on_read_candidate_(t_ms, frame_start_t_ms_, raw, len))
            return;
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
            // a 9052 frame is only OUR probe answer when the controller's
            // structural acceptance says so (first frame since our probe,
            // no master request open, short turnaround) AND the content is
            // an identity block (printable-ASCII ID, not a battery
            // number); routine 0x44 telemetry answers share the LENID and
            // must fall through to normal pairing - the reviewer's
            // blocker case
            if (lenid == kProbeAnswerLenid && consume_probe_answer_(t_ms, info)) return;
            // any window-opening response the acceptance did not consume
            // is recorded, not argued about (the 17:18 lesson) - see
            // record_probe_stray_ for the identification format
            record_probe_stray_(t_ms, info, lenid);
            on_response(t_ms);
            return;
        }
        // config-plane traffic: the single most important anomaly class.
        // A D5C9 presence probe from ANYONE re-latches the teleControl
        // write target — if one of OUR sequences is mid-flight, abort it
        // before any unsent frame (fail-closed: the target is no longer
        // provably ours).
        if (cid2 == "C9") notify_telecom_probe_(t_ms);
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
        // a fired read's ride-gap datum completes on the FIRST master
        // request after its fire: ring-recorded beside the start-delay
        // line, so verdicts line up with gap lengths directly (the
        // reviewer's instrument; the pack-02 clean->clip flip between
        // adjacent slots at identical conditions showed it decides)
        record_ride_gap_(t_ms);
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
        pairs_total_++;
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
    // Defined after ReadController (batch candidate consult) — true when
    // the frame was consumed as our own pending block read's answer
    bool on_read_candidate_(uint32_t t_ms, uint32_t answer_start_ms,
                            const std::string& raw, int len);

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
        slots_since_long_gap_ = (s.gap_ms >= kLongGapMs) ? 0 : slots_since_long_gap_ + 1;
        if (s.gap_ms >= kLongGapMs) {
            // quiet-open time of the observed long: resyncs the corridor predictor
            last_long_open_ms_ = (s.t_ms >= s.gap_ms) ? s.t_ms - s.gap_ms : 0;
            long_open_seen_ = true;
        }
    }

    void ring_now(uint32_t t_ms, const std::string& hex) {
        if (ring_frozen_) {
            ring_dropped_++;
            return;
        }
        RingEntry e;
        e.t_ms = t_ms;
        std::string text = sanitize_publish_text(hex);
        if (text.size() > kRingHexBudget) {
            e.hex = text.substr(0, kRingHexBudget) + "…(+" + std::to_string(text.size() - kRingHexBudget) + ")";
        } else {
            e.hex = text;
        }
        ring_[ring_head_] = e;
        ring_head_ = (ring_head_ + 1) % kRingSlots;
        if (ring_count_ < kRingSlots) ring_count_++;
    }

    void push_event(EventKind k, uint32_t t, int pack, const std::string& detail) {
        if (events_.size() >= 64) { dropped_events_++; return; }  // wrapper drains every 10 ms
        events_.push_back(LinkEvent{k, t, pack, sanitize_publish_text(detail)});
    }

    // ---- ack-waiter hookup (set only by the guarded write client) ---------
public:
    void set_ack_waiter(WriteController* w) { ack_waiter_ = w; }
    void set_read_waiter(ReadController* r) { read_waiter_ = r; }
    void set_telecom_waiter(TeleOpController* t) { telecom_waiter_ = t; }
    uint32_t dropped_events() const { return dropped_events_; }
    uint32_t last_frame_end_ms() const { return last_frame_end_ms_; }
    uint32_t last_rx_ms() const { return last_rx_t_ms_; }
    uint32_t last_pair_ms() const { return last_pair_ms_; }
    // completed request/response pairs since boot - a monotonic COUNT the
    // write controller baselines its clean-slot wait on (round 5: no
    // timestamp arithmetic, no millis wrap)
    uint32_t pairs_total() const { return pairs_total_; }

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
    uint32_t pairs_total_ = 0;
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
    uint32_t    slots_since_long_gap_ = 0;
    uint32_t    last_long_open_ms_ = 0;   // quiet-open ms of the last observed long gap
    bool        long_open_seen_ = false;  // corridor predictor lock (one observed long)

    std::vector<LinkEvent> events_;

    WriteController* ack_waiter_ = nullptr;
    ReadController* read_waiter_ = nullptr;
    TeleOpController* telecom_waiter_ = nullptr;
    // Defined after TeleOpController (incomplete-type boundary, same
    // pattern as on_bare_ack / on_read_candidate_)
    bool consume_probe_answer_(uint32_t t_ms, const std::string& info);
    void record_probe_stray_(uint32_t t_ms, const std::string& info, const std::string& lenid);
    void record_ride_gap_(uint32_t t_ms);
    void notify_telecom_probe_(uint32_t t_ms);
    bool route_teleop_ack_(uint32_t t_ms);
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
//     before the next fire, across operations (sticky shared clean-slot latch)
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

    WriteController(BmsLinkCore& core, SharedTxGate& gate, EnableFn enable, TxFn tx, void* ctx)
        : core_(core), gate_(gate), enable_(enable), tx_(tx), ctx_(ctx) {}

    // explicit one-shot trigger (the Send button). A press while an
    // operation is in flight is IGNORED - it never resets the pending
    // one-shot's ack window. the shared clean-slot latch is deliberately sticky: any
    // TX of ours from either client (this press or a previous one) demands one clean
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
        // Long-uptime hygiene (review round 5): this device stays up for
        // months, and a timestamp that sits idle for weeks flips the int32
        // ms-difference compares at the ~24.8-day wrap - a stale cooldown
        // could block later presses and a stale listen window could swallow
        // a stray bare ack and append a ghost "value probably written".
        // This tick runs every 10 ms in EVERY state, so each latch is
        // cleared here the moment it expires and the state machines below
        // only ever test latches (or see intervals the 10 ms cadence keeps
        // short):
        //   - the shared TX-gate cooldown latch clears at kTxDueCooldownMs
        //   - the late-ack listen window closes once it passes
        //   - a completed pair AFTER any of our frames clears the clean-slot
        //     wait - the baseline is core's pair COUNT, not a clock: a bus
        //     silent for weeks moves no count, so it cannot wrap by construction
        if (gate_.cooldown_active &&
            (int32_t)(t_ms - gate_.last_tx_ms) >= (int32_t)kTxDueCooldownMs)
            gate_.cooldown_active = false;
        if (late_ack_until_ms_ != 0 && (int32_t)(late_ack_until_ms_ - t_ms) <= 0)
            late_ack_until_ms_ = 0;
        if (gate_.need_clean_slot && core_.pairs_total() > gate_.clean_pair_baseline)
            gate_.need_clean_slot = false;
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
            end_tx_op_();
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

    // any TX of ours demands one clean observed pair before the next fire.
    // Round 5: the baseline is core's completed-pair COUNT, not a stored
    // timestamp - "a pair completed after our TX" cannot wrap, and tick()
    // clears the latch as soon as the count moves, so the armed gate never
    // does 25-day-old clock arithmetic.
    void require_clean_() {
        gate_.need_clean_slot = true;
        gate_.clean_pair_baseline = core_.pairs_total();
    }
    // every TX-ending outcome also opens the late-ack listen window,
    // bounded by the cooldown after our frame (tick() closes the latch)
    void end_tx_op_() {
        require_clean_();
        late_ack_until_ms_ = gate_.last_tx_ms + kTxDueCooldownMs;
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
            // round 4: has_tx_ is always false here (a TX moves the status
            // to WaitAck) - the "after TX" branch was dead code
            line_ = "aborted: write enable switched off";
            return;
        }
        if ((int32_t)(t_ms - armed_at_ms_) > (int32_t)kArmExpiryMs) {
            status_ = Status::Expired;
            line_ = "expired: no safe window within " + std::to_string(kArmExpiryMs / 1000) +
                    " s (watchdog, cadence, clean-slot or silence waits); press Send again";
            return;
        }
        if (core_.watchdog_tripped()) return;           // bus in unknown state: hold
        if (!core_.bus_established(t_ms)) return;       // no fresh poll cadence: hold
        if (gate_.need_clean_slot) return;              // no clean pair since our last frame, either client (latch: tick clears)
        // cooldown across operations (rounds 4-5): a LATCH set at every
        // TX attempt, either client (flush failures included - bytes may
        // be on the wire either way) and cleared by tick() 4 s later, so
        // this gate never compares a weeks-idle timestamp
        if (gate_.cooldown_active) return;
        uint32_t since = core_.since_last_rx(t_ms);
        if (since < kGapConfirmMs || since > kGapMaxMs) return;  // land mid-idle, not at the next poll's edge

        std::string frame = build_write_frame(pending_.pack, pending_.param, pending_.value_mv);
        tx_started_ms_ = t_ms;
        bool ok = tx_(frame, ctx_);
        has_tx_ = true;
        gate_.last_tx_ms = t_ms;
        gate_.cooldown_active = true;  // round 5: latch, tick() clears it 4 s later
        tx_count_++;  // every TX attempt counts, flush failures included:
                      // bytes may be on the wire (round 3: never undercount)
        if (!ok) {
            status_ = Status::Aborted;
            line_ = "aborted: uart flush failed - bytes may be on the wire; result unknown";
            end_tx_op_();
            return;
        }
        status_ = Status::WaitAck;
        line_ = "sent, waiting for ack (frame " + std::to_string(frame.size()) + " chars)";
    }

    void tick_wait_ack(uint32_t t_ms) {
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = "sent; result unknown (aborted after TX - write enable switched off)";
            end_tx_op_();
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
        end_tx_op_();
    }

    static std::string hex2(uint8_t v) {
        char b[3];
        snprintf(b, sizeof b, "%02X", (unsigned)v);
        return b;
    }

    BmsLinkCore& core_;
    SharedTxGate& gate_;
    EnableFn enable_;
    TxFn tx_;
    void* ctx_;

    Pending pending_{0, 0, 0};
    Status status_ = Status::Idle;
    bool has_tx_ = false;
    uint32_t armed_at_ms_ = 0;
    uint32_t tx_started_ms_ = 0;
    uint32_t late_ack_until_ms_ = 0;
    uint32_t tx_count_ = 0;
    uint32_t collision_count_ = 0;
    int last_ack_latency_ms_ = 0;
    std::string line_ = "idle";
};

// Bare-ack routing: a pending injection wait consumes it (AckForWrite,
// not an anomaly); a pending teleControl frame likewise (AckForTeleOp);
// anything else stays unexpected config-plane oddity.
inline void BmsLinkCore::on_bare_ack(uint32_t t_ms) {
    last_frame_end_ms_ = frame_end_t_ms_;
    if (ack_waiter_ && ack_waiter_->ack_expected(t_ms)) {
        push_event(EventKind::AckForWrite, t_ms, 0, "bare ack for injected write");
        ack_waiter_->on_ack(t_ms);
        return;
    }
    if (route_teleop_ack_(t_ms)) return;
    push_event(EventKind::AnomalyAck, t_ms, 0, "bare ack with no write pending");
    anomalies_++;
    ring_now(t_ms, kRawBareAck);
}

// ---------------------------------------------------------------------------
// Guarded block-read client — the USB-dongle PoC's rs485_verify_write.py
// productionized onto this device (the dongle is retired). One press = ONE
// D547 read frame, injected through the same fire gates as the write client
// (enable switch re-checked at every transition, fresh cadence, clean pair
// since any of our TX, 150..250 ms confirmed silence, shared 4 s cooldown).
// The pack's ~590 ms block answer overruns the idle window and collides
// with the master's next poll by design physics - each press accepts one
// possibly-garbled master slot. Grading diverges from the Mac tool
// deliberately: the reference-based collision reconstruction is NOT ported
// (its reference block is one standalone capture - battery-specific
// data does not belong in firmware); reads are idempotent, so a
// collision-damaged verdict simply invites a new deliberate press, and
// the strict trust rule is: whole-frame checksum valid AND battery
// number = the requested pack. Nothing else publishes a value.
//   ReadOk      trusted: checksum valid, pack matches, value extracted;
//               the line states whether it matches the last written value
//   Mismatch    trusted read differs from the write we last sent - the
//               step-4 write-verify FAIL case (stale or not applied)
//   WrongPack   checksum valid but battery number != requested pack
//   Damaged     envelope or checksum broken by the collision - value NOT
//               trustworthy regardless of what survived; press again
//   NoBlock     window closed with no block-sized answer
// ---------------------------------------------------------------------------
class ReadController {
public:
    enum class Status {
        Idle, Denied, Armed, WaitBlock,
        ReadOk, Mismatch, WrongPack, Damaged, NoBlock, Aborted, Expired
    };

    using EnableFn = bool (*)(void*);
    using TxFn = bool (*)(const std::string&, void*);

    ReadController(BmsLinkCore& core, SharedTxGate& gate,
                   EnableFn enable, TxFn tx, void* ctx)
        : core_(core), gate_(gate), enable_(enable), tx_(tx), ctx_(ctx) {}

    bool request(uint8_t pack, uint8_t param, bool has_expect, uint16_t expect, uint32_t now,
                 bool aim_long_gap = false) {
        if (status_ == Status::Armed || status_ == Status::WaitBlock) {
            line_ = "ignored: previous read still in flight";
            return false;
        }
        if (pack < kLinkPackFirst || pack > kLinkPackLast) {
            deny("pack must be 01-04 (observed Link addresses)");
            return false;
        }
        if (!read_param_valid(param)) {
            deny("parameter 0x" + hex2(param) + " outside the readable u16 band 0x07-0x7C");
            return false;
        }
        pack_ = pack;
        param_ = param;
        has_expect_ = has_expect;
        expect_raw_ = expect;
        aim_long_gap_ = aim_long_gap;
        armed_at_ms_ = now;
        status_ = Status::Armed;
        line_ = "armed: pack " + hex2(pack) + " param 0x" + hex2(param) +
                " block read; one frame on the next " +
                (aim_long_gap ? "(long-gap-aligned) " : "") + "window, expires in 6 s";
        return true;
    }

    void tick(uint32_t t_ms) {
        // same long-uptime hygiene as the write client: latches expire
        // here on the 10 ms cadence, never by stale timestamp compares
        if (gate_.cooldown_active &&
            (int32_t)(t_ms - gate_.last_tx_ms) >= (int32_t)kTxDueCooldownMs)
            gate_.cooldown_active = false;
        if (gate_.need_clean_slot && core_.pairs_total() > gate_.clean_pair_baseline)
            gate_.need_clean_slot = false;
        switch (status_) {
            case Status::Armed:     tick_armed(t_ms); break;
            case Status::WaitBlock: tick_wait_block(t_ms); break;
            default: break;  // terminal: nothing fires without a fresh press
        }
    }

    // core consults this for every block-sized RTN-00 frame: true while a
    // fired read is inside its collection window
    bool block_window_active(uint32_t now) const {
        return status_ == Status::WaitBlock &&
               (int32_t)(now - tx_started_ms_) <= (int32_t)kBlockWindowMs;
    }

    // The core routes our block here before pairing/classification. The
    // returned short line is the ring/event record; the long status_ line
    // is the operator's evidence. Grading is fail-closed everywhere. The
    // answer's start delay (fire -> answer-first-byte) rides both lines:
    // it is THE variable the block-answer timing theory depends on (the
    // reviewer: log it - a few bytes instead of 562), so every grade,
    // clean or damaged, carries the measurement.
    std::string on_block(const std::string& raw, int len, uint32_t answer_start_ms) {
        int start_delay = (int32_t)(answer_start_ms - tx_started_ms_);
        if (start_delay < 0) start_delay = 0;
        answer_start_delay_ms_ = start_delay;
        std::string delay_tag = " start+" + std::to_string(start_delay) + "ms";
        size_t expect = (size_t)(12 + len + 4);
        std::string short_line = "D547 " + hex2(pack_) + " " + std::to_string(raw.size()) + "ch" + delay_tag;
        if (raw.size() != expect) {
            status_ = Status::Damaged;
            line_ = "collision-damaged: frame length altered (" + std::to_string(raw.size()) +
                    " chars, envelope says " + std::to_string(expect) +
                    "); value not trustworthy - press Read again for a new single read";
            return short_line + " damaged(len)";
        }
        std::string full = "~" + raw + "\r";
        if (!rs485_verify_checksum(full)) {
            status_ = Status::Damaged;
            line_ = "collision-damaged: whole-frame checksum invalid (the Mac tool's"
                    " reconstruction proof is not ported - a stored value only ever"
                    " publishes from a checksum-valid frame); press Read again";
            return short_line + " damaged(chk)";
        }
        std::string info = raw.substr(12, (size_t)len);
        uint8_t battery = 0;
        if (!block_battery(info, battery)) {
            status_ = Status::Damaged;
            line_ = "damaged: battery number chars unreadable; press Read again";
            return short_line + " damaged(batt)";
        }
        if (battery != pack_) {
            status_ = Status::WrongPack;
            line_ = "block answered by pack " + hex2(battery) + ", not the requested " +
                    hex2(pack_) + " - nothing published; press Read again";
            return short_line + " wrongpack(" + hex2(battery) + ")";
        }
        uint16_t got = 0;
        std::string ptag = "0x" + hex2(param_);
        if (!block_param_u16(info, param_, got)) {
            status_ = Status::Damaged;
            line_ = "damaged: " + ptag + " entry unreadable (short INFO or non-hex chars);"
                    " press Read again";
            return short_line + " damaged(param)";
        }
        value_raw_ = got;
        // 0x4C is mV by definition; every other param publishes raw — the
        // select label carries the vendor XML's name/scale/unit for it
        std::string val = (param_ == kReadBalanceParam)
            ? std::to_string(got) + " mV"
            : std::to_string(got) + " raw";
        short_line += " ok " + val;
        if (has_expect_ && got != expect_raw_) {
            status_ = Status::Mismatch;
            line_ = "read-back FAIL: " + ptag + " = " + val + " but the last write sent " +
                    std::to_string(expect_raw_) + " - stale or not applied;"
                    " press Read again or re-send the write";
            return short_line + " mismatch";
        }
        status_ = Status::ReadOk;
        line_ = has_expect_
            ? "read-back PASS: " + ptag + " = " + val + ", matches the last written value"
            : "read ok: " + ptag + " = " + val + " (no write recorded for pack " +
              hex2(pack_) + " - baseline read)";
        return short_line;
    }

    uint8_t  last_pack() const { return pack_; }
    uint8_t  last_param() const { return param_; }
    uint16_t value_raw() const { return value_raw_; }
    Status status() const { return status_; }
    const std::string& status_line() const { return line_; }
    uint32_t tx_count() const { return tx_count_; }

    // consumed by the core's on_request: the first master poll after a
    // fired read completes the ride-gap datum (one ring line per read)
    bool ride_gap_complete(uint32_t req_start_ms, std::string* out) {
        if (!ride_gap_pending_) return false;
        ride_gap_pending_ = false;
        if (out) {
            *out = "D547 " + hex2(pack_) + " ride gap " +
                   std::to_string(req_start_ms - ride_gap_start_ms_) + "ms";
        }
        return true;
    }

private:
    void deny(const std::string& why) {
        status_ = Status::Denied;
        line_ = "denied: " + why;
    }

    void tick_armed(uint32_t t_ms) {
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = "aborted: enable switched off";
            return;
        }
        if ((int32_t)(t_ms - armed_at_ms_) > (int32_t)kArmExpiryMs) {
            status_ = Status::Expired;
            line_ = "expired: no safe window within " + std::to_string(kArmExpiryMs / 1000) +
                    " s (watchdog, cadence, clean-slot or silence waits); press Read again";
            return;
        }
        if (core_.watchdog_tripped()) return;           // bus in unknown state: hold
        if (!core_.bus_established(t_ms)) return;       // no fresh poll cadence: hold
        if (gate_.need_clean_slot) return;               // no clean pair since our last frame, either client
        if (gate_.cooldown_active) return;               // the shared 4 s latch
        uint32_t since = core_.since_last_rx(t_ms);
        const char* aim_class = nullptr;
        if (aim_long_gap_) {
            // PRE-LONG CORRIDOR v2 (see BmsLinkCore::pre_long_quiet + the
            // kPreLongLead* comment block): fire only into the single quiet
            // predicted to close right before a long gap opens, inside the
            // re-derived band. Firing anywhere inside a long quiet is the
            // retired cut class: measured every-quiet length (~455/~500/
            // ~1080) is shorter than the ~1094 ms block.
            uint32_t quiet_open = t_ms - since;
            bool in_corridor = core_.pre_long_quiet(quiet_open, t_ms);
            bool aim_at_predicted = in_corridor &&
                                    since >= kPreLongFireFromMs && since <= kPreLongFireToMs;
            if (!aim_at_predicted) return;
            aim_class = "pre-long";
        } else if (since < kGapConfirmMs || since > kGapMaxMs) {
            return;  // mid-idle, not at the edge
        }

        std::string frame = build_read_frame(pack_);
        tx_started_ms_ = t_ms;
        // ride-gap latch (reviewer instrument, the pack-02 flip showed
        // it is the datum that decides clipped vs clean): the gap this
        // read fired into opens at fire-since; the NEXT master request
        // completes the length. Consumed once, ring-recorded beside the
        // start-delay line so verdicts line up with gap lengths directly
        ride_gap_start_ms_ = t_ms - since;
        ride_gap_pending_ = true;
        bool ok = tx_(frame, ctx_);
        gate_.last_tx_ms = t_ms;
        gate_.cooldown_active = true;
        tx_count_++;  // flush failures included as TX attempts, same rule as writes
        gate_.need_clean_slot = true;
        gate_.clean_pair_baseline = core_.pairs_total();
        if (!ok) {
            status_ = Status::Aborted;
            line_ = "aborted: uart flush failed - bytes may be on the wire; press Read again";
            return;
        }
        status_ = Status::WaitBlock;
        line_ = aim_class
            ? "sent, collecting the parameter block (1.5 s window; fired " +
              std::to_string(since) + " ms into a " + aim_class + " gap)"
            : "sent, collecting the parameter block (1.5 s window)";
    }

    void tick_wait_block(uint32_t t_ms) {
        if (block_window_active(t_ms)) return;
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = "frame was sent, but collection stopped early (enable switched off);"
                    " nothing verified - press Read again";
            return;
        }
        status_ = Status::NoBlock;
        line_ = "no parameter block in the 1.5 s window - pack silent or the collision"
                " shredded the response; press Read again for a new single read";
    }

    static std::string hex2(uint8_t v) {
        char b[3];
        snprintf(b, sizeof b, "%02X", (unsigned)v);
        return b;
    }

    BmsLinkCore& core_;
    SharedTxGate& gate_;
    EnableFn enable_;
    TxFn tx_;
    void* ctx_;

    uint8_t  pack_ = 0;
    uint8_t  param_ = 0;
    bool     has_expect_ = false;
    uint16_t expect_raw_ = 0;
    uint16_t value_raw_ = 0;
    bool     aim_long_gap_ = false;
    int      answer_start_delay_ms_ = -1;  // fire -> answer-first-byte, logged on every grade
    uint32_t ride_gap_start_ms_ = 0;       // the ridden gap opens at fire - since
    bool     ride_gap_pending_ = false;    // until the next master request completes it
    Status   status_ = Status::Idle;
    uint32_t armed_at_ms_ = 0;
    uint32_t tx_started_ms_ = 0;
    uint32_t tx_count_ = 0;
    std::string line_ = "idle";
};

// ---------------------------------------------------------------------------
// Guarded teleControl client — the 2026-11-11 vendor-tool session
// productionized (SYS_OFF restart + the LMCHG wake pair). One press = ONE
// scripted sequence aimed through the same fire gates as the other clients
// (enable re-checked at every state, watchdog/cadence/clean-slot holds,
// 150..250 ms confirmed silence). The D545 write carries no target: every
// sequence opens with a D5C9 presence probe, requires the pack's 9052
// identity answer, and only then sends the write — no answer means no
// write, and a foreign D5C9 probe observed mid-sequence aborts before any
// unsent frame (the target latch may have moved). Only the cross-op 4 s
// cooldown is waived INSIDE one press's sequence: every frame still waits
// for a fresh clean master pair after our previous frame plus
// kTeleIntraGapMs, strictly tighter than the vendor tool's human timing
// that landed these same sequences all session. Short frames (~12 ms of
// wire) fire into regular idle windows - no long-gap aim needed.
// ---------------------------------------------------------------------------
static constexpr uint32_t kTeleAnswerWindowMs = 1500;  // probe answer collect window
static constexpr uint32_t kTeleAnswerQuickMs  = 400;   // acceptance bound (reviewer
                                                     // wire-math, re-review): probe
                                                     // ~20 ms + pack turnaround
                                                     // ~200-230 ms + ~100-char
                                                     // answer ~104 ms at 9600 +
                                                     // 10 ms batch rounding puts
                                                     // a genuine answer's last
                                                     // byte at ~+330-370 ms.
                                                     // This bound is fail-closed
                                                     // seasoning only - the
                                                     // first-frame and
                                                     // no-open-request rules
                                                     // are what exclude
                                                     // telemetry, not the clock
static constexpr uint32_t kTeleOpDeadlineMs   = 15000;  // whole-sequence budget
static constexpr uint32_t kTeleIntraGapMs     = 400;    // min spacing between our frames in one press

class TeleOpController {
public:
    enum class Op { None, SysOff, WakePair };
    enum class Status {
        Idle, Denied, Armed, WaitProbe, WaitWindow, WaitAck, Settle,
        Done, NoAnswer, NoAck, Collision, Aborted, Expired
    };

    using EnableFn = bool (*)(void*);
    using TxFn = bool (*)(const std::string&, void*);

    TeleOpController(BmsLinkCore& core, SharedTxGate& gate,
                     EnableFn enable, TxFn tx, void* ctx)
        : core_(core), gate_(gate), enable_(enable), tx_(tx), ctx_(ctx) {}

    bool request_sys_off(uint8_t pack, uint32_t now) {
        if (in_flight_()) {
            line_ = "ignored: previous teleControl sequence still in flight";
            return false;
        }
        uint8_t variant = 0;
        if (pack < kLinkPackFirst || pack > kLinkPackLast ||
            !telecontrol_variant(pack, variant)) {
            deny("pack must be 01-04 (never 00 - the master battery)");
            return false;
        }
        start_(Op::SysOff, pack, variant, now);
        line_ = "armed: SYS_OFF pack " + hex2(pack) + " (variant " + hex2(variant) +
                "): probe, then the teleControl write - one sequence across the" +
                " next windows, expires in " + std::to_string(kTeleOpDeadlineMs / 1000) + " s";
        return true;
    }

    bool request_wake_pair(uint8_t pack, uint32_t now) {
        if (in_flight_()) {
            line_ = "ignored: previous teleControl sequence still in flight";
            return false;
        }
        uint8_t variant = 0;
        if (pack < kLinkPackFirst || pack > kLinkPackLast ||
            !telecontrol_variant(pack, variant)) {
            deny("pack must be 01-04 (never 00 - the master battery)");
            return false;
        }
        if (variant != 0x04) {
            deny("the wake pair exists only as the captured new-gen sequence (packs 03-04);"\
                 " for an old-gen pack the physical button is the recovery");
            return false;
        }
        start_(Op::WakePair, pack, variant, now);
        line_ = "armed: LMCHG wake pair pack " + hex2(pack) + " (variant " + hex2(variant) +
                "): probe, disable, enable - one sequence, expires in " +
                std::to_string(kTeleOpDeadlineMs / 1000) + " s";
        return true;
    }

    void tick(uint32_t t_ms) {
        if (gate_.cooldown_active &&
            (int32_t)(t_ms - gate_.last_tx_ms) >= (int32_t)kTxDueCooldownMs)
            gate_.cooldown_active = false;
        if (late_ack_until_ms_ != 0 && (int32_t)(late_ack_until_ms_ - t_ms) <= 0)
            late_ack_until_ms_ = 0;
        if (gate_.need_clean_slot && core_.pairs_total() > gate_.clean_pair_baseline)
            gate_.need_clean_slot = false;
        switch (status_) {
            case Status::Armed:      tick_armed(t_ms); break;
            case Status::WaitProbe:  tick_wait_probe(t_ms); break;
            case Status::WaitWindow: tick_wait_window(t_ms); break;
            case Status::WaitAck:    tick_wait_ack(t_ms); break;
            case Status::Settle:     tick_settle(t_ms); break;
            default: break;  // terminal: nothing fires without a fresh press
        }
    }

    // Reviewer-prescribed structural acceptance (PR #82 blocker): a 9052
    // frame counts as OUR probe answer only when it is structurally provable
    // - the FIRST frame since our probe (frames_total moved by exactly one:
    // our frames ride the UART, never the classifier), no master request
    // open (the core checks that side - a routine 0x44 telemetry answer
    // shares this LENID and pairs to its poll), and inside the short
    // turnaround bound. Anything else falls through to the normal listener
    // path: the chain stays fail-closed against a silent pack.
    bool probe_accept(uint32_t now, uint32_t frames_total_now) const {
        return status_ == Status::WaitProbe &&
               frames_total_now == probe_frames_snapshot_ + 1 &&
               (int32_t)(now - probe_tx_ms_) <= (int32_t)kTeleAnswerQuickMs;
    }

    // open-window query for the core's stray recorder, plus the probe TX
    // timestamp for its delay field - every un-consumed response during a
    // window gets identified in the ring instead of argued about
    bool probe_window_open(uint32_t now) const {
        return status_ == Status::WaitProbe &&
               (int32_t)(now - probe_tx_ms_) <= (int32_t)kTeleAnswerWindowMs;
    }
    uint32_t probe_tx_ms() const { return probe_tx_ms_; }

    bool ack_expected(uint32_t now) const {
        if (status_ == Status::WaitAck) return true;
        return wrote_any_ && late_ack_until_ms_ != 0 &&
               (status_ == Status::NoAck || status_ == Status::Collision ||
                status_ == Status::Settle) &&
               (int32_t)(late_ack_until_ms_ - now) > 0;
    }

    void on_probe_answer(uint32_t t_ms, const std::string& info) {
        if (status_ != Status::WaitProbe) return;
        std::string id = info.size() > 2 ? info.substr(2, 8) : std::string("?");
        // measured answer delay on the verdict line: the first live press
        // records the only real calibration datum for this bound (no
        // capture holds the actual probe->answer latency)
        line_ = "probe answered (id " + id + "...) in " +
                std::to_string(t_ms - probe_tx_ms_) + " ms - sending " + op_name_() +
                " in the next quiet window (short frame, regular idle fits)";
        status_ = Status::WaitWindow;
    }

    // a 9052 frame arrived while we wait but failed structural acceptance
    // (late past the bound, not the first frame, or a master request was
    // open). Latch its delay so the NoAnswer verdict can record the real
    // number - the first live press is the calibration run for the
    // acceptance bound.
    void on_probe_answer_rejected(uint32_t t_ms) {
        if (status_ != Status::WaitProbe) return;
        int32_t lat = (int32_t)(t_ms - probe_tx_ms_);
        if (lat > 0) rejected_answer_ms_ = (uint32_t)lat;
    }

    void on_ack(uint32_t t_ms) {
        int lat = (int)(t_ms - tx_started_ms_);
        if (status_ == Status::WaitAck) {
            if (op_ == Op::SysOff) {
                status_ = Status::Done;
                line_ = "SYS_OFF acked in " + std::to_string(lat) + " ms - ack-level landing;"\
                        " the pack restarts (a reboot preserves parameters - NOT the" \
                        " 6-second button factory reset). Watch the LEDs; the wake pair" \
                        " button recovers a pack that stays dark";
                end_tx_op_();
            } else if (seq_idx_ == 0) {
                status_ = Status::Settle;
                settle_from_ms_ = t_ms;
                line_ = "LMCHG disable acked in " + std::to_string(lat) +
                        " ms - settling, then the enable";
                end_tx_op_();
            } else {
                status_ = Status::Done;
                line_ = "wake pair complete: disable then enable, this enable acked in " +
                        std::to_string(lat) + " ms - expect the pack back (LEDs, telemetry)"\
                        " within the minute; verify by read";
                end_tx_op_();
            }
        } else if (ack_expected(t_ms)) {
            line_ += " | late bare ack " + std::to_string(lat) +
                     " ms after our frame: likely the answer - the frame landed"
                     " (ack-level only)";
        }
    }

    void on_foreign_probe(uint32_t t_ms) {
        (void)t_ms;
        if (!in_flight_()) return;
        // pre-write states: our write may not have flown; the latch is
        // compromised - stop. WaitAck/Done: our frame is already out.
        if (status_ != Status::WaitProbe && status_ != Status::WaitWindow &&
            status_ != Status::Settle)
            return;
        status_ = Status::Aborted;
        line_ = "aborted: another presence probe re-latched the teleControl target -"\
                " sequence stopped before any unsent frame; press again";
        append_pair_warning_();
        end_tx_op_();
    }

    Op op() const { return op_; }
    uint8_t last_pack() const { return pack_; }
    Status status() const { return status_; }
    const std::string& status_line() const { return line_; }
    uint32_t tx_count() const { return tx_count_; }

private:
    bool in_flight_() const {
        return status_ == Status::Armed || status_ == Status::WaitProbe ||
               status_ == Status::WaitWindow || status_ == Status::WaitAck ||
               status_ == Status::Settle;
    }

    void deny(const std::string& why) {
        status_ = Status::Denied;
        line_ = "denied: " + why;
    }

    void start_(Op op, uint8_t pack, uint8_t variant, uint32_t now) {
        op_ = op;
        pack_ = pack;
        variant_ = variant;
        seq_idx_ = 0;
        wrote_any_ = false;
        disable_sent_ = false;
        enable_sent_ = false;
        rejected_answer_ms_ = 0;
        armed_at_ms_ = now;
        status_ = Status::Armed;
    }

    // Reviewer should-fix: a wake pair that stops after the disable flew
    // leaves the pack potentially LMCHG-disabled, and every terminal line
    // from such a stop must say so - "press again" alone is dangerous
    // ambiguity when half of the pair is battery-side already.
    void append_pair_warning_() {
        if (op_ == Op::WakePair && disable_sent_ && !enable_sent_) {
            line_ += " - the LMCHG enable was NOT sent and this pack may be"
                     " left with LMCHG disabled; press Wake Pair again or"
                     " use the physical button";
        }
    }

    // rejected-9052 report for the NoAnswer verdict: with the delay
    // measured, so the first live press tells us whether the acceptance
    // bound needs another turn (reviewer: no capture holds the real
    // probe->answer latency - the press itself is the measurement)
    void append_rejected_answer_() {
        if (rejected_answer_ms_ != 0) {
            line_ += " | a 9052 frame was seen " + std::to_string(rejected_answer_ms_) +
                     " ms after the probe but rejected (structure: not first frame" \
                     " / a master request was open / past the bound) - the real" \
                     " answer delay datum for the acceptance bound, recorded";
        }
    }

    const char* op_name_() const {
        return op_ == Op::SysOff ? "SYS_OFF" :
               (seq_idx_ == 0 ? "LMCHG disable" : "LMCHG enable");
    }

    void require_clean_() {
        gate_.need_clean_slot = true;
        gate_.clean_pair_baseline = core_.pairs_total();
    }
    void end_tx_op_() {
        require_clean_();
        late_ack_until_ms_ = gate_.last_tx_ms + kTxDueCooldownMs;
    }

    bool common_holds_(uint32_t t_ms, const char* who) {
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = std::string("aborted: enable switched off mid-sequence - ") + who +
                    (wrote_any_ ? " (frames already sent stay sent; nothing more fires)"
                                : " (the probe was sent, the write was not)");
            append_pair_warning_();
            return false;
        }
        if ((int32_t)(t_ms - armed_at_ms_) > (int32_t)kTeleOpDeadlineMs) {
            status_ = Status::Expired;
            line_ = "expired: no safe window within " +
                    std::to_string(kTeleOpDeadlineMs / 1000) +
                    " s - nothing further fires; press again";
            append_pair_warning_();
            return false;
        }
        if (core_.watchdog_tripped()) return false;
        if (!core_.bus_established(t_ms)) return false;
        if (gate_.need_clean_slot) return false;
        return true;
    }

    void fire_frame_(uint32_t t_ms, const std::string& what) {
        std::string frame = seq_idx_ == 0
            ? (op_ == Op::SysOff
                   ? build_telecontrol_enable(variant_, kTeleGateSysOff)
                   : std::string(kLmchgDisableNewGen))
            : build_telecontrol_enable(variant_, kTeleGateLmchg);
        tx_started_ms_ = t_ms;
        bool ok = tx_(frame, ctx_);
        gate_.last_tx_ms = t_ms;
        gate_.cooldown_active = true;
        tx_count_++;
        require_clean_();
        if (!ok) {
            status_ = Status::Aborted;
            line_ = "aborted: uart flush failed sending " + what +
                    " - bytes may be on the wire; nothing further fires this press";
            append_pair_warning_();
            end_tx_op_();
            return;
        }
        wrote_any_ = true;
        if (op_ == Op::WakePair) {
            if (seq_idx_ == 0) disable_sent_ = true;
            else enable_sent_ = true;
        }
        status_ = Status::WaitAck;
        line_ = what + " sent (" + std::to_string(frame.size()) +
                " chars) - waiting for the single-frame ack (150 ms window;"\
                " late acks listened for, never a retry)";
    }

    void tick_armed(uint32_t t_ms) {
        // the probe is the operation's first frame: full cross-op gates,
        // including the shared 4 s cooldown
        if (!common_holds_(t_ms, "before the probe")) return;
        if (gate_.cooldown_active) return;
        uint32_t since = core_.since_last_rx(t_ms);
        if (since < kGapConfirmMs || since > kGapMaxMs) return;
        std::string frame = build_probe_frame(pack_);
        probe_tx_ms_ = t_ms;
        probe_frames_snapshot_ = core_.frames_total();
        bool ok = tx_(frame, ctx_);
        gate_.last_tx_ms = t_ms;
        gate_.cooldown_active = true;
        tx_count_++;
        require_clean_();
        if (!ok) {
            status_ = Status::Aborted;
            line_ = "aborted: uart flush failed sending the probe - bytes may be on" \
                    " the wire; the write WILL NOT fire without a verified probe;" \
                    " press again";
            return;
        }
        status_ = Status::WaitProbe;
        line_ = "probe sent (pack " + hex2(pack_) + ") - waiting for the identity" \
                " answer (1.5 s); the write does NOT fire without it";
    }

    void tick_wait_probe(uint32_t t_ms) {
        // the verdict path runs ungated (same rule as the ack deadline):
        // bus holds never postpone a NoAnswer classification
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = "aborted: enable switched off - the probe was sent, the write" \
                    " was not; nothing further fires";
            append_pair_warning_();
            return;
        }
        // any RX since our probe that was not accepted as the answer means
        // the answer can no longer be the first frame: verdict NOW, never
        // at the window's edge - routine telemetry arrives within seconds
        // on this bus, and the write must not fly on the far side of it
        if (core_.frames_total() != probe_frames_snapshot_) {
            status_ = Status::NoAnswer;
            line_ = "no probe answer first-off: bus traffic followed our probe" \
                    " without the pack answering - the teleControl write was" \
                    " NOT sent (fail-closed); press again";
            append_rejected_answer_();
            return;
        }
        if ((int32_t)(t_ms - probe_tx_ms_) <= (int32_t)kTeleAnswerWindowMs) return;
        status_ = Status::NoAnswer;
        line_ = "no probe answer in the 1.5 s window - the teleControl write" \
                " was NOT sent (fail-closed: no answer, no write); press again";
        append_rejected_answer_();
    }

    void tick_wait_window(uint32_t t_ms) {
        // intra-op: the shared cooldown is deliberately waived (bounded by
        // the clean-pair demand + the settle gap); everything else holds
        if (!common_holds_(t_ms, "before the write")) return;
        uint32_t since = core_.since_last_rx(t_ms);
        if (since < kGapConfirmMs || since > kGapMaxMs) return;
        // the sent line must name the frame it actually sent: live wake
        // pair 17:29 labeled the enable "disable sent" while the wire
        // bytes were the byte-exact enable
        fire_frame_(t_ms, seq_idx_ == 1 && op_ == Op::WakePair
                          ? std::string("LMCHG enable")
                          : (op_ == Op::SysOff ? std::string("SYS_OFF write")
                                               : std::string("LMCHG disable")));
    }

    void tick_wait_ack(uint32_t t_ms) {
        // the ack deadline runs ungated, like the write client: bus holds
        // (clean-slot, cadence) never postpone a NoAck verdict
        if (!enable_(ctx_)) {
            status_ = Status::Aborted;
            line_ = std::string(op_name_()) +
                    " - result unknown (enable switched off after TX); nothing" \
                    " further fires";
            end_tx_op_();
            return;
        }
        if ((int32_t)(t_ms - tx_started_ms_) < (int32_t)kAckDeadlineMs) return;
        bool traffic = (int32_t)(core_.last_rx_ms() - tx_started_ms_) > 0;
        const char* verdict = traffic ? "collision after the frame" : "no ack in the window";
        if (op_ == Op::SysOff) {
            status_ = traffic ? Status::Collision : Status::NoAck;
            line_ = std::string(verdict) + " - disarmed, result unknown (the pack may" \
                    " still have taken it; no ack never means not-written)." \
                    " Listening for a late ack until the cooldown passes; press again" \
                    " if the LEDs show nothing changed";
            end_tx_op_();
            return;
        }
        if (seq_idx_ == 0) {
            // the disable's ack is unobserved: continue to the enable - the
            // enable is the recovery direction whether or not the disable
            // landed (re-enabling an already-enabled LMCHG gate is a no-op)
            status_ = Status::Settle;
            settle_from_ms_ = t_ms;
            line_ = std::string(verdict) + " on the LMCHG disable - continuing to the" \
                    " enable anyway (the enable is the recovery direction either way);" \
                    " listening for the disable's late ack until the cooldown passes";
            end_tx_op_();
            return;
        }
        status_ = traffic ? Status::Collision : Status::NoAck;
        line_ = std::string(verdict) + " on the LMCHG enable - disarmed, result unknown." \
                " The disable side of the pair already flew and this enable is" \
                " unconfirmed, so the pack may be left with LMCHG disabled;" \
                " press Wake Pair again or use the physical button." \
                " Listening for a late ack until the cooldown passes";
        end_tx_op_();
        return;
    }

    void tick_settle(uint32_t t_ms) {
        if (!common_holds_(t_ms, "between the pair")) return;
        if ((int32_t)(t_ms - settle_from_ms_) < (int32_t)kTeleIntraGapMs) return;
        seq_idx_ = 1;
        status_ = Status::WaitWindow;
        line_ = "settle passed - the LMCHG enable fires in the next quiet window";
    }

    static std::string hex2(uint8_t v) {
        char b[3];
        snprintf(b, sizeof b, "%02X", (unsigned)v);
        return b;
    }

    BmsLinkCore& core_;
    SharedTxGate& gate_;
    EnableFn enable_;
    TxFn tx_;
    void* ctx_;

    Op op_ = Op::None;
    uint8_t pack_ = 0;
    uint8_t variant_ = 0;
    int seq_idx_ = 0;              // 0 = the first write frame, 1 = the enable
    bool wrote_any_ = false;
    bool disable_sent_ = false;    // the wake pair's disable left our UART
    bool enable_sent_ = false;     // the wake pair's enable left our UART
    Status status_ = Status::Idle;
    uint32_t armed_at_ms_ = 0;
    uint32_t probe_tx_ms_ = 0;
    uint32_t probe_frames_snapshot_ = 0;  // core frames_total at probe TX
    uint32_t rejected_answer_ms_ = 0;     // last 9052 seen in-window but rejected
    uint32_t tx_started_ms_ = 0;
    uint32_t settle_from_ms_ = 0;
    uint32_t late_ack_until_ms_ = 0;
    uint32_t tx_count_ = 0;
    std::string line_ = "idle";
};

// ---------------------------------------------------------------------------
// Enable auto-off decision (review-vote fix): a finished write stays Acked
// until the next write, so "any client terminal" fired a PHANTOM auto-off on
// the second operation - a read armed right after a write verdict was
// killed on its next tick by the stale Acked. The gate turns off only
// when the client that WAS in flight on the previous tick is the one that
// just reached a terminal state.
inline bool write_status_in_flight(WriteController::Status s) {
    return s == WriteController::Status::Armed || s == WriteController::Status::WaitAck;
}
inline bool read_status_in_flight(ReadController::Status s) {
    return s == ReadController::Status::Armed || s == ReadController::Status::WaitBlock;
}
inline bool teleop_status_in_flight(TeleOpController::Status s) {
    using T = TeleOpController::Status;
    return s == T::Armed || s == T::WaitProbe || s == T::WaitWindow ||
           s == T::WaitAck || s == T::Settle;
}
inline bool write_status_terminal(WriteController::Status s) {
    using S = WriteController::Status;
    return s == S::Acked || s == S::NoAck || s == S::Collision ||
           s == S::Aborted || s == S::Expired || s == S::Denied;
}
inline bool read_status_terminal(ReadController::Status s) {
    using R = ReadController::Status;
    return s == R::ReadOk || s == R::Mismatch || s == R::WrongPack ||
           s == R::Damaged || s == R::NoBlock || s == R::Aborted ||
           s == R::Expired || s == R::Denied;
}
inline bool teleop_status_terminal(TeleOpController::Status s) {
    using T = TeleOpController::Status;
    return s == T::Done || s == T::NoAnswer || s == T::NoAck ||
           s == T::Collision || s == T::Aborted || s == T::Expired ||
           s == T::Denied;
}
inline bool op_just_finished(WriteController::Status prev_w, ReadController::Status prev_r,
                              TeleOpController::Status prev_t,
                              WriteController::Status cur_w, ReadController::Status cur_r,
                              TeleOpController::Status cur_t) {
    return (write_status_in_flight(prev_w) && write_status_terminal(cur_w)) ||
           (read_status_in_flight(prev_r) && read_status_terminal(cur_r)) ||
           (teleop_status_in_flight(prev_t) && teleop_status_terminal(cur_t));
}

inline bool BmsLinkCore::on_read_candidate_(uint32_t t_ms, uint32_t answer_start_ms,
                                            const std::string& raw, int len) {
    if (!read_waiter_->block_window_active(t_ms)) return false;
    if (raw.substr(6, 2) != "00") return false;  // only a RTN-00 block is our answer
    std::string short_line = read_waiter_->on_block(raw, len, answer_start_ms);
    push_event(EventKind::BlockForRead, t_ms, read_waiter_->last_pack(), short_line);
    ring_now(t_ms, short_line);  // ring budget truncates the 562-char frame; the verdict stays
    return true;
}

// Structural acceptance lives with the controller's probe_accept(); this
// side supplies the listener-state facts it cannot see. Returns false for
// anything that must fall through to normal pairing - the frame then
// pairs/responds like any other bus frame, listener unharmed.
inline bool BmsLinkCore::consume_probe_answer_(uint32_t t_ms, const std::string& info) {
    if (!telecom_waiter_) return false;
    if (open_req_.valid) {
        telecom_waiter_->on_probe_answer_rejected(t_ms);
        return false;
    }
    if (!telecom_waiter_->probe_accept(t_ms, frames_total_)) {
        telecom_waiter_->on_probe_answer_rejected(t_ms);
        return false;
    }
    if (!probe_identity_content(info)) {
        // shape + structure passed but the content is a battery number,
        // not an identity: a telemetry answer in disguise (reviewer route
        // two - our probe garbles the master's request, its polled pack
        // answers within ~15 ms, nothing is open, the frame count moved
        // only by the pair) - reject, the write never fires on it
        telecom_waiter_->on_probe_answer_rejected(t_ms);
        return false;
    }
    last_frame_end_ms_ = frame_end_t_ms_;
    telecom_waiter_->on_probe_answer(t_ms, info);
    std::string id = info.size() > 2 ? info.substr(2, 8) : std::string("?");
    push_event(EventKind::ProbeAnswer, t_ms, telecom_waiter_->last_pack(),
               "9052 identity answer id " + id);
    ring_now(t_ms, "9052 id " + id);
    return true;
}

// Identified, not argued about (the 17:18 lesson): every response during
// an open probe window that the acceptance did not consume lands in the
// ring with its LENID, its delay and its first bytes - the next surprise
// gets identified instead of inferred about.
inline void BmsLinkCore::record_probe_stray_(uint32_t t_ms, const std::string& info,
                                             const std::string& lenid) {
    if (!telecom_waiter_ || !telecom_waiter_->probe_window_open(t_ms)) return;
    ring_now(t_ms, "probe stray " + lenid + " +" +
                       std::to_string(t_ms - telecom_waiter_->probe_tx_ms()) + "ms " +
                       info.substr(0, 24));
}

// The first master request after a fired read completes the ride-gap
// datum: one ring line per read, next to the start-delay line, so
// clipped-vs-clean verdicts line up with gap lengths directly.
inline void BmsLinkCore::record_ride_gap_(uint32_t t_ms) {
    if (!read_waiter_) return;
    std::string g;
    if (read_waiter_->ride_gap_complete(t_ms, &g)) ring_now(t_ms, g);
}

inline void BmsLinkCore::notify_telecom_probe_(uint32_t t_ms) {
    if (telecom_waiter_) telecom_waiter_->on_foreign_probe(t_ms);
}

inline bool BmsLinkCore::route_teleop_ack_(uint32_t t_ms) {
    if (!telecom_waiter_ || !telecom_waiter_->ack_expected(t_ms)) return false;
    push_event(EventKind::AckForTeleOp, t_ms, 0, "bare ack for teleControl frame");
    telecom_waiter_->on_ack(t_ms);
    return true;
}

}  // namespace linksniff
