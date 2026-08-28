# Pylontech BMS & Solar Inverter MQTT Bridge

A comprehensive solar battery monitoring system supporting multiple hardware platforms and protocols. This repository provides ESPHome firmware for monitoring Pylontech/Shoto batteries and solar equipment via CAN bus, RS485, BLE, and Modbus.

---

## Repository Structure

```
pylontech-bms-mqtt/
├── firmware/
│   ├── deye-site-v2/esphome/     # PRODUCTION: Pylontech CAN + RS485 (ESP32-S3)
│   ├── rack-solar-site/
│   │   ├── esphome-smartshunt-epever-ble-v2/  # PRODUCTION: SmartShunt BLE + EPEVER
│   │   └── esphome-smartshunt-epever-v2/     # SmartShunt VE.Direct + EPEVER
│   └── shared/esphome/           # Shared C++ headers (hysteresis, availability, MQTT helpers)
│
├── archive/python-prototypes/    # Legacy Python scripts (reference only)
├── docs/                         # HLD, LLD, protocol reference, guides
└── tools/                        # Diagnostic scripts
```

---

## Implementations

| Site | Firmware | Hardware | Status | Purpose |
|------|----------|----------|--------|---------|
| **Deye Site** | `deye-site-v2` | ESP32-S3 Waveshare | **PRODUCTION** | Pylontech CAN bus + RS485 battery monitoring |
| **Rack Solar** | `esphome-smartshunt-epever-ble-v2` | ESP32-S3 Waveshare | **PRODUCTION** | SmartShunt (BLE) + EPEVER MPPT (Modbus) |
| **Rack Solar** | `esphome-smartshunt-epever-v2` | ESP32-S3 Waveshare | Available | SmartShunt (VE.Direct) + EPEVER MPPT |

### Shared Architecture

All v2 firmware use C++ handler classes with shared headers in `firmware/shared/esphome/`:

- **`hysteresis.h`** — `HysteresisFloat`/`Int`/`Bool`/`String` structs with threshold + heartbeat
- **`availability.h`** — `AvailabilityTracker` for stale detection + MQTT reconnect handling
- **`mqtt_helpers.h`** — `build_ha_sensor_payload()`, `build_ha_binary_sensor_payload()`, `PublishPacer`
- **`epever_handler.h`** — EPEVER MPPT Modbus handler (shared by both rack-solar variants)

Each firmware has a YAML file (component wiring) + C++ headers (handler logic). MQTT topics match the original implementations for seamless Home Assistant migration.

---

## Quick Start

```bash
# 1. Navigate to your firmware directory
cd firmware/deye-site-v2/esphome          # Deye site
cd firmware/rack-solar-site/esphome-smartshunt-epever-ble-v2  # Rack solar (BLE)

# 2. Configure secrets
cp secrets.yaml.example secrets.yaml
# Edit secrets.yaml with WiFi, MQTT, and BLE credentials

# 3. Compile
esphome compile deye-bms-can.yaml        # or rack-solar-bridge.yaml

# 4. Flash via USB (first time)
esphome run deye-bms-can.yaml

# 5. Subsequent updates via OTA
esphome upload deye-bms-can.yaml --device deye-bms-can.local
```

---

## Features

### All Implementations
- **WiFi Connectivity**: Automatic reconnection, fallback AP mode
- **MQTT Publishing**: Home Assistant auto-discovery, birth/will messages
- **OTA Updates**: Password-protected firmware updates
- **Diagnostics**: Heap monitoring, stale data detection, BLE connectivity tracking
- **Hysteresis**: Threshold-based publishing with 60s heartbeat to reduce MQTT traffic
- **Availability Tracking**: Stale detection marks HA entities unavailable when data source dies

### Deye Site (deye-site-v2)
- **CAN Bus**: Pylontech protocol at 500kbps (listen-only mode), 4 frame types (0x351, 0x355, 0x359, 0x370, 0x35C)
- **RS485**: Pylontech protocol at 9600 baud, up to 5 batteries, per-cell voltages/temps/alarms
- **State Machine**: C++ RS485 polling state machine with timeout and failure tracking
- **Entities**: 36 HA entities (11 CAN sensors + 17 binary sensors + 8 diagnostics)

### Rack Solar BLE (esphome-smartshunt-epever-ble-v2)
- **SmartShunt**: Victron BLE via `Fabian-Schmidt/esphome-victron_ble` external component
- **EPEVER MPPT**: 8 Modbus sensors via RS485
- **Entities**: 25 HA entities (6 SmartShunt + 8 EPEVER + 10 diagnostics + 1 binary)

### Rack Solar VE.Direct (esphome-smartshunt-epever-v2)
- **SmartShunt**: VE.Direct UART via `KinDR007/VictronMPPT-ESPHOME`
- **EPEVER MPPT**: Same Modbus handler as BLE variant
- **Text Validation**: Bitflip detection, stability windows, text sensor validation
- **Entities**: 40+ HA entities (18 sensors + 7 text sensors + 1 binary + diagnostics)

---

## Legacy Python Scripts

The original Python scripts have been archived to `archive/python-prototypes/`:

- `pylon_can2mqtt.py` - CAN bus bridge (replaced by deye-site-v2)
- `pylon_rs485_monitor.py` - RS485 cell monitoring (replaced by deye-site-v2)
- `deye_modbus2mqtt.py` - Modbus-TCP inverter bridge (not yet migrated)

These are kept for reference but are not actively maintained.

---

## Hardware Requirements

- **Board**: Waveshare ESP32-S3-RS485-CAN (4MB Flash)
- **CAN Interface**: Built-in TWAI controller (GPIO15 TX / GPIO16 RX, 500kbps)
- **RS485 Interface**: Built-in transceiver (GPIO17 TX / GPIO18 RX / GPIO21 DE/RE)
- **BLE**: Built-in ESP32-S3 Bluetooth radio (for SmartShunt BLE variant)
- **Power**: 5V USB or 12V DC input

---

## Documentation

- **[CLAUDE.md](CLAUDE.md)** — Development guide for AI agents
- **[docs/HLD.md](docs/HLD.md)** — High-level design
- **[docs/LLD.md](docs/LLD.md)** — Low-level design
- **[docs/ESP32_TRANSITION.md](docs/ESP32_TRANSITION.md)** — Python to ESPHome migration notes
- **[docs/PROTOCOL_REFERENCE.md](docs/PROTOCOL_REFERENCE.md)** — CAN/RS485 protocol details

---

## License

MIT License - See [LICENSE](LICENSE)
