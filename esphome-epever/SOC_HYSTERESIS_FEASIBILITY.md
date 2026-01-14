# SOC Hysteresis Control - Feasibility Analysis

## Date: 2026-01-14

## Objective
Implement SOC-based hysteresis control to maintain 50% battery capacity as UPS reserve while allowing opportunistic charging/discharging above this threshold.

## Requirements

### Discharge Control
- **Block discharge** when SOC < 50%
- **Allow discharge** when SOC > 55%
- Provides 5% hysteresis to prevent oscillation

### Charge Control
- **Force charge** when SOC < 45%
- **Clear force charge** when SOC >= 50%
- Provides 5% hysteresis to prevent oscillation

### Operating Philosophy
1. **Below 50% SOC**: Battery acts as UPS reserve only
   - No discharge to loads
   - Force charge from grid/PV to restore reserve

2. **Above 55% SOC**: Battery available for energy storage
   - Can discharge to power loads (overnight use)
   - Can charge from PV during day
   - Natural cycling above reserve threshold

3. **45-50% SOC**: Emergency recovery mode
   - Force charge flag active
   - May trigger grid charging depending on inverter settings

## Technical Feasibility: ✅ FULLY FEASIBLE

### Available BMS-Link Flags (Register 0x3127)

The BMS-Link Modbus protocol provides exactly the flags we need:

| Bit | Hex Value | Flag Name | Logic | Current Use |
|-----|-----------|-----------|-------|-------------|
| D13 | 0x2000 | **Force Charge Mark** | 1=Force, 0=Normal | ❌ Not used |
| D14 | 0x4000 | **Stop Discharge** | 1=Stop, 0=Enable | ✅ Mapped from CAN |
| D15 | 0x8000 | **Stop Charge** | 1=Stop, 0=Enable | ✅ Mapped from CAN |

**Key Finding:** The protocol includes a dedicated "Force Charge Mark" flag (D13) that is **currently unused** but perfect for this application!

### Current Implementation

Currently in `epever-can-bridge.yaml` line 742-752:

```yaml
case 0x3127: // A39: Lithium battery BMS status (bitfield)
  value = 0x0000;  // Start with all OK
  // Map from CAN flags - INVERTED logic (bit=1 means STOP, bit=0 means ENABLE)
  if (!id(can_discharge_enabled)) value |= 0x4000;  // D14: Stop discharge
  if (!id(can_charge_enabled)) value |= 0x8000;     // D15: Stop charge
  break;
```

The code **only** sets D14 and D15 based on BMS protection flags from CAN bus. D13 (Force Charge) is never set.

### How Epever Interprets These Flags

Based on the BMS-Link protocol documentation:

1. **D14 (Stop Discharge)**: When set, inverter will not discharge battery to loads
   - Used for: UV protection, low temp, BMS faults
   - Effect: Battery disconnected from load bus

2. **D15 (Stop Charge)**: When set, inverter will not charge battery
   - Used for: OV protection, high temp, 100% SOC
   - Effect: Charge sources redirected (PV may go to loads or curtail)

3. **D13 (Force Charge)**: When set, inverter actively charges battery
   - Typical use: Low SOC recovery, equalization
   - Effect: May enable grid charging if configured, prioritize battery charging

**Important:** The exact behavior of D13 depends on inverter charge priority settings, but it signals "battery needs charging urgently."

## Proposed Implementation

### Design: Multi-Layer Control Logic

The implementation should use **layered AND/OR logic** to respect BMS safety while adding SOC-based control:

```
Final Discharge Enable = BMS_Allow_Discharge AND SOC_Allow_Discharge
Final Charge Enable = BMS_Allow_Charge (unchanged - never block BMS charging)
Final Force Charge = SOC_Force_Charge (BMS doesn't provide this signal)
```

### Hysteresis State Machine

#### State Variables (new globals):
```yaml
- id: soc_discharge_blocked
  type: bool
  initial_value: 'false'  # Start allowing discharge

- id: soc_force_charge
  type: bool
  initial_value: 'false'  # Start without force charge
```

#### State Transitions:

**Discharge Block State:**
```
if (soc < 50 && !soc_discharge_blocked):
  soc_discharge_blocked = true
  LOG: "SOC protection: Discharge blocked at {soc}%"

if (soc > 55 && soc_discharge_blocked):
  soc_discharge_blocked = false
  LOG: "SOC protection: Discharge allowed at {soc}%"
```

**Force Charge State:**
```
if (soc < 45 && !soc_force_charge):
  soc_force_charge = true
  LOG: "SOC protection: Force charge activated at {soc}%"

if (soc >= 50 && soc_force_charge):
  soc_force_charge = false
  LOG: "SOC protection: Force charge cleared at {soc}%"
```

