#!/usr/bin/env python3
"""
Deye Inverter Modbus-TCP to MQTT Bridge
Polls Deye SG04LP3 (and compatible) inverters via Modbus-TCP and publishes to MQTT
with Home Assistant auto-discovery.

Tested with: Deye SUN-12K-SG04LP3-EU via Waveshare Modbus-TCP gateway

Usage:
    ./deye_modbus2mqtt.py                    # Single poll
    ./deye_modbus2mqtt.py --loop             # Continuous monitoring
    ./deye_modbus2mqtt.py --loop --quiet     # Daemon mode
"""

import sys
import os
import time
import json
import argparse
import signal
import logging
from dataclasses import dataclass
from typing import Optional, List, Dict, Any

try:
    from pymodbus.client import ModbusTcpClient
except ImportError:
    from pymodbus.client.sync import ModbusTcpClient

# Logging setup
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)

# Configuration - all values can be overridden via environment variables
MODBUS_HOST = os.environ.get("MODBUS_HOST", "192.168.200.111")
MODBUS_PORT = int(os.environ.get("MODBUS_PORT", "502"))
MODBUS_SLAVE = int(os.environ.get("MODBUS_SLAVE", "1"))

# MQTT settings
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER")
MQTT_PASS = os.environ.get("MQTT_PASS")
MQTT_PREFIX = os.environ.get("MQTT_PREFIX", "deye_inverter")
AVAIL_TOPIC = f"{MQTT_PREFIX}/status"

# Home Assistant Discovery
DISCOVERY_PREFIX = os.environ.get("DISCOVERY_PREFIX", "homeassistant")
DEVICE_ID = os.environ.get("DEVICE_ID", "deye_inverter")
DEVICE_NAME = os.environ.get("DEVICE_NAME", "Deye Inverter")
DEVICE_MODEL = os.environ.get("DEVICE_MODEL", "SUN-12K-SG04LP3-EU")
DEVICE_MANUFACTURER = os.environ.get("DEVICE_MANUFACTURER", "Deye")

# Legacy unique_id prefixes for preserving HA entity history
# Format: prefix_serial_SensorName (e.g., "deye_2957831690_Battery SOC")
SOLARMAN_PREFIX = os.environ.get("SOLARMAN_PREFIX", "")  # e.g., "deye"
SOLARMAN_SERIAL = os.environ.get("SOLARMAN_SERIAL", "")  # e.g., "2957831690"

# Mapping from our register names to Solarman sensor names
# Used to generate Solarman-compatible unique_ids for history preservation
SOLARMAN_NAME_MAP = {
    # Solar/PV
    "pv1_power": "PV1 Power",
    "pv2_power": "PV2 Power",
    "pv1_voltage": "PV1 Voltage",
    "pv2_voltage": "PV2 Voltage",
    "pv1_current": "PV1 Current",
    "pv2_current": "PV2 Current",
    "daily_production": "Daily Production",
    "total_production": "Total Production",
    # Battery
    "battery_temperature": "Battery Temperature",
    "battery_voltage": "Battery Voltage",
    "battery_soc": "Battery SOC",
    "battery_power": "Battery Power",
    "battery_current": "Battery Current",
    "daily_battery_charge": "Daily Battery Charge",
    "daily_battery_discharge": "Daily Battery Discharge",
    "total_battery_charge": "Total Battery Charge",
    "total_battery_discharge": "Total Battery Discharge",
    # Grid
    "grid_voltage_l1": "Grid Voltage L1",
    "grid_voltage_l2": "Grid Voltage L2",
    "grid_voltage_l3": "Grid Voltage L3",
    "grid_frequency": "Grid Frequency",
    "total_grid_power": "Total Grid Power",
    "grid_power_ct_l1": "Grid CT L1 Power",
    "grid_power_ct_l2": "Grid CT L2 Power",
    "grid_power_ct_l3": "Grid CT L3 Power",
    "grid_power_ext_ct_l1": "External CT L1 Power",
    "grid_power_ext_ct_l2": "External CT L2 Power",
    "grid_power_ext_ct_l3": "External CT L3 Power",
    "daily_energy_bought": "Daily Energy Bought",
    "daily_energy_sold": "Daily Energy Sold",
    "total_energy_bought": "Total Energy Bought",
    "total_energy_sold": "Total Energy Sold",
    # Load
    "total_load_power": "Total Load Power",
    "load_power_l1": "Load L1 Power",
    "load_power_l2": "Load L2 Power",
    "load_power_l3": "Load L3 Power",
    "load_voltage_l1": "Load Voltage L1",
    "load_voltage_l2": "Load Voltage L2",
    "load_voltage_l3": "Load Voltage L3",
    "daily_load_consumption": "Daily Load Consumption",
    "total_load_consumption": "Total Load Consumption",
    # Inverter
    "inverter_current_l1": "Inverter L1 Current",
    "inverter_current_l2": "Inverter L2 Current",
    "inverter_current_l3": "Inverter L3 Current",
    "inverter_power_l1": "Inverter L1 Power",
    "inverter_power_l2": "Inverter L2 Power",
    "inverter_power_l3": "Inverter L3 Power",
    "inverter_frequency": "Inverter Frequency",
    # Temperatures
    "dc_temperature": "DC Temperature",
    "ac_temperature": "AC Temperature",
}

