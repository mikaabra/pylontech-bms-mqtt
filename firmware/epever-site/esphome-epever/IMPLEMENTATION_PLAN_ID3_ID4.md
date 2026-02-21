# Implementation Plan: Separate Address 3 (BMS-Link) and Address 4 (Battery)

## Date: 2026-01-13

## Discovery Summary

Through testing, we discovered that the Epever inverter treats the two Modbus addresses differently:

- **Address 3**: BMS-Link configuration device (receives writes from inverter)
- **Address 4**: Battery real-time data (read-only from inverter perspective)

### Key Evidence

1. **Polling Pattern:**
   - Address 3: Read + **WRITE** operations (Function 0x10 writes 32 registers to 0x9000)
   - Address 4: **Read-only** operations

2. **Register 0x9009 Discovery:**
   - Epever **writes** 5000 (50Hz) to Address 3, register 0x9009
   - This explains why we saw frequency in 0x9009 - it's the BMS-Link AC frequency config!
   - Address 4's 0x9009 might actually be battery temperature (needs testing)

3. **Write Data Captured:**
```
Address 3, Function 0x10, write to 0x9000, 32 registers:
0x9000 = 5460  (54.60V UV Warning)
0x9001 = 8000  (80.00V LV Protection)
0x9002 = 5620  (56.20V OV Warning)
0x9003 = 5500  (55.00V OV Protection)
0x9004 = 4350  (43.5A Charge Rated)
0x9005 = 4630  (46.3A Charge Limit)
0x9006 = 4800  (48.0A Discharge Rated)
0x9007 = 5320  (53.2A Discharge Limit)
0x9008 = 22000 (220.0°C Charge High Temp - likely wrong, inverter bug?)
0x9009 = 5000  (50.0Hz **AC FREQUENCY**)
0x900A = 1
... (22 more registers)
```

## Current Architecture

Currently, both Address 3 and Address 4 are handled identically:
- Same register map
- Same data sources (all from CAN bus battery data)
- Writes are acknowledged but not stored or echoed back

## Proposed Architecture

### Address 3 (BMS-Link Configuration Device)

**Purpose:** Configuration/settings interface that the inverter writes to and reads back

**Register Ranges:**
- **0x9000-0x901F (32 registers)**: Configuration written by inverter
  - Store these values when written (Function 0x10)
  - Echo them back when read (Function 0x03)
  - Initialize with sensible defaults on boot

- **0x3100-0x3130**: Could be BMS-Link status/info (need more investigation)
  - Possibly static information about the BMS-Link device
  - May need different values than battery data

**Behavior:**
- **Function 0x03 (Read Holding)**: Return stored configuration values
- **Function 0x04 (Read Input)**: Return status/info registers
- **Function 0x10 (Write Multiple)**: Store written values for later echo
- **Function 0x05 (Write Coil)**: Acknowledge (currently happening)

### Address 4 (Battery Real-Time Data)

**Purpose:** Real-time battery data from CAN bus

**Register Ranges:**
- **0x3100-0x3130**: Real-time battery data (current implementation)
  - Voltage, current, SOC, temperatures, cell voltages
  - All sourced from Pylontech CAN frames

- **0x9000-0x9014 (21 registers)**: Battery protection thresholds
  - Derived from CAN data (charge/discharge limits, voltages)
  - **0x9009 might be battery temperature** (needs testing!)

**Behavior:**
- **Function 0x03 (Read Holding)**: Return battery protection data
- **Function 0x04 (Read Input)**: Return real-time battery measurements
- **Read-only**: No writes expected or needed

## Implementation Steps

### Step 1: Add Storage for Address 3 Configuration

```yaml
globals:
  # Address 3 (BMS-Link) configuration storage
  - id: addr3_config_0x9000
    type: std::vector<uint16_t>
    restore_value: no
    initial_value: '{}'  # Will initialize in on_boot
```

### Step 2: Initialize Address 3 Storage on Boot

```yaml
esphome:
  on_boot:
    - priority: -100
      then:
        - lambda: |-
            // Initialize Address 3 config with defaults
            id(addr3_config_0x9000).resize(32);
            // Set defaults (will be overwritten by inverter)
            id(addr3_config_0x9000)[0] = 5460;   // 0x9000: UV Warning
            id(addr3_config_0x9000)[1] = 8000;   // 0x9001: LV Protection
            id(addr3_config_0x9000)[2] = 5620;   // 0x9002: OV Warning
            id(addr3_config_0x9000)[3] = 5500;   // 0x9003: OV Protection
            id(addr3_config_0x9000)[4] = 4350;   // 0x9004: Charge Rated
            id(addr3_config_0x9000)[5] = 4630;   // 0x9005: Charge Limit
            id(addr3_config_0x9000)[6] = 4800;   // 0x9006: Discharge Rated
            id(addr3_config_0x9000)[7] = 5320;   // 0x9007: Discharge Limit
            id(addr3_config_0x9000)[8] = 4500;   // 0x9008: Charge High Temp
            id(addr3_config_0x9000)[9] = 0;      // 0x9009: AC Frequency (will be written by inverter)
            // ... rest with 0s
```

### Step 3: Refactor Modbus Handler - Split by Address

Current code structure:
```
if (slave_addr != 3 && slave_addr != 4) return;  // Early exit
// Build response (same for both)
```

