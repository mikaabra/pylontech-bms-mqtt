# Pylontech BMS & Solar Inverter MQTT Bridge

A comprehensive solar battery monitoring system supporting multiple hardware platforms and protocols. This repository provides ESPHome firmware and legacy Python scripts for monitoring Pylontech/Shoto batteries and solar inverters via CAN bus, RS485, and Modbus-TCP.

---

## 📁 Repository Structure

This repository contains **production ESPHome firmware** for three separate implementations across two installation sites:

```
pylontech-bms-mqtt/
├── firmware/                    # ESPHome firmware (PRODUCTION)
│   ├── deye-site/              # Pylontech + Deye inverter monitoring
│   ├── epever-site/            # Pylontech → EPever protocol translation
│   └── rack-solar-site/        # SmartShunt + EPEVER MPPT monitoring
│
├── archive/                     # Legacy code (Python prototypes)
│   └── python-prototypes/      # Original Python scripts
│
├── tools/                       # Utility scripts
│   ├── pylon_decode.py         # CAN frame decoder
│   ├── mqtt_display.py         # MQTT console monitor
│   └── ...                     # Other diagnostic tools
│
├── docs/                        # Documentation
│   ├── guides/                 # Troubleshooting, installation
│   ├── sessions/               # Development session logs
│   ├── reviews/                # Code reviews
│   └── analysis/               # Analysis documents
│
└── hardware/                    # Hardware specifications
```

**See [firmware/README.md](firmware/README.md) for production firmware documentation.**

---

## Implementations

| Site | Hardware | Location | Status | Purpose |
|------|----------|----------|--------|---------|
| **Deye Site** | ESP32-S3 Waveshare | Deye site | **PRODUCTION** | Monitor Pylontech + Deye inverter |
| **EPever Site** | ESP32-S3 Waveshare | EPever site | **PRODUCTION** | Translate Pylontech to EPever BMS-Link |
| **Rack Solar** | ESP32-S3 Waveshare | EPever site | **PRODUCTION** | SmartShunt + EPEVER MPPT monitoring |

---

## Quick Start (Production Firmware)

```bash
# 1. Navigate to your site's firmware
cd firmware/deye-site    # or epever-site, rack-solar-site

# 2. Configure secrets
cp secrets.yaml.example secrets.yaml
# Edit secrets.yaml with your WiFi and MQTT credentials

# 3. Compile firmware
esphome compile deye-bms-can.yaml

# 4. Flash via USB (first time)
esphome run deye-bms-can.yaml

# 5. Subsequent updates via OTA
esphome upload deye-bms-can.yaml --device <IP_ADDRESS>
```

---

## Features

### All Implementations
- **WiFi Connectivity**: Automatic reconnection, fallback AP mode
- **MQTT Publishing**: Home Assistant auto-discovery, birth/will messages
- **OTA Updates**: Password-protected firmware updates
- **Diagnostics**: Heap monitoring, stale data detection
- **Rate Limiting**: Hysteresis filters to reduce MQTT traffic

### Deye Site (deye-site)
- **CAN Bus**: Pylontech protocol at 500kbps (listen-only mode)
- **RS485**: Individual cell voltages, temperatures, alarms
- **Sensors**: 100+ entities including per-cell monitoring

### EPever Site (epever-site)
- **Protocol Translation**: Pylontech CAN → EPever Modbus
- **Inverter Control**: SOC-based priority switching
- **Modbus Gateway**: RTU over TCP communication

### Rack Solar (rack-solar-site)
- **Multi-Source**: Victron SmartShunt + EPEVER MPPT
- **Data Validation**: Bitflip detection and corruption filtering
- **VE.Direct**: Native Victron protocol support

---

## Legacy Python Scripts

The original Python scripts have been archived to `archive/python-prototypes/`:

- `pylon_can2mqtt.py` - CAN bus bridge
- `pylon_rs485_monitor.py` - RS485 cell monitoring
- `deye_modbus2mqtt.py` - Modbus-TCP inverter bridge

These are kept for reference but are not actively maintained.

---

## Documentation

- **[ENVIRONMENTS.md](ENVIRONMENTS.md)** - Detailed environment documentation
- **[firmware/deye-site/README.md](firmware/deye-site/README.md)** - Deye site firmware
- **[firmware/epever-site/README.md](firmware/epever-site/README.md)** - EPever site firmware
- **[firmware/rack-solar-site/README.md](firmware/rack-solar-site/README.md)** - Rack solar firmware
- **[docs/guides/](docs/guides/)** - Troubleshooting and installation guides

---

## Hardware Requirements

- **Board**: Waveshare ESP32-S3-RS485-CAN (8MB Flash, 2MB PSRAM)
- **CAN Interface**: Built-in TWAI controller (GPIO15/16)
- **RS485 Interface**: Built-in transceiver (GPIO17/18/21)
- **Power**: 5V USB or 12V DC input

---

## License

MIT License - See [LICENSE](LICENSE)

---

**Last Updated**: 2026-02-21
