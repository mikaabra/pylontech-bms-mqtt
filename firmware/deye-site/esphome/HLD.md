# High-Level Design (HLD) - Deye BMS CAN Bridge

## System Overview

The Deye BMS CAN Bridge is an ESP32-based monitoring system that reads battery data from Pylontech-compatible batteries via CAN bus and RS485, then publishes the data to MQTT for Home Assistant integration.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Pylontech Battery Stack                           │
│                  (3× US3000C in parallel = 840Ah)                    │
│                                                                       │
│                  ┌──────────────────────┐                            │
│                  │   Battery BMS        │                            │
│                  │  (Pylontech Protocol)│                            │
│                  └──────────┬───────────┘                            │
│                             │                                         │
│                        CAN Bus (500kbps)                             │
│                        Pylontech V1.2                                │
├─────────────────────────────┼───────────────────────────────────────┤
│                             │                                         │
│                        RS485 (9600 baud)                             │
│                        Pylontech Protocol                            │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
                              │ CAN-H/CAN-L + RS485-A/B
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │    ESP32-S3-RS485-CAN                  │
│                             │    (Waveshare)                         │
│                    ┌────────┴─────────┐                              │
│                    │   CAN Controller │                              │
│                    │  (Listen-Only)   │                              │
│                    │  GPIO15/16       │                              │
│                    └────────┬─────────┘                              │
│                             │                                         │
│              ┌──────────────┴──────────────┐                         │
│              │     ESPHome Firmware        │                         │
│              │                             │                         │
│              │  ┌────────────────────┐    │                         │
│              │  │ CAN Protocol       │    │                         │
│              │  │ Decoder            │    │                         │
│              │  │ (0x351-0x370)      │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ RS485 Protocol     │    │                         │
│              │  │ Handler            │    │                         │
│              │  │ (Per-cell data)    │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ MQTT Publisher     │    │                         │
│              │  │ (Hysteresis)       │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ HA Discovery       │    │                         │
│              │  │ (Auto-config)      │    │                         │
│              │  └────────────────────┘    │                         │
│              └──────────────┬──────────────┘                         │
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │      WiFi        │                              │
│                    │  (MQTT Client)   │                              │
│                    └────────┬─────────┘                              │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
                              │ MQTT (TCP/IP)
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │   MQTT Broker    │                              │
│                    │   (Mosquitto)    │                              │
│                    └────────┬─────────┘                              │
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │  Home Assistant  │                              │
│                    │  (Auto-discovery)│                              │
│                    └──────────────────┘                              │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

## Architecture Components

### 1. CAN Bus Interface
- **Hardware**: ESP32-S3 built-in TWAI controller (GPIO15 TX, GPIO16 RX)
- **Mode**: Listen-only (passive monitoring, no ACK signals)
- **Bitrate**: 500 kbps
- **Protocol**: Pylontech CAN V1.2
- **Frame IDs**: 0x351, 0x355, 0x359, 0x35C, 0x370

### 2. RS485 Interface
- **Hardware**: UART with RS485 transceiver (GPIO17 TX, GPIO18 RX, GPIO21 flow control)
- **Mode**: Half-duplex master (polls batteries sequentially)
- **Bitrate**: 9600 baud, 8N1
- **Protocol**: Pylontech RS485 ASCII protocol
- **Addressing**: Battery addresses 1-3 (configurable via `num_batteries`)

### 3. CAN Protocol Decoder
Parses incoming CAN frames and extracts:
- **0x351**: Voltage/current limits (charge/discharge limits)
- **0x355**: SOC/SOH percentages
- **0x359**: Protection flags (over/under voltage, temperature, current)
- **0x35C**: Charge/discharge enable flags
- **0x370**: Cell voltage extremes and temperatures

**Features**:
- Stale detection (30-second timeout)
- Automatic availability tracking
- Hysteresis for MQTT publishing (threshold + heartbeat)

### 4. RS485 Protocol Handler

**State Machine**: Non-blocking asynchronous polling

**States**:
1. **IDLE** → Send analog command to battery N
2. **SEND_ANALOG** → Wait for transmission complete
3. **WAIT_ANALOG** → Wait for response (~250ms timeout)
4. **PARSE_ANALOG** → Parse cell voltages, temperatures, currents
5. **SEND_ALARM** → Send alarm command to battery N
6. **WAIT_ALARM** → Wait for response
7. **PARSE_ALARM** → Parse alarm status, balancing info, cycles