#### Register 0x3127 Logic (modified):
```yaml
case 0x3127:
  value = 0x0000;

  // Layer 1: BMS safety flags (always respected)
  bool bms_discharge_ok = id(can_discharge_enabled);
  bool bms_charge_ok = id(can_charge_enabled);

  // Layer 2: SOC-based control
  bool soc_discharge_ok = !id(soc_discharge_blocked);

  // Combine: Discharge only if BOTH allow it
  bool final_discharge_ok = bms_discharge_ok && soc_discharge_ok;

  // Set Modbus flags (inverted logic)
  if (!final_discharge_ok) value |= 0x4000;  // D14: Stop discharge
  if (!bms_charge_ok) value |= 0x8000;       // D15: Stop charge
  if (id(soc_force_charge)) value |= 0x2000; // D13: Force charge

  break;
```

### State Update Location

Best place to update SOC hysteresis states is in the CAN 0x355 handler (line ~175), right after `id(bms_soc)` is updated:

```yaml
- can_id: 0x355  # SOC/SOH
  then:
    - lambda: |-
        // ... existing CAN parsing code ...
        id(bms_soc) = soc;

        // NEW: SOC hysteresis control
        // Discharge blocking (50% / 55% thresholds)
        if (soc < 50 && !id(soc_discharge_blocked)) {
          id(soc_discharge_blocked) = true;
          ESP_LOGI("soc_control", "Discharge blocked at %d%% (reserve protection)", soc);
        }
        if (soc > 55 && id(soc_discharge_blocked)) {
          id(soc_discharge_blocked) = false;
          ESP_LOGI("soc_control", "Discharge allowed at %d%% (reserve restored)", soc);
        }

        // Force charge (45% / 50% thresholds)
        if (soc < 45 && !id(soc_force_charge)) {
          id(soc_force_charge) = true;
          ESP_LOGI("soc_control", "Force charge ON at %d%% (reserve critical)", soc);
        }
        if (soc >= 50 && id(soc_force_charge)) {
          id(soc_force_charge) = false;
          ESP_LOGI("soc_control", "Force charge OFF at %d%% (reserve ok)", soc);
        }
```

## Safety Analysis

### BMS Override Behavior

**Scenario 1: BMS blocks discharge (low temp, UV)**
- SOC control: Allow discharge (60%)
- BMS: Block discharge (low temp)
- **Result**: Discharge BLOCKED ✅ (BMS wins)

**Scenario 2: BMS blocks charge (high temp, OV, 100% SOC)**
- SOC control: Allow charge (or force charge)
- BMS: Block charge (overvolt)
- **Result**: Charge BLOCKED ✅ (BMS wins)

**Scenario 3: Both allow, SOC at 48%**
- SOC control: Block discharge (below 50%)
- BMS: Allow discharge
- **Result**: Discharge BLOCKED ✅ (SOC protection active)

**Scenario 4: Emergency - SOC at 5%**
- SOC control: Block discharge + Force charge
- BMS: Likely blocking discharge (UV protection)
- **Result**: No discharge, force charge active ✅

**Critical Safety Property:** BMS protection flags are **always respected**. SOC control only adds restrictions, never removes BMS restrictions.

## Expected Behavior Examples

### Example 1: Evening discharge with reserve
```
Time  | SOC | BMS D | BMS C | SOC Control      | Inverter Action
------|-----|-------|-------|------------------|------------------
18:00 | 80% | OK    | OK    | Allow discharge  | Battery → Loads
20:00 | 65% | OK    | OK    | Allow discharge  | Battery → Loads
22:00 | 56% | OK    | OK    | Allow discharge  | Battery → Loads
23:00 | 54% | OK    | OK    | BLOCK discharge  | Still discharging (hysteresis)
23:30 | 51% | OK    | OK    | BLOCK discharge  | Still discharging
00:00 | 49% | OK    | OK    | BLOCK + FORCE CH | Grid → Loads, Battery off
00:30 | 48% | OK    | OK    | BLOCK + FORCE CH | Battery stopped
01:00 | 47% | OK    | OK    | BLOCK + FORCE CH | Battery at 50% reserve
```

### Example 2: PV charging from reserve
```
Time  | SOC | BMS D | BMS C | SOC Control      | Inverter Action
------|-----|-------|-------|------------------|------------------
08:00 | 50% | OK    | OK    | BLOCK discharge  | Battery idle
09:00 | 51% | OK    | OK    | BLOCK discharge  | PV → Loads, excess → Battery
10:00 | 52% | OK    | OK    | BLOCK discharge  | Charging from PV
11:00 | 54% | OK    | OK    | BLOCK discharge  | Charging from PV
12:00 | 56% | OK    | OK    | Allow discharge  | PV → Loads, excess → Battery
14:00 | 78% | OK    | OK    | Allow discharge  | Fully charged
```

