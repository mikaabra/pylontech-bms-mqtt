#!/usr/bin/env python3
"""
MQTT Display - Shows battery data in formatted console output.
Use this after migrating to ESP32 to get the same console view.

Usage:
    ./mqtt_display.py
    ./mqtt_display.py --host 192.168.200.217
"""

import os
import sys
import json
import time
import argparse
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Please install paho-mqtt: pip3 install paho-mqtt")
    sys.exit(1)

# Configuration
MQTT_HOST = os.environ.get("MQTT_HOST", "192.168.200.217")
MQTT_PORT = int(os.environ.get("MQTT_PORT", 1883))
MQTT_USER = os.environ.get("MQTT_USER")
MQTT_PASS = os.environ.get("MQTT_PASS")

RS485_PREFIX = "deye_bms/rs485"
CAN_PREFIX = "deye_bms"
NUM_BATTERIES = int(os.environ.get("NUM_BATTERIES", 3))

# Data storage - initialized in main() after parsing args
data = {
    'can': {},
    'stack': {},
    'batteries': []
}

last_display = 0
DISPLAY_INTERVAL = 5  # seconds


def on_message(client, userdata, msg):
    """Handle incoming MQTT messages."""
    global last_display

    topic = msg.topic
    try:
        payload = msg.payload.decode()
    except:
        return

    # Parse topic and store data
    if topic.startswith(f"{RS485_PREFIX}/stack/"):
        key = topic.replace(f"{RS485_PREFIX}/stack/", "")
        data['stack'][key] = payload
    elif topic.startswith(f"{RS485_PREFIX}/battery"):
        # Extract battery number and key
        parts = topic.replace(f"{RS485_PREFIX}/battery", "").split("/", 1)
        if len(parts) == 2:
            try:
                batt_num = int(parts[0])
                key = parts[1]
                if 0 <= batt_num < NUM_BATTERIES:
                    data['batteries'][batt_num][key] = payload
            except:
                pass
    elif topic.startswith(f"{CAN_PREFIX}/") and not topic.startswith(RS485_PREFIX):
        key = topic.replace(f"{CAN_PREFIX}/", "")
        data['can'][key] = payload

    # Display periodically
    now = time.time()
    if now - last_display >= DISPLAY_INTERVAL:
        last_display = now
        display_data()


