# Pylontech BMS & Deye Inverter MQTT Bridge

A Python-based bridge that reads battery and inverter data and publishes to MQTT with Home Assistant auto-discovery.

Supports **CAN bus**, **RS485**, and **Modbus-TCP** interfaces for comprehensive system monitoring.

## Features

- **CAN Bus Monitoring**: Real-time SOC, SOH, voltage/current limits, cell min/max, temperatures (Pylontech protocol)
- **RS485 Monitoring**: Individual cell voltages (48 cells), temperatures, balancing status, alarms, cycle counts
- **Modbus-TCP Monitoring**: Deye inverter data - PV power, grid/load power, temperatures, energy totals
- **Home Assistant Integration**: Auto-discovery for all sensors - entities appear automatically
- **Robust Operation**: Auto-reconnect, Last Will Testament for availability tracking, rate limiting with hysteresis
- **Parallel Battery Support**: Tested with 3× 16S LFP batteries in parallel configuration

## Quick Start

```bash
# 1. Clone and install
git clone https://github.com/mikaabra/pylontech-bms-mqtt.git
cd pylontech-bms-mqtt
pip3 install -r requirements.txt

# 2. Set up CAN interface (one-time)
sudo ip link set can0 up type can bitrate 500000

# 3. Configure MQTT (add to ~/.bashrc or use systemd env file)
export MQTT_HOST=192.168.1.100  # Your MQTT broker

# 4. Run the bridges
./pylon_can2mqtt.py              # CAN bus bridge (BMS)
./pylon_rs485_monitor.py --mqtt --loop  # RS485 monitor (BMS cells)
./deye_modbus2mqtt.py --mqtt --loop     # Modbus-TCP bridge (Inverter)

# 5. Check Home Assistant - entities appear automatically!
```

