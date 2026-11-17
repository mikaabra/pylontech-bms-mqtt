// Host oracle for the deye-bms-can gateway handler (pylontech_can.h) -
// the external gateway-feedback round, pinned permanently:
//   - feedback P2 #1: publish acceptance must gate every hysteresis
//     commit and timestamp (commit-on-acceptance, the RS485 handler's
//     #53 contract). A rejected publish advances no state, so the next
//     diagnostic pass re-sends the update - the reproduced failure was
//     an overvoltage update suppressed for up to a heartbeat period.
//   - feedback P2 #4: a half-invalid cell-extreme frame (one voltage
//     inside 2.0-4.5 V, the other garbage) must publish NOTHING - the
//     old code copied the survivor into both min and max, publishing
//     delta 0.000 = an apparently balanced battery.
//
// Build & run (from firmware/deye-site-v2/esphome):
//   clang++ -std=gnu++20 -Wall -Wextra -Itests/stubs -iquote ../../shared/esphome \
//     -Iincludes tests/test_pylontech_can.cpp -o /tmp/test_pylontech_can \
//     && /tmp/test_pylontech_can

#define USE_MQTT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esphome.h"   // host stub umbrella (tests/stubs/esphome.h)
#include "pylontech_can.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static bool seen_topic(const char* suffix) {
    size_t n = strlen(suffix);
    for (const auto& t : g_pub_topics)
        if (t.size() >= n && t.compare(t.size() - n, n, suffix) == 0) return true;
    return false;
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

static int count_topic_payload(const char* suffix, const char* payload) {
    size_t n = strlen(suffix);
    int count = 0;
    for (size_t i = 0; i < g_pub_topics.size(); i++) {
        if (g_pub_topics[i].size() >= n &&
            g_pub_topics[i].compare(g_pub_topics[i].size() - n, n, suffix) == 0 &&
            g_pub_messages[i] == payload) count++;
    }
    return count;
}

static bool last_payload_for(const char* suffix, std::string& out) {
    size_t n = strlen(suffix);
    for (size_t i = g_pub_topics.size(); i-- > 0;) {
        if (g_pub_topics[i].size() >= n &&
            g_pub_topics[i].compare(g_pub_topics[i].size() - n, n, suffix) == 0) {
            out = g_pub_messages[i];
            return true;
        }
    }
    return false;
}

static void reset_capture() {
    g_pub_messages.clear();
    g_pub_topics.clear();
}

// ---- 1. commit-on-acceptance: a refused alarm update retries -----------------
// The reproduced failure: an overvoltage/undervolt change lands while the
// MQTT pool refuses; the next diagnostic pass sent no alarm update because
// the handler had already committed the state as "published".

static void test_alarm_update_retries_after_refusal() {
    PylontechCAN can("can/test", 1, "can/test/status");

    g_ms = 1000;
    std::vector<uint8_t> f(8, 0);
    f[0] = 0x02;   // pack overvolt protect
    f[4] = 5;      // module count
    f[7] = 0;
    can.handle_0x359(f);
    check(seen_topic("/flags"), "flags published on the first frame");
    can.publish_diagnostics();
    check(seen_topic_payload("/can/prot_overvolt", "1"),
          "alarm bool published while accepting");
    can.publish_diagnostics();   // commit happens here (first pass had state!=0)
    reset_capture();

    // undervolt arrives while the pool refuses everything
    g_pub_accept = false;
    f[0] = 0x06;   // overvolt + undervolt protect
    can.handle_0x359(f);
    g_ms = 11000;
    can.publish_diagnostics();
    check(!seen_topic("/can/prot_undervolt"),
          "nothing reaches the wire while the pool refuses");

    // acceptance restored: the very next pass must deliver the update -
    // the failed publish left the state uncommitted, so it is still due
    g_pub_accept = true;
    g_ms = 20000;
    can.handle_0x359(f);   // same flags: retried because the refusal did not commit
    check(seen_topic("/flags"), "flags re-sent after acceptance restored");
    can.publish_diagnostics();
    check(seen_topic_payload("/can/prot_undervolt", "1"),
          "THE PIN: the refused alarm update is re-sent on the next pass");
    check(seen_topic_payload("/can/prot_overvolt", "1"),
          "the whole alarm group re-lands together");
    reset_capture();

    // and with nothing refused since, an identical frame must NOT re-publish
    g_ms = 30000;
    can.handle_0x359(f);
    can.publish_diagnostics();
    check(!seen_topic("/can/prot_undervolt"),
          "committed state: identical values stay quiet");
}

