# Code Review: deye-bms-can.yaml

**Date**: 2026-01-19
**Reviewer**: Claude Sonnet 4.5
**File**: `/home/micke/GitHub/pylontech-bms-mqtt/esphome/deye-bms-can.yaml`
**Lines**: 1575
**Purpose**: ESPHome firmware for Waveshare ESP32-S3-RS485-CAN board monitoring Pylontech batteries

---

## Executive Summary

This code review identifies opportunities to improve code quality, maintainability, and error resilience in the Deye BMS CAN bridge firmware. The code is functional but has significant duplication and lacks retry logic that was recently added to the epever-can-bridge.yaml. Key findings:

- ✅ **Strengths**: Good checksum validation, clear structure, per-battery state tracking
- ⚠️ **Major Issues**: No retry logic on RS485 failures, extensive code duplication
- 📋 **Recommendations**: 15 actionable improvements ranging from quick wins to architectural enhancements

---

## File Structure

| Section | Lines | Purpose |
|---------|-------|---------|
| Header & Substitutions | 1-20 | Device config, MQTT prefixes |
| External Components | 22-27 | Custom CAN listen-only mode |
| ESPHome Core | 29-308 | Boot initialization, MQTT, HA discovery |
| Hardware Config | 312-340 | WiFi, OTA, web server, UART, CAN |
| CAN Frame Handlers | 341-483 | Five CAN message parsers (0x351, 0x355, 0x359, 0x370, 0x35C) |
| Global Variables | 484-630 | State tracking arrays for 3 batteries |
| Polling Intervals | 632-1340 | Stale detection, RS485 polling (10s, 30s), MQTT publishing |
| Sensor Definitions | 1342-1575 | ESPHome platform sensors |

---

## Critical Issues

### 1. No Retry Logic on RS485 Failures ❌

**Location**: Lines 756-760, 942-946

**Current Code**:
```cpp
std::string error = rs485_validate_response(response, addr);
if (!error.empty()) {
  handle_poll_failure(error.c_str());
  return;  // ← Immediately gives up, no retry
}
```

**Impact**:
- Temporary bus contention causes permanent data loss for entire polling interval
- Single checksum error loses 10 seconds of battery telemetry
- No resilience against transient electrical noise

**Comparison with epever-can-bridge.yaml**:
The epever version (after commit a93472b) implements:
```cpp
const int max_retries = 3;
for (int retry = 0; retry < max_retries && !read_success; retry++) {
  if (retry > 0) {
    append_modbus_log("Retry attempt...");
    delay(200);
  }
  // ... validate response ...
  if (validation_ok) {
    read_success = true;
  }
}
```

**Recommendation**: Implement identical retry logic for RS485 polling.

**Effort**: Low (1-2 hours)
**Priority**: HIGH

---

### 2. Extensive Code Duplication 📋

#### A. Duplicate Response Reading (Lines 722-760, 922-939)

**Duplication**: 20+ lines repeated verbatim in both 10s and 30s polling intervals

```cpp
// This entire block appears twice:
std::string response;
response.reserve(600);
uint32_t start_ms = millis();
bool got_eof = false;
while (millis() - start_ms < 1200) {
  while (id(rs485_uart).available()) {
    uint8_t c;
    id(rs485_uart).read_byte(&c);
    response += (char)c;
    if (c == '\r') { got_eof = true; break; }
  }
  if (got_eof) break;
  delay(5);
}
auto tilde = response.find('~');
if (tilde != std::string::npos) response = response.substr(tilde);
```

**Recommendation**: Extract to helper function in `set_include.h`:

```cpp
std::string rs485_read_response(uint32_t timeout_ms = 1200) {
  std::string response;
  response.reserve(600);
  uint32_t start_ms = millis();
  bool got_eof = false;

  while (millis() - start_ms < timeout_ms) {
    while (id(rs485_uart).available()) {
      uint8_t c;
      id(rs485_uart).read_byte(&c);
      response += (char)c;
      if (c == '\r') { got_eof = true; break; }
    }
    if (got_eof) break;
    delay(5);
  }

  auto tilde = response.find('~');
  if (tilde != std::string::npos) {
    response = response.substr(tilde);
  }

  return response;
}
```

