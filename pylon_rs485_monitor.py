#!/usr/bin/env python3
"""
Pylontech RS485 Battery Monitor
Reads individual cell voltages from all batteries via RS485.

Protocol: Pylontech RS485 @ 9600 baud
Tested with: Shoto/Pylontech-compatible 16S LFP batteries

Usage:
    ./pylon_rs485_monitor.py                    # Single read
    ./pylon_rs485_monitor.py --loop             # Continuous monitoring
    ./pylon_rs485_monitor.py --json             # JSON output
    ./pylon_rs485_monitor.py --mqtt             # Publish to MQTT
"""

import sys
import time
import json
import argparse
import serial

# Configuration
RS485_PORT = "/dev/ttyUSB0"
RS485_BAUD = 9600
PYLONTECH_ADDR = 2  # Battery stack address
NUM_BATTERIES = 3   # Number of batteries in stack (0, 1, 2)

# MQTT settings (optional)
MQTT_HOST = "192.168.200.217"
MQTT_PORT = 1883
MQTT_USER = "mqtt_explorer2"
MQTT_PASS = "exploder99"
MQTT_PREFIX = "deye_bms"


def calc_chksum(frame_content: str) -> str:
    """Calculate Pylontech frame checksum."""
    total = sum(ord(c) for c in frame_content)
    return f"{(~total + 1) & 0xFFFF:04X}"


def make_analog_cmd(addr: int, batt_num: int) -> bytes:
    """Build analog data request command."""
    ver, cid1, cid2 = "20", "46", "42"
    adr = f"{addr:02X}"
    info = f"{batt_num:02X}"
    len_hex = f"{len(info):03X}"
    lchksum = (~sum(int(c, 16) for c in len_hex) + 1) & 0xF
    lenid = f"{lchksum:X}{len_hex}"
    frame = f"{ver}{adr}{cid1}{cid2}{lenid}{info}"
    return f"~{frame}{calc_chksum(frame)}\r".encode('ascii')


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

    # Voltage (mV)
    if i + 4 <= len(data_hex):
        result['voltage'] = int(data_hex[i:i+4], 16) / 1000.0
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


def read_battery(ser: serial.Serial, addr: int, batt_num: int) -> dict:
    """Read data from a single battery."""
    cmd = make_analog_cmd(addr, batt_num)
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()
    time.sleep(0.3)

    if not ser.in_waiting:
        return None

    response = ser.read(ser.in_waiting)
    resp_text = response.decode('ascii', errors='replace').strip()

    if len(resp_text) < 18 or resp_text[7:9] != '00':
        return None

    data_hex = resp_text[13:-4]
    return decode_analog_response(data_hex)


def read_all_batteries(port: str = RS485_PORT, baud: int = RS485_BAUD,
                       addr: int = PYLONTECH_ADDR, num_batteries: int = NUM_BATTERIES) -> dict:
    """Read data from all batteries in stack."""
    ser = serial.Serial(port, baud, timeout=1.0)

    result = {
        'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
        'batteries': [],
        'stack': {}
    }

    all_cells = []
    all_temps = []
    total_current = 0

    for batt_num in range(num_batteries):
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
                'voltage': sum(data['cells']),  # Calculate from cells
                'remain_ah': data.get('remain_ah', 0),
                'total_ah': data.get('total_ah', 0),
                'soc': round(data.get('remain_ah', 0) / data.get('total_ah', 1) * 100, 1) if data.get('total_ah') else 0,
                'cycles': data.get('cycles', 0)
            }
            result['batteries'].append(batt_data)
            all_cells.extend(data['cells'])
            all_temps.extend(data.get('temps', []))
            total_current += data.get('current', 0)

    ser.close()

    if all_cells:
        result['stack'] = {
            'num_batteries': len(result['batteries']),
            'num_cells': len(all_cells),
            'cell_min': round(min(all_cells), 3),
            'cell_max': round(max(all_cells), 3),
            'cell_delta_mv': round((max(all_cells) - min(all_cells)) * 1000, 1),
            'voltage': round(sum(all_cells), 2),
            'current': round(total_current, 2),
            'temp_min': round(min(all_temps), 1) if all_temps else None,
            'temp_max': round(max(all_temps), 1) if all_temps else None,
        }

    return result


