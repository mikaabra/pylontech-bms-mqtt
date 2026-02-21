# Low-Level Design (LLD) - Deye BMS CAN Bridge

## Technical Specifications

### Hardware Platform
- **Board**: Waveshare ESP32-S3-RS485-CAN
- **MCU**: ESP32-S3-WROOM-1-N8R2 (8MB Flash, 2MB PSRAM)
- **CAN Controller**: Built-in TWAI (Two-Wire Automotive Interface)
- **RS485**: Isolated transceiver with hardware flow control
- **Power**: USB-C or 5V terminal block

### Firmware Environment
- **Framework**: ESPHome 2026.1.0+
- **Platform**: ESP-IDF 5.x
- **Language**: C++ (Arduino/ESP-IDF)
- **Build System**: PlatformIO
- **OTA**: Supported via WiFi

## Protocol Implementations

### CAN Bus Protocol (Pylontech V1.2)

#### Frame ID 0x351 - Voltage/Current Limits
**Length**: 8 bytes, little-endian 16-bit values

| Byte | Field | Formula | Units |
|------|-------|---------|-------|
| 0-1 | Charge voltage limit | value / 10 | V |
| 2-3 | Charge current limit | value / 10 | A |
| 4-5 | Discharge current limit | value / 10 | A |
| 6-7 | Discharge voltage limit | value / 10 | V |

**Implementation**:
```cpp
// In canbus: on_frame: can_id: 0x351
float v_charge_max = (can_le_u16(x[0], x[1]) / 10.0f);
float i_charge_lim = (can_le_u16(x[2], x[3]) / 10.0f);
float i_dis_lim = (can_le_u16(x[4], x[5]) / 10.0f);
float v_low_lim = (can_le_u16(x[6], x[7]) / 10.0f);

// Sanity check: valid ranges
if (v_charge_max >= 30.0f && v_charge_max <= 65.0f &&
    v_low_lim >= 30.0f && v_low_lim <= 65.0f &&
    i_charge_lim >= 0.0f && i_charge_lim <= 500.0f &&
    i_dis_lim >= 0.0f && i_dis_lim <= 500.0f) {
    // Update sensors
    id(sensor_v_charge_max).publish_state(v_charge_max);
    // ... MQTT publish with hysteresis
}
```

#### Frame ID 0x355 - SOC/SOH
**Length**: 4 bytes, 16-bit little-endian values

| Byte | Field | Units |
|------|-------|-------|
| 0-1 | State of Charge (SOC) | % |
| 2-3 | State of Health (SOH) | % |

#### Frame ID 0x359 - Protection Flags
**Length**: 8 bytes

| Byte | Bits | Meaning |
|------|------|---------|
| 0 | 0-7 | Protection flags (overvolt, undervolt, overtemp, etc.) |
| 1 | 0-7 | Protection flags (system error, charge overcurrent) |
| 2 | 0-7 | Warning flags (high voltage, low voltage, etc.) |
| 3 | 0-7 | Warning flags (comms fail, high charge current) |
| 4 | 0-7 | Module count |
| 7 | 0-7 | Additional status byte |

#### Frame ID 0x35C - Charge Request Flags
**Length**: 1-8 bytes

| Bit | Mask | Field |
|-----|------|-------|
| 7 | 0x80 | Charge Enable (RCE) |
| 6 | 0x40 | Discharge Enable (RDE) |
| 5 | 0x20 | Force Charge Level 1 (CI1) |

#### Frame ID 0x370 - Cell Voltage/Temperature Extremes
**Length**: 8 bytes

| Byte | Field | Formula | Units |
|------|-------|---------|-------|
| 0-1 | Temperature 1 | value / 10 | °C |
| 2-3 | Temperature 2 | value / 10 | °C |
| 4-5 | Max cell voltage | value / 1000 | V |
| 6-7 | Min cell voltage | value / 1000 | V |

**Helper Function** (in includes/set_include.h):
```cpp
uint16_t can_le_u16(uint8_t low, uint8_t high) {
    return (uint16_t)low | ((uint16_t)high << 8);
}
```

### RS485 Protocol (Pylontech ASCII)

#### Command Format
```
~{ADDR}146{A1}{A2}{LEN}{DATA}{CHK}{CR}
```

**Fields**:
- `~`: Start delimiter
- `{ADDR}`: Battery address (2 digits, e.g., "01", "02", "03")
- `146`: Command (analog info)
- `{A1}{A2}`: Sub-command
- `{LEN}`: Data length
- `{DATA}`: Command data
- `{CHK}`: Checksum (2 hex digits)
- `{CR}`: Carriage return (\r, 0x0D)

#### Analog Command Example
```
~200146A1CC8F\r
```
**Breakdown**:
- `~20`: Battery address 2, group 0
- `0146`: Command 146 (analog info)
- `A1`: Sub-command A1
- `CC`: Checksum
- `8F`: Checksum
- `\r`: End

#### Response Format
```
~{ADDR}14600{LEN}{DATA}{CHK}{CR}
```

