# ESPHome Configuration for Waveshare ESP32-S3-RS485-CAN

This directory contains the ESPHome configuration to replace `pylon_can2mqtt.py` with an ESP32-based solution.

## Hardware

- **Board**: Waveshare ESP32-S3-RS485-CAN
- **CAN pins**: TX=GPIO15, RX=GPIO16
- **CAN transceiver**: Onboard isolated transceiver
- **Protocol**: Pylontech CAN at 500kbps

## Files

- `deye-bms-can.yaml` - Main ESPHome configuration
- `secrets.yaml` - WiFi/MQTT credentials (edit before flashing)

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

2. Connect the ESP32-S3 to the Raspberry Pi via USB

3. Put the board in download mode:
   - Hold the BOOT button
   - Press and release the RESET button
   - Release the BOOT button

## Flashing Commands

### First-time flash (via USB)
```bash
cd /root/esphome
esphome run deye-bms-can.yaml
```

Select the USB serial port when prompted (usually `/dev/ttyACM0` or `/dev/ttyUSB0`).

### Subsequent updates (via WiFi OTA)
```bash
esphome run deye-bms-can.yaml
```

Select the network option (device will show as `deye-bms-can.local`).

### Compile only (no upload)
```bash
esphome compile deye-bms-can.yaml
```

### View logs
```bash
# Via USB
esphome logs deye-bms-can.yaml --device /dev/ttyACM0

# Via WiFi
esphome logs deye-bms-can.yaml
```

## Wiring

Connect to BMS CAN bus:
- ESP32 CAN-H → BMS CAN-H
- ESP32 CAN-L → BMS CAN-L
- GND → GND (if not isolated)

Enable the 120Ω termination resistor jumper if this is at the end of the CAN bus.

## MQTT Topics

The ESPHome device publishes to the same topics as `pylon_can2mqtt.py` for seamless migration:

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

## Migration from pylon_can2mqtt.py

1. Stop the Python script
2. Disconnect CAN from Raspberry Pi
3. Flash and connect the ESP32
4. Home Assistant entities will continue working (same MQTT topics)

## Troubleshooting

### Board not detected
- Check USB cable (use data cable, not charge-only)
- Try different USB port
- Enter download mode (BOOT + RESET sequence)

### CAN not receiving data
- Check CAN-H/CAN-L wiring
- Verify 500kbps baud rate matches BMS
- Check termination resistor if at end of bus
- View logs for errors: `esphome logs deye-bms-can.yaml`

### WiFi connection issues
- Check `secrets.yaml` credentials
- Board falls back to AP mode: connect to `deye-bms-can-fallback` / `fallback123`
- Access web UI at http://192.168.4.1 in AP mode