// ---- 2. heartbeat timestamps also gate on acceptance -----------------------
// A refused heartbeat publish must not advance the 60 s stamps either,
// or a refused heatbeat silences the value for a full period.

static void test_refused_heartbeat_does_not_advance() {
    reset_capture();
    PylontechCAN can("can/test", 1, "can/test/status");

    g_ms = 1000;
    std::vector<uint8_t> f(8, 0);
    f[0] = 0xC8; f[1] = 0x00;   // tmin = 20.0C
    f[2] = 0xC8; f[3] = 0x00;   // tmax = 20.0C
    f[4] = 0xE4; f[5] = 0x0C;   // v1 = 3300 (little-endian) = 3.300 V
    f[6] = 0x48; f[7] = 0x0D;   // v2 = 3400 (little-endian) = 3.400 V
    can.handle_0x370(f);
    check(seen_topic_payload("/ext/cell_v_delta", "0.100"),
          "valid extremes publish min/max/delta");
    reset_capture();

    // 60 s later the heartbeat is due; the pool refuses the whole group
    g_ms = 62000;
    g_pub_accept = false;
    std::vector<uint8_t> g = f;
    can.handle_0x370(g);
    check(!seen_topic("/ext/cell_v_min"), "refused: nothing on the wire");
    g_pub_accept = true;

    // 5 s later (heartbeat STILL due - the refusal did not advance the
    // stamp) the same unchanged values must re-publish
    g_ms = 67000;
    can.handle_0x370(g);
    check(seen_topic_payload("/ext/cell_v_delta", "0.100"),
          "the refused heartbeat re-fires on the next frame");
}

// ---- 3. half-invalid cell extremes publish nothing (feedback P2 #4) --------
// 3.300 V + 65.535 V: the old code published min=max=3.300, delta 0.000 -
// an apparently balanced battery. Nothing may publish until BOTH validate.

static void test_half_invalid_extreme_publishes_nothing() {
    reset_capture();
    PylontechCAN can("can/test", 1, "can/test/status");

    g_ms = 1000;
    std::vector<uint8_t> f(8, 0);
    f[0] = 0xC8; f[1] = 0x00;   // tmin = 20.0C (valid)
    f[2] = 0xC8; f[3] = 0x00;   // tmax = 20.0C (valid)
    f[4] = 0xE4; f[5] = 0x0C;   // v1 = 3.300 V (valid)
    f[6] = 0xFF; f[7] = 0xFF;   // v2 = 65.535 V (fails the 4.5 V gate)
    can.handle_0x370(f);
    check(seen_topic("/ext/temp_min"), "valid temps still publish");
    g_ms = 71000;                // heartbeats due
    can.handle_0x370(f);          // even a heartbeat must not publish cells
    check(!seen_topic("/ext/cell_v_min") && !seen_topic("/ext/cell_v_max") &&
              !seen_topic("/ext/cell_v_delta"),
          "half-invalid extremes: no cell publish ever - delta 0.000 would be"
          " a false balanced battery");

    // recovery: both valid - the pair publishes with the correct delta
    std::vector<uint8_t> g = f;
    g[6] = 0x48; g[7] = 0x0D;   // v2 = 3400 (little-endian) = 3.400 V
    g_ms = 73000;
    can.handle_0x370(g);
    check(seen_topic_payload("/ext/cell_v_min", "3.300") &&
              seen_topic_payload("/ext/cell_v_max", "3.400") &&
              seen_topic_payload("/ext/cell_v_delta", "0.100"),
          "recovered pair publishes the real min/max/delta");
    check(!seen_topic_payload("/ext/cell_v_delta", "0.000"),
          "no zero delta was ever published");

    // reviewer follow-up: a permanently invalid extreme is silent on the
    // wire (stale detection is frame-age based) - the diag error counter
    // is its only visible trace; two invalid frames were fed, so it must
    // read exactly 2 at the next diagnostic pass
    reset_capture();
    g_ms = 74000;
    can.publish_diagnostics();
    std::string ec;
    check(last_payload_for("/diag/error_count", ec) && atoi(ec.c_str()) == 2,
          "each half-invalid cell frame bumps the diag error counter");

    // same contract on the temperature gate: out-of-gate temps freeze
    // the pair silently and must count (valid cells in this frame keep
    // the cell path quiet, isolating the temp counter)
    std::vector<uint8_t> h(8, 0);
    h[0] = 0x00; h[1] = 0x80;   // t1 = int16 0x8000 = -3276.8C (out of gate)
    h[2] = 0xC8; h[3] = 0x00;   // t2 = 20.0C
    h[4] = 0xE4; h[5] = 0x0C;   // v1 = 3.300 V
    h[6] = 0x48; h[7] = 0x0D;   // v2 = 3.400 V
    g_ms = 100000;
    can.handle_0x370(h);
    reset_capture();
    g_ms = 135000;
    can.publish_diagnostics();
    check(last_payload_for("/diag/error_count", ec) && atoi(ec.c_str()) == 3,
          "an out-of-gate temperature frame bumps the error counter too");
    check(!seen_topic("/ext/temp_min"),
          "the frozen temp pair stayed silent off the wire");
}