**Polling Sequence**:
- Cycles through batteries 1→2→3→1...
- ~5 seconds per battery complete cycle
- Separate analog and alarm polling phases

### 5. MQTT Publisher with Hysteresis

**Problem**: Raw sensor updates at high frequency would flood MQTT broker and Home Assistant

**Solution**: Dual-threshold hysteresis system

**CAN Data Hysteresis**:
| Parameter | Threshold | Heartbeat |
|-----------|-----------|-----------|
| Voltage/Current limits | 0.1V / 0.1A | 60s |
| SOC/SOH | 1% | Immediate |
| Temperatures | 0.5°C | 60s |
| Cell voltages | 0.005V | 60s |
| Flags (protection) | Any change | 60s |

**RS485 Data Hysteresis**:
| Parameter | Threshold | Heartbeat |
|-----------|-----------|-----------|
| Stack voltage/current | 0.1V / 0.1A | 60s |
| Cell voltages | 0.005V | 60s |
| Temperatures | 0.2°C | 60s |
| SOC | 1% | 60s |

**Heartbeat Mechanism**: Forces publish every 60 seconds even if no threshold crossed

### 6. Home Assistant Discovery

**Approach**: Compatible with Python script discovery

The ESP32 publishes sensor data to the same MQTT topics as the original Python scripts:
- `deye_bms/limit/v_charge_max`
- `deye_bms/limit/i_charge`
- `deye_bms/soc`
- `deye_bms/rs485/batt1/cell01`
- `deye_bms/rs485/batt1/temp1`
- etc.

**Discovery Configs**: Published once on boot via MQTT to `homeassistant/sensor/...`

### 7. Multi-Battery Support

**Configuration**: `num_batteries: 3` (configurable 1-3)

**Per-Battery Data**:
- 16 cell voltages (48 cells total for 3 batteries)
- 6 temperature sensors (18 total)
- Current, voltage, SOC, SOH
- Cycle count
- Balancing status
- Alarm/warning flags

**Memory Management**:
- Dynamic vector initialization on boot
- Resize based on `num_batteries` substitution
- ~2KB RAM per battery for cell data

## Data Flow

### CAN to MQTT Flow
1. **Battery broadcasts CAN frame** (e.g., 0x355 with SOC every ~3 seconds)
2. **ESP32 receives frame** in listen-only mode
3. **CAN decoder parses frame** and updates global variables
4. **Hysteresis check**: Compare to last published value
5. **If changed > threshold OR heartbeat due** → Publish to MQTT
6. **Home Assistant receives update** via MQTT subscription

### RS485 to MQTT Flow
1. **State machine initiates poll** (e.g., battery 1 analog data)
2. **Send ASCII command**: `~200146A1CC8F\r` (battery address 2)
3. **Wait for response** (~50-250ms)
4. **Parse ASCII response** (~200 bytes)
5. **Extract cell voltages 1-16**, temps 1-6, current, SOC, etc.
6. **Apply hysteresis** (0.005V threshold for cells, 0.2°C for temps)
7. **Publish to MQTT** if threshold crossed or heartbeat
8. **Move to next battery** or alarm polling phase

### Stale Detection Flow
1. **Track last CAN frame time** on every received frame
2. **30-second watchdog timer** checks elapsed time
3. **If > 30s since last frame** → Mark CAN as stale
4. **Publish `offline` status** to `${can_prefix}/status`
5. **Resume** → Mark as not stale, publish `online`

## Key Design Decisions

### Why Listen-Only CAN Mode?
- Battery and inverter already communicate via CAN
- ESP32 passively monitors without interfering
- No ACK signals prevent bus conflicts
- No risk of disrupting BMS-to-inverter communication

### Why Non-Blocking RS485?
- RS485 responses take 50-250ms
- Blocking would freeze all other operations
- State machine allows concurrent CAN processing
- MQTT publishing continues during RS485 waits

### Why Separate Hysteresis for CAN vs RS485?
- CAN updates every ~3 seconds (from BMS)
- RS485 polls every ~5 seconds (per battery)
- Different data volatility:
  - CAN: Slow-changing (limits, SOC)
  - RS485: Faster-changing (cell voltages during balancing)
- Prevents MQTT flooding while maintaining responsiveness

