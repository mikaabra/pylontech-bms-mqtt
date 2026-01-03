#!/usr/bin/env python3
"""
Pylontech RS485 Battery Monitor
Reads individual cell voltages and alarm/balancing status from all batteries via RS485.
Publishes to MQTT with Home Assistant auto-discovery.

Protocol: Pylontech RS485 @ 9600 baud
Tested with: Shoto/Pylontech-compatible 16S LFP batteries

Usage:
    ./pylon_rs485_monitor.py                    # Single read
    ./pylon_rs485_monitor.py --loop             # Continuous monitoring
    ./pylon_rs485_monitor.py --json             # JSON output
    ./pylon_rs485_monitor.py --mqtt             # Publish to MQTT with HA discovery
"""

import sys
import os
import time
import json
import argparse
import signal
import logging
import serial

# -----------------------------
# Logging setup
# -----------------------------
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)

# Configuration
RS485_PORT = "/dev/ttyUSB0"
RS485_BAUD = 9600
PYLONTECH_ADDR = 2  # Battery stack address
NUM_BATTERIES = 3   # Number of batteries in stack (0, 1, 2)

# MQTT settings (from environment variables)
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER")
MQTT_PASS = os.environ.get("MQTT_PASS")
MQTT_PREFIX = "deye_bms/rs485"
AVAIL_TOPIC = f"{MQTT_PREFIX}/status"

# Home Assistant Discovery
DISCOVERY_PREFIX = "homeassistant"
DEVICE_ID = "deye_bms_rs485"
DEVICE_NAME = "Deye BMS (RS485)"
DEVICE_MODEL = "Pylontech RS485 Protocol"
DEVICE_MANUFACTURER = "Shoto"

# Rate limiting
FORCE_PUBLISH_INTERVAL_S = 60
MIN_INTERVAL_S_DEFAULT = 1.0
MIN_INTERVAL_S_CELLS = 5.0
VOLT_HYST_V = 0.002  # 2mV hysteresis for cell voltages

# Global state for signal handlers
_mqtt_client = None
_running = True

# Debug log state tracking (for --debug-log)
_debug_log_file = None
_prev_state = {}  # {batt_id: {'balancing': set(), 'overvolt': set(), 'state': str}}


def calc_chksum(frame_content: str) -> str:
    """Calculate Pylontech frame checksum."""
    total = sum(ord(c) for c in frame_content)
    return f"{(~total + 1) & 0xFFFF:04X}"


def make_command(addr: int, cid2: int, info: str = "") -> bytes:
    """Build a Pylontech command frame."""
    ver, cid1 = "20", "46"
    adr = f"{addr:02X}"
    cid2_hex = f"{cid2:02X}"
    len_hex = f"{len(info):03X}"
    lchksum = (~sum(int(c, 16) for c in len_hex) + 1) & 0xF
    lenid = f"{lchksum:X}{len_hex}"
    frame = f"{ver}{adr}{cid1}{cid2_hex}{lenid}{info}"
    return f"~{frame}{calc_chksum(frame)}\r".encode('ascii')


def make_analog_cmd(addr: int, batt_num: int) -> bytes:
    """Build analog data request command (CID2=0x42)."""
    return make_command(addr, 0x42, f"{batt_num:02X}")


def make_alarm_cmd(addr: int, batt_num: int) -> bytes:
    """Build alarm info request command (CID2=0x44)."""
    return make_command(addr, 0x44, f"{batt_num:02X}")


def decode_analog_response(data_hex: str) -> dict:
    """Decode analog value response."""
    i = 0
    result = {}

    # Header (4 chars)
    result['header'] = data_hex[i:i+4]
    i += 4

    # Number of cells
    if i + 2 > len(data_hex):
        return result
    num_cells = int(data_hex[i:i+2], 16)
    i += 2

    # Cell voltages
    cells = []
    for _ in range(num_cells):
        if i + 4 <= len(data_hex):
            cells.append(int(data_hex[i:i+4], 16) / 1000.0)
            i += 4
    result['cells'] = cells

    # Temperature count and values
    if i + 2 <= len(data_hex):
        num_temps = int(data_hex[i:i+2], 16)
        i += 2
        temps = []
        for _ in range(num_temps):
            if i + 4 <= len(data_hex):
                raw = int(data_hex[i:i+4], 16)
                temps.append(round((raw - 2731) / 10.0, 1))
                i += 4
        result['temps'] = temps

    # Current (signed, 10mA units)
    if i + 4 <= len(data_hex):
        raw = int(data_hex[i:i+4], 16)
        if raw > 0x7FFF:
            raw -= 0x10000
        result['current'] = raw / 100.0
        i += 4

    # Voltage (10mV units = centivolts)
    if i + 4 <= len(data_hex):
        result['voltage'] = int(data_hex[i:i+4], 16) / 100.0
        i += 4

    # Remaining capacity (10mAh)
    if i + 4 <= len(data_hex):
        result['remain_ah'] = int(data_hex[i:i+4], 16) / 100.0
        i += 4

    # Custom byte (skip)
    if i + 2 <= len(data_hex):
        i += 2

    # Total capacity (10mAh)
    if i + 4 <= len(data_hex):
        result['total_ah'] = int(data_hex[i:i+4], 16) / 100.0
        i += 4

    # Cycle count
    if i + 4 <= len(data_hex):
        result['cycles'] = int(data_hex[i:i+4], 16)

    return result


