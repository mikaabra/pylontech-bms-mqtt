# Pylontech BMS to MQTT Bridge

A Python-based bridge that reads battery data from Deye/Shoto BMS systems using the Pylontech protocol and publishes to MQTT with Home Assistant auto-discovery.

Supports both **CAN bus** and **RS485** interfaces for comprehensive battery monitoring.

## Features

- **CAN Bus Monitoring**: Real-time SOC, SOH, voltage/current limits, cell min/max, temperatures
- **RS485 Monitoring**: Individual cell voltages (48 cells), temperatures, balancing status, alarms, cycle counts
- **Home Assistant Integration**: Auto-discovery for all sensors - entities appear automatically
- **Robust Operation**: Auto-reconnect, Last Will Testament for availability tracking, rate limiting with hysteresis
- **Parallel Battery Support**: Tested with 3× 16S LFP batteries in parallel configuration

## Hardware Requirements

- Raspberry Pi (or similar Linux SBC)
- CAN bus interface (e.g., Waveshare RS485 CAN HAT, MCP2515 module)
- RS485 USB adapter (e.g., FTDI-based USB-RS485)
- Deye/Shoto battery with Pylontech-compatible BMS

### Wiring

| Interface | BMS Connection |
|-----------|----------------|
| CAN-H     | CAN-H (typically pin 1) |
| CAN-L     | CAN-L (typically pin 2) |
| RS485-A   | RS485-A/+ |
| RS485-B   | RS485-B/- |
| GND       | GND |

## Installation

```bash
# Clone the repository
git clone https://github.com/mikaabra/pylontech-bms-mqtt.git
cd pylontech-bms-mqtt

# Install Python dependencies
pip3 install python-can paho-mqtt pyserial

# Configure CAN interface (add to /etc/network/interfaces.d/can0)
# auto can0
# iface can0 inet manual
#     pre-up /sbin/ip link set can0 type can bitrate 500000
#     up /sbin/ifconfig can0 up
#     down /sbin/ifconfig can0 down
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

### Debug Tool

Decode raw CAN frames for protocol analysis:

```bash
./pylon_decode.py
```

## Home Assistant

Entities appear automatically under two devices:

| Device | Sensors |
|--------|---------|
| **Deye BMS (CAN)** | SOC, SOH, charge/discharge limits, cell min/max, temperatures, flags |
| **Deye BMS (RS485)** | Per-battery: 16 cell voltages, 6 temps, SOC, cycles, balancing status. Stack: totals and alarms |

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

Create systemd service files for automatic startup:

```bash
# /etc/systemd/system/pylon-can2mqtt.service
[Unit]
Description=Pylontech CAN to MQTT Bridge
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root/pylontech-bms-mqtt
Environment="MQTT_HOST=192.168.1.100"
Environment="MQTT_USER=your_user"
Environment="MQTT_PASS=your_pass"
ExecStart=/root/pylontech-bms-mqtt/pylon_can2mqtt.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable pylon-can2mqtt
sudo systemctl start pylon-can2mqtt
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

## License

MIT License - feel free to use and modify.

## Acknowledgments

- Protocol reverse-engineered from Pylontech documentation and Deye/Shoto BMS behavior
- Tested with Shoto SDA10-48200 batteries
