# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### Planned
- Hardware testing with Waveshare ESP32-S3-RS485-CAN board
- Validate ESPHome firmware as drop-in replacement for Python scripts

---

## [2026-01-01] - RS485 Alarm Decoder Fix

### Fixed
- **False alarm reports at 100% SOC** - The alarm decoder was incorrectly reporting
  `module_overvolt` and `module_undervolt` when batteries were fully charged
- Cell overvoltage at 100% SOC is now correctly treated as a warning (informational),
  not an alarm (problem)

### Changed
- Rewrote `decode_alarm_response()` with correct byte/bit positions from BMS XML
- Separated `warnings` (informational) from `protections` (actual problems)
- Only undervolt protections and pack-level issues are now reported as alarms
- Added documentation of the status byte layout from BMS protocol

---

## [2025-12-31] - Security & Usability Improvements

### Added
- **LICENSE file** - MIT license for open source distribution
- **Quick Start section** in README - 5-step guide to get running quickly

### Changed
- **systemd services** now run as dedicated `pylon` user instead of root
- **Security hardening** added to systemd services:
  - `NoNewPrivileges=true`
  - `ProtectSystem=strict`
  - `ProtectHome=true`
  - `PrivateTmp=true`
- Installation path changed to `/opt/pylontech-bms-mqtt` for production deployments
- README updated with user creation and installation instructions

### Security
- Services no longer run as root, reducing attack surface
- Dedicated service user `pylon` with minimal permissions (dialout, can groups only)

---

## [2025-12-31] - ESPHome Full Implementation

### Changed
- **ESPHome Configuration** - Complete rewrite to match Python scripts exactly
  - Full CAN decoding (0x351, 0x355, 0x359, 0x370 frames)
  - Full RS485 decoding (analog data + alarms)
  - RS485 temperature reading (6 sensors per battery)
  - RS485 per-battery current measurement
  - Stack totals calculation (voltage, current, cell min/max)
  - Alarm and protection flag decoding
  - Balancing status monitoring
  - MQTT topics identical to Python scripts (`deye_bms/` and `deye_bms/rs485/`)
  - Ready as drop-in replacement when ESP32 hardware arrives

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

**Documentation & Discoverability Improvements:**
- Added `requirements.txt` for easy pip install
- Added ready-to-use systemd service files in `systemd/`
- Expanded README with:
  - Hardware compatibility list (tested batteries, interfaces)
  - Detailed CAN and RS485 setup instructions
  - Comprehensive troubleshooting guide
  - Contributing section
- Added `docs/HLD.md` - High-level design with architecture diagrams
- Added `docs/LLD.md` - Low-level protocol specifications
- Added GitHub topics for discoverability

**Session: ESPHome Full Implementation**

Reviewed ESPHome config and identified gaps compared to Python scripts. Complete rewrite to achieve feature parity.

**ESPHome Changes:**
- Rewrote `esphome/deye-bms-can.yaml` from scratch
- Added full CAN frame decoding matching `pylon_can2mqtt.py`:
  - 0x351: Voltage/current limits
  - 0x355: SOC/SOH
  - 0x359: Status flags
  - 0x370: Cell extremes and temperatures
- Added full RS485 protocol implementation matching `pylon_rs485_monitor.py`:
  - CID2=0x42 analog data (cells, temps, current, SOC, cycles)
  - CID2=0x44 alarm data (protection flags, balancing status)
- Added stack-level calculations:
  - Stack voltage (average of batteries)
  - Stack current (sum of batteries)
  - Stack cell min/max across all batteries
  - Total balancing cell count
- MQTT topic structure now identical to Python scripts
- Uses esp-idf framework for ESP32-S3 compatibility

**Key Implementation Details:**
- RS485 UART on GPIO43 (TX) / GPIO44 (RX)
- CAN on GPIO15 (TX) / GPIO16 (RX) at 500kbps
- 30-second RS485 polling interval
- Configurable via substitutions (num_batteries, pylontech_addr)

**Next Steps:**
- Test with actual ESP32-S3-RS485-CAN hardware when it arrives

**Session: RS485 Alarm Decoder Bug Fix**

User reported false alarms ("overvolt" and "undervolt") at 100% SOC. Investigation revealed:

**Root Cause:**
- Status byte bit interpretation was completely wrong
- Code was treating byte 3 bits as `module_overvolt` (bit 0) and `module_undervolt` (bit 1)
- Actually, bit 0 = Cell OV Alarm, bit 1 = Cell OV Protect (both normal at 100%)
- When status_byte = 0x03, both flags were set, causing false reports

**Investigation Process:**
1. Read BMS XML file (`BMS073-H17-BL08-16S-en-US.xml`) for protocol documentation
2. Found status byte layout: ByteIndex 1 has voltage flags, ByteIndex 2 has temp flags
3. Captured raw RS485 responses to verify byte positions
4. Correlated with display values (OV=Y, OVP=Y = bits 0+1 for Cell OV Alarm+Protect)

**Fix:**
- Rewrote decoder with correct bit meanings from XML
- Separated `warnings` (informational) from `protections` (problems)
- Cell OV at 100% SOC is now a warning, not an alarm
- Only undervolt and pack-level protections are real alarms

**Lesson Learned:**
- Protocol reverse-engineering needs verification against actual device behavior
- BMS XML files contain valuable protocol documentation

---

**Session: Security & Usability Audit**

Reviewed project for reliability, security, and newcomer experience. Identified several improvements.

**Changes Made:**
- Added MIT LICENSE file (was mentioned in README but missing)
- Added Quick Start section - 5 commands to get running
- Rewrote systemd services for security:
  - Run as dedicated `pylon` user (not root)
  - Added systemd hardening options
  - Changed working directory to /opt for production installs
- Updated README with user creation and permission setup

**Security Considerations Reviewed:**
- Credentials already in environment variables (good)
- MQTT TLS not implemented (low priority - typically internal network)
- Input validation present on CAN values (sanity ranges)
- RS485 protocol decoding is defensive

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
