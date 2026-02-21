# MQTT Hysteresis Diagnostic Report

**Issue:** Topics publishing every few seconds in MQTT Explorer  
**Date:** 2026-02-14  
**Status:** INVESTIGATING  

---

## Suspected Causes (in order of likelihood)

### 1. SmartShunt Values Fluctuating Naturally
**Likelihood:** HIGH

**Evidence:**
- SmartShunt has `throttle: 1s` - values update every second
- Voltage threshold: 0.1V
- Current threshold: 0.1A
- Power threshold: 1.0W

**Analysis:**
During active battery charging/discharging:
- Battery voltage naturally fluctuates ±0.05-0.2V as charge controller cycles
- Current fluctuates based on MPPT tracking and load changes
- Power = V × I, so both fluctuations compound

**If voltage changes 0.1V every 3-5 seconds during charging = PUBLISH**
This may be NORMAL behavior during active charging.

### 2. Text Sensors Oscillating
**Likelihood:** MEDIUM

**Evidence:**
- 5s interval checks all 7 text sensors
- Globals initialized to "UNINITIALIZED"
- Internal values updated from victron text sensors

**Potential Bug:**
```cpp
// In victron text sensor on_value:
- lambda: "if (!x.empty()) id(ss_model_description_val) = x;"
```

If the text sensor occasionally sends empty strings, the value might oscillate:
- `current_val` = "SmartShunt"
- Next interval: `current_val` = "" (empty)
- `last_val` = "SmartShunt" 
- Comparison: "" != "SmartShunt" → PUBLISH
- Update `last_val` = ""
- Next: value restored to "SmartShunt" → PUBLISH again

### 3. Power Sensor Overflow
**Likelihood:** LOW (but was already fixed once)

**Previous fix:** Commit `4af1a31` - Power range fixed to accept negative values

**Current range:** -10000.0f to 10000.0f (should be fine)

---

## Diagnostic Steps

### Step 1: Identify Which Topics Are Spamming

**Via MQTT Explorer:**
1. Sort topics by "last update" time
2. Note which topics update every few seconds
3. Common culprits:
   - `rack-solar/smartshunt/battery_voltage`
   - `rack-solar/smartshunt/battery_current`
   - `rack-solar/smartshunt/instantaneous_power`
   - `rack-solar/smartshunt/model_description` (text)

### Step 2: Enable Debug Logging Temporarily

Add this to `rack-solar-bridge.yaml` for debugging:
```yaml
logger:
  level: DEBUG
  logs:
    mqtt: DEBUG
    victron: DEBUG
```

Flash and watch logs to see:
- Which sensors trigger `on_value`
- What values are being published

### Step 3: Add Publish Logging (Non-Invasive)

Add ESP_LOGI before each MQTT publish to identify spamming sensors:
```cpp
- lambda: |-
    ESP_LOGI("mqtt", "SS Voltage: %.2fV (last: %.2fV)", x, id(last_ss_battery_voltage));
    if (check_threshold_float(...)) {
      id(mqtt_client).publish(...);
      ESP_LOGI("mqtt", "PUBLISHED voltage");
    }
```

---

## Recommended Fixes (by priority)

### Fix 1: Increase Voltage Threshold (if voltage is the culprit)
```cpp
// Current:
if (check_threshold_float(x, id(last_ss_battery_voltage),
                           id(last_ss_battery_voltage_publish),
                           0.1f, 15.0f, 30.0f)) {

// Change to:
if (check_threshold_float(x, id(last_ss_battery_voltage),
                           id(last_ss_battery_voltage_publish),
                           0.2f, 15.0f, 30.0f)) {  // 0.2V threshold
```

**Impact:** Reduces voltage publishes by ~50% during fluctuation

### Fix 2: Add Text Sensor Debounce
```cpp
// Add timestamp tracking for text sensors
globals:
  - id: last_ss_model_description_publish
    type: uint32_t
    initial_value: '0'

// In interval lambda, add minimum interval:
auto publish_text = [&](const char* topic, 
                        const std::string& current_val, 
                        std::string& last_val,
                        esphome::text_sensor::TextSensor* sensor,
                        uint32_t& last_publish) {
  uint32_t now = millis();
  if (current_val != last_val && (now - last_publish) >= 10000) {  // Min 10s between publishes
    id(mqtt_client).publish(topic, current_val.c_str());
    sensor->publish_state(current_val);
    last_val = current_val;
    last_publish = now;
  }
};
```

### Fix 3: Group Related Publishes
Like deye-bms-can.yaml does - publish voltage/current/power together as a group:
```cpp
// Check all thresholds
bool v_changed = fabs(v - last_v) >= 0.1f;
bool i_changed = fabs(i - last_i) >= 0.1f;
bool p_changed = fabs(p - last_p) >= 1.0f;
bool heartbeat = (now - last_publish) >= 60000;

// Publish all if ANY changed (reduces partial updates)
if (v_changed || i_changed || p_changed || heartbeat) {
  publish voltage;
  publish current;
  publish power;
  update all tracking vars;
  last_publish = now;
}
```

---

## Immediate Action Required

**Before implementing fixes, we need to identify the actual culprit:**

1. **Check MQTT Explorer** - Which specific topics are updating every few seconds?
2. **Check ESPHome logs** (if accessible) - Which sensors are triggering?
3. **Note system state** - Is this during active charging? (Normal) Or idle? (Bug)

**Questions for user:**
1. Which MQTT topics are updating most frequently? (voltage, current, power, text?)
2. Is the battery actively charging/discharging when this happens?
3. What are the typical value changes? (e.g., voltage jumping 0.1-0.2V?)

---

## Reference Comparison

| Aspect | rack-solar-bridge | deye-bms-can | Difference |
|--------|------------------|--------------|------------|
| Voltage threshold | 0.1V | 0.1V | Same |
| Grouped publishes | No | Yes (limits together) | Reference groups related sensors |
| Text sensor interval | 5s | N/A (no text sensors) | We have 7 text sensors |
| Heartbeat | 60s | 60s | Same |
| Throttle | 1s (Victron) | N/A (CAN frames) | Different data sources |

**Key insight:** deye-bms-can.yaml groups voltage/current/power limits together and publishes all if ANY change. This reduces partial updates.

---

## Next Steps

1. **User provides:** List of spamming topics from MQTT Explorer
2. **Identify:** Which sensor type is the culprit (voltage/current/power/text)
3. **Implement:** Appropriate fix from recommendations above
4. **Verify:** Monitor MQTT Explorer for reduced traffic

---

*Diagnostic report generated with Ultrawork Mode*
