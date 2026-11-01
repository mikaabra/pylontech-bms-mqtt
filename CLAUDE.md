# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPHome firmware for solar battery monitoring on ESP32-S3 hardware:
- **Deye Site** — Pylontech BMS via CAN bus + RS485 (cell voltages, temperatures, alarms)
- **Rack Solar** — Victron SmartShunt (BLE or VE.Direct) + EPEVER MPPT (Modbus/RS485)

All data published to MQTT with Home Assistant auto-discovery.

## Active Deployments (verified 2026-11-01)

| Device | Firmware | Status |
|--------|----------|--------|
| `deye-bms-can.local` | `firmware/deye-site-v2/esphome/deye-bms-can.yaml` | **ACTIVE** — production, ESPHome 2026.9.1, encrypted OTA, async MQTT (`idf_send_async`, issue #42 fix merged 1919071), flashed + stall-test accepted 2026-11-01, #42 closed |
| `rack-solar-bridge.local` | `firmware/rack-solar-site/esphome-smartshunt-epever-ble-v2/rack-solar-bridge.yaml` | **ACTIVE** — production (BLE SmartShunt + EPEVER), flashed 2026-11-01 with shared availability.h state-flip-on-success semantics (first on-device run; post-flash verified: all 6 stamps online, single clean reboot, data live). Async MQTT NOT yet adopted — see issue #45 |
| — | `firmware/rack-solar-site/esphome-smartshunt-epever-v2/` | **NOT DEPLOYED** — VE.Direct variant, not in use (user confirmed). Do not flash. |
| — | `firmware/rack-solar-site/esphome-smartshunt-epever/` | **LEGACY v1** — superseded |

Do not flash the VE.Direct variant: it targets the same device name (`rack-solar-bridge.local`) and would overwrite the BLE firmware. Both variants build for `rack-solar-bridge`; only the BLE v2 firmware is running. Verified via MQTT topic set (`rack-solar/smartshunt/#` carries the BLE variant's 6-topic set; the VE.Direct variant publishes 18 topics incl. `battery_temperature`, `depth_*`).

## Coding Guidelines

When adding functionality that's similar to existing code, always propose consolidation first. Don't duplicate - extend or refactor. Before writing new code, search for existing similar patterns and reuse them. If you find yourself about to copy-paste logic for a third time, stop and refactor into a shared helper first.

## Building and Flashing

```bash
# Deye site (Pylontech CAN + RS485)
cd firmware/deye-site-v2/esphome
esphome compile deye-bms-can.yaml
esphome upload deye-bms-can.yaml --device deye-bms-can.local

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
for t in test_breadcrumbs test_discovery_driver test_rs485_info_decode test_rs485_temp_decode; do
  clang++ -std=gnu++20 -Wall -Wextra -I. -Itests/stubs -Iincludes tests/$t.cpp -o /tmp/$t && /tmp/$t
done
```

- **`test_discovery_driver.cpp`** — the two-stream discovery driver: 15-tick convergence (36 CAN + 261 RS485 sites), starvation negative control (pins the pre-#44 semantics at 132 ticks / 2,637 publishes), pool-rejection resume, reconnect + short-gap cursor rewind, yaml-exact button blast. Models stream sizes, not real payloads — the on-device retained-config count (285) is the real-stream proof.
- **`test_breadcrumbs.cpp`** — breadcrumb stage marks, snapshot/report gating, result-controllable stub MQTT client
- **`test_rs485_info_decode.cpp`** / **`test_rs485_temp_decode.cpp`** — RS485 INFO + temperature decoders

NOTE: do NOT add `-I../../shared/esphome` to these compiles — on macOS's case-insensitive filesystem it shadows the SDK's own `<Availability.h>` and corrupts the libc headers. The shared headers resolve via relative includes from the test files.

## Architecture

All v2 firmware uses the same pattern: YAML (component wiring) + C++ header handlers (logic).

### Shared Headers (`firmware/shared/esphome/`)

- **`hysteresis.h`** — `HysteresisFloat`/`Int`/`Bool`/`String` with threshold + heartbeat logic
- **`availability.h`** — `AvailabilityTracker` for stale detection; stamps flip state only on accepted publishes, heartbeat republishes in both states (CAN 10s via diagnostics, RS485 30s repair pass)
- **`mqtt_helpers.h`** — `build_ha_sensor_payload()`, `build_ha_binary_sensor_payload()`, `PublishPacer` (1 ms producer pacing under async), `PublishBudget` (per-tick discovery budget)
- **`breadcrumbs.h`** — RTC noinit forensic breadcrumbs: sticky stage marks survive task-watchdog resets, scope guards, boot snapshot with a checked retried direct publish (deye-bms-can only; host-tested)
- **`epever_handler.h`** — EPEVER MPPT handler (shared by both rack-solar variants)

### Deye Site v2 (`firmware/deye-site-v2/esphome/`)

- **`includes/pylontech_can.h`** — CAN frame parsing (0x351, 0x355, 0x359, 0x35C, 0x370), diagnostics, stale detection, resumable budgeted discovery stream + 10s availability heartbeat
- **`includes/pylontech_rs485.h`** — RS485 polling state machine, per-battery data, stack aggregation, resumable budgeted discovery stream + 30s stamp repair pass
- **`includes/pylontech_protocol.h`** — Shared protocol pure helpers + RS485 INFO decoders (0x42 analog / 0x44 alarm), host-oracle tested (`tests/`)

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

`deye-bms-can` runs with `idf_send_async: true` (issue #42 fix, merged 1919071): publishes enqueue into esphome's 30-slot pool and a dedicated mqtt task performs all network I/O — the 3s loop task never blocks on a publish, so task-watchdog resets from a stalled broker are gone (stall-tested 2026-11-01). Consequences: "accepted" means queued, not delivered; queued-but-unsent items are discarded on disconnect, so every new connection rewinds the discovery cursors.

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
