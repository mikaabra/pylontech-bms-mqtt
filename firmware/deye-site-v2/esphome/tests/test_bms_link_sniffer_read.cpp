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
        c.set_variant_waiter(&r);
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
// ticks wide so jittered feed ticks always land inside; falls back never
// - unlocked, missed-long or stale reads expire and re-press) +
// missed-long unlock (reviewer ruling, option A, PR prelong-unlock): a
// missed expected long disarms the aim until an observed long re-locks
// it - no slide. The #99 fire fixtures below are re-anchored on the
// ruled geometry: the anchor is an OBSERVED long one period (2511 ms)
// out, ordinary pairs in between, no expected-long window elapsing
// before the fire - their old buses were built on the retired slide
// and missed the predicted long, the exact 01b bug class. That old
// construction is preserved verbatim as the missed-long oracle. ----

static void test_aim_fires_in_pre_long_quiet() {
    Harness h;
    // ruled geometry (option A): the anchor is an OBSERVED long ONE
    // PERIOD out. Quiet [1015, 2100] is a long (gap 1085 -> lock,
    // anchor 1015); prediction P = 1015 + 2511 = 3526. Ordinary pairs
    // 2544/2829 run between the anchor's closing pair and the corridor
    // quiet - no gap >= 700 (no re-anchor), nothing reaches P before
    // the fire: zero missed expected longs throughout.
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 2100);          // ends 2115; long open recorded 1015
    check(h.r.request(0x01, 0x4C, false, 0, 2130, true), "aimed read armed");
    check_line_has(h.r.status_line(), "long-gap-aligned", "armed line marks the aim");
    // the quiet opening at 2559 leads P by 967 - not a corridor, and
    // ticks there must not fire at any band
    pair_at(h, 3, 2544);          // gap 429, ends 2559
    h.r.tick(2739);               // since 180: the retired v1 fire band, lead 967
    check(h.r.status() == ReadController::Status::Armed,
          "v1-band tick in a non-corridor quiet holds");
    h.r.tick(2758);               // since 199: below the re-derived band
    check(h.r.status() == ReadController::Status::Armed,
          "below-band tick in a non-corridor quiet holds");
    // the corridor quiet: opens 2844 (pair 4 ends 2829+15), lead
    // 3526 - 2844 = 682 in [630, 700]
    pair_at(h, 4, 2829);          // gap 270, ends 2844
    h.r.tick(2924);               // since 80: far below the band
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds far below the band");
    h.r.tick(3024);               // since 180: the v1 band - must NOT fire
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds at the retired v1 band (180 ms)");
    h.r.tick(3094);               // since 250: the re-derived band, lead 682
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
    // must land inside it. Ruled geometry: observed long [1015, 2100]
    // one period out (prediction 3526), ordinary pairs 2544/2829, no
    // missed expected long at any tick.
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 2100);          // long open 1015, ends 2115
    pair_at(h, 3, 2544);          // gap 429, ends 2559
    pair_at(h, 4, 2829);          // gap 270, ends 2844: corridor, lead 682
    check(h.r.request(0x01, 0x4C, false, 0, 2900, true), "aimed read armed (jitter case)");
    h.r.tick(3091);               // since 247: jittered, just below the band
    check(h.r.status() == ReadController::Status::Armed,
          "jittered tick just below the band holds (247 ms)");
    h.r.tick(3102);               // since 258: jittered, lands inside [250, 300]
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

