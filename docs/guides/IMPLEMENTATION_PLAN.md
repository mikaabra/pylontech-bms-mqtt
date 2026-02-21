# Implementation Plan: Code Review Recommendations

**Project:** rack-solar-bridge.yaml Production Quality Improvements  
**Date:** 2026-02-14  
**Status:** Planning Phase  

---

## Overview

This plan implements the HIGH and MEDIUM priority recommendations from the code review to bring `rack-solar-bridge.yaml` to full production quality, matching the stability and features of the reference implementation (`deye-bms-can.yaml`).

---

## Current State

✅ **COMPLETED:**
- MQTT reconnect re-publishing (timestamp reset)
- Config compiles and validates
- Comprehensive code review documented

⚠️ **TO IMPLEMENT:**
- Runtime debug toggle (HIGH priority)
- Availability transition tracking (MEDIUM priority)
- safe_snprintf helper (MEDIUM priority)

---

## Phase 1: Runtime Debug Toggle (HIGH Priority)

**Objective:** Add ability to toggle between WARN and DEBUG log levels at runtime without recompiling.

**Reference Implementation:** `deye-bms-can.yaml` lines 33-37, 97-99, 985-988

### 1.1 Files to Modify
- `rack-solar-bridge.yaml`
- `includes/solar_helpers.h` (minor - ensure compatibility)

### 1.2 Implementation Steps

#### Step 1: Add logger ID and update level
```yaml
logger:
  level: WARN  # Start with minimal logging
  id: logger_level_global
```

#### Step 2: Add global for log level tracking
```yaml
globals:
  - id: logger_level
    type: std::string
    initial_value: '"WARN"'  # Default to minimal logging
```

#### Step 3: Add on_boot initialization
```yaml
esphome:
  on_boot:
    priority: -100
    then:
      - lambda: |-
          // Ensure logger starts in WARN mode (minimal logging)
          id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_WARN);
          id(logger_level) = "WARN";
```

#### Step 4: Add text sensor for debug level
```yaml
text_sensor:
  - platform: template
    name: "Debug Level"
    id: debug_level_sensor
    icon: "mdi:console"
    update_interval: never
    lambda: return id(logger_level);
```

#### Step 5: Add button to toggle log level
```yaml
button:
  - platform: template
    name: "Toggle Debug Level"
    id: toggle_debug_button
    icon: "mdi:toggle-switch"
    on_press:
      - lambda: |-
          if (id(logger_level) == "WARN") {
            id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_DEBUG);
            id(logger_level) = "DEBUG";
            ESP_LOGI("main", "Debug level changed to DEBUG");
          } else {
            id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_WARN);
            id(logger_level) = "WARN";
            ESP_LOGI("main", "Debug level changed to WARN");
          }
          id(debug_level_sensor).publish_state(id(logger_level));
```

### 1.3 Verification
- [ ] Config compiles: `esphome config rack-solar-bridge.yaml`
- [ ] Debug level sensor visible in HA
- [ ] Toggle button changes log level
- [ ] Log output changes between WARN (minimal) and DEBUG (verbose)

---

## Phase 2: Availability Transition Tracking (MEDIUM Priority)

**Objective:** Track SmartShunt and EPEVER connection states and publish availability only on transitions, avoiding spam on every MQTT reconnect.

**Reference Implementation:** `deye-bms-can.yaml` lines 131-143, 936-951

### 2.1 Files to Modify
- `rack-solar-bridge.yaml`

### 2.2 Implementation Steps

#### Step 1: Add availability topics to mqtt section
```yaml
mqtt:
  birth_message:
    topic: ${mqtt_prefix}/status
    payload: "online"
    retain: true
  will_message:
    topic: ${mqtt_prefix}/status
    payload: "offline"
    retain: true
  
  # Additional availability topics for subsystems
  on_connect:
    - lambda: |-
        // Publish availability only on transitions
        if (!id(last_smartshunt_online)) {
          id(mqtt_client).publish(std::string("${mqtt_prefix}/smartshunt/status"), std::string("online"), (uint8_t)0, true);
          id(last_smartshunt_online) = true;
          ESP_LOGI("mqtt", "SmartShunt availability: online");
        }
        if (!id(last_epever_online)) {
          id(mqtt_client).publish(std::string("${mqtt_prefix}/epever/status"), std::string("online"), (uint8_t)0, true);
          id(last_epever_online) = true;
          ESP_LOGI("mqtt", "EPEVER availability: online");
        }
```

#### Step 2: Add globals for availability tracking
```yaml
globals:
  - id: last_smartshunt_online
    type: bool
    initial_value: 'false'
  - id: last_epever_online
    type: bool
    initial_value: 'false'
  - id: last_availability_heartbeat
    type: uint32_t
    initial_value: '0'
```

#### Step 3: Add interval for availability heartbeat (optional, 10 minutes)
```yaml
interval:
  - interval: 10min
    then:
      - lambda: |-
          // Heartbeat to ensure availability is retained
          uint32_t now = millis();
          if (now - id(last_availability_heartbeat) >= 600000) {
            if (id(last_smartshunt_online)) {
              id(mqtt_client).publish(std::string("${mqtt_prefix}/smartshunt/status"), std::string("online"), (uint8_t)0, true);
            }
            if (id(last_epever_online)) {
              id(mqtt_client).publish(std::string("${mqtt_prefix}/epever/status"), std::string("online"), (uint8_t)0, true);
            }
            id(last_availability_heartbeat) = now;
          }
```