**Effort**: Low (30 minutes)
**Priority**: MEDIUM

#### B. Duplicate Poll Failure Handling (Lines 742-753, 876-900)

**Duplication**: Nearly identical lambda functions defined in both polling intervals

```cpp
// Appears in both 10s and 30s intervals:
auto handle_poll_failure = [&batt](const char* reason) {
  ESP_LOGW("rs485", "Batt %d poll failed: %s", batt, reason);
  id(batt_poll_failures)[batt]++;
  if (id(batt_poll_failures)[batt] >= 10 && !id(batt_poll_alarm)[batt]) {
    id(batt_poll_alarm)[batt] = true;
    char topic[64];
    snprintf(topic, sizeof(topic), "${rs485_prefix}/battery%d/poll_alarm", batt);
    id(mqtt_client).publish(topic, "true");
  }
};
```

**Recommendation**: Extract to global helper function with configurable threshold:

```cpp
void handle_rs485_poll_failure(int battery, const char* reason, int threshold = 10) {
  ESP_LOGW("rs485", "Batt %d poll failed: %s", battery, reason);
  id(batt_poll_failures)[battery]++;

  if (id(batt_poll_failures)[battery] >= threshold && !id(batt_poll_alarm)[battery]) {
    id(batt_poll_alarm)[battery] = true;
    char topic[64];
    snprintf(topic, sizeof(topic), "${rs485_prefix}/battery%d/poll_alarm", battery);
    id(mqtt_client).publish(topic, "true");
  }
}
```

**Effort**: Low (20 minutes)
**Priority**: MEDIUM

#### C. Duplicate Buffer Clearing (Lines 701-705, 903-907)

**Duplication**: Buffer flush code repeated verbatim

```cpp
// Clear RX buffer before transmit
while (id(rs485_uart).available()) {
  uint8_t c;
  id(rs485_uart).read_byte(&c);
}
```

**Recommendation**: Extract to helper function:

```cpp
void rs485_clear_rx_buffer() {
  while (id(rs485_uart).available()) {
    uint8_t c;
    id(rs485_uart).read_byte(&c);
  }
}
```

**Effort**: Trivial (5 minutes)
**Priority**: LOW

#### D. Duplicate HA Discovery Patterns (Lines 117-267)

**Issue**: Extensive use of repetitive snprintf loops for discovery config generation

**Example Pattern**:
```cpp
// Repeated for stack sensors, battery sensors, binary sensors, cells, temps
for (int i = 0; i < count; i++) {
  snprintf(topic, sizeof(topic), "homeassistant/sensor/...");
  snprintf(payload, sizeof(payload), "{\"name\":\"...\",...}");
  id(mqtt_client).publish(topic, payload, 0, true);
}
```

**Recommendation**: Create parameterized discovery helper function:

```cpp
void publish_ha_discovery(
  const char* component,      // "sensor" or "binary_sensor"
  const char* object_id,      // unique id
  const char* name,           // friendly name
  const char* state_topic,    // MQTT topic
  const char* device_class = nullptr,
  const char* unit = nullptr,
  const char* value_template = nullptr
) {
  char topic[128];
  char payload[512];

  snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", component, object_id);

  // Build JSON payload...

  id(mqtt_client).publish(topic, payload, 0, true);
}
```

**Effort**: Medium (2-3 hours)
**Priority**: LOW (improves maintainability)

#### E. Duplicate MQTT Publishing Patterns (Lines 1187-1294)

**Issue**: 100+ repetitions of snprintf/publish sequences

```cpp
// Repeated pattern:
snprintf(topic, sizeof(topic), "${rs485_prefix}/battery%d/metric", batt);
snprintf(payload, sizeof(payload), "%.3f", value);
id(mqtt_client).publish(topic, payload);
```

**Recommendation**: Create templated helper:

