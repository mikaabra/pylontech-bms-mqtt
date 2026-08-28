# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPHome firmware for solar battery monitoring on ESP32-S3 hardware:
- **Deye Site** — Pylontech BMS via CAN bus + RS485 (cell voltages, temperatures, alarms)
- **Rack Solar** — Victron SmartShunt (BLE or VE.Direct) + EPEVER MPPT (Modbus/RS485)

All data published to MQTT with Home Assistant auto-discovery.

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

# Rack solar (VE.Direct — alternative)
cd firmware/rack-solar-site/esphome-smartshunt-epever-v2
esphome compile rack-solar-bridge.yaml
esphome upload rack-solar-bridge.yaml --device rack-solar-bridge.local
```

## Architecture

All v2 firmware uses the same pattern: YAML (component wiring) + C++ header handlers (logic).

### Shared Headers (`firmware/shared/esphome/`)

- **`hysteresis.h`** — `HysteresisFloat`/`Int`/`Bool`/`String` with threshold + heartbeat logic
- **`availability.h`** — `AvailabilityTracker` for stale detection + on-connect republish
- **`mqtt_helpers.h`** — `build_ha_sensor_payload()`, `build_ha_binary_sensor_payload()`, `PublishPacer`
- **`epever_handler.h`** — EPEVER MPPT handler (shared by both rack-solar variants)

### Deye Site v2 (`firmware/deye-site-v2/esphome/`)

- **`includes/pylontech_can.h`** — CAN frame parsing (0x351, 0x355, 0x359, 0x35C, 0x370), diagnostics, stale detection
- **`includes/pylontech_rs485.h`** — RS485 polling state machine, per-battery data, stack aggregation
- **`includes/pylontech_protocol.h`** — Shared protocol constants

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

### MQTT Discovery

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
