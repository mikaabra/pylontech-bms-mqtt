# Development Guide - ESPHome EPever CAN Bridge

## ESPHome Installation

ESPHome is installed in a Python virtual environment:

```bash
/home/micke/GitHub/esphome/venv/bin/esphome
```

## Common Commands

```bash
# Navigate to project directory
cd /home/micke/GitHub/pylontech-bms-mqtt/esphome-epever

# Compile firmware
/home/micke/GitHub/esphome/venv/bin/esphome compile epever-can-bridge.yaml

# Upload via OTA (requires device on network)
/home/micke/GitHub/esphome/venv/bin/esphome upload --device 10.10.0.45 epever-can-bridge.yaml

# View logs
/home/micke/GitHub/esphome/venv/bin/esphome logs --device 10.10.0.45 epever-can-bridge.yaml

# Clean build cache
/home/micke/GitHub/esphome/venv/bin/esphome clean epever-can-bridge.yaml
```

## Configuration Files

- **epever-can-bridge.yaml** - Main ESPHome configuration
- **secrets.yaml** - Sensitive configuration (WiFi, IPs, passwords) - NOT committed to git
- **custom_components/listen_only_can/** - Custom CAN component for listen-only mode

## Recent Changes (2026-01-16)

### SOC Discharge Control Now Controls Inverter Priority

**Previous behavior**: SOC discharge blocking set the D14 "Stop Discharge" flag, which blocked battery discharge in ALL modes including island mode (power outages).

**New behavior**: SOC discharge blocking now controls the EPever inverter's output priority mode via Modbus register 0x9608:

- **Discharge BLOCKED** (SOC < 50%): Inverter switches to **Utility Priority** (grid mode)
  - Battery is not used for loads
  - Prevents hourly battery cycling at low SOC
  - Battery can still charge from PV/grid

- **Discharge ALLOWED** (SOC > 55%): Inverter switches to **Inverter Priority** (battery mode)
  - Battery can discharge normally
  - Battery/PV preferred over grid

**Key advantage**: This allows the battery to be used in island mode (power outages) regardless of SOC control settings, since we're not blocking discharge via D14 flag anymore.

### Instant Response to State Changes

The system now **immediately** updates inverter priority (within 1 second) when:

1. **SOC crosses thresholds**: When `soc_discharge_blocked` flag changes
   - SOC drops below 50% → Triggers update to Utility Priority
   - SOC rises above 55% → Triggers update to Inverter Priority

2. **User changes controls**: When any control is modified (5 second delay)
   - Enable/Disable SOC Reserve Control
   - Change discharge thresholds
   - Change force charge thresholds
   - Change inverter priority control mode

This eliminates the need to wait for the 3-hour interval or manually press the refresh button.

### Statistics and Monitoring

**New sensors on web UI**:
- **Inverter Priority Change Attempts**: Total write attempts
- **Inverter Priority Change Successes**: Successful writes
- **Inverter Priority Change Failures**: Failed writes (timeouts, exceptions)
- **Inverter Priority Change Success Rate**: Percentage of successful writes
- All counters persist across reboots via NVRAM

**Debug Level Control**:
- **Minimal**: Only Modbus READ/WRITE commands and responses (default)
- **Normal**: Adds success/failure messages, status updates, CAN health
- **Verbose**: Full debug including socket operations, connections, timing

### Improved Reliability

**Function 0x04 Support**: EPever inverters sometimes respond with function 0x04 (Read Input Registers) instead of 0x03 (Read Holding Registers). The firmware now accepts both response types, fixing issues where the refresh button required multiple presses.

**Retry Logic**: Automatic retry on Modbus timeouts to handle conflicts when Home Assistant is also polling the inverter (single-client Modbus gateway limitation).

**State Persistence**: SOC Reserve Control switch state now persists across reboots.

### Implementation Details

**Key code locations**:
- Lines 202-215: SOC threshold crossing triggers `inverter_priority_update_requested` flag
- Lines 754-918: Fast 1-second interval checks flag and executes Modbus write
- Lines 1822-1844: Control changes trigger update after 5-second delay
- Line 2001: SOC Reserve Control uses `restore_mode: RESTORE_DEFAULT_OFF`
- Lines 2172-2179: Refresh button accepts both function 0x03 and 0x04 responses

**Modbus communication**:
- Background 3-hour interval (configurable via `inverter_priority_update_interval`)
- Triggered updates via flag mechanism (1 second response time)
- Only sends Modbus command when mode actually needs to change
- Uses function 0x10 (Write Multiple Registers) - required by EPever

## Hardware

- **Board**: Waveshare ESP32-S3-RS485-CAN
- **CAN**: GPIO15 (TX), GPIO16 (RX), 500 kbps, listen-only mode
- **RS485**: GPIO17 (TX), GPIO18 (RX), 9600 baud, 8N1
- **Network**: WiFi with static IP 10.10.0.45

## Troubleshooting

### Compilation Errors

If you see "esphome: command not found":
```bash
# Use full path to virtual environment
/home/micke/GitHub/esphome/venv/bin/esphome
```

### OTA Upload Fails

1. Check device is reachable: `ping 10.10.0.45`
2. Check logs via web interface: `http://10.10.0.45/`
3. Try USB upload if OTA fails:
   ```bash
   /home/micke/GitHub/esphome/venv/bin/esphome run epever-can-bridge.yaml
   ```

### Inverter Priority Not Switching

1. Check SOC Reserve Control is enabled in web UI
2. Verify Modbus connection to inverter (check logs)
3. Confirm inverter_host/port/slave in secrets.yaml are correct
4. Check that SOC is crossing thresholds (default 50%/55%)

## Web Interface

Access the device web interface at: `http://10.10.0.45/`

**Configuration controls**:
- Enable SOC Reserve Control (master switch)
- SOC Discharge Control thresholds (50%/55% default)
- SOC Force Charge Control thresholds (45%/50% default)
- Inverter Output Priority Control (Auto/Force Inverter/Force Utility)
- Manual D13/D14/D15 flag overrides

**Buttons**:
- **Refresh Inverter Priority**: Reads current mode from inverter, logs comparison with desired mode, and updates if needed
  - Use this to verify Modbus communication is working
  - Bypasses the 3-hour timer for immediate check/update
  - Logs output includes current mode, desired mode, and whether they match

## Testing

After uploading new firmware:

1. Monitor logs to verify CAN frames are being received
2. Check SOC updates every ~3 seconds
3. Verify Modbus responses to inverter polling
4. Test SOC threshold crossing to confirm inverter priority switching
5. Check that D14 flag only responds to BMS protection, not SOC control

## Known Issues

See KNOWN_ISSUES.md for details on:
- D14 flag blocking discharge in island mode (mitigated by new inverter priority control)
- Modbus single-client limitation (use 3-hour update interval)

## Related Documentation

- **HLD.md** - High-level design and architecture
- **LLD.md** - Low-level implementation details
- **KNOWN_ISSUES.md** - Known limitations and workarounds
- **SOC_HYSTERESIS_FEASIBILITY.md** - SOC control design rationale
