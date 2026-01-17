# Low-Level Design (LLD) - ESPHome EPever CAN Bridge

## Technical Specifications

### Hardware Platform
- **Board**: Waveshare ESP32-S3-RS485-CAN
- **MCU**: ESP32-S3-WROOM-1-N8R2 (8MB Flash, 2MB PSRAM)
- **CAN Controller**: Built-in TWAI (Two-Wire Automotive Interface)
- **RS485**: Isolated transceiver with hardware flow control
- **Power**: USB-C or 5V terminal block

### Firmware Environment
- **Framework**: ESPHome 2026.1.0-dev
- **Platform**: ESP-IDF 5.5.2
- **Language**: C++ (Arduino/ESP-IDF)
- **Build System**: PlatformIO
- **OTA**: Supported via WiFi

## Protocol Implementations

### CAN Bus Protocol (Pylontech V1.2)

For complete protocol reference, see [docs/PROTOCOL_REFERENCE.md](../docs/PROTOCOL_REFERENCE.md).

#### Frame ID 0x351 - Voltage/Current Limits
**Length**: 8 bytes, little-endian 16-bit values

| Byte | Field | Formula | Units |
|------|-------|---------|-------|
| 0-1 | Charge voltage limit | value / 10 | V |
| 2-3 | Charge current limit | value / 10 | A |
| 4-5 | Discharge current limit | value / 10 | A |
| 6-7 | Discharge voltage limit | value / 10 | V |

**Implementation** (lines 119-133):
```cpp
uint16_t v_charge = (x[1] << 8) | x[0];
uint16_t i_charge = (x[3] << 8) | x[2];
uint16_t i_discharge = (x[5] << 8) | x[4];
uint16_t v_low = (x[7] << 8) | x[6];

id(bms_v_charge_max) = v_charge / 10.0f;
id(bms_i_charge) = i_charge / 10.0f;
id(bms_i_discharge) = i_discharge / 10.0f;
id(bms_v_low) = v_low / 10.0f;
```

#### Frame ID 0x355 - SOC/SOH
**Length**: 4 bytes, 16-bit little-endian values

| Byte | Field | Formula | Units |
|------|-------|---------|-------|
| 0-1 | State of Charge (SOC) | value / 1 | % |
| 2-3 | State of Health (SOH) | value / 1 | % |

**Implementation** (lines 155-224):
```cpp
uint16_t soc_raw = (x[1] << 8) | x[0];
uint16_t soh_raw = (x[3] << 8) | x[2];
int soc = soc_raw;
int soh = soh_raw;

id(bms_soc) = soc;
id(bms_soh) = soh;

// SOC hysteresis control state machine
if (id(soc_control_enabled)) {
  // Discharge blocking hysteresis
  if (soc < id(soc_discharge_block_threshold) && !id(soc_discharge_blocked)) {
    id(soc_discharge_blocked) = true;
    id(inverter_priority_update_requested) = true;  // Trigger instant update
  }
  if (soc > id(soc_discharge_allow_threshold) && id(soc_discharge_blocked)) {
    id(soc_discharge_blocked) = false;
    id(inverter_priority_update_requested) = true;  // Trigger instant update
  }

  // Force charge hysteresis
  if (soc < id(soc_force_charge_on_threshold) && !id(soc_force_charge_active)) {
    id(soc_force_charge_active) = true;
  }
  if (soc >= id(soc_force_charge_off_threshold) && id(soc_force_charge_active)) {
    id(soc_force_charge_active) = false;
  }
}

// Mode mismatch detection - check on every CAN frame
// Triggers correction if inverter mode doesn't match desired state
int desired_mode = id(soc_discharge_blocked) ? 1 : 0;
if (id(inverter_priority_control_mode) == 0 && desired_mode != id(inverter_priority_mode)) {
  ESP_LOGI("soc_control", "Mode mismatch detected: current=%d, desired=%d",
           id(inverter_priority_mode), desired_mode);
  id(inverter_priority_update_requested) = true;  // Trigger instant correction
}
```

