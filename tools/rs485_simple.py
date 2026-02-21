#!/usr/bin/env python3
"""
Simple RS485 Pylontech protocol tester.
Sends raw commands and shows responses.
"""

import sys
import time
import serial

def calc_lchksum(length: int) -> int:
    """Calculate LENGTH checksum (sum of ASCII nibbles, mod 16, inverted)."""
    length_hex = f"{length:04X}"
    total = sum(int(c, 16) for c in length_hex)
    return (~total + 1) & 0xF

def calc_chksum(data: str) -> str:
    """Calculate frame checksum (sum of ASCII bytes, inverted + 1)."""
    total = sum(ord(c) for c in data)
    checksum = (~total + 1) & 0xFFFF
    return f"{checksum:04X}"

def make_command(addr: int, cid2: int, info: str = "") -> bytes:
    """Build a Pylontech command frame."""
    ver = "20"
    adr = f"{addr:02X}"
    cid1 = "46"
    cid2_hex = f"{cid2:02X}"

    # Calculate LENID with LCHKSUM
    info_len = len(info)
    lchksum = calc_lchksum(info_len)
    lenid = f"{lchksum:X}{info_len:03X}"

    # Build frame content (for checksum calculation)
    frame_content = f"{ver}{adr}{cid1}{cid2_hex}{lenid}{info}"

    # Calculate checksum
    checksum = calc_chksum(frame_content)

    # Complete frame
    frame = f"~{frame_content}{checksum}\r"
    return frame.encode('ascii')

def decode_response(data: bytes) -> dict:
    """Decode a Pylontech response frame."""
    try:
        text = data.decode('ascii').strip()
        text = text.lstrip('~').rstrip('\r')

        result = {
            'raw': data.hex(' '),
            'ascii': '~' + text,
            'ver': text[0:2],
            'addr': int(text[2:4], 16),
            'cid1': text[4:6],
            'rtn': text[6:8],
            'length': text[8:12],
            'info': text[12:-4] if len(text) > 16 else '',
            'checksum': text[-4:] if len(text) >= 4 else ''
        }

        # Decode RTN code
        rtn_codes = {
            '00': 'OK - Normal',
            '01': 'VER error',
            '02': 'CHKSUM error',
            '03': 'LCHKSUM error',
            '04': 'CID2 invalid',
            '05': 'Command format error',
            '06': 'Info data invalid',
            '07': 'No data (slave/address error)',
            '80': 'ADR error',
            '81': 'CID2 error',
            '82': 'Communication error',
            '83': 'Data length error',
            '84': 'Precharge MOS error',
        }
        result['rtn_meaning'] = rtn_codes.get(result['rtn'], f'Unknown ({result["rtn"]})')

        return result
    except Exception as e:
        return {'error': str(e), 'raw': data.hex(' ')}

