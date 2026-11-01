// Host oracle for the bms-link-sniffer guarded write client (part 2,
// issue #59), review round 3: ONE FRAME PER PRESS (the Mac tool's posture,
// reviewer-ruled - no retry), 150 ms verdict deadline (the no-ack/collision
// decision lands before the master's next poll can mislabel it), late-ack
// LISTEN-and-report (bounded by the cooldown), per-operation has_tx_ reset
// (wordings never claim a frame the op did not send), and flush failures
// counted as TX attempts.
//
// Every reviewer finding has a named scenario, including the round-3 table:
// a later operation with zero frames sent must not say "sent; result
// unknown" (enable-off before firing / expiry without a window).
//
// Byte-exactness policy: the builder must reproduce BOTH captured write
// frames (tests/fixtures/bms_link_fixtures.h) - which pins LENID 8008 and
// the checksum formula to real wire evidence, not to arithmetic.
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

static void check_line_lacks(const std::string& line, const char* needle, const char* what) {
    if (line.find(needle) != std::string::npos) {
        failures++;
        printf("FAIL: %s - line must not contain '%s': %s\n", what, needle, line.c_str());
    }
}

// ---- driver plumbing --------------------------------------------------------

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

static void consume(BmsLinkCore& c) { (void)c.drain_events(); }

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

// fire discipline: 150..250 ms window inside a fresh (<=1500 ms) cadence
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
    w.tick(1180);  // 165 ms: mid-window, cadence 180 ms
    check(bus.tx_log.size() == 1, "fired inside the 150..250 ms window");
    check(w.status() == WriteController::Status::WaitAck, "waiting ack");

    BmsLinkCore c2;
    FakeBus bus2;
    WriteController w2(c2, FakeBus::enable_cb, FakeBus::tx_cb, &bus2);
    c2.set_ack_waiter(&w2);
    uint32_t t = 10000;
    slot(c2, t, 2);
    check(w2.request(0x02, 0x4C, 3420, 10030), "arm accepted (corner case)");
    w2.tick(10450);  // 435 ms since last RX: past the window top
    check(bus2.tx_log.empty(), "no fire at 435 ms since last RX");
    slot(c2, t, 2);
    w2.tick(10800);  // 161 ms into the fresh idle
    check(bus2.tx_log.size() == 1, "fired in the fresh slot's window");
}

// ONE FRAME PER PRESS: firing late in the window, the verdict must land
// BEFORE the master's next poll (~436 ms after our last RX) - a plain
// no-ack can never be mislabeled "collision" by the next slot's traffic
static void test_verdict_before_next_poll() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);
    using S = WriteController::Status;

    send(c, poll_req(2, "42"), 1000);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1015);      // last RX
    check(w.request(0x02, 0x4C, 3420, 1030), "arm accepted");
    w.tick(1245);   // 230 ms: firing near the window TOP
    check(bus.tx_log.size() == 1, "fired near the window top");
    check(w.status() == S::WaitAck, "waiting");

    w.tick(1400);   // 155 ms after TX start: verdict due (150 + slack)
    check(w.status() == S::NoAck, "no-ack verdict, not collision");
    check(w.collision_count() == 0, "no false collision from the next poll");
    check_line_has(w.status_line(), "no ack", "no-ack verdict text");

    // the master's next poll arrives at +436 after the last RX: AFTER the
    // verdict. It is just traffic; it must not change the outcome.
    send(c, poll_req(2, "42"), 1451);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1466);
    check(w.status() == S::NoAck, "verdict holds after the next poll lands");
    check(w.collision_count() == 0, "post-verdict traffic never counted as collision");
    check(w.tx_count() == 1, "one frame per press, no retry exists");
}

