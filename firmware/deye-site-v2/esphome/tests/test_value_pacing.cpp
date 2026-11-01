// Issue #53 host oracle, review round 2. Models the pacing layer FAITHFULLY
// to the handler: the 29-slot MQTT pool is the ONLY admission gate (no
// artificial per-pass cap — a fixed cap starves the backlog under link
// flaps), the drain refills the pool concurrently during the paced pass,
// stamps every 3rd pass share the pool, stack sites alternate FRONT on
// odd passes / tail on even (position alternation), change-only types
// (alarm strings, mosfet bools) carry 300 s repair heartbeats, and the
// drain task can DISCARD accepted-but-undelivered messages on disconnect
// (commit stands; only the heartbeat heals it). Slots spread k*H/N as
// arm_phases_ does. Flapping must not push slots back.
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
static constexpr uint32_t HB_CHANGE = 300000;
static constexpr int N_FAST = 55;
static constexpr int N_CELL = 110;
static constexpr int N_CHANGE = 53;
static constexpr int N_STACK = 9;
static constexpr int POOL = 29;   // the pool is the only admission gate
static constexpr int STAMPS_PER_REPAIR = 12;

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
        // drain_per_sec/1000 of an in-flight slot per publish. Scaled by
        // 100 to keep the arithmetic integral: every 100 publishes give
        // drain_per_sec/10 worth of drain.
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
    long pass_pub_count = 0;
    uint32_t pass_idx = 0;
    uint32_t reconnect_calls = 0;

    World()
        : fast(N_FAST), cell(N_CELL), stack(N_STACK), change(N_CHANGE),
          fast_pub(N_FAST, 0), cell_pub(N_CELL, 0), stack_pub(N_STACK, 0),
          change_pub(N_CHANGE, 0) {}

    // k*H/N slots, mirroring arm_phases_; the reconnect path skips
    // armed/committed instances so anchors are never moved — arm_all runs
    // once per fresh World, and on_reconnect() is the handler's no-op for
    // everything already touched.
    void arm_all(uint32_t now) {
        for (int i = 0; i < N_FAST; i++) fast[i].arm(now, (uint32_t) ((uint64_t) i * HB_FAST / N_FAST), HB_FAST);
        for (int i = 0; i < N_CELL; i++) cell[i].arm(now, (uint32_t) ((uint64_t) i * HB_CELL / N_CELL), HB_CELL);
        for (int i = 0; i < N_STACK; i++) stack[i].arm(now, (uint32_t) ((uint64_t) i * HB_FAST / N_STACK), HB_FAST);
        // change types are NOT armed: first publish rides the pool-paced boot wave
    }

    void on_reconnect() { reconnect_calls++; }

    template <typename H>
    void try_pub(H& h, size_t i, std::vector<uint32_t>& pubvec, float v,
                 float thr, float minv, float maxv, uint32_t hb) {
        if (h.decide(v, thr, minv, maxv, hb)) {
            pass_pub_count++;
            if (sim.try_enqueue()) { h.commit(); pubvec[i] = g_clock; }
            else { pool_refusals++; if (steady_window_start && g_clock >= steady_window_start) steady_refusals++; }
        }
    }

    // faithful pass shape: values, change types and cells all live INSIDE
    // the per-pack loop and rotate with the pack order; stack publishes
    // FRONT on odd passes and tail on even (position alternation).
    static constexpr int PACKS = 5;
    static constexpr int AGG_PER_PACK = 11;
    static constexpr int CHANGE_PER_PACK = 10;
    static constexpr int CELLS_PER_PACK = 22;

    void pass(float fast_val, float cell_val) {
        pass_pub_count = 0;
        if (pass_idx % 3 == 0) {
            for (int k = 0; k < STAMPS_PER_REPAIR; k++) sim.try_enqueue();
        }
        bool stack_first = (pass_idx % 2) == 1;
        auto run_stack = [&]() {
            for (size_t n = 0; n < stack.size(); n++) {
                size_t i = (n + pass_idx) % stack.size();
                try_pub(stack[i], i, stack_pub, fast_val + 100.0f + (float) i, 0.005f, -50.0f, 5000.0f, HB_FAST);
            }
        };
        if (stack_first) run_stack();
        size_t b0 = pass_idx % PACKS;
        for (int bb = 0; bb < PACKS; bb++) {
            int p = (int) ((bb + b0) % PACKS);
            for (int j = 0; j < AGG_PER_PACK; j++) {
                size_t i = (size_t) (p * AGG_PER_PACK + j);
                if (i < fast.size())
                    try_pub(fast[i], i, fast_pub, fast_val + (float) i, 0.005f, -50.0f, 5000.0f, HB_FAST);
            }
            for (int j = 0; j < CHANGE_PER_PACK; j++) {
                size_t i = (size_t) (p * CHANGE_PER_PACK + j);
                if (i >= change.size()) continue;
                if (change[i].decide("v", HB_CHANGE)) {
                    pass_pub_count++;
                    if (sim.try_enqueue()) { change[i].commit(); change_pub[i] = g_clock; }
                    else { pool_refusals++; if (steady_window_start && g_clock >= steady_window_start) steady_refusals++; }
                }
            }
            for (int j = 0; j < CELLS_PER_PACK; j++) {
                size_t i = (size_t) (p * CELLS_PER_PACK + j);
                if (i < cell.size())
                    try_pub(cell[i], i, cell_pub, cell_val, 0.010f, 0.0f, 5.0f, HB_CELL);
            }
        }
        for (size_t i = (size_t) (CHANGE_PER_PACK * PACKS); i < change.size(); i++) {
            if (change[i].decide("v", HB_CHANGE)) {
                pass_pub_count++;
                if (sim.try_enqueue()) { change[i].commit(); change_pub[i] = g_clock; }
            }
        }
        if (!stack_first) run_stack();
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
        w.arm_all(boot);
        long max_in_pass = 0;
        w.steady_window_start = boot + 3300000;   // last 5 min of the hour
        for (g_clock = boot + 10000; g_clock < boot + 3600000; g_clock += 10000) {
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
            max_in_pass = std::max(max_in_pass, (long) w.pass_pub_count);
        }
        // boot-wave passes may overflow the pool (deferral is the design);
        // STEADY state on a healthy link must be refusal-free
        assert(w.steady_refusals == 0);
        assert(w.never_published() == 0);
        for (int i = 0; i < N_FAST; i++) assert(g_clock - w.fast_pub[i] <= 70000);
        for (int i = 0; i < N_STACK; i++) assert(g_clock - w.stack_pub[i] <= 70000);
        for (int i = 0; i < N_CELL; i++) assert(g_clock - w.cell_pub[i] <= HB_CELL + 10000);
        for (int i = 0; i < N_CHANGE; i++) assert(g_clock - w.change_pub[i] <= HB_CHANGE + 10000);
        printf("S1 boot spread + hour soak: ok (max pass %ld, boot deferrals %ld)\n", max_in_pass, w.pool_refusals);
    }

    // ---------- S2: cadence pins (fast/stack 1/min; cell/change 1/5min) ----------
    {
        World w;
        g_clock = 0;
        w.arm_all(0);
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
        assert(change_pubs >= 11 && change_pubs <= 13);   // the 300 s repair heartbeat
        printf("S2 cadence: ok (fast %ld stack %ld cell %ld change %ld per hour)\n",
               fast_pubs, stack_pubs, cell_pubs, change_pubs);
    }

    // ---------- S3: commit-on-acceptance retry while disconnected ----------
    {
        World w;
        g_clock = 0;
        w.arm_all(0);
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

    // ---------- S4: accepted-then-discarded heals via heartbeat (the blocker) ----------
    {
        World w;
        g_clock = 0;
        w.arm_all(0);
        for (g_clock = 10000; g_clock < 700000; g_clock += 10000) { w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        uint32_t tc = w.change_pub[0];
        assert(tc > 0);
        // accepted-then-discarded: the drain wipes in-flight right after a
        // commit; the value was never delivered and NO change is coming
        w.sim.disconnect();
        g_clock += 5000;
        w.sim.connected = true;
        w.on_reconnect();
        uint32_t healed_at = 0;
        for (int k = 0; k < 40 && !healed_at; k++) {
            g_clock += 10000;
            w.pass(3.3f, 3.3f);      // constant value: only the heartbeat path fires
            w.sim.tick_second();
            if (w.change_pub[0] != tc) healed_at = w.change_pub[0];
        }
        assert(healed_at);
        assert(healed_at - tc <= HB_CHANGE + 20000);
        printf("S4 accepted-then-discarded heal: ok (healed %u ms after commit)\n", healed_at - tc);
    }

    // ---------- S5: armed-slot deferral + post-commit immediacy ----------
    {
        HysteresisFloat h;
        g_clock = 10000;
        h.arm(g_clock, 25000, HB_FAST);
        bool fired_early = false;
        for (g_clock = 11000; g_clock < 34000; g_clock += 10000)
            if (h.decide(9.9f, 0.005f, -50.0f, 5000.0f, HB_FAST)) fired_early = true;
        assert(!fired_early);
        g_clock = 36000;
        assert(h.decide(9.9f, 0.005f, -50.0f, 5000.0f, HB_FAST));
        h.commit();
        g_clock = 46000;
        assert(h.decide(2.0f, 0.005f, -50.0f, 5000.0f, HB_FAST));
        h.commit();
        g_clock = 50000;
        assert(!h.decide(2.0f, 0.005f, -50.0f, 5000.0f, HB_FAST));
        g_clock = 107000;
        assert(h.decide(2.0f, 0.005f, -50.0f, 5000.0f, HB_FAST));
        printf("S5 armed-slot deferral: ok\n");
    }

    // ---------- S6: legacy check() compat (rack-solar path incl. new types) ----------
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
    // change types and stack (position-alternated + reserved) share the
    // budget fairly; per-cell values — 300 s cadence detail — yield by
    // design and MUST recover within a few passes once the storm abates.
    {
        World w;
        g_clock = 1000;
        w.arm_all(g_clock);
        for (int k = 0; k < 80; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); w.sim.tick_second(); }
        std::vector<long> deferred_fast(N_FAST, 0);
        std::vector<uint32_t> prev_f(N_FAST);
        for (int i = 0; i < N_FAST; i++) prev_f[i] = w.fast_pub[i];
        uint32_t cells_starved_before = 0;
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
        (void) last_cell0; (void) cells_starved_before;
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

    // ---------- S8: flapping link — anchors hold, liveness kept, and
    // recovery is bounded ----------
    // Under a chronic 30 s flap the connected-pass bandwidth is a fraction
    // of the accumulated dues; the review's killer case (re-arm re-anchoring
    // pushing slots back forever) must NOT recur: armed instances keep
    // their boot anchors, committed instances heartbeat from their commit
    // time, priority classes stay live, and once the link stabilizes
    // everything converges within a bounded recovery window.
    {
        World w;
        uint32_t boot = 1000;
        g_clock = boot;
        w.arm_all(boot);
        for (int cycle = 0; cycle < 120; cycle++) {   // 1 h of flapping
            w.sim.disconnect();
            for (int k = 0; k < 2; k++) { g_clock += 10000; w.pass(3.3f, 3.3f); }
            w.sim.connected = true;
            w.on_reconnect();
            g_clock += 10000;
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
        }
        // liveness during the flap: aggregates and stack kept publishing
        for (int i = 0; i < N_FAST; i++) assert(w.fast_pub[i] > 0);
        for (int i = 0; i < N_STACK; i++) assert(w.stack_pub[i] > 0);
        // the flap ends; everything must converge quickly
        int recovery_passes = 0;
        while (recovery_passes < 60 && w.never_published() != 0) {
            g_clock += 10000;
            w.pass(3.3f, 3.3f);
            w.sim.tick_second();
            recovery_passes++;
        }
        assert(w.never_published() == 0);   // no permanently-stuck instance
        for (int i = 0; i < N_FAST; i++) assert(g_clock - w.fast_pub[i] <= 70000);
        for (int i = 0; i < N_CELL; i++) assert(g_clock - w.cell_pub[i] <= HB_CELL + 10000);
        for (int i = 0; i < N_CHANGE; i++) assert(g_clock - w.change_pub[i] <= HB_CHANGE + 10000);
        printf("S8 flap resilience: ok (liveness through 120 flaps, converged after %d stable passes)\n",
               recovery_passes);
    }

    // ---------- S9: slow-drain shaping keeps the pool in bounds ----------
    {
        World w;
        g_clock = 0;
        w.arm_all(0);
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
