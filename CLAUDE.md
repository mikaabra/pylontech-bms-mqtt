# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPHome firmware for solar battery monitoring on ESP32-S3 hardware:
- **Deye Site** — Pylontech BMS via CAN bus + RS485 (cell voltages, temperatures, alarms)
- **Rack Solar** — Victron SmartShunt (BLE or VE.Direct) + EPEVER MPPT (Modbus/RS485)

All data published to MQTT with Home Assistant auto-discovery.

## Active Deployments (verified 2026-11-03)

| Device | Firmware | Status |
|--------|----------|--------|
| `deye-bms-can.local` | `firmware/deye-site-v2/esphome/deye-bms-can.yaml` | **ACTIVE, #53 stage 3 (value pacing) on device** — production, ESPHome 2026.9.1, encrypted OTA, async MQTT (`idf_send_async`, #42 merged 1919071). Stage 2 (`power_save_mode: none`) closed 2026-11-03: **no large effect**, line kept (closure postmortems in the #57 comments). **Stage 3 flashed 2026-11-03 19:56 local from main `ac0097d` (merge of #55: pool-only pacing, commit-on-acceptance, shared SlotSchedule), config hash `0x7d53b7eb`, built 19:53:46 +0100.** Post-boot verified: cells/temps rode their slots (first occurrences ≈ boot+300 s exactly), refused counter 0, pass-max 261 ms. Induced-refusal acceptance test 2026-11-03 20:04–20:15 local, both pre-registered variants: **IP-drop 60 s** (device saw TCP disconnect at +9 s, pool never jammed → zero counted refusals — model correction: this stack's TCP keepalive detects faster than the pool can fill; reconnect +14 s; slot lattice held to the second through the outage (cell01 exactly 300.0 s apart); no discarded item was identified) and **broker restart** (reconnect +8 s, refused counter unmoved, zero staleness across all 250 rs485 topics, budgeted discovery burst 23 msgs). Counted-refusal path exercised only by the host oracle so far; refused counter stays on as a passive alarm; broker + native-log captures running. Rollback build on demand: wt-main checkout `1327628` → compile → upload. Rollback: unexplained reboot, MQTT disconnect, arrival gap >12 s, sustained RSSI drop, value stale past its pre-registered bound |
| `bms-link-sniffer.local` (DHCP; use mDNS) | `firmware/deye-site-v2/esphome/bms-link-sniffer.yaml` | **ON BUS — FULL STACK, WRITE+READ CLIENTS LIVE (`0x56ac88b9`, built 2026-11-11 12:38:25 +0100 from main `7e79a4a` content: listener + guarded write client #59/#69 + D547 block-read client + generic debug surface + all review fixes from the #69/#70 verdict)** — second Waveshare ESP32-S3-RS485-CAN for the BMS073 Link bus, end-of-bus (battery5 second RJ45 port, CAT6 tail; SP3485EN front end; onboard 120 Ω end jumper: found in 120R on arrival (the default-NC assumption was wrong), moved NC 14:38, briefly 120R again ~21:45-11-11 (harmful in the flipped topology — see evening rounds), state after the final ~22:00 rework UNVERIFIED — checking this jumper is the FIRST action at the next rack visit; no pack-01 read result is interpretable until it is known). Lineage: listen-only USB flash 148e8b1/0x4a6b4ca5 2026-11-10 → write-enabled 0xce216ce3 (11:25) → read client 0xcaf81dc8 (12:08, 10 live reads: packs 03/04 trusted **3430 mV**, 01/02 collision-fail-closed both real damage shapes) → review-fixed build deployed 12:39 (reboot glitch clean, house side unblinking each flash). **CHECKLIST STEP 4 COMPLETE 2026-11-11 12:40 local (first planned write)**: 3430 mV (= current value, zero semantic change) → pack 03 via native-API operator channel: one frame, bare ack landed late (+231 ms, reported per the listen rule, never retried), enable auto-off at each verdict, then the on-device read-back graded **`read-back PASS: 0x4C = 3430 mV, matches the last written value`** — the ack is never the proof; the read-back is. **Write-change proof 12:47 (review-prescribed): 3430 → 3440 → 3430** — both writes stored the changed value (late bare acks ~230 ms consistently, ~15 ms via dongle on 2026-10-31 — the 150 ms deadline missed, the listen window caught, one-frame-per-press held), both read-backs PASS on first press; the device's write path provably changes live pack state in both directions. **Vendor-tool session 2026-11-11 13:11–13:52 (teleControl captured; distilled into `docs/PROTOCOL_REFERENCE.md` "teleControl captured on the wire")**: CID2 45 write `80 00 D5 45 A0 06 [variant: 01 old-gen/04 new-gen] [gate: 00 DISCHG/01 CHG/02 LMCHG/04 SYS_OFF — matches the XML teleControl bit numbers] [set: 01 disable/10 enable]`; probe-addressing (`80 00 D5 C9 E0 02 [addr]…`, pack answers `…90 52 00 [id]`, the write carries no target); ack `80 00 D5 00 00 00 FD 9F` +192–218 ms; all four button pairs captured with cksm classes (enable 0x3AA / disable 0x408 / probe 0x409). Observed behavior: SYS_OFF = fast reboot on an old-gen pack, sits-dark-answering-polls on new-gen pack 4 (woken by a LMCHG disable→enable pair — recovery framing still unobserved, gate-05 conjecture); the master (addr 00) reported 5-of-5 available to the inverter regardless — powered-off packs are bus-invisible; tool's success display ≠ battery state. SYS_OFF gated-op round PAUSED (user) with all ingredients + constraints recorded (allowlist 01–04, never 00; restart-not-off; recovery-first). With-dongle probe: pack 02 first-ever clean block read (0x4C = 3420 mV), pack 01 still silent → strengthens the 120 Ω end-loading hypothesis. **Sanitizer round flashed 2026-11-11 14:02 (PR #75, commit cb5d8bb; config hash unchanged 0x56ac88b9 — YAML identical, build identity by commit + ELF symbol check): ring entries and event details sanitized to printable ASCII at the publish boundary** — the vendor session found raw UART noise bytes (≥0x80) riding garbled ring entries into text-sensor publishes, and invalid UTF-8 killed every subscribed native-API consumer (protobuf rejects the decode); proven on-device: subscribed read-driver survived live damaged-read publishes (exit 0), esphome-logs client went from 70+ DecodeError crash-loops to zero post-flash through the watcher's 20 s sweep dumps, dumps strictly printable, oracle scenario `test_publish_text_is_ascii` permanent, all 7 host suites green -Wall -Wextra. Fresh-boot counters 0/0 verified. **0x4C far-pack check (user task) 14:09–14:10: pack 02 = 3420 mV (at target, no write), packs 03/04 = 3430 mV held through all session SYS_OFFs → SYS_OFF does NOT factory-reset settings** (documented in PROTOCOL_REFERENCE teleControl section); pack 01 still unreadable (1-char-clipped 562-char blocks, 6+ attempts) — its 3420 write pends the 120 Ω inspection so the check-then-write intent holds. **Post-dongle-removal verification + blind write (user-authorized) 14:25–14:28: no-dongle reads: pack 01 0/6 (0/22 across all conditions today — clips of 557–561 of 562, counts varying = far-end RX character loss), pack 02 back to damaged 0/2 (559-of-562 twice) — proving the dongle's far-end loading enabled pack 02's single clean 3420 read (bidirectional end-loading confirmation for the 120 Ω item). BLIND-WRITE 3420 mV → pack 01 per user instruction (14:28:16): fired in a confirmed quiet window, late bare ack +230 ms = the accepted-write signature (ack-level only — pack not identified, read-back ungradable, value probably stored); read-back attempt returned no block (expected per the return-path analysis). Hard-reset of pack 01 parked as user's last resort — evidence says the corruption is the wire's return path (pack 01 answers every block request with full frames; 03/04 read identical blocks cleanly from the near end; settings persist across resets per the SYS_OFF cycle data), so a reset is not expected to change readability.** **Termination/ jumper round 14:38–15:01 (user at rack): the jumper was found in 120R — the Waveshare default-NC assumption was wrong for this unit, so every earlier measurement ran with our end terminated; moved to NC for testing.** NC-mode results: pack 02 readability improved (3 clean reads incl. 2 consecutive at close; 0 clean ever in 120R-no-dongle), pack 01 unchanged (0/28 across all conditions — the 1↔2 interlink/port remains the suspect; reseat pending), 03/04 control reads clean either state (parser exonerated live). User hard-reset the whole rack sequentially 0→4 (first-ever master reset): house bank survived cleanly (SOC 61, 1.5 kW flowing, alarm silent), the sniffer's incident window froze and released correctly (14:56:22). **Hard reset ≠ SYS_OFF for parameters (documented in PROTOCOL_REFERENCE): pack 02's 0x4C read 3500 post-reset (was 3420 on three clean reads) — its stored value did not survive the button reset — while 03/04 held 3430 through the same procedure. Pack 02 restored to 3420 (write ack +231 ms, read-back PASS first press, 15:00–15:01); pack 01's 3420 write remains ack-level (block path still unreadable through everything incl. its own reset and the full-rack reset).** Jumper stays NC pending counter soak (zero checksum errors so far, master polling indifferent). **Gap-report + long-gap-aim round (PR #80 MERGED 16:08 — fall-forward single line incl. the sanitizer, the Link Gap Report sensor, and the aim; #75 closed as superseded): Link Gap Report text sensor (60 s: n/min/avg/max + the 32-gap sequence; live-measured: regular idle 470–530 ms, one ~1100 ms rotation gap every 3rd slot) + block reads now aim at the rotation's long window (115 ms confirm — above the 100 ms frame-pause ceiling — firing phase-predicted or ≥600 ms detected, fire line labels the window class; non-aim oracle paths unchanged, 3 new aim scenarios pin predicted-fire/no-long-fallback/mid-flight-detect). LIVE VERDICT 16:07: pack 01 read clean TWICE CONSECUTIVELY — 0x4C = 3420 mV VERIFIED, its first clean blocks after 0/50+ across every condition (the far-pack damage was timing geometry: ~585 ms answer + ~230 ms turnaround fits only a ~1100 ms long gap) — all four packs now read-verified (01/02 = 3420, 03/04 = 3430 factory). All 8 host suites green -Wall -Wextra. **TeleControl op round EXECUTED LIVE 17:18-17:31 (the paused round, run step-by-step with the operator at the rack; client = PR #82 reviewer-gated: their blocker verdict caught the telemetry-as-probe-answer hole pre-flash, the 400 ms bound + measured-delay verdict lines + the shape-blind structural acceptance followed; PR #83 = mid-test intake generalization under retro-review, PR #84 = wake-pair sent-line label fix, both verdict-pending, no merge until ruled): SYS_OFF on pack 04 sniffer-sent and landed (probe answered 277 ms, id 4A473235, write acked +228 ms) → pack dark ~3 min under 11.6 kW load (RS485 current exactly 0.00 A, comms answering polls at 2-4 s age, master 5-of-5 the whole time, house unblinking, operator LED observations: probe alone = brief doubled blink, SYS_OFF = all dark) → LMCHG wake pair sniffer-sent (disable +228, enable +229 ms acks, 2 s pair, recovery immediate, current rejoined -42.9 A) → 0x4C = 3430 mV held through the full cycle (reboot ≠ factory reset, proven by our own frames). The dark-window counter-datum to the idle-with-sense theory recorded in PROTOCOL_REFERENCE. Operator tooling: ~/press.sh + rev53b/sys-off-driver.py.** Review fixes live in this build:** per-client auto-off predicate (`op_just_finished` — a stale Acked can no longer kill the second operation), ONE shared TX gate (bidirectional 4 s cooldown + clean-pair baseline), accepted-only write recording. Counters through step 4: writes 1 / reads 1, anomalies 0, checksum errors 0, 4/4 packs. Generic debug surface included: Debug Read Parameter select (118 u16 params 0x07–0x7C, labels from the vendor XML via `gen_param_menus.py`, which guards the index mapping), raw-value publishing, allowlist-keyed debug writes (one reviewed entry per param; XML has no min/max so bounds stay hand-curated). Per-pack 0x4C sensors publish on trusted reads only (03/04 = 3430; **01/02 pending the 120 Ω rack inspection** — their long block answers are position-correlated damaged from this tap while short frames stay perfect; inspection checklist + vendor research on PR #69). Controls: Link Operate Enable / Target Pack / read+write buttons, all MQTT-stripped (web UI or encrypted native API only — the aioesphomeapi driver scripts in the session tooling use the device's ota/api key). **Ride-gap instrument round (PR #88, flashed 2026-11-11 20:42, config hash `0x725dfc6e` built 20:33 worktree — fall-forward, after-the-fact review pending): per-read gap-length record (`D547 NN ride gap NNNNms`) beside the `start+Nms` line, latch at fire, completed by the first master request, consumed once — oracle-pinned exact-length/placement/single-consumption, all 9 suites green, ELF string-verified, reboot verified via MQTT uptime. First live data: pack 02 4-for-4 clean (`start+524`, 3420), pack 01 20+ consecutive fails across clip (557–561/562) and shred families, shreds on both short (514/518 ms) and long (1734/1737 ms) ride gaps — gap length decided nothing on 01's slot tonight; block content byte-identical in every shred (fingerprint + `073-BLC06` label), pre- and post-reset.** **Pack 01 full-reset A/B (operator, both procedures, wire captured): Procedure A bus-attached (DIP standalone, 6 s hold, DIP back): pack-1-only mute 19.6 s, rest of bank untouched, and standalone provably does NOT seize the bus. Procedure B bus-detached: bank-wide Link comms halt ~47 s (all four packs' response ages 21–27 s, watchdog trip/recover) — two readings from the far-end sniffer (chain break at pack 01 vs master going quiet), resolved by the CAN side: the master kept CAN-reporting to the inverter the whole window and its own reported cell delta went nonsense-flat mid-outage recovering at the cadence resume → master alive, all packs lost = chain-break: the Link bus is a daisy chain, pack 01 is the first unit after the master. OPERATIONAL RULE: never unplug pack 01's Link cable under load (bank-wide comms halt, ~47 s observed). Read signature unchanged after both resets (same clips, same shreds, same delays) — CONSISTENT WITH wire/slot-local corruption: the resets show configuration is not the cause, not where the corruption happens; post-reset 0x4C most likely old-gen factory 3500 mV (pack 02's same-day reset measured it; operator confirms the procedure factory-defaults) — off the 3420 target, read path cannot verify until the physical layer is addressed. Pack 01 clean reads: 16:07/16:43 ONLY (the #80 aim-fix window); 0/50+ before, 0/20+ after; tonight's failures gap-independent (476–1737 ms) and survived both factory restores.** **Evening physical-layer rounds 21:14-22:03 (operator at rack, all wire-captured): plug-order reseat, DIP-slave#1-position 6 s hold, and a full-bank in-place reset each left pack 01's read signature unchanged (clips/shreds, gap-independent - clips on 1736 ms gaps) - then THE decisive experiment: topology flipped to master→04→03→02→01→ESP32 and pack 01 read CLEAN 4/4 from the position adjacent to the sniffer while pack 04 stayed clean as chain head at the longest path (its answer crosses every cable, so a single lossy segment cannot explain the pattern; damage moved to mid-bus: 02 0/7 incl. a 440/562 deep clip, 03 one shred) - the corruption is POSITION/TOPOLOGY-BOUND, NOT PACK-INTRINSIC (mid-bus failures block the stronger claim; reads as transmitter-position × impedance-step interaction = a reflection problem, which is also why termination moved the results); it followed the topology in both directions. First fully verified write for pack 01 through that clean path: 3420 mV, ack +230 ms, read-back PASS (no reset since - the value holds). 120 Ω far-end termination then proved HARMFUL (01's streak lost instantly, 03 damaged, 04 clean); scope at the ESP32 bus tap showed clean square waves - BUT an unsynchronized roll-through of general traffic, so it proves only ordinary traffic clean at the tap, not the medium during failing answers. Key operator observation: the master's exchanges with all four slaves have NEVER shown corruption in any capture, in any topology - only the sniffer-initiated 562-char block reads clip/shred anywhere. Suspect narrowed to the long-frame context and the never-observed stage: the SNIFFER'S OWN RECEIVE CHAIN (transceiver RO output into the ESP32) - named follow-up, declined for the night; firmware receive path excluded by the reviewer (rx_buffer_size 1024 @ 10 ms feed cannot drop chars at 9600 on a 562-char frame). CAN-side across both reset days: MASTER SETTINGS CHANGE NOT VISIBLE ON CAN (0x351 charge-voltage-max constant 55.5 V since 2026-11-09 through every reset; steady 250 A / 500 A / module count 5 restored each time; dark windows only transient fail-safe curtailment - charge limit re-ramping from near-zero, discharge to 100 A - inverter told the same limits once comms return). Topology restored to the original master→01→02→03→04→ESP32 - old signature reproduced within one matrix (01 shredded ×2, 02 clean ×2) and pack 02 restored through the clean window: 3420 written, ack +230 ms, read-back PASS (22:03). FINAL BANK: 01=3420 ✓ 02=3420 ✓ 03/04=3430 ✓ ALL FOUR AT TARGET (03/04 factory = target). Open physical item: 01's read path shreds in the original topology - the sniffer receive-chain (RO) observation; terminator state after the final rework unverified (last touched 120R ~21:45) - jumper check is the first rack action. Never-unplug rule now measured and qualified: every deliberate rewire halted bank comms 1-2.5 min, all at the evening trough (~18 A / ~1 kW, operator present, survivable; CAN curtailed during dark windows) - stands for unattended or loaded contexts; rack reconfiguration only with the operator present at low load. Topology change recorded as a DIAGNOSTIC NOTE, not an operational rule (reviewer correction): strongest diagnostic instrument in this bank's kit. Target-selection race in the read driver noted once; armed-line target check is now standard on every sweep.** Unexercised: power-on reset (closes at final placement), forced safe-mode (ledgered) |
| `rack-solar-bridge.local` | `firmware/rack-solar-site/esphome-smartshunt-epever-ble-v2/rack-solar-bridge.yaml` | **ACTIVE** — production (BLE SmartShunt + EPEVER), flashed 2026-11-01 with shared availability.h state-flip-on-success semantics (first on-device run; post-flash verified: all 6 stamps online, single clean reboot, data live). Async MQTT NOT yet adopted — see issue #45 |
| — | `firmware/rack-solar-site/esphome-smartshunt-epever-v2/` | **NOT DEPLOYED** — VE.Direct variant, not in use (user confirmed). Do not flash. |
| — | `firmware/rack-solar-site/esphome-smartshunt-epever/` | **LEGACY v1** — superseded |

Do not flash the VE.Direct variant: it targets the same device name (`rack-solar-bridge.local`) and would overwrite the BLE firmware. Both variants build for `rack-solar-bridge`; only the BLE v2 firmware is running. Verified via MQTT topic set (`rack-solar/smartshunt/#` carries the BLE variant's 6-topic set; the VE.Direct variant publishes 18 topics incl. `battery_temperature`, `depth_*`).

## Coding Guidelines

When adding functionality that's similar to existing code, always propose consolidation first. Don't duplicate - extend or refactor. Before writing new code, search for existing similar patterns and reuse them. If you find yourself about to copy-paste logic for a third time, stop and refactor into a shared helper first.

## Building and Flashing

```bash
# Deye site (Pylontech CAN + RS485)
# NOTE: build secrets live in firmware/deye-site-v2/esphome/secrets.yaml — a
# LOCAL, git-ignored file in each esphome dir (see the per-dir .gitignore;
# ignored files never show in git status). It is the durable/authoritative
# copy for deye-bms-can builds incl. its ota_encryption_key; never commit,
# never rely on temp-worktree copies. bms-link-sniffer was flashed 2026-11-10
# with a FRESH per-device set (kept by the user, ~/bms-link-sniffer-secrets.yaml).
cd firmware/deye-site-v2/esphome
esphome compile deye-bms-can.yaml
esphome upload deye-bms-can.yaml --device deye-bms-can.local

# Deye site second device (BMS073 Link-bus sniffer — do not flash until the
# board arrives and the Link-bus clip-on checklist in #59 has been run)
esphome compile bms-link-sniffer.yaml

# Rack solar (BLE — production)
cd firmware/rack-solar-site/esphome-smartshunt-epever-ble-v2
esphome compile rack-solar-bridge.yaml
esphome upload rack-solar-bridge.yaml --device rack-solar-bridge.local

# Rack solar (VE.Direct — NOT DEPLOYED, do not flash; would overwrite BLE firmware on same device)
cd firmware/rack-solar-site/esphome-smartshunt-epever-v2
esphome compile rack-solar-bridge.yaml
esphome upload rack-solar-bridge.yaml --device rack-solar-bridge.local
```

## Host Tests (`firmware/deye-site-v2/esphome/tests/`)

Desktop oracle tests run directly with clang, no esphome toolchain needed:

```bash
cd firmware/deye-site-v2/esphome
for t in test_breadcrumbs test_discovery_driver test_rs485_info_decode test_rs485_temp_decode test_bms_link_sniffer_core test_bms_link_sniffer_inject test_value_pacing; do
  clang++ -std=gnu++20 -Wall -Wextra -I. -Itests/stubs -Iincludes tests/$t.cpp -o /tmp/$t && /tmp/$t
done
```

- **`test_discovery_driver.cpp`** — the two-stream discovery driver: 15-tick convergence (36 CAN + 261 RS485 sites), starvation negative control (pins the pre-#44 semantics at 132 ticks / 2,637 publishes), pool-rejection resume, reconnect + short-gap cursor rewind, yaml-exact button blast. Models stream sizes, not real payloads — the on-device retained-config count (285) is the real-stream proof.
- **`test_breadcrumbs.cpp`** — breadcrumb stage marks, snapshot/report gating, result-controllable stub MQTT client
- **`test_rs485_info_decode.cpp`** / **`test_rs485_temp_decode.cpp`** — RS485 INFO + temperature decoders
- **`test_value_pacing.cpp`** — issue #53 pacing oracle: pool-only admission (the 29-slot pool is the only gate, concurrent-drain sim), fixed slot occurrences (change types included), commit-on-acceptance retry, pass stops at the first client refusal, storm fairness + bounded cell recovery, flap resilience (slots immovable through 30 s flaps; reconnect catch-up burst bounded by the pass stop; post-flap steady passes fit the pool), slow-drain shaping

NOTE: do NOT add `-I../../shared/esphome` to these compiles — on macOS's case-insensitive filesystem it shadows the SDK's own `<Availability.h>` and corrupts the libc headers. The shared headers resolve via relative includes from the test files.

## Architecture

All v2 firmware uses the same pattern: YAML (component wiring) + C++ header handlers (logic).

### Shared Headers (`firmware/shared/esphome/`)

- **`hysteresis.h`** — `HysteresisFloat`/`Int`/`Bool`/`String` with threshold + heartbeat logic; issue #53 adds `decide()/commit()` commit-on-acceptance and fixed slot heartbeats (`assign_slot`, k*H/N occurrence spread, outage-proof, strings included) — deye consumes the new API, legacy `check()` keeps rack-solar/epever untouched
- **`availability.h`** — `AvailabilityTracker` for stale detection; `stale` (data-age truth) flips unconditionally while the retained online/offline stamp flips only on accepted publishes; heartbeat republishes in both states (CAN 10s via diagnostics, RS485 30s repair pass)
- **`mqtt_helpers.h`** — `build_ha_sensor_payload()`, `build_ha_binary_sensor_payload()`, `PublishPacer` (1 ms producer pacing under async), `PublishBudget` (per-tick discovery budget)
- **`breadcrumbs.h`** — RTC noinit forensic breadcrumbs: sticky stage marks survive task-watchdog resets, scope guards, boot snapshot with a checked retried report publish through the normal client path (deye-bms-can only; host-tested)
- **`epever_handler.h`** — EPEVER MPPT handler (shared by both rack-solar variants)

### Deye Site v2 (`firmware/deye-site-v2/esphome/`)

- **`includes/pylontech_can.h`** — CAN frame parsing (0x351, 0x355, 0x359, 0x35C, 0x370), diagnostics, stale detection, resumable budgeted discovery stream + 10s availability heartbeat
- **`includes/pylontech_rs485.h`** — RS485 polling state machine, per-battery data, stack aggregation, resumable budgeted discovery stream + 10s pool-gated staggered value pass (issue #53: fixed slot occurrences per instance, stamps-first repair pass every 30s, pack rotation + stack position alternation, pass stops at the first pool refusal)
- **`includes/pylontech_protocol.h`** — Shared protocol pure helpers + RS485 INFO decoders (0x42 analog / 0x44 alarm), host-oracle tested (`tests/`)
- **`includes/bms_link_sniffer_core.h` / `bms_link_sniffer.h`** — BMS073 Link-bus sniffer (#58 listener + #59 guarded write client): pure core (frame scan/classify/pairing/stats/watchdog/anomaly ring; on top: write allowlist {0x4C 3400-3500 mV}, byte-exact captured builder, gap-injection state machine — one frame per press, 4 s cooldown + clean-pair + fresh-cadence + 150..250 ms silence gates, all latches long-uptime-safe (10 ms tick clears them; pair-count baselines, not idle timestamps) — all host-oracle tested) + thin UART wrapper (10 ms feed, event/text push into native ESPHome entities, gated TX via flow_control). Part 1 (#60, merged) is the passive listener; part 2 (#61, merged — the staged-flash deployment order is in the deployment table above and PR #61's checklist) adds the guarded TX path

### Rack Solar BLE v2 (`firmware/rack-solar-site/esphome-smartshunt-epever-ble-v2/`)

- **`includes/smartshunt_ble_handler.h`** — 6 BLE sensors, hysteresis, BLE connectivity tracking, discovery

### Rack Solar VE.Direct v2 (`firmware/rack-solar-site/esphome-smartshunt-epever-v2/`)

- **`includes/smartshunt_handler.h`** — 18 sensors, text validation, relay debounce, bitflip tracking
- **`includes/solar_validation.h`** — Stability windows, text validation, rate limiting

### Handler Pattern

Handlers are allocated in `on_boot` and accessed via `void*` globals with null checks:

```cpp
// on_boot (YAML)
id(handler) = (void*) new HandlerType();

// sensor on_value (YAML)
- lambda: 'if (!id(handler)) return; ((HandlerType*)id(handler))->handle_xxx(x);'

// intervals (YAML) — 3s discovery fallback, 5-30s stale checks
```

### MQTT Discovery (deye-bms-can: async + budgeted)

`deye-bms-can` runs with `idf_send_async: true` (issue #42 fix, merged 1919071): publishes enqueue into esphome's 29-usable-slot pool (MQTT_QUEUE_LENGTH=30) and a dedicated mqtt task performs all network I/O — the main loop task never blocks on a publish, so the mechanism behind the 2026-11-01 crash is closed (the device survived a 45 s stall test 2026-11-01; the original 02:00 trigger itself is still unidentified). Consequences: "accepted" means queued, not delivered; queued-but-unsent items are discarded on disconnect, so every new connection rewinds the discovery cursors.

Discovery streams (CAN + RS485) are resumable: per-handler cursor + shared `PublishBudget(20)` per tick converges in ~15 ticks / 297 messages; a completed stream is a skip-all no-op. The 3s tick order is: crash report first → once-per-connection resets → budgeted discovery (incomplete passes log one WARN and resume next tick). `rack-solar-bridge` is still on the synchronous publish path — async adoption is issue #45.

ESPHome's `on_connect` trigger doesn't fire reliably on ESP32-S3 + ESP-IDF 5.5.5. All v2 firmware use a 3s interval polling pattern with `mqtt_disconnect_count` debounce (6s) for broker-restart recovery.

## CAN Protocol Reference

| Arb ID | Content |
|--------|---------|
| 0x351  | Voltage/current limits (V_charge_max, I_charge, I_discharge, V_low) |
| 0x355  | SOC/SOH percentages |
| 0x359  | Status flags (bitfield) |
| 0x35C  | Charge/discharge enable flags |
| 0x370  | Cell voltage extremes (min/max) and temperatures |

All values are little-endian 16-bit, divided by 10 (voltage/current) or 1000 (cell voltage in mV).

## Legacy Python Scripts

Archived to `archive/python-prototypes/` for reference only. Not actively maintained.

- `pylon_can2mqtt.py` — CAN bus bridge (replaced by deye-site-v2)
- `pylon_rs485_monitor.py` — RS485 cell monitoring (replaced by deye-site-v2)
- `deye_modbus2mqtt.py` — Modbus-TCP inverter bridge
