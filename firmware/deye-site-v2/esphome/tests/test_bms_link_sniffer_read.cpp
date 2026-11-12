// Host oracle for the bms-link-sniffer guarded block-read client: the
// on-device replacement for docs/analysis/bms073-tools/rs485_verify_
// write.py (the USB-dongle PoC, retired). Pins, against committed wire
// evidence:
//   - the builder: byte-exact against bms073.build(cid2=0x47,
//     info="01") -> ~8000D547E00201FD1C
//   - the trust rule: a read only ever grades ReadOk from a
//     whole-frame-checksum-VALID block whose battery number equals the
//     requested pack
//   - the fail-closed divergence from the Mac tool: the reference-based
//     collision reconstruction is NOT ported - DAMAGED_FRAME (the real
//     2026-10-31 collision, 0x0D66 chars intact at INFO 308 but checksum
//     invalid) must grade Damaged, not verified, however readable the
//     value region looks
//   - read artifacts never poison the listener: a consumed block (clean
//     or damaged) adds no checksum error, anomaly, or slot pairing, and
//     master telemetry flows uninterrupted through the collection window
//   - the fire gates: same 150..250 ms window, 4 s read cooldown, the
//     write client's 4 s latch shared, clean-pair-since-our-TX, packs
//     01-04 only
//   - one frame per press; a second press during WaitBlock is ignored
//
// Build & run (CLAUDE.md host-test loop):
//   cd firmware/deye-site-v2/esphome
//   clang++ -std=gnu++20 -Wall -Wextra -I. -Itests/stubs -Iincludes \
//     tests/test_bms_link_sniffer_read.cpp -o /tmp/test_bms_link_sniffer_read \
//     && /tmp/test_bms_link_sniffer_read

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "bms_link_sniffer_core.h"
#include "fixtures/bms_link_fixtures.h"

using linksniff::BmsLinkCore;
using linksniff::EventKind;
using linksniff::ReadController;
using linksniff::WriteController;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void check_str(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        failures++;
        printf("FAIL: %s got='%s' want='%s'\n", what, got.c_str(), want.c_str());
    }
}

static void check_line_has(const std::string& line, const char* needle, const char* what) {
    if (line.find(needle) == std::string::npos) {
        failures++;
        printf("FAIL: %s - line lacks '%s': %s\n", what, needle, line.c_str());
    }
}

static bool has_event(const std::vector<linksniff::LinkEvent>& ev, EventKind k) {
    for (const auto& e : ev)
        if (e.kind == k) return true;
    return false;
}

// ---- driver plumbing (same shapes as the write oracle) ---------------------

struct FakeBus {
    std::vector<std::string> tx_log;
    bool enable = true;
    bool tx_ok = true;

    static bool enable_cb(void* ctx) { return ((FakeBus*)ctx)->enable; }
    static bool tx_cb(const std::string& frame, void* ctx) {
        FakeBus* self = (FakeBus*)ctx;
        if (!self->tx_ok) return false;
        self->tx_log.push_back(frame);
        return true;
    }
};

// one core + one fake bus + both tx clients wired the way the wrapper
// wires them (the write's cooldown latch shared into the read controller)
struct Harness {
    BmsLinkCore c;
    FakeBus bus;
    linksniff::SharedTxGate gate;
    WriteController w;
    ReadController r;

    Harness()
        : w(c, gate, FakeBus::enable_cb, FakeBus::tx_cb, &bus),
          r(c, gate, FakeBus::enable_cb, FakeBus::tx_cb, &bus) {
        c.set_ack_waiter(&w);
        c.set_read_waiter(&r);
    }
};

static void send(BmsLinkCore& c, const std::string& frame, uint32_t t_ms) {
    c.feed(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), t_ms);
}

static std::string poll_req(int pack, const char* cid2) {
    char body[16];
    std::string info = (pack < 10 ? "0" + std::to_string(pack) : std::to_string(pack));
    snprintf(body, sizeof body, "8050D5%sE002", cid2);
    std::string b = body + info;
    return "~" + b + rs485_calc_chksum(b) + "\r";
}

