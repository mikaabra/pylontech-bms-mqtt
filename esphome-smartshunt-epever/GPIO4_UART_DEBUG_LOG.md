# ESP32-S3 GPIO4 UART Initialization Issue - Debug Log

**Date:** 2026-02-15  
**Hardware:** ESP32-S3 Waveshare board with SmartShunt (VE.Direct)  
**Problem:** UART on GPIO4 works after fresh flash but fails after any reboot

---

## Problem Description

### Symptoms
- SmartShunt (VE.Direct protocol via UART) works immediately after OTA flash
- After ANY reboot (soft or power cycle), UART receives no data
- Signal IS reaching GPIO4 electrically (confirmed by edge detection: ~700k edges/min)
- UART shows "NO DATA ON UART" in diagnostics
- Other UART (RS485/Modbus on GPIO17/18) continues to work

### What Fixes It (The "Dance")
1. Configure pulse_counter on GPIO4 + UART on GPIO5
2. Flash firmware
3. SmartShunt starts working on GPIO5
4. Can then move wire back to GPIO4 if desired

### What Doesn't Work
- Any software-based GPIO/UART manipulation at runtime
- UART driver delete/reinstall sequences
- GPIO pin reconfiguration
- Different GPIO pins (tested GPIO5 - same issue)
- ESP32-S3 UART peripheral resets

---

## Everything We Tried (And Results)

### 1. GPIO Configuration Attempts
**Tried:** Configure GPIO4 as input with various pull modes  
**Result:** ❌ Didn't fix UART  
**Notes:** Tried pull-down, pull-up, no pull. Signal detected but UART still broken.

### 2. UART Peripheral Reset
**Tried:** Delete and reinstall UART1 driver via ESP-IDF API  
**Result:** ❌ Didn't fix UART  
**Notes:** Used `uart_driver_delete()` and `uart_driver_install()`. Driver reinstalls successfully but still no data.

### 3. GPIO Priming with Edge Detection
**Tried:** Configure GPIO4 with edge interrupt (like pulse_counter) before UART init  
**Result:** ❌ Didn't fix UART  
**Notes:** Used `gpio_config()` with `GPIO_INTR_ANYEDGE`, waited 500ms, then disabled interrupt. Edges detected but UART still broken.

### 4. Post-Init UART Reset
**Tried:** Delete/reinstall UART driver AFTER ESPHome UART component initialization  
**Result:** ❌ Broke Modbus AND didn't fix SmartShunt  
**Notes:** Running at priority -100 with 1s delay. Successfully reinstalled UART but broke other UART (RS485).

### 5. Manual GPIO Manipulation Button
**Tried:** Button that configures GPIO4 as input, reads level, waits, reconfigures  
**Result:** ❌ Didn't fix UART  
**Notes:** Various combinations of pull modes and delays tried.

### 6. GPIO5 Instead of GPIO4
**Tried:** Move SmartShunt UART from GPIO4 to GPIO5  
**Result:** ❌ Same problem on GPIO5  
**Notes:** Edge detection works on GPIO5 (~13k edges/min), so signal reaches pin. UART still doesn't work after reboot.

### 7. UART Pin Dance
**Tried:** Runtime reconfiguration: GPIO5→GPIO4→GPIO5  
**Result:** ❌ Broke Modbus AND didn't fix SmartShunt  
**Notes:** Deleted driver from GPIO5, installed on GPIO4, deleted from GPIO4, reinstalled on GPIO5. All operations returned ESP_OK but no data.

### 8. Different Boot Priorities
**Tried:** on_boot at priorities 800, 200, 100, -100  
**Result:** ❌ Code executes but doesn't fix the issue  
**Notes:** Logger wasn't ready at priority 200, fixed by using ESP_LOGI. Code runs but no effect.

### 9. Hardware Pulse Counter Test
**Tried:** Actual pulse_counter component on GPIO4 while UART on GPIO5  
**Result:** ✅ WORKS - This is the key  
**Notes:** Only the full pulse_counter component initialization (not just GPIO manipulation) fixes the issue. Suggests the component does something we haven't replicated.

---

## Key Findings

### What's Different About pulse_counter?
The pulse_counter component initializes something that:
1. Is NOT just GPIO configuration
2. Is NOT just edge interrupt setup
3. Persists after pulse_counter is removed
4. Clears whatever blocks UART from working

### What pulse_counter Does (from ESPHome source):
- Installs GPIO ISR service (`gpio_install_isr_service`)
- Configures PCNT (Pulse Counter) hardware peripheral on ESP32-S3
- Maps GPIO to PCNT channel
- Sets up filter and counter

### Hypothesis
The PCNT (Pulse Counter) peripheral on ESP32-S3 shares resources with UART1 or affects GPIO matrix routing. Initializing PCNT on GPIO4 clears some internal state that prevents UART from working.

---

## Working Solutions