# Rate limiting
FORCE_PUBLISH_INTERVAL_S = 60
MIN_INTERVAL_S = 1.0

# Global state
_mqtt_client = None
_running = True


@dataclass
class Register:
    """Modbus register definition."""
    address: int
    name: str
    unit: Optional[str] = None
    scale: float = 1.0
    offset: float = 0.0  # Added for temperature sensors (raw * scale + offset)
    data_type: str = "int16"  # int16, uint16, int32, uint32
    device_class: Optional[str] = None
    state_class: Optional[str] = None
    icon: Optional[str] = None
    scan_group: str = "normal"  # fast (10s), normal (30s), slow (60s)
    # For preserving existing HA entity unique_ids
    legacy_unique_id: Optional[str] = None


# Helper to create registers with proper defaults
def R(addr, name, unit=None, scale=1.0, offset=0.0, dtype="int16", dclass=None, sclass=None,
      icon=None, group="normal", legacy_id=None):
    return Register(addr, name, unit, scale, offset, dtype, dclass, sclass, icon, group, legacy_id)


# Register definitions for Deye SG04LP3
# Addresses are decimal (converted from hex in comments)
REGISTERS: List[Register] = [
    # === Solar/PV Parameters ===
    R(672, "pv1_power", "W", 1, 0, "int16", "power", "measurement", None, "fast", "deye-tcp-pv1-power"),
    R(673, "pv2_power", "W", 1, 0, "int16", "power", "measurement", None, "fast", "deye-tcp-pv2-power"),
    R(676, "pv1_voltage", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(678, "pv2_voltage", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(677, "pv1_current", "A", 0.1, 0, "uint16", "current", "measurement"),
    R(679, "pv2_current", "A", 0.1, 0, "uint16", "current", "measurement"),
    R(529, "daily_production", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(534, "total_production", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),

    # === Battery Parameters ===
    R(99, "battery_equalization_voltage", "V", 0.01, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(100, "battery_absorption_voltage", "V", 0.01, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(101, "battery_float_voltage", "V", 0.01, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(102, "battery_capacity_setting", "Ah", 1, 0, "uint16", None, "measurement", "mdi:battery", "slow"),
    R(108, "battery_max_charge_current", "A", 1, 0, "uint16", "current", "measurement", None, "slow"),
    R(109, "battery_max_discharge_current", "A", 1, 0, "uint16", "current", "measurement", None, "slow"),
    R(514, "daily_battery_charge", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(515, "daily_battery_discharge", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(516, "total_battery_charge", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),
    R(518, "total_battery_discharge", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),
    R(586, "battery_temperature", "°C", 0.1, -100, "int16", "temperature", "measurement"),
    R(587, "battery_voltage", "V", 0.01, 0, "uint16", "voltage", "measurement"),
    R(588, "battery_soc", "%", 1, 0, "uint16", "battery", "measurement", None, "normal", "deye-tcp-battery-soc"),
    R(590, "battery_power", "W", 1, 0, "int16", "power", "measurement", None, "fast", "deye-tcp-battery-power"),
    R(591, "battery_current", "A", 0.01, 0, "int16", "current", "measurement"),

    # === Grid Parameters ===
    R(598, "grid_voltage_l1", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(599, "grid_voltage_l2", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(600, "grid_voltage_l3", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(638, "grid_frequency", "Hz", 0.01, 0, "uint16", "frequency", "measurement", None, "fast", "deye-tcp-grid-frequency1"),
    R(625, "total_grid_power", "W", 1, 0, "int16", "power", "measurement", None, "fast", "deye-tcp-total-grid-power"),
    # Internal CT power per phase
    R(604, "grid_power_ct_l1", "W", 1, 0, "int16", "power", "measurement"),
    R(605, "grid_power_ct_l2", "W", 1, 0, "int16", "power", "measurement"),
    R(606, "grid_power_ct_l3", "W", 1, 0, "int16", "power", "measurement"),
    # External CT power per phase (if installed)
    R(616, "grid_power_ext_ct_l1", "W", 1, 0, "int16", "power", "measurement"),
    R(617, "grid_power_ext_ct_l2", "W", 1, 0, "int16", "power", "measurement"),
    R(618, "grid_power_ext_ct_l3", "W", 1, 0, "int16", "power", "measurement"),
    # Grid energy
    R(520, "daily_energy_bought", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(522, "total_energy_bought", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),
    R(521, "daily_energy_sold", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(524, "total_energy_sold", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),

    # === Load Parameters ===
    R(653, "total_load_power", "W", 1, 0, "int16", "power", "measurement", None, "fast", "deye-tcp-total-load-power"),
    R(650, "load_power_l1", "W", 1, 0, "int16", "power", "measurement"),
    R(651, "load_power_l2", "W", 1, 0, "int16", "power", "measurement"),
    R(652, "load_power_l3", "W", 1, 0, "int16", "power", "measurement"),
    R(644, "load_voltage_l1", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(645, "load_voltage_l2", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(646, "load_voltage_l3", "V", 0.1, 0, "uint16", "voltage", "measurement"),
    R(526, "daily_load_consumption", "kWh", 0.1, 0, "uint16", "energy", "total_increasing"),
    R(527, "total_load_consumption", "kWh", 0.1, 0, "uint32", "energy", "total_increasing", None, "slow"),

    # === Inverter Output ===
    R(630, "inverter_current_l1", "A", 0.01, 0, "int16", "current", "measurement"),
    R(631, "inverter_current_l2", "A", 0.01, 0, "int16", "current", "measurement"),
    R(632, "inverter_current_l3", "A", 0.01, 0, "int16", "current", "measurement"),
    R(633, "inverter_power_l1", "W", 1, 0, "int16", "power", "measurement"),
    R(634, "inverter_power_l2", "W", 1, 0, "int16", "power", "measurement"),
    R(635, "inverter_power_l3", "W", 1, 0, "int16", "power", "measurement"),
    R(636, "inverter_frequency", "Hz", 0.01, 0, "uint16", "frequency", "measurement"),

    # === Temperatures ===
    R(540, "dc_temperature", "°C", 0.1, -100, "int16", "temperature", "measurement"),
    R(541, "ac_temperature", "°C", 0.1, -100, "int16", "temperature", "measurement"),

    # === BMS Communication (limits received from BMS via CAN) ===
    R(212, "bms_charge_current_limit", "A", 1, 0, "uint16", "current", "measurement", None, "normal", "deye-tcp-bms-charge-current"),
    R(213, "bms_discharge_current_limit", "A", 1, 0, "uint16", "current", "measurement", None, "normal", "deye-tcp-bms-discharge-current"),

    # === Settings (read-only monitoring) ===
    R(143, "max_sell_power", "W", 1, 0, "uint16", "power", "measurement", None, "slow", "deye-tcp-max-sell-power"),
    R(142, "sell_mode_enabled", None, 1, 0, "uint16", None, None, "mdi:transmission-tower-export", "slow"),

    # === Generator Port (if used) ===
    R(661, "gen_voltage_l1", "V", 0.1, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(662, "gen_voltage_l2", "V", 0.1, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(663, "gen_voltage_l3", "V", 0.1, 0, "uint16", "voltage", "measurement", None, "slow"),
    R(664, "gen_power_l1", "W", 1, 0, "int16", "power", "measurement", None, "slow"),
    R(665, "gen_power_l2", "W", 1, 0, "int16", "power", "measurement", None, "slow"),
    R(666, "gen_power_l3", "W", 1, 0, "int16", "power", "measurement", None, "slow"),
    R(667, "gen_total_power", "W", 1, 0, "int16", "power", "measurement", None, "slow"),

    # === Status/Alerts ===
    R(552, "running_status", None, 1, 0, "uint16", None, None, "mdi:state-machine"),
    R(553, "alert_code_1", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
    R(554, "alert_code_2", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
    R(555, "alert_code_3", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
    R(556, "alert_code_4", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
    R(557, "alert_code_5", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
    R(558, "alert_code_6", None, 1, 0, "uint16", None, None, "mdi:alert", "slow"),
]


class Publisher:
    """Publishes MQTT topics with rate limiting."""

    def __init__(self, client):
        self.client = client
        self.last_value: Dict[str, Any] = {}
        self.last_ts: Dict[str, float] = {}

    def publish(self, topic: str, value, retain=False, min_interval=MIN_INTERVAL_S):
        full_topic = f"{MQTT_PREFIX}/{topic}"
        now = time.time()

        prev_val = self.last_value.get(full_topic)
        prev_ts = self.last_ts.get(full_topic, 0)

        if (now - prev_ts) < min_interval:
            return False

        force_due = (now - prev_ts) >= FORCE_PUBLISH_INTERVAL_S

        if isinstance(value, (int, float)):
            store_val = float(value)
            payload = str(value)
        else:
            store_val = str(value)
            payload = str(value)

        should_pub = (prev_val != store_val) or force_due

        if not should_pub:
            return False

        try:
            self.client.publish(full_topic, payload, retain=retain)
        except Exception:
            return False

        self.last_value[full_topic] = store_val
        self.last_ts[full_topic] = now
        return True


def ha_sensor_config(reg: Register) -> dict:
    """Build HA MQTT Discovery payload for a register."""
    # Determine unique_id with priority:
    # 1. Explicit legacy_unique_id (e.g., "deye-tcp-battery-soc")
    # 2. Solarman format if prefix/serial configured (e.g., "deye_2957831690_Battery SOC")
    # 3. Default format (device_id_name)
    if reg.legacy_unique_id:
        unique_id = reg.legacy_unique_id
    elif SOLARMAN_PREFIX and SOLARMAN_SERIAL and reg.name in SOLARMAN_NAME_MAP:
        solarman_name = SOLARMAN_NAME_MAP[reg.name]
        unique_id = f"{SOLARMAN_PREFIX}_{SOLARMAN_SERIAL}_{solarman_name}"
    else:
        unique_id = f"{DEVICE_ID}_{reg.name}"

    cfg = {
        "name": reg.name.replace("_", " ").title(),
        "state_topic": f"{MQTT_PREFIX}/{reg.name}",
        "unique_id": unique_id,
        "availability_topic": AVAIL_TOPIC,
        "payload_available": "online",
        "payload_not_available": "offline",
        "device": {
            "identifiers": [DEVICE_ID],
            "name": DEVICE_NAME,
            "manufacturer": DEVICE_MANUFACTURER,
            "model": DEVICE_MODEL,
        },
    }

    if reg.unit:
        cfg["unit_of_measurement"] = reg.unit
    if reg.device_class:
        cfg["device_class"] = reg.device_class
    if reg.state_class:
        cfg["state_class"] = reg.state_class
    if reg.icon:
        cfg["icon"] = reg.icon

    # Set precision based on scale
    if reg.scale < 1:
        if reg.scale <= 0.01:
            cfg["suggested_display_precision"] = 2
        else:
            cfg["suggested_display_precision"] = 1

    return cfg


def publish_discovery(client):
    """Publish HA MQTT Discovery configs for all registers."""
    logging.info("Publishing HA discovery configs...")

    for reg in REGISTERS:
        cfg = ha_sensor_config(reg)
        cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{reg.name}/config"
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

    client.publish(AVAIL_TOPIC, "online", retain=True)
    logging.info("HA discovery published for %d registers", len(REGISTERS))


def read_register(client: ModbusTcpClient, reg: Register, device_id: int) -> Optional[float]:
    """Read a single register and return scaled value."""
    try:
        if reg.data_type in ("int32", "uint32"):
            result = client.read_holding_registers(reg.address, count=2, device_id=device_id)
        else:
            result = client.read_holding_registers(reg.address, count=1, device_id=device_id)

        if result.isError():
            return None

        regs = result.registers

        if reg.data_type == "int16":
            raw = regs[0]
            if raw > 0x7FFF:
                raw -= 0x10000
        elif reg.data_type == "uint16":
            raw = regs[0]
        elif reg.data_type == "int32":
            # LSB first (little-endian word order)
            raw = regs[0] | (regs[1] << 16)
            if raw > 0x7FFFFFFF:
                raw -= 0x100000000
        elif reg.data_type == "uint32":
            # LSB first (little-endian word order)
            raw = regs[0] | (regs[1] << 16)
        else:
            raw = regs[0]

        return round(raw * reg.scale + reg.offset, 3)

    except Exception as e:
        logging.debug("Error reading %s: %s", reg.name, e)
        return None


def poll_registers(modbus_client: ModbusTcpClient, device_id: int,
                   scan_groups: List[str] = None) -> Dict[str, Any]:
    """Poll all registers (or specific scan groups) and return values."""
    data = {
        "timestamp": time.strftime('%Y-%m-%d %H:%M:%S'),
        "values": {}
    }

    for reg in REGISTERS:
        if scan_groups and reg.scan_group not in scan_groups:
            continue

        value = read_register(modbus_client, reg, device_id)
        if value is not None:
            data["values"][reg.name] = value

    return data


def print_report(data: Dict[str, Any]):
    """Print human-readable report."""
    print("=" * 70)
    print(f"DEYE INVERTER - {data['timestamp']}")
    print("=" * 70)

    # Group values by category
    categories = {
        "Solar": ["pv1", "pv2", "daily_production", "total_production"],
        "Battery": ["battery", "bms", "daily_battery", "total_battery"],
        "Grid": ["grid", "daily_energy", "total_energy"],
        "Load": ["load", "daily_load", "total_load"],
        "Inverter": ["inverter", "dc_temp", "ac_temp"],
        "Other": []
    }

    printed = set()

    for cat_name, prefixes in categories.items():
        cat_values = []
        for name, value in data["values"].items():
            if any(name.startswith(p) or p in name for p in prefixes):
                cat_values.append((name, value))
                printed.add(name)

        if cat_values:
            print(f"\n▸ {cat_name}")
            for name, value in sorted(cat_values):
                # Find the register for units
                reg = next((r for r in REGISTERS if r.name == name), None)
                unit = reg.unit if reg else ""
                print(f"    {name}: {value} {unit}")

    # Print any uncategorized values
    other = [(n, v) for n, v in data["values"].items() if n not in printed]
    if other:
        print(f"\n▸ Other")
        for name, value in sorted(other):
            reg = next((r for r in REGISTERS if r.name == name), None)
            unit = reg.unit if reg else ""
            print(f"    {name}: {value} {unit}")

    print("=" * 70)


def publish_mqtt_data(pub: Publisher, data: Dict[str, Any]):
    """Publish all values to MQTT."""
    for name, value in data["values"].items():
        reg = next((r for r in REGISTERS if r.name == name), None)
        min_interval = {"fast": 5, "normal": 15, "slow": 30}.get(
            reg.scan_group if reg else "normal", 15)
        pub.publish(name, value, min_interval=min_interval)


def shutdown(signum=None, frame=None):
    """Graceful shutdown."""
    global _running
    _running = False
    sig_name = signal.Signals(signum).name if signum else "unknown"
    logging.info("Shutdown requested (signal %s)", sig_name)

    if _mqtt_client is not None:
        try:
            _mqtt_client.publish(AVAIL_TOPIC, "offline", retain=True)
            _mqtt_client.disconnect()
        except Exception:
            pass

    sys.exit(0)


def main():
    global _mqtt_client, _running

    parser = argparse.ArgumentParser(
        description='Deye Inverter Modbus-TCP to MQTT Bridge',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Environment variables (can also be set via command line):
  MODBUS_HOST, MODBUS_PORT, MODBUS_SLAVE
  MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, MQTT_PREFIX
  DEVICE_ID, DEVICE_NAME, DEVICE_MODEL, DEVICE_MANUFACTURER
  SOLARMAN_PREFIX, SOLARMAN_SERIAL (for preserving Solarman entity history)

Examples:
  # Basic usage
  ./deye_modbus2mqtt.py --host 192.168.1.100 --mqtt --loop

  # Preserve Solarman entity history
  ./deye_modbus2mqtt.py --mqtt --loop --solarman-prefix deye --solarman-serial 2957831690
        """)
    # Modbus settings
    parser.add_argument('--host', default=MODBUS_HOST, help='Modbus TCP host')
    parser.add_argument('--port', type=int, default=MODBUS_PORT, help='Modbus TCP port')
    parser.add_argument('--slave', type=int, default=MODBUS_SLAVE, help='Modbus slave ID')
    # Operation mode
    parser.add_argument('--loop', action='store_true', help='Continuous monitoring')
    parser.add_argument('--interval', type=int, default=10, help='Fast poll interval (seconds)')
    parser.add_argument('--json', action='store_true', help='JSON output')
    parser.add_argument('--mqtt', action='store_true', help='Publish to MQTT')
    parser.add_argument('--quiet', action='store_true', help='Suppress console output')
    parser.add_argument('--test', action='store_true',
                        help='Test mode: use separate device/topics to run alongside existing integration')
    # Device/MQTT customization
    parser.add_argument('--mqtt-prefix', default=None, help='MQTT topic prefix (default: deye_inverter)')
    parser.add_argument('--device-id', default=None, help='HA device identifier')
    parser.add_argument('--device-name', default=None, help='HA device display name')
    parser.add_argument('--device-model', default=None, help='HA device model')
    parser.add_argument('--device-manufacturer', default=None, help='HA device manufacturer')
    # Solarman compatibility
    parser.add_argument('--solarman-prefix', default=None,
                        help='Solarman unique_id prefix (e.g., "deye") for history preservation')
    parser.add_argument('--solarman-serial', default=None,
                        help='Solarman inverter serial (e.g., "2957831690") for history preservation')
    args = parser.parse_args()

    # Apply command-line overrides to globals
    global MQTT_PREFIX, AVAIL_TOPIC, DEVICE_ID, DEVICE_NAME, DEVICE_MODEL, DEVICE_MANUFACTURER
    global SOLARMAN_PREFIX, SOLARMAN_SERIAL

    if args.mqtt_prefix:
        MQTT_PREFIX = args.mqtt_prefix
        AVAIL_TOPIC = f"{MQTT_PREFIX}/status"
    if args.device_id:
        DEVICE_ID = args.device_id
    if args.device_name:
        DEVICE_NAME = args.device_name
    if args.device_model:
        DEVICE_MODEL = args.device_model
    if args.device_manufacturer:
        DEVICE_MANUFACTURER = args.device_manufacturer
    if args.solarman_prefix:
        SOLARMAN_PREFIX = args.solarman_prefix
    if args.solarman_serial:
        SOLARMAN_SERIAL = args.solarman_serial

    # Test mode: use different prefix/device to avoid conflicts
    if args.test:
        MQTT_PREFIX = "deye_inverter_test"
        AVAIL_TOPIC = f"{MQTT_PREFIX}/status"
        DEVICE_ID = "deye_inverter_test"
        DEVICE_NAME = "Deye Inverter (TEST)"
        logging.info("TEST MODE: Using prefix '%s' - will create separate HA device", MQTT_PREFIX)

    # Connect to Modbus
    modbus = ModbusTcpClient(args.host, port=args.port)
    if not modbus.connect():
        logging.error("Failed to connect to Modbus at %s:%d", args.host, args.port)
        sys.exit(1)
    logging.info("Connected to Modbus at %s:%d", args.host, args.port)

    pub = None
    if args.mqtt:
        import paho.mqtt.client as mqtt

        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:
            client = mqtt.Client()

        def on_connect(client, userdata, flags, rc, properties=None):
            rc_val = rc.value if hasattr(rc, 'value') else rc
            if rc_val == 0:
                logging.info("Connected to MQTT broker %s:%d", MQTT_HOST, MQTT_PORT)
                publish_discovery(client)
            else:
                logging.error("MQTT connection failed: %s", rc)

        def on_disconnect(client, userdata, rc, properties=None):
            logging.warning("MQTT disconnected (rc=%s)", rc)

        client.on_connect = on_connect
        client.on_disconnect = on_disconnect
        client.reconnect_delay_set(min_delay=1, max_delay=60)

        if MQTT_USER and MQTT_PASS:
            client.username_pw_set(MQTT_USER, MQTT_PASS)

        client.will_set(AVAIL_TOPIC, payload="offline", qos=0, retain=True)

        try:
            client.connect(MQTT_HOST, MQTT_PORT, 60)
        except Exception as e:
            logging.error("Failed to connect to MQTT: %s", e)
            sys.exit(1)

        client.loop_start()
        _mqtt_client = client
        pub = Publisher(client)

        signal.signal(signal.SIGTERM, shutdown)
        signal.signal(signal.SIGINT, shutdown)

        logging.info("Modbus->MQTT bridge started")

    # Polling counters for different scan groups
    poll_count = 0

    while _running:
        try:
            # Determine which groups to poll this cycle
            # Fast: every cycle, Normal: every 3rd, Slow: every 6th
            if poll_count % 6 == 0:
                groups = ["fast", "normal", "slow"]
            elif poll_count % 3 == 0:
                groups = ["fast", "normal"]
            else:
                groups = ["fast"]

            data = poll_registers(modbus, args.slave, groups)

            if args.json:
                print(json.dumps(data, indent=2))
            elif not args.quiet:
                # Only print full report on slow cycles
                if "slow" in groups:
                    print_report(data)
                else:
                    # Quick status line
                    v = data["values"]
                    pv = v.get("pv1_power", 0) + v.get("pv2_power", 0)
                    batt = v.get("battery_power", 0)
                    grid = v.get("total_grid_power", 0)
                    load = v.get("total_load_power", 0)
                    soc = v.get("battery_soc", 0)
                    print(f"[{data['timestamp']}] PV:{pv:5.0f}W | Batt:{batt:+6.0f}W ({soc:.0f}%) | Grid:{grid:+6.0f}W | Load:{load:5.0f}W")

            if args.mqtt and pub:
                publish_mqtt_data(pub, data)

            if not args.loop:
                break

            poll_count += 1
            time.sleep(args.interval)

        except KeyboardInterrupt:
            logging.info("Stopped by user")
            break
        except Exception as e:
            logging.exception("Error: %s", e)
            if not args.loop:
                break
            time.sleep(5)

    # Cleanup
    modbus.close()
    if args.mqtt and _mqtt_client:
        try:
            time.sleep(0.5)
            _mqtt_client.publish(AVAIL_TOPIC, "offline", retain=True)
            time.sleep(0.2)
            _mqtt_client.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    main()