New structure:
```
if (slave_addr == 3) {
  // Handle Address 3 (BMS-Link) logic
  // - Store writes to 0x9000-0x901F
  // - Echo stored config on reads
} else if (slave_addr == 4) {
  // Handle Address 4 (Battery) logic
  // - Real-time CAN data
  // - Battery-specific registers
} else {
  return;  // Unknown address
}
```

### Step 4: Implement Address 3 Write Handler (Function 0x10)

```cpp
// Inside Address 3 handler, when func_code == 0x10:
if (start_reg == 0x9000 && reg_count <= 32) {
  // Parse write data
  uint8_t byte_count = rx_buffer[6];
  if (byte_count == reg_count * 2) {
    // Store written values
    for (uint16_t i = 0; i < reg_count; i++) {
      uint8_t hi = rx_buffer[7 + i*2];
      uint8_t lo = rx_buffer[7 + i*2 + 1];
      uint16_t value = (hi << 8) | lo;

      uint16_t reg_offset = i;
      if (reg_offset < id(addr3_config_0x9000).size()) {
        id(addr3_config_0x9000)[reg_offset] = value;
      }
    }
    ESP_LOGI("modbus", "Stored %d config values to Address 3, start=0x%04X",
             reg_count, start_reg);

    // Log important values
    if (id(addr3_config_0x9000)[9] > 0) {
      ESP_LOGI("modbus", "AC Frequency: %.1fHz", id(addr3_config_0x9000)[9] / 100.0f);
    }
  }
}
// Send acknowledgment response
```

### Step 5: Implement Address 3 Read Handler (Function 0x03)

```cpp
// Inside Address 3 handler, when func_code == 0x03:
if (start_reg >= 0x9000 && start_reg < 0x9020) {
  uint8_t byte_count = reg_count * 2;
  response.push_back(byte_count);

  for (uint16_t r = start_reg; r < start_reg + reg_count; r++) {
    uint16_t value = 0;
    uint16_t offset = r - 0x9000;

    if (offset < id(addr3_config_0x9000).size()) {
      value = id(addr3_config_0x9000)[offset];
    }

    // Add to response (big-endian)
    response.push_back((value >> 8) & 0xFF);
    response.push_back(value & 0xFF);
  }
}
```

### Step 6: Update Address 4 Register 0x9009 (Test Theory)

For Address 4, try setting 0x9009 to battery temperature:

```cpp
// Inside Address 4 handler, in 0x9000 range switch:
case 0x9009:
  // Test: Maybe this is battery temperature for Address 4?
  value = (uint16_t)(id(bms_temp_min) * 100.0f);
  ESP_LOGD("modbus", "Addr4 0x9009 = %d (%.1f°C)", value, id(bms_temp_min));
  break;
```

## Testing Strategy

### Phase 1: Basic Implementation
1. Implement storage and write handler for Address 3
2. Verify writes are being stored (check logs)
3. Verify stored values are echoed back on reads

### Phase 2: Validate Address 3 Behavior
1. Check if "Load Frequency" shows correct value (50Hz from stored 0x9009)
2. Monitor for any errors or unexpected behavior
3. Capture logs of write/read cycles

### Phase 3: Test Address 4 Theory
1. Set Address 4's 0x9009 to battery temperature
2. Check if "Battery Temp" on Epever display updates
3. If it works, we've solved the temperature issue!

### Phase 4: Optimize and Clean Up
1. Remove commented-out code
2. Add comprehensive logging
3. Document final register mapping

## Expected Outcomes

After implementation:
- ✅ Load frequency shows 50Hz (from Address 3, 0x9009)
- ✅ Battery temperature shows correctly (from Address 4, 0x9009)
- ✅ All writes are properly acknowledged and stored
- ✅ Inverter behaves as if talking to real BMS-Link device

## Risks and Considerations

1. **Code Complexity**: Significantly more complex than current implementation
2. **Testing Required**: Need extensive testing to avoid breaking current working features
3. **Unknown Registers**: Some Address 3 registers (0x3100 range) may need different handling
4. **Performance**: Vector storage and lookups should be fine, but worth monitoring

## Files to Modify

1. `esphome-epever/epever-can-bridge.yaml` - Main implementation
2. `esphome-epever/EPEVER_MODBUS_FINDINGS.md` - Update with findings
3. `esphome-epever/IMPLEMENTATION_PLAN_ID3_ID4.md` - This file (track progress)

## Implementation Checklist

- [ ] Add `addr3_config_0x9000` global storage vector
- [ ] Add initialization in `on_boot`
- [ ] Refactor modbus handler to split Address 3 and Address 4 logic
- [ ] Implement Address 3 write handler (Function 0x10, store values)
- [ ] Implement Address 3 read handler (Function 0x03, echo stored values)
- [ ] Update Address 4 register 0x9009 to send battery temperature
- [ ] Test and verify "Load Frequency" displays 50Hz
- [ ] Test and verify "Battery Temp" displays correctly
- [ ] Add comprehensive logging for debugging
- [ ] Update documentation with final findings
- [ ] Commit and push to GitHub

## Next Steps

After review and approval:
1. Start with Phase 1 implementation
2. Test incrementally after each change
3. Document any new discoveries
4. Iterate based on results