**NEW: Instant Response Mechanism**:
- When `soc_discharge_blocked` flag changes, sets `inverter_priority_update_requested = true`
- Fast 1-second interval checks this flag and triggers Modbus write to register 0x9608
- Mode mismatch detection verifies inverter mode on every CAN frame and corrects if needed

#### Frame ID 0x35C - Battery Charge Request Flags (V1.2)
**Length**: 1-2 bytes (only byte 0 is used)

| Bit | Mask | Field | Meaning |
|-----|------|-------|---------|
| 7 | 0x80 | RCE | **Charge Enable** - Battery accepts charging |
| 6 | 0x40 | RDE | **Discharge Enable** - Battery allows discharge |
| 5 | 0x20 | CI1 | **Force Charge Level 1** - Request immediate charge |
| 4 | 0x10 | CI2 | **Force Charge Level 2** - Urgent charge request |
| 3 | 0x08 | RFC | **Request Full Charge** - Request 100% charge |

**CRITICAL**: Bits 7 and 6 control charge/discharge enable. Bit 5 is NOT charge enable - it's a force charge request flag.

**Implementation** (lines 241-282):
```cpp
uint8_t flags = x[0];

// CORRECTED bit mapping per Pylontech CAN V1.2 spec
bool charge_en = (flags & 0x80) != 0;       // Bit 7 - Charge Enable
bool discharge_en = (flags & 0x40) != 0;    // Bit 6 - Discharge Enable
bool force_chg = (flags & 0x20) != 0;       // Bit 5 - Force Charge Level 1

id(can_charge_enabled) = charge_en;
id(can_discharge_enabled) = discharge_en;
id(can_force_charge_request) = force_chg;
```

**Bug History**: Originally implemented with bit 5 as charge enable (WRONG). Fixed 2026-01-15 based on Setfire Labs documentation.

#### Frame ID 0x370 - Cell Voltage/Temperature Extremes
**Length**: 8 bytes

| Byte | Field | Formula | Units |
|------|-------|---------|-------|
| 0-1 | Max cell voltage | value / 1000 | V |
| 2-3 | Min cell voltage | value / 1000 | V |
| 4-5 | Max cell temperature | value / 10 - 273.15 | °C |
| 6-7 | Min cell temperature | value / 10 - 273.15 | °C |

**Implementation** (lines 284-305):
```cpp
uint16_t cell_v_max_mv = (x[1] << 8) | x[0];
uint16_t cell_v_min_mv = (x[3] << 8) | x[2];
uint16_t temp_max_dK = (x[5] << 8) | x[4];
uint16_t temp_min_dK = (x[7] << 8) | x[6];

id(bms_cell_v_max) = cell_v_max_mv / 1000.0f;
id(bms_cell_v_min) = cell_v_min_mv / 1000.0f;
id(bms_temp_max) = (temp_max_dK / 10.0f) - 273.15f;
id(bms_temp_min) = (temp_min_dK / 10.0f) - 273.15f;
```

### Modbus Server Protocol (BMS-Link)

For complete protocol reference, see [EPEVER_MODBUS_FINDINGS.md](EPEVER_MODBUS_FINDINGS.md).

#### Register Map: 0x3100-0x3127 (Battery Real-Time Data)

**Implementation**: Custom Modbus server using ESPHome `modbus_controller` component with `on_receive` callback.

**Key Registers**:

| Register | Name | Value Source | Scaling |
|----------|------|--------------|---------|
| 0x3100 | Battery capacity (Ah) | Hardcoded | 560 (2× 280Ah batteries) |
| 0x3101 | Battery voltage (V) | CAN 0x351 (v_charge_max) | × 100 |
| 0x3102 | Battery current (A) | Hardcoded 0 | Not available from CAN |
| 0x3105 | Total capacity (Ah) | Hardcoded | 560 |
| 0x3106 | SOC (%) | CAN 0x355 | × 1 |
| 0x3107 | SOH (%) | CAN 0x355 | × 1 |
| 0x3108 | Max temperature (°C) | CAN 0x370 | × 10 |
| 0x3109 | Min temperature (°C) | CAN 0x370 | × 10 |
| 0x3111 | MOS status | CAN 0x35C | See below |
| 0x3127 | BMS status | Layered control | See below |