```cpp
template<typename T>
void publish_battery_metric(int battery, const char* metric, T value, const char* format) {
  char topic[64];
  char payload[32];
  snprintf(topic, sizeof(topic), "${rs485_prefix}/battery%d/%s", battery, metric);
  snprintf(payload, sizeof(payload), format, value);
  id(mqtt_client).publish(topic, payload);
}

// Usage:
publish_battery_metric(batt, "voltage", id(batt_voltage)[batt], "%.3f");
publish_battery_metric(batt, "current", id(batt_current)[batt], "%.3f");
```

**Effort**: Medium (1-2 hours)
**Priority**: LOW

---

## Major Issues

### 3. Fixed Response Timeout ⏱️

**Location**: Lines 727, 920

**Current Code**:
```cpp
while (millis() - start_ms < 1200) {  // Hard-coded 1200ms
```

**Impact**:
- Legitimate slow responses might timeout
- Fast responses waste time waiting
- No adaptive behavior based on network conditions

**Recommendation**: Make timeout configurable and consider dynamic adjustment based on response history.

**Effort**: Low (30 minutes)
**Priority**: MEDIUM

---

### 4. No Rate Limiting on Error Logging 🚨

**Location**: Lines 744, 877

**Current Code**:
```cpp
ESP_LOGW("rs485", "Batt %d poll failed: %s", batt, reason);
```

**Impact**:
- If battery is disconnected: floods logs with 6 messages/minute per battery
- With 3 batteries offline: 18 log messages/minute
- Log spam makes debugging harder

**Comparison with epever-can-bridge.yaml**:
The epever version now implements rate limiting (commit a93472b):
```cpp
uint32_t now = millis();
if (now - id(last_mismatch_log_time) >= 60000) {
  ESP_LOGI("soc_control", "Mismatch detected...");
  id(last_mismatch_log_time) = now;
}
```

**Recommendation**: Add similar rate limiting (max 1 log per minute per battery).

**Effort**: Low (30 minutes)
**Priority**: MEDIUM

---

### 5. Stale Detection Never Re-Enables ❌

**Location**: Lines 640-644

**Current Code**:
```cpp
if (elapsed > 30000 && !id(can_stale) && id(last_can_rx) > 0) {
  id(can_stale) = true;
  ESP_LOGW("can", "No CAN data for 30s, marking stale");
  id(mqtt_client).publish("${can_prefix}/status", "offline", 0, true);
}
```

**Issue**: Condition `&& !id(can_stale)` prevents re-evaluation
- Once stale, never checks if data returns
- Requires reboot to recover even if CAN bus comes back

**Recommendation**: Add reverse transition:

```cpp
// Check for stale
if (elapsed > 30000 && !id(can_stale) && id(last_can_rx) > 0) {
  id(can_stale) = true;
  ESP_LOGW("can", "No CAN data for 30s, marking stale");
  id(mqtt_client).publish("${can_prefix}/status", "offline", 0, true);
}

// Check for recovery
if (elapsed <= 30000 && id(can_stale)) {
  id(can_stale) = false;
  ESP_LOGI("can", "CAN data resumed, marking online");
  id(mqtt_client).publish("${can_prefix}/status", "online", 0, true);
}
```

**Effort**: Trivial (10 minutes)
**Priority**: HIGH

---

### 6. Unchecked Array Bounds 💥

**Location**: Lines 745, 878, and throughout

**Current Code**:
```cpp
id(batt_poll_failures)[batt]++;
id(batt_poll_alarm)[batt] = true;
```

**Issue**: If `batt` variable exceeds configured `num_batteries`, undefined behavior
- Could crash ESP32
- Could corrupt adjacent memory
- No bounds checking anywhere

**Recommendation**: Add bounds checking macro:

```cpp
#define SAFE_BATTERY_ACCESS(array, index, default_value) \
  ((index) < id(array).size() ? id(array)[index] : (default_value))

// Usage:
if (batt < id(batt_poll_failures).size()) {
  id(batt_poll_failures)[batt]++;
}
```

**Effort**: Low (1 hour to add throughout)
**Priority**: HIGH (safety issue)

---

## Minor Issues

### 7. Magic Numbers Scattered Throughout 🔢