// one full master slot pair: request at t, response at t+15
static void pair_at(Harness& h, int pack, uint32_t t) {
    send(h.c, poll_req(pack, "42"), t);
    send(h.c, std::string(REAL_42_RESPONSE) + "\r", t + 15);
}

// Clean A222 block interior with a chosen battery number and a patched
// entry value, derived from the captured standalone frame and
// re-checksummed with the shared firmware function - labeled synthetic
// in every scenario that uses it, for shapes the real captures do not
// already cover.
static std::string clean_block_inner(uint8_t battery, uint8_t param, const char* value_hex4) {
    std::string inner(REF_BLOCK_STANDALONE_INNER);
    const size_t info0 = 12;                  // after VER/ADR/CID1/CID2/LENID
    char b[3];
    snprintf(b, sizeof b, "%02X", (unsigned)battery);
    inner[info0 + 0] = b[0];
    inner[info0 + 1] = b[1];
    const size_t p4c = info0 + 4 * param + 4;  // entry: INFO char 4*idx+4
    for (int i = 0; i < 4; i++) inner[p4c + i] = value_hex4[i];
    std::string body = inner.substr(0, inner.size() - 4);
    return body + rs485_calc_chksum(body);
}

static void send_block(BmsLinkCore& c, const std::string& inner, uint32_t t_ms) {
    send(c, "~" + inner + "\r", t_ms);
}

// arm after a complete pair at pair_t, fire on that pair's idle window
// (request at +30, fire tick at +170 = 155 ms of silence)
static void fire_read(Harness& h, uint8_t pack, uint8_t param, bool has_expect,
                      uint16_t expect, uint32_t pair_t) {
    check(h.r.request(pack, param, has_expect, expect, pair_t + 30), "read arm accepted");
    h.r.tick(pair_t + 170);
    check(h.r.status() == ReadController::Status::WaitBlock,
          "read fired into the idle window, collecting");
}

// ------------------------------------------------------------- scenarios ---

static void test_builder_byte_exact() {
    check_str(linksniff::build_read_frame(0x01),
              std::string("~8000D547E00201FD1C\r"),
              "builder reproduces the Python tool's pack-01 read frame");
    check_str(linksniff::build_read_frame(0x04).substr(0, 15),
              std::string("~8000D547E00204"),
              "pack byte rides the shared LENID-E002 shape");
}

static void test_deny_shapes() {
    Harness h;
    h.r.request(0x00, 0x4C, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "pack 00 (self-address) denied");
    check_line_has(h.r.status_line(), "01-04", "deny reason names the observed addresses");
    h.r.request(0x05, 0x4C, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "pack 05 denied");
    h.r.request(0xFF, 0x4C, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "pack FF (broadcast) denied");
    check(h.bus.tx_log.empty(), "nothing transmitted on deny");
}

static void test_baseline_read_ok() {
    Harness h;
    uint32_t anomalies_before = h.c.anomalies_total();
    pair_at(h, 1, 1000);
    fire_read(h, 1, 0x4C, false, 0, 1000);
    uint32_t pairs_before = h.c.pairs_total();
    check(h.bus.tx_log.size() == 1, "one read frame per press");
    check_str(h.bus.tx_log[0], std::string("~8000D547E00201FD1C") + "\r",
              "transmitted frame is the pinned read");

    // master's next poll lands inside our window (the bus keeps running)
    pair_at(h, 2, 1300);

    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D5C"), 1400);
    h.r.tick(1420);
    check(h.r.status() == ReadController::Status::ReadOk, "clean block grades ReadOk");
    check(h.r.value_raw() == 0x0D5C, "0x4C extracted: 3420 mV");
    check_line_has(h.r.status_line(), "baseline read", "no recorded write -> baseline wording");
    check_line_has(h.r.status_line(), "3420 mV", "line carries the value");

    auto ev = h.c.drain_events();
    check(has_event(ev, EventKind::BlockForRead), "BlockForRead event visible");
    for (const auto& e : ev)
        if (e.kind == EventKind::BlockForRead)
            check(e.pack == 1, "BlockForRead attributed to the read's pack");
    check(h.c.anomalies_total() == anomalies_before, "read artifact added no anomaly");
    check(h.c.checksum_errors() == 0, "read artifact added no checksum error");
    // the mid-window telemetry pair counts normally; the consumed block does not
    check(h.c.pairs_total() == pairs_before + 1,
          "telemetry pairs kept counting; the consumed block added none");
}

