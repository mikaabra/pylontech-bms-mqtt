# MQTT Traffic Optimization Analysis

## Current Situation

Looking at `/tmp/top-deyebms.txt`, there's excessive MQTT traffic from topics that should be relatively static:

```
2     deye_bms/can/module_count                               11         0.8% 0.12        
3     deye_bms/can/status_byte7                               11         0.8% 0.12        
4     deye_bms/can/alarm_summary                              11         0.8% 0.12        
5     deye-bms-can/sensor/can_frame_count/state               10         0.7% 0.11        
6     deye-bms-can/sensor/can_error_count/state               10         0.7% 0.11        
```

## Current Hysteresis Settings Found

### 1. Cell Voltage Hysteresis
- **Current**: 0.002V (2mV)
- **Location**: Line 1940 in `deye-bms-can.yaml`
- **Issue**: Too sensitive - cell voltages fluctuate naturally

### 2. Temperature Hysteresis  
- **Current**: 0.5°C for min/max temps
- **Location**: Line 1952
- **Status**: Reasonable for temperatures

### 3. Cell Voltage Delta
- **Current**: 0.002V (2mV)
- **Location**: Line 1940
- **Issue**: Same as above

## Proposed Optimizations

### 1. Increase Cell Voltage Hysteresis
**Recommendation**: Increase from 2mV to 10mV

**Rationale**:
- Cell voltages naturally fluctuate due to load changes
- 2mV changes are often noise, not meaningful data
- 10mV provides better stability while still catching real changes
- Reduces unnecessary MQTT traffic significantly

**Impact**: ~80-90% reduction in cell voltage updates

### 2. Move Static Values to 5-Minute Updates

**Values to move from 30s/10s to 300s (5min) updates:**

#### CAN Values (Currently updating every 10s or 30s)
- `deye_bms/can/module_count` - Only changes when modules are added/removed
- `deye_bms/can/status_byte7` - Rarely changes (Shoto-specific diagnostic)
- `deye_bms/can/alarm_summary` - Only changes when alarms occur
- `deye_bms/can/flags` - Rarely changes

#### RS485 Values (Currently updating every 30s)
- `deye_bms/rs485/battery*/state` - Battery state changes infrequently
- `deye_bms/rs485/battery*/warnings` - Only changes when warnings occur
- `deye_bms/rs485/battery*/alarms` - Only changes when alarms occur
- `deye_bms/rs485/battery*/balancing_cells` - Only changes when balancing starts/stops
- `deye_bms/rs485/battery*/overvolt_cells` - Only changes when overvoltage occurs

### 3. Keep Current 5-Minute Update Values

These are already on 5-minute updates and should stay:
- `deye_bms/rs485/stack/cell_min`
- `deye_bms/rs485/stack/cell_max` 
- `deye_bms/rs485/stack/cell_delta`
- `deye_bms/rs485/stack/voltage`
- `deye_bms/rs485/stack/current`

### 4. Keep Frequent Updates for Critical Values

These should continue with current update frequency:
- `deye_bms/can/status` - Availability monitoring (keep 30s)
- `deye_bms/rs485/status` - Availability monitoring (keep 30s)
- `deye_bms/cell_voltages` - Individual cell voltages (with increased hysteresis)
- `deye_bms/temperatures` - Individual temperatures (keep 0.5°C hysteresis)
- `deye_bms/stack/voltage` - Stack voltage (keep frequent)
- `deye_bms/stack/current` - Stack current (keep frequent)

## Implementation Plan

### Step 1: Increase Cell Voltage Hysteresis
```yaml
# Change line 1940 from:
delta: 0.002

# To:
delta: 0.010  # 10mV instead of 2mV
```

### Step 2: Add Hysteresis to CAN Status Values
```yaml
# For CAN sensors that currently update every 10s:
- platform: template
  id: sensor_can_module_count
  update_interval: never  # Changed from 10s
  # Add on-change logic in CAN processing lambda

# Similar for status_byte7 and alarm_summary
```

### Step 3: Add Hysteresis to RS485 Status Values
```yaml
# For RS485 sensors that currently update every 30s:
- platform: template
  id: sensor_batt_state
  update_interval: never  # Changed from 30s
  # Add on-change logic in RS485 processing lambda

# Similar for warnings, alarms, balancing_cells, etc.
```

### Step 4: Implement On-Change Logic
Add tracking variables and publish-only-on-change logic similar to what we did for CAN sensors:

```cpp
// Add to globals section
- id: last_batt_states
  type: std::vector<std::string>
  initial_value: '{}'

// Add to RS485 publishing logic
if (id(batt_state)[batt] != id(last_batt_states)[batt]) {
    id(sensor_batt_state).publish_state(id(batt_state)[batt]);
    id(last_batt_states)[batt] = id(batt_state)[batt];
}
```

## Expected Traffic Reduction

| Category | Current Updates/Hour | Expected Updates/Hour | Reduction |
|----------|---------------------|----------------------|-----------|
| Cell voltages | ~12,000 | ~1,200 | 90% |
| CAN status values | ~360 | ~10-50 | 86-97% |
| RS485 status values | ~720 | ~20-100 | 86-97% |
| **Total** | **~13,080** | **~1,370** | **89%** |

## Benefits

✅ **Reduced MQTT broker load** - Less processing, memory, and network usage
✅ **Lower power consumption** - ESP32 does less work
✅ **Better Home Assistant performance** - Fewer state updates to process
✅ **Easier monitoring** - Less noise in logs and dashboards
✅ **Same functionality** - All important changes still captured
✅ **Better battery life** - For battery-powered MQTT clients

## Risk Assessment

**Low Risk**: These changes only affect update frequency, not data accuracy. All real changes will still be captured, just with less noise from minor fluctuations.

**Testing**: Should test with real hardware to ensure no important changes are missed, but 10mV hysteresis is still very sensitive for cell voltage monitoring.

## Recommendation

Implement these changes in stages:

1. **First**: Increase cell voltage hysteresis to 10mV (easiest, lowest risk)
2. **Second**: Add on-change logic to CAN status values
3. **Third**: Add on-change logic to RS485 status values
4. **Monitor**: Check MQTT traffic before and after each stage
5. **Adjust**: Fine-tune hysteresis values if needed

This will systematically reduce unnecessary traffic while maintaining all critical functionality.