**Locations**: Lines 746, 879 (failure threshold = 10), Lines 727, 920 (timeouts), Lines 1041, 1072 (byte offsets)

**Recommendation**: Define as substitutions:

```yaml
substitutions:
  # ... existing ...
  poll_failure_threshold: "10"
  rs485_tx_delay_ms: "300"
  rs485_rx_timeout_ms: "1200"
```

**Effort**: Low (30 minutes)
**Priority**: LOW

---

### 8. Temperature Conversion Undocumented 🌡️

**Location**: Line 812

**Current Code**:
```cpp
id(cell_temps)[batt * 6 + t] = (raw - 2731) / 10.0f;  // What's 2731?
```

**Recommendation**: Add comment:

```cpp
// Convert from Kelvin*10 to Celsius: (K*10 - 2731) / 10 = °C
// Example: 2981 (298.1K) → (2981 - 2731) / 10 = 25.0°C
id(cell_temps)[batt * 6 + t] = (raw - 2731) / 10.0f;
```

**Effort**: Trivial (2 minutes)
**Priority**: LOW

---

### 9. Hard-Coded Battery Count in Sensors 🔧

**Location**: Lines 1554-1570

**Current Code**:
```yaml
- platform: template
  name: "Battery 0 Poll Alarm"
  lambda: 'return id(batt_poll_alarm).size() > 0 ? id(batt_poll_alarm)[0] : false;'

- platform: template
  name: "Battery 1 Poll Alarm"
  lambda: 'return id(batt_poll_alarm).size() > 1 ? id(batt_poll_alarm)[1] : false;'

- platform: template
  name: "Battery 2 Poll Alarm"
  lambda: 'return id(batt_poll_alarm).size() > 2 ? id(batt_poll_alarm)[2] : false;'
```

**Issue**: If you change `num_batteries` to 2 or 4, must manually edit sensor list

**Recommendation**: This is a limitation of ESPHome YAML - sensors can't be dynamically generated. Document the manual steps required when changing battery count.

**Effort**: Documentation only
**Priority**: LOW

---

### 10. Cell Voltage Parsing Without Validation 📏

**Location**: Lines 796-801

**Current Code**:
```cpp
for (int cell = 0; cell < num_cells && cell < 16; cell++) {
  if (data.length() < i + 4) break;
  int mv = strtol(data.substr(i, 4).c_str(), nullptr, 16);
  id(cell_voltages)[batt * 16 + cell] = mv / 1000.0f;
  i += 4;
}
```

**Issue**: `num_cells` extracted from response without bounds check
- Could be 255 if data is corrupted
- Would loop 255 times reading garbage

**Recommendation**: Add sanity check:

```cpp
if (num_cells > 16) {
  ESP_LOGW("rs485", "Batt %d: invalid num_cells=%d, clamping to 16", batt, num_cells);
  num_cells = 16;
}
```

**Effort**: Trivial (5 minutes)
**Priority**: MEDIUM

---

## Strengths ✅

### 1. Checksum Validation
- ✅ Uses `rs485_validate_response()` helper from `set_include.h`
- ✅ Validates checksum, error codes, address matching
- ✅ Better than initial epever implementation

### 2. Per-Battery State Tracking
- ✅ Comprehensive arrays for each battery metric
- ✅ Independent poll failure tracking per battery
- ✅ Per-battery alarm states

### 3. Clear Code Organization
- ✅ Logical section separation with comments
- ✅ Consistent naming conventions
- ✅ Helper functions in separate header file

### 4. MQTT Topics Match Python Scripts
- ✅ Seamless migration path from Python to ESP32
- ✅ Existing Home Assistant dashboards continue working
- ✅ Good backward compatibility design

---

## Comparison with epever-can-bridge.yaml

