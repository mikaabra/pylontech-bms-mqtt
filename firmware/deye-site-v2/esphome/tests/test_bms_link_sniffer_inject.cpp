// Host oracle for the bms-link-sniffer guarded write client (part 2,
// issue #59), review round 2: write-frame builder, allowlist gate, and
// the rewritten gap-injection state machine - all against a faked clock
// and recorded/played traffic, fully deterministic, no wall-clock.
//
// Every scenario from the reviewer's round-1 attack table is a named
// test here: outage out-waiting (now expiry), second-press reset (now
// in-flight ignore), burst rate limits (cooldown + sticky clean pair),
// cadence-less silence fires (now gated), the too-late fire corner
// (150..250 ms window), late-ack duplicate-write risk (late ack disarms
// the retry), and post-TX abort wording (result unknown).
//
// Byte-exactness policy: the builder must reproduce BOTH captured write
// frames (tests/fixtures/bms_link_fixtures.h) - which pins LENID 8008
// and the checksum formula to real wire evidence, not to arithmetic.
//
// Build & run (CLAUDE.md host-test loop):
//   cd firmware/deye-site-v2/esphome
//   clang++ -std=gnu++20 -Wall -Wextra -I. -Itests/stubs -Iincludes \
//     tests/test_bms_link_sniffer_inject.cpp -o /tmp/test_bms_link_sniffer_inject \
//     && /tmp/test_bms_link_sniffer_inject

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "bms_link_sniffer_core.h"
#include "fixtures/bms_link_fixtures.h"

using linksniff::BmsLinkCore;
using linksniff::EventKind;
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

// ---- driver plumbing: record frames a fake bus receives -------------------

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

static void slot(BmsLinkCore& c, uint32_t& t, int pack) {
    send(c, poll_req(pack, "42"), t);
    send(c, std::string(REAL_42_RESPONSE) + "\r", t + 15);
    t += 624;
}

static bool has_event(const std::vector<linksniff::LinkEvent>& ev, EventKind k) {
    for (const auto& e : ev)
        if (e.kind == k) return true;
    return false;
}

// ------------------------------------------------------------- scenarios ---

static void test_builder_byte_exact() {
    std::string f1 = linksniff::build_write_frame(0x00, 0x4C, 3421);
    check_str(f1, std::string(REAL_WRITE_TOOL) + "\r", "builder reproduces captured tool frame");
    std::string f2 = linksniff::build_write_frame(0x01, 0x4C, 3430);
    check_str(f2, std::string(REAL_WRITE_INSTACK) + "\r", "builder reproduces in-stack frame");
}

static void test_allowlist_gate() {
    std::string why;
    check(linksniff::write_request_valid(0x01, 0x4C, 3420, &why), "pack1 valid");
    check(linksniff::write_request_valid(0x04, 0x4C, 3400, nullptr), "3400 boundary allowed");
    check(linksniff::write_request_valid(0x04, 0x4C, 3500, nullptr), "3500 boundary allowed");
    check(!linksniff::write_request_valid(0x01, 0x4C, 3399, &why), "3399 rejected");
    check(!linksniff::write_request_valid(0x01, 0x4D, 100, &why), "param not allowlisted");
    check(!linksniff::write_request_valid(0x00, 0x4C, 3420, &why), "pack 00 (self-address) rejected");
    check(!linksniff::write_request_valid(0x05, 0x4C, 3420, &why), "pack 05 rejected");
    check(!linksniff::write_request_valid(0xFF, 0x4C, 3420, &why), "pack FF (broadcast) rejected");
}

static void test_deny_sets_status() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);
    check(!w.request(0x00, 0x4C, 3420, 1000), "request with bad pack returns false");
    check(w.status() == WriteController::Status::Denied, "status Denied");
    check(bus.tx_log.empty(), "nothing transmitted on deny");
}

// fire discipline: the window is 150..250 ms after the last RX, inside a
// fresh (<1500 ms) poll cadence - silence alone is not a window
static void test_fire_window() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    send(c, poll_req(1, "42"), 1000);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1015);

    check(w.request(0x01, 0x4C, 3420, 1030), "arm accepted");
    w.tick(1070);  // 55 ms silence
    check(bus.tx_log.empty(), "no fire below the 150 ms floor");
    w.tick(1180);  // 165 ms: inside the window, cadence 180 ms old
    check(bus.tx_log.size() == 1, "fired inside the 150..250 ms window");
    check(w.status() == WriteController::Status::WaitAck, "waiting ack");

    // the reviewer's 430 ms corner: a stale quiet arm must NOT fire
    BmsLinkCore c2;
    FakeBus bus2;
    WriteController w2(c2, FakeBus::enable_cb, FakeBus::tx_cb, &bus2);
    c2.set_ack_waiter(&w2);
    uint32_t t = 10000;
    slot(c2, t, 2);  // req 10000, resp 10015
    check(w2.request(0x02, 0x4C, 3420, 10030), "arm accepted (corner case)");
    w2.tick(10450);  // 435 ms since last RX - outside the window
    check(bus2.tx_log.empty(), "no fire at 435 ms since last RX");
    slot(c2, t, 2);  // req 10624, resp 10639
    w2.tick(10800);  // 161 ms since last RX
    check(bus2.tx_log.size() == 1, "fired in the fresh slot's window");
}