#### Register 0x3111 - MOS Status (Bitfield)

| Bit | Name | Source | Logic |
|-----|------|--------|-------|
| D0 | Discharge MOS | CAN charge enable | 1=ON, 0=OFF |
| D1 | Charge MOS | CAN discharge enable | 1=ON, 0=OFF |

**Implementation** (lines 633-640):
```cpp
case 0x3111:  // MOS status - dynamically set from CAN flags
  value = 0;
  if (id(can_discharge_enabled)) value |= 0x01;  // D0
  if (id(can_charge_enabled)) value |= 0x02;     // D1
  break;
```

#### Register 0x3127 - BMS Status (Bitfield)

**Critical Register**: Controls inverter charge/discharge behavior

| Bit | Hex | Name | Logic |
|-----|-----|------|-------|
| D13 | 0x2000 | Force Charge Mark | 1=Force, 0=Normal |
| D14 | 0x4000 | Stop Discharge | 1=Stop, 0=Enable (INVERTED) |
| D15 | 0x8000 | Stop Charge | 1=Stop, 0=Enable (INVERTED) |

**Layered Control Logic** (lines 830-864):
```cpp
// Determine D13 (Force Charge) - layered priority
bool d13_force_charge = false;
if (id(manual_mode_d13_force_charge) == 0) {
  // Auto mode: Layer SOC control over CAN
  if (id(soc_control_enabled) && id(soc_force_charge_active)) {
    d13_force_charge = true;  // SOC control activates force charge
  } else {
    d13_force_charge = id(can_force_charge_request);  // Use CAN value
  }
} else if (id(manual_mode_d13_force_charge) == 2) {
  d13_force_charge = true;  // Manual override: Force On
}

// Determine D14 (Stop Discharge) - BMS only (no longer used for SOC control)
bool d14_stop_discharge = false;
if (id(manual_mode_d14_stop_discharge) == 0) {
  // Auto mode: Use CAN value only (BMS protection)
  d14_stop_discharge = !id(can_discharge_enabled);  // BMS blocks discharge
  // NOTE: SOC control now uses inverter priority mode switching instead
} else if (id(manual_mode_d14_stop_discharge) == 2) {
  d14_stop_discharge = true;  // Manual override: Force On (Block)
}

// Determine D15 (Stop Charge) - BMS only
bool d15_stop_charge = false;
if (id(manual_mode_d15_stop_charge) == 0) {
  // Auto mode: Use CAN value (inverted)
  d15_stop_charge = !id(can_charge_enabled);
} else if (id(manual_mode_d15_stop_charge) == 2) {
  d15_stop_charge = true;  // Manual override: Force On (Block)
}

// Construct register value
value = 0x0000;
if (d13_force_charge) value |= 0x2000;
if (d14_stop_discharge) value |= 0x4000;
if (d15_stop_charge) value |= 0x8000;
```

**IMPORTANT CHANGE** (2026-01-16): D14 flag is no longer used for SOC discharge control. SOC control now uses **inverter priority mode switching** via Modbus register 0x9608 (see Modbus Client section below). This allows the battery to remain available during power outages regardless of SOC reserve settings.

**Example Values**:
- `0x0000`: All OK (charge enabled, discharge enabled, no force)
- `0x2000`: Force charge active
- `0x4000`: Discharge blocked (SOC reserve or BMS protection)
- `0x6000`: Force charge + discharge blocked (recovering from low SOC)
- `0x8000`: Charge blocked (BMS protection, overvolt, high temp, 100% SOC)
- `0xC000`: Both charge and discharge blocked (BMS protection)