def decode_alarm_response(data_hex: str) -> dict:
    """Decode alarm info response (CID2=0x44).

    Returns balancing status, cell/temp status, and protection flags.

    Response structure:
    - [0:2] info_flag, [2:4] battery, [4:6] num_cells
    - [6:6+num_cells*2] cell status bytes (0x00=normal, 0x01=under, 0x02=over)
    - [next 2] num_temps
    - [next num_temps*2] temp status bytes
    - [remaining] status bytes:
      - ByteIndex 0: bit0=Balance On, bit1=Static Balance, bit2=Static Balance Timeout
      - ByteIndex 4: voltage flags (bit0=CellOV_Alarm, bit1=CellOV_Protect, etc.)
      - ByteIndex 9: Balance1-8 (bit0=cell1, bit1=cell2, ... bit7=cell8)
      - ByteIndex 10: Balance9-16 (bit0=cell9, ... bit7=cell16)
    """
    result = {
        'balancing_cells': [],
        'overvolt_cells': [],
        'undervolt_cells': [],
        'overtemp_sensors': [],
        'undertemp_sensors': [],
        'warnings': [],      # Informational (normal at 100% SOC)
        'protections': [],   # Active protection events
        'alarms': [],        # Only real problems (protections)
        'status': {},
        'debug': {}  # Raw data for debugging
    }

    if len(data_hex) < 10:
        return result

    # Byte positions
    # [0:2] info_flag, [2:4] battery, [4:6] num_cells
    result['info_flag'] = int(data_hex[0:2], 16)
    result['battery_num'] = int(data_hex[2:4], 16)
    num_cells = int(data_hex[4:6], 16)
    result['num_cells'] = num_cells

    # Cell status bytes (1 byte per cell)
    # 0x00=normal, 0x01=below limit (undervolt), 0x02=above limit (overvolt)
    # Note: Balance flags are NOT here - they're in status bytes 9-10
    cell_start = 6
    for c in range(num_cells):
        pos = cell_start + c * 2
        if pos + 2 > len(data_hex):
            break
        status = int(data_hex[pos:pos+2], 16)
        # Note: 0x02 at 100% SOC is normal - cells at charge voltage threshold
        if status == 0x01:
            result['undervolt_cells'].append(c + 1)
        elif status == 0x02:
            result['overvolt_cells'].append(c + 1)

    # Temperature alarms
    temp_count_pos = cell_start + num_cells * 2
    if temp_count_pos + 2 <= len(data_hex):
        num_temps = int(data_hex[temp_count_pos:temp_count_pos+2], 16)
        result['num_temps'] = num_temps
        temp_start = temp_count_pos + 2
        for t in range(num_temps):
            pos = temp_start + t * 2
            if pos + 2 > len(data_hex):
                break
            status = int(data_hex[pos:pos+2], 16)
            if status == 0x01:
                result['undertemp_sensors'].append(t + 1)
            elif status == 0x02:
                result['overtemp_sensors'].append(t + 1)

        # Status bytes after temps
        # Layout: [0]=charge_current, [1]=module_voltage, [2]=unknown,
        #         [3]=voltage_flags, [4]=charge_state_flags, ...
        # Last byte = operating state (1=Discharge, 2=Charge, 4=Float, 8=Full)
        status_pos = temp_start + num_temps * 2

        # The status section structure is:
        # - 2 bytes: Current status, Pack Voltage status (GB_Byte block)
        # - 1 byte: Ext_Bit count field (NumFieldEnable=True)
        # - N bytes: Ext_Bit status data where ByteIndex 0 is at offset +6
        ext_bit_start = status_pos + 6  # Skip Current(1) + PackVolt(1) + Count(1) = 3 bytes = 6 hex chars

        # Debug: capture raw status bytes
        status_hex = data_hex[status_pos:]
        result['debug']['status_pos'] = status_pos
        result['debug']['ext_bit_start'] = ext_bit_start
        result['debug']['status_hex'] = status_hex
        result['debug']['ext_bit_hex'] = data_hex[ext_bit_start:] if ext_bit_start < len(data_hex) else ""
        result['debug']['status_len'] = len(status_hex) // 2
        result['debug']['full_data_hex'] = data_hex

        # Bytes 0-1: Current/voltage status (0=normal, 1=below, 2=above)
        if status_pos + 2 <= len(data_hex):
            charge_current = int(data_hex[status_pos:status_pos+2], 16)
            if charge_current == 0x02:
                result['protections'].append('charge_overcurrent')

        if status_pos + 4 <= len(data_hex):
            module_voltage = int(data_hex[status_pos+2:status_pos+4], 16)
            if module_voltage == 0x01:
                result['protections'].append('pack_undervolt')
            elif module_voltage == 0x02:
                result['protections'].append('pack_overvolt')

        # ByteIndex 0: Balance status flags
        # bit0=Balance On, bit1=Static Balance, bit2=Static Balance Timeout
        if ext_bit_start + 2 <= len(data_hex):
            balance_status = int(data_hex[ext_bit_start:ext_bit_start+2], 16)
            result['status']['balance_status'] = balance_status
            if balance_status & 0x01:
                result['status']['balance_on'] = True
            if balance_status & 0x02:
                result['status']['static_balance'] = True

        # ByteIndex 4 is voltage status bitfield
        # Layout: bit0=CellOV_Alarm, bit1=CellOV_Protect, bit2=CellUV_Alarm, bit3=CellUV_Protect,
        #         bit4=PackOV_Alarm, bit5=PackOV_Protect, bit6=PackUV_Alarm, bit7=PackUV_Protect
        if ext_bit_start + 10 <= len(data_hex):
            voltage_status = int(data_hex[ext_bit_start+8:ext_bit_start+10], 16)
            result['status']['voltage_raw'] = voltage_status

            # These are informational - normal during charging
            if voltage_status & 0x01:
                result['warnings'].append('cell_overvolt_alarm')
            if voltage_status & 0x02:
                result['warnings'].append('cell_overvolt_protect')
            if voltage_status & 0x04:
                result['warnings'].append('cell_undervolt_alarm')
            if voltage_status & 0x10:
                result['warnings'].append('pack_overvolt_alarm')
            if voltage_status & 0x20:
                result['warnings'].append('pack_overvolt_protect')
            if voltage_status & 0x40:
                result['warnings'].append('pack_undervolt_alarm')

            # Only these are actual problems worth alerting
            if voltage_status & 0x08:
                result['protections'].append('cell_undervolt_protect')
            if voltage_status & 0x80:
                result['protections'].append('pack_undervolt_protect')

        # ByteIndex 8: MOSFET status
        # bit0=DISCHG_MOSFET On, bit1=CHG_MOSFET On, bit2=LMCHG_MOSFET On, bit3=Heat_MOSFET On
        if ext_bit_start + 18 <= len(data_hex):
            mosfet_status = int(data_hex[ext_bit_start+16:ext_bit_start+18], 16)
            result['status']['mosfet_raw'] = mosfet_status
            result['status']['discharge_mosfet_on'] = bool(mosfet_status & 0x01)
            result['status']['charge_mosfet_on'] = bool(mosfet_status & 0x02)
            result['status']['lmcharge_mosfet_on'] = bool(mosfet_status & 0x04)
            # If both charge and discharge MOSFETs are off, battery is isolated from bus
            result['status']['isolated'] = (mosfet_status & 0x03) == 0

        # ByteIndex 9-10: Individual cell balance flags (per XML spec)
        # ByteIndex 9: Balance1-8 (bit0=cell1, bit7=cell8)
        # ByteIndex 10: Balance9-16 (bit0=cell9, bit7=cell16)
        if ext_bit_start + 22 <= len(data_hex):  # Need to reach ByteIndex 10
            balance1_8 = int(data_hex[ext_bit_start+18:ext_bit_start+20], 16)
            balance9_16 = int(data_hex[ext_bit_start+20:ext_bit_start+22], 16)
            result['status']['balance1_8_raw'] = balance1_8
            result['status']['balance9_16_raw'] = balance9_16

            # Only report balancing cells if Balance On flag is set
            if result['status'].get('balance_on'):
                for bit in range(8):
                    if balance1_8 & (1 << bit):
                        result['balancing_cells'].append(bit + 1)
                    if balance9_16 & (1 << bit):
                        result['balancing_cells'].append(bit + 9)

        # CW flag detection - read from OLD position (status_pos+18) which correlated with CW=Y on display
        # This appears to be a cell warning/balancing indicator that differs from the XML-spec ByteIndex 9
        if status_pos + 22 <= len(data_hex):
            cw_byte1 = int(data_hex[status_pos+18:status_pos+20], 16)
            cw_byte2 = int(data_hex[status_pos+20:status_pos+22], 16)
            result['status']['cw_raw'] = (cw_byte1, cw_byte2)
            # CW=Y if any bits are set
            result['status']['cw_active'] = (cw_byte1 != 0) or (cw_byte2 != 0)
            # Decode which cells have CW flag
            cw_cells = []
            for bit in range(8):
                if cw_byte1 & (1 << bit):
                    cw_cells.append(bit + 1)
                if cw_byte2 & (1 << bit):
                    cw_cells.append(bit + 9)
            result['status']['cw_cells'] = cw_cells

        # Raw status bytes for correlation with display
        # Store key bytes with their positions for debugging
        result['status']['raw_bytes'] = {}
        status_hex_bytes = data_hex[status_pos:]
        for i in range(min(16, len(status_hex_bytes) // 2)):
            byte_val = int(status_hex_bytes[i*2:i*2+2], 16)
            result['status']['raw_bytes'][i] = byte_val

        # Operating state from last byte (from XML modeText)
        # 0x01=Discharge, 0x02=Charge, 0x04=Float, 0x08=Full, 0x10=Standby, 0x20=Shutdown
        if len(data_hex) >= 2:
            last_byte = int(data_hex[-2:], 16)
            result['status']['state_raw'] = last_byte
            states = []
            if last_byte & 0x01:
                states.append('Discharge')
            if last_byte & 0x02:
                states.append('Charge')
            if last_byte & 0x04:
                states.append('Float')
            if last_byte & 0x08:
                states.append('Full')
            if last_byte & 0x10:
                states.append('Standby')
            if last_byte & 0x20:
                states.append('Shutdown')
            result['status']['state'] = ', '.join(states) if states else 'Idle'

        # Only actual protection events that indicate problems go to alarms
        # Overvolt at 100% SOC is normal (BMS limiting charge), not an alarm
        result['alarms'] = list(result['protections'])

    return result


# -----------------------------
# MQTT Publisher with hysteresis
# -----------------------------
class Publisher:
    """Publishes state topics with hysteresis + rate limiting."""

    def __init__(self, client):
        self.client = client
        self.last_value = {}
        self.last_ts = {}

    def publish(self, topic: str, value, retain=False, min_interval=MIN_INTERVAL_S_DEFAULT, hyst=None):
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

        if hyst is None:
            should_pub = (prev_val != store_val) or force_due
        else:
            if not isinstance(store_val, float):
                return False
            prev_num = prev_val if isinstance(prev_val, float) else None
            should_pub = force_due or (prev_num is None) or (abs(store_val - prev_num) >= hyst)

        if not should_pub:
            return False

        try:
            self.client.publish(full_topic, payload, retain=retain)
        except Exception:
            return False

        self.last_value[full_topic] = store_val
        self.last_ts[full_topic] = now
        return True


# -----------------------------
# Home Assistant Discovery
# -----------------------------
def ha_sensor_config(object_id: str, name: str, state_topic: str,
                     unit=None, device_class=None, state_class=None,
                     icon=None, entity_category=None, display_precision=None):
    """Build a HA MQTT Discovery payload for a sensor."""
    cfg = {
        "name": name,
        "state_topic": state_topic,
        "unique_id": f"{DEVICE_ID}_{object_id}",
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
    if unit is not None:
        cfg["unit_of_measurement"] = unit
    if device_class is not None:
        cfg["device_class"] = device_class
    if state_class is not None:
        cfg["state_class"] = state_class
    if icon is not None:
        cfg["icon"] = icon
    if entity_category is not None:
        cfg["entity_category"] = entity_category
    if display_precision is not None:
        cfg["suggested_display_precision"] = display_precision
    return cfg


def ha_binary_sensor_config(object_id: str, name: str, state_topic: str,
                            device_class=None, icon=None, entity_category=None):
    """Build a HA MQTT Discovery payload for a binary sensor."""
    cfg = {
        "name": name,
        "state_topic": state_topic,
        "unique_id": f"{DEVICE_ID}_{object_id}",
        "availability_topic": AVAIL_TOPIC,
        "payload_available": "online",
        "payload_not_available": "offline",
        "payload_on": "1",
        "payload_off": "0",
        "device": {
            "identifiers": [DEVICE_ID],
            "name": DEVICE_NAME,
            "manufacturer": DEVICE_MANUFACTURER,
            "model": DEVICE_MODEL,
        },
    }
    if device_class is not None:
        cfg["device_class"] = device_class
    if icon is not None:
        cfg["icon"] = icon
    if entity_category is not None:
        cfg["entity_category"] = entity_category
    return cfg


def publish_discovery(client, num_batteries: int = NUM_BATTERIES, cells_per_battery: int = 16):
    """Publish retained Home Assistant MQTT Discovery configs."""
    logging.info("Publishing HA discovery configs...")

    # Stack-level sensors
    stack_sensors = [
        ("stack_cell_min", "Stack Cell Min", f"{MQTT_PREFIX}/stack/cell_min", "V", "voltage", "measurement", None, 3),
        ("stack_cell_max", "Stack Cell Max", f"{MQTT_PREFIX}/stack/cell_max", "V", "voltage", "measurement", None, 3),
        ("stack_cell_delta", "Stack Cell Delta", f"{MQTT_PREFIX}/stack/cell_delta_mv", "mV", None, "measurement", "mdi:chart-bell-curve-cumulative", 1),
        ("stack_voltage", "Stack Voltage", f"{MQTT_PREFIX}/stack/voltage", "V", "voltage", "measurement", None, 2),
        ("stack_current", "Stack Current", f"{MQTT_PREFIX}/stack/current", "A", "current", "measurement", None, 2),
        ("stack_temp_min", "Stack Temp Min", f"{MQTT_PREFIX}/stack/temp_min", "°C", "temperature", "measurement", None, 1),
        ("stack_temp_max", "Stack Temp Max", f"{MQTT_PREFIX}/stack/temp_max", "°C", "temperature", "measurement", None, 1),
        ("stack_balancing_count", "Stack Balancing Cells", f"{MQTT_PREFIX}/stack/balancing_count", None, None, "measurement", "mdi:scale-balance", 0),
        ("stack_alarms", "Stack Alarms", f"{MQTT_PREFIX}/stack/alarms", None, None, None, "mdi:alert", None),
    ]

    for object_id, name, st, unit, dclass, sclass, icon, precision in stack_sensors:
        cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{object_id}/config"
        cfg = ha_sensor_config(object_id, name, st, unit, dclass, sclass, icon, None, precision)
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

    # Stack balancing active binary sensor
    cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/stack_balancing_active/config"
    cfg = ha_binary_sensor_config("stack_balancing_active", "Stack Balancing Active",
                                   f"{MQTT_PREFIX}/stack/balancing_active", None, "mdi:scale-balance")
    client.publish(cfg_topic, json.dumps(cfg), retain=True)

    # Stack balancing cells list (text sensor)
    cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/stack_balancing_cells/config"
    cfg = ha_sensor_config("stack_balancing_cells", "Stack Balancing Cells List",
                           f"{MQTT_PREFIX}/stack/balancing_cells", None, None, None, "mdi:scale-balance", None, None)
    client.publish(cfg_topic, json.dumps(cfg), retain=True)

    # Per-battery sensors
    for batt in range(num_batteries):
        prefix = f"batt{batt}"
        state_prefix = f"{MQTT_PREFIX}/battery{batt}"

        batt_sensors = [
            (f"{prefix}_cell_min", f"Battery {batt} Cell Min", f"{state_prefix}/cell_min", "V", "voltage", "measurement", None, 3),
            (f"{prefix}_cell_max", f"Battery {batt} Cell Max", f"{state_prefix}/cell_max", "V", "voltage", "measurement", None, 3),
            (f"{prefix}_cell_delta", f"Battery {batt} Cell Delta", f"{state_prefix}/cell_delta_mv", "mV", None, "measurement", "mdi:chart-bell-curve-cumulative", 1),
            (f"{prefix}_voltage", f"Battery {batt} Voltage", f"{state_prefix}/voltage", "V", "voltage", "measurement", None, 2),
            (f"{prefix}_current", f"Battery {batt} Current", f"{state_prefix}/current", "A", "current", "measurement", None, 2),
            (f"{prefix}_soc", f"Battery {batt} SOC", f"{state_prefix}/soc", "%", None, "measurement", "mdi:battery", 0),
            (f"{prefix}_remain_ah", f"Battery {batt} Remaining Capacity", f"{state_prefix}/remain_ah", "Ah", None, "measurement", "mdi:battery-50", 1),
            (f"{prefix}_total_ah", f"Battery {batt} Total Capacity", f"{state_prefix}/total_ah", "Ah", None, "measurement", "mdi:battery-100", 1),
            (f"{prefix}_cycles", f"Battery {batt} Cycles", f"{state_prefix}/cycles", None, None, "total_increasing", "mdi:counter", 0),
            (f"{prefix}_balancing_count", f"Battery {batt} Balancing Cells", f"{state_prefix}/balancing_count", None, None, "measurement", "mdi:scale-balance", 0),
            (f"{prefix}_state", f"Battery {batt} State", f"{state_prefix}/state", None, None, None, "mdi:battery-charging", None),
            (f"{prefix}_warnings", f"Battery {batt} Warnings", f"{state_prefix}/warnings", None, None, None, "mdi:alert-circle-outline", None),
            (f"{prefix}_alarms", f"Battery {batt} Alarms", f"{state_prefix}/alarms", None, None, None, "mdi:alert", None),
        ]

        for object_id, name, st, unit, dclass, sclass, icon, precision in batt_sensors:
            cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{object_id}/config"
            cfg = ha_sensor_config(object_id, name, st, unit, dclass, sclass, icon, None, precision)
            client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # Battery balancing active binary sensor
        cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/{prefix}_balancing_active/config"
        cfg = ha_binary_sensor_config(f"{prefix}_balancing_active", f"Battery {batt} Balancing Active",
                                       f"{state_prefix}/balancing_active", None, "mdi:scale-balance")
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # MOSFET status binary sensors
        cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/{prefix}_charge_mosfet/config"
        cfg = ha_binary_sensor_config(f"{prefix}_charge_mosfet", f"Battery {batt} Charge MOSFET",
                                       f"{state_prefix}/charge_mosfet", None, "mdi:electric-switch")
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/{prefix}_discharge_mosfet/config"
        cfg = ha_binary_sensor_config(f"{prefix}_discharge_mosfet", f"Battery {batt} Discharge MOSFET",
                                       f"{state_prefix}/discharge_mosfet", None, "mdi:electric-switch")
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/{prefix}_lmcharge_mosfet/config"
        cfg = ha_binary_sensor_config(f"{prefix}_lmcharge_mosfet", f"Battery {batt} LM Charge MOSFET",
                                       f"{state_prefix}/lmcharge_mosfet", None, "mdi:electric-switch")
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # CW (Cell Warning) binary sensor
        cfg_topic = f"{DISCOVERY_PREFIX}/binary_sensor/{DEVICE_ID}/{prefix}_cw_active/config"
        cfg = ha_binary_sensor_config(f"{prefix}_cw_active", f"Battery {batt} Cell Warning",
                                       f"{state_prefix}/cw_active", None, "mdi:alert-circle")
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # Balancing cells list (text sensor)
        cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{prefix}_balancing_cells/config"
        cfg = ha_sensor_config(f"{prefix}_balancing_cells", f"Battery {batt} Balancing Cells List",
                               f"{state_prefix}/balancing_cells", None, None, None, "mdi:scale-balance", None, None)
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # CW cells list (text sensor)
        cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{prefix}_cw_cells/config"
        cfg = ha_sensor_config(f"{prefix}_cw_cells", f"Battery {batt} CW Cells List",
                               f"{state_prefix}/cw_cells", None, None, None, "mdi:alert-circle-outline", None, None)
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # Individual cell voltages
        for cell in range(1, cells_per_battery + 1):
            object_id = f"{prefix}_cell{cell:02d}"
            name = f"Battery {batt} Cell {cell}"
            st = f"{state_prefix}/cell{cell:02d}"
            cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{object_id}/config"
            cfg = ha_sensor_config(object_id, name, st, "V", "voltage", "measurement", None, None, 3)
            client.publish(cfg_topic, json.dumps(cfg), retain=True)

        # Temperature sensors (assume 4 per battery for discovery)
        for temp in range(1, 5):
            object_id = f"{prefix}_temp{temp}"
            name = f"Battery {batt} Temp {temp}"
            st = f"{state_prefix}/temp{temp}"
            cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{object_id}/config"
            cfg = ha_sensor_config(object_id, name, st, "°C", "temperature", "measurement", None, None, 1)
            client.publish(cfg_topic, json.dumps(cfg), retain=True)

    # Publish availability
    client.publish(AVAIL_TOPIC, "online", retain=True)
    logging.info("HA discovery published for %d batteries", num_batteries)


def shutdown(signum=None, frame=None):
    """Graceful shutdown: publish offline status."""
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


def send_command(ser: serial.Serial, cmd: bytes, timeout: float = 0.3) -> str:
    """Send command and return response INFO hex string, or None on error."""
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()
    time.sleep(timeout)

    if not ser.in_waiting:
        return None

    response = ser.read(ser.in_waiting)
    resp_text = response.decode('ascii', errors='replace').strip()

    # Check minimum length and RTN code
    if len(resp_text) < 18 or resp_text[7:9] != '00':
        return None

    # Extract INFO data (between LENID and checksum)
    return resp_text[13:-4]


def read_battery(ser: serial.Serial, addr: int, batt_num: int) -> dict:
    """Read analog data from a single battery."""
    cmd = make_analog_cmd(addr, batt_num)
    data_hex = send_command(ser, cmd)
    if data_hex:
        return decode_analog_response(data_hex)
    return None


def read_battery_alarms(ser: serial.Serial, addr: int, batt_num: int) -> dict:
    """Read alarm/balancing status from a single battery."""
    cmd = make_alarm_cmd(addr, batt_num)
    data_hex = send_command(ser, cmd)
    if data_hex:
        return decode_alarm_response(data_hex)
    return None


def read_all_batteries(port: str = RS485_PORT, baud: int = RS485_BAUD,
                       addr: int = PYLONTECH_ADDR, num_batteries: int = NUM_BATTERIES) -> dict:
    """Read data from all batteries in stack, including alarm/balancing status."""
    ser = serial.Serial(port, baud, timeout=1.0)

    result = {
        'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
        'batteries': [],
        'stack': {}
    }

    all_cells = []
    all_temps = []
    total_current = 0
    all_balancing = []
    all_alarms = []

    for batt_num in range(num_batteries):
        # Read analog data (voltages, temps, etc.)
        data = read_battery(ser, addr, batt_num)
        if data and data.get('cells'):
            batt_data = {
                'id': batt_num,
                'cells': data['cells'],
                'cell_min': min(data['cells']),
                'cell_max': max(data['cells']),
                'cell_delta_mv': round((max(data['cells']) - min(data['cells'])) * 1000, 1),
                'temps': data.get('temps', []),
                'current': data.get('current', 0),
                'voltage': data.get('voltage', sum(data['cells'])),  # BMS-reported voltage
                'voltage_calculated': sum(data['cells']),  # Sum of cells for comparison
                'remain_ah': data.get('remain_ah', 0),
                'total_ah': data.get('total_ah', 0),
                'soc': round(data.get('remain_ah', 0) / data.get('total_ah', 1) * 100, 1) if data.get('total_ah') else 0,
                'cycles': data.get('cycles', 0),
                # Alarm/balancing/warning defaults
                'balancing_cells': [],
                'balancing_count': 0,
                'warnings': [],
                'alarms': [],
            }

            # Read alarm/balancing status
            alarm_data = read_battery_alarms(ser, addr, batt_num)
            if alarm_data:
                batt_data['balancing_cells'] = alarm_data.get('balancing_cells', [])
                batt_data['balancing_count'] = len(batt_data['balancing_cells'])
                batt_data['overvolt_cells'] = alarm_data.get('overvolt_cells', [])
                batt_data['undervolt_cells'] = alarm_data.get('undervolt_cells', [])
                batt_data['warnings'] = alarm_data.get('warnings', [])
                batt_data['alarms'] = alarm_data.get('alarms', [])
                batt_data['status'] = alarm_data.get('status', {})
                batt_data['debug'] = alarm_data.get('debug', {})

                # Collect for stack summary
                for cell in batt_data['balancing_cells']:
                    all_balancing.append(f"B{batt_num}C{cell}")
                all_alarms.extend(alarm_data.get('alarms', []))

            result['batteries'].append(batt_data)
            all_cells.extend(data['cells'])
            all_temps.extend(data.get('temps', []))
            total_current += data.get('current', 0)

    ser.close()

    if all_cells and result['batteries']:
        # Parallel config: voltage is avg of batteries, current is sum
        avg_voltage = sum(b['voltage'] for b in result['batteries']) / len(result['batteries'])
        result['stack'] = {
            'num_batteries': len(result['batteries']),
            'num_cells': len(all_cells),
            'cell_min': round(min(all_cells), 3),
            'cell_max': round(max(all_cells), 3),
            'cell_delta_mv': round((max(all_cells) - min(all_cells)) * 1000, 1),
            'voltage': round(avg_voltage, 2),  # Parallel: same voltage
            'current': round(total_current, 2),  # Parallel: sum of currents
            'temp_min': round(min(all_temps), 1) if all_temps else None,
            'temp_max': round(max(all_temps), 1) if all_temps else None,
            'balancing_count': len(all_balancing),
            'balancing_cells': all_balancing,
            'alarms': list(set(all_alarms)),
        }

    return result


def print_report(data: dict):
    """Print human-readable report."""
    print("=" * 70)
    print(f"PYLONTECH BATTERY MONITOR - {data['timestamp']}")
    print("=" * 70)

    for batt in data['batteries']:
        # Header with state and balancing indicator
        status = batt.get('status', {})
        state_str = status.get('state', '')
        bal_indicator = f" ⚡ BALANCING {batt.get('balancing_count', 0)} cells" if batt.get('balancing_count') else ""
        print(f"\n▸ BATTERY {batt['id']} [{state_str}] ({len(batt['cells'])} cells, {batt['cycles']} cycles){bal_indicator}")

        # Cell voltages with flags (from BMS, not hardcoded thresholds)
        for i, v in enumerate(batt['cells'], 1):
            flags = []
            if i in batt.get('balancing_cells', []):
                flags.append("BAL")
            if i in batt.get('overvolt_cells', []):
                flags.append("OV")
            if i in batt.get('undervolt_cells', []):
                flags.append("UV")
            flag_str = f" ◄ {', '.join(flags)}" if flags else ""
            print(f"    Cell {i:2d}: {v:.3f}V{flag_str}")

        print(f"    Range: {batt['cell_min']:.3f}V - {batt['cell_max']:.3f}V (Δ {batt['cell_delta_mv']:.0f}mV)")

        # Voltage with MOSFET status
        mosfet_info = []
        if status.get('discharge_mosfet_on'):
            mosfet_info.append("DCHG")
        if status.get('charge_mosfet_on'):
            mosfet_info.append("CHG")
        if status.get('lmcharge_mosfet_on'):
            mosfet_info.append("LMCHG")
        mosfet_str = f" [FETs: {'+'.join(mosfet_info)}]" if mosfet_info else " [FETs: OFF - ISOLATED]"
        print(f"    Voltage: {batt['voltage']:.2f}V  Current: {batt['current']:.2f}A{mosfet_str}")

        if batt['temps']:
            print(f"    Temps: {[f'{t:.1f}°C' for t in batt['temps']]}")
        print(f"    SOC: {batt['soc']:.0f}% ({batt['remain_ah']:.0f}/{batt['total_ah']:.0f} Ah)")

        # Show warnings (OV/OVP flags etc)
        if batt.get('warnings'):
            print(f"    Warnings: {', '.join(batt['warnings'])}")

        # Show balance status from raw bytes (for debugging)
        status = batt.get('status', {})
        if status.get('balance_on') or status.get('static_balance'):
            flags = []
            if status.get('balance_on'):
                flags.append('BalanceOn')
            if status.get('static_balance'):
                flags.append('StaticBalance')
            print(f"    Balance: {', '.join(flags)}")

        # Show CW flag (Cell Warning - correlates with CW=Y on display)
        cw_raw = status.get('cw_raw', (0, 0))
        if status.get('cw_active'):
            cw_cells = status.get('cw_cells', [])
            print(f"    CW=Y: cells {cw_cells} (raw: 0x{cw_raw[0]:02X} 0x{cw_raw[1]:02X})")
        elif cw_raw:
            print(f"    CW=N (raw: 0x{cw_raw[0]:02X} 0x{cw_raw[1]:02X})")

        # Show raw status bytes for correlation with display
        raw_bytes = status.get('raw_bytes', {})
        if raw_bytes:
            byte_str = ' '.join(f'{raw_bytes.get(i, 0):02X}' for i in range(min(16, len(raw_bytes))))
            print(f"    Raw status: [{byte_str}]")
            # Label key positions
            print(f"    Positions: [0-1:Cur/Volt 2:ExtCnt 3-5:ExtBit0-2 9:CW1 10:CW2 11:? 12-13:? 14:State]")

        # Show alarms if any (actual problems)
        if batt.get('alarms'):
            print(f"    ⚠️  ALARMS: {', '.join(batt['alarms'])}")

    if data['stack']:
        s = data['stack']
        print(f"\n{'=' * 70}")
        print(f"STACK TOTAL: {s['num_cells']} cells across {s['num_batteries']} batteries")
        print(f"  Voltage: {s['voltage']:.2f}V")
        print(f"  Current: {s['current']:.2f}A")
        print(f"  Cell Range: {s['cell_min']:.3f}V - {s['cell_max']:.3f}V (Δ {s['cell_delta_mv']:.0f}mV)")
        if s['temp_min'] is not None:
            print(f"  Temp Range: {s['temp_min']:.1f}°C - {s['temp_max']:.1f}°C")

        # Balancing summary
        if s.get('balancing_count'):
            print(f"  ⚡ Balancing: {s['balancing_count']} cells ({', '.join(s['balancing_cells'])})")
        else:
            print(f"  Balancing: None active")

        # Stack alarms
        if s.get('alarms'):
            print(f"  ⚠️  ALARMS: {', '.join(s['alarms'])}")

    print("=" * 70)


def write_debug_log(data: dict):
    """Write change-based debug log for balancing/OV flags and state transitions."""
    global _prev_state, _debug_log_file

    if not _debug_log_file:
        return

    timestamp = data.get('timestamp', time.strftime('%Y-%m-%d %H:%M:%S'))
    lines = []

    for batt in data.get('batteries', []):
        batt_id = batt['id']
        cells = batt.get('cells', [])

        # Current state
        curr_bal = set(batt.get('balancing_cells', []))
        curr_ov = set(batt.get('overvolt_cells', []))
        curr_state = batt.get('status', {}).get('state', 'Unknown')

        # Previous state (initialize if first time - log initial state)
        if batt_id not in _prev_state:
            _prev_state[batt_id] = {'balancing': set(), 'overvolt': set(), 'state': ''}
            # Log initial state on first poll
            init_parts = [f"STATE={curr_state}"]
            if curr_bal:
                cell_info = ','.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(curr_bal) if c <= len(cells))
                init_parts.append(f"BAL[{cell_info}]")
            if curr_ov:
                cell_info = ','.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(curr_ov) if c <= len(cells))
                init_parts.append(f"OV[{cell_info}]")
            lines.append(f"{timestamp} B{batt_id} INIT  {' '.join(init_parts)}")

        prev = _prev_state[batt_id]

        # Check for balancing changes
        bal_added = curr_bal - prev['balancing']
        bal_removed = prev['balancing'] - curr_bal

        if bal_added:
            cell_info = ' '.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(bal_added) if c <= len(cells))
            lines.append(f"{timestamp} B{batt_id} BAL+ {cell_info}")

        if bal_removed:
            cell_info = ' '.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(bal_removed) if c <= len(cells))
            lines.append(f"{timestamp} B{batt_id} BAL- {cell_info}")

        # Check for overvolt changes
        ov_added = curr_ov - prev['overvolt']
        ov_removed = prev['overvolt'] - curr_ov

        if ov_added:
            cell_info = ' '.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(ov_added) if c <= len(cells))
            lines.append(f"{timestamp} B{batt_id} OV+  {cell_info}")

        if ov_removed:
            cell_info = ' '.join(f"C{c}={cells[c-1]:.3f}V" for c in sorted(ov_removed) if c <= len(cells))
            lines.append(f"{timestamp} B{batt_id} OV-  {cell_info}")

        # Check for state changes
        if curr_state != prev['state'] and prev['state']:  # Skip initial empty state
            lines.append(f"{timestamp} B{batt_id} STATE {prev['state']} -> {curr_state}")

        # Update previous state
        _prev_state[batt_id] = {'balancing': curr_bal, 'overvolt': curr_ov, 'state': curr_state}

    # Write to log file
    if lines:
        try:
            with open(_debug_log_file, 'a') as f:
                for line in lines:
                    f.write(line + '\n')
                f.flush()
        except Exception as e:
            logging.warning("Failed to write debug log: %s", e)


