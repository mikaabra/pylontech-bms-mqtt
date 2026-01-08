# ESPHome Configuration for Waveshare ESP32-S3-RS485-CAN

This directory contains the ESPHome configuration to replace both `pylon_can2mqtt.py` and `pylon_rs485_monitor.py` with a single ESP32-based solution.

## Features

- **CAN Bus**: Full decoding of 0x351, 0x355, 0x359, 0x370 frames (passive listen-only mode)
- **RS485**: Individual cell voltages, temperatures, current, SOC, cycles, alarms, balancing
- **Per-battery tracking**: Balancing cells, overvolt cells, MOSFET status, warnings, alarms
- **Stack aggregation**: Min/max voltages, balancing/overvolt counts across all batteries
- **MQTT Topics**: Identical to Python scripts for seamless migration
- **Home Assistant**: Publishes HA discovery configs on connect (no Python scripts needed)
- **Resilient**: Automatic WiFi/MQTT reconnection, stale data detection
- **Rate limiting**: Hysteresis filters to reduce MQTT traffic (60s heartbeat + delta thresholds)
- **Data validation**: Response checksum verification and address matching
- **Poll alarms**: Per-battery alarm after 10 consecutive communication failures
- **Diagnostics**: Heap monitoring for memory health tracking

## Hardware

- **Board**: [Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)
- **CAN pins**: TX=GPIO15, RX=GPIO16
- **RS485 pins**: TX=GPIO17, RX=GPIO18, DE/RE=GPIO21
- **CAN transceiver**: Onboard isolated transceiver
- **RS485 transceiver**: Onboard isolated transceiver with hardware flow control
- **Protocol**: Pylontech CAN at 500kbps, Pylontech RS485 at 9600 baud

## Files

- `deye-bms-can.yaml` - Main ESPHome configuration
- `secrets.yaml` - WiFi/MQTT credentials (edit before flashing)
- `custom_components/esp32_can_listen/` - Custom CAN component with listen-only mode

## Important Implementation Notes

### CAN Bus Listen-Only Mode

The ESP32 must operate in **listen-only mode** on the CAN bus to avoid interfering with
the existing BMS-to-inverter communication. Standard ESPHome `esp32_can` component doesn't
support this, so we use a custom component (`esp32_can_listen`) that sets the ESP-IDF
TWAI driver to `TWAI_MODE_LISTEN_ONLY`.

**Why this matters**: Without listen-only mode, the ESP32 sends ACK signals on the CAN bus,
which can cause the inverter to think batteries are offline or behave erratically.

### RS485 Hardware Flow Control

The Waveshare ESP32-S3-RS485-CAN board has automatic RS485 direction control when using
ESPHome's `flow_control_pin` feature. **Do NOT manually toggle GPIO21** - instead, configure
the UART with:

```yaml
uart:
  tx_pin: GPIO17
  rx_pin: GPIO18
  baud_rate: 9600
  flow_control_pin: GPIO21  # Hardware RS485 direction control
```

This uses the ESP-IDF UART driver's built-in RS485 half-duplex mode, which automatically:
- Asserts the RTS/DE pin when transmitting
- De-asserts when transmission completes
- Switches to receive mode with proper timing

**Manual GPIO toggling doesn't work** on this board because the timing is critical and
the hardware flow control handles it correctly.

### RS485 Termination Resistor

The Waveshare board has an onboard 120Ω termination resistor that can be enabled via jumper.
**Recommendation: Leave termination OFF** unless you're at the end of a long RS485 bus.
In testing, disabling the termination resistor improved communication reliability.

### Pylontech RS485 Protocol Notes

The Pylontech RS485 protocol uses a specific LENID checksum calculation:

```
LENID = LCHKSUM + LENGTH (4 hex chars total)
LCHKSUM = (~(sum of hex digits of LENGTH) + 1) & 0xF
```

For example, with INFO length of 2 hex characters (1 byte):
- LENGTH = "002" (3 hex chars)
- Sum of digits: 0 + 0 + 2 = 2
- LCHKSUM = (~2 + 1) & 0xF = 14 = 0xE
- LENID = "E002"

Getting this checksum wrong results in error code 03 (CID2 invalid) from the battery.

### Battery Numbering

Battery numbers in the INFO field are **0-based** (0, 1, 2), matching the Python script.
If a battery doesn't respond, try resetting the BMS - it may have entered a bad state
from previous RS485 communication attempts.

## Before First Flash

1. Edit `secrets.yaml` with your actual credentials:
   ```yaml
   wifi_ssid: "YOUR_WIFI_SSID"
   wifi_password: "YOUR_WIFI_PASSWORD"
   mqtt_host: "YOUR_MQTT_BROKER_IP"
   mqtt_user: "YOUR_MQTT_USER"
   mqtt_password: "YOUR_MQTT_PASSWORD"
   ota_password: "YOUR_OTA_PASSWORD"
   ```