For production use, see [Running as a Service](#running-as-a-service) below.

## Compatible Hardware

### Tested Batteries
- Shoto SDA10-48200 (16S 200Ah LFP)
- Should work with any Pylontech-protocol compatible BMS:
  - Pylontech US2000/US3000/US5000
  - Deye SE-G5.1 Pro
  - Shoto branded batteries
  - Other "Pylontech compatible" batteries

### Tested Interfaces
- **CAN**: Openmoko USB-CAN adapter (gs_usb compatible) - [Amazon](https://www.amazon.se/dp/B0CRB8KXWL)
- **CAN**: MCP2515-based SPI CAN modules
- **RS485**: FTDI FT232-based USB-RS485 adapters
- **RS485**: CH340-based USB-RS485 adapters

### Known Non-Working Hardware
- **Waveshare USB to CAN Adapter (STM32-based, "4 Working Modes")** - Cannot run in passive/listen-only mode, which is required for monitoring BMS-to-inverter CAN traffic without interfering with the bus. The adapter sends ACKs which disrupts the existing communication. Avoid for CAN bus monitoring applications.

### Host Systems
- Raspberry Pi 4 (recommended)
- Raspberry Pi 3/3B+
- Any Linux system with SocketCAN support

### Wiring

| Interface | BMS Connection | Notes |
|-----------|----------------|-------|
| CAN-H     | CAN-H (pin 1) | 120Ω termination may be needed |
| CAN-L     | CAN-L (pin 2) | |
| RS485-A   | RS485-A/+ | Active high |
| RS485-B   | RS485-B/- | Active low |
| GND       | GND | Required for RS485 |

**BMS Connector**: Typically RJ45 or similar. Check your battery documentation for pinout.

## Installation

```bash
# Clone the repository
git clone https://github.com/mikaabra/pylontech-bms-mqtt.git
cd pylontech-bms-mqtt

# Install Python dependencies
pip3 install -r requirements.txt
```

### CAN Interface Setup

```bash
# Install CAN utilities
sudo apt-get install can-utils

# Create CAN interface config
sudo tee /etc/network/interfaces.d/can0 << 'EOF'
auto can0
iface can0 inet manual
    pre-up /sbin/ip link set can0 type can bitrate 500000
    up /sbin/ifconfig can0 up
    down /sbin/ifconfig can0 down
EOF

# Bring up the interface
sudo ifup can0

# Test - you should see CAN frames if battery is connected
candump can0
```

### RS485 Setup

```bash
# Check USB adapter is detected
ls -la /dev/ttyUSB*

# Test RS485 communication
./pylon_rs485_monitor.py --port /dev/ttyUSB0
```

## Configuration

Set environment variables (add to `~/.bashrc` or systemd service):

```bash
export MQTT_HOST=192.168.1.100    # Your MQTT broker IP
export MQTT_PORT=1883              # MQTT port (default: 1883)
export MQTT_USER=your_username     # MQTT username (optional)
export MQTT_PASS=your_password     # MQTT password (optional)
```

## Usage

### CAN Bus Bridge

Reads SOC, limits, and cell extremes from CAN bus:

```bash
./pylon_can2mqtt.py
```

### RS485 Monitor

Reads detailed cell-level data via RS485:

```bash
# Single read with console output
./pylon_rs485_monitor.py

# Continuous monitoring with MQTT publishing
./pylon_rs485_monitor.py --mqtt --loop

# Daemon mode (no console output)
./pylon_rs485_monitor.py --mqtt --loop --quiet --interval 30
```

Options:
- `--mqtt` - Enable MQTT publishing with Home Assistant discovery
- `--loop` - Continuous monitoring mode
- `--interval N` - Polling interval in seconds (default: 30)
- `--quiet` - Suppress console output
- `--port /dev/ttyUSB0` - RS485 serial port
- `--debug-log FILE` - Log balancing/OV/state changes to file (for debugging)

### Modbus-TCP Bridge (Inverter)

Reads Deye inverter data via Modbus-TCP gateway:

```bash
# Single read with console output
./deye_modbus2mqtt.py --host 192.168.200.111

# Continuous monitoring with MQTT publishing
./deye_modbus2mqtt.py --mqtt --loop

# Daemon mode with 10-second polling
./deye_modbus2mqtt.py --mqtt --loop --quiet --interval 10
```

Options:
- `--host` - Modbus-TCP gateway address (default: from MODBUS_HOST env)
- `--port` - Modbus-TCP port (default: 502)
- `--slave` - Modbus device ID (default: 1)
- `--mqtt` - Enable MQTT publishing with Home Assistant discovery
- `--loop` - Continuous monitoring mode
- `--interval N` - Fast poll interval in seconds (default: 10)
- `--quiet` - Suppress console output
- `--mqtt-prefix` - MQTT topic prefix (default: deye_inverter)
- `--device-id` - Home Assistant device identifier
- `--solarman-prefix` - Solarman unique_id prefix for history preservation
- `--solarman-serial` - Solarman inverter serial for history preservation

Environment variables:
- `MODBUS_HOST` - Modbus gateway address
- `MODBUS_PORT` - Modbus port (default: 502)
- `MODBUS_SLAVE` - Device ID (default: 1)
- `MQTT_PREFIX` - MQTT topic prefix
- `DEVICE_ID`, `DEVICE_NAME`, `DEVICE_MODEL`, `DEVICE_MANUFACTURER` - HA device info
- `SOLARMAN_PREFIX`, `SOLARMAN_SERIAL` - For preserving Solarman entity history

### Debug Tool

Decode raw CAN frames for protocol analysis:

```bash
./pylon_decode.py
```

## Home Assistant

Entities appear automatically under three devices:

| Device | Sensors |
|--------|---------|
| **Deye BMS (CAN)** | SOC, SOH, charge/discharge limits, cell min/max, temperatures, flags |
| **Deye BMS (RS485)** | Per-battery: 16 cell voltages, 6 temps, SOC, cycles, balancing status. Stack: totals and alarms |
| **Deye Inverter** | PV power/voltage/current, battery power/SOC/temp, grid power/voltage, load power, temperatures, energy totals |

### Example Entities

- `sensor.bms_soc` - State of Charge
- `sensor.stack_cell_min` - Lowest cell voltage across all batteries
- `sensor.battery_0_cell_01` - Individual cell voltage
- `binary_sensor.stack_balancing_active` - Balancing indicator

## MQTT Topics

### CAN Bridge (`deye_bms/`)
```
deye_bms/status          # online/offline
deye_bms/soc             # State of charge (%)
deye_bms/soh             # State of health (%)
deye_bms/limit/v_charge_max
deye_bms/limit/i_charge
deye_bms/limit/i_discharge
deye_bms/ext/cell_v_min
deye_bms/ext/cell_v_max
deye_bms/ext/temp_min
deye_bms/ext/temp_max
```

### RS485 Monitor (`deye_bms/rs485/`)
```
deye_bms/rs485/status           # online/offline
deye_bms/rs485/stack/cell_min   # Stack-wide minimum cell voltage
deye_bms/rs485/stack/cell_max
deye_bms/rs485/stack/voltage
deye_bms/rs485/stack/current
deye_bms/rs485/stack/balancing_count
deye_bms/rs485/battery0/cell01  # Individual cell voltages
deye_bms/rs485/battery0/cell02
...
deye_bms/rs485/battery0/temp1   # Temperature sensors
deye_bms/rs485/battery0/soc
deye_bms/rs485/battery0/cycles
```

## Running as a Service

Ready-to-use systemd service files are provided in the `systemd/` directory. The services run as a dedicated `pylon` user for security.

### Initial Setup

```bash
# Create dedicated user with access to CAN and serial devices
sudo useradd -r -s /sbin/nologin pylon
sudo usermod -a -G dialout,can pylon

# Install to /opt (recommended for services)
sudo mkdir -p /opt/pylontech-bms-mqtt
sudo cp pylon_can2mqtt.py pylon_rs485_monitor.py /opt/pylontech-bms-mqtt/
sudo chown -R pylon:pylon /opt/pylontech-bms-mqtt

# Copy environment config and edit with your settings
sudo cp systemd/pylon-mqtt.env /etc/default/pylon-mqtt
sudo nano /etc/default/pylon-mqtt
```

### Install Services

```bash
# Install and enable CAN bridge service
sudo cp systemd/pylon-can2mqtt.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable pylon-can2mqtt
sudo systemctl start pylon-can2mqtt

# Install and enable RS485 monitor service
sudo cp systemd/pylon-rs485-mqtt.service /etc/systemd/system/
sudo systemctl enable pylon-rs485-mqtt
sudo systemctl start pylon-rs485-mqtt

# Check status
sudo systemctl status pylon-can2mqtt pylon-rs485-mqtt

# View logs
sudo journalctl -u pylon-can2mqtt -f
sudo journalctl -u pylon-rs485-mqtt -f
```

## Protocol Reference

### CAN Bus (500kbps)

| Arbitration ID | Content |
|----------------|---------|
| 0x351 | Voltage/current limits (charge max, charge current, discharge current, low voltage) |
| 0x355 | SOC/SOH percentages |
| 0x359 | Status flags (protection states) |
| 0x370 | Cell voltage extremes and temperatures |

### RS485 (9600 baud)

| Command | CID2 | Response |
|---------|------|----------|
| Analog Data | 0x42 | Cell voltages, temps, current, capacity, cycles |
| Alarm Info | 0x44 | Cell alarms, balancing flags, protection status |

## ESPHome (Optional)

The `esphome/` directory contains configuration for a Waveshare ESP32-S3-RS485-CAN board to replace the Pi-based bridge. See `esphome/README.md` for details.

## Troubleshooting

### CAN Bus Issues

**No CAN frames received (`candump can0` shows nothing)**
- Check wiring: CAN-H to CAN-H, CAN-L to CAN-L
- Verify 500kbps bitrate matches your BMS
- Try adding 120Ω termination resistor between CAN-H and CAN-L
- Ensure CAN interface is up: `ip link show can0`

**"Network is down" error**
```bash
sudo ip link set can0 up type can bitrate 500000
```

**Wrong data / garbage values**
- Confirm your BMS uses Pylontech protocol (not Modbus or other)
- Check byte order - some BMS variants use big-endian

### RS485 Issues

**No response from battery**
- Check wiring: A to A (or +), B to B (or -)
- Verify baud rate is 9600
- Try swapping A and B wires
- Check address: default is 2, some systems use 0 or 1

**Permission denied on /dev/ttyUSB0**
```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

**Multiple USB adapters - wrong port**
```bash
# Find the right one
dmesg | grep ttyUSB
# Or create udev rule for consistent naming
```

### MQTT Issues

**Connection refused**
- Verify MQTT broker is running: `systemctl status mosquitto`
- Check firewall allows port 1883
- Verify credentials in environment variables

**Entities not appearing in Home Assistant**
- Check MQTT integration is installed in HA
- Verify discovery prefix is `homeassistant` (default)
- Check HA logs for MQTT errors
- Try restarting the bridge to re-publish discovery

**Entities show "unavailable"**
- Bridge is not running or crashed - check `systemctl status`
- MQTT connection lost - check broker logs
- Run bridge manually to see error output

### General

**Check logs**
```bash
# Systemd service logs
sudo journalctl -u pylon-can2mqtt -f
sudo journalctl -u pylon-rs485-mqtt -f

# Run manually with debug output
./pylon_can2mqtt.py
./pylon_rs485_monitor.py --mqtt
```

## Contributing

Contributions welcome! Please:
1. Test with your hardware
2. Update documentation if adding features
3. Add your battery model to the compatibility list

## License

MIT License - feel free to use and modify.

## Acknowledgments

- Protocol reverse-engineered from Pylontech documentation and Deye/Shoto BMS behavior
- Tested with Shoto SDA10-48200 batteries

## See Also

- [Pylontech Protocol Documentation](https://github.com/search?q=pylontech+protocol)
- [Home Assistant MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
- [ESPHome CAN Bus](https://esphome.io/components/canbus.html)