static void test_write_verify_pass_then_fail() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, true, 0x0D66, t);
    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D66"), t + 300);
    h.r.tick(t + 320);
    check(h.r.status() == ReadController::Status::ReadOk, "matching value = ReadOk (verify PASS)");
    check_line_has(h.r.status_line(), "read-back PASS", "verify wording");
    check_line_has(h.r.status_line(), "matches the last written value", "PASS states the match");
    check(h.r.value_raw() == 0x0D66, "verified value is the written 3430 mV");

    // fresh pair ~5 s later: read cooldown (4 s) and clean-pair gate pass
    uint32_t t2 = t + 5000;
    pair_at(h, 1, t2);
    fire_read(h, 1, 0x4C, true, 0x0D66, t2);
    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D5C"), t2 + 300);
    h.r.tick(t2 + 320);
    check(h.r.status() == ReadController::Status::Mismatch, "stale value grades Mismatch");
    check_line_has(h.r.status_line(), "read-back FAIL", "verify-fail wording");
    check_line_has(h.r.status_line(), "stale or not applied", "FAIL names the two causes");
}

static void test_wrong_pack() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, false, 0, t);
    send_block(h.c, clean_block_inner(0x03, 0x4C, "0D5C"), t + 300);
    h.r.tick(t + 320);
    check(h.r.status() == ReadController::Status::WrongPack, "battery mismatch grades WrongPack");
    check_line_has(h.r.status_line(), "answered by pack 03", "the line names the answering pack");
    check_line_has(h.r.status_line(), "not the requested 01", "and the requested one");
}

static void test_real_collision_frame_fails_closed() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, true, 0x0D66, t);
    // the REAL damaged remnant: 0D66 intact at INFO char 308, checksum invalid
    send(h.c, std::string(DAMAGED_FRAME), t + 300);
    h.r.tick(t + 320);
    check(h.r.status() == ReadController::Status::Damaged, "real collision grades Damaged");
    check_line_has(h.r.status_line(), "checksum invalid", "verdict names the broken checksum");
    check_line_has(h.r.status_line(), "reconstruction proof is not ported",
                   "the divergence from the Mac tool is stated in-band");
    check(h.r.value_raw() == 0, "no value from a damaged frame");
    check(h.c.checksum_errors() == 0, "damaged read artifact added no checksum error");
    check(h.c.anomalies_total() == 0, "damaged read artifact added no anomaly");
}

static void test_no_block_window() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, false, 0, t);
    h.r.tick(t + 1670);   // window edge: exactly 1500 ms
    check(h.r.status() == ReadController::Status::WaitBlock, "window edge still collecting");
    h.r.tick(t + 1730);  // past the window
    check(h.r.status() == ReadController::Status::NoBlock, "silent window grades NoBlock");
    check_line_has(h.r.status_line(), "1.5 s window", "NoBlock names the window");
    check_line_has(h.r.status_line(), "press Read again", "retry is a deliberate new press");
}