2. Connect the ESP32-S3 to the Raspberry Pi via USB-C

3. Put the board in download mode (for first flash only):
   - Hold the BOOT button
   - Press and release the RESET button
   - Release the BOOT button

## Flashing Commands

### First-time flash (via USB)
```bash
cd /root/esphome
esphome run deye-bms-can.yaml
```

Select the USB serial port when prompted (usually `/dev/ttyACM0`).

### Subsequent updates (via WiFi OTA)
```bash
esphome upload deye-bms-can.yaml --device <IP_ADDRESS>
```

Or use the device name:
```bash
esphome upload deye-bms-can.yaml --device deye-bms-can.local
```

### Compile only (no upload)
```bash
esphome compile deye-bms-can.yaml
```

### View logs
```bash
# Via network (preferred when running on external power)
esphome logs deye-bms-can.yaml --device <IP_ADDRESS>

# Via USB
esphome logs deye-bms-can.yaml --device /dev/ttyACM0
```

## Wiring

### CAN Bus
Connect to the BMS-to-inverter CAN bus (passive tap):
- ESP32 CAN-H → CAN-H on bus
- ESP32 CAN-L → CAN-L on bus

**CAN Termination**: Leave the 120Ω jumper **OFF** since you're tapping into the middle
of an existing bus (the BMS and inverter already have termination).

### RS485
Connect to the battery RS485 port:
- ESP32 RS485-A → Battery RS485-A (D+)
- ESP32 RS485-B → Battery RS485-B (D-)

**RS485 Termination**: Try with jumper **OFF** first. Enable only if communication
is unreliable on long cable runs.

**Note**: Some batteries have multiple RS485 ports (e.g., pins 1,2 and pins 7,8).
If one pair doesn't work, try the other.

## MQTT Topics

### CAN Bus Topics (deye_bms/)

| Topic | Description |
|-------|-------------|
| `deye_bms/status` | online/offline availability |
| `deye_bms/soc` | State of Charge (%) |
| `deye_bms/soh` | State of Health (%) |
| `deye_bms/limit/v_charge_max` | Max charge voltage |
| `deye_bms/limit/v_low` | Low voltage limit |
| `deye_bms/limit/i_charge` | Charge current limit |
| `deye_bms/limit/i_discharge` | Discharge current limit |
| `deye_bms/ext/cell_v_min` | Min cell voltage |
| `deye_bms/ext/cell_v_max` | Max cell voltage |
| `deye_bms/ext/cell_v_delta` | Cell voltage delta |
| `deye_bms/ext/temp_min` | Min temperature |
| `deye_bms/ext/temp_max` | Max temperature |
| `deye_bms/flags` | BMS status flags |
| `deye_bms/diag/free_heap` | ESP32 free heap memory (bytes) |

### RS485 Topics (deye_bms/rs485/)

| Topic | Description |
|-------|-------------|
| `deye_bms/rs485/status` | online/offline availability |
| **Stack Topics** | |
| `deye_bms/rs485/stack/cell_min` | Stack minimum cell voltage |
| `deye_bms/rs485/stack/cell_max` | Stack maximum cell voltage |
| `deye_bms/rs485/stack/cell_delta_mv` | Stack cell voltage delta (mV) |
| `deye_bms/rs485/stack/voltage` | Stack average voltage |
| `deye_bms/rs485/stack/current` | Stack total current |
| `deye_bms/rs485/stack/temp_min` | Stack minimum temperature |
| `deye_bms/rs485/stack/temp_max` | Stack maximum temperature |
| `deye_bms/rs485/stack/balancing_count` | Total cells balancing |
| `deye_bms/rs485/stack/balancing_active` | 1 if any cell balancing |
| `deye_bms/rs485/stack/balancing_cells` | List of balancing cells (e.g., "B0C3,B1C7") |
| `deye_bms/rs485/stack/overvolt_count` | Total cells in overvolt |
| `deye_bms/rs485/stack/overvolt_active` | 1 if any cell overvolt |
| `deye_bms/rs485/stack/overvolt_cells` | List of overvolt cells |
| `deye_bms/rs485/stack/alarms` | Combined alarm list |
| **Per-Battery Topics** | (N = 0, 1, 2) |
| `deye_bms/rs485/batteryN/cellXX` | Individual cell voltages (XX = 01-16) |
| `deye_bms/rs485/batteryN/tempX` | Temperature sensors (X = 1-6) |
| `deye_bms/rs485/batteryN/voltage` | Battery voltage |
| `deye_bms/rs485/batteryN/current` | Battery current |
| `deye_bms/rs485/batteryN/soc` | Battery SOC |
| `deye_bms/rs485/batteryN/cycles` | Charge cycles |
| `deye_bms/rs485/batteryN/balancing_count` | Cells balancing in this battery |
| `deye_bms/rs485/batteryN/balancing_cells` | List of balancing cells (e.g., "3,7") |
| `deye_bms/rs485/batteryN/overvolt_count` | Cells in overvolt |
| `deye_bms/rs485/batteryN/overvolt_cells` | List of overvolt cells |
| `deye_bms/rs485/batteryN/state` | Operating state (Charge, Discharge, etc.) |
| `deye_bms/rs485/batteryN/warnings` | Warning list |
| `deye_bms/rs485/batteryN/alarms` | Alarm list |
| `deye_bms/rs485/batteryN/charge_mosfet` | Charge MOSFET status (1/0) |
| `deye_bms/rs485/batteryN/discharge_mosfet` | Discharge MOSFET status (1/0) |
| `deye_bms/rs485/batteryN/poll_alarm` | ON if 10+ consecutive poll failures |

