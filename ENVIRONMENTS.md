# Development & Production Environments

This repository contains **two separate installation sites** with different environments, hardware, and deployment methods.

## Overview

| Aspect | Deye Site | EPever Site |
|--------|-----------|-------------|
| **Location in Repo** | `esphome/` (production) + Root directory (legacy) | `esphome-epever/` subdirectory |
| **Implementations** | **ESPHome** (production) + Python scripts (legacy/backup) | **ESPHome only** |
| **Languages** | ESPHome YAML → C++ (Python legacy available) | ESPHome YAML → C++ |
| **Hardware** | ESP32-S3 (Waveshare RS485-CAN board) | ESP32-S3 (Waveshare RS485-CAN board) |
| **Development Environment** | Ubuntu 24.04 VM (ESPHome toolchain) | Ubuntu 24.04 VM (ESPHome toolchain) |
| **Production Environment** | ESP32 device (Deye site) | ESP32 device (EPever site) |
| **Deployment** | OTA firmware upload | OTA firmware upload |
| **Configuration** | YAML + secrets.yaml | YAML + secrets.yaml |
| **Purpose** | Monitor Pylontech + Deye inverter | Translate Pylontech CAN to EPever BMS-Link |

---

## 1. Deye Site - Python Scripts (Root Directory) - LEGACY

### Purpose
~~Monitor Pylontech batteries and Deye inverter at the **Deye installation site** using Python scripts running on a Raspberry Pi.~~

**Status: LEGACY** - These scripts are kept as a backup option. Production has migrated to ESPHome on ESP32 (see Section 2).

### File Structure
```
/home/micke/GitHub/pylontech-bms-mqtt/
├── pylon_can2mqtt.py          # CAN bus → MQTT bridge
├── pylon_rs485_monitor.py     # RS485 → MQTT bridge
├── deye_modbus2mqtt.py        # Modbus-TCP → MQTT bridge
├── requirements.txt           # Python dependencies
├── systemd/                   # systemd service files
│   ├── pylon-can2mqtt.service
│   ├── pylon-rs485-mqtt.service
│   └── pylon-mqtt.env
└── venv/                      # Python virtual environment
```

### Development Environment

**Host**: Ubuntu 24.04.3 LTS VM (`micke-VMware20-1`)
- Python 3.x with venv
- Used for testing and development
- No actual hardware connected (simulated)

**Tools**:
```bash
cd /home/micke/GitHub/pylontech-bms-mqtt
source venv/bin/activate
python3 pylon_can2mqtt.py --help
```

### Production Environment

**Host**: Raspberry Pi (Deye site - location details private)
- **OS**: Raspberry Pi OS (Debian-based)
- **Python**: 3.9+ with virtual environment
- **Services**: Runs as systemd services under `pylon` user
- **Hardware**:
  - CAN interface: USB-CAN adapter (gs_usb compatible)
  - RS485 interface: USB-RS485 adapter (FTDI FT232-based)
  - Network: Ethernet to local MQTT broker

**Deployment Method**:
```bash
# On Raspberry Pi
git pull origin main
systemctl restart pylon-can2mqtt
systemctl restart pylon-rs485-mqtt
```

### Dependencies
```
python-can
paho-mqtt>=2.0.0
pyserial
pymodbus
```

### Configuration
Environment variables (stored in `/etc/default/pylon-mqtt` on production):
```bash
MQTT_HOST=192.168.1.100
MQTT_PORT=1883
MQTT_USER=username
MQTT_PASS=password
MODBUS_HOST=192.168.200.111
```

---

## 2. Deye Site - ESPHome Firmware (esphome/) - PRODUCTION

### Purpose
Monitor Pylontech batteries and Deye inverter at the **Deye installation site** using ESP32 firmware. This is the **current production system**.

### File Structure
```
/home/micke/GitHub/pylontech-bms-mqtt/esphome/
├── deye-bms-can.yaml          # Main ESPHome configuration
├── secrets.yaml               # WiFi, MQTT credentials (gitignored)
├── custom_components/         # ESP32 CAN listen-only component
│   └── esp32_can_listen/
├── includes/                  # C++ header files
├── upstream-pr/               # Upstream ESPHome contributions
└── README.md                  # Full ESPHome documentation
```

### Development Environment

**Host**: Ubuntu 24.04.3 LTS VM (`micke-VMware20-1`)
- **ESPHome**: Same installation as EPever site (`/home/micke/GitHub/esphome/venv/`)
- **Platform**: ESP-IDF 5.5.2
- **Build System**: PlatformIO

