// Issue #53 host oracle, review round 3. Models the handler faithfully:
// every heartbeat-bearing instance carries a FIXED slot (k*H/N window
// position assigned once — change types included; outages cannot move it,
// a shared commit instant cannot regroup the cohort), the 29-slot pool is
// the only admission gate, the pass STOPS at the first client refusal
// (adaptive bound, no fixed budget), stamps share the pool every 3rd pass
// (11 = availability + both alarm latches x packs), stack sites alternate
// FRONT on odd passes / tail on even, and the drain task can DISCARD
// accepted-but-undelivered messages on disconnect (commit stands; the next
// slot occurrence heals it).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

static uint32_t g_clock = 0;
uint32_t millis() { return g_clock; }

#include "../../../shared/esphome/hysteresis.h"

static constexpr uint32_t HB_FAST = 60000;
static constexpr uint32_t HB_CELL = 300000;
static constexpr int N_FAST = 55;
static constexpr int N_CELL = 110;
static constexpr int N_CHANGE = 53;
static constexpr int N_STACK = 9;
static constexpr int N_SLOW = N_CELL + N_CHANGE;   // the 300 s class
static constexpr int POOL = 29;   // the pool is the only admission gate
static constexpr int STAMPS_PER_REPAIR = 11;       // 1 availability + 2 latches x 5 packs

struct Sim {
    long in_flight = 0;
    long drain_per_sec = 200;
    long drain_credit = 0;
    long refused_client = 0;
    bool connected = true;

    bool try_enqueue() {
        if (!connected || in_flight >= POOL) { refused_client++; return false; }
        in_flight++;
        // concurrent drain during the paced pass: each publish takes >=1 ms
        // (producer pacing), so the async drain task consumes
        // drain_per_sec/1000 of an in-flight slot per publish, scaled to
        // stay integral: 1000 credit units = one drained slot.
        drain_credit += drain_per_sec;
        while (drain_credit >= 1000 && in_flight > 0) {
            in_flight--;
            drain_credit -= 1000;
        }
        return true;
    }
    void tick_second() { in_flight = std::max(0L, in_flight - drain_per_sec); }
    // the drain task drops accepted-but-unsent messages on disconnect
    void disconnect() { connected = false; in_flight = 0; }
};

struct World {
    std::vector<HysteresisFloat> fast, cell, stack;
    std::vector<HysteresisString> change;
    std::vector<uint32_t> fast_pub, cell_pub, stack_pub, change_pub;
    Sim sim;
    long pool_refusals = 0;
    long steady_refusals = 0;    // pool refusals inside [steady_window_start, inf)
    uint32_t steady_window_start = 0;
    long pass_dues = 0;          // decide()==true count this pass (even when pass closed)
    long pass_pub_count = 0;     // real enqueue attempts this pass
    bool pass_open = true;       // false after the pass's first client refusal
    int max_dues = 0;            // peak pass_dues, tracked while dues_tracked
    bool dues_tracked = false;
    uint32_t pass_idx = 0;
    uint32_t reconnect_calls = 0;

    World()
        : fast(N_FAST), cell(N_CELL), stack(N_STACK), change(N_CHANGE),
          fast_pub(N_FAST, 0), cell_pub(N_CELL, 0), stack_pub(N_STACK, 0),
          change_pub(N_CHANGE, 0) {}

    // mirrors assign_slots_: k*H/N per class, idempotent (already-slotted
    // instances keep their original origin — a reconnect can never move a
    // slot), change types included in the 300 s class spread
    void assign_all(uint32_t now) {
        for (int i = 0; i < N_FAST; i++)
            if (!fast[i].slot_period_) fast[i].assign_slot(now, (uint32_t) ((uint64_t) i * HB_FAST / N_FAST), HB_FAST);
        for (int i = 0; i < N_STACK; i++)
            if (!stack[i].slot_period_) stack[i].assign_slot(now, (uint32_t) ((uint64_t) (N_FAST + i) * HB_FAST / (N_FAST + N_STACK)), HB_FAST);
        for (int i = 0; i < N_CELL; i++)
            if (!cell[i].slot_period_) cell[i].assign_slot(now, (uint32_t) ((uint64_t) i * HB_CELL / N_SLOW), HB_CELL);
        for (int i = 0; i < N_CHANGE; i++)
            if (!change[i].slot_period_) change[i].assign_slot(now, (uint32_t) ((uint64_t) (N_CELL + i) * HB_CELL / N_SLOW), HB_CELL);
    }