static void test_telemetry_through_window_plus_consume() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 2, t);
    fire_read(h, 2, 0x4C, false, 0, t);
    uint32_t pairs_before = h.c.pairs_total();
    // master telemetry inside the collection window: NOT consumed (126 chars)
    pair_at(h, 3, t + 300);
    pair_at(h, 2, t + 900);
    check(h.r.status() == ReadController::Status::WaitBlock, "no false verdict from telemetry");
    check(h.c.pairs_total() == pairs_before + 2, "both mid-window pairs paired normally");
    // our pack-02 block still consumed on arrival
    send_block(h.c, clean_block_inner(0x02, 0x4C, "0D5C"), t + 1000);
    h.r.tick(t + 1020);
    check(h.r.status() == ReadController::Status::ReadOk, "telemetry did not steal the block");
    check(h.r.value_raw() == 0x0D5C, "pack-02 value extracted");
    check(h.c.pairs_total() == pairs_before + 2, "consumed block added no pairing");
}

static void test_block_outside_window_not_consumed() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    // no read armed: an A222-sized RTN-00 frame is ordinary (if unusual)
    // bus traffic and must take the normal classification path
    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D5C"), t + 300);
    auto ev = h.c.drain_events();
    check(has_event(ev, EventKind::AnomalyUnpaired),
          "unarmed: block classed unpaired response, not consumed");
    check(!has_event(ev, EventKind::BlockForRead), "no BlockForRead without a pending read");
    check(h.r.status() == ReadController::Status::Idle, "read client untouched");
}

static void test_second_press_ignored() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, false, 0, t);
    check(!h.r.request(1, 0x4C, false, 0, t + 400), "press during WaitBlock is rejected");
    check_line_has(h.r.status_line(), "ignored: previous read still in flight", "ignore wording");
    check(h.bus.tx_log.size() == 1, "still exactly one frame");
}

static void test_write_cooldown_shared() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    check(h.w.request(0x01, 0x4C, 3430, t + 30), "write arm accepted");
    h.w.tick(t + 170);
    check(h.w.status() == WriteController::Status::WaitAck, "write fired");
    send(h.c, std::string(REAL_ACK) + "\r", t + 200);
    h.w.tick(t + 210);
    check(h.w.status() == WriteController::Status::Acked, "write acked");

    // read armed right after; slots keep the bus alive while the write's
    // 4 s TX latch runs. r.tick must hold, and the latch must clear
    // through w.tick so the read fires only after the shared cooldown.
    h.r.request(0x01, 0x4C, true, 3430, t + 700);
    bool fired = false;
    uint32_t fired_at = 0;
    uint32_t st = t + 624;
    for (int i = 0; i < 10 && !fired; i++, st += 624) {
        pair_at(h, 1, st);
        h.w.tick(st + 170);
        h.r.tick(st + 170);
        if (h.r.status() == ReadController::Status::WaitBlock) {
            fired = true;
            fired_at = st + 170;
        } else {
            check(h.r.status() == ReadController::Status::Armed, "holding, not expired/aborted");
        }
    }
    check(fired, "read fires once the shared cooldown clears");
    check(fired_at - (t + 170) >= 4000, "it waited out the full 4 s write latch");
    check(h.bus.tx_log.size() == 2, "write frame + read frame, nothing between");
    check_str(h.bus.tx_log[1], std::string("~8000D547E00201FD1C") + "\r",
              "the second frame is the read");
}

static void test_expiry_no_cadence() {
    Harness h;
    h.r.request(0x01, 0x4C, false, 0, 1000);
    for (uint32_t t = 1000; t < 7200; t += 100)
        h.r.tick(t);
    check(h.r.status() == ReadController::Status::Expired, "no safe window -> expired");
    check_line_has(h.r.status_line(), "no safe window", "expiry wording names the cause");
    check(h.bus.tx_log.empty(), "nothing ever fired without cadence");
}