// outage row: expiry during the outage; expiry is terminal; without a TX in
// the operation, the wording must not claim a sent frame
static void test_outage_expiry() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    for (int i = 0; i < 3; i++) slot(c, t, 1);

    check(w.request(0x01, 0x4C, 3420, 2300), "arm accepted in the outage");
    c.tick(4300);
    check(c.watchdog_tripped(), "watchdog tripped");
    w.tick(4400);
    check(bus.tx_log.empty(), "no fire while watchdog tripped");
    w.tick(8400);
    check(w.status() == WriteController::Status::Expired, "expired in the outage");
    check_line_has(w.status_line(), "expired", "expiry line");
    check_line_lacks(w.status_line(), "result unknown",
                     "expiry without a TX must not claim result-unknown");
    check(bus.tx_log.empty(), "nothing transmitted during the outage");

    uint32_t t2 = 9000;
    slot(c, t2, 1);
    w.tick(9200);
    check(w.status() == WriteController::Status::Expired, "expiry is terminal");
    check(bus.tx_log.empty(), "no fire on recovery after expiry");
}

// second press mid-flight: ignored, never a reset
static void test_press_in_flight_ignored() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    send(c, poll_req(1, "42"), 1000);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1015);
    check(w.request(0x01, 0x4C, 3420, 1030), "first arm accepted");
    w.tick(1180);
    check(bus.tx_log.size() == 1, "first frame sent");
    check(!w.request(0x02, 0x4C, 3440, 1190), "second press refused");
    check(w.status() == WriteController::Status::WaitAck, "first op still owns the window");
    check(w.tx_count() == 1, "still one frame");

    send(c, std::string(REAL_ACK) + "\r", 1200);  // in-window ack
    auto ev = c.drain_events();
    check(w.status() == WriteController::Status::Acked, "ack attributed to the in-flight op");
    check(has_event(ev, EventKind::AckForWrite), "AckForWrite event");
    check(!has_event(ev, EventKind::AnomalyAck), "no unexpected-ack misfire");
}

// burst row: 1 failure + 5 more presses - the single frame stands; presses
// after the terminal are accepted but held by clean-pair + cooldown + window
static void test_burst_storm_single_frame() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);
    check(bus.tx_log.size() == 1, "attempt sent");
    w.tick(1340);   // verdict deadline: single-frame terminal
    check(w.status() == WriteController::Status::NoAck, "terminal NoAck after the single frame");
    check_line_has(w.status_line(), "after the single frame", "no-retry wording");

    // five presses right after the terminal: the first re-arms (and is
    // held by clean-pair + cooldown + window), the rest are in-flight
    // refusals; nothing else reaches the wire regardless
    check(w.request(0x02, 0x4C, 3430, 1400), "first post-terminal press re-arms");
    for (int i = 1; i < 5; i++)
        check(!w.request(0x02, 0x4C, 3430, 1400 + i * 40), "storm press in flight: refused");
    for (uint32_t tt = 1400; tt <= 2360; tt += 80) w.tick(tt);
    check(bus.tx_log.size() == 1, "burst produced no extra frame (was 6 in 960 ms)");
    w.tick(7200);   // their 6 s expiry also bounds the held arm
    check(w.status() == WriteController::Status::Expired ||
          w.status() == WriteController::Status::Armed,
          "burst arm bounded by expiry");
    check(bus.tx_log.size() == 1, "still exactly one frame for the whole burst");
}