#### Register Map: 0x9000-0x9019 (Configuration Echo)

**Purpose**: Store and echo inverter configuration writes

The inverter writes configuration to Address 3 (BMS-Link), and the BMS-Link writes the same configuration to the ESP32 at Address 1. The ESP32 stores these values and echoes them back when polled.

**Implementation** (lines 315-342, 778-824):
```cpp
// Storage vector (32 registers)
std::vector<uint16_t> addr3_config_0x9000;

// On Function 0x10 write from BMS-Link
if (address == 0x9000 && register_count == 32) {
  for (int i = 0; i < 32; i++) {
    id(addr3_config_0x9000)[i] = data[i];
  }
}

// On Function 0x03 read request
if (start_address >= 0x9000 && start_address + register_count <= 0x9020) {
  for (uint16_t i = 0; i < register_count; i++) {
    uint16_t reg = start_address + i;
    uint16_t idx = reg - 0x9000;
    value = id(addr3_config_0x9000)[idx];
    // Write value to response buffer
  }
}
```

**Key Configuration Register**:
- **0x9009**: AC Grid Frequency (Hz × 100)
  - Example: 5000 = 50.0 Hz
  - **BMS-Link documentation is WRONG** - claims this is "Charging Low Temperature Protection"
  - **Actual function**: AC frequency for inverter display

### Modbus Client Protocol (Inverter Priority Control)

**NEW** (2026-01-16): ESP32 acts as Modbus RTU over TCP client to control inverter priority mode.

#### Connection Details
- **Protocol**: Modbus RTU over TCP (not Modbus TCP - uses RTU framing over TCP socket)
- **Gateway**: 10.10.0.117:9999 (Modbus RTU to TCP bridge)
- **Slave ID**: 10 (EPever inverter)
- **Target Register**: 0x9608 (Inverter Output Priority Mode)

#### Register 0x9608 - Inverter Output Priority Mode

| Value | Mode | Behavior |
|-------|------|----------|
| 0 | Inverter Priority | Battery/PV preferred, grid backup |
| 1 | Utility Priority | Grid preferred, battery/PV backup |

#### Implementation Details

**Read Operation** (Function 0x03/0x04):
```cpp
// Build command: Slave=10, Func=0x03, Reg=0x9608, Count=1
cmd = [0x0A, 0x03, 0x96, 0x08, 0x00, 0x01, CRC_L, CRC_H];

// EPever sometimes responds with function 0x04 instead of 0x03
// Firmware accepts BOTH function codes for reliability
if (response[1] == 0x03 || response[1] == 0x04) {
  value = (response[3] << 8) | response[4];
  id(inverter_priority_mode) = value;
}
```

**Write Operation** (Function 0x10):
```cpp
// EPever requires function 0x10 even for single register
// Function 0x06 returns exception 0x01 (Illegal Function)
cmd = [0x0A, 0x10, 0x96, 0x08, 0x00, 0x01, 0x02, value_H, value_L, CRC_L, CRC_H];

// Expected response: [0x0A, 0x10, 0x96, 0x08, 0x00, 0x01, CRC_L, CRC_H]
if (response[1] == 0x10 && response_register == 0x9608) {
  // Success!
  id(inverter_priority_write_successes)++;
}
```

**Update Triggers** (lines 754-918):
1. **Fast interval** (1 second): Checks `inverter_priority_update_requested` flag
   - Set by SOC threshold crossing
   - Set by mode mismatch detection
   - Set by control changes (after 5-second delay)
2. **Background interval** (3 hours, configurable): Periodic verification

**Reliability Features**:
- **Retry logic**: Automatic retry on Modbus timeout
- **Dual function support**: Accepts both 0x03 and 0x04 responses
- **Statistics tracking**: Attempts, successes, failures, success rate (persists to NVRAM)
- **Conditional updates**: Only sends write command if mode actually needs to change