static void test_generic_param_read_and_band_denies() {
    Harness h;
    h.r.request(0x01, 0x06, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "0x06 is a 4-byte entry - denied");
    check_line_has(h.r.status_line(), "0x07-0x7C", "deny names the readable band");
    h.r.request(0x01, 0x7D, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "0x7D bit-param - denied");
    h.r.request(0x01, 0x00, false, 0, 1000);
    check(h.r.status() == ReadController::Status::Denied, "0x00 - below the band, denied");
    check(h.bus.tx_log.empty(), "nothing transmitted on band denies");

    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x48, false, 0, t);  // Sleep Delay Time (minutes)
    send_block(h.c, clean_block_inner(0x01, 0x48, "05A0"), t + 300);
    h.r.tick(t + 320);
    check(h.r.status() == ReadController::Status::ReadOk, "generic 0x48 read grades ReadOk");
    check(h.r.value_raw() == 0x05A0, "raw 1440 extracted (the documented 24 h sleep delay)");
    check_line_has(h.r.status_line(), "1440 raw", "non-balance params publish raw");
    check_line_has(h.r.status_line(), "0x48", "the verdict names the param");

    // 0x4C through the same generic path keeps the balance wording
    uint32_t t2 = t + 5000;
    pair_at(h, 1, t2);
    fire_read(h, 1, 0x4C, false, 0, t2);
    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D5C"), t2 + 300);
    h.r.tick(t2 + 320);
    check(h.r.status() == ReadController::Status::ReadOk, "0x4C through the same path");
    check_line_has(h.r.status_line(), "3420 mV", "balance param keeps mV wording");
}

// The review blocker, made permanent: the enable auto-off decision is
// linksniff::op_just_finished over the wrapper's prev/cur status pair.
// Drive BOTH real controllers through the exact sequence that used to die
// (write verdict -> re-enable -> read) and its mirror, asserting the
// wrapper's own decision at every tick.
static void test_second_operation_not_killed() {
    Harness h;
    linksniff::WriteController::Status prev_w = h.w.status();
    linksniff::ReadController::Status prev_r = h.r.status();
    auto tick_both = [&](uint32_t t) {
        h.w.tick(t);
        h.r.tick(t);
        bool off = linksniff::op_just_finished(prev_w, prev_r,
                                              linksniff::TeleOpController::Status::Idle,
                                              h.w.status(), h.r.status(),
                                              linksniff::TeleOpController::Status::Idle);
        prev_w = h.w.status();
        prev_r = h.r.status();
        return off;  // true = the wrapper turns the enable off here
    };

    uint32_t t = 1000;
    pair_at(h, 1, t);
    check(h.w.request(0x01, 0x4C, 3430, t + 30), "write armed");
    check(!tick_both(t + 170), "in-flight write is not itself an auto-off");
    check(h.w.status() == WriteController::Status::WaitAck, "write fired");
    send(h.c, std::string(REAL_ACK) + "\r", t + 200);
    check(tick_both(t + 210), "write completion auto-offs the enable (correct)");
    check(h.w.status() == WriteController::Status::Acked, "write acked");

    // operator re-enables and presses Read - the old "any terminal" rule
    // killed the read on its first tick via the stale Acked
    uint32_t t2 = t + 8000;
    pair_at(h, 1, t2);
    check(h.r.request(0x01, 0x4C, true, 3430, t2 + 30), "read armed after the write verdict");
    check(!tick_both(t2 + 31), "stale Acked must NOT auto-off the in-flight read");
    check(h.r.status() == ReadController::Status::Armed, "read survives the phantom tick");
    check(!tick_both(t2 + 170), "no phantom auto-off while the read fires");
    check(h.r.status() == ReadController::Status::WaitBlock, "read fired - not aborted");
    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D66"), t2 + 300);
    check(tick_both(t2 + 320), "read verdict auto-offs the enable (correct)");
    check(h.r.status() == ReadController::Status::ReadOk, "read-back PASS after a write - the step-4 sequence");

    // mirror: a write armed right after the read verdict survives too
    uint32_t t3 = t2 + 8000;
    pair_at(h, 1, t3);
    check(h.w.request(0x01, 0x4C, 3430, t3 + 30), "write armed after the read verdict");
    check(!tick_both(t3 + 170), "second operation is a write: no phantom auto-off");
    check(h.w.status() == WriteController::Status::WaitAck, "write fired - not aborted");
    send(h.c, std::string(REAL_ACK) + "\r", t3 + 300);
    check(tick_both(t3 + 310), "second write's verdict auto-offs the enable (correct)");
    check(h.w.status() == WriteController::Status::Acked, "second write acked");

    // the read-back graded against the recorded write value end to end
    check(h.r.value_raw() == 0x0D66, "the read verified the written value");
}