def publish_mqtt_data(pub: Publisher, data: dict):
    """Publish battery data using Publisher with hysteresis."""
    # Publish stack data
    s = data.get('stack', {})
    if s:
        pub.publish("stack/cell_min", round(s['cell_min'], 3), hyst=VOLT_HYST_V)
        pub.publish("stack/cell_max", round(s['cell_max'], 3), hyst=VOLT_HYST_V)
        pub.publish("stack/cell_delta_mv", round(s['cell_delta_mv'], 1))
        pub.publish("stack/voltage", round(s['voltage'], 2))
        pub.publish("stack/current", round(s['current'], 2))
        if s.get('temp_min') is not None:
            pub.publish("stack/temp_min", round(s['temp_min'], 1))
        if s.get('temp_max') is not None:
            pub.publish("stack/temp_max", round(s['temp_max'], 1))
        pub.publish("stack/balancing_count", s.get('balancing_count', 0))
        pub.publish("stack/balancing_active", 1 if s.get('balancing_count') else 0)
        # Publish which cells are balancing across stack (e.g., "B0C3,B1C7")
        bal_cells = s.get('balancing_cells', [])
        pub.publish("stack/balancing_cells", ','.join(bal_cells) if bal_cells else '')
        pub.publish("stack/alarms", ','.join(s.get('alarms', [])) if s.get('alarms') else '')

    # Publish per-battery data
    for batt in data.get('batteries', []):
        prefix = f"battery{batt['id']}"
        pub.publish(f"{prefix}/cell_min", round(batt['cell_min'], 3), hyst=VOLT_HYST_V)
        pub.publish(f"{prefix}/cell_max", round(batt['cell_max'], 3), hyst=VOLT_HYST_V)
        pub.publish(f"{prefix}/cell_delta_mv", round(batt['cell_delta_mv'], 1))
        pub.publish(f"{prefix}/voltage", round(batt.get('voltage', 0), 2))
        pub.publish(f"{prefix}/current", round(batt.get('current', 0), 2))
        pub.publish(f"{prefix}/soc", round(batt['soc'], 0))
        pub.publish(f"{prefix}/remain_ah", round(batt.get('remain_ah', 0), 1))
        pub.publish(f"{prefix}/total_ah", round(batt.get('total_ah', 0), 1))
        pub.publish(f"{prefix}/cycles", batt['cycles'])
        pub.publish(f"{prefix}/balancing_count", batt.get('balancing_count', 0))
        pub.publish(f"{prefix}/balancing_active", 1 if batt.get('balancing_count') else 0)
        # Publish which cells are balancing (e.g., "3,7,12")
        bal_cells = batt.get('balancing_cells', [])
        pub.publish(f"{prefix}/balancing_cells", ','.join(str(c) for c in bal_cells) if bal_cells else '')

        # Warnings (OV/OVP flags) and alarms
        pub.publish(f"{prefix}/warnings", ','.join(batt.get('warnings', [])) if batt.get('warnings') else '')
        pub.publish(f"{prefix}/alarms", ','.join(batt.get('alarms', [])) if batt.get('alarms') else '')

        # State and MOSFET status
        status = batt.get('status', {})
        pub.publish(f"{prefix}/state", status.get('state', ''))
        pub.publish(f"{prefix}/charge_mosfet", 1 if status.get('charge_mosfet_on') else 0)
        pub.publish(f"{prefix}/discharge_mosfet", 1 if status.get('discharge_mosfet_on') else 0)
        pub.publish(f"{prefix}/lmcharge_mosfet", 1 if status.get('lmcharge_mosfet_on') else 0)

        # CW (Cell Warning) flag
        pub.publish(f"{prefix}/cw_active", 1 if status.get('cw_active') else 0)
        cw_cells = status.get('cw_cells', [])
        pub.publish(f"{prefix}/cw_cells", ','.join(str(c) for c in cw_cells) if cw_cells else '')

        # Individual cell voltages
        for i, v in enumerate(batt['cells'], 1):
            pub.publish(f"{prefix}/cell{i:02d}", round(v, 3),
                       min_interval=MIN_INTERVAL_S_CELLS, hyst=VOLT_HYST_V)

        # Temperature sensors
        for i, t in enumerate(batt.get('temps', []), 1):
            pub.publish(f"{prefix}/temp{i}", round(t, 1))


