// Host oracle for the deye-bms-can RS485 handler state machine
// (pylontech_rs485.h) - the external gateway-feedback round, pinned
// permanently at handler level through the stub UART:
//   - feedback P2 #2: ONE checksum-valid-but-unparseable alarm response
//     must NOT drop the battery's previous valid alarms from stack totals;
//     the ONLY path to empty is the explicit 10-strike poll-alarm
//     transition in handle_alarm_failure (the reproduced failure was an
//     active pack_undervolt disappearing while the poll alarm stayed OFF).
//   - feedback P2 #3: freshness and the online mark belong to the DECODED
//     branch - repeated checksum-valid-but-unusable analog responses must
//     never keep the channel "online"; the stale clock advances only on
//     real decodes and the poll alarm still counts the failures.
//
// Build & run (from firmware/deye-site-v2/esphome):
//   clang++ -std=gnu++20 -Wall -Wextra -Itests/stubs -iquote ../../shared/esphome \
//     -Iincludes tests/test_pylontech_rs485_handler.cpp -o /tmp/test_rs485_handler \
//     && /tmp/test_rs485_handler

#define USE_MQTT
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esphome.h"   // host stub umbrella (tests/stubs/esphome.h)
#include "pylontech_rs485.h"
#include "fixtures/pylontech_frame_fixtures.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static bool seen_topic_payload(const char* suffix, const char* payload) {
    size_t n = strlen(suffix);
    for (size_t i = 0; i < g_pub_topics.size(); i++) {
        if (g_pub_topics[i].size() >= n &&
            g_pub_topics[i].compare(g_pub_topics[i].size() - n, n, suffix) == 0 &&
            g_pub_messages[i] == payload) return true;
    }
    return false;
}