def print_report(data: dict):
    """Print human-readable report."""
    print("=" * 70)
    print(f"PYLONTECH BATTERY MONITOR - {data['timestamp']}")
    print("=" * 70)

    for batt in data['batteries']:
        print(f"\n▸ BATTERY {batt['id']} ({len(batt['cells'])} cells, {batt['cycles']} cycles)")
        for i, v in enumerate(batt['cells'], 1):
            flag = " ◄ LOW" if v < 3.4 else " ◄ HIGH" if v > 3.55 else ""
            print(f"    Cell {i:2d}: {v:.3f}V{flag}")
        print(f"    Range: {batt['cell_min']:.3f}V - {batt['cell_max']:.3f}V (Δ {batt['cell_delta_mv']:.0f}mV)")
        if batt['temps']:
            print(f"    Temps: {[f'{t:.1f}°C' for t in batt['temps']]}")
        print(f"    SOC: {batt['soc']:.0f}% ({batt['remain_ah']:.0f}/{batt['total_ah']:.0f} Ah)")

    if data['stack']:
        s = data['stack']
        print(f"\n{'=' * 70}")
        print(f"STACK TOTAL: {s['num_cells']} cells across {s['num_batteries']} batteries")
        print(f"  Voltage: {s['voltage']:.2f}V")
        print(f"  Current: {s['current']:.2f}A")
        print(f"  Cell Range: {s['cell_min']:.3f}V - {s['cell_max']:.3f}V (Δ {s['cell_delta_mv']:.0f}mV)")
        if s['temp_min'] is not None:
            print(f"  Temp Range: {s['temp_min']:.1f}°C - {s['temp_max']:.1f}°C")
    print("=" * 70)


def publish_mqtt(data: dict):
    """Publish data to MQTT."""
    try:
        import paho.mqtt.client as mqtt

        client = mqtt.Client()
        client.username_pw_set(MQTT_USER, MQTT_PASS)
        client.connect(MQTT_HOST, MQTT_PORT, 60)

        # Publish stack data
        s = data.get('stack', {})
        if s:
            client.publish(f"{MQTT_PREFIX}/rs485/cell_min", s['cell_min'])
            client.publish(f"{MQTT_PREFIX}/rs485/cell_max", s['cell_max'])
            client.publish(f"{MQTT_PREFIX}/rs485/cell_delta_mv", s['cell_delta_mv'])
            client.publish(f"{MQTT_PREFIX}/rs485/stack_voltage", s['voltage'])

        # Publish per-battery data
        for batt in data['batteries']:
            prefix = f"{MQTT_PREFIX}/rs485/battery{batt['id']}"
            client.publish(f"{prefix}/cell_min", batt['cell_min'])
            client.publish(f"{prefix}/cell_max", batt['cell_max'])
            client.publish(f"{prefix}/cell_delta_mv", batt['cell_delta_mv'])
            client.publish(f"{prefix}/soc", batt['soc'])
            client.publish(f"{prefix}/cycles", batt['cycles'])

            # Publish individual cells
            for i, v in enumerate(batt['cells'], 1):
                client.publish(f"{prefix}/cell{i:02d}", round(v, 3))

        client.disconnect()
        print(f"Published to MQTT {MQTT_HOST}")

    except Exception as e:
        print(f"MQTT error: {e}")


def main():
    parser = argparse.ArgumentParser(description='Pylontech RS485 Battery Monitor')
    parser.add_argument('--port', default=RS485_PORT, help='Serial port')
    parser.add_argument('--loop', action='store_true', help='Continuous monitoring')
    parser.add_argument('--interval', type=int, default=30, help='Loop interval seconds')
    parser.add_argument('--json', action='store_true', help='JSON output')
    parser.add_argument('--mqtt', action='store_true', help='Publish to MQTT')
    args = parser.parse_args()

    while True:
        try:
            data = read_all_batteries(port=args.port)

            if args.json:
                print(json.dumps(data, indent=2))
            else:
                print_report(data)

            if args.mqtt:
                publish_mqtt(data)

            if not args.loop:
                break

            time.sleep(args.interval)

        except KeyboardInterrupt:
            print("\nStopped.")
            break
        except Exception as e:
            print(f"Error: {e}")
            if not args.loop:
                break
            time.sleep(5)


if __name__ == "__main__":
    main()