**Example Flow**:
```
1. SOC drops below 50%
   └─> soc_discharge_blocked = true
   └─> inverter_priority_update_requested = true

2. Fast interval (within 1 second) detects flag
   └─> desired_mode = 1 (Utility Priority)
   └─> current_mode = 0 (Inverter Priority)
   └─> Mode change needed!

3. Modbus write to 0x9608
   └─> TX: 0A 10 96 08 00 01 02 00 01 E3 E1
   └─> RX: 0A 10 96 08 00 01 AC F8

4. Success!
   └─> inverter_priority_mode = 1
   └─> inverter_priority_update_requested = false
   └─> Battery stops cycling, inverter runs on grid
```

## SOC Reserve Control State Machine

### Global State Variables

```cpp
// Master enable/disable
bool soc_control_enabled = false;  // Default: OFF

// Discharge control thresholds
int soc_discharge_block_threshold = 50;   // Block discharge below this
int soc_discharge_allow_threshold = 55;   // Allow discharge above this

// Force charge control thresholds
int soc_force_charge_on_threshold = 45;   // Activate force charge below this
int soc_force_charge_off_threshold = 50;  // Clear force charge above this

// Hysteresis state tracking
bool soc_discharge_blocked = false;
bool soc_force_charge_active = false;
```

### State Transition Diagram

```
SOC Discharge Blocking State Machine:

    ┌─────────────┐
    │  ALLOWED    │  (soc_discharge_blocked = false)
    │ (SOC > 55%) │
    └──────┬──────┘
           │
           │ SOC drops below 50%
           ▼
    ┌─────────────┐
    │  BLOCKED    │  (soc_discharge_blocked = true)
    │ (SOC < 55%) │
    └──────┬──────┘
           │
           │ SOC rises above 55%
           ▼
    ┌─────────────┐
    │  ALLOWED    │
    └─────────────┘


SOC Force Charge State Machine:

    ┌─────────────┐
    │     OFF     │  (soc_force_charge_active = false)
    │ (SOC ≥ 50%) │
    └──────┬──────┘
           │
           │ SOC drops below 45%
           ▼
    ┌─────────────┐
    │   ACTIVE    │  (soc_force_charge_active = true)
    │ (SOC < 50%) │
    └──────┬──────┘
           │
           │ SOC rises to 50%
           ▼
    ┌─────────────┐
    │     OFF     │
    └─────────────┘
```

### Anti-Cycling Mechanism

**Problem**: Without a gap between thresholds, the system could cycle:
1. Battery charges to 50% → Force charge clears
2. Discharge immediately allowed at 50%
3. Battery discharges to 49%
4. Force charge activates again
5. Cycle repeats: 49% ↔ 50% (oscillation)

**Solution**: 5% safety buffer (50-55%)

**How it works**:
- Force charge clears at 50% (lower threshold)
- Discharge remains BLOCKED until 56% (upper threshold + 1%)
- Battery cannot discharge during 50-55% range
- Battery naturally charges to 56% before discharge is allowed

**Example with default thresholds**:
```
SOC    Force Charge    Discharge Block    What Happens
----   -------------   ----------------   ----------------------------------
49%    ✓ Active        ✓ Blocked          Battery idle, waiting for charge
50%    ✗ CLEARED       ✓ Still BLOCKED    Charging from PV (no cycling!)
52%    ✗ Off           ✓ Still BLOCKED    Charging continues
54%    ✗ Off           ✓ Still BLOCKED    Charging continues
56%    ✗ Off           ✗ ALLOWED          Battery available for discharge
```

## Web UI Implementation

### ESPHome Components Used

1. **select (template)**: Dropdown menus
   - `restore_value: true` - Saves selection to NVRAM
   - `optimistic: true` - Updates immediately without server confirmation
   - `on_value` callback - Updates global variables

2. **switch (template)**: Toggle switches
   - Default restore mode - Saves ON/OFF state to NVRAM
   - `turn_on_action` / `turn_off_action` - Callbacks for state changes

