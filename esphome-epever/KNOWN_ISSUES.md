# Known Issues - EPever CAN Bridge

## Critical: Discharge Flag Blocks Island Mode Operation

**Date Discovered**: 2026-01-15
**Severity**: HIGH - Affects UPS/backup power functionality

### Issue Description

When the EPever inverter receives the "Stop Discharge" flag (D14 = 1, register 0x3127 bit 14), it will **not run on battery power even in island mode** (grid disconnected).

**Expected Behavior**: In island mode, the inverter should use battery power to supply loads regardless of discharge flag state.

**Actual Behavior**: The inverter refuses to discharge the battery even when the grid is unavailable, leaving loads unpowered.

### Impact on SOC Reserve Control

This severely limits the usefulness of the SOC Reserve Control feature:

- **Original Intent**: Block discharge below 50% SOC during normal operation, but allow emergency discharge during power outages
- **Reality**: Once discharge is blocked, the inverter won't use the battery even during a blackout
- **Result**: The "UPS reserve" cannot be used for its intended purpose (backup power)

### Workaround Options

#### Option 1: Disable SOC Reserve Control (Recommended for now)
```
Web UI: Enable SOC Reserve Control = OFF
```

Use inverter's native battery voltage settings instead:
- Set discharge cutoff voltage in inverter (e.g., 48V for 50% SOC on 16S LiFePO4)
- Allows natural discharge curve without flag-based blocking
- Inverter will still discharge during island mode

#### Option 2: Accept Limited Reserve Functionality
Keep SOC reserve enabled but understand:
- Reserve protects battery from over-discharge during normal operation
- Reserve is **NOT available** during power outages
- Essentially functions as a "prevent deep discharge" rather than "UPS reserve"

#### Option 3: Time-Based Control (Future Enhancement)
Potential solution requiring firmware modification:
- Automatically disable D14 flag when grid is unavailable
- Requires detecting island mode (may need additional hardware input)
- Not currently implemented

### Technical Details

**Register**: 0x3127 (BMS Status), Bit D14 (0x4000)
**Modbus Logic**: 1 = Stop Discharge, 0 = Allow Discharge
**Affected Modes**: Both grid-tied and island mode

**Related Code** (lines 830-864):
```cpp
// D14 (Stop Discharge) - layered priority
bool bms_blocks = !id(can_discharge_enabled);  // BMS blocks discharge
bool soc_blocks = id(soc_control_enabled) && id(soc_discharge_blocked);
d14_stop_discharge = bms_blocks || soc_blocks;  // Either can block
```

When `d14_stop_discharge = true`, register 0x3127 includes 0x4000 flag, and inverter interprets this as "do not discharge under any circumstances."

### Inverter Behavior Documentation

**EPever UP5000 (Protocol 10 / BMS-Link)**:
- D14 (Stop Discharge) is treated as **absolute prohibition**
- No distinction between grid-tied and island mode
- Safety-first design: assumes BMS has critical reason for blocking discharge
- No documented override mechanism

### Testing Performed

**Test Date**: 2026-01-15
**Procedure**:
1. Enabled SOC Reserve Control (50%/55% thresholds)
2. Allowed battery to discharge below 50%
3. Confirmed D14 flag set (0x4000 in register 0x3127)
4. Disconnected grid to simulate power outage
5. Observed inverter behavior

**Result**: Inverter refused to discharge battery. Loads remained unpowered despite 50% SOC available.

**Conclusion**: D14 flag blocks discharge in ALL operating modes, not just grid-tied mode.

### Recommendations

**For Users**:
1. **Disable SOC Reserve Control** until a solution is found
2. Use inverter's built-in voltage-based cutoff settings
3. Set conservative discharge voltage (e.g., 48V = ~50% SOC)
4. Monitor battery SOC manually if you want a reserve buffer

**For Developers**:
1. Investigate island mode detection methods
2. Consider conditional D14 logic: block in grid-tied, allow in island
3. Add user-configurable "Emergency Override" mode
4. Explore alternative inverter protocols that support conditional discharge blocking

### Related Documentation

- **HLD.md**: SOC Reserve Control design (Section: Safety Features)
- **LLD.md**: Register 0x3127 implementation (lines 830-864)
- **SOC_HYSTERESIS_FEASIBILITY.md**: Original feasibility analysis (did not account for island mode behavior)
- **SESSION_2026-01-13.md**: Feature implementation history

### Future Work

**Possible Solutions** (in order of complexity):

1. **Document limitation and recommend voltage-based cutoff** (current approach)
2. **Add GPIO input for grid detection**
   - Connect to inverter's "grid available" signal
   - Automatically clear D14 when grid is down
   - Requires hardware modification
3. **Time-based hysteresis**
   - Disable D14 block during typical power outage hours
   - Risky: assumes power outages don't occur during day
4. **Alternative protocol investigation**
   - Research other BMS-Link protocol variants
   - Look for conditional discharge flags
   - May require different inverter firmware

### Status

**Current Status**: ⚠️ **KNOWN LIMITATION - WORKAROUND REQUIRED**

**Next Steps**: User testing with voltage-based cutoff settings to validate workaround approach.

**Last Updated**: 2026-01-15

