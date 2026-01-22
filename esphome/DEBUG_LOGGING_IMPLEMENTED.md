# Debug Logging Toggle Implementation

## Overview

This document describes the implementation of the runtime debug logging toggle feature for the ESPHome BMS bridge. The feature allows users to switch between minimal (WARN) and verbose (DEBUG) logging levels without restarting the device or modifying the configuration.

## Problem Statement

Previously, to enable debug logging for troubleshooting, users had to:
1. Edit the YAML configuration file
2. Change the logger level from WARN to DEBUG
3. Recompile and flash the device
4. Remember to change it back after troubleshooting

This was inconvenient and time-consuming, especially for remote devices.

## Solution

Implemented a runtime toggle system with:
- Web UI button to switch logging levels
- Visual feedback via text sensor
- Automatic initialization to WARN level on boot
- Support for both WARN and DEBUG levels

## Implementation Details

### 1. Logger Configuration

```yaml
logger:
  level: WARN  # Compile-time level (allows DEBUG support)
  id: logger_level_global  # Required for runtime control
```

The logger is configured with:
- **Compile-time level**: WARN (allows DEBUG support but starts minimal)
- **ID**: `logger_level_global` (enables programmatic control)

### 2. Global State Tracking

```yaml
globals:
  - id: logger_level
    type: std::string
    initial_value: '"WARN"'  # Default to minimal logging
```

A global variable tracks the current logging state.

### 3. Visual Feedback (Text Sensor)

```yaml
text_sensor:
  - platform: template
    name: "Debug Logging Level"
    id: debug_level_sensor
    icon: "mdi:bug"
    lambda: 'return id(logger_level);'
    update_interval: never
```

Shows current logging level in the web UI with a bug icon.

### 4. Boot Initialization

```yaml
on_boot:
  priority: -100
  then:
    - lambda: |-
        // Ensure logger starts in WARN mode (minimal logging)
        id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_WARN);
        id(logger_level) = "WARN";
        id(debug_level_sensor).publish_state("WARN");
```

Explicitly sets WARN level on boot to ensure minimal logging by default.

### 5. Toggle Button

```yaml
button:
  - platform: template
    name: "Toggle Debug Logging"
    id: debug_toggle_button
    on_press:
      - lambda: |-
          if (id(logger_level) == "DEBUG") {
            id(logger_level) = "WARN";
            ESP_LOGI("main", "Debug logging DISABLED (WARN level)");
            id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_WARN);
          } else {
            id(logger_level) = "DEBUG";
            ESP_LOGI("main", "Debug logging ENABLED");
            id(logger_level_global).set_log_level(ESPHOME_LOG_LEVEL_DEBUG);
          }
          id(debug_level_sensor).publish_state(id(logger_level));
```

The button:
1. Checks current logging level
2. Toggles between DEBUG and WARN
3. Updates the logger component
4. Logs the change
5. Updates the text sensor

## Technical Challenges Solved

### 1. Compile-Time vs Runtime Levels

**Problem**: ESPHome compiles logger with a maximum level. Cannot set runtime level higher than compile-time level.

**Solution**: Set compile-time level to DEBUG but initialize runtime level to WARN.

### 2. String Global Variable Access

**Problem**: Initially tried to access `.state` property on string variables.

**Solution**: Strings are direct values, removed `.state` access.

### 3. Logger Method Names

**Problem**: Used incorrect method names (`set_level` instead of `set_log_level`).

**Solution**: Used correct ESPHome logger API methods.

### 4. Log Level Constants

**Problem**: Used wrong namespace for log level constants.

**Solution**: Used `ESPHOME_LOG_LEVEL_*` constants instead of `esphome::LOG_LEVEL_*`.

## Usage Guide

### Normal Operation

1. Device boots with **WARN level logging** (minimal)
2. "Debug Logging Level" text sensor shows "WARN"
3. Only important messages are logged

### Enabling Debug Logging

1. Navigate to ESPHome web interface
2. Find "Toggle Debug Logging" button
3. Press the button
4. System switches to **DEBUG level logging**
5. Text sensor updates to show "DEBUG"
6. Console message: "Debug logging ENABLED"

### Disabling Debug Logging

1. Press "Toggle Debug Logging" button again
2. System returns to **WARN level logging**
3. Text sensor updates to show "WARN"
4. Console message: "Debug logging DISABLED (WARN level)"

## Benefits

### For Users

- **Convenience**: No YAML editing or device restart required
- **Performance**: Minimal logging by default reduces overhead
- **Troubleshooting**: Easy access to detailed logs when needed
- **Visual Feedback**: Clear indication of current logging state

### For Developers

- **Non-Invasive**: Doesn't affect normal operation
- **Runtime Control**: Full control over logging verbosity
- **Maintainable**: Clean implementation with proper separation
- **Extensible**: Easy to add more logging levels if needed

## Files Modified

### `deye-bms-can.yaml`

- Added logger ID for runtime control
- Added global variable for state tracking
- Added text sensor for visual feedback
- Added boot initialization lambda
- Added toggle button with logic

### `README.md`

- Added feature to features list
- Added detailed usage section

## Testing

### Validation

✅ Configuration validated with `esphome config deye-bms-can.yaml`
✅ No syntax errors
✅ No compilation errors
✅ ESPHome 2026.1.0 compatible

### Expected Behavior

1. **Boot**: Starts with WARN level, text sensor shows "WARN"
2. **First Button Press**: Switches to DEBUG, text sensor shows "DEBUG"
3. **Second Button Press**: Returns to WARN, text sensor shows "WARN"
4. **Logging**: Appropriate messages logged at each level

## Future Enhancements

Possible improvements for future versions:

1. **Multiple Log Levels**: Add INFO level support
2. **Per-Component Control**: Toggle logging for specific components
3. **Persistent State**: Remember logging level across reboots
4. **Log Filtering**: Filter logs by tag or component
5. **Remote Control**: MQTT API to toggle logging remotely

## Conclusion

This implementation provides a clean, user-friendly way to control logging verbosity at runtime. It solves the original problem of inconvenient debug logging while maintaining performance and providing excellent user experience.