### Why Vector-Based Cell Storage?
- Dynamic sizing based on `num_batteries` configuration
- Easy iteration for hysteresis checking
- Type-safe with `std::vector<float>`
- Initialize with invalid values (-999, -1) to detect missing data

## Safety Features

### 1. CAN Stale Detection
- 30-second timeout on CAN data
- Publishes `offline` status if no frames received
- Prevents serving stale data to Home Assistant

### 2. RS485 Timeout Handling
- 250ms timeout on responses
- Retry logic for failed polls
- Tracks consecutive failures per battery

### 3. Sanity Checking
- Cell voltages: Valid range 2.0-4.5V
- Temperatures: Valid range -40°C to 80°C
- Current: Valid range -500A to 500A
- SOC/SOH: Valid range 0-100%

### 4. Runtime Debug Toggle
- Web UI switch to change log level (WARN → INFO → DEBUG)
- Reduces log noise in normal operation
- Enables detailed debugging when needed

## Performance Characteristics

| Metric | Value |
|--------|-------|
| CAN Frame Processing | ~0.5ms per frame |
| RS485 Response Time | 50-250ms (battery-dependent) |
| MQTT Publish Rate | Throttled by hysteresis (~10-20 msg/min) |
| Web UI Responsiveness | < 100ms |
| OTA Update Time | ~15 seconds |
| Firmware Size | ~1.2 MB |
| RAM Usage | ~80 KB (including vectors) |
| Power Consumption | ~0.5W |
| Uptime | Continuous (24/7) |

## Failure Modes and Recovery

| Failure | Detection | Recovery |
|---------|-----------|----------|
| CAN bus disconnect | 30s timeout | Publishes offline status, continues RS485 |
| RS485 disconnect | Timeout on poll | Logs error, marks battery stale |
| WiFi disconnect | ESPHome auto-detect | Auto-reconnect with exponential backoff |
| MQTT disconnect | Will message | Auto-reconnect, republishes discovery |
| Firmware crash | Watchdog timer | Auto-reboot, resumes operation |
| Power loss | N/A | Restores operation on boot |

## Configuration Examples

### Example 1: 3-Battery Stack (Default)
```yaml
substitutions:
  num_batteries: "3"
  pylontech_addr: "2"
```
**Result**: Monitors 3 batteries × 16 cells = 48 cell voltages

### Example 2: Single Battery
```yaml
substitutions:
  num_batteries: "1"
  pylontech_addr: "2"
```
**Result**: Monitors single battery, reduces memory usage

### Example 3: Different MQTT Prefix
```yaml
substitutions:
  can_prefix: "battery_can"
  rs485_prefix: "battery_rs485"
```
**Result**: Changes MQTT topic structure

## Sensor Summary

**CAN Sensors** (14 total):
- Voltage limits (charge max, discharge low)
- Current limits (charge, discharge)
- SOC, SOH
- Cell voltage extremes (min, max, delta)
- Temperature extremes (min, max)
- Protection flags summary

**RS485 Sensors** (per battery × num_batteries):
- 16 cell voltages
- 6 temperatures
- Stack voltage, current, SOC
- Cycle count
- Balancing count and cells
- Alarm/warning status
- MOSFET states (charge, discharge, limiting)

**Total for 3 batteries**: ~120+ sensors

## Related Documentation

- **LLD.md**: Low-level technical implementation details
- **README.md**: Quick start and troubleshooting
- **PROTOCOL_REFERENCE.md**: CAN and RS485 protocol specifications
- **docs/guides/TROUBLESHOOTING.md**: Common issues and solutions

## Future Enhancements

Potential features for future development:

1. **Modbus-TCP integration**: Add Deye inverter monitoring (like Python script)
2. **CAN transmit mode**: Support for writing configuration to BMS
3. **SD card logging**: Historical data storage
4. **Display support**: OLED/LCD for local status display
5. **Time-based controls**: Different behavior based on time of day
6. **Weather integration**: Adjust behavior based on solar forecast
7. **Multiple RS485 buses**: Support for more than 3 batteries

## Acknowledgments

- Pylontech CAN protocol from [Setfire Labs documentation](https://www.setfirelabs.com/tag/can)
- Pylontech RS485 protocol reverse-engineered from captures
- ESPHome framework for embedded development
- Waveshare ESP32-S3-RS485-CAN hardware design