3. **binary_sensor (template)**: Status indicators
   - `lambda` - Returns boolean expression
   - Updates automatically when expression changes

### Configuration Dropdowns

#### D13/D14/D15 Manual Controls
**Options**: "Auto (CAN)" / "Force Off" / "Force On"

**Encoding**:
- `manual_mode_d13_force_charge` = 0 (Auto), 1 (Force Off), 2 (Force On)
- `manual_mode_d14_stop_discharge` = 0 (Auto), 1 (Force Off), 2 (Force On)
- `manual_mode_d15_stop_charge` = 0 (Auto), 1 (Force Off), 2 (Force On)

#### SOC Threshold Selectors
**Discharge Control Options**: "40% / 45%", "45% / 50%", "50% / 55%", "55% / 60%", "60% / 65%"

**Encoding** (lines 1437-1460):
```cpp
if (x == "50% / 55%") {
  id(soc_discharge_block_threshold) = 50;
  id(soc_discharge_allow_threshold) = 55;
}
```

**Force Charge Control Options**: "35% / 40%", "40% / 45%", "45% / 50%", "50% / 55%", "55% / 60%"

### NVRAM Persistence

**ESPHome Preferences System**:
- Automatic flash storage management
- Wear leveling for flash longevity
- Atomic writes to prevent corruption
- Restores on boot before `on_boot` runs

**Stored Settings**:
- All dropdown selections (5 selects)
- Master switch state (1 switch)

**Not Stored** (intentional):
- Hysteresis state flags (soc_discharge_blocked, soc_force_charge_active)
- CAN-derived values (charge_enabled, discharge_enabled, SOC, etc.)
- Temporary status values

## Modbus Interaction Log Buffer

### Purpose
RAM-backed circular buffer for capturing Modbus RTU over TCP interactions with the EPever inverter for troubleshooting and debugging.

### Global Variables (lines 551-563)

```yaml
globals:
  - id: modbus_log_buffer
    type: std::vector<std::string>
    restore_value: no                    # RAM only, no NVRAM persistence

  - id: modbus_log_index
    type: int
    initial_value: '0'
    restore_value: no

  - id: modbus_log_max_entries
    type: int
    initial_value: '50'                  # 50 entries × ~160 bytes = ~8KB
    restore_value: no
```

### Helper Lambda Function

Added in three locations (auto interval, triggered interval, manual refresh button):

```cpp
auto append_modbus_log = [](const std::string& message) {
  // Add uptime timestamp [NNNNNN]
  uint32_t uptime_sec = millis() / 1000;
  char timestamp[12];
  snprintf(timestamp, sizeof(timestamp), "[%06u] ", uptime_sec);

  std::string entry = std::string(timestamp) + message;

  // Circular buffer logic
  if (id(modbus_log_buffer).size() < id(modbus_log_max_entries)) {
    // Buffer not full - just append
    id(modbus_log_buffer).push_back(entry);
  } else {
    // Buffer full - overwrite oldest entry
    id(modbus_log_buffer)[id(modbus_log_index)] = entry;
    id(modbus_log_index) = (id(modbus_log_index) + 1) % id(modbus_log_max_entries);
  }
};
```

### Logging Points

**Auto check interval** (lines 659-801):
- Line 659: "Auto check: current=X, desired=Y (SOC=Z%)"
- Line 672: "Mode change needed: X -> Y"
- Line 715-720: TX command hex dump
- Line 761-766: RX response hex dump
- Line 783: Exception errors
- Line 797: Success messages
- Line 808, 815, 823, 831: Various error conditions

**Triggered interval** (lines 873-1006): Similar logging pattern

**Manual refresh button** (lines 2208-2354): READ command logging only

### Text Sensor Output (lines 2566-2596)