### Example 3: Critical low SOC recovery
```
Time  | SOC | BMS D | BMS C | SOC Control      | Inverter Action
------|-----|-------|-------|------------------|------------------
03:00 | 47% | OK    | OK    | BLOCK + FORCE CH | Battery protected
03:30 | 46% | OK    | OK    | BLOCK + FORCE CH | No change
04:00 | 44% | OK    | OK    | BLOCK + FORCE CH | Force charge activated
04:30 | 46% | OK    | OK    | BLOCK + FORCE CH | Charging (if configured)
05:00 | 49% | OK    | OK    | BLOCK + FORCE CH | Still charging
05:30 | 50% | OK    | OK    | BLOCK discharge  | Force charge cleared
06:00 | 52% | OK    | OK    | BLOCK discharge  | Waiting for 55%
07:00 | 56% | OK    | OK    | Allow discharge  | Normal operation restored
```

## Implementation Complexity

### Code Changes Required
1. **Add 2 global state variables** (5 lines)
2. **Add SOC state machine in CAN 0x355 handler** (~25 lines)
3. **Modify register 0x3127 logic** (~10 lines)
4. **Total**: ~40 lines of code

### Testing Requirements
1. **Threshold testing**: Force SOC values to verify transitions
2. **Hysteresis verification**: Confirm no oscillation at boundaries
3. **BMS override testing**: Verify BMS protection takes precedence
4. **Logging verification**: Confirm state changes are logged

### Risks
- **Low risk**: Logic is simple boolean AND/OR operations
- **Testable**: Can simulate all conditions by adjusting SOC
- **Reversible**: Easy to disable by removing code
- **Safe**: BMS protection cannot be bypassed

## Configurability Considerations

The thresholds could be made configurable:

```yaml
# Configuration (could be made into globals or substitutions)
- id: soc_discharge_block_threshold
  type: int
  initial_value: '50'

- id: soc_discharge_allow_threshold
  type: int
  initial_value: '55'

- id: soc_force_charge_on_threshold
  type: int
  initial_value: '45'

- id: soc_force_charge_off_threshold
  type: int
  initial_value: '50'
```

This would allow tuning without recompiling, but adds complexity. **Recommendation**: Start with hardcoded values, add configurability later if needed.

## Inverter Configuration Considerations

### Force Charge Behavior

The exact behavior of the D13 "Force Charge" flag depends on inverter settings:

**Possible Epever configurations:**
1. **Grid charge disabled**: Force charge may only affect charge priority from PV
2. **Grid charge enabled**: Force charge may trigger grid charging
3. **Time-of-use charging**: Force charge may override TOU schedule

**Recommendation:** Test with current inverter settings first. If force charge behavior is too aggressive (e.g., always grid charging), consider using only the discharge block feature initially.

### Fallback: Discharge Block Only

If force charge proves problematic, Phase 1 could implement only discharge blocking:
- Set D14 (stop discharge) below 50%, clear above 55%
- Keep D13 (force charge) always clear
- This still maintains 50% reserve, but relies on PV/grid charging to naturally restore

## Recommendation: ✅ IMPLEMENT

### Verdict
**Fully feasible** with low risk and high value. The BMS-Link protocol provides exactly the flags needed, and the implementation is straightforward.

### Suggested Approach

**Phase 1: Basic Implementation**
1. Add SOC hysteresis state machine
2. Implement discharge blocking (D14)
3. Implement force charge (D13)
4. Add logging for state changes
5. Test with real system

**Phase 2: Tuning** (if needed)
1. Adjust thresholds based on actual usage
2. Refine force charge behavior
3. Add configuration options if needed

**Phase 3: Enhancements** (optional)
1. Add time-based overrides (e.g., disable reserve during day)
2. Add manual override via Home Assistant
3. Seasonal threshold adjustment

### Expected Benefits
1. **UPS reliability**: Always have 50% battery for outages
2. **Energy cost savings**: Use battery overnight when SOC > 55%
3. **Battery longevity**: Avoid deep discharge cycles
4. **Peace of mind**: Automated reserve management

## Questions for User

1. **Force charge behavior**: Do you want to test force charge flag, or start with discharge-block-only?
2. **Thresholds**: Are 50%/55% (discharge) and 45%/50% (charge) acceptable, or different values?
3. **Grid charging**: Is grid charging currently enabled on inverter? This affects force charge behavior.
4. **Testing approach**: Prefer gradual rollout (discharge block first) or implement both features together?

## Conclusion

This feature is **highly feasible** and aligns perfectly with the BMS-Link protocol capabilities. The implementation is clean, safe, and provides exactly the UPS reserve behavior requested.