// late-ack row: no-ack verdict, then the straggler ack lands inside the
// listen window - reported, never a duplicate write; past the cooldown the
// bare ack is an unexplained anomaly again
static void test_late_ack_reported() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);   // TX at 1180
    w.tick(1340);   // verdict deadline: terminal NoAck
    check(w.status() == WriteController::Status::NoAck, "no-ack after the single frame");
    check(w.tx_count() == 1, "one frame");

    // straggler ack 200 ms after the frame, inside the cooldown listen
    send(c, std::string(REAL_ACK) + "\r", 1380);
    auto ev = c.drain_events();
    check(w.status() == WriteController::Status::NoAck, "listen is report-only: status holds");
    check(has_event(ev, EventKind::AckForWrite), "late ack consumed, not an anomaly");
    check_line_has(w.status_line(), "late bare ack", "late-ack report on the status line");
    check_line_has(w.status_line(), "after our frame", "reported as time since the frame, not the deadline");
    check_line_has(w.status_line(), "probably written", "the report says what it means");
    check(w.tx_count() == 1, "no duplicate write: still one frame");

    // a bare ack AFTER the listen window (cooldown passed) is unexplained
    uint32_t t2 = 6000;
    slot(c, t2, 1);   // clean pair for the next press + fresh cadence
    uint32_t t3 = 8000;
    slot(c, t3, 1);
    send(c, std::string(REAL_ACK) + "\r", 9500);  // >4 s since our frame
    ev = c.drain_events();
    check(has_event(ev, EventKind::AnomalyAck), "post-listen bare ack is an anomaly again");
    check(w.tx_count() == 1, "still nothing new transmitted");
}

// explicit second press lifecycle: cooldown + clean pair + window gate the
// next frame; the count is two frames across two presses, never a retry
static void test_explicit_second_press() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm 1");
    w.tick(1180);   // TX 1
    w.tick(1340);   // NoAck terminal
    check(w.tx_count() == 1, "one frame so far");

    uint32_t t2 = 2000;
    slot(c, t2, 1);   // fresh pair for the second press
    check(w.request(0x01, 0x4C, 3420, 2100), "arm 2 (explicit press)");
    // round 4: the tick lands 200 ms after the fresh pair - window OPEN,
    // clean pair observed, cadence fresh. The 1035 ms cooldown since TX 1
    // is the ONLY gate holding this frame (with the broken has_tx_-keyed
    // gate this fired ~620 ms after TX 1).
    w.tick(2215);
    check(bus.tx_log.size() == 1, "cooldown-only gate holds the second press");
    w.tick(5400);     // cadence long stale, window long gone: still holds
    check(bus.tx_log.size() == 1, "no window, no fire");

    uint32_t t3 = 6000;
    slot(c, t3, 1);
    w.tick(6215);     // clean pair + cooldown + 200 ms window
    check(bus.tx_log.size() == 2, "second press fired after all gates");
    w.tick(6370);     // verdict
    check(w.status() == WriteController::Status::NoAck, "second op terminal NoAck");
    check(w.tx_count() == 2, "two frames total, both deliberate presses");
    w.tick(8000);
    check(w.tx_count() == 2, "no retry exists to fire");
}

// collision: traffic inside the 150 ms verdict window - terminal on the
// FIRST collision, counted on the first attempt
// Round 4 must-fix oracle: every gate EXCEPT the cooldown is satisfied -
// fresh pairs keep the cadence live, the clean pair is long observed, and
// each tick lands inside a fresh 150..250 ms window - so the tick_armed
// cooldown (keyed on never-reset tx_count_) is the only gate left. It
// must hold at every window until 4 s after the TX, then fire once.
static void test_cooldown_only_gate() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);   // pair 1000/1015 (clean)
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);    // 165 ms window: TX at 1180
    check(bus.tx_log.size() == 1, "first frame sent");
    w.tick(1340);    // NoAck terminal
    check(w.request(0x01, 0x4C, 3420, 1400), "second press accepted post-terminal");

    // five consecutive OPEN windows after the TX: the cooldown must hold
    // every single one (with the round-3 has_tx_-keyed bug every one of
    // these fired ~620 ms apart in the reviewer harness)
    uint32_t pairs[] = {2000, 2624, 3248, 3872, 4496};
    for (uint32_t p : pairs) {
        send(c, poll_req(1, "42"), p);
        send(c, std::string(REAL_42_RESPONSE) + "\r", p + 15);
        w.tick(p + 215);   // 200 ms after the fresh pair: window open
        check(bus.tx_log.size() == 1,
              "cooldown holds with the window open and a clean pair");
    }
    check(w.status() == WriteController::Status::Armed, "still armed through the holds");

    // cooldown ends at 1180 + 4000 = 5180; the NEXT open window fires once
    uint32_t p = 5120;
    send(c, poll_req(1, "42"), p);
    send(c, std::string(REAL_42_RESPONSE) + "\r", p + 15);
    w.tick(p + 215);   // 4155 ms since TX, 200 ms window: fires
    check(bus.tx_log.size() == 2, "fires once the cooldown elapsed");
    w.tick(p + 370);   // verdict deadline (155 ms)
    check(w.status() == WriteController::Status::NoAck, "terminal NoAck");
    check(w.tx_count() == 2, "exactly two frames across the whole scenario");
}

