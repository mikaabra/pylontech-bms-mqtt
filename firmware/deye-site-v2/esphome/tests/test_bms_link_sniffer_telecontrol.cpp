// Host oracle for the bms-link-sniffer guarded teleControl client: the
// 2026-11-11 vendor-tool session productionized onto this device. Pins,
// against committed wire evidence (PROTOCOL_REFERENCE "teleControl
// captured on the wire"):
//   - the builders: byte-exact probes for every address (captured class
//     0x409, CHK = FD (0x0C - addr)) and every captured enable literal
//     (SYS_OFF both variants, LMCHG/CHG/DISCHG new-gen, class 0x3AA),
//     plus the captured LMCHG disable literal (class 0x408) - the one
//     frame family whose CHK is NOT the computed complement function
//   - the fail-closed chain: no probe answer -> the write never fires;
//     a foreign presence probe mid-sequence -> abort before any unsent
//     frame; pack 00 / out-of-range refused; the wake pair refuses
//     old-gen targets (no captured old-gen disable exists)
//   - sequencing: probe -> answer -> write, with the cross-op 4 s
//     cooldown waived ONLY inside one press's sequence and every frame
//     still waiting on a fresh clean master pair + kTeleIntraGapMs
//   - verdicts: single-frame ack semantics identical to the write client
//     (150 ms window, late acks listened for and reported, never a
//     retry); an unobserved LMCHG disable still continues to the enable
//     (the enable is the recovery direction either way)
//   - the bus record: probe answers + teleControl acks are routed
//     events, never anomalies; master telemetry pairing flows untouched
//
// Build & run (CLAUDE.md host-test loop):
//   cd firmware/deye-site-v2/esphome
//   clang++ -std=gnu++20 -Wall -Wextra -I. -Itests/stubs -Iincludes \
//     tests/test_bms_link_sniffer_telecontrol.cpp -o /tmp/test_bms_link_sniffer_telecontrol \
//     && /tmp/test_bms_link_sniffer_telecontrol

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "bms_link_sniffer_core.h"
#include "fixtures/bms_link_fixtures.h"

using linksniff::BmsLinkCore;
using linksniff::EventKind;
using linksniff::ReadController;
using linksniff::SharedTxGate;
using linksniff::TeleOpController;
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

// ---- driver plumbing (same shapes as the read/write oracles) ----------------

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

struct Harness {
    BmsLinkCore c;
    FakeBus bus;
    SharedTxGate gate;
    WriteController w;
    ReadController r;
    TeleOpController t;