def main():
    global _mqtt_client, _running

    parser = argparse.ArgumentParser(description='Pylontech RS485 Battery Monitor')
    parser.add_argument('--port', default=RS485_PORT, help='Serial port')
    parser.add_argument('--loop', action='store_true', help='Continuous monitoring')
    parser.add_argument('--interval', type=int, default=30, help='Loop interval seconds')
    parser.add_argument('--json', action='store_true', help='JSON output')
    parser.add_argument('--mqtt', action='store_true', help='Publish to MQTT with HA discovery')
    parser.add_argument('--quiet', action='store_true', help='Suppress console output (for daemon mode)')
    parser.add_argument('--debug-log', metavar='FILE', help='Log balancing/OV/state changes to file')
    args = parser.parse_args()

    # Set up debug logging
    global _debug_log_file
    if args.debug_log:
        _debug_log_file = args.debug_log
        logging.info("Debug logging to: %s", _debug_log_file)

    pub = None

    if args.mqtt:
        import paho.mqtt.client as mqtt

        # paho-mqtt v2.x compatibility
        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:
            client = mqtt.Client()

        def on_connect(client, userdata, flags, rc, properties=None):
            if hasattr(rc, 'value'):
                rc_val = rc.value
            else:
                rc_val = rc
            if rc_val == 0:
                logging.info("Connected to MQTT broker %s:%d", MQTT_HOST, MQTT_PORT)
                # Re-publish discovery after reconnect
                try:
                    publish_discovery(client)
                except Exception:
                    pass
            else:
                logging.error("MQTT connection failed with code %s", rc)

        def on_disconnect(client, userdata, rc, properties=None):
            logging.warning("MQTT disconnected (rc=%s), will auto-reconnect", rc)

        client.on_connect = on_connect
        client.on_disconnect = on_disconnect
        client.reconnect_delay_set(min_delay=1, max_delay=60)
        if MQTT_USER and MQTT_PASS:
            client.username_pw_set(MQTT_USER, MQTT_PASS)

        # Last Will Testament
        client.will_set(AVAIL_TOPIC, payload="offline", qos=0, retain=True)

        try:
            client.connect(MQTT_HOST, MQTT_PORT, 60)
        except Exception as e:
            logging.error("Failed to connect to MQTT: %s", e)
            sys.exit(1)

        client.loop_start()
        _mqtt_client = client
        pub = Publisher(client)

        # Register signal handlers
        signal.signal(signal.SIGTERM, shutdown)
        signal.signal(signal.SIGINT, shutdown)

        logging.info("RS485->MQTT bridge started (topics under %s/)", MQTT_PREFIX)

    while _running:
        try:
            data = read_all_batteries(port=args.port)

            if args.json:
                print(json.dumps(data, indent=2))
            elif not args.quiet:
                print_report(data)

            if args.mqtt and pub:
                publish_mqtt_data(pub, data)
                if not args.quiet:
                    logging.info("Published %d batteries to MQTT", len(data.get('batteries', [])))

            # Write debug log (if enabled)
            if _debug_log_file:
                write_debug_log(data)

            if not args.loop:
                break

            time.sleep(args.interval)

        except KeyboardInterrupt:
            logging.info("Stopped by user.")
            break
        except serial.SerialException as e:
            logging.error("Serial error: %s", e)
            if not args.loop:
                break
            time.sleep(5)
        except Exception as e:
            logging.exception("Error: %s", e)
            if not args.loop:
                break
            time.sleep(5)

    # Cleanup
    if args.mqtt and _mqtt_client:
        try:
            # Give MQTT time to flush queued messages
            time.sleep(0.5)
            _mqtt_client.publish(AVAIL_TOPIC, "offline", retain=True)
            time.sleep(0.2)
            _mqtt_client.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    main()