static void test_aim_upper_leads_696_fires_710_holds() {
    Harness h;
    // the widened lead bound (verdict correction 3): the worst-case
    // corridor lead is ~696 ms (144 B closing answer + 25 ms start
    // delay) - below the old 690 cap, inside the new 700. A 710 lead is
    // not a corridor: misses cost latency, not safety. Ruled geometry:
    // observed long [1015, 2100] (prediction 3526), ordinary pairs
    // 2544/2815, corridor quiet opens 2830 = 3526 - 696, no missed
    // expected long before the fire.
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 2100);          // gap 1085 -> LOCK, long open 1015; ends 2115
    pair_at(h, 3, 2544);          // gap 429, ends 2559
    pair_at(h, 4, 2815);          // gap 256, ends 2830: quiet opens 2830;
                                  // lead 3526 - 2830 = 696
    check(h.r.request(0x01, 0x4C, false, 0, 2900, true), "aimed read armed (696-lead case)");
    h.r.tick(3080);               // since 250: in band, lead 696 - the verdict's worst case
    check(h.r.status() == ReadController::Status::WaitBlock,
          "the 696 ms worst-case lead corridor fires under the widened bound");
    check_line_has(h.r.status_line(), "pre-long", "fire line names the window class");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");

    // The 710 ms lead is outside: the same lock (anchor 1015,
    // prediction 3526) carries a 710 lead for a quiet opening at
    // 3526 - 710 = 2816 - an in-band tick must hold, on the no-miss
    // geometry so the hold is attributable to the lead alone.
    Harness h2;
    pair_at(h2, 1, 1000);         // ends 1015
    pair_at(h2, 2, 2100);         // LOCK, ends 2115
    pair_at(h2, 3, 2544);         // gap 429, ends 2559
    pair_at(h2, 4, 2801);         // gap 242, ends 2816: quiet opens 2816, lead 710
    check(h2.r.request(0x01, 0x4C, false, 0, 2870, true), "aimed read armed (710-lead case)");
    h2.r.tick(3066);              // since 250: in band, lead 710: OUTSIDE the window
    check(h2.r.status() == ReadController::Status::Armed,
          "a 710 ms lead is not a corridor - in-band tick holds");
    check(h2.bus.tx_log.empty(), "nothing fired outside the lead window");
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

// The missed-long unlock's oracle (1) - THE 01b CLASS, named for what it
// is: this is the OLD #99 696 fixture bus, preserved verbatim on
// purpose. Anchor long opens 1015 (prediction 3526), the ordinary
// 469-ms pairs stream straight through the predicted long - it never
// arrives - and then a quiet opens whose lead is 696 in-window ONLY
// under the retired slide (which had shifted the prediction to 6037 by
// then). That is the live failure demonstrated at 19:39:43 Nov 12
// (records #103, pack-01 attempt 1): the suppression run began ~7 s
// before the corridor fire into which the shift-forward shipped a
// frame. The anchor here is only 4.6 s old - the stale gate is fresh -
// so the hold proves the missed-long unlock alone.
static void test_01b_missed_long_blocks_corridor_fire() {
    Harness h;
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 3080);          // gap 2065 -> LOCK, long open 1015; ends 3095
    pair_at(h, 3, 3564);          // gap 469: prediction 3526 missed (the
                                  // quiet [3095, 3564] straddles it, regular)
    pair_at(h, 4, 4048);          // gap 469, ends 4063
    pair_at(h, 5, 4532);          // gap 469, ends 4547
    pair_at(h, 6, 5016);          // gap 469, ends 5031
    pair_at(h, 7, 5326);          // gap 295, ends 5341: quiet opens 5341.
                                  // Old slide: prediction shifted to 6037,
                                  // lead 696 - THE 01b fire. New: unlocked.
    check(h.r.request(0x01, 0x4C, false, 0, 5360, true), "aimed read armed (01b case)");
    h.r.tick(5591);               // since 250: in band, stale gate fresh -
                                  // only the missed long explains the hold
    check(h.r.status() == ReadController::Status::Armed,
          "a quiet in-window only under the retired slide holds - the 01b class");
    check(h.bus.tx_log.empty(), "nothing fired into the broken cadence");
}