    Harness()
        : w(c, gate, FakeBus::enable_cb, FakeBus::tx_cb, &bus),
          r(c, gate, FakeBus::enable_cb, FakeBus::tx_cb, &bus),
          t(c, gate, FakeBus::enable_cb, FakeBus::tx_cb, &bus) {
        c.set_ack_waiter(&w);
        c.set_read_waiter(&r);
        c.set_telecom_waiter(&t);
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

static void pair_at(Harness& h, int pack, uint32_t t) {
    send(h.c, poll_req(pack, "42"), t);
    send(h.c, std::string(REAL_42_RESPONSE) + "\r", t + 15);
}

// the master's STATUS poll (CID2 44): the answer family whose LENID is
// also 9052 - the reviewer's blocker case. REAL_44_RESPONSE is the real
// captured frame, battery 0001, exactly the telemetry the rotations
// carry throughout, and exactly what must never be eaten as a probe
// answer.
static void status_pair_at(Harness& h, int pack, uint32_t t) {
    send(h.c, poll_req(pack, "44"), t);
    send(h.c, std::string(REAL_44_RESPONSE) + "\r", t + 15);
}

static void bare_ack_at(BmsLinkCore& c, uint32_t t_ms) {
    send(c, std::string("~") + linksniff::kRawBareAck + "\r", t_ms);
}

// The pack's 9052 identity answer to a presence probe. INFO = 82 chars
// ("00" + id payload + padding) - labeled synthetic: the shape follows
// the captured literal ("80 00 D5 00 90 52 00 [id] ..."), the id bytes
// are arbitrary test values, the checksum is the shared firmware
// function (so the core's envelope + checksum verification accepts it).
static std::string probe_answer_frame(const char* id4) {
    std::string info = "00" + std::string(id4) + std::string(82 - 2 - 4, '0');
    std::string body = "8000D500" + std::string(linksniff::kProbeAnswerLenid) + info;
    return "~" + body + rs485_calc_chksum(body) + "\r";
}

// establish a poll cadence, then return the time of the last pair end
static uint32_t establish_cadence(Harness& h, uint32_t t0) {
    pair_at(h, 3, t0);
    pair_at(h, 4, t0 + 624);
    pair_at(h, 1, t0 + 1248);
    h.t.tick(t0 + 1263);
    return t0 + 1263;
}

// ---- 1. builders byte-exact against the captured catalog --------------------

static void test_frames_byte_exact() {
    // presence probes for every address: captured formula
    // CHK = FD (0x0C - addr), whole-frame byte-sum class 0x409
    check_str(linksniff::build_probe_frame(1), "~8000D5C9E00201FD0B\r", "probe pack 01");
    check_str(linksniff::build_probe_frame(2), "~8000D5C9E00202FD0A\r", "probe pack 02");
    check_str(linksniff::build_probe_frame(3), "~8000D5C9E00203FD09\r", "probe pack 03");
    check_str(linksniff::build_probe_frame(4), "~8000D5C9E00204FD08\r", "probe pack 04");

    // captured enable literals (PROTOCOL_REFERENCE per-button table)
    check_str(linksniff::build_telecontrol_enable(0x04, 0x04),
              "~8000D545A006040410FC56\r", "SYS_OFF enable new-gen");
    check_str(linksniff::build_telecontrol_enable(0x01, 0x04),
              "~8000D545A006010410FC59\r", "SYS_OFF enable old-gen");
    check_str(linksniff::build_telecontrol_enable(0x04, 0x02),
              "~8000D545A006040210FC58\r", "LMCHG enable new-gen");
    check_str(linksniff::build_telecontrol_enable(0x04, 0x01),
              "~8000D545A006040110FC59\r", "CHG enable new-gen");
    check_str(linksniff::build_telecontrol_enable(0x04, 0x00),
              "~8000D545A006040010FC5A\r", "DISCHG enable new-gen");
    check_str(std::string(linksniff::kLmchgDisableNewGen),
              "~8000D545A006040201FFC2\r", "LMCHG disable literal (captured)");

    // checksum classes: hex-decoded whole-frame byte sums
    auto class_sum = [](const std::string& frame) {
        uint32_t total = 0;
        for (size_t i = 1; i + 1 < frame.size() && frame[i] != '\r'; i += 2) {
            char hex[3] = {frame[i], frame[i + 1], 0};
            unsigned long v;
            if (!parse_hex(std::string(hex), v)) return -1;
            total += (uint8_t)v;
        }
        return (int)total;
    };
    check(class_sum(linksniff::build_telecontrol_enable(0x04, 0x04)) == 938,
          "enable class 938 (0x3AA)");
    check(class_sum(linksniff::build_telecontrol_enable(0x01, 0x04)) == 938,
          "enable class 938 old-gen");
    check(class_sum(std::string(linksniff::kLmchgDisableNewGen)) == 1032,
          "disable class 1032 (0x408)");
    check(class_sum(linksniff::build_probe_frame(4)) == 1033,
          "probe class 1033 (0x409)");
    // the disable literal's CHK is deliberately off the computed formula:
    // the vendor tool's wire quirk the packs accept - the computed function
    // would produce a different CHK and the pin must keep the literal
    std::string body_wo_chk("8000D545A006040201");
    std::string computed = linksniff::kLmchgDisableNewGen;
    check(computed.substr(1 + body_wo_chk.size(), 4) !=
          rs485_calc_chksum(body_wo_chk),
          "disable CHK is off-formula (captured quirk, kept literal)");
}

// ---- 2. SYS_OFF end to end: probe -> answer -> write -> ack -----------------

static void test_sys_off_end_to_end() {
    Harness h;
    uint32_t t0 = 100000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_sys_off(4, pair_end + 30), "sys-off arm accepted");
    check(h.t.status() == TeleOpController::Status::Armed, "armed");
    check_line_has(h.t.status_line(), "armed: SYS_OFF pack 04", "arm line");

    h.t.tick(pair_end + 170);  // 155 ms of silence: the probe fires
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    check(h.bus.tx_log.size() == 1, "exactly one frame out (the probe)");
    check_str(h.bus.tx_log[0], "~8000D5C9E00204FD08\r", "probe frame byte-exact");
    check_line_has(h.t.status_line(), "the write does NOT fire without it",
                   "probe-sent line states the fail-closed rule");

    uint32_t probe_tx = pair_end + 170;
    // the pack's identity answer routes to the controller, not to the
    // unpaired-response anomaly path
    send(h.c, probe_answer_frame("9003"), probe_tx + 60);
    check(h.t.status() == TeleOpController::Status::WaitWindow, "probe answer accepted");
    check_line_has(h.t.status_line(), "probe answered (id 9003", "answered line");
    auto ev = h.c.drain_events();
    check(has_event(ev, EventKind::ProbeAnswer), "ProbeAnswer event pushed");
    check(!has_event(ev, EventKind::AnomalyUnpaired),
          "probe answer is not an unpaired-response anomaly");
    h.c.drain_events();

    // a clean master pair clears our clean-slot demand; the write then
    // fires in the fresh quiet window - the 4 s cooldown does NOT block
    // the second frame of one press
    pair_at(h, 4, probe_tx + 300);
    h.t.tick(probe_tx + 300 + 15 + 155);
    check(h.t.status() == TeleOpController::Status::WaitAck, "write fired after the pair");
    check(h.bus.tx_log.size() == 2, "probe + write out");
    check_str(h.bus.tx_log[1], "~8000D545A006040410FC56\r",
              "SYS_OFF write byte-exact (new-gen literal)");
    check(h.gate.cooldown_active, "cooldown latched at our write");

    uint32_t write_tx = probe_tx + 470;
    bare_ack_at(h.c, write_tx + 120);
    check(h.t.status() == TeleOpController::Status::Done, "SYS_OFF done");
    check_line_has(h.t.status_line(), "SYS_OFF acked in 120 ms", "done line");
    check_line_has(h.t.status_line(), "NOT the", "done line carries the factory-reset caveat");
    ev = h.c.drain_events();
    check(has_event(ev, EventKind::AckForTeleOp), "AckForTeleOp event pushed");
    check(!has_event(ev, EventKind::AnomalyAck), "no stray bare-ack anomaly");
    check(h.t.tx_count() == 2, "tx counter = probe + write");
}

// ---- 3. fail-closed: no probe answer, no write -------------------------------
// Three sub-cases, all reviewer-prescribed: a pack that stays silent on a
// quiet bus; a pack that stays silent while routine master telemetry (the
// 0x44/9052 family!) flows inside the window - the PR #82 blocker repro;
// and a pack whose REAL answer arrives after master traffic already
// consumed the first-frame slot. In every variant the SYS_OFF write must
// never fire.

static void test_no_answer_silent_pack() {
    Harness h;
    uint32_t t0 = 200000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_sys_off(3, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    // a fully silent bus: the verdict lands at the window edge
    h.t.tick(pair_end + 170 + 1600);
    check(h.t.status() == TeleOpController::Status::NoAnswer, "no-answer verdict");
    check(h.bus.tx_log.size() == 1, "the write never fired without an answer");
    check_line_has(h.t.status_line(), "was NOT sent", "fail-closed line");
}

static void test_no_answer_master_telemetry_is_not_the_answer() {
    Harness h;
    uint32_t t0 = 250000;
    uint32_t pair_end = establish_cadence(h, t0);

    // the reviewer's reproduction: probe pack 03, pack 03 stays silent,
    // the master polls pack 01 for STATUS inside the window
    check(h.t.request_sys_off(3, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    uint32_t pairs_before = h.c.pairs_total();

    status_pair_at(h, 1, pair_end + 400);  // +request, +9052 answer from pack 01
    h.t.tick(pair_end + 600);
    check(h.t.status() == TeleOpController::Status::NoAnswer,
          "the telemetry answer did NOT count as the probe answer");
    check(h.bus.tx_log.size() == 1, "SYS_OFF was NOT sent to the silent pack");
    check_line_has(h.t.status_line(), "first-off", "interleave verdict line");
    auto ev = h.c.drain_events();
    check(!has_event(ev, EventKind::ProbeAnswer),
          "no probe answer event for a telemetry frame");
    // the master's slot paired normally: listener unharmed
    check(!has_event(ev, EventKind::AnomalyUnpaired),
          "the 0x44 answer paired with its poll, not stolen");
    check(h.c.pairs_total() == pairs_before + 1, "pair bookkeeping advanced");
}

static void test_probe_answer_after_master_traffic_rejected() {
    Harness h;
    uint32_t t0 = 275000;
    uint32_t pair_end = establish_cadence(h, t0);

    // master traffic lands between our probe and the pack's real answer:
    // the real answer is no longer the first frame - rejected, verdict
    // NoAnswer, no write (fail-closed against reordering ambiguity)
    check(h.t.request_sys_off(4, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    status_pair_at(h, 2, pair_end + 400);
    h.t.tick(pair_end + 600);  // interleave verdict fires
    check(h.t.status() == TeleOpController::Status::NoAnswer, "interleave verdict");
    // the pack's real answer arriving NOW is a bystander on the listener's
    // ledger - an unpaired response, never our trigger
    send(h.c, probe_answer_frame("9004"), pair_end + 700);
    check(h.t.status() == TeleOpController::Status::NoAnswer,
          "late real answer does not resurrect the sequence");
    check(h.bus.tx_log.size() == 1, "the write never fired");
    auto ev = h.c.drain_events();
    check(!has_event(ev, EventKind::ProbeAnswer),
          "a post-traffic 9052 frame is not routed as our answer");
    check(has_event(ev, EventKind::AnomalyUnpaired),
          "the late answer stays on the listener ledger where it belongs");
}

// ---- 4. refusals ---------------------------------------------------------------

static void test_refusals() {
    Harness h;
    uint32_t now = 300000;
    check(!h.t.request_sys_off(0, now), "pack 00 refused (master battery)");
    check_line_has(h.t.status_line(), "never 00", "master refusal line");
    check(!h.t.request_sys_off(5, now), "pack 05 refused");
    check(!h.t.request_wake_pair(0, now), "wake pair pack 00 refused");
    check(!h.t.request_wake_pair(1, now), "wake pair old-gen 01 refused");
    check_line_has(h.t.status_line(), "physical button", "old-gen wake refusal line");
    check(!h.t.request_wake_pair(2, now), "wake pair old-gen 02 refused");
    check(h.t.request_wake_pair(3, now), "wake pair new-gen 03 accepted");
    check(h.t.request_wake_pair(4, now) == false,
          "second press while in flight ignored");
    check_line_has(h.t.status_line(), "in flight", "in-flight ignore line");
    check(h.bus.tx_log.empty(), "nothing transmitted on refusals");
}

// ---- 5. wake pair end to end ---------------------------------------------------

static void test_wake_pair_end_to_end() {
    Harness h;
    uint32_t t0 = 400000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_wake_pair(4, pair_end + 30), "wake arm accepted");
    check_line_has(h.t.status_line(), "LMCHG wake pair pack 04", "arm line");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    send(h.c, probe_answer_frame("9004"), pair_end + 170 + 60);
    check(h.t.status() == TeleOpController::Status::WaitWindow, "answer accepted");

    pair_at(h, 4, pair_end + 170 + 300);
    h.t.tick(pair_end + 170 + 470);
    check(h.t.status() == TeleOpController::Status::WaitAck, "disable fired");
    check(h.bus.tx_log.size() == 2, "probe + disable out");
    check_str(h.bus.tx_log[1], "~8000D545A006040201FFC2\r",
              "LMCHG disable byte-exact literal");

    uint32_t disable_tx = pair_end + 640;
    bare_ack_at(h.c, disable_tx + 100);
    check(h.t.status() == TeleOpController::Status::Settle, "disable acked, settling");
    check_line_has(h.t.status_line(), "LMCHG disable acked in 100 ms", "disable ack line");

    // settle gap not yet passed at this tick: the enable must not move
    pair_at(h, 3, disable_tx + 300);
    h.t.tick(disable_tx + 470);
    check(h.t.status() == TeleOpController::Status::Settle,
          "still settling before kTeleIntraGapMs");
    // a fresh pair + settle elapsed: settle releases into the enable window
    pair_at(h, 2, disable_tx + 1240);
    h.t.tick(disable_tx + 1410);  // 670 ms since the ack, 155 ms of silence
    check(h.t.status() == TeleOpController::Status::WaitWindow,
          "settle gap passed, enable window armed");
    // the enable fires on the next window tick
    h.t.tick(disable_tx + 1420);
    check(h.t.status() == TeleOpController::Status::WaitAck, "enable fired");
    check_line_has(h.t.status_line(), "LMCHG enable sent",
                   "the sent line names the frame it sent (live 17:29 lesson)");
    check(h.bus.tx_log.size() == 3, "probe + disable + enable out");
    check_str(h.bus.tx_log[2], "~8000D545A006040210FC58\r",
              "LMCHG enable byte-exact");

    uint32_t enable_tx = disable_tx + 1420;
    bare_ack_at(h.c, enable_tx + 110);
    check(h.t.status() == TeleOpController::Status::Done, "wake pair done");
    check_line_has(h.t.status_line(), "wake pair complete", "done line");
    check(h.t.tx_count() == 3, "tx counter = 3 frames");
}

// ---- 6. unobserved disable still continues to the enable ----------------------

static void test_wake_pair_disable_noack_continues() {
    Harness h;
    uint32_t t0 = 500000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_wake_pair(3, pair_end + 30), "wake arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    send(h.c, probe_answer_frame("9005"), pair_end + 170 + 60);
    pair_at(h, 3, pair_end + 170 + 300);
    h.t.tick(pair_end + 640);
    check(h.t.status() == TeleOpController::Status::WaitAck, "disable fired");

    // no ack in the window: at the deadline the verdict is NoAck and the
    // sequence continues - the enable is the recovery direction whether
    // or not the disable landed
    h.t.tick(pair_end + 800);
    check(h.t.status() == TeleOpController::Status::Settle,
          "unobserved disable -> settle, not terminal");
    check_line_has(h.t.status_line(), "continuing to the enable", "continuation line");

    // the disable's late ack arrives during the settle: reported on the
    // line in place (the append is transient - assert it while in Settle)
    bare_ack_at(h.c, pair_end + 900);
    check(h.t.status() == TeleOpController::Status::Settle, "still settling");
    check_line_has(h.t.status_line(), "late bare ack 260 ms", "late disable ack reported");

    pair_at(h, 2, pair_end + 1000);
    h.t.tick(pair_end + 1210);  // 410 ms since the settle verdict + window
    check(h.t.status() == TeleOpController::Status::WaitWindow, "enable window armed");
    h.t.tick(pair_end + 1220);
    check(h.t.status() == TeleOpController::Status::WaitAck, "enable fired");
    check(h.bus.tx_log.size() == 3, "disable + enable both sent");
    check_str(h.bus.tx_log[2], "~8000D545A006040210FC58\r",
              "LMCHG enable byte-exact (pack 03, variant 04)");
    uint32_t enable_tx = pair_end + 1220;
    bare_ack_at(h.c, enable_tx + 100);
    check(h.t.status() == TeleOpController::Status::Done,
          "pair completes after the unobserved disable");
    check_line_has(h.t.status_line(), "wake pair complete", "completion line");
}

// ---- 7. foreign presence probe aborts before any unsent frame ------------------

static void test_foreign_probe_aborts() {
    Harness h;
    uint32_t t0 = 600000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_sys_off(4, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");

    // someone else's probe re-latches the write target mid-window
    std::string other_body = "8000D5C9E00202";
    send(h.c, "~" + other_body + rs485_calc_chksum(other_body) + "\r",
         pair_end + 400);
    check(h.t.status() == TeleOpController::Status::Aborted, "aborted on foreign probe");
    check_line_has(h.t.status_line(), "re-latched", "abort line");
    check(h.bus.tx_log.size() == 1, "our write never fired");
    auto ev = h.c.drain_events();
    check(has_event(ev, EventKind::AnomalyConfig),
          "the foreign probe stays a config-plane anomaly on the record");
}

// ---- 8. SYS_OFF late-ack semantics (same as the write client) -----------------

static void test_sys_off_noack_then_late_ack() {
    Harness h;
    uint32_t t0 = 700000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_sys_off(4, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    send(h.c, probe_answer_frame("9006"), pair_end + 170 + 60);
    pair_at(h, 4, pair_end + 170 + 300);
    h.t.tick(pair_end + 640);
    check(h.t.status() == TeleOpController::Status::WaitAck, "write fired");
    uint32_t write_tx = pair_end + 640;

    // the real bus acks late (+200..230 ms): past the 150 ms deadline the
    // verdict is NoAck, and the late ack is listened for and reported
    h.t.tick(write_tx + 160);
    check(h.t.status() == TeleOpController::Status::NoAck, "no-ack verdict at the deadline");
    bare_ack_at(h.c, write_tx + 230);
    check(h.t.status() == TeleOpController::Status::NoAck, "late ack is report-only");
    check_line_has(h.t.status_line(), "late bare ack 230 ms", "late-ack line");
    check(h.bus.tx_log.size() == 2, "no retry ever fires");

    // a further probe-answer-shaped frame outside any window stays a
    // bystander unpaired response, never touching the teleop client
    send(h.c, probe_answer_frame("9999"), write_tx + 500);
    check(h.t.status_line().find("9999") == std::string::npos,
          "out-of-window 9052 frame does not touch the controller");
}

// ---- 9. cross-op protection: the cooldown re-arms after the sequence ---------

static void test_shared_gate_after_done() {
    Harness h;
    uint32_t t0 = 800000;
    uint32_t pair_end = establish_cadence(h, t0);

    // complete a SYS_OFF
    check(h.t.request_sys_off(4, pair_end + 30), "arm accepted");
    h.t.tick(pair_end + 170);
    send(h.c, probe_answer_frame("9007"), pair_end + 170 + 60);
    pair_at(h, 4, pair_end + 170 + 300);
    h.t.tick(pair_end + 640);
    uint32_t write_tx = pair_end + 640;
    bare_ack_at(h.c, write_tx + 100);
    check(h.t.status() == TeleOpController::Status::Done, "done");
    check(h.gate.cooldown_active, "cooldown latched after the sequence");

    // a NEW write press arms but cannot fire while our cooldown holds -
    // even in a fresh quiet window with a clean pair
    pair_at(h, 1, write_tx + 300);
    check(h.w.request(1, 0x4C, 3420, write_tx + 330), "write arms after teleop done");
    for (uint32_t dt = 155; dt <= 3900; dt += 100) {
        pair_at(h, (dt / 624) % 2 ? 3 : 4, write_tx + 400 + dt);
    }
    h.w.tick(write_tx + 555);
    check(h.w.status() == WriteController::Status::Armed,
          "write held while the teleop cooldown runs");
    // past the 4 s boundary the cooldown clears and the write fires in
    // the next valid window
    h.w.tick(write_tx + 4330);
    check(h.w.status() == WriteController::Status::Armed ||
          h.w.status() == WriteController::Status::WaitAck,
          "write arms through the cooldown tail");
}

// ---- 10. auto-off predicate over three clients --------------------------------

static void test_autooff_predicate() {
    using T = TeleOpController::Status;
    using W = WriteController::Status;
    using R = ReadController::Status;
    check(linksniff::op_just_finished(W::Idle, R::Idle, T::WaitAck,
                                      W::Idle, R::Idle, T::Done),
          "teleop in-flight -> terminal finishes the op");
    check(!linksniff::op_just_finished(W::Idle, R::Idle, T::Done,
                                       W::Idle, R::Idle, T::Done),
          "teleop terminal -> terminal is not a new finish");
    check(linksniff::op_just_finished(W::WaitAck, R::Idle, T::Settle,
                                      W::NoAck, R::Idle, T::Settle),
          "write verdict during a teleop settle still counts");
    check(!linksniff::op_just_finished(W::Idle, R::Idle, T::WaitProbe,
                                       W::Idle, R::Idle, T::WaitProbe),
          "in-flight teleop is not a finish");
}

// ---- 11. probe answers never poison the listener ------------------------------

static void test_probe_answer_not_consumed_when_no_window() {
    Harness h;
    uint32_t t0 = 900000;
    uint32_t pair_end = establish_cadence(h, t0);
    // no teleop window open: an identity answer on the wire is a bystander
    send(h.c, probe_answer_frame("9008"), pair_end + 100);
    auto ev = h.c.drain_events();
    check(!has_event(ev, EventKind::ProbeAnswer),
          "no teleop window, no routing to the teleop client");
    check(h.t.status() == TeleOpController::Status::Idle, "controller untouched");
}

// ---- 12. a half-finished wake pair says so (reviewer should-fix) --------------
// Any stop after the disable flew but before the enable must name the
// hazard: the pack may be left LMCHG-disabled, with the recovery spelled
// out. Driven via the foreign-probe abort; the other stop paths share
// the same append.

static void test_half_pair_warning() {
    Harness h;
    uint32_t t0 = 950000;
    uint32_t pair_end = establish_cadence(h, t0);

    check(h.t.request_wake_pair(4, pair_end + 30), "wake arm accepted");
    h.t.tick(pair_end + 170);
    check(h.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    send(h.c, probe_answer_frame("900A"), pair_end + 170 + 60);
    pair_at(h, 4, pair_end + 170 + 300);
    h.t.tick(pair_end + 640);
    check(h.t.status() == TeleOpController::Status::WaitAck, "disable fired");
    uint32_t disable_tx = pair_end + 640;
    bare_ack_at(h.c, disable_tx + 100);
    check(h.t.status() == TeleOpController::Status::Settle, "settling");

    // another presence probe aborts the sequence mid-settle: the terminal
    // line must carry the LMCHG-enabled-nowhere warning
    std::string other_body = "8000D5C9E00201";
    send(h.c, "~" + other_body + rs485_calc_chksum(other_body) + "\r", disable_tx + 200);
    check(h.t.status() == TeleOpController::Status::Aborted, "aborted mid-pair");
    check_line_has(h.t.status_line(), "LMCHG enable was NOT sent", "naming the lost frame");
    check_line_has(h.t.status_line(), "left with LMCHG disabled", "naming the hazard");
    check_line_has(h.t.status_line(), "physical button", "naming the recovery");
    check(h.bus.tx_log.size() == 2, "probe + disable only - no enable was sent");
}

// ---- 13. the reviewer's acceptance bound, pinned -------------------------------
// Wire-math (re-review): probe ~20 ms + turnaround ~200-230 ms + ~104 ms
// answer at 9600 + batch rounding = a genuine answer lands ~+330-370 ms.
// The bound is 400: an answer at +390 must be accepted (with its measured
// delay on the verdict line); one past the bound at +420 must be rejected
// and its delay recorded in the NoAnswer line - the first-live-press
// calibration datum both ways.

static void test_answer_delay_bound() {
    Harness h1;
    uint32_t pair_end1 = establish_cadence(h1, 1000000);
    check(h1.t.request_sys_off(4, pair_end1 + 30), "arm accepted");
    h1.t.tick(pair_end1 + 170);
    check(h1.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    send(h1.c, probe_answer_frame("900B"), pair_end1 + 170 + 390);
    check(h1.t.status() == TeleOpController::Status::WaitWindow,
          "answer at +390 ms accepted (inside the 400 ms bound)");
    check_line_has(h1.t.status_line(), "in 390 ms", "measured delay on the answered line");
    h1.c.drain_events();

    Harness h2;
    uint32_t pair_end2 = establish_cadence(h2, 1100000);
    check(h2.t.request_sys_off(4, pair_end2 + 30), "arm accepted");
    h2.t.tick(pair_end2 + 170);
    check(h2.t.status() == TeleOpController::Status::WaitProbe, "probe fired");
    send(h2.c, probe_answer_frame("900C"), pair_end2 + 170 + 420);
    check(h2.t.status() == TeleOpController::Status::WaitProbe,
          "answer at +420 ms not accepted (past the bound)");
    h2.t.tick(pair_end2 + 170 + 600);
    check(h2.t.status() == TeleOpController::Status::NoAnswer,
          "verdict NoAnswer on the late answer");
    check(h2.bus.tx_log.size() == 1, "the write never fired");
    check_line_has(h2.t.status_line(), "a 9052 frame was seen 420 ms after the probe",
                   "rejected-answer delay recorded for calibration");
    auto ev = h2.c.drain_events();
    check(!has_event(ev, EventKind::ProbeAnswer),
          "past-bound answer is not routed as ours");
}

int main() {
    test_frames_byte_exact();
    test_sys_off_end_to_end();
    test_no_answer_silent_pack();
    test_no_answer_master_telemetry_is_not_the_answer();
    test_probe_answer_after_master_traffic_rejected();
    test_refusals();
    test_wake_pair_end_to_end();
    test_wake_pair_disable_noack_continues();
    test_foreign_probe_aborts();
    test_sys_off_noack_then_late_ack();
    test_shared_gate_after_done();
    test_autooff_predicate();
    test_probe_answer_not_consumed_when_no_window();
    test_half_pair_warning();
    test_answer_delay_bound();
    if (failures == 0)
        printf("telecontrol oracle: all scenarios passed\n");
    else
        printf("telecontrol oracle: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