// Review should-fix: the 4 s cooldown is ONE shared gate. A write armed
// within 4 s of a read's TX must hold until the gate clears (the old code
// consulted the write's own latch only).
static void test_cooldown_bidirectional() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_read(h, 1, 0x4C, false, 0, t);  // the read's TX lands at t+170
    check(h.bus.tx_log.size() == 1, "read frame out");

    check(h.w.request(0x01, 0x4C, 3430, t + 700), "write armed during the read's cooldown");
    bool fired = false;
    uint32_t fired_at = 0;
    uint32_t st = t + 624;
    for (int i = 0; i < 10 && !fired; i++, st += 624) {
        pair_at(h, 1, st);
        h.w.tick(st + 170);
        h.r.tick(st + 170);
        if (h.w.status() == WriteController::Status::WaitAck) {
            fired = true;
            fired_at = st + 170;
        } else {
            check(h.w.status() == WriteController::Status::Armed, "write holds, not expired/aborted");
        }
    }
    check(fired, "write fires once the SHARED cooldown clears");
    check(fired_at - (t + 170) >= 4000, "it waited out the read's full 4 s latch");
    check(h.bus.tx_log.size() == 2, "read frame + write frame, nothing between");
    check_str(h.bus.tx_log[1], std::string(REAL_WRITE_INSTACK) + "\r",
              "the second frame is the write");
}

static void test_enable_gates() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    h.bus.enable = false;
    h.r.request(0x01, 0x4C, false, 0, t + 30);
    h.r.tick(t + 170);
    check(h.r.status() == ReadController::Status::Aborted, "enable off aborts the armed read");
    check_line_has(h.r.status_line(), "enable switched off", "abort names the gate");
    check(h.bus.tx_log.empty(), "nothing fired with the gate closed");

    // enable restored for a fresh press, then dropped mid-collection
    pair_at(h, 1, t + 624);
    h.bus.enable = true;
    h.r.request(0x01, 0x4C, false, 0, t + 654);
    h.r.tick(t + 794);
    check(h.r.status() == ReadController::Status::WaitBlock, "fired with gate open");
    h.bus.enable = false;
    // the gate is only consulted when the window ENDS - keep the bus
    // alive past the 1.5 s window so the gate, not cadence, ends it
    pair_at(h, 1, t + 1248);
    pair_at(h, 1, t + 1872);
    h.r.tick(t + 2400);
    check(h.r.status() == ReadController::Status::Aborted,
          "gate-off mid-window aborts collection once the window ends");
    check_line_has(h.r.status_line(), "collection stopped early", "abort names the mid-window stop");
    check_line_has(h.r.status_line(), "nothing verified", "and the fail-closed wording");
}

// ---- pre-long corridor aim v2 (2026-11-12 reviewer-ruling round: fire
// band re-derived from the worst-case closing answers -> 250-300 ms, 5+
// ticks wide so jittered feed ticks always land inside; stale predictions
// unlock the aim; falls back never - unlocked or unlocked-stale reads
// expire and re-press) ----