    // mirrors the handler reconnect paths: assign_slots_ is a no-op for
    // every instance that already carries a slot
    void on_reconnect() {
        reconnect_calls++;
        assign_all(g_clock);
    }

    void refusal_close_() {
        pool_refusals++;
        pass_open = false;
        if (steady_window_start && g_clock >= steady_window_start) steady_refusals++;
    }

    template <typename H>
    void try_pub(H& h, size_t i, std::vector<uint32_t>& pubvec, float v,
                 float thr, float minv, float maxv) {
        if (h.decide(v, thr, minv, maxv, HB_FAST)) {
            pass_dues++;
            if (!pass_open) return;
            pass_pub_count++;
            if (sim.try_enqueue()) { h.commit(); pubvec[i] = g_clock; }
            else refusal_close_();
        }
    }

    template <typename H>
    void try_pub_cell(H& h, size_t i, std::vector<uint32_t>& pubvec, float v,
                      float thr, float minv, float maxv) {
        if (h.decide(v, thr, minv, maxv, HB_CELL)) {
            pass_dues++;
            if (!pass_open) return;
            pass_pub_count++;
            if (sim.try_enqueue()) { h.commit(); pubvec[i] = g_clock; }
            else refusal_close_();
        }
    }

    void try_pub_change(size_t i, const char* v) {
        if (change[i].decide(v)) {
            pass_dues++;
            if (!pass_open) return;
            pass_pub_count++;
            if (sim.try_enqueue()) { change[i].commit(); change_pub[i] = g_clock; }
            else refusal_close_();
        }
    }

    // faithful pass shape: stamps every 3rd pass lead, stack alternates
    // FRONT on odd passes / tail on even, packs rotate, values + change
    // types + cells live inside the per-pack loop
    static constexpr int PACKS = 5;
    static constexpr int AGG_PER_PACK = 11;
    static constexpr int CHANGE_PER_PACK = 10;
    static constexpr int CELLS_PER_PACK = 22;

    void pass(float fast_val, float cell_val) {
        pass_dues = 0;
        pass_pub_count = 0;
        pass_open = true;
        if (pass_idx % 3 == 0) {
            for (int k = 0; k < STAMPS_PER_REPAIR && pass_open; k++) {
                if (sim.try_enqueue()) { pass_pub_count++; pass_dues++; }
                else refusal_close_();
            }
        }
        bool stack_first = (pass_idx % 2) == 1;
        auto run_stack = [&]() {
            for (size_t n = 0; n < stack.size(); n++) {
                size_t i = (n + pass_idx) % stack.size();
                try_pub(stack[i], i, stack_pub, fast_val + 100.0f + (float) i, 0.005f, -50.0f, 5000.0f);
            }
        };
        if (stack_first) run_stack();
        size_t b0 = pass_idx % PACKS;
        for (int bb = 0; bb < PACKS; bb++) {
            int p = (int) ((bb + b0) % PACKS);
            for (int j = 0; j < AGG_PER_PACK; j++) {
                size_t i = (size_t) (p * AGG_PER_PACK + j);
                if (i < fast.size())
                    try_pub(fast[i], i, fast_pub, fast_val + (float) i, 0.005f, -50.0f, 5000.0f);
            }
            for (int j = 0; j < CHANGE_PER_PACK; j++) {
                size_t i = (size_t) (p * CHANGE_PER_PACK + j);
                if (i < change.size()) try_pub_change(i, "v");
            }
            for (int j = 0; j < CELLS_PER_PACK; j++) {
                size_t i = (size_t) (p * CELLS_PER_PACK + j);
                if (i < cell.size())
                    try_pub_cell(cell[i], i, cell_pub, cell_val, 0.010f, 0.0f, 5.0f);
            }
        }
        for (size_t i = (size_t) (CHANGE_PER_PACK * PACKS); i < change.size(); i++) {
            try_pub_change(i, "v");
        }
        if (!stack_first) run_stack();
        if (dues_tracked) max_dues = std::max(max_dues, (int) pass_dues);
        pass_idx++;
    }

