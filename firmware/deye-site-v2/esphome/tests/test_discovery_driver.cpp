// Desktop oracle for the two-stream discovery driver — the layer the
// 2026-11-01 hand-trace missed (review blocker 1: a done stream reset its
// cursor at completion, replayed itself every tick, and starved the second
// handler; 132 ticks / 2,637 messages instead of 15 / 297).
//
// This models the per-stream cursor contract that publish_discovery()
// implements and the exact 3s-tick driver from the yaml. Drift risk: the
// real stream bodies are not executed — the CONTRACT below must stay
// mirrored in the handlers and in the yaml lambda.
//
// Contract (fixed semantics):
//   - pass(): resumes from disc_pos_; every due site consumes one budget
//     slot and publishes; budget exhaustion or a rejected publish returns
//     false with the cursor unchanged (retry-point resume).
//   - A COMPLETED stream keeps disc_pos_ at the end: a re-call skips every
//     site, consumes zero budget, and returns true.
//   - Only on_mqtt_connect() (new latched connection) and the button rewind
//     the cursor — sites queued but undelivered during a disconnect are
//     discarded by the drain task, so a reconnect must resend from the top.
#include "../../../shared/esphome/mqtt_helpers.h"

#include <cassert>
#include <cstdio>
#include <set>

struct FakeStream {
    explicit FakeStream(int site_count) : sites(site_count) {}

    int sites;
    int pos = 0;
    int reject_at = -1;  // pool rejection at this site (-1 = none)
    int publish_count = 0;
    std::set<int> sent;

    bool pass(PublishBudget &budget) {
        while (pos < sites) {
            if (!budget.take()) return false;
            if (pos == reject_at) return false;
            pos++;
            publish_count++;
            sent.insert(pos);
        }
        return true;
    }

    void restart() { pos = 0; }
};

struct OldBugStream {
    // negative control: the pre-fix semantics that reset the cursor at
    // completion, making every re-call replay the full stream
    FakeStream s;
    bool pass(PublishBudget &budget) {
        bool done = s.pass(budget);
        if (done) s.restart();
        return done;
    }
};

// Mirrors the yaml 3s tick for the discovery machinery
struct Driver {
    FakeStream can, rs485;
    bool discovery_published = false;
    bool discovery_started = false;
    int disconnect_count = 0;
    bool mqtt_was_connected = false;

    Driver(int can_sites, int rs485_sites) : can{can_sites}, rs485{rs485_sites} {}

    void tick(bool connected) {
        if (connected) {
            mqtt_was_connected = true;
            disconnect_count = 0;
            if (!discovery_started) {
                discovery_started = true;
                can.restart();  // on_mqtt_connect(): cursor rewind per new connection
                rs485.restart();
            }
            if (!discovery_published) {
                PublishBudget budget(20);
                bool can_ok = can.pass(budget);
                bool rs485_ok = rs485.pass(budget);
                discovery_published = can_ok && rs485_ok;
            }
        } else if (mqtt_was_connected) {
            disconnect_count++;
            if (disconnect_count >= 2) {
                discovery_published = false;
                discovery_started = false;
                mqtt_was_connected = false;
            }
        }
    }

    void button_blast() {
        can.restart();
        rs485.restart();
        PublishBudget budget(250);
        bool can_ok = can.pass(budget);
        bool rs485_ok = rs485.pass(budget);
        discovery_published = can_ok && rs485_ok;
    }
};

int main() {
    const int CAN_SITES = 36;
    const int RS485_SITES = 261;  // audit + external review agreed count

    // 1. Clean convergence: 15 ticks, 297 messages, no double-sends,
    //    and the done CAN stream never replays itself.
    Driver d(CAN_SITES, RS485_SITES);
    int ticks = 0;
    while (!d.discovery_published && ticks < 200) {
        d.tick(true);
        ticks++;
        assert(d.can.publish_count <= CAN_SITES);  // starvation would break this
    }
    assert(d.discovery_published);
    assert(ticks == 15);
    assert(d.can.publish_count == CAN_SITES);
    assert(d.rs485.publish_count == RS485_SITES);
    int total = d.can.publish_count + d.rs485.publish_count;
    assert(total == 297);
    assert((int) d.can.sent.size() == CAN_SITES);         // each site once
    assert((int) d.rs485.sent.size() == RS485_SITES);       // each site once
    d.tick(true);  // extra ticks after done: pure no-ops
    d.tick(true);
    assert(d.can.publish_count == CAN_SITES);
    assert(d.rs485.publish_count == RS485_SITES);

    // 2. Pool rejection mid-stream: progress stops without advancing the
    //    cursor; clearing the rejection resumes exactly where it stopped.
    Driver r(CAN_SITES, RS485_SITES);
    ticks = 0;
    while (!r.discovery_published && ticks < 200) { r.tick(true); ticks++; }
    assert(ticks == 15);
    // start fresh, reject rs485 site 100
    Driver x(CAN_SITES, RS485_SITES);
    x.rs485.reject_at = 100;
    x.can.reject_at = -1;
    for (int i = 0; i < 50; i++) x.tick(true);
    assert(!x.discovery_published);
    int stalled = x.rs485.publish_count;
    assert(stalled == 100);  // frozen at the rejection site across ticks
    x.rs485.reject_at = -1;
    ticks = 0;
    while (!x.discovery_published && ticks < 200) { x.tick(true); ticks++; }
    assert(x.discovery_published);
    assert((int) x.rs485.sent.size() == RS485_SITES);  // no duplicates after resume

    // 3. Negative control — the pre-fix completion reset: same driver, old
    //    stream semantics; convergence takes >100 ticks instead of 15.
    OldBugStream can_bug{FakeStream{CAN_SITES}}, rs_bug{FakeStream{RS485_SITES}};
    bool published = false;
    int bug_ticks = 0;
    while (!published && bug_ticks < 500) {
        PublishBudget budget(20);
        bool ok1 = can_bug.pass(budget);
        bool ok2 = rs_bug.pass(budget);
        published = ok1 && ok2;
        bug_ticks++;
    }
    assert(published);
    assert(bug_ticks == 132);  // the reviewer's starvation arithmetic
    assert(can_bug.s.publish_count + rs_bug.s.publish_count == 2637);

    // 4. Mid-stream reconnect rewinds both cursors (queue discard during
    //    the gap makes a resume-from-cursor lose sites).
    Driver m(CAN_SITES, RS485_SITES);
    for (int i = 0; i < 5; i++) m.tick(true);
    assert(!m.discovery_published);
    const int before = m.can.publish_count + m.rs485.publish_count;
    m.tick(false);  // debounce tick 1
    m.tick(false);  // debounce tick 2: latched disconnect
    assert(!m.discovery_started);
    for (int i = 0; i < 40 && !m.discovery_published; i++) m.tick(true);
    assert(m.discovery_published);
    // full protocol resend from the top after the rewind; duplicates of
    // the pre-gap sends are expected here (the queue discarded them)
    assert(m.can.publish_count + m.rs485.publish_count == before + 297);
    assert((int) m.rs485.sent.size() == RS485_SITES);

    printf("discovery driver oracle passed\n");
    return 0;
}