**Response Data Structure** (A1 - Cell Voltages 1-16):
| Position | Field | Bytes | Format |
|----------|-------|-------|--------|
| 0-1 | Cells 1-16 | 32 | 4-char hex per cell (mV) |
| 32-33 | Temperature 1-6 | 12 | 4-char hex per temp (0.1°C) |
| 44-47 | Current | 4 | 4-char hex (0.1A, signed) |
| 48-51 | Voltage | 4 | 4-char hex (0.01V) |
| 52-55 | Remaining Ah | 4 | 4-char hex (0.1Ah) |
| ... | ... | ... | ... |

**Parsing Example** (cell voltage):
```cpp
// Cell 1 voltage at offset 0
uint16_t cell1_raw = hex_to_uint16(response.substr(0, 4));
float cell1_v = cell1_raw / 1000.0f;  // Convert mV to V
```

## RS485 State Machine Implementation

### Global State Variables
```yaml
globals:
  - id: rs485_state
    type: int
    initial_value: '0'  # 0=IDLE, 1=SEND_ANALOG, 2=WAIT_ANALOG, 3=PARSE_ANALOG, etc.
  
  - id: rs485_current_batt
    type: int
    initial_value: '0'  # Current battery being polled (1-3)
  
  - id: rs485_tx_time
    type: uint32_t
    initial_value: '0'  # Timestamp of last transmission
  
  - id: rs485_response_buf
    type: std::string
    restore_value: no
```

### State Machine Logic (interval: 10ms)
```cpp
switch (id(rs485_state)) {
  case 0: // IDLE
    id(rs485_state) = 1;
    id(rs485_tx_time) = millis();
    break;
    
  case 1: // SEND_ANALOG
    if (millis() - id(rs485_tx_time) >= 10) {
      send_rs485_command(id(rs485_current_batt), "A1");
      id(rs485_state) = 2;
      id(rs485_tx_time) = millis();
    }
    break;
    
  case 2: // WAIT_ANALOG
    if (millis() - id(rs485_tx_time) >= 250) {
      // Timeout - mark as failure
      id(rs485_state) = 0;
      id(rs485_current_batt) = (id(rs485_current_batt) % num_batt) + 1;
    } else if (uart_available()) {
      // Response received
      id(rs485_state) = 3;
    }
    break;
    
  case 3: // PARSE_ANALOG
    parse_analog_response();
    id(rs485_state) = 4;  // Move to alarm phase
    break;
    
  // ... similar for alarm phases
}
```

## Hysteresis Implementation

### CAN Hysteresis
```cpp
// Last published values (global variables)
float last_can_v_charge_max = -1.0f;
uint32_t last_can_limits_publish = 0;

// In CAN frame handler
uint32_t now = millis();
bool heartbeat = (now - last_can_limits_publish >= 60000);
bool changed = (fabs(v_charge_max - last_can_v_charge_max) >= 0.1f);

if (changed || heartbeat) {
    // Publish to MQTT
    mqtt_client.publish("deye_bms/limit/v_charge_max", 
                       std::to_string(v_charge_max));
    last_can_v_charge_max = v_charge_max;
    last_can_limits_publish = now;
}
```

### RS485 Cell Voltage Hysteresis
```cpp
// Vector-based tracking
std::vector<float> last_cell_voltages;  // Size: 16 * num_batteries

// On parse
for (int cell = 0; cell < 16; cell++) {
    int idx = (batt_num - 1) * 16 + cell;
    float voltage = parsed_voltages[cell];
    
    // Check threshold (5mV)
    if (fabs(voltage - last_cell_voltages[idx]) >= 0.005f) {
        publish_cell_voltage(batt_num, cell, voltage);
        last_cell_voltages[idx] = voltage;
    }
}
```

## Vector Initialization on Boot
```cpp
// In esphome: on_boot: lambda:
const int num_batt = 3;
const int cells_per_batt = 16;
const int temps_per_batt = 6;

// Cell voltages: 16 cells × 3 batteries = 48 entries
id(cell_voltages).resize(cells_per_batt * num_batt, 0.0f);
id(last_cell_voltages).resize(cells_per_batt * num_batt, -1.0f);

// Temperatures: 6 temps × 3 batteries = 18 entries
id(cell_temps).resize(temps_per_batt * num_batt, -999.0f);
id(last_batt_temps).resize(temps_per_batt * num_batt, -999.0f);

// Battery-level data
id(batt_current).resize(num_batt, 0.0f);
id(last_batt_currents).resize(num_batt, -1.0f);

// ... similar for all other vectors
```

## MQTT Topic Structure

### CAN Topics
```
deye_bms/limit/v_charge_max
deye_bms/limit/v_low
deye_bms/limit/i_charge
deye_bms/limit/i_discharge
deye_bms/soc
deye_bms/soh
deye_bms/ext/temp_min
deye_bms/ext/temp_max
deye_bms/ext/cell_v_min
deye_bms/ext/cell_v_max
deye_bms/ext/cell_v_delta
deye_bms/flags
deye_bms/module_count
deye_bms/status_byte7
deye_bms/alarm_summary
deye_bms/can_frame_count
deye_bms/can_error_count
```

