// Host oracle for breadcrumbs.h: exercises the boot -> MQTT-connect ->
// broker-report sequence the 2026-11-01 review found broken (snapshot flag
// cleared before the broker pass), plus the entry/exit-mark discrimination
// and last-publish-duration capture.
#include "../../../shared/esphome/breadcrumbs.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace esphome;

static size_t count_logs(const char *level, const char *needle = nullptr) {
    size_t n = 0;
    for (auto &r : g_logs) {
        if (strcmp(r.level, level) != 0) continue;
        if (needle && !strstr(r.msg, needle)) continue;
        n++;
    }
    return n;
}

int main() {
    // Session 1: power-on — RTC noinit reads garbage; setup initializes
    // silently, marks arm.
    bc_magic_ = 0xdeadbeef;
    g_reset_reason = ESP_RST_POWERON;
    bc_setup();
    assert(g_logs.empty());
    assert(bc_magic_ == BC_MAGIC);

    // Session 1 runs: a publish completes (duration 30 ms at ms=1000),
    // loop marks accumulate.
    g_ms = 1000;
    bc_pub_begin();
    g_ms = 1030;
    bc_pub_end();
    assert(bc_stage_ == BC_IDLE);
    assert(bc_last_pub_ms_ == 30);
    bc_mark(BC_TX_ALARM, 4);
    assert(bc_stage_ == BC_TX_ALARM && bc_aux_ == 4);

    // Session 2: wdt panic reboot. Setup must snapshot + log on serial at
    // ERROR, and KEEP the snapshot pending for the broker pass.
    g_reset_reason = ESP_RST_TASK_WDT;
    g_ms = 2000;
    bc_setup();
    assert(count_logs("ERROR", "prior session ended in stage 4(tx_alarm)") == 1);
    assert(bc_have_snapshot_);
    assert(bc_snap_last_pub_ms_ == 30);
    assert(bc_snap_stage_ms_ == 1030);

    // Broker pass: fires once on connect, at ERROR, then seals the flag;
    // a second call is a no-op even if MQTT flaps.
    bc_report_to_broker(false);
    assert(count_logs("ERROR") == 1);
    bc_report_to_broker(true);
    assert(count_logs("ERROR", "reset_reason=6") == 2);  // serial + broker pass
    assert(!bc_have_snapshot_);
    bc_report_to_broker(true);
    assert(count_logs("ERROR") == 2);  // no third line on repeat

    // Session 3: a publish that never returns. Entry mark stays sticky —
    // this is the "blocked inside the call" case the crumb exists for.
    g_ms = 3000;
    bc_pub_begin();
    g_ms = 3500;
    // (no bc_pub_end: publish "never returned")
    g_reset_reason = ESP_RST_TASK_WDT;
    bc_setup();
    // broker reconnect never happened before this reboot; serial shows the
    // blocked-in-publish position with the entry timestamp.
    assert(count_logs("ERROR", "stage 6(pub_data)") == 1);
    assert(bc_snap_stage_ms_ == 3000);
    bc_report_to_broker(true);
    assert(count_logs("ERROR", "stage 6(pub_data)") == 2);

    // Session 4: routine OTA reboot (software reset) — serial INFO, broker
    // WARN flagged as routine, so HA keeps the reset_reason field without
    // an error-level false alarm.
    bc_mark(BC_IDLE);
    g_reset_reason = ESP_RST_SW;
    bc_setup();
    assert(count_logs("INFO", "routine reset") == 1);
    assert(bc_have_snapshot_);
    bc_report_to_broker(true);
    assert(count_logs("WARN", "routine reset") == 1);
    assert(count_logs("ERROR", "routine reset") == 0);

    // Session 5: blocked AFTER a publish returned (exit mark wins: IDLE,
    // not pub_data) — the "blocked in unmarked code" discrimination.
    g_ms = 5000;
    bc_pub_begin();
    g_ms = 5060;
    bc_pub_end();
    g_ms = 5100;  // loop continues into unmarked code and "hangs" here
    g_reset_reason = ESP_RST_TASK_WDT;
    bc_setup();
    assert(count_logs("ERROR", "stage 1(idle)") == 1);
    assert(bc_snap_stage_ms_ == 5060);

    printf("breadcrumbs oracle passed\n");
    return 0;
}