| Feature | epever-can-bridge.yaml | deye-bms-can.yaml | Winner |
|---------|------------------------|-------------------|--------|
| **CRC/Checksum Validation** | ✅ Modbus CRC16 | ✅ Pylontech checksum | Tie |
| **Retry Logic** | ✅ Up to 3 attempts | ❌ None | epever |
| **Rate-Limited Logging** | ✅ 1 log/min | ❌ Unlimited | epever |
| **Code Duplication** | ✅ Eliminated in latest | ❌ Extensive | epever |
| **Stale Detection Recovery** | ✅ Auto-recovers | ❌ Manual reboot | epever |
| **Error Metrics** | ✅ Success/failure counters | ✅ Per-battery counters | Tie |
| **Modbus Log Buffer** | ✅ Circular buffer | ❌ None | epever |
| **Documentation** | ✅ Extensive session notes | ⚠️ Needs updating | epever |

---

## Recommendations Summary

### Quick Wins (1-2 hours total)

1. ✅ **Add retry logic** to RS485 polling (Priority: HIGH)
2. ✅ **Fix stale detection** to auto-recover (Priority: HIGH)
3. ✅ **Add bounds checking** on array access (Priority: HIGH)
4. ✅ **Extract response reading** to helper function (Priority: MEDIUM)
5. ✅ **Extract poll failure handling** to helper function (Priority: MEDIUM)
6. ✅ **Add rate limiting** to error logging (Priority: MEDIUM)
7. ✅ **Validate num_cells** before parsing (Priority: MEDIUM)
8. ✅ **Extract buffer clearing** to helper function (Priority: LOW)
9. ✅ **Document temperature conversion** (Priority: LOW)
10. ✅ **Define magic numbers** as substitutions (Priority: LOW)

### Medium Effort (4-6 hours total)

11. ✅ **Refactor HA discovery** into parameterized helpers
12. ✅ **Refactor MQTT publishing** into templated helpers
13. ✅ **Make timeout configurable** and adaptive
14. ✅ **Add circular buffer** for debug logging (like epever)

### Higher Effort (8+ hours)

15. ✅ **Create C++ class wrapper** for RS485 protocol handling
16. ✅ **Implement configurable retry policy** with exponential backoff
17. ✅ **Add comprehensive input validation** for all responses

---

## Testing Recommendations

Before deploying improvements:

1. **Test with battery disconnected** - Verify poll alarm triggers at 10 failures
2. **Test with RS485 bus error** - Verify retry logic recovers within 3 attempts
3. **Test battery count change** - Change `num_batteries` from 3 to 2, verify no crashes
4. **Test CAN bus recovery** - Disconnect/reconnect CAN, verify auto-recovery
5. **Test simultaneous failures** - Offline multiple batteries, verify independent tracking
6. **Test log volume** - Disconnect all batteries, measure log spam before/after rate limiting
7. **Memory leak test** - Run for 24 hours, check free heap doesn't decrease

---

## Next Steps

### Immediate (This Week)
1. Implement retry logic for RS485 (mirrors epever improvements)
2. Fix stale detection recovery
3. Add bounds checking on all array accesses

### Short Term (This Month)
4. Extract duplicate code to helper functions
5. Add rate limiting to error logs
6. Document all magic numbers
7. Write comprehensive session notes (like SESSION_2026-01-19.md for epever)

### Long Term (Future)
8. Consider architectural refactoring using C++ classes
9. Implement adaptive timeout based on response history
10. Add circular debug log buffer
11. Create automated test suite

---

## Files Referenced

- `/home/micke/GitHub/pylontech-bms-mqtt/esphome/deye-bms-can.yaml` - Main firmware (1575 lines)
- `/home/micke/GitHub/pylontech-bms-mqtt/esphome/includes/set_include.h` - Helper functions (158 lines)
- `/home/micke/GitHub/pylontech-bms-mqtt/esphome-epever/epever-can-bridge.yaml` - Comparison baseline (2201 lines)
- `/home/micke/GitHub/pylontech-bms-mqtt/esphome-epever/SESSION_2026-01-19.md` - Recent improvements reference

---

**Review Completed**: 2026-01-19
**Estimated Total Improvement Effort**: 15-20 hours
**Risk Level**: LOW (incremental improvements to working system)
**Recommended Approach**: Implement quick wins first, test thoroughly, then proceed to medium effort items

