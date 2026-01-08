# Add listen-only mode to esp32_can component

## Description

This PR adds a `mode` configuration option to the `esp32_can` component, enabling support for listen-only mode using the ESP32's native TWAI controller.

## Motivation

Listen-only mode is essential for:
- **Passive CAN bus monitoring** without interfering with existing communication
- **BMS monitoring** - tapping into battery-to-inverter communication (e.g., Pylontech, Deye, Growatt)
- **Debugging CAN traffic** without sending ACK signals that could confuse other devices
- **Multi-master scenarios** where the ESP32 should observe but not participate

Without listen-only mode, the ESP32 sends ACK signals on the CAN bus which can cause issues when monitoring existing communication between other devices.

## Implementation

- Adds `mode` configuration option with values `NORMAL` (default) and `LISTEN_ONLY`
- Uses ESP-IDF's native `TWAI_MODE_LISTEN_ONLY` constant
- Prevents transmission attempts in listen-only mode with a warning log
- Follows the same pattern as the existing MCP2515 component's mode support

## Example Configuration

```yaml
canbus:
  - platform: esp32_can
    tx_pin: GPIO15
    rx_pin: GPIO16
    bit_rate: 500kbps
    mode: LISTEN_ONLY  # New option - passive monitoring
    on_frame:
      - can_id: 0x355
        then:
          - lambda: |-
              ESP_LOGI("can", "Received frame 0x355");
```

## Changes

### `canbus.py`
- Import `CONF_MODE` from esphome.const
- Add `CanMode` enum with `CAN_MODE_NORMAL` and `CAN_MODE_LISTEN_ONLY`
- Add `mode` to CONFIG_SCHEMA with default `NORMAL`
- Call `var.set_mode()` in `to_code()`

### `esp32_can.h`
- Add `CanMode` enum definition
- Add `set_mode()` method
- Add `mode_` member variable with default `CAN_MODE_NORMAL`

### `esp32_can.cpp`
- In `setup_internal()`: Select `twai_mode_t` based on configuration
- In `setup_internal()`: Log when listen-only mode is active
- In `send_message()`: Return error with warning if attempting to transmit in listen-only mode

## Testing

Tested on:
- **Hardware**: Waveshare ESP32-S3-RS485-CAN board
- **Use case**: Monitoring Pylontech battery CAN bus at 500kbps
- **Result**: Without listen-only mode, ACK signals interfered with inverter communication. With listen-only mode, passive monitoring works correctly.

## Related

- MCP2515 component already supports `mode: LISTENONLY` - this brings feature parity
- ESP-IDF TWAI documentation: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/twai.html

## Checklist

- [x] Follows ESPHome coding standards
- [x] Backwards compatible (default mode is NORMAL)
- [x] Consistent with existing MCP2515 mode implementation
- [x] Tested on real hardware
