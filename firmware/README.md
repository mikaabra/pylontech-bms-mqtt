# Firmware Directory

This directory contains all ESPHome firmware implementations for the Pylontech BMS MQTT Bridge project.

## Directory Structure

```
firmware/
├── deye-site/           # Deye site: Pylontech + Dye inverter monitoring
│   ├── esphome/
│   │   ├── deye-bms-can.yaml      # Main ESPHome configuration
│   │   ├── secrets.yaml.example   # Template for secrets
│   │   ├── includes/              # C++ helper functions
│   │   └── custom_components/     # Custom ESPHome components
│   └── README.md
│
├── epever-site/         # EPever site: Protocol translation
│   ├── esphome-epever/
│   │   ├── epever-can-bridge.yaml  # Main configuration
│   │   ├── secrets.yaml.example    # Template for secrets
│   │   ├── tools/                  # Utility scripts
│   │   ├── HLD.md                  # High-level design
│   │   └── LLD.md                  # Low-level design
│   └── README.md
│
└── rack-solar-site/     # Rack solar: SmartShunt + EPEVER MPPT
    ├── esphome-smartshunt-epever/
    │   ├── rack-solar-bridge.yaml  # Main configuration
    │   ├── secrets.yaml.example    # Template for secrets
    │   └── includes/               # Helper functions
    └── README.md
```

## Quick Reference

| Site | Main YAML | Hardware | Purpose |
|------|-----------|----------|---------|
| Deye | `deye-site/esphome/deye-bms-can.yaml` | ESP32-S3 Waveshare | Monitor Pylontech batteries + Deye inverter |
| EPever | `epever-site/esphome-epever/epever-can-bridge.yaml` | ESP32-S3 Waveshare | Translate Pylontech CAN → EPever Modbus |
| Rack Solar | `rack-solar-site/esphome-smartshunt-epever/rack-solar-bridge.yaml` | ESP32-S3 Waveshare | Monitor SmartShunt + EPEVER MPPT |

## Common Commands

```bash
# Compile firmware
cd firmware/<site>
esphome compile <config>.yaml

# Flash via USB (first time)
esphome run <config>.yaml

# Update via OTA
esphome upload <config>.yaml --device <IP>

# View logs
esphome logs <config>.yaml --device <IP>
```

## Configuration

Each site requires a `secrets.yaml` file with:

```yaml
wifi_ssid: "YOUR_WIFI_SSID"
wifi_password: "YOUR_WIFI_PASSWORD"
mqtt_host: "YOUR_MQTT_BROKER_IP"
mqtt_user: "YOUR_MQTT_USER"
mqtt_password: "YOUR_MQTT_PASSWORD"
ota_password: "YOUR_OTA_PASSWORD"
```

Copy from `secrets.yaml.example` in each directory and fill in your values.

## Documentation

- Individual site READMEs contain detailed documentation
- See `docs/` for troubleshooting guides
- See `hardware/` for hardware specifications