// The missed-long unlock's oracle (2): after the 01b miss, the next
// OBSERVED long re-anchors the predictor, and the corridor before the
// following prediction fires with the usual validators (lead + band).
static void test_aim_relock_after_miss_fires_next_corridor() {
    Harness h;
    // the 01b bus and its hold (previous oracle), then recovery
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 3080);          // LOCK, long open 1015; ends 3095
    pair_at(h, 3, 3564);          // prediction 3526 missed; ends 3579
    pair_at(h, 4, 4048);          // ends 4063
    pair_at(h, 5, 4532);          // ends 4547
    pair_at(h, 6, 5016);          // ends 5031
    pair_at(h, 7, 5326);          // ends 5341
    check(h.r.request(0x01, 0x4C, false, 0, 5360, true), "aimed read armed (re-lock case)");
    h.r.tick(5591);               // the 01b candidate: holds while unlocked
    check(h.r.status() == ReadController::Status::Armed,
          "unlocked predictor holds at the 01b-class candidate");
    pair_at(h, 8, 6460);          // gap 1119 -> RE-LOCK (observed long,
                                  // quiet-open 5341); next prediction
                                  // 5341 + 2511 = 7852; ends 6475
    pair_at(h, 9, 7155);          // gap 680 (ordinary), ends 7170: corridor
                                  // quiet opens 7170, lead 7852 - 7170 = 682
    h.r.tick(7420);               // since 250: re-locked + fresh, in band,
                                  // fire before 7852 - no further miss
    check(h.r.status() == ReadController::Status::WaitBlock,
          "the observed long re-locks the predictor and the next corridor fires");
    check_line_has(h.r.status_line(), "pre-long", "fire line names the window class");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired, and only after the re-lock");
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

// ---- CID2-47 read variants (reviewer ruling round 2, PR read-variant-micro) ----
// One selector extends the existing read request; no parallel TX path.
// INFO comes only from the reviewed allowlist: FOUR short-read shapes,
// packs 03/04 ONLY (the v1 pack-01 isolation variant is retired - the
// clean-phase retry settled request-vs-answer, records #103). Grading is
// EVIDENCE-ONLY with #87-style STRUCTURAL acceptance: on this bus every
// pack answers with ADR 00 (PROTOCOL_REFERENCE.md:409), so v1's ADR
// matcher could never match a real answer (its oracle passed only on
// synthetic ADR=pack fixtures - none of those shapes remain here). The
// FIRST response-shaped frame (80 00 D5, ANY RTN incl. error codes 04/06)
// after our TX is the answer; a master poll (42/44) arriving first grades
// the probe terminal, and its B07E-class answer then pairs normally -
// never consumed. The verdict records RTN first, then LENID and the
// start delay - a new answer shape is never a value.
// NOTE the LENID pin: the len-4 LENID is C004 (the digit-sum formula of
// lenid4_valid and the pack-side parser gives lchk 0xC for len 4 - a
// D004 frame would fail the pack's own LCHK check). A006 for the 6-char
// tails matches the captured teleControl LENID family for 6-char INFO.

static void test_variant_frames_byte_exact() {
    // the block form is the byte-exact regression: default + explicit
    check_str(linksniff::build_read_frame(0x01),
              std::string("~8000D547E00201FD1C\r"),
              "default (kBlock) keeps the pinned capture byte-exact");
    check_str(linksniff::build_read_frame(0x01, linksniff::ReadVariant::kBlock),
              linksniff::build_read_frame(0x01),
              "explicit kBlock equals the default");
    // fourth short-read candidate: INFO <pack>4C, LENID C004, packs 03/04
    // (the retired isolation shape retargeted to the new-gen packs)
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort4C),
              std::string("~8000D547C004034C") +
                  rs485_calc_chksum("8000D547C004034C") + "\r",
              "short-4C pack 03 via the shared CHK helper");
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort4C),
              std::string("~8000D547C004034CFCA3\r"),
              "short-4C pack 03 pinned literal");
    check_str(linksniff::build_read_frame(0x04, linksniff::ReadVariant::kShort4C),
              std::string("~8000D547C004044CFCA2\r"),
              "short-4C pack 04 pinned literal");
    // the len-6 tails keep their literals: 6-char INFO, LENID A006
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort004C),
              std::string("~8000D547A00603004CFC43\r"),
              "short-004C pack 03 pinned literal");
    check_str(linksniff::build_read_frame(0x04, linksniff::ReadVariant::kShort4C01),
              std::string("~8000D547A006044C01FC41\r"),
              "short-4C01 pack 04 pinned literal");
    check_str(linksniff::build_read_frame(0x04, linksniff::ReadVariant::kShort4C4C),
              std::string("~8000D547A006044C4CFC2B\r"),
              "short-4C4C pack 04 pinned literal");
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort4C4C),
              std::string("~8000D547A006034C4CFC2C\r"),
              "short-4C4C pack 03 pinned literal");
    // the LENID family stays in the measured/captured set
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort4C).substr(9, 4),
              std::string("C004"), "4-char INFO LENID is C004 (digit-sum LCHK)");
    check_str(linksniff::build_read_frame(0x03, linksniff::ReadVariant::kShort004C).substr(9, 4),
              std::string("A006"), "6-char INFO LENID is A006 (teleControl family)");
}

