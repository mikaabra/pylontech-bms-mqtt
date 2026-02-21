# ESPHome EPever CAN Bridge

**⚠️ This is a SEPARATE PROJECT from other code in this repository!**

This repository contains THREE separate implementations:
1. **Deye site - Python scripts** (root directory): Monitor Pylontech + Deye inverter
2. **Deye site - ESPHome firmware** (`esphome/`): Same as Python, different platform
3. **EPever site - ESPHome firmware** (`esphome-epever/` - THIS directory): Translate Pylontech to EPever BMS-Link

See [../ENVIRONMENTS.md](../ENVIRONMENTS.md) for full environment documentation.

## What This Is

ESP32-based firmware that translates Pylontech CAN bus protocol to EPever BMS-Link Modbus protocol.

**Use Case**: Pylontech-compatible batteries + EPever UP5000 inverter

## Hardware

- **Board**: Waveshare ESP32-S3-RS485-CAN
- **MCU**: ESP32-S3-WROOM-1-N8R2 (8MB Flash, 2MB PSRAM)
- **CAN**: Built-in TWAI controller (GPIO15 TX, GPIO16 RX)
- **RS485**: Built-in transceiver with flow control (GPIO17 TX, GPIO18 RX, GPIO21 DE/RE)
- **Network**: WiFi (configured in secrets.yaml)

## Quick Start

### 1. Prerequisites

```bash
# Install ESPHome 2026.1.0 in venv (if not already installed)
cd /home/micke/GitHub/pylontech-bms-mqtt
python3 -m venv venv
venv/bin/pip install esphome

# Check version
cd esphome-epever
../venv/bin/esphome version
# Should show: Version: 2026.1.0
```

### 2. Configure Secrets

```bash
cp secrets.yaml.example secrets.yaml
nano secrets.yaml
```

Required secrets:
- WiFi credentials (ssid, password)
- OTA password (for secure firmware updates)
- API encryption key (for Home Assistant/ESPHome API)
- Web UI credentials (username, password)
- Inverter Modbus gateway (host, port, slave ID)

### 3. Compile & Upload

```bash
# Compile firmware
../venv/bin/esphome compile epever-can-bridge.yaml

# Upload via OTA (WiFi)
../venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml

# Upload via USB (first time) or full compile + flash
../venv/bin/esphome run epever-can-bridge.yaml
```

### 4. Monitor Logs

```bash
# View live logs from ESPHome
../venv/bin/esphome logs epever-can-bridge.yaml

# View Modbus interaction log (terminal viewer)
./modbus_log_tail.sh -f

# Access web UI (requires authentication)
firefox http://10.10.0.45/
# Default: admin / epever_web_2026 (configured in secrets.yaml)
```

## Features

### SOC Reserve Control
- Configurable discharge blocking threshold (default 50%/55%)
- Configurable force charge activation (default 45%/50%)
- Hysteresis prevents oscillation
- Uses inverter priority mode switching (NOT D14 flag)
- Master enable/disable switch (disabled by default)

### Modbus Interaction Log
- RAM-backed circular buffer (~8KB, 50 entries)
- Captures state machine events and Modbus RTU over TCP operations
- Terminal viewer: `./modbus_log_tail.sh`
- Browser viewer: `modbus_log_viewer.html`
- Survives browser disconnects, clears on power loss

### Modbus Error Handling (Added 2026-01-19)
- **CRC validation**: Verifies Modbus CRC16 on all responses
- **Function code validation**: Rejects responses with wrong function codes
- **Automatic retry logic**: Up to 3 attempts with 200ms delays
- **Corruption detection**: Catches electrical noise from inverter's internal RS485
- Logs all errors and retries to Modbus interaction log

### Web UI
- Manual control dropdowns (D13/D14/D15)
- SOC threshold selectors (5% increments)
- Inverter priority mode control
- Debug level selector
- Statistics (success/failure counters)
- All settings persist to NVRAM

## Documentation

- **[HLD.md](HLD.md)** - High-level design, architecture, configuration examples
- **[LLD.md](LLD.md)** - Low-level technical details, protocol implementations
- **[SUCCESS_SUMMARY.md](SUCCESS_SUMMARY.md)** - Inverter priority control implementation
- **[SESSION_2026-01-17.md](SESSION_2026-01-17.md)** - Modbus log buffer development session
- **[SESSION_2026-01-18.md](SESSION_2026-01-18.md)** - Refactoring to eliminate code duplication
- **[SESSION_2026-01-19.md](SESSION_2026-01-19.md)** - Modbus CRC validation and retry logic
- **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** - Known limitations and workarounds
- **[../ENVIRONMENTS.md](../ENVIRONMENTS.md)** - Development vs production environments

## Architecture

```
Battery (CAN 500kbps) ←→ ESP32 ←→ BMS-Link ←→ EPever Inverter
   Pylontech protocol     CAN RX     RS485      Modbus RTU
                          Listen     9600       Protocol 10
                          Only       baud
```

## Key Design Decisions

### CAN Bus Support
- ESP32 TWAI (Two-Wire Automotive Interface) controller
- **ESPHome 2026.1.0**: Native LISTENONLY mode support
- Current mode: NORMAL (sends ACK signals for reliable operation)
- No custom components required

### Inverter Priority Mode Switching (NOT D14 Flag)
- **Low SOC**: Switch to Utility Priority (grid mode)
- **High SOC**: Switch to Inverter Priority (battery mode)
- **Advantage**: Battery available during power outages regardless of SOC control