#### Step 4: Update discovery configs to include availability topics
Update the `publish_discovery` script to add availability topics for all sensors:
```cpp
// Add availability config for SmartShunt sensors
snprintf(avail_payload, sizeof(avail_payload),
  R"("availability_topic":"${mqtt_prefix}/smartshunt/status","payload_available":"online","payload_not_available":"offline")");
```

### 2.3 Verification
- [ ] Config compiles
- [ ] Availability topics published on first connect only
- [ ] Reconnect doesn't spam availability messages
- [ ] 10-minute heartbeat works (check logs)

---

## Phase 3: safe_snprintf Helper (MEDIUM Priority)

**Objective:** Add buffer overflow protection to discovery payload construction.

**Reference Implementation:** `deye-bms-can.yaml` includes/set_include.h

### 3.1 Files to Modify
- `includes/solar_helpers.h`
- `rack-solar-bridge.yaml` (discovery script)

### 3.2 Implementation Steps

#### Step 1: Add safe_snprintf to solar_helpers.h
```cpp
// Add at top of file
#include <cstdarg>

// Add before #pragma once or after existing includes
inline bool safe_snprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vsnprintf(buf, size, fmt, args);
  va_end(args);
  if (ret < 0 || (size_t)ret >= size) {
    ESP_LOGW("safe_snprintf", "Truncation detected! Needed %d bytes, have %zu", ret, size);
    return false;
  }
  return true;
}
```

#### Step 2: Update discovery script to use safe_snprintf
Replace direct `snprintf` calls with `safe_snprintf` and add error checking:
```cpp
// Before:
snprintf(payload, sizeof(payload), ...);
id(mqtt_client).publish(...);

// After:
if (safe_snprintf(payload, sizeof(payload), ...)) {
  id(mqtt_client).publish(...);
  pace_publish();
} else {
  ESP_LOGW("discovery", "Failed to build payload for %s", sensor_name);
}
```

### 3.3 Verification
- [ ] Config compiles
- [ ] Discovery publishes successfully
- [ ] Truncation warning appears in logs if buffer too small (can test by temporarily reducing buffer size)

---

## Phase 4: Integration and Testing

### 4.1 Integration Order
1. **safe_snprintf** (Phase 3) - Low risk, foundational
2. **Runtime debug toggle** (Phase 1) - Independent, HIGH priority
3. **Availability tracking** (Phase 2) - Changes MQTT behavior, needs testing

### 4.2 Test Plan

#### Test 1: Config Validation
```bash
source ~/src/esphome-venv/bin/activate
esphome config rack-solar-bridge.yaml
```
Expected: INFO Configuration is valid!

#### Test 2: Compile Check
```bash
esphome compile rack-solar-bridge.yaml
```
Expected: Successful compilation

#### Test 3: Runtime Debug Toggle (after flashing)
1. Check initial log level - should be minimal (WARN)
2. Press "Toggle Debug Level" button
3. Verify log level changes to DEBUG (verbose output)
4. Press button again
5. Verify log level returns to WARN

#### Test 4: Availability Tracking (after flashing)
1. First MQTT connect - verify availability topics published
2. Check HA - SmartShunt and EPEVER sensors should show as available
3. Restart MQTT broker
4. Verify availability topics NOT re-published (check logs)
5. Verify sensors remain available in HA

#### Test 5: safe_snprintf Protection
1. Temporarily reduce payload buffer to 100 bytes
2. Compile and flash
3. Check logs for truncation warnings
4. Restore buffer size

---

## Dependencies and Risks

### Dependencies
- Phase 1 (debug toggle): None, independent
- Phase 2 (availability): None, independent
- Phase 3 (safe_snprintf): None, independent

**All phases can be implemented in parallel.**

### Risk Assessment
| Phase | Risk | Mitigation |
|-------|------|------------|
| Debug toggle | Low | Follows reference pattern exactly |
| Availability | Medium | Test reconnect behavior carefully |
| safe_snprintf | Low | Non-functional change, adds safety |

### Rollback Strategy
Each phase is independent. If issues occur:
1. Revert specific commit for that phase
2. Other phases remain functional

---

## Success Criteria

All phases complete when:
- [ ] Config compiles without warnings
- [ ] Runtime debug toggle works in HA
- [ ] Availability published only on transitions
- [ ] safe_snprintf protects against buffer overflow
- [ ] No regressions in existing functionality
- [ ] Production quality matches deye-bms-can.yaml reference

---

## Timeline Estimate

| Phase | Effort | Parallelizable |
|-------|--------|----------------|
| Debug toggle | 30 min | Yes |
| Availability tracking | 45 min | Yes |
| safe_snprintf | 30 min | Yes |
| Testing | 30 min | No (after all phases) |
| **Total** | **~2 hours** | **~1 hour parallel** |

---

## Next Steps

1. **Choose implementation order** (recommend: safe_snprintf → debug toggle → availability)
2. **Create feature branches** for each phase
3. **Implement and test each phase independently**
4. **Merge to main when all phases complete**

---

*Plan created with Ultrawork Mode precision protocols*