// reviewer row: "Armed during a bus outage, bus returns 6 h later - fires on
// recovery". Expiry + the watchdog hold must make that unreachable.
static void test_outage_expiry() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    for (int i = 0; i < 3; i++) slot(c, t, 1);  // cadence established

    check(w.request(0x01, 0x4C, 3420, 2300), "arm accepted in the outage");
    w.tick(2400);   // 137 ms silence: below the floor
    c.tick(4300);   // 2 s without requests: watchdog trips
    check(c.watchdog_tripped(), "watchdog tripped during outage");
    w.tick(4400);
    check(bus.tx_log.empty(), "no fire while watchdog tripped");
    w.tick(8400);   // 6.1 s after arming
    check(w.status() == WriteController::Status::Expired, "armed write expired in the outage");
    check_line_has(w.status_line(), "expired", "expiry line");
    check(bus.tx_log.empty(), "nothing transmitted during the outage");

    // bus returns much later: no resurrection without a new press
    uint32_t t2 = 9000;
    slot(c, t2, 1);
    w.tick(9200);
    w.tick(10000);
    check(w.status() == WriteController::Status::Expired, "expiry is terminal");
    check(bus.tx_log.empty(), "no fire on recovery after expiry");
}

// reviewer row: second press 10 ms after TX, different pack and value - must
// not reset the in-flight op and must not produce a second frame
static void test_press_in_flight_ignored() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    send(c, poll_req(1, "42"), 1000);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1015);
    check(w.request(0x01, 0x4C, 3420, 1030), "first arm accepted");
    w.tick(1180);  // 165 ms window: fire
    check(bus.tx_log.size() == 1, "first frame sent");

    // second press 10 ms later, different pack and value: IGNORED
    check(!w.request(0x02, 0x4C, 3440, 1190), "second press refused");
    check(w.status() == WriteController::Status::WaitAck, "first op still owns the window");
    check(bus.tx_log.size() == 1, "still one frame");

    // the FIRST frame's ack must land in the FIRST op's result
    send(c, std::string(REAL_ACK) + "\r", 1200);
    auto ev = c.drain_events();
    check(w.status() == WriteController::Status::Acked, "ack attributed to the in-flight op");
    check(has_event(ev, EventKind::AckForWrite), "AckForWrite event");
    check(!has_event(ev, EventKind::AnomalyAck), "no unexpected-ack misfire");
    check(w.tx_count() == 1, "no second frame from the straggler press");
}

// reviewer row: "1 failure, then 5 more presses - 6 frames in 960 ms". The
// in-flight ignore must cap the burst: no new frame may appear while the
// original one-shot (including its retry) is anywhere in flight.
static void test_burst_storm_single_frame() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);  // req 1000, resp 1015
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);    // window fire
    check(bus.tx_log.size() == 1, "attempt 1 sent");
    w.tick(1450);   // deadline (>250 ms): no-ack verdict, retry armed in flight
    check(w.status() == WriteController::Status::Armed, "retry armed, in flight");
    check_line_has(w.status_line(), "no ack", "no-ack wording carries retry intent");

    // burst: five presses while the failed one-shot is armed - all ignored
    for (int i = 0; i < 5; i++) {
        check(!w.request(0x02, 0x4C, 3430, 1500 + i * 40), "storm press ignored");
    }
    for (uint32_t tt = 1500; tt <= 2460; tt += 80) {
        w.tick(tt);
    }
    check(bus.tx_log.size() == 1, "burst produced no extra frame (was 6 in 960 ms)");

    // the armed retry itself dies at expiry without a window
    w.tick(7300);   // >6 s after the TX-restarted arm clock
    check(w.status() == WriteController::Status::Expired ||
          w.status() == WriteController::Status::Armed, "no burst growth");
    check(w.tx_count() == 1, "tx count still exactly one for the whole burst");
}

