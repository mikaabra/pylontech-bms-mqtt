// Host oracle for the bms-link-sniffer guarded write client (part 2,
// issue #59): write-frame builder, allowlist gate, and the gap-injection
// state machine - all against a faked clock and recorded/played traffic,
// fully deterministic, no wall-clock anywhere.
//
// Byte-exactness policy: the builder must reproduce BOTH captured write
// frames (tests/fixtures/bms_link_fixtures.h) - the tool-session write and
// the executed in-stack write - which pins LENID 8008 and the checksum
// formula to real wire evidence, not to my arithmetic.
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

// one normal 624 ms slot: request, response 15 ms later
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
    // reproducing the captured tool-session frame == builder correctness
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

static void test_gap_inject_ack_success() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    for (int i = 0; i < 3; i++) slot(c, t, 1);  // t ends at 2872+624=2972? track: starts 1000

    // arm in the idle window after the last response: response ended at
    // t-624+15; the next slot starts at t. Everything in between is idle.
    check(w.request(0x01, 0x4C, 3430, t), "arm accepted");
    check(w.status() == WriteController::Status::Armed, "status Armed");

    // mid-idle: silence since (t-624+15) has exceeded 150 ms
    w.tick(t + 200);
    check(w.status() == WriteController::Status::WaitAck, "fired into gap, waiting ack");
    check(bus.tx_log.size() == 1, "exactly one frame transmitted");
    check_str(bus.tx_log[0], std::string(REAL_WRITE_INSTACK) + "\r", "transmitted frame is byte-exact");

    // ack arrives 15 ms later
    send(c, std::string(REAL_ACK) + "\r", t + 215);
    auto ev = c.drain_events();
    check(w.status() == WriteController::Status::Acked, "status Acked");
    check(has_event(ev, EventKind::AckForWrite), "AckForWrite event, not anomaly");
    check(c.anomalies_total() == 0, "healthy injection adds zero anomalies");
    check(bus.tx_log.size() == 1, "one-shot: no second transmit");

    // and the next slots continue normally (clock moves forward)
    t += 1200;
    slot(c, t, 2);
    check(c.frames_total() == 9, "frames counted through injection");
}

static void test_gap_not_confirmed_holds_fire() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);  // request 1000, response 1015
    check(w.request(0x01, 0x4C, 3420, 1030), "arm accepted");
    // tick while the silence since the response is only ~55 ms old
    w.tick(1070);
    check(w.status() == WriteController::Status::Armed, "no fire inside 150 ms window");
    check(bus.tx_log.empty(), "nothing sent early");
}

static void test_no_ack_single_retry_rules() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    for (int i = 0; i < 2; i++) slot(c, t, 1);   // last slot ended at t-624+... track

    // attempt 1: armed late in the idle window so we control the deadline
    check(w.request(0x01, 0x4C, 3420, t), "arm accepted");
    uint32_t last_slot_end = t + 200;
    w.tick(last_slot_end + 400);  // beyond 150ms silence but... see below
    // silence since last activity (t-624+15) is >150ms at last_slot_end+400? (t+200+400 vs t-609)
    check(bus.tx_log.size() == 1, "attempt 1 transmitted");
    check(w.status() == WriteController::Status::WaitAck, "waiting ack");

    // deadline passes with NO traffic at all -> NoAck verdict -> retry armed
    w.tick(last_slot_end + 400 + 131);
    check(w.status() == WriteController::Status::Armed, "retry armed after no-ack");
    check(w.tx_count() == 1, "one attempt so far");

    // retry holds fire until a clean pair is observed
    w.tick(last_slot_end + 1300);
    check(bus.tx_log.size() == 1, "retry held before clean slot");
    // bus resumes: one clean pair
    uint32_t t2 = last_slot_end + 1400;
    slot(c, t2, 2);
    // now a gap again -> retry fires
    w.tick(t2 + 200);
    check(bus.tx_log.size() == 2, "retry fired after clean slot");
    check(w.status() == WriteController::Status::WaitAck, "retry waiting ack");
    // second deadline passes with no traffic -> terminal NoAck, disarmed
    w.tick(t2 + 200 + 131);
    check(w.status() == WriteController::Status::NoAck, "terminal NoAck after retry");
    check(bus.tx_log.size() == 2, "no third attempt: initial + single retry only");
    w.tick(t2 + 2000);
    check(bus.tx_log.size() == 2, "terminal: no further fire without new one-shot");
}

