# ESPHome 2026.1.0 Upgrade Notes

## Major Changes and New Features

### 1. CAN Bus Component Now Supports LISTENONLY Mode ✅

**Status:** CONFIRMED - The standard `esp32_can` component now supports `mode: LISTENONLY`

**Impact:** We can now migrate from the custom `esp32_can_listen` component to the standard ESPHome CAN component.

**Configuration Change:**
```yaml
# Old (custom component)
canbus:
  - platform: esp32_can_listen
    id: can_bus
    tx_pin: GPIO15
    rx_pin: GPIO16
    can_id: 0
    bit_rate: 500kbps
    mode: LISTENONLY

# New (standard component)
canbus:
  - platform: esp32_can
    id: can_bus
    tx_pin: GPIO15
    rx_pin: GPIO16
    can_id: 0
    bit_rate: 500kbps
    mode: LISTENONLY
```

**Benefits:**
- No more custom component maintenance
- Better integration with ESPHome ecosystem
- Future-proof as ESPHome evolves
- Access to all standard CAN features

### 2. Configuration Format Changes

**Status:** CONFIRMED - The `platform` key has been removed from the `esphome` block

**Impact:** Need to update configuration format:
```yaml
# Old format
esphome:
  name: test-can
  platform: ESP32
  board: esp32-s3-devkitc-1

# New format
esphome:
  name: test-can

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf
```

### 3. Potential New Utilities

**Status:** NEEDS INVESTIGATION - ESPHome 2026.1.0 may include new utility functions

**Areas to Check:**
- Built-in CRC/checksum functions
- Enhanced string manipulation utilities
- Improved data parsing components
- Better error handling frameworks

## Migration Plan

### Phase 1: CAN Component Migration (HIGH PRIORITY)
1. Replace `esp32_can_listen` with `esp32_can`
2. Update configuration format
3. Test CAN communication thoroughly
4. Remove custom component dependency

### Phase 2: Configuration Format Update
1. Update all `esphome` blocks to new format
2. Move platform-specific settings to component blocks
3. Verify all configurations compile correctly

### Phase 3: Leverage New Features
1. Investigate new utility functions
2. Update CRC implementation if better options available
3. Improve string/data handling with new utilities
4. Enhance error handling with new frameworks

## Testing Strategy

### CAN Component Testing
1. **Basic Functionality:** Verify LISTENONLY mode works correctly
2. **Frame Reception:** Ensure all expected CAN frames are received
3. **Error Handling:** Test error conditions and recovery
4. **Performance:** Compare with previous custom implementation

### Configuration Testing
1. **Validation:** Ensure all configurations pass ESPHome validation
2. **Compilation:** Verify successful compilation with new format
3. **Upload:** Test device flashing with updated configuration
4. **Runtime:** Confirm all functionality works as expected

## Benefits of Upgrade

1. **Reduced Maintenance:** No more custom component updates
2. **Better Integration:** Full ESPHome ecosystem compatibility
3. **Future-Proof:** Aligned with ESPHome development direction
4. **Improved Stability:** Standard components are more thoroughly tested
5. **Enhanced Features:** Access to latest ESPHome capabilities

## Risks and Mitigation

1. **Configuration Errors:** Careful testing and validation
2. **Functionality Changes:** Thorough regression testing
3. **Performance Differences:** Benchmarking and comparison
4. **Dependency Issues:** Gradual migration with fallback options

## Recommendations

1. **Proceed with CAN migration immediately** - This provides the most significant benefit
2. **Update configuration format** - Required for ESPHome 2026.1.0 compatibility
3. **Investigate new utilities** - Could provide additional improvements
4. **Maintain backward compatibility** - Ensure smooth transition for existing deployments

The upgrade to ESPHome 2026.1.0 enables us to eliminate the custom CAN component and leverage the standard ESPHome ecosystem, which is a major improvement for maintainability and future development.