**Tools**:
```bash
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome
/home/micke/GitHub/esphome/venv/bin/esphome compile deye-bms-can.yaml
/home/micke/GitHub/esphome/venv/bin/esphome upload deye-bms-can.yaml
/home/micke/GitHub/esphome/venv/bin/esphome logs deye-bms-can.yaml
```

### Production Environment

**Host**: Waveshare ESP32-S3-RS485-CAN board (Deye site - location details private)
- **MCU**: ESP32-S3-WROOM-1-N8R2 (8MB Flash, 2MB PSRAM)
- **Firmware**: ESPHome (version varies)
- **Hardware**:
  - CAN interface: Built-in TWAI controller (GPIO15/16)
  - RS485 interface: Built-in transceiver (GPIO17/18/21)
  - Network: WiFi to site network
- **Status**: **PRODUCTION** - Active system monitoring Pylontech batteries and Deye inverter

**Deployment Method**:
```bash
# Compile firmware on development VM
/home/micke/GitHub/esphome/venv/bin/esphome compile deye-bms-can.yaml

# Upload via OTA (WiFi)
/home/micke/GitHub/esphome/venv/bin/esphome upload deye-bms-can.yaml

# Alternative: Upload via USB (first time or if OTA fails)
/home/micke/GitHub/esphome/venv/bin/esphome run deye-bms-can.yaml
```

### Dependencies
- ESPHome toolchain (PlatformIO, ESP-IDF)
- Custom component: `esp32_can_listen` (in `custom_components/`)

### Configuration
YAML-based (secrets in `secrets.yaml`):
```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

mqtt:
  broker: !secret mqtt_host
  username: !secret mqtt_user
  password: !secret mqtt_password
```

### Relationship to Python Scripts (Legacy)

**ESPHome firmware provides identical functionality**:
- Same MQTT topics (`deye_bms/`, `deye_bms/rs485/`)
- Same Home Assistant auto-discovery
- Drop-in replacement for `pylon_can2mqtt.py` + `pylon_rs485_monitor.py`
- No changes needed in Home Assistant

**Migration completed** - The Python scripts on Raspberry Pi have been replaced by ESP32. The scripts remain in the repo as a fallback option if needed.

---

## 3. EPever Site - ESPHome Firmware (esphome-epever/)

### Purpose
Translate Pylontech CAN to EPever BMS-Link Modbus protocol at the **EPever installation site** using ESP32 firmware.

### File Structure
```
/home/micke/GitHub/pylontech-bms-mqtt/esphome-epever/
├── epever-can-bridge.yaml     # Main ESPHome configuration
├── secrets.yaml               # WiFi, MQTT, Modbus credentials (gitignored)
├── custom_components/         # ESP32 CAN listen-only component
├── HLD.md                     # High-level design doc
├── LLD.md                     # Low-level design doc
├── SESSION_*.md               # Development session logs
├── modbus_log_tail.sh         # Terminal log viewer
├── modbus_log_viewer.html     # Browser log viewer
├── modbus_log_server.py       # Proxy server for SSH tunnels
└── .esphome/                  # Build artifacts (gitignored)
```

### Development Environment

**Host**: Ubuntu 24.04.3 LTS VM (`micke-VMware20-1`)
- **ESPHome**: 2026.1.0b3 (installed in `/home/micke/GitHub/esphome/venv/`)
- **Platform**: ESP-IDF 5.5.2
- **Build System**: PlatformIO

**Tools**:
```bash
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever
/home/micke/GitHub/esphome/venv/bin/esphome compile epever-can-bridge.yaml
/home/micke/GitHub/esphome/venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml
/home/micke/GitHub/esphome/venv/bin/esphome logs epever-can-bridge.yaml
```

**Access Methods**:
- SSH to development VM
- Web browser to http://10.10.0.45/ (ESP32 web UI)
- Terminal viewer: `./modbus_log_tail.sh`

### Production Environment

**Host**: Waveshare ESP32-S3-RS485-CAN board (EPever site - location details private)
- **MCU**: ESP32-S3-WROOM-1-N8R2 (8MB Flash, 2MB PSRAM)
- **Firmware**: ESPHome 2026.1.0b3
- **IP**: 10.10.0.45 (static, site-specific network)
- **Hardware**:
  - CAN interface: Built-in TWAI controller (GPIO15/16)
  - RS485 interface: Built-in transceiver (GPIO17/18)
  - Network: WiFi to site network