// arm after a complete pair at pair_t, fire on that pair's idle window
// (request at +30, fire tick at +170 = 155 ms of silence)
static void fire_variant(Harness& h, uint8_t pack, linksniff::ReadVariant v, uint32_t t) {
    check(h.r.request(pack, 0x4C, false, 0, t + 30, false, v), "variant probe armed");
    h.r.tick(t + 170);
    check(h.r.status() == ReadController::Status::WaitVariant,
          "variant fired into the idle window, collecting");
}

// expire an armed-but-unfired probe so the next press is legal (one
// operation in flight at a time, same rule as the block read)
static void expire_arm(Harness& h) {
    for (uint32_t t = 1000; t < 7200; t += 100) h.r.tick(t);
    check(h.r.status() == ReadController::Status::Expired, "previous arm expired");
}

static void test_variant_allowlist() {
    Harness h;
    // round-2 scope change: EVERY short-read shape denies on packs 01
    // AND 02 - nothing but the block read is ever allowed there
    const linksniff::ReadVariant all_short[] = {
        linksniff::ReadVariant::kShort4C, linksniff::ReadVariant::kShort004C,
        linksniff::ReadVariant::kShort4C01, linksniff::ReadVariant::kShort4C4C,
    };
    for (auto v : all_short) {
        check(!h.r.request(0x01, 0x4C, false, 0, 1000, false, v),
              "short-read shape refused on pack 01");
        check(h.r.status() == ReadController::Status::Denied, "denied fail-closed");
        check_line_has(h.r.status_line(), "packs 03/04 only", "deny names the 03/04 rule");
    }
    for (auto v : all_short) {
        check(!h.r.request(0x02, 0x4C, false, 0, 1000, false, v),
              "short-read shape refused on pack 02");
        check(h.r.status() == ReadController::Status::Denied, "denied fail-closed");
    }
    check(h.bus.tx_log.empty(), "nothing transmitted on allowlist denies");
    // every kShort* arms on BOTH new-gen packs
    check(h.r.request(0x03, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort4C),
          "short-read 4C armed on pack 03");
    check_line_has(h.r.status_line(), "INFO 034C", "armed line names the exact INFO");
    expire_arm(h);
    check(h.r.request(0x04, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort4C),
          "short-read 4C armed on pack 04");
    check_line_has(h.r.status_line(), "INFO 044C", "armed line names the exact INFO");
    expire_arm(h);
    check(h.r.request(0x04, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort004C),
          "short-read 004C armed on pack 04");
    expire_arm(h);
    check(h.r.request(0x03, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort4C01),
          "short-read 4C01 armed on pack 03");
    expire_arm(h);
    check(h.r.request(0x04, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort4C4C),
          "short-read 4C4C armed on pack 04");
    check(h.r.status() == ReadController::Status::Armed, "armed");
}

static void test_variant_gates_hold() {
    // every existing fire gate applies to variant frames: no cadence ->
    // hold to expiry, never a TX (the existing test_expiry_no_cadence
    // pattern)
    Harness h;
    check(h.r.request(0x03, 0x4C, false, 0, 1000, false, linksniff::ReadVariant::kShort4C),
          "variant armed");
    for (uint32_t t = 1000; t < 7200; t += 100)
        h.r.tick(t);
    check(h.r.status() == ReadController::Status::Expired,
          "no cadence -> expired, no fire");
    check_line_has(h.r.status_line(), "no safe window within 6 s",
                   "same expiry wording as the block read");
    check(h.bus.tx_log.empty(), "the variant never TXed without cadence");

    // the enable gate (the existing test_enable_gates pattern): a
    // perfectly cadenced bus, the switch off - aborted before any TX
    Harness h2;
    pair_at(h2, 1, 1000);
    h2.bus.enable = false;
    check(h2.r.request(0x03, 0x4C, false, 0, 1030, false, linksniff::ReadVariant::kShort004C),
          "variant armed (enable off)");
    h2.r.tick(1170);
    check(h2.r.status() == ReadController::Status::Aborted,
          "enable off aborts the armed variant");
    check_line_has(h2.r.status_line(), "enable switched off", "abort names the gate");
    check(h2.bus.tx_log.empty(), "nothing fired with the gate closed");
}