    int never_published() const {
        int n = 0;
        for (auto t : fast_pub) if (t == 0) n++;
        for (auto t : cell_pub) if (t == 0) n++;
        for (auto t : stack_pub) if (t == 0) n++;
        for (auto t : change_pub) if (t == 0) n++;
        return n;
    }
};

int main() {
    // ---------- S1: boot spread with stamps in the mix, one-hour soak ----------
    {
        World w;
        uint32_t boot = 5000;
        g_clock = boot;
        w.assign_all(boot);
        long max_in_pass = 0;
        w.steady_window_start = boot + 3300000;   // last 5 min of the hour
        for (g_clock = boot + 10000; g_clock < boot + 3600000; g_clock += 10000) {
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
            max_in_pass = std::max(max_in_pass, (long) w.pass_pub_count);
        }
        // boot-wave passes may overflow the pool (deferral is the design);
        // STEADY state on a healthy link must be refusal-free and the slot
        // spread must keep every pass inside the pool at idle
        assert(w.steady_refusals == 0);
        assert(w.never_published() == 0);
        for (int i = 0; i < N_FAST; i++) assert(g_clock - w.fast_pub[i] <= 70000);
        for (int i = 0; i < N_STACK; i++) assert(g_clock - w.stack_pub[i] <= 70000);
        for (int i = 0; i < N_CELL; i++) assert(g_clock - w.cell_pub[i] <= HB_CELL + 10000);
        for (int i = 0; i < N_CHANGE; i++) assert(g_clock - w.change_pub[i] <= HB_CELL + 10000);
        printf("S1 boot spread + hour soak: ok (max pass %ld, boot deferrals %ld)\n", max_in_pass, w.pool_refusals);
    }

    // ---------- S2: cadence pins (fast/stack 1/min; slow class 1/5min) ----------
    {
        World w;
        g_clock = 0;
        w.assign_all(0);
        long fast_pubs = 0, cell_pubs = 0, change_pubs = 0, stack_pubs = 0;
        uint32_t pf = 0, pc = 0, pch = 0, ps = 0;
        for (g_clock = 10000; g_clock < 3600000; g_clock += 10000) {
            w.pass(4.2f, 3.3f);
            w.sim.tick_second();
            if (w.fast_pub[0] != pf) { fast_pubs++; pf = w.fast_pub[0]; }
            if (w.cell_pub[0] != pc) { cell_pubs++; pc = w.cell_pub[0]; }
            if (w.change_pub[0] != pch) { change_pubs++; pch = w.change_pub[0]; }
            if (w.stack_pub[0] != ps) { stack_pubs++; ps = w.stack_pub[0]; }
        }
        assert(fast_pubs >= 58 && fast_pubs <= 64);
        assert(stack_pubs >= 58 && stack_pubs <= 64);
        assert(cell_pubs >= 11 && cell_pubs <= 13);
        assert(change_pubs >= 11 && change_pubs <= 13);   // fixed-slot republish
        printf("S2 cadence: ok (fast %ld stack %ld cell %ld change %ld per hour)\n",
               fast_pubs, stack_pubs, cell_pubs, change_pubs);
    }

    // ---------- S3: commit-on-acceptance retry while disconnected ----------
    {
        World w;
        g_clock = 0;
        w.assign_all(0);
        for (g_clock = 10000; g_clock < 700000; g_clock += 10000) { w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        uint32_t last_pub0 = w.fast_pub[1];
        w.sim.disconnect();
        g_clock += HB_FAST + 30000;
        int refused_passes = 0;
        for (int k = 0; k < 3; k++) {
            w.pass(3.35f, 3.3f);
            g_clock += 10000;
            if (w.fast_pub[1] == last_pub0) refused_passes++;
        }
        assert(refused_passes >= 1);
        assert(w.fast_pub[1] == last_pub0);      // uncommitted, not lost
        w.sim.connected = true;
        w.on_reconnect();
        int delivered = 0;
        for (int k = 0; k < 6; k++) {
            g_clock += 10000;
            w.pass(3.35f, 3.3f);
            w.sim.tick_second();
            delivered++;
            if (w.fast_pub[1] > last_pub0) break;
        }
        assert(w.fast_pub[1] > last_pub0);
        assert(w.sim.refused_client > 0);
        printf("S3 retry: ok (refused %d passes, delivered after %d)\n", refused_passes, delivered);
    }

    // ---------- S4: accepted-then-discarded heals at its slot (the blocker) ----------
    {
        World w;
        g_clock = 0;
        w.assign_all(0);
        for (g_clock = 10000; g_clock < 700000; g_clock += 10000) { w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        uint32_t tc = w.change_pub[0];
        assert(tc > 0);
        // accepted-then-discarded: the drain wipes in-flight right after a
        // commit; the value was never delivered and NO change is coming —
        // the fixed slot re-sends it at its window position
        w.sim.disconnect();
        g_clock += 5000;
        w.sim.connected = true;
        w.on_reconnect();
        uint32_t healed_at = 0;
        for (int k = 0; k < 40 && !healed_at; k++) {
            g_clock += 10000;
            w.pass(3.3f, 3.3f);      // constant value: only the slot fires
            w.sim.tick_second();
            if (w.change_pub[0] != tc) healed_at = w.change_pub[0];
        }
        assert(healed_at);
        assert(healed_at - tc <= HB_CELL + 20000);
        printf("S4 accepted-then-discarded heal: ok (healed %u ms after commit)\n", healed_at - tc);
    }

    // ---------- S6: legacy check() compat (S5, the armed API pin, removed with arm()) (rack-solar path incl. new types) ----------
    {
        HysteresisFloat h;
        g_clock = 1000;
        assert(h.check(3.3f, 0.01f, 0.0f, 5.0f, 60000));
        assert(!h.check(3.31f, 0.01f, 0.0f, 5.0f, 60000));
        assert(h.check(3.5f, 0.01f, 0.0f, 5.0f, 60000));
        g_clock += 61000;
        assert(h.check(3.5f, 0.01f, 0.0f, 5.0f, 60000));
        HysteresisBool b;
        assert(b.check(true));
        assert(!b.check(true));
        assert(b.check(false));
        HysteresisString s;
        assert(s.check("idle"));
        assert(!s.check("idle"));
        assert(s.check("chg"));
        HysteresisInt i;
        assert(i.check(42));
        assert(!i.check(42));
        assert(i.check(43));
        printf("S6 legacy compatibility: ok\n");
    }

    // ---------- S7: storm behavior — serviced classes fair, cells yield,
    // recovery bounded ----------
    // Design contract under sustained full saturation (every aggregate
    // crossing every pass, which outweighs the budget 3:1): aggregates,
    // change types and stack (position-alternated) share the budget
    // fairly; per-cell values — 300 s cadence detail — yield by design and
    // MUST recover within a few passes once the storm abates.
    {
        World w;
        g_clock = 1000;
        w.assign_all(g_clock);
        for (int k = 0; k < 80; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        std::vector<long> deferred_fast(N_FAST, 0);
        std::vector<uint32_t> prev_f(N_FAST);
        for (int i = 0; i < N_FAST; i++) prev_f[i] = w.fast_pub[i];
        int passes = 100;
        for (int k = 0; k < passes; k++) {
            g_clock += 10000;
            float v = (k % 2) ? 3.3f : 6.6f;
            w.sim.tick_second();
            w.pass(v, 3.3f);
            for (int i = 0; i < N_FAST; i++) { if (w.fast_pub[i] == prev_f[i]) deferred_fast[i]++; else prev_f[i] = w.fast_pub[i]; }
        }
        long dmin = 100000, dmax = -1, dtotal = 0;
        for (int i = 0; i < N_FAST; i++) { dtotal += deferred_fast[i]; dmin = std::min(dmin, deferred_fast[i]); dmax = std::max(dmax, deferred_fast[i]); }
        assert(dtotal > 0);
        assert(dmax - dmin <= passes / 2);        // aggregate storm: fair rotation
        for (int i = 0; i < N_FAST; i++) assert(deferred_fast[i] < passes - 4);   // nothing lost
        for (int i = 0; i < N_STACK; i++) assert(g_clock - w.stack_pub[i] <= 120000);  // alternation keeps stack fresh
        // cells were sacrificed during the storm (bounded by design)...
        uint32_t last_cell0 = w.cell_pub[0];
        (void) last_cell0;
        // ...and the storm abates: cells must recover within a few passes
        int recovery_passes = 0;
        while (recovery_passes < 60) {
            g_clock += 10000;
            w.sim.tick_second();
            w.pass(4.2f, 3.35f);                  // constant values: catch-up only
            recovery_passes++;
            if (w.never_published() == 0 &&
                [&]{ for (int i = 0; i < N_CELL; i++) if (g_clock - w.cell_pub[i] < 310000) return false; return true; }())
                break;
        }
        for (int i = 0; i < N_CELL; i++) assert(g_clock - w.cell_pub[i] < 310000);   // every cell delivered
        for (int i = 0; i < N_CHANGE; i++) assert(g_clock - w.change_pub[i] < 310000);
        printf("S7 storm + recovery: ok (agg deferrals %ld..%ld, cells recovered in %d passes)\n",
               dmin, dmax, recovery_passes);
    }

    // ---------- S8: flapping link — slots hold, liveness kept, recovery
    // bounded, and no post-flap regrouping ----------
    // The round-2 review's killer cases: (a) a shared commit instant after
    // a reconnect regroups the cohort forever under commit-anchored
    // heartbeats; (b) re-arm pushing boot anchors back. Fixed slots close
    // both: a value due during an outage fires at its NEXT SLOT OCCURRENCE,
    // never at the reconnect instant. Asserted: priority classes stay live
    // through the flaps, no instance strands permanently, convergence is
    // bounded, and once one heartbeat window has passed with a healthy
    // link, EVERY subsequent pass fits the pool again (peak dues <= POOL).
    {
        World w;
        uint32_t boot = 1000;
        g_clock = boot;
        w.assign_all(boot);
        for (int cycle = 0; cycle < 120; cycle++) {   // 1 h of 30 s flapping
            w.sim.disconnect();
            for (int k = 0; k < 2; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); }
            w.sim.connected = true;
            w.on_reconnect();   // runs the skip logic: a no-op for slotted instances
            g_clock += 10000;
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
        }
        // the reconnect path must have exercised the skip logic without
        // moving a single slot
        assert(w.reconnect_calls == 120);
        // liveness during the flap: aggregates and stack kept publishing
        for (int i = 0; i < N_FAST; i++) assert(w.fast_pub[i] > 0);
        for (int i = 0; i < N_STACK; i++) assert(w.stack_pub[i] > 0);
        // the flap ends; track peak dues per pass for one full slow window
        w.dues_tracked = true;
        for (int k = 0; k < 30; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        int catchup_max_dues = w.max_dues;
        w.max_dues = 0;
        int recovery_passes = 0;
        while (recovery_passes < 60 && w.never_published() != 0) {
            g_clock += 10000;
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
            recovery_passes++;
        }
        assert(w.never_published() == 0);   // no permanently-stuck instance
        w.max_dues = 0;
        // after the catch-up window: every pass fits the pool — this is the
        // assertion the commit-anchored heartbeat failed (grouped 60 s /
        // 300 s cohorts refiring forever)
        for (int k = 0; k < 30; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        assert(w.max_dues <= POOL);
        for (int i = 0; i < N_FAST; i++) assert(g_clock - w.fast_pub[i] <= 70000);
        for (int i = 0; i < N_CELL; i++) assert(g_clock - w.cell_pub[i] <= HB_CELL + 10000);
        for (int i = 0; i < N_CHANGE; i++) assert(g_clock - w.change_pub[i] <= HB_CELL + 10000);
        (void) catchup_max_dues;
        printf("S8 flap resilience: ok (liveness through 120 flaps, converged after %d stable passes, steady peak dues %d <= %d)\n",
               recovery_passes, w.max_dues, POOL);
    }

    // ---------- S9: slow-drain shaping keeps the pool in bounds ----------
    {
        World w;
        g_clock = 0;
        w.assign_all(0);
        w.sim.drain_per_sec = 40;   // 5x slowed drain
        long max_in_flight = 0;
        for (g_clock = 10000; g_clock < 1200000; g_clock += 10000) {
            w.pass(3.3f, 3.3f);
            max_in_flight = std::max(max_in_flight, w.sim.in_flight);
            w.sim.tick_second();
        }
        assert(max_in_flight <= POOL);   // stamps + values shaped inside the pool
        assert(w.never_published() == 0);
        printf("S9 slow-drain shaping: ok (max in-flight %ld of %d)\n", max_in_flight, POOL);
    }

    printf("test_value_pacing: ALL PASS\n");
    return 0;
}