static void test_collision_detected() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm accepted");
    w.tick(1180);   // TX
    send(c, poll_req(1, "42"), 1250);   // master poll inside the verdict window
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1265);
    w.tick(1340);   // deadline with traffic before it
    check(w.status() == WriteController::Status::Collision, "terminal Collision on the first attempt");
    check(w.collision_count() == 1, "first-attempt collision counted");
    check_line_has(w.status_line(), "collision", "collision wording");
    check_line_has(w.status_line(), "result unknown", "collision also result-unknown");
    check(w.tx_count() == 1, "no retry follows a collision");

    // a re-press is a fresh, gated one-shot - not an automatic retry
    uint32_t t2 = 6000;
    slot(c, t2, 1);
    check(w.request(0x01, 0x4C, 3420, 6100), "re-press accepted");
    w.tick(6215);
    check(bus.tx_log.size() == 2, "re-press fired once gated");
    w.tick(6370);
    check(w.status() == WriteController::Status::NoAck, "second attempt verdict");
    check(w.collision_count() == 1, "clean second attempt adds no collision");
}

// reviewer round-3 status-bug table: a LATER operation with zero frames of
// its own must not inherit "sent; result unknown" wording
static void test_has_tx_reset_wordings() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);
    using S = WriteController::Status;

    // operation 1 completes WITH a frame (ack in window)
    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, 1040), "arm 1");
    w.tick(1180);   // TX
    send(c, std::string(REAL_ACK) + "\r", 1200);
    consume(c);
    check(w.status() == S::Acked, "op 1 acked");

    // operation 2: enable off BEFORE any fire - must not say "sent"
    bus.enable = false;
    check(w.request(0x02, 0x4C, 3420, 3000), "arm 2 accepted");
    w.tick(3215);
    check(w.status() == S::Aborted, "op 2 aborted pre-TX");
    check_line_has(w.status_line(), "aborted", "op 2 abort wording");
    check_line_lacks(w.status_line(), "sent", "op 2 must not claim a sent frame");
    check_line_lacks(w.status_line(), "result unknown", "op 2 must not claim result-unknown");
    bus.enable = true;

    // operation 3: expires without a window - must not append result-unknown
    uint32_t t2 = 5000;
    slot(c, t2, 1);
    check(w.request(0x03, 0x4C, 3420, 6000), "arm 3 accepted");
    // no further traffic, no fire window; watchdog trips at ~7.1 s
    c.tick(9500);
    w.tick(9600);
    check(w.status() == S::Armed || w.status() == S::Expired, "op 3 held by watchdog");
    w.tick(12200);   // 6 s after arm: expiry
    check(w.status() == S::Expired, "op 3 expired");
    check_line_has(w.status_line(), "expired", "op 3 expiry wording");
    check_line_lacks(w.status_line(), "result unknown",
                     "op 3 never sent: expiry must not inherit result-unknown");
    check(bus.tx_log.size() == 1, "only op 1 ever transmitted");
}