```yaml
text_sensor:
  - platform: template
    name: "zzz Modbus Interaction Log"
    id: modbus_log_text
    icon: "mdi:script-text-outline"
    entity_category: diagnostic
    update_interval: 5s
    lambda: |-
      if (id(modbus_log_buffer).empty()) {
        return std::string("No Modbus interactions logged yet");
      }

      std::string result;
      int total = id(modbus_log_buffer).size();

      if (total < id(modbus_log_max_entries)) {
        // Buffer not full - display in order
        for (int i = 0; i < total; i++) {
          result += id(modbus_log_buffer)[i] + "\n";
        }
      } else {
        // Buffer full - display from oldest to newest
        for (int i = 0; i < total; i++) {
          int idx = (id(modbus_log_index) + i) % id(modbus_log_max_entries);
          result += id(modbus_log_buffer)[idx] + "\n";
        }
      }

      return result;
```

**Note**: Named "zzz Modbus Interaction Log" to force alphabetical sorting to bottom of web UI.

### Memory Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| Buffer size | ~8 KB | 50 entries × 160 bytes average |
| Entry format | `[NNNNNN] Message` | Uptime in seconds + message |
| Growth behavior | Fixed | Overwrites oldest when full |
| Persistence | RAM only | Clears on power loss |
| Flash wear | None | No NVRAM writes |

### Access Methods

**API Endpoint:**
```
GET http://10.10.0.45/text_sensor/zzz_modbus_interaction_log
```

Returns JSON:
```json
{
  "id": "text_sensor/zzz Modbus Interaction Log",
  "value": "[000001] Triggered update check\n[000001] TX: 0A 10 96...",
  "state": "[000001] Triggered update check\n[000001] TX: 0A 10 96..."
}
```

**Terminal Viewer:**
- Script: `modbus_log_tail.sh`
- Fetches JSON from API endpoint
- Applies ANSI color syntax highlighting
- Three modes: one-shot, follow (5s), watch (2s)

**Web Viewers:**
- `modbus_log_viewer.html`: Standalone HTML with auto-refresh
- `modbus_log_server.py`: Python proxy for SSH tunnel scenarios

### Implementation Notes

**Circular Buffer Algorithm:**
- Uses `std::vector<std::string>` for dynamic array storage
- Integer index tracks oldest entry position
- Modulo arithmetic wraps index: `(index + 1) % max_entries`
- Always displays oldest-to-newest chronological order

**Why Not Ring Buffer (Linked List)?**
- Vectors provide faster random access for display
- Simpler implementation in ESPHome lambda environment
- Predictable memory usage (no fragmentation)

**Why Prefix with "zzz"?**
- ESPHome web UI sorts entities alphabetically by name
- YAML file order has no effect on web UI ordering
- "zzz" prefix forces sensor to appear last in list
- Alternative approaches (internal, entity_category) break API access

### Log Entry Format Examples

```
[000005] Triggered update check
[000171] Mode change needed: 0 -> 1 (SOC=48%)
[000171] TX: 0A 10 96 08 00 01 02 00 01 E3 E1
[000172] RX: 0A 10 96 08 00 01 AC F8
[000172] ✓ Mode changed successfully to Utility Priority (1)
[000300] ✗ Connection failed (errno=111: Connection refused)
[000301] ✗ Exception 0x01: Illegal Function
```

## Build Configuration

### ESPHome YAML Structure

```yaml
esphome:
  name: epever-can-bridge
  platform: ESP32
  board: esp32-s3-devkitc-1

# Global variables (370-397)
globals:
  - id: bms_soc
    type: int
  - id: soc_control_enabled
    type: bool
  # ... 50+ global variables

# CAN bus (98-114)
canbus:
  - platform: esp32_can
    tx_pin: GPIO15
    rx_pin: GPIO16
    can_id: 0
    bit_rate: 500kbps
    on_frame:
      - can_id: 0x351  # Voltage/current limits
      - can_id: 0x355  # SOC/SOH
      - can_id: 0x35C  # Charge request flags
      - can_id: 0x370  # Cell extremes

# Modbus server (400-900)
modbus_controller:
  uart_id: uart_modbus
  address: 1
  on_receive:
    - lambda: |-
        // Custom Modbus server implementation
        // Function 0x03: Read Holding Registers
        // Function 0x04: Read Input Registers
        // Function 0x10: Write Multiple Registers

# Web UI (1359-1520)
select:
  - platform: template
    name: "D13 Force Charge Control"
    restore_value: true
    # ... 4 more selects

switch:
  - platform: template
    name: "Enable SOC Reserve Control"
    # ... restore enabled by default

binary_sensor:
  - platform: template
    name: "SOC Discharge Blocked"
    lambda: 'return id(soc_control_enabled) && id(soc_discharge_blocked);'
```