// reviewer row: "Ack arrives at 200 ms - treated as no ack; retry stays armed
// - duplicate write risk". A late ack, before the retry fires, must disarm.
static void test_late_ack_disarms_retry() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);  // fire
    check(bus.tx_log.size() == 1, "frame out");

    w.tick(1450);  // deadline passed, no traffic: no-ack verdict, retry armed
    check(w.status() == WriteController::Status::Armed, "retry armed at deadline");
    check(w.tx_count() == 1, "one attempt so far");

    // the straggler ack lands after the deadline but before the retry fires
    send(c, std::string(REAL_ACK) + "\r", 1500);
    auto ev = c.drain_events();
    check(w.status() == WriteController::Status::Acked, "late ack accepted");
    check(has_event(ev, EventKind::AckForWrite), "late ack consumed, not an anomaly");
    check_line_has(w.status_line(), "late", "status says the ack was late");
    check_line_has(w.status_line(), "no retry", "explicit duplicate-write disarm");

    // even a perfect window later must NOT transmit anything more
    uint32_t t2 = 2000;
    slot(c, t2, 1);  // clean pair after the TX (the ack set the requirement)
    w.tick(2215);
    w.tick(5400);   // cooldown long past
    check(w.status() == WriteController::Status::Acked, "ack is terminal");
    check(w.tx_count() == 1, "no duplicate write: total frames stay at 1");
}

// full legitimate retry lifecycle under the new rules: no ack -> clean pair +
// 4 s cooldown -> retry fires -> terminal result-unknown
static void test_single_retry_with_cooldown() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);   // attempt 1
    check(bus.tx_log.size() == 1, "attempt 1 out");
    w.tick(1460);   // deadline, silence: no-ack verdict
    check(w.status() == WriteController::Status::Armed, "retry armed (clean slot + cooldown wait)");

    uint32_t t2 = 2000;
    slot(c, t2, 1);   // clean pair observed after the failure... but cooldown
    w.tick(2215);     // only ~1 s since the TX: cooldown holds
    check(bus.tx_log.size() == 1, "cooldown holds the retry under 4 s");
    w.tick(5300);     // ~4.1 s since the TX but 1165 ms since last RX: window miss
    check(bus.tx_log.size() == 1, "window miss also holds the retry");

    uint32_t t3 = 6000;
    slot(c, t3, 1);   // fresh pair: cooldown passed, window opens
    w.tick(6215);     // 200 ms since last RX
    check(bus.tx_log.size() == 2, "retry fired after clean pair + cooldown + window");
    check(w.status() == WriteController::Status::WaitAck, "retry waiting ack");

    w.tick(6740);     // second deadline, no traffic: terminal
    check(w.status() == WriteController::Status::NoAck, "terminal NoAck after retry");
    check(w.tx_count() == 2, "exactly initial + single retry");
    check_line_has(w.status_line(), "result unknown", "result-unknown wording");
    w.tick(8000);
    check(w.tx_count() == 2, "terminal: no further frame without a press");
}

// collision path: traffic inside our ack window verdicts as collision (now
// counted on EVERY attempt, not only the terminal one)
static void test_collision_detected() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);   // attempt 1 out
    send(c, poll_req(1, "42"), 1250);   // master's poll lands inside our window
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1265);
    w.tick(1450);   // deadline: traffic seen: collision verdict on attempt 1
    check(w.status() == WriteController::Status::Armed, "retry armed after collision");
    check(w.collision_count() == 1, "first-attempt collision counted (round-2 nit)");
    check_line_has(w.status_line(), "collision", "collision wording");

    // retry happens only after clean pair + cooldown + fresh window
    uint32_t t2 = 6000;
    slot(c, t2, 1);   // pair at 6000/6015 - also refreshes cadence
    w.tick(6215);     // 200 ms window, cooldown 4820 ms passed
    check(bus.tx_log.size() == 2, "retry fired");
    send(c, poll_req(1, "42"), 6280);   // colliding poll inside window 2
    send(c, std::string(REAL_42_RESPONSE) + "\r", 6295);
    w.tick(6470);   // deadline with traffic: terminal Collision
    check(w.status() == WriteController::Status::Collision, "terminal Collision");
    check(w.collision_count() == 2, "both collisions counted");
    check_line_has(w.status_line(), "result unknown", "collision also result-unknown");
}