**Deployment Method**:
```bash
# Compile firmware on development VM
/home/micke/GitHub/esphome/venv/bin/esphome compile epever-can-bridge.yaml

# Upload via OTA (WiFi)
/home/micke/GitHub/esphome/venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml

# Alternative: Upload via USB (first time or if OTA fails)
/home/micke/GitHub/esphome/venv/bin/esphome run epever-can-bridge.yaml
```

### Dependencies
- ESPHome toolchain (PlatformIO, ESP-IDF)
- Custom component: `esp32_can_listen` (in `custom_components/`)

### Configuration
YAML-based (secrets in `secrets.yaml`):
```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

mqtt:
  broker: !secret mqtt_host
  username: !secret mqtt_user
  password: !secret mqtt_password

substitutions:
  inverter_host: !secret inverter_host  # Modbus RTU over TCP gateway
  inverter_port: !secret inverter_port  # 9999
  inverter_slave: !secret inverter_slave  # 10
```

---

## Key Differences

### 1. **Two Sites, Two Production Systems**

| Implementation | Hardware | Location | Status | Purpose |
|---------------|----------|----------|--------|---------|
| **Deye ESPHome** | ESP32-S3 | Deye site | **PRODUCTION** | Monitor Pylontech + Deye inverter |
| **EPever ESPHome** | ESP32-S3 | EPever site | **PRODUCTION** | Translate Pylontech to EPever BMS-Link |
| **Deye Python** | Raspberry Pi | Deye site | Legacy/backup | Same as Deye ESPHome |

**Key insight**: Both sites now run ESPHome on ESP32. Python scripts are kept as a fallback option for Deye site.

### 2. **Hardware Comparison**

**Both Sites - ESP32 (ESPHome) - PRODUCTION**:
- Embedded microcontroller (ESP32-S3)
- Built-in CAN/RS485 hardware (Waveshare board)
- Low-power (~0.5W), WiFi-only
- Dedicated single-purpose device
- **Different firmware per site**: Deye monitors, EPever translates protocols

**Deye Site - Raspberry Pi (Python) - LEGACY**:
- General-purpose Linux computer
- USB peripherals for CAN/RS485
- Higher power consumption (~5-10W)
- Kept as fallback option

### 3. **Development Workflows**

**ESPHome Firmware (Both sites - PRODUCTION)**:
1. Edit YAML file (`deye-bms-can.yaml` OR `epever-can-bridge.yaml`)
2. Compile with ESPHome toolchain
3. Upload firmware OTA or USB
4. Monitor via web UI or logs
5. Commit YAML changes

**Note**: Same toolchain for both sites, but different YAML configs and different device IPs.

**Python Scripts (Deye only - LEGACY)**:
1. Edit `.py` files in Git repo (root directory)
2. Test locally with `python3 script.py`
3. Commit changes
4. Pull on Raspberry Pi
5. Restart systemd services

### 4. **Debugging Methods**

**ESPHome Firmware (Both sites - PRODUCTION)**:
```bash
# Deye site
/home/micke/GitHub/esphome/venv/bin/esphome logs deye-bms-can.yaml

# EPever site
/home/micke/GitHub/esphome/venv/bin/esphome logs epever-can-bridge.yaml
./modbus_log_tail.sh -f  # EPever-specific Modbus log viewer
```

**Python Scripts (Deye site - LEGACY)**:
```bash
# View systemd logs
sudo journalctl -u pylon-can2mqtt -f

# Run manually for debug output
./pylon_can2mqtt.py

# Check process status
systemctl status pylon-can2mqtt
```

### 5. **Configuration Management**

**Python Scripts (Deye)**:
- Environment variables in shell or `/etc/default/pylon-mqtt`
- No secrets in Git repo
- Manually replicated to production host

**ESPHome Firmware (Both sites)**:
- YAML configuration with `!secret` substitutions
- `secrets.yaml` is gitignored (different file per site)
- Secrets embedded in compiled firmware

### 6. **Network Access**

**Deye Site (Raspberry Pi or ESP32)**:
- Direct access to MQTT broker on local network
- Local CAN/RS485 interfaces
- SSH access from development VM (if Raspberry Pi)
- HTTP web UI (if ESP32)

**EPever Site (ESP32 only)**:
- WiFi to site-specific network (10.10.0.x)
- Modbus RTU over TCP gateway at 10.10.0.117:9999
- HTTP web UI at http://10.10.0.45/
- **Access requires SSH tunnel or VPN to EPever site network**

---

## Should We Split Into Two Repos?

### Pros of Splitting