### Compilation Output

```
INFO ESPHome 2026.1.0-dev
INFO Reading configuration epever-can-bridge.yaml...
INFO Compiling...
RAM:   [=         ]  11.3% (used 36912 bytes from 327680 bytes)
Flash: [=====     ]  51.0% (used 936083 bytes from 1835008 bytes)
INFO Successfully compiled program.
```

### Custom Component: Listen-Only CAN

**Location**: `esphome-epever/.esphome/external_components/19804791/esp32_can/`

**Modification**: Adds `listen_only` mode support to standard ESPHome CAN component

**Required**: Standard ESPHome CAN component sends ACK signals, which interferes with existing BMS-to-inverter CAN bus communication.

## Error Handling

### CAN Bus Errors
- **No frames received**: 30-second timeout, publishes warning
- **Invalid frame length**: Logs error, skips frame
- **Bus-off state**: Auto-recovery via ESP32 TWAI controller

### Modbus Errors
- **Invalid function code**: Returns Modbus exception 0x01
- **Invalid register address**: Returns exception 0x02
- **CRC mismatch**: No response (per Modbus spec)
- **Timeout**: BMS-Link handles retry logic

### Configuration Errors
- **Invalid threshold selection**: Prevented by dropdown options
- **Manual mode conflict**: Last setting wins (no validation needed)

## Testing and Validation

### Unit Tests (Conceptual)
```cpp
// Test SOC hysteresis state machine
TEST(SOCControl, DischargeBlocking) {
  soc_control_enabled = true;
  soc_discharge_blocked = false;

  // Should activate at threshold
  update_soc(49);
  ASSERT_TRUE(soc_discharge_blocked);

  // Should stay active until upper threshold
  update_soc(54);
  ASSERT_TRUE(soc_discharge_blocked);

  // Should clear above upper threshold
  update_soc(56);
  ASSERT_FALSE(soc_discharge_blocked);
}
```

### Integration Test Scenarios
1. **CAN to Modbus translation**: Verify SOC from CAN matches Modbus 0x3106
2. **SOC reserve activation**: Verify discharge blocked at 50%
3. **Force charge triggering**: Verify D13 set at 45%
4. **Manual override**: Verify manual settings override auto mode
5. **NVRAM persistence**: Verify settings restored after reboot
6. **Anti-cycling**: Verify no oscillation at 50% threshold

## Performance Profiling

**Typical Processing Times** (measured via logging):
- CAN frame decode: 0.1-0.5ms
- Modbus request parsing: 0.5-2ms
- Register value lookup: < 0.1ms
- Modbus response generation: 1-3ms
- Web UI update: 10-20ms
- NVRAM write: 20-50ms

**Memory Usage**:
- Stack: ~8KB
- Heap: ~28KB
- Static globals: ~1KB
- Flash: 936KB (51% of 1.8MB available)

## Related Documentation

- **HLD.md**: High-level system architecture and design decisions
- **PROTOCOL_REFERENCE.md**: Complete CAN and Modbus protocol specifications
- **EPEVER_MODBUS_FINDINGS.md**: Modbus register mapping discoveries
- **SOC_HYSTERESIS_FEASIBILITY.md**: SOC control feature design and implementation
- **SESSION_2026-01-13.md**: Development history and bug fixes
