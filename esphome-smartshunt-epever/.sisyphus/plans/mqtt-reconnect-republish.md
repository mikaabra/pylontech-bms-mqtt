# Work Plan: Add MQTT Reconnect Re-publishing

## Objective
Add logic to re-publish all sensor values when MQTT reconnects (e.g., after broker restart), preventing "Unknown" states in Home Assistant.

## Context
- Current `on_connect` only publishes HA Discovery configs
- If broker restarts, HA shows "Unknown" until sensors naturally update
- Need to force immediate re-publishing of all cached values on reconnect

## Implementation Strategy

### Option A: Reset Timestamp Globals (Recommended)
Reset all `last_*_publish` timestamp globals to 0 in `on_connect`. This causes the threshold helper functions to immediately publish on next value update (within seconds).

**Pros:**
- Simple, minimal code
- Uses existing hysteresis logic
- No duplication of publish logic

**Cons:**
- Slight delay (seconds) until next sensor update
- Requires listing all 27 timestamp globals

### Option B: Direct Publish Script
Create a script that directly reads current sensor values and publishes them.

**Pros:**
- Immediate publish on reconnect

**Cons:**
- Duplicates publish logic
- More complex
- Must handle uninitialized values carefully

## Selected Approach: Option A

## TODOs

### 1. Add timestamp reset lambda to on_connect
**Location:** `rack-solar-bridge.yaml`, mqtt: on_connect section

**Change:**
Add lambda that resets all 27 `last_*_publish` timestamp globals to 0:
- 19 SmartShunt sensor timestamps
- 8 EPEVER sensor timestamps

**Code to insert after discovery check:**
```yaml
- lambda: |-
    // Force re-publish all sensor values on reconnect
    id(last_ss_battery_voltage_publish) = 0;
    id(last_ss_battery_current_publish) = 0;
    id(last_ss_state_of_charge_publish) = 0;
    id(last_ss_instantaneous_power_publish) = 0;
    // ... (all 27 timestamps)
    ESP_LOGI("mqtt", "Reset publish timestamps for re-publish on reconnect");
```

### 2. Validate config compiles
**Command:** `esphome config rack-solar-bridge.yaml`

### 3. Document the behavior
Add comment explaining why timestamps are reset.

## Success Criteria
- [ ] Config compiles without errors
- [ ] All 27 timestamp globals are reset in on_connect
- [ ] Log message confirms reset occurred
- [ ] After MQTT reconnect, sensor values re-publish within seconds

## Estimated Effort
Small - 15 minutes

## Parallel Execution
No - single sequential task

## Risk Assessment
Low - simple timestamp reset, no logic changes

## Files to Modify
- `rack-solar-bridge.yaml` (mqtt: on_connect section)

## Test Strategy
1. Compile config
2. Flash to ESP32
3. Monitor logs
4. Restart MQTT broker
5. Verify "Reset publish timestamps" log appears
6. Verify sensor values re-publish within 10 seconds
