# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a multi-protocol MQTT bridge for Deye/Shoto solar systems:
- **BMS monitoring** via CAN bus and RS485 (Pylontech protocol)
- **Inverter monitoring** via Modbus-TCP

All data is published to MQTT with Home Assistant auto-discovery.

## Running

```bash
# Run the CAN-to-MQTT bridge (requires can0 interface up)
./pylon_can2mqtt.py

# Run the RS485 monitor with MQTT publishing
./pylon_rs485_monitor.py --mqtt --loop

# Run the RS485 monitor with debug logging
./pylon_rs485_monitor.py --mqtt --loop --debug-log /var/log/batt_balance.log

# Run the Modbus-TCP inverter bridge
./deye_modbus2mqtt.py --mqtt --loop

# Debug/decode raw CAN frames
./pylon_decode.py

# Analyze discharge limits from candump log
./show_discharge_limits.sh can.log
```

## Environment Variables

The bridges require MQTT configuration via environment variables:
- `MQTT_HOST` - MQTT broker address (default: localhost)
- `MQTT_PORT` - MQTT broker port (default: 1883)
- `MQTT_USER` - MQTT username (optional)
- `MQTT_PASS` - MQTT password (optional)

## Dependencies

- Python 3.x with `python-can`, `paho-mqtt` (v2.x), `pyserial`, `pymodbus`
- Linux SocketCAN (`can0` interface must be configured)
- RS485 USB adapter (e.g., `/dev/ttyUSB0`)
- Modbus-TCP gateway for inverter monitoring

## Architecture

**pylon_can2mqtt.py** - Main production bridge:
- Decodes CAN frames from Pylontech protocol (arbitration IDs 0x351, 0x355, 0x359, 0x370)
- Publishes to MQTT topics under `deye_bms/` prefix with hysteresis/rate-limiting
- Publishes Home Assistant MQTT Discovery configs under `homeassistant/sensor/`
- Handles MQTT reconnection and republishes discovery on reconnect
- Uses Last Will Testament for availability tracking

**pylon_rs485_monitor.py** - RS485 battery monitor:
- Reads individual cell voltages, temperatures, and alarm status via RS485
- Supports 3 batteries in parallel configuration (configurable)
- Publishes to MQTT topics under `deye_bms/rs485/` prefix
- Publishes Home Assistant MQTT Discovery configs
- Reports balancing status, cycle counts, and per-cell data

**pylon_decode.py** - Development/debug tool:
- Prints decoded CAN frames with optional repeat suppression
- Useful for reverse-engineering unknown frame types

**deye_modbus2mqtt.py** - Inverter Modbus-TCP bridge:
- Polls Deye SG04LP3 (and compatible) inverters via Modbus-TCP
- 60+ sensors: PV, battery, grid, load, temperatures, energy totals
- Three scan groups (fast/normal/slow) for efficient polling
- Fully configurable via environment variables or CLI arguments
- Supports Solarman unique_id format for history preservation when migrating

## CAN Protocol Reference

| Arb ID | Content |
|--------|---------|
| 0x351  | Voltage/current limits (V_charge_max, I_charge, I_discharge, V_low) |
| 0x355  | SOC/SOH percentages |
| 0x359  | Status flags (bitfield) |
| 0x370  | Cell voltage extremes (min/max) and temperatures |

All values are little-endian 16-bit, divided by 10 (voltage/current) or 1000 (cell voltage in mV).

## ESPHome (Future Hardware)

The `esphome/` directory contains configuration for Waveshare ESP32-S3-RS485-CAN board to replace the Pi-based bridge:

```bash
cd /root/esphome

# Edit secrets.yaml first with WiFi/MQTT credentials

# Compile firmware
esphome compile deye-bms-can.yaml

# Flash via USB (first time)
esphome run deye-bms-can.yaml

# View logs
esphome logs deye-bms-can.yaml
```

Hardware pinout: CAN TX=GPIO15, CAN RX=GPIO16, 500kbps.
See `esphome/README.md` for full documentation.