// enable-off aborts: pre-TX wording stays clean post-op-1; post-TX abort
// says sent-unknown - but only for the op that actually sent
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
    check_line_lacks(w.status_line(), "result unknown", "pre-TX abort wording is clean");

    // post-TX abort (this op DID send)
    bus.enable = true;
    check(w.request(0x01, 0x4C, 3420, 3000), "re-arm accepted");
    uint32_t t2 = 4000;
    slot(c, t2, 1);
    w.tick(4215);   // fresh cadence + window: fires
    check(bus.tx_log.size() == 1, "post-arm fire happened");
    bus.enable = false;
    w.tick(4230);
    check(w.status() == S::Aborted, "post-TX abort status");
    check_line_has(w.status_line(), "result unknown", "post-TX abort wording says result unknown");
}

// flush failure: counted as a TX attempt (never undercount), abort honest
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
    check(w.tx_count() == 1, "flush failure counted as a TX attempt (round-3 undercount fix)");
}

// noise-only row: no poll ever seen -> cadence gate holds -> expiry
static void test_noise_only_no_cadence() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint8_t junk[] = {0x33, 0x55};
    c.feed(junk, sizeof junk, 1000);
    check(w.request(0x01, 0x4C, 3420, 1100), "arm accepted on a never-polled bus");
    w.tick(2000);
    check(bus.tx_log.empty(), "no fire without an established poll cadence");
    check(!c.watchdog_tripped(), "watchdog genuinely quiet (no requests ever)");
    w.tick(7200);
    check(w.status() == WriteController::Status::Expired, "expired without cadence");
    check(bus.tx_log.empty(), "nothing ever sent");
}

// full state machine across the uint32 millis() wrap: TX before the wrap,
// in-window ack across the seam, second press held by cooldown, fires after
// it, terminal verdict, late-ack report - all post-0x00000000
static void test_millis_wraparound() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    send(c, poll_req(1, "42"), 0xFFFFFF00u);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 0xFFFFFF15u);
    check(w.request(0x01, 0x4C, 3420, 0xFFFFFF20u), "arm accepted before the wrap");
    w.tick(0xFFFFFFC0u);  // 171 ms window: fires 64 ms before the wrap
    check(bus.tx_log.size() == 1, "fired before the wrap");
    check(w.status() == WriteController::Status::WaitAck, "waiting across the seam");

    send(c, std::string(REAL_ACK) + "\r", 0x40);  // 160 ms elapsed: in-window
    check(w.status() == WriteController::Status::Acked, "ack accepted across the seam");
    check(w.tx_count() == 1, "still one frame");

    check(w.request(0x02, 0x4C, 3425, 0x100u), "re-arm after the wrap");
    uint32_t t = 0x600u;
    slot(c, t, 2);              // fresh pair at 0x600/0x615
    w.tick(0x705u);   // round 4: 240 ms window OPEN post-pair - cooldown (1861 ms since TX across the seam) is the only gate: HOLDS
    check(bus.tx_log.size() == 1, "cooldown-only hold across the millis wrap");
    w.tick(0x800u);   // window missed (491 ms since last RX): separately held
    check(bus.tx_log.size() == 1, "window miss holds across the wrap");
    uint32_t t2 = 0x1000u;
    slot(c, t2, 2);
    w.tick(0x1100u);  // 4416 ms since TX (cooldown over), 235 ms window
    check(bus.tx_log.size() == 2, "second press fired after the gates");
    w.tick(0x119au);  // 154 ms: verdict deadline
    check(w.status() == WriteController::Status::NoAck, "verdict after the wrap");
    send(c, std::string(REAL_ACK) + "\r", 0x1250u);   // late, inside listen
    check(w.status() == WriteController::Status::NoAck, "listen stays report-only");
    check(w.tx_count() == 2, "wrap cycle ends at two frames");
    check(c.anomalies_total() == 0, "whole wrap cycle healthy");
}

int main() {
    test_builder_byte_exact();
    test_allowlist_gate();
    test_deny_sets_status();
    test_fire_window();
    test_verdict_before_next_poll();
    test_outage_expiry();
    test_press_in_flight_ignored();
    test_burst_storm_single_frame();
    test_late_ack_reported();
    test_explicit_second_press();
    test_cooldown_only_gate();
    test_collision_detected();
    test_has_tx_reset_wordings();
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