static void test_aim_fires_in_pre_long_quiet() {
    Harness h;
    // lock the predictor: quiet [1015, 3080] is a long (gap 2065 >= 700)
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 3080);          // ends 3095; long open recorded 1015
    check(h.r.request(0x01, 0x4C, false, 0, 3100, true), "aimed read armed");
    check_line_has(h.r.status_line(), "long-gap-aligned", "armed line marks the aim");
    // P1 = 1015 + 2511 = 3526; the quiet opening at 3095 leads by 431 - a
    // tick at the v1 fire band (since 180) must hold: no corridor, no fire
    h.r.tick(3275);               // since 180: v1-band-passing, lead 431
    check(h.r.status() == ReadController::Status::Armed,
          "v1-band tick in a non-corridor quiet holds");
    // ordinary pairs while P1 goes stale (slides toward the trial corridor)
    pair_at(h, 3, 3564);          // gap 469
    pair_at(h, 4, 4048);          // gap 469
    pair_at(h, 5, 4532);          // gap 469
    pair_at(h, 6, 5016);          // gap 469
    h.r.tick(5230);               // since 199: below the v2 band
    check(h.r.status() == ReadController::Status::Armed,
          "below-band tick in a non-corridor quiet holds");
    // the corridor quiet: opens 5355 (pair 7 ends 5340+15), lead
    // 6037 - 5355 = 682 in [630, 690]; P slid to 6037 by now
    pair_at(h, 7, 5340);          // gap 309, ends 5355
    h.r.tick(5435);               // since 80: far below the band
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds far below the band");
    h.r.tick(5535);               // since 180: the v1 band - must NOT fire now
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds at the retired v1 band (180 ms)");
    h.r.tick(5605);               // since 250: the re-derived band, lead 682
    check(h.r.status() == ReadController::Status::WaitBlock,
          "aimed read fires in the corridor at the re-derived band");
    check_line_has(h.r.status_line(), "pre-long", "fire line names the window class");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");
}

static void test_aim_jittered_ticks_find_the_band() {
    Harness h;
    // v1's 175-180 band was 5 ms wide - two 10 ms ticks at 171/181 could
    // straddle it forever (the oracle hand-picked since=180 and could not
    // see this). The 250-300 band is 5+ ticks wide: ticks at ODD phases
    // must land inside it.
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 3080);          // long open 1015, ends 3095
    pair_at(h, 3, 3564);          // gap 469
    pair_at(h, 4, 4048);          // gap 469
    pair_at(h, 5, 4532);          // gap 469
    pair_at(h, 6, 5016);          // gap 469
    pair_at(h, 7, 5340);          // gap 309, ends 5355: corridor, lead 682
    check(h.r.request(0x01, 0x4C, false, 0, 5400, true), "aimed read armed (jitter case)");
    h.r.tick(5602);               // since 247: jittered, just below the band
    check(h.r.status() == ReadController::Status::Armed,
          "jittered tick just below the band holds (247 ms)");
    h.r.tick(5613);               // since 258: jittered, lands inside [250, 300]
    check(h.r.status() == ReadController::Status::WaitBlock,
          "jittered tick inside the 50 ms band fires (258 ms)");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");
}

static void test_aim_stale_prediction_unlocks_until_relock() {
    Harness h;
    // the ruling's bounded-catch-up case: with no long observed for
    // >10 s the predictor refuses to fire even in a live corridor lead,
    // and a fresh long re-locks it
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 3080);          // long open 1015, ends 3095
    // ordinary 490 ms cadence until the trial corridor (P slides to 13570)
    for (uint32_t t = 3585; t <= 12405; t += 490) pair_at(h, 3, t);
    pair_at(h, 4, 12880);         // gap 460, ends 12895: corridor by lead
                                  // (13570 - 12895 = 675) - would fire fresh
    check(h.r.request(0x01, 0x4C, false, 0, 12700, true), "aimed read armed (stale case)");
    h.r.tick(13145);              // since 250: in band, lead 675 - but the
                                  // last long is 12.1 s old: STALE -> refuse
    check(h.r.status() == ReadController::Status::Armed,
          "in-band corridor tick refuses on a stale prediction (>10 s)");
    pair_at(h, 5, 14000);         // gap 1105 -> RE-LOCK (long open 12895)
    pair_at(h, 6, 14705);         // gap 690, ends 14720: corridor by the
                                  // fresh lock (15406 - 14720 = 686)
    h.r.tick(14970);              // since 250: fresh lock, in band
    check(h.r.status() == ReadController::Status::WaitBlock,
          "a fresh long re-locks the predictor and the corridor fires");
    check_line_has(h.r.status_line(), "pre-long", "fire line names the window class");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired, and only after the re-lock");
}