✅ **Clear separation**: Each project has its own history, issues, releases
✅ **Different dependencies**: Python vs ESPHome toolchains are independent
✅ **Different deployment cycles**: Deye site vs EPever site updates are unrelated
✅ **Simpler documentation**: Each repo focuses on one use case
✅ **Reduced confusion**: No mixing up environments or deployment targets

### Pros of Keeping Together

✅ **Shared protocol knowledge**: Both use Pylontech CAN protocol
✅ **Code reuse potential**: Python script logic could inform ESPHome implementations
✅ **Common documentation**: `docs/PROTOCOL_REFERENCE.md` applies to both
✅ **Historical context**: Evolution from Python to ESPHome is documented
✅ **Easier comparison**: Side-by-side for users choosing between approaches

### Recommendation

**Keep in one repo for now**, but:

1. ✅ **This file (`ENVIRONMENTS.md`)** - Clearly documents the separation
2. ✅ **Separate READMEs**:
   - Root `README.md` for Python scripts (Deye site)
   - `esphome-epever/HLD.md` for ESPHome (EPever site)
3. ✅ **Clear directory structure**: `esphome-epever/` is self-contained
4. ✅ **Separate git tags**: `deye-v1.0` vs `epever-v1.0` for releases
5. ⏳ **Consider splitting later** if projects diverge significantly

---

## Common Mistakes to Avoid

### ❌ Don't Mix Development Environments

**Wrong**:
```bash
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever
python3 epever-can-bridge.yaml  # WRONG! This is ESPHome YAML, not Python
```

**Right**:
```bash
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever
/home/micke/GitHub/esphome/venv/bin/esphome compile epever-can-bridge.yaml
```

### ❌ Don't Confuse Production Hosts

**Wrong**:
```bash
# Trying to upload Python script to ESP32
scp pylon_can2mqtt.py 10.10.0.45:/   # ESP32 doesn't run Python!
```

**Right**:
```bash
# Python scripts go to Raspberry Pi
ssh pi@raspberry-pi-host
cd /opt/pylontech-bms-mqtt
git pull
systemctl restart pylon-can2mqtt
```

### ❌ Don't Mix Configuration Methods

**Wrong**:
```bash
# Trying to set ESPHome secrets via environment variables
export WIFI_PASSWORD=secret  # Won't work - ESPHome uses secrets.yaml
```

**Right**:
```bash
# Edit secrets.yaml for ESPHome
nano /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever/secrets.yaml
```

---

## Quick Reference

### Working on Deye Site - ESPHome (PRODUCTION)

```bash
# Location
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome

# Compile
/home/micke/GitHub/esphome/venv/bin/esphome compile deye-bms-can.yaml

# Upload OTA
/home/micke/GitHub/esphome/venv/bin/esphome upload deye-bms-can.yaml

# Upload via USB (if OTA fails)
/home/micke/GitHub/esphome/venv/bin/esphome run deye-bms-can.yaml

# View logs
/home/micke/GitHub/esphome/venv/bin/esphome logs deye-bms-can.yaml
```

### Working on EPever Site - ESPHome (PRODUCTION)

```bash
# Location
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever

# Compile
/home/micke/GitHub/esphome/venv/bin/esphome compile epever-can-bridge.yaml

# Upload OTA
/home/micke/GitHub/esphome/venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml

# View logs
/home/micke/GitHub/esphome/venv/bin/esphome logs epever-can-bridge.yaml
# OR
./modbus_log_tail.sh -f  # EPever-specific Modbus interaction log

# Web UI
firefox http://10.10.0.45/
```

### Working on Deye Site - Python Scripts (LEGACY)

```bash
# Location
cd /home/micke/GitHub/pylontech-bms-mqtt

# Activate venv
source venv/bin/activate

# Test script
./pylon_can2mqtt.py

# Deploy to production (if reverting to Pi)
ssh pi@raspberry-pi
cd /opt/pylontech-bms-mqtt
git pull
sudo systemctl restart pylon-can2mqtt
```

---

## Contact & Support

- **Deye ESPHome Issues**: Tag with `[deye]` and `[esphome]`
- **EPever ESPHome Issues**: Tag with `[epever]` and `[esphome]`
- **Deye Python Issues (legacy)**: Tag with `[deye]` and `[python]`
- **Protocol Questions**: Relevant to both sites, check `docs/PROTOCOL_REFERENCE.md`

---

**Last Updated**: 2026-01-18
**Maintained By**: micke
**Repository**: https://github.com/mikaabra/pylontech-bms-mqtt