static void test_variant_fires_in_corridor() {
    // #104's re-anchored corridor geometry (same bus as the #99 aim
    // fixtures, post-prelong-unlock), driven with a short-read shape on
    // a new-gen pack: same locks, same band, same gates - only the
    // INFO differs. The anchor is an OBSERVED long one full period
    // (2511 ms) before the predicted long, ordinary pairs between
    // (every gap < 700, no accidental re-anchor), and NO missed
    // expected long anywhere before the fire - under the #104
    // predictor a post-miss construction cannot fire (that is the
    // point of the missed-long unlock), so the corridor here is the
    // pre-prediction quiet.
    Harness h;
    pair_at(h, 1, 1000);          // ends 1015
    pair_at(h, 2, 2100);           // gap 1085 -> LOCK, anchor A=1015
                                   // (long open 1015); prediction
                                   // P = 1015 + 2511 = 3526; ends 2115
    check(h.r.request(0x03, 0x4C, false, 0, 2130, true, linksniff::ReadVariant::kShort4C),
          "short-read variant armed (aimed)");
    check_line_has(h.r.status_line(), "variant probe pack 03 INFO 034C",
                   "armed line names the variant");
    check_line_has(h.r.status_line(), "long-gap-aligned", "armed line keeps the aim tag");
    // the quiet opening at 2559 leads P by 967 - not a corridor; ticks
    // there must not fire at any band
    pair_at(h, 3, 2544);          // gap 429, ends 2559
    h.r.tick(2739);               // since 180: the retired v1 fire band
    check(h.r.status() == ReadController::Status::Armed,
          "v1-band tick in a non-corridor quiet holds");
    h.r.tick(2758);               // since 199: below the re-derived band
    check(h.r.status() == ReadController::Status::Armed,
          "below-band tick in a non-corridor quiet holds");
    // the corridor quiet: opens 2844 (pair 4 ends 2829+15), lead
    // 3526 - 2844 = 682 in [630, 700]; the fire lands at 3094, before
    // the prediction - zero missed expected longs at the fire
    pair_at(h, 4, 2829);          // gap 270, ends 2844
    h.r.tick(2924);               // since 80: far below the band
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds far below the band");
    h.r.tick(3024);               // since 180: the retired v1 band - must NOT fire
    check(h.r.status() == ReadController::Status::Armed,
          "corridor quiet holds at the retired v1 band (180 ms)");
    h.r.tick(3094);               // since 250: the re-derived band, lead 682
    check(h.r.status() == ReadController::Status::WaitVariant,
          "variant fires through the corridor path");
    check_line_has(h.r.status_line(), "variant probe INFO 034C",
                   "sent line names the variant");
    check_line_has(h.r.status_line(), "pre-long", "sent line names the window class");
    check(h.bus.tx_log.size() == 1, "exactly one frame");
    check_str(h.bus.tx_log[0], std::string("~8000D547C004034CFCA3\r"),
              "the TX callback received the exact 034C frame");
}

