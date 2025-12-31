# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### Planned
- ESP32 migration using Waveshare ESP32-S3-RS485-CAN board
- Combined CAN+RS485 in single ESPHome firmware

---

## [2025-12-31] - Initial Release

### Added
- **CAN Bus Bridge** (`pylon_can2mqtt.py`)
  - Decodes Pylontech CAN protocol (0x351, 0x355, 0x359, 0x370)
  - Publishes to MQTT with Home Assistant auto-discovery
  - Rate limiting with hysteresis to reduce traffic
  - Last Will Testament for availability tracking
  - Auto-reconnect on MQTT/CAN failures

- **RS485 Monitor** (`pylon_rs485_monitor.py`)
  - Reads individual cell voltages (48 cells across 3 batteries)
  - Reads per-battery temperatures (6 sensors each)
  - Reports balancing status per cell
  - Reports alarm conditions and protection flags
  - Cycle count tracking
  - Home Assistant MQTT Discovery integration
  - Configurable polling interval

- **Debug Tools**
  - `pylon_decode.py` - CAN frame decoder with repeat suppression
  - `show_discharge_limits.sh` - Analyze discharge limits from candump logs

- **ESPHome Configuration**
  - Configuration for Waveshare ESP32-S3-RS485-CAN board
  - CAN bus integration at 500kbps
  - RS485 support (prepared for future use)

- **Documentation**
  - README.md with installation and usage instructions
  - HLD.md - High-level architecture design
  - LLD.md - Low-level protocol and implementation details
  - CHANGELOG.md - This file

### Technical Details
- Tested with 3× Shoto SDA10-48200 (16S LFP) in parallel
- Raspberry Pi 4 with Waveshare RS485 CAN HAT
- FTDI USB-RS485 adapter
- paho-mqtt v2.x compatibility
- Python 3.x required

---

## Development Diary

### 2025-12-31

**Session: RS485 MQTT Integration & GitHub Setup**

Started with working CAN and RS485 monitoring scripts. Goal was to add Home Assistant MQTT discovery to the RS485 monitor.

**RS485 MQTT Discovery Implementation:**
- Added `Publisher` class with hysteresis and rate limiting (ported from CAN script)
- Created HA discovery configs for:
  - Stack-level sensors (9 sensors)
  - Per-battery sensors (10 per battery)
  - Individual cell voltages (16 per battery)
  - Temperature sensors (6 per battery)
  - Binary sensors for balancing status
- Total: ~105 entities auto-discovered in Home Assistant
- Added `--quiet` flag for daemon mode
- Fixed message flush delay before disconnect

**Credential Cleanup:**
- Removed hardcoded MQTT credentials from all scripts
- Now uses environment variables: `MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS`
- Updated ESPHome README with placeholder values
- Created `.gitignore` to exclude secrets and build artifacts

**GitHub Repository Setup:**
- Generated SSH key for Pi
- Created repository: `mikaabra/pylontech-bms-mqtt`
- Initial commit with all scripts and documentation

**Key Decisions:**
- RS485 sensors as separate HA device from CAN sensors (allows future hardware separation)
- 5-second minimum interval for individual cell voltages (reduce HA database load)
- 2mV hysteresis on cell voltages (reduce noise)

---

*Template for future entries:*

```markdown
### YYYY-MM-DD

**Session: Brief Title**

Summary of what was worked on.

**Changes:**
- Item 1
- Item 2

**Issues Encountered:**
- Issue and resolution

**Next Steps:**
- Planned work
```
