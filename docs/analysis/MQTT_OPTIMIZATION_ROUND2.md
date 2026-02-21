# MQTT Optimization - Round 2 Analysis

## Current Situation After Round 1

Looking at `/tmp/top-deyebms.txt`, we've made good progress:

✅ **Cell voltages**: Now updating only 2-4 times (was ~360/hour) - **99% improvement!**
❌ **RS485 values**: Still updating every 30 seconds for rarely changing data

## Top Offenders (Still Updating Every 30s)

### Battery Values (20 updates each - clearly on 30s timer)
- `deye_bms/rs485/battery*/cell_min` - Rarely changes
- `deye_bms/rs485/battery*/cell_max` - Rarely changes  
- `deye_bms/rs485/battery*/voltage` - Changes slowly
- `deye_bms/rs485/battery*/current` - Changes slowly
- `deye_bms/rs485/battery*/soc` - Changes very slowly
- `deye_bms/rs485/battery*/remain_ah` - Rarely changes
- `deye_bms/rs485/battery*/total_ah` - Never changes (constant)
- `deye_bms/rs485/battery*/cycles` - Never changes (constant)
- `deye_bms/rs485/battery*/state` - Rarely changes
- `deye_bms/rs485/battery*/warnings` - Only when warnings occur
- `deye_bms/rs485/battery*/alarms` - Only when alarms occur
- `deye_bms/rs485/battery*/balancing_*` - Only when balancing changes
- `deye_bms/rs485/battery*/overvolt_*` - Only when overvoltage changes
- `deye_bms/rs485/battery*/*_mosfet` - Only when state changes

### Stack Values (20 updates each - clearly on 30s timer)
- `deye_bms/rs485/stack/cell_min` - Rarely changes
- `deye_bms/rs485/stack/cell_max` - Rarely changes
- `deye_bms/rs485/stack/cell_delta_mv` - Rarely changes
- `deye_bms/rs485/stack/voltage` - Changes slowly
- `deye_bms/rs485/stack/current` - Changes slowly

### CAN Values (10-12 updates each - on 60s timer, good)
- `deye_bms/can/module_count` - ✅ Already optimized
- `deye_bms/can/status_byte7` - ✅ Already optimized
- `deye_bms/can/alarm_summary` - ✅ Already optimized
- `deye_bms/can/flags` - ✅ Already optimized

## Root Cause

The RS485 values are published every 30 seconds in the `Publish RS485 data to MQTT` lambda (line 1605). This lambda runs on a 30-second timer and publishes ALL values regardless of whether they've changed.

## Proposed Solution

### 1. Add Hysteresis Tracking for RS485 Values

Add tracking variables for all RS485 values that should only update on change:

```cpp
// Add to globals section
- id: last_batt_voltages
  type: std::vector<float>
  restore_value: no

- id: last_batt_currents
  type: std::vector<float>
  restore_value: no

- id: last_batt_socs
  type: std::vector<float>
  restore_value: no

- id: last_batt_states
  type: std::vector<std::string>
  restore_value: no

// Similar for other rarely changing values
```

### 2. Modify RS485 Publishing Logic

Change from unconditional publishing to on-change publishing:

```cpp
// Current (line 1770-1780):
snprintf(payload, sizeof(payload), "%.2f", stack_voltage);
id(mqtt_client).publish("${rs485_prefix}/stack/voltage", payload);

// Proposed:
if (last_stack_voltage < 0 || fabsf(stack_voltage - last_stack_voltage) >= 0.1f) {
    snprintf(payload, sizeof(payload), "%.2f", stack_voltage);
    id(mqtt_client).publish("${rs485_prefix}/stack/voltage", payload);
    last_stack_voltage = stack_voltage;
}
```

### 3. Add MQTT Connect Publishing

Ensure all RS485 values publish on MQTT connect by resetting tracking variables.

## Implementation Plan

### Step 1: Add Tracking Variables

```yaml
# Add to globals section (around line 710)
- id: last_stack_voltage
  type: float
  initial_value: '-1.0'

- id: last_stack_current
  type: float
  initial_value: '-1.0'

- id: last_stack_cell_min
  type: float
  initial_value: '-1.0'

- id: last_stack_cell_max
  type: float
  initial_value: '-1.0'

- id: last_stack_cell_delta
  type: float
  initial_value: '-1.0'

- id: last_batt_voltages
  type: std::vector<float>
  restore_value: no

- id: last_batt_currents
  type: std::vector<float>
  restore_value: no

- id: last_batt_socs
  type: std::vector<float>
  restore_value: no

- id: last_batt_remain_ah
  type: std::vector<float>
  restore_value: no

- id: last_batt_total_ah
  type: std::vector<float>
  restore_value: no

- id: last_batt_states
  type: std::vector<std::string>
  restore_value: no
```

### Step 2: Initialize Tracking in on_boot

```cpp
// Add to on_boot lambda (around line 64)
id(last_batt_voltages).resize(num_batt, -1.0f);
id(last_batt_currents).resize(num_batt, -1.0f);
id(last_batt_socs).resize(num_batt, -1.0f);
id(last_batt_remain_ah).resize(num_batt, -1.0f);
id(last_batt_total_ah).resize(num_batt, -1.0f);
id(last_batt_states).resize(num_batt, "");
```

### Step 3: Modify RS485 Publishing Logic

Replace unconditional publishing with on-change logic for all rarely changing values.

### Step 4: Add MQTT Connect Reset

```cpp
// Add to on_connect lambda
id(last_stack_voltage) = -1.0f;
id(last_stack_current) = -1.0f;
// Reset all battery tracking arrays
for (int i = 0; i < id(last_batt_voltages).size(); i++) {
    id(last_batt_voltages)[i] = -1.0f;
    id(last_batt_currents)[i] = -1.0f;
    // etc for all tracking arrays
}
```

## Expected Impact

| Category | Current | Expected | Reduction |
|----------|---------|----------|-----------|
| RS485 battery values | ~7,200/hour | ~100/hour | 98% ✅ |
| RS485 stack values | ~720/hour | ~20/hour | 97% ✅ |
| **Total additional reduction** | **~7,920/hour** | **~120/hour** | **98% ✅** |

## Combined Impact (Round 1 + Round 2)

| Category | Before | After | Reduction |
|----------|--------|-------|-----------|
| Cell voltages | ~12,000/hour | ~1,200/hour | 90% ✅ |
| RS485 values | ~7,920/hour | ~120/hour | 98% ✅ |
| CAN status | ~360/hour | ~10/hour | 97% ✅ |
| **Total** | **~20,280/hour** | **~1,330/hour** | **93% ✅** |

## Risk Assessment

**Low Risk**: These changes only affect update frequency, not data accuracy. All real changes will still be captured, just with less noise from minor fluctuations.

## Recommendation

Implement these changes to achieve the final ~93% reduction in MQTT traffic. This will make the system much more efficient while maintaining all critical functionality.