static void test_variant_answer_evidence() {
    // RTN 00 - the broad-answer shape, real bus shape ADR 00: LENID 0004,
    // INFO 03 4C, valid CHK, start+130 ms after our TX
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_variant(h, 0x03, linksniff::ReadVariant::kShort4C, t);
    check(h.bus.tx_log.size() == 1, "one probe frame");
    check_str(h.bus.tx_log[0], std::string("~8000D547C004034CFCA3\r"),
              "the 034C probe went out");
    check(!h.r.request(0x03, 0x4C, false, 0, t + 200, false, linksniff::ReadVariant::kShort4C),
          "press during WaitVariant is ignored");
    check_line_has(h.r.status_line(), "ignored: previous read still in flight", "ignore wording");
    check(h.bus.tx_log.size() == 1, "still exactly one frame");
    send(h.c, "~8000D5000004034CFCC1\r", t + 300);
    check(h.r.status() == ReadController::Status::VariantEvidence,
          "first 80 00 D5 response after TX consumed as evidence");
    check_line_has(h.r.status_line(), "RTN=00", "verdict records the return code");
    check_line_has(h.r.status_line(), "LENID=0004", "verdict records the LENID");
    check_line_has(h.r.status_line(), "start+130", "verdict records the fire->answer delay");
    check_line_has(h.r.status_line(), "evidence", "verdict states evidence-only");
    check_line_has(h.r.status_line(), "nothing published", "and the fail-closed wording");
    check(h.r.value_raw() == 0, "evidence never becomes a value");
    check(h.c.checksum_errors() == 0, "consumed answer added no checksum error");
    check(h.c.anomalies_total() == 0, "consumed answer added no anomaly");
    auto ev = h.c.drain_events();
    check(has_event(ev, EventKind::VariantAnswer), "VariantAnswer event visible");
    for (const auto& e : ev)
        if (e.kind == EventKind::VariantAnswer)
            check(e.pack == 3, "VariantAnswer attributed to the probe's pack");
    std::string dump = h.c.dump_bounded();
    check(dump.find("D547 03 034C ans RTN=00 LENID=0004 start+130ms evidence-only")
              != std::string::npos,
          "ring line records RTN first, then LENID and the delay");

    // RTN 04 - the error shape (CID2 invalid): the likeliest informative
    // live result to a strange read shape, consumed as evidence too
    Harness h2;
    pair_at(h2, 1, t);
    fire_variant(h2, 0x03, linksniff::ReadVariant::kShort004C, t);
    send(h2.c, "~8000D5040000FD9B\r", t + 300);
    check(h2.r.status() == ReadController::Status::VariantEvidence,
          "error return code consumed as evidence");
    check_line_has(h2.r.status_line(), "RTN=04", "verdict records the error return code");
    check_line_has(h2.r.status_line(), "LENID=0000", "verdict records the bare-ack LENID");
    check(h2.r.value_raw() == 0, "an error is evidence, never a value");
    std::string dump2 = h2.c.dump_bounded();
    check(dump2.find("D547 03 03004C ans RTN=04 LENID=0000 start+130ms evidence-only")
              != std::string::npos,
          "ring line pins the error-answer evidence record");

    // a collision remnant is still an answer (unchanged from v1): CHK
    // deliberately broken, RTN 00 - consumed, no checksum error counted
    Harness h3;
    pair_at(h3, 1, t);
    fire_variant(h3, 0x04, linksniff::ReadVariant::kShort4C, t);
    check_str(h3.bus.tx_log[0], std::string("~8000D547C004044CFCA2\r"),
              "the 044C probe went out");
    send(h3.c, "~8000D50000040444CDEAD\r", t + 300);
    check(h3.r.status() == ReadController::Status::VariantEvidence,
          "checksum remnant consumed as evidence (validity not required)");
    check_line_has(h3.r.status_line(), "RTN=00", "remnant verdict records the return code");
    check(h3.c.checksum_errors() == 0, "consumed remnant added no checksum error");
    check(h3.c.anomalies_total() == 0, "consumed remnant added no anomaly");
    check(h3.r.value_raw() == 0, "no value from a remnant");
}