### RS485 Topics (per battery)
```
deye_bms/rs485/batt{N}/voltage
deye_bms/rs485/batt{N}/current
deye_bms/rs485/batt{N}/soc
deye_bms/rs485/batt{N}/remain_ah
deye_bms/rs485/batt{N}/total_ah
deye_bms/rs485/batt{N}/cycles
deye_bms/rs485/batt{N}/cell{01-16}
deye_bms/rs485/batt{N}/temp{1-6}
deye_bms/rs485/batt{N}/balancing_count
deye_bms/rs485/batt{N}/balancing_cells
deye_bms/rs485/batt{N}/alarms
deye_bms/rs485/batt{N}/warnings
deye_bms/rs485/batt{N}/state
```

## Stale Detection

### CAN Stale Check (interval: 1s)
```cpp
if (millis() - last_can_rx > 30000) {
    if (!can_stale) {
        can_stale = true;
        mqtt_client.publish("deye_bms/status", "offline");
    }
} else {
    if (can_stale) {
        can_stale = false;
        mqtt_client.publish("deye_bms/status", "online");
    }
}
```

### RS485 Stale Check (per battery)
```cpp
for (int i = 0; i < num_batt; i++) {
    if (millis() - last_rs485_poll[i] > 90000) {
        mark_battery_stale(i + 1);
    }
}
```

## Build Configuration

### ESPHome YAML Structure
```yaml
esphome:
  name: deye-bms-can
  platform: ESP32
  board: esp32-s3-devkitc-1
  min_version: "2026.1.0"
  includes:
    - includes/set_include.h

globals:
  # 50+ global variables for state tracking
  - id: bms_soc
    type: int
  - id: can_stale
    type: bool
  # ... vectors, timestamps, etc.

canbus:
  - platform: esp32_can
    tx_pin: GPIO15
    rx_pin: GPIO16
    bit_rate: 500kbps
    mode: LISTENONLY
    on_frame:
      # Frame handlers for 0x351, 0x355, 0x359, 0x35C, 0x370

uart:
  id: rs485_uart
  tx_pin: GPIO17
  rx_pin: GPIO18
  flow_control_pin: GPIO21
  baud_rate: 9600

interval:
  - interval: 10ms  # RS485 state machine
    then:
      - lambda: rs485_state_machine();

sensor:
  # 120+ sensors for CAN and RS485 data
  - platform: template
    name: "CAN SOC"
    id: sensor_can_soc
```

### Compilation Output
```
INFO ESPHome 2026.1.0
INFO Reading configuration deye-bms-can.yaml...
INFO Compiling...
RAM:   [====      ]  25% (used 81920 bytes from 327680 bytes)
Flash: [======    ]  65% (used 1200000 bytes from 1835008 bytes)
INFO Successfully compiled program.
```

## Memory Usage Breakdown

| Component | Size | Notes |
|-----------|------|-------|
| Cell voltage vectors | 384 bytes | 48 floats × 4 bytes × 2 (current + last) |
| Temperature vectors | 144 bytes | 18 floats × 4 bytes × 2 |
| Battery data vectors | 480 bytes | Various per-battery metrics |
| CAN state variables | 256 bytes | Last values, timestamps, flags |
| RS485 state machine | 512 bytes | Buffers, state variables |
| MQTT publish tracking | 384 bytes | Timestamps for hysteresis |
| **Total vectors/state** | **~2.1 KB** | Dynamic allocation |
| Stack | ~16 KB | Function call stack |
| Heap | ~64 KB | Available for runtime |
| **Total RAM** | **~80 KB** | Includes all allocations |

## Performance Profiling

**Measured via logging**:
- CAN frame decode: 0.3-0.8ms
- RS485 command send: 0.1ms
- RS485 response wait: 50-250ms (I/O bound)
- RS485 parse: 2-5ms
- MQTT publish: 5-10ms (WiFi dependent)
- Vector resize on boot: 5-10ms

## Testing and Validation

### Unit Test Scenarios
1. **CAN frame decoding**: Verify all 5 frame types parse correctly
2. **RS485 state machine**: Verify all state transitions
3. **Hysteresis logic**: Verify threshold and heartbeat behavior
4. **Vector initialization**: Verify correct sizing for 1-3 batteries
5. **Stale detection**: Verify timeout and recovery

### Integration Tests
1. **Full data flow**: CAN → ESP32 → MQTT → Home Assistant
2. **RS485 polling**: Verify all 3 batteries polled correctly
3. **Hysteresis**: Verify MQTT publish rate throttled
4. **Multi-battery**: Verify 1, 2, 3 battery configurations
5. **Recovery**: Power cycle, WiFi reconnect, MQTT reconnect

## Related Documentation

- **HLD.md**: High-level system architecture
- **README.md**: Quick start guide
- **PROTOCOL_REFERENCE.md**: Complete protocol specifications
- **docs/guides/TROUBLESHOOTING.md**: Common issues