static void test_collision_detected() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);  // request 1000, response 1015
    check(w.request(0x01, 0x4C, 3420, 1624), "arm accepted");
    w.tick(1824);
    check(bus.tx_log.size() == 1, "attempt fired");

    // the master's next poll arrives INSIDE our ack window (not the ack)
    send(c, poll_req(1, "42"), 1884);

    // deadline passes → retry armed, clean slot required
    w.tick(1955);
    check(w.status() == WriteController::Status::Armed,
          "retry-armed after traffic in window");
    check(w.tx_count() == 1, "one attempt so far");

    // the collided slot's response completed BEFORE the failure was
    // recorded, so it must NOT clear the clean-slot hold
    send(c, std::string(REAL_42_RESPONSE) + "\r", 1899);
    w.tick(2060);
    check(bus.tx_log.size() == 1, "retry held: clean slot must postdate the failure");

    // a full clean slot AFTER the failed attempt clears the hold
    send(c, poll_req(2, "42"), 2400);
    send(c, std::string(REAL_42_RESPONSE) + "\r", 2415);
    w.tick(2600);  // silence since 2415 is 185 ms → retry fires
    check(bus.tx_log.size() == 2, "retry fired after clean slot");
    check(w.status() == WriteController::Status::WaitAck, "retry waiting ack");

    // the second window also sees the master's poll inside → terminal Collision
    send(c, poll_req(1, "42"), 2660);
    w.tick(2731);
    check(w.status() == WriteController::Status::Collision, "terminal Collision");
    check(w.collision_count() == 1, "collision counted");
    check(bus.tx_log.size() == 2, "no attempt beyond initial + single retry");
}

static void test_enable_off_aborts() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, t), "arm accepted");

    // before firing
    bus.enable = false;
    w.tick(t + 200);
    check(bus.tx_log.empty(), "never fired with enable off");
    check(w.status() == WriteController::Status::Aborted, "aborted before fire");

    // mid-wait abort
    bus.enable = true;
    check(w.request(0x01, 0x4C, 3420, t), "re-arm accepted");
    w.tick(t + 201);
    check(w.status() == WriteController::Status::WaitAck, "fired");
    bus.enable = false;
    w.tick(t + 300);
    check(w.status() == WriteController::Status::Aborted, "aborted mid-wait");
}

static void test_uart_fail_aborts() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, t), "arm accepted");
    bus.tx_ok = false;
    w.tick(t + 200);
    check(bus.tx_log.empty(), "uart failure: nothing recorded as sent");
    check(w.status() == WriteController::Status::Aborted, "aborted on uart failure");
}

static void test_watchdog_holds_fire() {
    BmsLinkCore c;
    FakeBus bus;
    WriteController w(c, FakeBus::enable_cb, FakeBus::tx_cb, &bus);
    c.set_ack_waiter(&w);

    uint32_t t = 1000;
    slot(c, t, 1);
    check(w.request(0x01, 0x4C, 3420, t), "arm accepted");
    // let the bus go silent long enough to trip the watchdog
    c.tick(t + 2200);
    check(c.watchdog_tripped(), "watchdog tripped");
    w.tick(t + 2600);
    check(bus.tx_log.empty(), "no injection while watchdog tripped");
    check(w.status() == WriteController::Status::Armed, "still armed, patient");
    // bus recovers, silence confirmed, then it fires
    send(c, poll_req(1, "42"), t + 2800);
    send(c, std::string(REAL_42_RESPONSE) + "\r", t + 2815);
    w.tick(t + 3100);
    check(!c.watchdog_tripped(), "watchdog recovered");
    w.tick(t + 3500);
    check(bus.tx_log.size() == 1, "fired after recovery + silence");
}

int main() {
    test_builder_byte_exact();
    test_allowlist_gate();
    test_deny_sets_status();
    test_gap_inject_ack_success();
    test_gap_not_confirmed_holds_fire();
    test_no_ack_single_retry_rules();
    test_collision_detected();
    test_enable_off_aborts();
    test_uart_fail_aborts();
    test_watchdog_holds_fire();

    if (failures == 0) {
        printf("test_bms_link_sniffer_inject: ALL PASS\n");
        return 0;
    }
    printf("test_bms_link_sniffer_inject: %d FAILURES\n", failures);
    return 1;
}