// enable-off aborts: clean pre-TX wording vs sent-unknown post-TX wording
static void test_enable_off_aborts() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);
    using S = WriteController::Status;

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    bus.enable = false;
    w.tick(1180);
    check(bus.tx_log.empty(), "pre-TX abort sends nothing");
    check(w.status() == S::Aborted, "pre-TX abort status");
    check_line_has(w.status_line(), "aborted", "pre-TX abort wording");
    check(w.status_line().find("result unknown") == std::string::npos,
          "pre-TX abort does not claim a sent frame");

    bus.enable = true;
    check(w.request(0x01, 0x4C, 3420, 3000), "re-arm accepted");
    w.tick(3215);   // cadence from the old slot is stale: holds
    check(w.status() == S::Armed, "stale cadence holds (fresh pair needed)");
    uint32_t t2 = 4000;
    slot(c, t2, 1);
    w.tick(4215);   // fresh window: fires
    check(bus.tx_log.size() == 1, "post-arm fire happened");
    bus.enable = false;
    w.tick(4230);
    check(w.status() == S::Aborted, "post-TX abort status");
    check_line_has(w.status_line(), "result unknown", "post-TX abort wording says result unknown");
}

// uart flush failure after the write: honest abort, bytes may be on the wire
static void test_uart_fail_aborts() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    bus.tx_ok = false;
    w.tick(1180);
    check(w.status() == WriteController::Status::Aborted, "flush failure aborts");
    check_line_has(w.status_line(), "on the wire", "flush failure wording is conservative");
    check(w.tx_count() == 0, "failed flush not counted as a sent frame");
}

// reviewer row: "One noise byte, no poll ever seen - fires, watchdog healthy"
static void test_noise_only_no_cadence() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint8_t junk[] = {0x33, 0x55};
    c.feed(junk, sizeof junk, 1000);
    check(w.request(0x01, 0x4C, 3420, 1100), "arm accepted on a never-polled bus");
    w.tick(2000);   // silence is 'confirmed' but no cadence was ever seen
    check(bus.tx_log.empty(), "no fire without an established poll cadence");
    check(!c.watchdog_tripped(), "watchdog genuinely quiet (no requests ever)");
    w.tick(7200);   // expiry bounds the wait
    check(w.status() == WriteController::Status::Expired, "expired without cadence");
    check(bus.tx_log.empty(), "nothing ever sent");
}

// the whole state machine across the uint32 millis() wrap seam: TX fired
// 64 ms before the wrap, ack accepted across the seam, second op held by
// cooldown, retried, and late-acked - all after 0x00000000
static void test_millis_wraparound() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    send(c, poll_req(1, "42"), 0xFFFFFF00u);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 0xFFFFFF15u);
    check(w.request(0x01, 0x4C, 3420, 0xFFFFFF20u), "arm accepted before the wrap");
    w.tick(0xFFFFFFC0u);  // 171 ms window: fire, 64 ms before the wrap
    check(bus.tx_log.size() == 1, "fired before the wrap");
    check(w.status() == WriteController::Status::WaitAck, "waiting across the seam");

    // ack 64 ms after the wrap: 160 ms elapsed, inside the deadline
    send(c, std::string(REAL_ACK) + "\r", 0x40);
    check(w.status() == WriteController::Status::Acked, "ack accepted across the seam");
    check(w.tx_count() == 1, "still one frame");

    // second one-shot: sticky clean pair + cooldown across the wrap
    check(w.request(0x02, 0x4C, 3425, 0x100u), "re-arm after the wrap");
    uint32_t t = 0x600u;
    slot(c, t, 2);           // pair at 0x600/0x615
    w.tick(0x800u);          // cooldown (2112 ms since TX) still holds
    check(bus.tx_log.size() == 1, "cooldown holds across the wrap");
    uint32_t t2 = 0x1000u;
    slot(c, t2, 2);          // fresh pair at 0x1000/0x1015
    w.tick(0x1100u);         // 4416 ms since TX, window 235 ms
    check(bus.tx_log.size() == 2, "second op fired after cooldown across the wrap");
    w.tick(0x11FBu);         // deadline, no traffic: retry armed
    check(w.status() == WriteController::Status::Armed, "retry armed after the wrap");
    send(c, std::string(REAL_ACK) + "\r", 0x1200u);   // late ack disarms the retry
    check(w.status() == WriteController::Status::Acked, "late ack consumed after the wrap");
    check(w.tx_count() == 2, "wrap cycle ends at two frames");
    check(c.anomalies_total() == 0, "whole wrap cycle healthy");
}

int main() {
    test_builder_byte_exact();
    test_allowlist_gate();
    test_deny_sets_status();
    test_fire_window();
    test_outage_expiry();
    test_press_in_flight_ignored();
    test_burst_storm_single_frame();
    test_late_ack_disarms_retry();
    test_single_retry_with_cooldown();
    test_collision_detected();
    test_enable_off_aborts();
    test_uart_fail_aborts();
    test_noise_only_no_cadence();
    test_millis_wraparound();

    if (failures == 0) {
        printf("test_bms_link_sniffer_inject: ALL PASS\n");
        return 0;
    }
    printf("test_bms_link_sniffer_inject: %d FAILURES\n", failures);
    return 1;
}