static void test_variant_master_poll_cutoff() {
    // the reviewer's blocker edge: the master's next poll lands inside
    // the open window BEFORE any answer -> the terminal grade, and the
    // poll's own B07E-class answer arriving inside the window must NOT
    // be consumed as evidence - it pairs to the master's exchange
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_variant(h, 0x03, linksniff::ReadVariant::kShort004C, t);
    uint32_t pairs_before = h.c.pairs_total();
    // the master's 42 poll, real request shape, inside the window
    send(h.c, poll_req(3, "42"), t + 500);
    check(h.r.status() == ReadController::Status::NoAnswerMasterPoll,
          "master poll inside the window grades the terminal verdict");
    check_line_has(h.r.status_line(), "no answer before the master's next poll",
                   "terminal grade names the master's poll");
    check_line_has(h.r.status_line(), "nothing published - press Read again",
                   "terminal grade keeps the fail-closed retry wording");
    check(h.r.value_raw() == 0, "no value ever recorded");
    std::string dump = h.c.dump_bounded();
    check(dump.find("D547 03 03004C no-answer-before-master-poll") != std::string::npos,
          "ring carries the no-answer-before-master-poll evidence line");
    auto ev0 = h.c.drain_events();
    check(has_event(ev0, EventKind::VariantAnswer), "the terminal grade is visible");
    for (const auto& e : ev0)
        if (e.kind == EventKind::VariantAnswer)
            check(e.detail.find("no-answer-before-master-poll") != std::string::npos,
                  "event detail names the cutoff");
    // the poll's B07E telemetry answer arrives INSIDE the original
    // window: the grade is final, so it falls to normal pairing - never
    // consumed as VariantEvidence
    send(h.c, std::string(REAL_42_RESPONSE) + "\r", t + 515);
    check(h.r.status() == ReadController::Status::NoAnswerMasterPoll,
          "the B07E answer NOT consumed - controller stays terminal");
    check(h.r.status() != ReadController::Status::VariantEvidence,
          "the master's answer is never evidence");
    check(h.c.pairs_total() == pairs_before + 1,
          "the master's poll and its B07E answer pair normally");
    check(h.r.value_raw() == 0, "still no value");
    auto ev = h.c.drain_events();
    for (const auto& e : ev)
        if (e.kind == EventKind::VariantAnswer)
            check(e.detail.find("ans RTN=") == std::string::npos,
                  "no evidence consumed after the terminal grade");
}

static void test_variant_no_answer_expiry() {
    Harness h;
    uint32_t t = 1000;
    pair_at(h, 1, t);
    fire_variant(h, 0x03, linksniff::ReadVariant::kShort4C4C, t);
    check(h.bus.tx_log.size() == 1, "one probe frame on the wire");
    check_str(h.bus.tx_log[0], std::string("~8000D547A006034C4CFC2C\r"),
              "the 034C4C probe went out");
    // non-80 garbage inside the window (the tool's documented 46 4F
    // knock shape, VER 26): not response-shaped, never consumed - normal
    // processing, the controller keeps waiting
    std::string knock = "2600464F0000";
    send(h.c, "~" + knock + rs485_calc_chksum(knock) + "\r", t + 400);
    check(h.r.status() == ReadController::Status::WaitVariant,
          "non-80 garbage is not consumed - still waiting");
    auto ev0 = h.c.drain_events();
    check(!has_event(ev0, EventKind::VariantAnswer),
          "garbage never produces evidence");
    check(has_event(ev0, EventKind::AnomalyConfig), "it took the normal listener path");
    h.r.tick(t + 1670);   // window edge: exactly 1500 ms
    check(h.r.status() == ReadController::Status::WaitVariant, "window edge still waiting");
    h.r.tick(t + 1730);   // past the window, no answer-shaped frame delivered
    check(h.r.status() == ReadController::Status::Expired, "no answer -> Expired");
    check_line_has(h.r.status_line(), "no answer in the 1.5 s window (variant probe)",
                   "exact variant no-answer wording");
    check_line_has(h.r.status_line(), "nothing published", "nothing published");
    check(h.bus.tx_log.size() == 1, "one frame per press held");
    check(h.r.value_raw() == 0, "no value without an answer");
    auto ev = h.c.drain_events();
    check(!has_event(ev, EventKind::VariantAnswer), "no answer event without an answer");
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
    test_aim_upper_leads_696_fires_710_holds();
    test_aim_jittered_ticks_find_the_band();
    test_aim_stale_prediction_unlocks_until_relock();
    test_aim_expires_without_long_observed();
    test_01b_missed_long_blocks_corridor_fire();
    test_aim_relock_after_miss_fires_next_corridor();
    test_ride_gap_recorded_per_read();
    test_variant_frames_byte_exact();
    test_variant_allowlist();
    test_variant_gates_hold();
    test_variant_fires_in_corridor();
    test_variant_answer_evidence();
    test_variant_master_poll_cutoff();
    test_variant_no_answer_expiry();
    if (failures) {
        printf("%d read-oracle failures\n", failures);
        return 1;
    }
    printf("read oracle: all scenarios passed\n");
    return 0;
}