static void test_aim_expires_without_long_observed() {
    Harness h;
    // all-regular cadence (no gap ever reaches 700): the predictor never
    // locks, no quiet is ever a corridor, and the press expires - the
    // no-starvation fallback is RETIRED (a fire outside the corridor is
    // the wire-proven cut class; expiry + re-press is the safe path)
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 1536);          // gap 521, ends 1551
    pair_at(h, 3, 2072);          // gap 521, ends 2087
    check(h.r.request(0x02, 0x4C, false, 0, 2200, true), "aimed read armed (no-long cadence)");
    h.r.tick(2267);               // since 180: below the band - holds
    check(h.r.status() == ReadController::Status::Armed,
          "below-band tick holds on an unlocked predictor");
    h.r.tick(2337);               // since 250: in-band but never locked
    check(h.r.status() == ReadController::Status::Armed,
          "in-band tick holds when no long was ever observed");
    h.r.tick(8600);               // arm watchdog (strictly past the 6 s window)
    check(h.r.status() == ReadController::Status::Expired,
          "aimed read expires without an observed long");
    check_line_has(h.r.status_line(), "no safe window within 6 s",
                   "expiry names the wait");
    check(h.bus.tx_log.empty(), "nothing ever fired without a corridor");
}

// The reviewer's ride-gap instrument (upgraded to the deciding datum by
// the live 20:33/20:35 pack-02 flip: clean and clipped at identical
// load, position and start delay, adjacent slots): the first master
// request after a fired read completes the ridden gap's length - one
// ring line per read, next to the start-delay record - so verdicts line
// up with gap lengths directly. Latch consumed once per read.
static void test_ride_gap_recorded_per_read() {
    Harness h;
    pair_at(h, 1, 1000);          // request 1000, response ends 1015
    fire_read(h, 1, 0x4C, false, 0, 1000);  // fires at 1170: ride gap opens at 1015
    check(h.r.status() == ReadController::Status::WaitBlock, "read fired");

    send_block(h.c, clean_block_inner(0x01, 0x4C, "0D5C"), 1560);
    h.r.tick(1580);
    check(h.r.status() == ReadController::Status::ReadOk, "clean block grades ReadOk");

    // the master's next request completes the datum: 2195 - 1015 = 1180
    pair_at(h, 2, 2195);
    std::string dump = h.c.dump_bounded();
    check(dump.find("D547 01 ride gap 1180ms") != std::string::npos,
          "ride gap length recorded beside the start-delay line");

    // consumed once: the next pair must not add a second line
    pair_at(h, 3, 2819);
    dump = h.c.dump_bounded();
    size_t n = 0, pos = 0;
    while ((pos = dump.find("ride gap", pos)) != std::string::npos) { n++; pos += 8; }
    check(n == 1, "one ride-gap line per read, latch consumed once");
}

int main() {
    test_builder_byte_exact();
    test_deny_shapes();
    test_baseline_read_ok();
    test_write_verify_pass_then_fail();
    test_wrong_pack();
    test_real_collision_frame_fails_closed();
    test_no_block_window();
    test_telemetry_through_window_plus_consume();
    test_block_outside_window_not_consumed();
    test_second_press_ignored();
    test_write_cooldown_shared();
    test_expiry_no_cadence();
    test_generic_param_read_and_band_denies();
    test_second_operation_not_killed();
    test_cooldown_bidirectional();
    test_enable_gates();
    test_aim_fires_in_pre_long_quiet();
    test_aim_jittered_ticks_find_the_band();
    test_aim_stale_prediction_unlocks_until_relock();
    test_aim_expires_without_long_observed();
    test_ride_gap_recorded_per_read();
    if (failures) {
        printf("%d read-oracle failures\n", failures);
        return 1;
    }
    printf("read oracle: all scenarios passed\n");
    return 0;
}
