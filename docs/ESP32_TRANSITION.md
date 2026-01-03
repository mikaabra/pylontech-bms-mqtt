# ESP32 Transition Guide

This document contains all information needed to transition from the Raspberry Pi to the Waveshare ESP32-S3-RS485-CAN board for the Deye BMS monitoring.

## Current Setup (Raspberry Pi)

### Hardware
- Raspberry Pi 4
- Openmoko USB-CAN adapter (gs_usb compatible)
- FTDI USB-RS485 adapter (RS485)

### Services Running
```bash
# CAN-to-MQTT bridge
./pylon_can2mqtt.py

# RS485 monitor with MQTT (typical command)
./pylon_rs485_monitor.py --mqtt --loop --interval 30 --debug-log /var/log/batt_balance.log
```

### MQTT Topics Published
- `deye_bms/` - CAN data (SOC, limits, cell extremes)
- `deye_bms/rs485/` - RS485 data (per-cell voltages, temps, balancing)

---

## Target Setup (ESP32-S3-RS485-CAN)

### Hardware: Waveshare ESP32-S3-RS485-CAN
- CAN: TX=GPIO15, RX=GPIO16, 500kbps
- RS485: TX=GPIO17, RX=GPIO18, 115200 baud
- Power: USB-C or 5V

### Wiring

| Signal | BMS RJ45 Pin | ESP32 Connection |
|--------|--------------|------------------|
| CAN-H | Pin 1 | CAN H terminal |
| CAN-L | Pin 2 | CAN L terminal |
| RS485-A | Pin 7 | RS485 A terminal |
| RS485-B | Pin 8 | RS485 B terminal |
| GND | Pin 3 or 4 | GND terminal |

### ESPHome Configuration
File: `/root/esphome/deye-bms-can.yaml`

**Key substitutions to verify/modify:**
```yaml
substitutions:
  device_name: deye-bms-can
  friendly_name: "Deye BMS"
  can_prefix: deye_bms
  rs485_prefix: deye_bms/rs485
  num_batteries: "3"
  pylontech_addr: "2"
```

---

## Pre-Transition Checklist

### 1. Update secrets.yaml
Edit `/root/esphome/secrets.yaml`:
```yaml
wifi_ssid: "YOUR_WIFI_SSID"
wifi_password: "YOUR_WIFI_PASSWORD"
mqtt_host: "192.168.200.217"  # Your MQTT broker
mqtt_user: "YOUR_MQTT_USER"
mqtt_password: "YOUR_MQTT_PASSWORD"
ota_password: "YOUR_OTA_PASSWORD"
```

### 2. Compile Firmware
```bash
cd /root/esphome
esphome compile deye-bms-can.yaml
```

### 3. Flash ESP32 (First Time - USB)
```bash
esphome run deye-bms-can.yaml
```
Select the USB port when prompted.

### 4. Verify WiFi Connection
Check router/DHCP for new device, or use:
```bash
esphome logs deye-bms-can.yaml
```

---

## Transition Steps

### Step 1: Stop Pi Services
```bash
# Stop any running Python scripts
pkill -f pylon_can2mqtt
pkill -f pylon_rs485_monitor
```

### Step 2: Disconnect Wires from Pi HAT
- Note current wiring before disconnecting
- CAN-H, CAN-L from CAN HAT
- RS485 A, B from USB adapter

### Step 3: Connect to ESP32
Wire as per table above. The ESP32 has screw terminals for easy connection.

### Step 4: Power ESP32
Connect USB-C power. Device should boot and connect to WiFi.

### Step 5: Verify Data Flow
```bash
# Check MQTT messages from ESP32
mosquitto_sub -h 192.168.200.217 -u USER -P PASS -t "deye_bms/#" -v
```

### Step 6: Check Home Assistant
Entities should remain available (same MQTT topics, same unique_ids).

---

## Feature Parity Verification

Both Python and ESPHome implementations have been verified to include:

### CAN Bus (500kbps)
| CAN ID | Feature | Python | ESPHome |
|--------|---------|--------|---------|
| 0x351 | Charge/discharge limits | ✓ | ✓ |
| 0x355 | SOC/SOH | ✓ | ✓ |
| 0x359 | Flags | ✓ | ✓ |
| 0x370 | Cell/temp extremes | ✓ | ✓ |

### RS485 (115200 baud)
| Feature | Python | ESPHome |
|---------|--------|---------|
| Cell voltages (16 per battery) | ✓ | ✓ |
| Temperatures (6 per battery) | ✓ | ✓ |
| Current, voltage, SOC | ✓ | ✓ |
| Remain/total capacity | ✓ | ✓ |
| Cycle count | ✓ | ✓ |
| Balance flags (ByteIndex 9-10) | ✓ | ✓ |
| Balance On flag check | ✓ | ✓ |
| MOSFET status | ✓ | ✓ |
| Battery state | ✓ | ✓ |
| Warnings/alarms | ✓ | ✓ |
| CW flag (status_pos+18) | ✓ | ✓ |
| Stack totals | ✓ | ✓ |

### Debug Logging
- Python: `--debug-log` writes to file
- ESPHome: No equivalent (would need custom logging component)

---

## Troubleshooting

### ESP32 Not Connecting to WiFi
1. Check secrets.yaml credentials
2. Verify WiFi is 2.4GHz (ESP32 doesn't support 5GHz)
3. Check router for MAC filtering

### No CAN Data
1. Verify wiring (CAN-H to H, CAN-L to L)
2. Check 120Ω termination if at end of bus
3. Look for CAN RX LED activity on board

### No RS485 Data
1. Verify A/B wiring (try swapping if reversed)
2. Check baud rate (115200)
3. Verify Pylontech address (default 2)

### MQTT Not Publishing
1. Check broker connectivity
2. Verify MQTT credentials
3. Check ESPHome logs for errors

### Entities Unavailable in HA
1. Check MQTT broker is receiving messages
2. Verify topic prefixes match Python scripts
3. Restart Home Assistant to pick up changes

---

## Rollback Procedure

If ESP32 doesn't work, reconnect Pi:

1. Power off ESP32
2. Reconnect USB-CAN adapter and USB-RS485 to Pi
3. Start services:
```bash
./pylon_can2mqtt.py &
./pylon_rs485_monitor.py --mqtt --loop &
```

---

## Post-Transition: Pi Redeployment

After successful ESP32 transition, the Pi will be moved to the EPever UPower-HI location for protocol debugging.

### Equipment to Take
- Raspberry Pi 4
- USB-RS485 adapter (for protocol sniffing)
- SD card with current setup

### Preparation for EPever Site
See `/root/docs/PROTOCOL_REFERENCE.md` for:
- EPever UPower-HI protocol details
- BMS-Link troubleshooting checklist
- RS485 sniffer script

---

## Key Files Reference

| File | Purpose |
|------|---------|
| `/root/esphome/deye-bms-can.yaml` | ESPHome config |
| `/root/esphome/secrets.yaml` | WiFi/MQTT credentials |
| `/root/pylon_can2mqtt.py` | Python CAN bridge (backup) |
| `/root/pylon_rs485_monitor.py` | Python RS485 monitor (backup) |
| `/root/docs/PROTOCOL_REFERENCE.md` | Protocol documentation |
| `/root/docs/ESP32_TRANSITION.md` | This file |

---

## Contact/Support

GitHub: https://github.com/mikaabra/pylontech-bms-mqtt

All code and documentation is version controlled. Check git log for recent changes.
