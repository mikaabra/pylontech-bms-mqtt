#!/usr/bin/env python3
"""
Pylontech RS485 Responder / Fake Battery
Responds to Epever inverter requests at 115200 baud using Pylontech protocol.
Fakes battery data for testing RS485 communication.

Usage:
    ./pylon_rs485_responder.py --port /dev/cu.usbserial-A10N5EG8
"""

import serial
import time
import argparse
import sys

# Configuration
DEFAULT_PORT = "/dev/cu.usbserial-A10N5EG8"
BAUD_RATE = 115200
BATTERY_ADDR = 2  # Address we respond as

# Fake battery parameters
FAKE_BATTERY = {
    'num_cells': 16,
    'cell_voltage_mv': 3350,  # 3.35V per cell
    'num_temps': 4,
    'temp_c': 25.0,
    'current_a': 0.0,  # Idle
    'soc_percent': 80,
    'capacity_ah': 100,
    'cycles': 50,
    'voltage_v': 53.6,  # 16 * 3.35V
}


def calc_chksum(frame_content: str) -> str:
    """Calculate Pylontech frame checksum."""
    total = sum(ord(c) for c in frame_content)
    return f"{(~total + 1) & 0xFFFF:04X}"


def make_response(addr: int, cid2: int, rtn: int, info: str = "") -> bytes:
    """Build a Pylontech response frame."""
    ver, cid1 = "20", "46"
    adr = f"{addr:02X}"
    cid2_hex = f"{cid2:02X}"
    rtn_hex = f"{rtn:02X}"

    # LENID: length checksum + 3-digit length
    info_len = len(info)
    len_hex = f"{info_len:03X}"
    lchksum = (~sum(int(c, 16) for c in len_hex) + 1) & 0xF
    lenid = f"{lchksum:X}{len_hex}"

    frame = f"{ver}{adr}{cid1}{cid2_hex}{rtn_hex}{lenid}{info}"
    return f"~{frame}{calc_chksum(frame)}\r".encode('ascii')


def make_analog_response(addr: int, batt_num: int = 0) -> bytes:
    """Build analog data response (CID2=0x42).

    Response format:
    - 2 bytes: info_flag + command_value
    - 2 bytes: battery number
    - 2 bytes: num_cells
    - num_cells * 4 bytes: cell voltages (mV, 16-bit each)
    - 2 bytes: num_temps
    - num_temps * 4 bytes: temperatures (K * 10, 16-bit each)
    - 4 bytes: current (10mA units, signed)
    - 4 bytes: voltage (10mV units)
    - 4 bytes: remaining capacity (10mAh)
    - 2 bytes: user-defined (0x03)
    - 4 bytes: total capacity (10mAh)
    - 4 bytes: cycle count
    """
    b = FAKE_BATTERY
    info = ""

    # Header: info_flag (0x00) + command (0x00)
    info += "00"
    # Battery number
    info += f"{batt_num:02X}"

    # Number of cells
    info += f"{b['num_cells']:02X}"

    # Cell voltages (mV as 16-bit values)
    for i in range(b['num_cells']):
        # Add slight variation to make it realistic
        mv = b['cell_voltage_mv'] + (i % 3) * 5 - 5
        info += f"{mv:04X}"

    # Number of temps
    info += f"{b['num_temps']:02X}"

    # Temperatures (Kelvin * 10, offset by 2731 for 0°C)
    for i in range(b['num_temps']):
        temp_k10 = int((b['temp_c'] + 273.1) * 10)
        info += f"{temp_k10:04X}"

    # Current (10mA units, signed 16-bit)
    current_10ma = int(b['current_a'] * 100)
    if current_10ma < 0:
        current_10ma = current_10ma & 0xFFFF
    info += f"{current_10ma:04X}"

    # Voltage (10mV units)
    voltage_10mv = int(b['voltage_v'] * 100)
    info += f"{voltage_10mv:04X}"

    # Remaining capacity (10mAh units)
    remain_10mah = int(b['capacity_ah'] * b['soc_percent'] / 100 * 100)
    info += f"{remain_10mah:04X}"

    # User-defined byte
    info += "03"

    # Total capacity (10mAh units)
    total_10mah = int(b['capacity_ah'] * 100)
    info += f"{total_10mah:04X}"

    # Cycle count
    info += f"{b['cycles']:04X}"

    return make_response(addr, 0x42, 0x00, info)


def make_alarm_response(addr: int, batt_num: int = 0) -> bytes:
    """Build alarm info response (CID2=0x44).

    All zeros = no alarms, no balancing, normal operation.
    """
    b = FAKE_BATTERY
    info = ""

    # Info flag
    info += "00"
    # Battery number
    info += f"{batt_num:02X}"

    # Number of cells
    info += f"{b['num_cells']:02X}"

    # Cell status (0x00 = normal for each cell)
    for _ in range(b['num_cells']):
        info += "00"

    # Number of temps
    info += f"{b['num_temps']:02X}"

    # Temp status (0x00 = normal)
    for _ in range(b['num_temps']):
        info += "00"

    # Status bytes: charge current, module voltage
    info += "00"  # Charge current normal
    info += "00"  # Module voltage normal

    # Status byte count (extended status)
    info += "06"  # 6 extended status bytes follow

    # Extended status bytes (all normal/off)
    info += "00"  # Balance status (0 = not balancing)
    info += "00"  # Reserved
    info += "00"  # Reserved
    info += "00"  # Reserved
    info += "00"  # Voltage status flags
    info += "00"  # Temperature status flags
    info += "03"  # MOSFET status: charge + discharge on
    info += "00"  # Balance cells 1-8
    info += "00"  # Balance cells 9-16

    # Operating state (0x00 = Idle, 0x01 = Discharge, 0x02 = Charge)
    info += "00"

    return make_response(addr, 0x44, 0x00, info)