**Resolution**: ✅ **FIXED** - See SUCCESS_SUMMARY.md. Implemented inverter priority mode switching instead of D14 flag on 2026-01-16.

---

## Modbus Gateway Returns Wrong Register Data

**Date Discovered**: 2026-01-17
**Severity**: MEDIUM - Causes data corruption and infinite loops, but now mitigated
**Status**: ✅ **MITIGATED** - Validation added to prevent corruption

### Issue Description

The Modbus RTU over TCP gateway (10.10.0.117:9999) occasionally returns data from the **wrong register** when reading register 0x9608 (Inverter Output Priority Mode).

**Normal Response**:
```
0A 03 02 00 01 DC 45
^  ^  ^  ^^^^^
|  |  |  Value = 1 (Utility Priority)
|  |  Byte count = 2 (correct)
|  Function 0x03 (Read Holding)
Slave 10
```

**Abnormal Responses Observed**:

1. **Battery voltage instead of mode**:
```
Current value: 5460 (0x1554) = 54.60V battery voltage
Expected: 0 or 1 (inverter mode)
```

2. **Wrong byte count**:
```
0A 04 04 00 00 00 00
   ^  ^  ^^^^^^^^^^^
   |  |  4 bytes of zeros
   |  Byte count = 4 (should be 2)
   Function 0x04 (should be 0x03)
```

### Impact

**Before Mitigation**:
- Invalid data stored in `inverter_priority_mode` variable
- Infinite "mode mismatch" detection loops
- Confusing log messages showing voltage values as mode
- System unable to properly track inverter state

**After Mitigation**:
- Invalid data rejected with clear error messages
- Variable protected from corruption
- System continues with last known good value
- Clear diagnostic logging for troubleshooting

### Root Cause

**Hypothesis**: The Modbus RTU over TCP gateway has a **cache or routing bug** where it sometimes returns:
1. Data from a different register (likely battery voltage or current)
2. Wrong function code (0x04 instead of 0x03)
3. Wrong byte count (4 bytes instead of 2)

**Possible triggers**:
- Concurrent access from multiple clients (Home Assistant + ESP32)
- Gateway caching responses from previous queries
- Gateway firmware bug in register address routing
- Single-client limitation causing data mixing

### Mitigation Applied

**Commit 8a80f29** (2026-01-17 20:24):
- Added mode value validation (only 0 or 1 accepted)
- Reject invalid values before storing
- Log detailed error with decimal and hex values

**Commit ce5fb3d** (2026-01-17 23:00):
- Added byte count validation (must be exactly 2 bytes)
- Reject malformed responses before processing
- Log gateway errors with diagnostic messages

**Code Changes** (lines 2318-2338):
```cpp
// Validate byte count
int byte_count = response[2];
if (byte_count != 2) {
  ESP_LOGE("inverter_priority", "✗ Wrong byte count in response: %d (expected 2 for 1 register)",
           byte_count);
  // Reject and return early
}

// Validate mode value
int current_mode = (response[3] << 8) | response[4];
if (current_mode != 0 && current_mode != 1) {
  ESP_LOGE("inverter_priority", "✗ Invalid mode value from register 0x9608: %d (0x%04X) - expected 0 or 1",
           current_mode, current_mode);
  // Reject and return early
}
```

### Error Messages

When malformed data is detected, the log will show:

**Wrong byte count**:
```
✗ Wrong byte count: 4 (expected 2) - gateway error?
Gateway may be returning wrong register data
```

**Invalid mode value**:
```
✗ Invalid mode: 5460 (0x1554) - wrong register?
This may indicate wrong register being read or Modbus gateway error
```

### Testing Results

**After fixes applied**:
- 3/3 manual refresh tests successful
- All validation checks passing
- No corrupted data stored
- No infinite mismatch loops

**Example successful operation**:
```
[000072] TX READ: 0A 03 96 08 00 01 29 3B
[000072] RX READ: 0A 03 02 00 01 DC 45
[000072] ✓ Mode correct: Utility Priority (1)
```

### Recommendations

**For Users**:
1. Monitor the Modbus interaction log for validation errors
2. If errors occur frequently, investigate gateway configuration
3. Consider reducing polling frequency if gateway is overloaded
4. Check for concurrent Modbus clients accessing the gateway

**For Troubleshooting**:
1. Use `./modbus_log_tail.sh -f` to monitor for errors
2. Look for patterns in when errors occur (time of day, SOC values, etc.)
3. Test gateway directly with `./modbus_rtu_tcp.py 0x9608 -v`
4. Check Home Assistant Modbus integration load

### Related Documentation

- **SESSION_2026-01-17.md**: Bug discovery and fix details
- **LLD.md**: Modbus interaction log implementation
- **SUCCESS_SUMMARY.md**: Inverter priority control implementation

### Future Work

**Possible improvements**:
1. Add retry logic for malformed responses
2. Implement exponential backoff on repeated errors
3. Add statistics tracking for gateway reliability
4. Consider alternative Modbus gateway hardware/firmware
5. Investigate Home Assistant Modbus integration scheduling

### Status

**Current Status**: ✅ **MITIGATED**

The system now validates all Modbus responses and rejects invalid data before it can corrupt system state. The underlying gateway issue remains unresolved, but the system is protected from its effects.

**Last Updated**: 2026-01-17 23:05