def decode_analog_data(info: str, num_cells: int = 16) -> dict:
    """Decode analog value response (CID2=0x42 response)."""
    try:
        result = {}
        i = 0

        # Number of module
        if len(info) >= i + 2:
            result['modules'] = int(info[i:i+2], 16)
            i += 2

        # For each module's cell data...
        cells = []
        for cell in range(num_cells):
            if len(info) >= i + 4:
                voltage_mv = int(info[i:i+4], 16)
                cells.append(voltage_mv / 1000.0)
                i += 4

        if cells:
            result['cell_voltages'] = cells
            result['cell_min'] = min(cells)
            result['cell_max'] = max(cells)
            result['cell_delta_mv'] = (max(cells) - min(cells)) * 1000

        # Temperature count and values
        if len(info) >= i + 2:
            temp_count = int(info[i:i+2], 16)
            i += 2
            temps = []
            for t in range(temp_count):
                if len(info) >= i + 4:
                    temp_raw = int(info[i:i+4], 16)
                    # Kelvin to Celsius (offset varies by manufacturer)
                    temp_c = (temp_raw - 2731) / 10.0
                    temps.append(temp_c)
                    i += 4
            result['temperatures'] = temps

        # Current (signed, 10mA units)
        if len(info) >= i + 4:
            current_raw = int(info[i:i+4], 16)
            if current_raw > 0x7FFF:
                current_raw -= 0x10000
            result['current'] = current_raw / 100.0
            i += 4

        # Total voltage (mV)
        if len(info) >= i + 4:
            result['total_voltage'] = int(info[i:i+4], 16) / 1000.0
            i += 4

        # Remaining capacity (10mAh)
        if len(info) >= i + 4:
            result['remain_cap_ah'] = int(info[i:i+4], 16) / 100.0
            i += 4

        # Custom field
        if len(info) >= i + 2:
            result['custom'] = int(info[i:i+2], 16)
            i += 2

        # Total capacity (10mAh)
        if len(info) >= i + 4:
            result['total_cap_ah'] = int(info[i:i+4], 16) / 100.0
            i += 4

        # Cycle count
        if len(info) >= i + 4:
            result['cycles'] = int(info[i:i+4], 16)
            i += 4

        return result

    except Exception as e:
        return {'decode_error': str(e), 'raw_info': info}

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
    baud = 9600

    print(f"Simple Pylontech RS485 Tester")
    print(f"Port: {port}, Baud: {baud}")
    print("="*60)

    ser = serial.Serial(port, baud, timeout=1.0)
    ser.reset_input_buffer()

    # Commands to try - using corrected checksum
    commands = [
        (2, 0x42, "", "Analog data addr 2"),
        (1, 0x42, "", "Analog data addr 1"),
        (3, 0x42, "", "Analog data addr 3"),
        (0, 0x42, "", "Analog data broadcast"),
        (2, 0x44, "", "Alarm info addr 2"),
        (2, 0x4F, "", "Protocol version addr 2"),
        (2, 0x51, "", "Manufacturer info addr 2"),
    ]

    for addr, cid2, info, desc in commands:
        cmd = make_command(addr, cid2, info)
        print(f"\n[{desc}]")
        print(f"  TX: {cmd.decode('ascii').strip()}")

        ser.reset_input_buffer()
        ser.write(cmd)
        ser.flush()
        time.sleep(0.5)

        if ser.in_waiting:
            response = ser.read(min(ser.in_waiting, 512))
            decoded = decode_response(response)
            print(f"  RX: {decoded.get('ascii', decoded.get('raw', 'N/A'))}")

            if 'error' not in decoded:
                print(f"      RTN={decoded['rtn']} ({decoded['rtn_meaning']})")

                # If successful and has data, try to decode analog values
                if decoded['rtn'] == '00' and decoded['info'] and cid2 == 0x42:
                    analog = decode_analog_data(decoded['info'])
                    print(f"\n      === DECODED DATA ===")
                    if 'cell_voltages' in analog:
                        print(f"      Cell voltages ({len(analog['cell_voltages'])} cells):")
                        for i, v in enumerate(analog['cell_voltages'], 1):
                            print(f"        Cell {i:2d}: {v:.3f} V")
                        print(f"      Min: {analog['cell_min']:.3f} V, Max: {analog['cell_max']:.3f} V")
                        print(f"      Delta: {analog['cell_delta_mv']:.1f} mV")
                    if 'temperatures' in analog:
                        print(f"      Temperatures: {analog['temperatures']}")
                    if 'current' in analog:
                        print(f"      Current: {analog['current']:.2f} A")
                    if 'total_voltage' in analog:
                        print(f"      Total voltage: {analog['total_voltage']:.2f} V")
                    if 'remain_cap_ah' in analog:
                        print(f"      Remaining: {analog['remain_cap_ah']:.2f} Ah")
                    if 'cycles' in analog:
                        print(f"      Cycles: {analog['cycles']}")
        else:
            print("  RX: (no response)")

    ser.close()
    print("\n" + "="*60)

if __name__ == "__main__":
    main()