def make_system_param_response(addr: int) -> bytes:
    """Build system parameter response (CID2=0x4F)."""
    # Simple response with basic system info
    # Format varies by implementation, this is a minimal response
    info = ""
    info += "00"  # Info flag
    info += "01"  # Number of batteries
    info += "10"  # 16 cells per battery
    info += "00"  # Reserved
    return make_response(addr, 0x4F, 0x00, info)


def make_protocol_version_response(addr: int) -> bytes:
    """Build protocol version response (CID2=0x4F with different content)."""
    info = "0020"  # Version 2.0
    return make_response(addr, 0x90, 0x00, info)


def make_manufacturer_response(addr: int) -> bytes:
    """Build manufacturer info response (CID2=0x61)."""
    # Return "PYLON" as manufacturer
    mfr = "PYLONTECH"
    info = mfr.encode('ascii').hex().upper()
    return make_response(addr, 0x61, 0x00, info)


def make_firmware_response(addr: int) -> bytes:
    """Build firmware version response (CID2=0x62)."""
    # Return firmware version string
    fw = "V1.0"
    info = fw.encode('ascii').hex().upper()
    return make_response(addr, 0x62, 0x00, info)


def make_serial_response(addr: int) -> bytes:
    """Build serial number response (CID2=0x63)."""
    # Return serial number
    serial = "FAKE00001"
    info = serial.encode('ascii').hex().upper()
    return make_response(addr, 0x63, 0x00, info)


def parse_request(frame: bytes) -> dict:
    """Parse incoming Pylontech request frame."""
    try:
        text = frame.decode('ascii', errors='replace').strip()
        if not text.startswith('~') or len(text) < 18:
            return None

        content = text[1:]  # Strip ~
        return {
            'ver': content[0:2],
            'addr': int(content[2:4], 16),
            'cid1': int(content[4:6], 16),
            'cid2': int(content[6:8], 16),
            'lenid': content[8:12],
            'info': content[12:-4] if len(content) > 16 else "",
            'chksum': content[-4:],
            'raw': text
        }
    except Exception as e:
        print(f"Parse error: {e}")
        return None


def handle_request(req: dict) -> bytes:
    """Generate response for a request."""
    cid2 = req['cid2']
    addr = BATTERY_ADDR

    # Extract battery number from info if present
    batt_num = 0
    if req['info'] and len(req['info']) >= 2:
        try:
            batt_num = int(req['info'][0:2], 16)
        except:
            pass

    if cid2 == 0x42:  # Get analog data
        return make_analog_response(addr, batt_num)
    elif cid2 == 0x44:  # Get alarm info
        return make_alarm_response(addr, batt_num)
    elif cid2 == 0x4F:  # Get system parameter
        return make_system_param_response(addr)
    elif cid2 == 0x61:  # Manufacturer info
        return make_manufacturer_response(addr)
    elif cid2 == 0x62:  # Firmware version
        return make_firmware_response(addr)
    elif cid2 == 0x63:  # Serial number
        return make_serial_response(addr)
    elif cid2 == 0x90:  # Protocol version
        return make_protocol_version_response(addr)
    else:
        print(f"  Unknown CID2: 0x{cid2:02X}")
        # Return error response (RTN=0x01 = version error, or appropriate)
        return make_response(addr, cid2, 0x04, "")  # 0x04 = CID2 invalid


def main():
    parser = argparse.ArgumentParser(description='Pylontech RS485 Fake Battery Responder')
    parser.add_argument('--port', default=DEFAULT_PORT, help='Serial port')
    parser.add_argument('--baud', type=int, default=BAUD_RATE, help='Baud rate')
    parser.add_argument('--soc', type=int, default=80, help='Fake SOC percentage')
    parser.add_argument('--voltage', type=float, default=53.6, help='Fake pack voltage')
    args = parser.parse_args()

    # Update fake battery params
    FAKE_BATTERY['soc_percent'] = args.soc
    FAKE_BATTERY['voltage_v'] = args.voltage

    print(f"Pylontech RS485 Fake Battery Responder")
    print(f"Port: {args.port} @ {args.baud} baud")
    print(f"Responding as battery address {BATTERY_ADDR}")
    print(f"Fake SOC: {args.soc}%, Voltage: {args.voltage}V")
    print()
    print("Waiting for requests... (Ctrl+C to stop)")
    print("=" * 60)

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    buffer = b""
    request_count = 0

    try:
        while True:
            # Read available data
            chunk = ser.read(100)
            if chunk:
                buffer += chunk

                # Look for complete frames (~ ... \r)
                while b'~' in buffer and b'\r' in buffer:
                    start = buffer.find(b'~')
                    end = buffer.find(b'\r', start)

                    if end == -1:
                        break

                    frame = buffer[start:end+1]
                    buffer = buffer[end+1:]

                    # Parse and respond
                    req = parse_request(frame)
                    if req:
                        request_count += 1
                        cid2_names = {
                            0x42: 'GetAnalog',
                            0x44: 'GetAlarm',
                            0x4F: 'GetSysParam',
                            0x61: 'GetMfr',
                            0x62: 'GetFirmware',
                            0x63: 'GetSerial',
                            0x90: 'GetProtocol',
                        }
                        cmd_name = cid2_names.get(req['cid2'], f"0x{req['cid2']:02X}")
                        print(f"[{request_count:4d}] RX: {req['raw']}")
                        print(f"       Addr={req['addr']} CID2={cmd_name} Info={req['info'] or '(none)'}")

                        response = handle_request(req)
                        ser.write(response)
                        ser.flush()

                        print(f"       TX: {response.decode('ascii', errors='replace').strip()}")
                        print()

            time.sleep(0.01)

    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
