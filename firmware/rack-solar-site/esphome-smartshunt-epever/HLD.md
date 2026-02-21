# High-Level Design (HLD) - Rack Solar Bridge

## System Overview

The Rack Solar Bridge is an ESP32-based monitoring system that integrates data from two distinct solar system components:
1. **Victron SmartShunt** (via VE.Direct UART) - Battery monitoring
2. **EPEVER MPPT Solar Charge Controller** (via RS485/Modbus) - Solar production

The system validates data quality, detects corruption, and publishes to MQTT for Home Assistant integration.

```
┌─────────────────────────────────────────────────────────────────────┐
│                         VICTRON SMARTSHUNT                           │
│                    (Battery Monitor / Shunt)                         │
│                                                                       │
│                  ┌──────────────────────┐                            │
│                  │  VE.Direct Protocol  │                            │
│                  │  (Text-based UART)   │                            │
│                  └──────────┬───────────┘                            │
│                             │                                         │
│                        UART (19200 baud)                             │
│                        GPIO5 (RX with pull-up)                       │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │    ESP32-S3-RS485-CAN                  │
│                             │    (Waveshare)                         │
│                             │                                        │
│                    ┌────────┴─────────┐                              │
│                    │   UART Input     │                              │
│                    │  (VE.Direct)     │                              │
│                    │  GPIO5           │                              │
│                    └────────┬─────────┘                              │
│                             │                                         │
│              ┌──────────────┴──────────────┐                         │
│              │     ESPHome Firmware        │                         │
│              │                             │                         │
│              │  ┌────────────────────┐    │                         │
│              │  │ SmartShunt Parser  │    │                         │
│              │  │ (External Component)    │                         │
│              │  │ - Data validation       │                         │
│              │  │ - Bitflip detection     │                         │
│              │  │ - Threshold check       │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ EPEVER Modbus      │    │                         │
│              │  │ (RS485 Master)     │    │                         │
│              │  │ - Poll registers   │    │                         │
│              │  │ - Parse responses  │    │                         │
│              │  │ - Error counting   │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ Data Quality       │    │                         │
│              │  │ Monitor            │    │                         │
│              │  │ - Bitflip rate     │    │                         │
│              │  │ - Validation score │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ MQTT Publisher     │    │                         │
│              │  │ (Hysteresis)       │    │                         │
│              │  │ - HA Discovery     │    │                         │
│              │  │ - Status tracking  │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              └────────┼───────────────────┘                         │
│                       │                                               │
│            ┌─────────┴──────────┐                                    │
│            │  RS485 Output      │                                    │
│            │  (GPIO17/18/21)    │                                    │
│            └─────────┬──────────┘                                    │
└──────────────────────┼──────────────────────────────────────────────┘
                       │
                       │ RS485 (9600 baud)
                       │
┌──────────────────────┼──────────────────────────────────────────────┐
│                      │                                               │
│            ┌─────────┴──────────┐                                    │
│            │  EPEVER MPPT       │                                    │
│            │  Solar Controller  │                                    │
│            │  (Modbus RTU)      │                                    │
│            └────────────────────┘                                    │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

## Architecture Components

### 1. VE.Direct Interface (SmartShunt)
- **Hardware**: UART (GPIO5 RX with INPUT_PULLUP)
- **Baud Rate**: 19200 bps
- **Protocol**: Victron VE.Direct (text-based, space-delimited)
- **Component**: External ESPHome component (KinDR007/VictronMPPT-ESPHOME)

### 2. SmartShunt Data Validation

**Problem**: VE.Direct can experience data corruption due to:
- Electrical noise on long cables
- Ground loops
- Serial bitflips

**Solution**: Multi-layer validation system

#### Validation Layers:
1. **Range Validation**: Check values within expected bounds
   - Voltage: 20-70V
   - Current: -200A to +200A
   - SOC: 0-100%
   - Temperature: -20°C to 80°C

2. **Pattern Validation**: Check text fields for valid format
   - Model description: Alphanumeric only
   - Firmware version: X.XX format
   - Serial number: Specific patterns

3. **Checksum Validation**: VE.Direct includes implicit checksum via field structure

4. **Bitflip Detection**: Track corruption events over time
   - 60-second rolling window
   - Rate calculation: events per minute
   - Alert when rate exceeds threshold

### 3. RS485/Modbus Interface (EPEVER)
- **Hardware**: UART with RS485 transceiver (GPIO17 TX, GPIO18 RX, GPIO21 DE/RE)
- **Baud Rate**: 115200 bps
- **Protocol**: Modbus RTU
- **Role**: Master (polls EPEVER every 5 seconds)

#### EPEVER Register Map:
| Register | Description | Units |
|----------|-------------|-------|
| 0x3100 | PV array voltage | 0.01V |
| 0x3101 | PV array current | 0.01A |
| 0x3102 | PV array power | 0.01W |
| 0x3104 | Battery voltage | 0.01V |
| 0x3105 | Battery current | 0.01A |
| 0x3106 | Battery SOC | % |
| 0x3110 | Device temperature | °C |

### 4. Data Quality Monitoring

**Bitflip Rate Calculation**:
```
bitflip_rate = (bitflip_count in last 60s) / (time_window in minutes)
```

**Data Quality Score**:
```
quality_score = 100% - (validation_failures / total_validations × 100%)
```

**Thresholds**:
- **Healthy**: Bitflip rate < 1 event/min, Quality > 95%
- **Warning**: Bitflip rate 1-5 events/min, Quality 80-95%
- **Critical**: Bitflip rate > 5 events/min, Quality < 80%

### 5. MQTT Publishing with Hysteresis

**SmartShunt Sensors** (19 total):
- Battery voltage, current, SOC
- Instantaneous power
- Temperature
- Consumed Ah, time to go
- Historical: deepest discharge, cycles, cumulative Ah
- Min/max voltage tracking
- Energy: discharged, charged

**EPEVER Sensors** (8 total):
- Solar voltage, current, power
- Battery capacity
- Device temperature
- Battery voltage, current
- Total energy (kWh)

**Publishing Strategy**:
- **Threshold-based**: Only publish if value changed beyond threshold
- **Heartbeat**: Force publish every 60 seconds
- **Separate topics**: `rack-solar/smartshunt/*` and `rack-solar/epever/*`

### 6. Home Assistant Discovery

**Approach**: Custom lambda-based discovery

**Process**:
1. On MQTT connect, publish discovery configs for all sensors
2. Discovery topic: `homeassistant/sensor/rack_solar/{sensor_id}/config`
3. Device grouping: Single device with multiple entities
4. Availability topics: Separate for SmartShunt and EPEVER

**Example Discovery Config**:
```json
{
  "name": "SmartShunt Battery Voltage",
  "state_topic": "rack-solar/smartshunt/battery_voltage",
  "unique_id": "rack_solar_ss_battery_voltage",
  "unit_of_measurement": "V",
  "device_class": "voltage",
  "device": {
    "identifiers": ["rack_solar_bridge"],
    "name": "Rack Solar Bridge"
  }
}
```

## Data Flow

### SmartShunt Data Flow
1. **SmartShunt broadcasts** VE.Direct frame (~1 second interval)
2. **VE.Direct component parses** text into sensor values
3. **Validation lambda checks** each value:
   - Range check
   - Pattern check (for text fields)
   - Track validation results
4. **Bitflip detection**: Compare to previous valid values
5. **Threshold check**: Compare to last published value
6. **Publish to MQTT** if changed beyond threshold or heartbeat due
7. **Update quality metrics** (bitflip rate, quality score)

### EPEVER Data Flow
1. **Interval timer triggers** (every 5 seconds)
2. **Build Modbus request** for registers 0x3100-0x311B
3. **Send via RS485** (GPIO17/18)
4. **Wait for response** (~50-100ms)
5. **Parse Modbus response** (16-bit registers)
6. **Apply scaling factors** (0.01V, 0.01A, etc.)
7. **Threshold check**: Compare to last published value
8. **Publish to MQTT** if changed or heartbeat

### Stale Detection Flow
1. **Track last update time** for each subsystem
2. **30-second watchdog** for SmartShunt
3. **30-second watchdog** for EPEVER
4. **Mark stale** if no data received
5. **Publish offline** to availability topic
6. **Resume** → Mark online, publish availability

## Key Design Decisions

### Why External Victron Component?
- VE.Direct protocol is complex (text-based, multiple frame types)
- KinDR007 component is well-maintained and widely used
- Handles frame parsing, checksum validation, and error recovery
- Throttle support prevents overwhelming ESP32

### Why Data Validation?
- Real-world testing showed bitflips on long VE.Direct cables
- Corrupted data would cause false alarms in Home Assistant
- Validation provides early warning of wiring issues
- Quality metrics help diagnose problems

### Why Separate SmartShunt and EPEVER Topics?
- Different subsystems with different update rates
- Independent availability tracking
- Easier debugging (isolate issues by subsystem)
- Cleaner Home Assistant entity organization

### Why Custom HA Discovery?
- Native ESPHome discovery uses `topic_prefix` which conflicts with custom topics
- Custom lambda allows full control over entity naming and organization
- Can group all sensors under single device
- Compatible with existing Python-based discovery

### Why Threshold-Based Publishing?
- SmartShunt updates every second (too frequent for MQTT)
- EPEVER polls every 5 seconds
- Without throttling, would flood MQTT broker
- Thresholds balance responsiveness with network efficiency

## Safety Features

### 1. Data Validation
- Range checks prevent out-of-bounds values
- Pattern checks catch corrupted text fields
- Bitflip detection tracks data integrity

### 2. Stale Detection
- 30-second timeout on both subsystems
- Publishes offline status if no data
- Prevents Home Assistant from showing stale values

### 3. Error Counting
- CRC errors on RS485 tracked
- Timeout errors tracked
- Frame errors tracked
- Helps diagnose communication issues

### 4. Runtime Log Level Toggle
- Web UI switch: WARN → INFO → DEBUG
- Reduces noise in normal operation
- Enables detailed debugging when needed

## Performance Characteristics

| Metric | Value |
|--------|-------|
| VE.Direct Processing | ~5ms per frame |
| EPEVER Poll Cycle | ~100ms (including wait) |
| MQTT Publish Rate | ~5-10 msg/min per subsystem |
| SmartShunt Update Rate | 1 second (throttled by hysteresis) |
| EPEVER Poll Rate | 5 seconds |
| Web UI Responsiveness | < 100ms |
| OTA Update Time | ~15 seconds |
| Firmware Size | ~1.1 MB |
| RAM Usage | ~50 KB |
| Power Consumption | ~0.5W |
| Uptime | Continuous (24/7) |

## Failure Modes and Recovery

| Failure | Detection | Recovery |
|---------|-----------|----------|
| SmartShunt disconnect | 30s timeout | Publishes offline, continues EPEVER |
| EPEVER disconnect | 30s timeout | Publishes offline, continues SmartShunt |
| VE.Direct corruption | Validation failure | Logs warning, increments bitflip count |
| RS485 CRC error | CRC mismatch | Logs error, increments CRC counter |
| WiFi disconnect | ESPHome auto-detect | Auto-reconnect with backoff |
| MQTT disconnect | Will message | Auto-reconnect, republishes discovery |
| High bitflip rate | >5 events/min | Publishes alert to HA |

## Configuration Examples

### Example 1: Default Configuration
```yaml
substitutions:
  mqtt_prefix: rack-solar
```
**Result**: Standard operation with default thresholds

### Example 2: Tighter Thresholds
```yaml
# Modify threshold constants in includes/solar_helpers.h
THRESHOLD_VOLTAGE = 0.05;  // 50mV instead of 100mV
THRESHOLD_CURRENT = 0.05;  // 50mA instead of 100mA
```
**Result**: More responsive but more MQTT traffic

### Example 3: Different MQTT Prefix
```yaml
substitutions:
  mqtt_prefix: solar_rack_1
```
**Result**: Topics become `solar_rack_1/smartshunt/*`

## Sensor Summary

### SmartShunt Sensors (19)
**Real-time**:
- Battery voltage, current, SOC
- Instantaneous power
- Battery temperature
- Time to go

**Historical**:
- Deepest discharge (Ah)
- Last discharge (Ah)
- Average discharge (Ah)
- Charge cycles
- Full discharges
- Cumulative Ah
- Min/max voltage
- Last full charge time
- Discharged energy (Wh)
- Charged energy (Wh)

**Device Info**:
- Model description
- Firmware version
- Device type
- Serial number
- Monitor mode
- Alarm condition/reason

### EPEVER Sensors (8)
- PV array voltage, current, power
- Battery capacity (%)
- Device temperature
- Battery voltage, current
- Total energy (kWh)

### Diagnostic Sensors (11)
- Bitflip rate (events/min)
- Data quality score (%)
- Bitflip window total
- RS485 CRC errors
- RS485 timeout errors
- RS485 frame errors
- SmartShunt stale status
- EPEVER stale status
- Free heap (bytes)
- WiFi signal (dBm)
- Uptime (seconds)

**Total**: ~40 sensors

## Related Documentation

- **LLD.md**: Low-level technical implementation details
- **README.md**: Quick start and troubleshooting
- **ESPHOME_ESP32_PRODUCTION_BEST_PRACTICES.md**: ESPHome best practices
- **docs/guides/TROUBLESHOOTING.md**: Common issues and solutions

## Future Enhancements

Potential features for future development:

1. **Weather integration**: Adjust behavior based on forecast
2. **Load control**: Control external relays based on SOC
3. **Generator start**: Trigger generator at low SOC
4. **Historical logging**: SD card for long-term data
5. **Display support**: OLED for local monitoring
6. **Multiple SmartShunts**: Support for multiple battery banks
7. **Modbus TCP**: Direct TCP connection to EPEVER
8. **Alert thresholds**: Configurable alert levels in web UI

## Acknowledgments

- Victron VE.Direct protocol documentation
- KinDR007 VictronMPPT-ESPHOME external component
- EPEVER Modbus protocol documentation
- ESPHome framework
- Waveshare ESP32-S3-RS485-CAN hardware