// ---- 4. a partially-landed alarm group repairs the wire (review #112 follow-up)
// The reproduced sequence: OV ON fully published and committed; OV clears
// and its OFF lands but later group members are refused (partial group);
// OV returns - it now MATCHES the last committed group, so change
// detection sees nothing and HA keeps showing an ACTIVE alarm as OFF.
// The dirty latch retries the group at every opportunity until a full
// retry succeeds, regardless of the value's subsequent travels.

static void test_partial_alarm_group_repairs_the_wire() {
    reset_capture();
    PylontechCAN can("can/test", 1, "can/test/status");

    // 1. overvolt ON: flags + the full alarm group land and commit
    g_ms = 1000;
    std::vector<uint8_t> f(8, 0);
    f[0] = 0x02;   // pack overvolt protect
    f[4] = 5;      // module count
    f[7] = 0;
    can.handle_0x359(f);
    can.publish_diagnostics();
    can.publish_diagnostics();   // second pass lands the group (first had state!=0 too)
    check(seen_topic_payload("/can/prot_overvolt", "1"),
          "OV ON fully landed and committed");

    // 2. OV clears - partial acceptance: accept the availability stamp +
    // the group's FIRST member (the OV OFF), refuse every later member
    reset_capture();
    f[0] = 0x00;
    can.handle_0x359(f);
    g_ms = 5000;
    g_pub_refuse_after = g_pub_total + 2;   // one heartbeat stamp, then the OV OFF
    can.publish_diagnostics();
    g_pub_refuse_after = -1;
    check(seen_topic_payload("/can/prot_overvolt", "0"),
          "the OFF landed (first group member accepted)");
    check(!seen_topic_payload("/can/warn_high_voltage", "0"),
          "later group members were refused - partial group");
    check(count_topic_payload("/can/prot_overvolt", "1") == 0,
          "no full group landed during the partial attempt");

    // 3. overvolt returns: the state now MATCHES the committed group -
    // without the dirty latch nothing would publish until the heartbeat
    reset_capture();
    f[0] = 0x02;
    can.handle_0x359(f);
    g_ms = 9000;                  // far from any 60 s heartbeat
    can.publish_diagnostics();
    check(seen_topic_payload("/can/prot_overvolt", "1"),
          "THE PIN: the wire is repaired - the ON correction re-lands"
          " without waiting for the heartbeat");
    check(seen_topic_payload("/can/warn_high_voltage", "0"),
          "the refused group members land with the repair");
    check(count_topic_payload("/can/prot_overvolt", "1") == 1,
          "the corrected group landed exactly once");

    // repair committed: a further pass stays quiet
    reset_capture();
    g_ms = 15000;
    can.publish_diagnostics();
    check(count_topic_payload("/can/prot_overvolt", "1") == 0,
          "after the full repair the committed group stays quiet");
    reset_capture();
    g_pub_total = 0;
}

int main() {
    test_alarm_update_retries_after_refusal();
    test_refused_heartbeat_does_not_advance();
    test_half_invalid_extreme_publishes_nothing();
    test_partial_alarm_group_repairs_the_wire();
    if (failures == 0)
        printf("pylontech_can oracle: all scenarios passed\n");
    else
        printf("pylontech_can oracle: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