### Modbus RTU over TCP Gateway
- EPever inverter accessible via Modbus RTU over TCP gateway
- Gateway at 10.10.0.117:9999 (site-specific)
- Uses function 0x10 (Write Multiple Registers) for register 0x9608
- Validation prevents corrupted data from gateway bugs

## Development Workflow

```bash
# 1. Make changes to YAML
nano epever-can-bridge.yaml

# 2. Compile to check for errors
../venv/bin/esphome compile epever-can-bridge.yaml

# 3. Upload to device
../venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml

# 4. Monitor logs
./modbus_log_tail.sh -f

# 5. Test via web UI
firefox http://10.10.0.45/

# 6. Commit changes
git add epever-can-bridge.yaml
git commit -m "Description of changes"
git push
```

## Troubleshooting

### Compile Errors

```bash
# Check ESPHome version (should be 2026.1.0)
../venv/bin/esphome version

# Clean build cache
rm -rf .esphome/build/epever-can-bridge
../venv/bin/esphome compile epever-can-bridge.yaml
```

### Upload Failures

```bash
# OTA timeout - try USB upload
../venv/bin/esphome run epever-can-bridge.yaml

# Check device is reachable
ping 10.10.0.45

# View existing logs to see if device is stuck
../venv/bin/esphome logs epever-can-bridge.yaml
```

### Modbus Errors

```bash
# View Modbus interaction log
./modbus_log_tail.sh

# Test Modbus gateway directly
./modbus_rtu_tcp.py 0x9608 -v

# Check for validation errors
grep "Invalid mode\|Wrong byte count\|CRC mismatch\|Wrong function code" in logs
```

Common Modbus error patterns:
- **`✗ Wrong function code: 0x04 (expected 0x03)`** - Electrical noise corruption, will auto-retry
- **`✗ CRC mismatch`** - Data corruption in transit, will auto-retry
- **`✗ Failed to read mode after all retries`** - Network or gateway failure, check connectivity
- **Retry attempt 2/3...** - Normal operation, corruption recovered automatically

## Common Mistakes

### ❌ Wrong: Trying to run YAML as Python
```bash
python3 epever-can-bridge.yaml  # This is YAML, not Python!
```

### ✅ Right: Use ESPHome toolchain
```bash
../venv/bin/esphome compile epever-can-bridge.yaml
```

### ❌ Wrong: Confusing with Deye site implementations
```bash
nano ../pylon_can2mqtt.py        # Deye site Python script - different site!
nano ../esphome/deye-bms-can.yaml  # Deye site ESPHome - different site!
```

### ✅ Right: Edit THIS project's YAML
```bash
nano epever-can-bridge.yaml  # EPever site ESPHome
```

### ❌ Wrong: Deploying to wrong host
```bash
# Trying to deploy to Raspberry Pi (Deye site)
scp epever-can-bridge.yaml pi@raspberry-pi:/opt/

# Trying to deploy to Deye ESP32
/home/micke/GitHub/esphome/venv/bin/esphome upload --device deye-esp32.local epever-can-bridge.yaml
```

### ✅ Right: Upload to EPever site ESP32
```bash
../venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml
```

## Performance

| Metric | Value |
|--------|-------|
| RAM Usage | 37 KB (11.4%) |
| Flash Usage | 960 KB (52.3%) |
| Power Consumption | ~0.5W |
| CAN Frame Processing | ~1ms per frame |
| Modbus Response Time | < 10ms |
| SOC Update Rate | ~3 seconds |
| Uptime | Continuous (24/7) |

## Safety & Security Features

### Security (ESPHome 2026.1.0)
1. **API Encryption**: Mandatory encryption key for Home Assistant API
2. **OTA Password**: Password-protected firmware updates
3. **Web Authentication**: Username/password required for web UI access
4. **No API Password Support**: Deprecated password authentication removed in 2026.1.0

### Safety
1. **BMS Protection Priority**: BMS flags always respected
2. **SOC Reserve Default: OFF**: Feature disabled by default
3. **Layered Control Logic**: Multiple control sources with OR/AND logic
4. **Anti-Cycling Mechanism**: 5% gap between thresholds
5. **CAN Stale Detection**: 30-second timeout on CAN data
6. **Modbus Validation**: CRC16 validation with automatic retry (up to 3 attempts)

## License

MIT License - See [../LICENSE](../LICENSE)

## Related Projects

This repository contains THREE implementations:
1. **[Deye Python Scripts](../)**: Monitor Pylontech + Deye inverter (Raspberry Pi)
2. **[Deye ESPHome](../esphome/)**: Same as Python, ESP32 platform (migration option)
3. **[EPever ESPHome](.)**: Translate Pylontech to EPever BMS-Link (THIS project)

See **[ENVIRONMENTS.md](../ENVIRONMENTS.md)** for full documentation of all three implementations.

---

## Version Information

- **ESPHome**: 2026.1.0 (released January 2026)
- **ESP-IDF Framework**: 5.5.2
- **Platform**: ESP32-S3 with 8MB Flash, 2MB PSRAM
- **Last Updated**: 2026-01-22
- **Production Device**: ESP32 at 10.10.0.45 (EPever site)
- **Development Host**: Ubuntu 24.04 VM (micke-VMware20-1)