### Option 1: Keep the Dance (Current Best)
**Implementation:** Keep pulse_counter on GPIO4 always running  
**Pros:** Works reliably  
**Cons:** Wastes one GPIO, slight overhead  
**Status:** Not implemented - would need to keep wire on GPIO5

### Option 2: Permanent GPIO5 Move
**Implementation:** Move SmartShunt wire to GPIO5 permanently  
**Pros:** Simple, no dance needed  
**Cons:** GPIO5 has same issue as GPIO4 (untested if it survives reboots)  
**Status:** Not tested - GPIO5 showed same UART init problem

### Option 3: Manual Workaround Button
**Implementation:** Guide user through the dance when needed  
**Pros:** No code changes needed  
**Cons:** Manual intervention required  
**Status:** Documented but not ideal

---

## Open Questions

1. **Why does pulse_counter fix it but raw GPIO manipulation doesn't?**
   - Something in PCNT peripheral initialization clears the state
   
2. **Is this a known ESPHome bug?**
   - Found similar reports: ESPHome Issue #10405 "2025.8 onwards UART Pins can not be used for C6/S3"
   - May be related to ESPHome 2025.8+ UART changes
   
3. **Would downgrading ESPHome help?**
   - Last known working: 2025.7.5 according to bug reports
   - Not tested - would need to downgrade entire environment
   
4. **Can we use UART2 instead of UART1?**
   - Not tested - would need different GPIO pins
   - ESP32-S3 UART2: GPIO15 (RX), GPIO16 (TX)

---

## Next Steps (Not Yet Tried)

1. **Test UART2 on GPIO15/16**
   - Move SmartShunt to UART2 (different peripheral)
   - See if issue is specific to UART1
   
2. **Downgrade ESPHome to 2025.7.5**
   - If bug was introduced in 2025.8, earlier version may work
   - Major change, affects entire project
   
3. **Investigate PCNT peripheral documentation**
   - Understand what PCNT initialization does to GPIO matrix
   - May reveal workaround
   
4. **Keep pulse_counter always active**
   - Add pulse_counter sensor on GPIO4 permanently
   - Keep UART on GPIO5
   - Accept the GPIO waste

---

## Files Modified During Debug

- `rack-solar-bridge.yaml` - Main configuration
- Added various diagnostic intervals and buttons
- Added UART reset code (now mostly removed/reverted)

## Commits Made

1. `e5b9f07` - Initial GPIO4 reset attempt (priority 800)
2. `d19bb71` - UART diagnostic interval
3. `2eecf56` - GPIO5 test + GPIO4 edge detection
4. `7a5dbce` - Aggressive GPIO4 reset
5. `121e5be` - Nuclear UART reset
6. `1f1bce4` - Delayed UART reset at priority -100
7. `16c4b8e` - Manual reset button
8. `c2249fc` - SmartShunt on GPIO5 test
9. `ad464ad` - Manual GPIO4 fix button
10. Current state - Testing various approaches

---

## Conclusion

This is a complex ESP32-S3 initialization issue where UART1 on GPIO4/5 fails to receive data after reboot, despite:
- Signal being electrically present (edge detection works)
- UART driver reporting success (all ESP_OK returns)
- Other UART peripherals working fine

The ONLY reliable fix found is the "pulse_counter dance" - configuring pulse_counter on the GPIO before UART. This suggests a hardware or low-level driver issue that cannot be worked around in software alone.

**Recommended Action:** Move SmartShunt to UART2 (GPIO15) or different hardware, or keep pulse_counter permanently configured on GPIO4.

---

## SOLUTION FOUND

**Date:** 2026-02-15  
**Status:** ✅ RESOLVED

### The Fix

Use explicit `INPUT_PULLUP` mode on the UART RX pin:

```yaml
uart:
  - id: vedirect_uart
    baud_rate: 19200
    rx_pin:
      number: GPIO5
      mode: INPUT_PULLUP  # Critical for ESP32-S3 UART cold boot
    data_bits: 8
    parity: NONE
    stop_bits: 1
```

### Why It Works

The ESP32-S3 UART peripheral requires the RX pin to be in a known state (HIGH) during initialization. Without the internal pull-up:

1. The pin floats or is pulled down during boot
2. UART peripheral sees this as active data/line noise
3. UART state machine gets confused and fails to receive properly
4. Result: "NO DATA ON UART" despite signal being electrically present

With `INPUT_PULLUP`:
1. Pin is pulled HIGH during boot (VE.Direct idle state)
2. UART peripheral sees clean idle state
3. UART initializes correctly
4. Data reception works normally

### Pin Change

SmartShunt moved from GPIO4 to GPIO5 to avoid any residual issues with GPIO4 state.

### Testing

- ✅ Works after fresh flash
- ✅ Works after power cycle (tested twice)
- ✅ Works after soft reboot
- ✅ Stable operation confirmed

### Final Configuration

See commit `0bf14a1` for cleaned up code with documentation.

**Key Lesson:** Always use `INPUT_PULLUP` on UART RX pins for ESP32-S3 when the protocol idle state is HIGH (like VE.Direct).

