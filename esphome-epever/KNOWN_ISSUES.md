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
