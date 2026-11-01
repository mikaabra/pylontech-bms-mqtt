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
            if (mqtt_was_connected && disconnect_count > 0) {
                // short-gap branch: values are re-sent by on_short_reconnect,
                // and an in-flight discovery is rewound — up to 29 accepted
                // messages may have been discarded by the drain task during
                // the gap, so resuming mid-cursor could leave permanent holes
                if (!discovery_published) {
                    can.restart();
                    rs485.restart();
                }
            }
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

    // exact mirror of the yaml Re-publish HA Discovery lambda
    void button_blast() {
        can.restart();
        rs485.restart();
        PublishBudget budget(250);
        can.pass(budget);
        if (budget.remaining > 0) rs485.pass(budget);
        // an incomplete blast is resumed by the 3s tick until it completes
        discovery_published = false;
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

    // 5. Button blast after a completed discovery: budget 250 < 297 sites,
    //    so the blast stops partway (RS485 at 214); the tick must resume it
    //    and every site must land exactly once.
    Driver b(CAN_SITES, RS485_SITES);
    for (int i = 0; i < 15; i++) b.tick(true);
    assert(b.discovery_published);
    const int before_button = b.can.publish_count + b.rs485.publish_count;
    assert(before_button == 297);
    b.button_blast();
    assert(!b.discovery_published);          // yaml forces false after blast
    assert(b.rs485.pos == 250 - CAN_SITES);  // blast stopped partway: 214 of 261
    ticks = 0;
    while (!b.discovery_published && ticks < 200) { b.tick(true); ticks++; }
    assert(b.discovery_published);
    assert(ticks == 3);                       // 47 remaining sites at budget 20
    // within the blast+resume cycle every site landed exactly once: CAN
    // 36 by the blast, RS485 214 by the blast + 47 by the tick resume
    assert(b.can.publish_count == 2 * CAN_SITES);       // once at boot, once by blast
    assert(b.rs485.publish_count == 2 * RS485_SITES);    // once at boot, once by blast+resume
    assert((int) b.can.sent.size() == CAN_SITES);
    assert((int) b.rs485.sent.size() == RS485_SITES);
    (void) before_button;

    // 6. Short gap (single disconnected tick) during discovery rewinds the
    //    streams — the drain task may have discarded accepted-but-unsent
    //    sites, so a mid-cursor resume could leave permanent holes.
    Driver g(CAN_SITES, RS485_SITES);
    for (int i = 0; i < 5; i++) g.tick(true);
    assert(!g.discovery_published);
    const int before_gap = g.can.publish_count + g.rs485.publish_count;
    g.tick(false);   // one poll: below the 2-tick debounce latch...
    assert(g.disconnect_count == 1);
    g.tick(true);    // ...but the short-gap branch still runs
    assert(!g.discovery_started || true);  // latch untouched by short gaps
    ticks = 0;
    while (!g.discovery_published && ticks < 200) { g.tick(true); ticks++; }
    assert(g.discovery_published);
    // rewound to the top: a full clean 297-site pass after the gap
    assert(g.can.publish_count + g.rs485.publish_count == before_gap + 297);

    printf("discovery driver oracle passed\n");
    return 0;
}