static bool seen_topic_payload_contains(const char* suffix, const char* needle) {
    size_t n = strlen(suffix);
    for (size_t i = 0; i < g_pub_topics.size(); i++) {
        if (g_pub_topics[i].size() >= n &&
            g_pub_topics[i].compare(g_pub_topics[i].size() - n, n, suffix) == 0 &&
            g_pub_messages[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

static void reset_capture() {
    g_pub_messages.clear();
    g_pub_topics.clear();
}

// response frame: "~" + 12-char envelope + DATA + chksum + "\r".
// Envelope carries the expected address at [3..4] and error code "00" at
// [7..8]; data starts at [13] - exactly where the handler cuts it.
// A body of anything-but-data makes checksum validation pass while the
// data section stays unusable: the feedback's "checksum-valid but
// dataless/garbage" class.
static std::string response_frame(const std::string& data) {
    std::string body = "AB" "01" "CD" "00" "FFFF" + data;
    return "~" + body + rs485_calc_chksum(body) + "\r";
}

struct TH {
    esphome::uart::UARTComponent uart;
    PylontechRS485 h;
    TH() : h(&uart, "rs/test", 1, 1, "rs/test/status") {}
    void feed(const std::string& s) { uart.rx.insert(uart.rx.end(), s.begin(), s.end()); }
};

// one analog poll round at t: arm, TX, feed response, read it up.
// Response frames stay under 128 bytes, so two read ticks suffice.
static void run_analog_cycle(TH& th, const std::string& resp, uint32_t t) {
    g_ms = t;
    th.h.tick();              // state 0 -> 1 (analog gate passes)
    th.h.tick();              // state 1: TX request -> 2
    if (!resp.empty()) th.feed(resp);
    th.h.tick();              // read up to 64 bytes
    th.h.tick();              // rest + '\r' -> processed, back to state 0
}

// one alarm poll round at t; the caller must have just completed the
// analog cycle at the same t (state 0, analog not due, alarm gate armed)
static void run_alarm_cycle(TH& th, const std::string& resp, uint32_t t) {
    g_ms = t;
    th.h.tick();              // state 0 -> 3
    th.h.tick();              // state 3: TX alarm request -> 4
    if (!resp.empty()) th.feed(resp);
    th.h.tick();
    th.h.tick();              // process + compute_stack_totals -> state 0
}

// ---- 1. one bad alarm frame preserves the previous valid alarms -----------

static void test_bad_alarm_frame_preserves_totals() {
    TH th;
    const std::string good_alarm = response_frame(ALARM_FULL);
    const std::string bad_alarm = response_frame(std::string(80, 'G'));

    // round A: healthy analog + a full alarm frame (pack_undervolt active)
    run_analog_cycle(th, response_frame(ANALOG_FULL), 40000);
    run_alarm_cycle(th, good_alarm, 40000);
    th.h.publish_data();       // rotate 1: repair stamps + stack values land
    check(seen_topic_payload_contains("stack/alarms", "pack_undervolt"),
          "good frame: stack alarms published with the pack's active alarms");
    check(seen_topic_payload("battery0/alarm_poll_alarm", "OFF"),
          "repair pass stamps the alarm poll OFF (healthy)");

    // round B: ONE checksum-valid, unparseable alarm response
    reset_capture();
    run_analog_cycle(th, response_frame(ANALOG_FULL), 70000);
    run_alarm_cycle(th, bad_alarm, 70000);
    th.h.publish_data();       // rotate 2 (stack pass rests)
    th.h.publish_data();       // rotate 3 (stack pass runs)
    check(!seen_topic_payload("stack/alarms", ""),
          "THE PIN: one bad frame does not empty the stack alarms");
    check(seen_topic_payload_contains("battery0/alarm_poll_alarm", "OFF"),
          "the poll alarm is still OFF after a single failure (10-strike rule)");
    check(!seen_topic_payload("battery0/alarm_poll_alarm", "ON"),
          "no premature alarm poll ON");

    // rounds C: nine more consecutive bad alarm frames - the explicit
    // stale transition. Emptying stack alarms is now CORRECT, and only
    // this path can produce the empty payload.
    reset_capture();
    for (int k = 0; k < 9; k++) {
        uint32_t t = 100000 + k * 30000;
        run_analog_cycle(th, response_frame(ANALOG_FULL), t);
        run_alarm_cycle(th, bad_alarm, t);
    }
    check(seen_topic_payload("battery0/alarm_poll_alarm", "ON"),
          "ten strikes: the alarm poll alarm goes ON (explicit stale)");
    th.h.publish_data();       // rotate 4
    th.h.publish_data();       // rotate 5: stack pass with has_alarm=false
    check(seen_topic_payload("stack/alarms", ""),
          "the explicit stale transition empties the alarms (by design)");
    reset_capture();
}

// ---- 2. garbage analog responses go stale, never online -------------------

static void test_garbage_analog_goes_stale() {
    TH th;
    const std::string garbage = response_frame(std::string(80, 'G'));

    // ten checksum-valid but unusable analog responses
    for (int k = 0; k < 10; k++)
        run_analog_cycle(th, garbage, 10000 * (k + 1));
    check(seen_topic_payload("battery0/poll_alarm", "ON"),
          "ten unusable analogs trip the poll alarm");
    check(!seen_topic_payload("/status", "online"),
          "THE PIN: garbage responses never marked the channel online");
    check(!seen_topic_payload("/status", "offline"),
          "and no bogus offline claim either - the tracker simply stayed stale");

    // control: one real decode brings the channel online
    run_analog_cycle(th, response_frame(ANALOG_FULL), 110000);
    check(seen_topic_payload("/status", "online"),
          "a real decode marks the channel online");
    check(seen_topic_payload("battery0/poll_alarm", "OFF"),
          "and the recovery decode clears the poll alarm (OFF published on parse)");

    g_ms = 201000;             // 91 s of silence since the last DECODE
    th.h.check_stale();
    check(seen_topic_payload("/status", "offline"),
          "the stale clock, fed only by decodes, times the channel out");
    reset_capture();
}

int main() {
    test_bad_alarm_frame_preserves_totals();
    test_garbage_analog_goes_stale();
    if (failures == 0)
        printf("rs485 handler oracle: all scenarios passed\n");
    else
        printf("rs485 handler oracle: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