def display_data():
    """Display formatted battery data."""
    print("\033[2J\033[H", end="")  # Clear screen

    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print("=" * 70)
    print(f"BATTERY MONITOR (MQTT) - {ts}")
    print("=" * 70)

    # CAN data
    if data['can']:
        print(f"\n▸ CAN DATA")
        soc = data['can'].get('soc', '?')
        soh = data['can'].get('soh', '?')
        print(f"    SOC: {soc}%  SOH: {soh}%")

        v_charge = data['can'].get('limit/v_charge_max', '?')
        i_charge = data['can'].get('limit/i_charge', '?')
        i_discharge = data['can'].get('limit/i_discharge', '?')
        v_low = data['can'].get('limit/v_low', '?')
        print(f"    Limits: Charge {v_charge}V/{i_charge}A  Discharge {i_discharge}A  Low {v_low}V")

        cell_min = data['can'].get('ext/cell_v_min', '?')
        cell_max = data['can'].get('ext/cell_v_max', '?')
        print(f"    Cell Range: {cell_min}V - {cell_max}V")

    # Per-battery data
    for batt_num in range(NUM_BATTERIES):
        batt = data['batteries'][batt_num]
        if not batt:
            continue

        state = batt.get('state', '?')
        cycles = batt.get('cycles', '?')
        print(f"\n▸ BATTERY {batt_num} [{state}] ({cycles} cycles)")

        # Cell voltages
        cells = []
        for i in range(1, 17):
            key = f"cell{i:02d}"
            if key in batt:
                cells.append(float(batt[key]))

        if cells:
            for i, v in enumerate(cells):
                print(f"    Cell {i+1:2d}: {v:.3f}V")

            cell_min = min(cells)
            cell_max = max(cells)
            delta = (cell_max - cell_min) * 1000
            print(f"    Range: {cell_min:.3f}V - {cell_max:.3f}V (Δ {delta:.0f}mV)")

        # Voltage, current, FETs
        voltage = batt.get('voltage', '?')
        current = batt.get('current', '?')
        chg_fet = '1' if batt.get('charge_mosfet') == '1' else '0'
        dchg_fet = '1' if batt.get('discharge_mosfet') == '1' else '0'
        lm_fet = '1' if batt.get('lmcharge_mosfet') == '1' else '0'

        fets = []
        if dchg_fet == '1': fets.append('DCHG')
        if chg_fet == '1': fets.append('CHG')
        if lm_fet == '1': fets.append('LMCHG')
        fet_str = '+'.join(fets) if fets else 'OFF'

        print(f"    Voltage: {voltage}V  Current: {current}A [FETs: {fet_str}]")

        # Temperatures
        temps = []
        for i in range(1, 7):
            key = f"temp{i}"
            if key in batt:
                temps.append(f"{float(batt[key]):.1f}°C")
        if temps:
            print(f"    Temps: {temps}")

        # SOC, capacity
        soc = batt.get('soc', '?')
        remain = batt.get('remain_ah', '?')
        total = batt.get('total_ah', '?')
        print(f"    SOC: {soc}% ({remain}/{total} Ah)")

        # CW flag
        cw_active = batt.get('cw_active', '0')
        cw_cells = batt.get('cw_cells', '')
        if cw_active == '1' and cw_cells:
            print(f"    CW=Y: cells [{cw_cells}]")
        else:
            print(f"    CW=N")

        # Balancing
        bal_count = batt.get('balancing_count', '0')
        bal_cells = batt.get('balancing_cells', '')
        if int(bal_count) > 0:
            print(f"    Balancing: {bal_count} cells [{bal_cells}]")

        # Alarms/warnings
        alarms = batt.get('alarms', '')
        warnings = batt.get('warnings', '')
        if alarms:
            print(f"    ALARMS: {alarms}")
        if warnings:
            print(f"    Warnings: {warnings}")

    # Stack summary
    if data['stack']:
        stack = data['stack']
        print(f"\n▸ STACK TOTAL")
        voltage = stack.get('voltage', '?')
        current = stack.get('current', '?')
        cell_min = stack.get('cell_min', '?')
        cell_max = stack.get('cell_max', '?')
        delta = stack.get('cell_delta_mv', '?')
        bal = stack.get('balancing_count', '0')

        print(f"    Voltage: {voltage}V  Current: {current}A")
        print(f"    Cell Range: {cell_min}V - {cell_max}V (Δ {delta}mV)")
        bal_cells = stack.get('balancing_cells', '')
        if int(bal) > 0:
            print(f"    Balancing: {bal} cells [{bal_cells}]")

        alarms = stack.get('alarms', '')
        if alarms:
            print(f"    STACK ALARMS: {alarms}")

    print("\n" + "=" * 70)


def main():
    global NUM_BATTERIES

    parser = argparse.ArgumentParser(description='Display battery data from MQTT')
    parser.add_argument('--host', default=MQTT_HOST, help='MQTT broker host')
    parser.add_argument('--port', type=int, default=MQTT_PORT, help='MQTT broker port')
    parser.add_argument('--batteries', type=int, default=NUM_BATTERIES, help='Number of batteries')
    args = parser.parse_args()

    NUM_BATTERIES = args.batteries
    data['batteries'] = [{} for _ in range(NUM_BATTERIES)]

    # Connect to MQTT
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except:
        client = mqtt.Client()

    if MQTT_USER and MQTT_PASS:
        client.username_pw_set(MQTT_USER, MQTT_PASS)

    client.on_message = on_message

    print(f"Connecting to MQTT broker at {args.host}:{args.port}...")
    try:
        client.connect(args.host, args.port, 60)
    except Exception as e:
        print(f"Failed to connect: {e}")
        sys.exit(1)

    # Subscribe to all relevant topics
    client.subscribe(f"{RS485_PREFIX}/#")
    client.subscribe(f"{CAN_PREFIX}/#")

    print("Connected. Waiting for data...")

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nExiting...")
        client.disconnect()


if __name__ == "__main__":
    main()
