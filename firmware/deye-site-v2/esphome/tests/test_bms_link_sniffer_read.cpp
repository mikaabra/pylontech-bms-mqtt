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
        bool off = linksniff::op_just_finished(prev_w, prev_r, h.w.status(), h.r.status());
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

// ---- long-gap aim (live cadence measured 2026-11-11: regular idle
// 470-530 ms, one ~1100 ms rotation gap every 3rd slot; a block answer
// needs ~590 ms of wire at 9600, so it only completes inside a long gap)

static void test_aim_fires_in_predicted_long_gap() {
    Harness h;
    // gaps: 1100, 520, 520, 1100 - rotation aligned
    pair_at(h, 1, 1000);
    pair_at(h, 2, 2116);   // gap 1100 (long)
    pair_at(h, 3, 2651);   // gap 520
    check(h.r.request(0x01, 0x4C, false, 0, 2700, true), "aimed read armed");
    check_line_has(h.r.status_line(), "long-gap-aligned", "armed line marks the aim");
    h.r.tick(2836);        // opening gap: counter 1, not due - must hold
    check(h.r.status() == ReadController::Status::Armed,
          "aimed read holds through a not-due regular gap");
    pair_at(h, 4, 3186);   // gap 520 -> counter reaches due
    h.r.tick(3371);        // confirm-band tick inside the due gap
    check(h.r.status() == ReadController::Status::WaitBlock,
          "aimed read fires in the predicted-long gap");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");
}

static void test_aim_falls_back_when_no_longs() {
    Harness h;
    // all-regular cadence (no long gaps at all): the counter climbs past
    // due, so the read must still fire - no starvation, just one gap
    // later than legacy behavior
    pair_at(h, 1, 1000);
    pair_at(h, 2, 1535);   // gap 520
    check(h.r.request(0x02, 0x4C, false, 0, 1600, true), "aimed read armed (no-long cadence)");
    h.r.tick(1720);        // opening gap: counter 1 - hold (legacy would fire here)
    check(h.r.status() == ReadController::Status::Armed,
          "aimed read holds before the counter reaches due");
    pair_at(h, 3, 2070);   // counter reaches due from here on
    h.r.tick(2255);
    check(h.r.status() == ReadController::Status::WaitBlock,
          "aimed read fires once due even without real longs");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");
}

static void test_aim_detects_early_long_gap() {
    Harness h;
    // rotation drift: a long gap opens while the counter says not-due.
    // Hold through the confirm band, then fire mid-flight once the gap
    // out-lives every regular idle
    pair_at(h, 1, 1000);
    pair_at(h, 2, 2116);   // gap 1100 (long)
    pair_at(h, 3, 2651);   // gap 520
    check(h.r.request(0x01, 0x4C, false, 0, 2700, true), "aimed read armed (drift)");
    h.r.tick(2836);        // counter 1, since 170: in the confirm band but not due
    check(h.r.status() == ReadController::Status::Armed, "holds before detection band");
    h.r.tick(3110);        // since 444: confirm band passed, not detected yet
    check(h.r.status() == ReadController::Status::Armed, "still holding below detect band");
    h.r.tick(3320);        // since 654 >= kLongGapDetectMs: long gap detected mid-flight
    check(h.r.status() == ReadController::Status::WaitBlock,
          "fires mid-flight once the gap out-lives regular idle");
    check(h.bus.tx_log.size() == 1, "exactly one frame fired");
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
    test_aim_fires_in_predicted_long_gap();
    test_aim_falls_back_when_no_longs();
    test_aim_detects_early_long_gap();
    if (failures) {
        printf("%d read-oracle failures\n", failures);
        return 1;
    }
    printf("read oracle: all scenarios passed\n");
    return 0;
}