## Migration from Python Scripts

1. Stop the Python scripts:
   ```bash
   pkill -f pylon_can2mqtt.py
   pkill -f pylon_rs485_monitor.py
   ```

2. Disconnect CAN and RS485 from Raspberry Pi adapters

3. Connect CAN and RS485 to the ESP32

4. Power the ESP32 (12V input or USB-C)

5. Home Assistant entities will continue working (same MQTT topics)

## Troubleshooting

### Board not detected via USB
- Check USB-C cable (use data cable, not charge-only)
- Try different USB port
- Enter download mode (BOOT + RESET sequence)
- Check `lsusb` for "Espressif USB JTAG/serial debug unit"

### CAN not receiving data
- Check CAN-H/CAN-L wiring (don't swap them)
- Verify 500kbps baud rate matches BMS
- Ensure you're using the custom `esp32_can_listen` component with `mode: LISTEN_ONLY`
- View logs for "CAN bus configured in LISTEN_ONLY mode" message

### RS485 not receiving data
- Verify `flow_control_pin: GPIO21` is set in UART config
- Try swapping A/B wires
- Try different RS485 port on battery (some have multiple)
- Disable 120Ω termination resistor
- Check baud rate is 9600
- Look for "RX len=0" in debug logs (no response) vs "RX len=144" (good response)
- Error code 03 means checksum error in the request

### RS485 receiving data but error codes
- Error 03 (CID2 invalid): Check LENID checksum calculation
- Error 07 (ADR error): Battery address doesn't exist
- No response from a battery: Try resetting that battery's BMS

### WiFi connection issues
- Check `secrets.yaml` credentials
- Board falls back to AP mode: connect to `deye-bms-can-fallback` / `fallback123`
- Access web UI at http://192.168.4.1 in AP mode

### Device crashes or becomes unresponsive
- Check power supply - USB-C from Pi may be insufficient under load
- Use external 12V power supply for production deployment

## Production Deployment

### Log Level

The default log level is `WARN` to minimize serial output. To enable debug logging
for troubleshooting, edit `deye-bms-can.yaml`:

```yaml
logger:
  level: DEBUG  # Change from WARN to DEBUG
```

### Stale Data Detection

The ESP32 monitors data freshness and publishes availability status:

- **CAN**: Publishes `deye_bms/status: offline` if no CAN frames received for 30 seconds
- **RS485**: Publishes `deye_bms/rs485/status: offline` if no valid responses for 90 seconds

When data resumes, status automatically returns to `online`.

### Data Validation

Every RS485 response is validated before processing:
- **Checksum**: Verifies the 4-character checksum at end of frame matches calculated value
- **Address**: Confirms response address matches the configured Pylontech address
- **Error code**: Checks for success code (00) in response

If validation fails, the response is rejected and logged.

### Poll Alarms

Each battery has an independent poll failure counter:
- Counter increments on any validation failure (checksum, address, error code, timeout)
- Counter resets to zero on successful response
- **Alarm triggers** when counter reaches 10 consecutive failures
- Alarm publishes `ON` to `deye_bms/rs485/batteryN/poll_alarm` (retained)
- Alarm clears automatically when battery responds successfully

### Heap Monitoring

Free heap memory is published every 60 seconds to `deye_bms/diag/free_heap`.
Typical healthy values are 200-250KB. Significant drops may indicate memory leaks.

### Rate Limiting (Hysteresis)

To reduce MQTT traffic, sensors use filters:
- **Heartbeat**: Publish at least every 60 seconds
- **Delta**: Publish immediately if value changes significantly
  - SOC/SOH: ±1%
  - Voltages: ±0.1V (pack) or ±5mV (cell)
  - Currents: ±1A
  - Temperatures: ±0.5°C

### Automatic Recovery

ESPHome handles these scenarios automatically:
- **WiFi disconnect**: Reconnects and resumes operation
- **MQTT disconnect**: Reconnects and republishes availability/discovery
- **OTA updates**: Safe mode if boot fails multiple times
