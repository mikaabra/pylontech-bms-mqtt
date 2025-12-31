#!/usr/bin/env python3
"""
Simple RS485 Pylontech protocol tester.
Sends raw commands and shows responses.
"""

import sys
import time
import serial

def calc_checksum(data: bytes) -> str:
    """Calculate Pylontech checksum."""
    total = sum(data)
    checksum = (~total + 1) & 0xFFFF
    return f"{checksum:04X}"

def make_command(addr: int, cid2: int, info: str = "") -> bytes:
    """Build a Pylontech command frame."""
    ver = "20"
    adr = f"{addr:02X}"
    cid1 = "46"
    cid2_hex = f"{cid2:02X}"
    length = f"{len(info):04X}"

    # Data for checksum (without SOI/EOI)
    frame_data = f"{ver}{adr}{cid1}{cid2_hex}{length}{info}"

    # Calculate length checksum (LCHKSUM)
    len_sum = sum(int(c, 16) for c in length)
    lchksum = ((~len_sum) + 1) & 0xF
    length_with_check = f"{lchksum:X}{length[1:]}"

    # Recalculate frame with LCHKSUM
    frame_data = f"{ver}{adr}{cid1}{cid2_hex}{length_with_check}{info}"
    checksum = calc_checksum(frame_data.encode('ascii'))

    frame = f"~{frame_data}{checksum}\r"
    return frame.encode('ascii')

def decode_response(data: bytes) -> dict:
    """Decode a Pylontech response frame."""
    try:
        text = data.decode('ascii').strip()
        if not text.startswith('~') or not text.endswith('\r'):
            text = text.replace('\r', '')

        # Remove ~ prefix
        text = text.lstrip('~').rstrip('\r')

        result = {
            'raw': data.hex(' '),
            'ascii': text,
            'ver': text[0:2],
            'addr': int(text[2:4], 16),
            'cid1': text[4:6],
            'rtn': text[6:8],
            'length': text[8:12],
            'data': text[12:-4] if len(text) > 16 else '',
            'checksum': text[-4:] if len(text) >= 4 else ''
        }

        # Decode RTN code
        rtn_codes = {
            '00': 'Normal',
            '01': 'VER error',
            '02': 'CHKSUM error',
            '03': 'LCHKSUM error',
            '04': 'CID2 invalid',
            '05': 'Command format error',
            '06': 'Info data invalid',
            '07': 'Address error / No data',
            '80': 'ADR error (unknown address)',
            '81': 'CID2 error (invalid for ADR)',
        }
        result['rtn_meaning'] = rtn_codes.get(result['rtn'], f'Unknown ({result["rtn"]})')

        return result
    except Exception as e:
        return {'error': str(e), 'raw': data.hex(' ')}

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
    baud = 9600  # We know this works

    print(f"Simple Pylontech RS485 Tester")
    print(f"Port: {port}, Baud: {baud}")
    print("="*60)

    ser = serial.Serial(port, baud, timeout=1.0)
    ser.reset_input_buffer()

    # Commands to try
    # CID2 values:
    # 0x42 = Get analog value (cell voltages, etc)
    # 0x44 = Get alarm info
    # 0x47 = Get system parameter
    # 0x4F = Get protocol version
    # 0x51 = Get manufacturer info
    # 0x92 = Get serial number
    # 0x93 = Get firmware version

    commands = [
        (0, 0x4F, "Protocol version (broadcast)"),
        (1, 0x4F, "Protocol version addr 1"),
        (2, 0x4F, "Protocol version addr 2"),
        (0, 0x42, "Analog data (broadcast)"),
        (1, 0x42, "Analog data addr 1"),
        (2, 0x42, "Analog data addr 2"),
        (3, 0x42, "Analog data addr 3"),
        (1, 0x51, "Manufacturer info addr 1"),
        (2, 0x51, "Manufacturer info addr 2"),
        (1, 0x93, "Firmware version addr 1"),
    ]

    for addr, cid2, desc in commands:
        cmd = make_command(addr, cid2)
        print(f"\n[{desc}]")
        print(f"  TX: {cmd.hex(' ')}")
        print(f"      {cmd.decode('ascii').strip()}")

        ser.reset_input_buffer()
        ser.write(cmd)
        ser.flush()
        time.sleep(0.3)

        if ser.in_waiting:
            response = ser.read(ser.in_waiting)
            decoded = decode_response(response)
            print(f"  RX: {decoded.get('raw', 'N/A')}")
            if 'error' not in decoded:
                print(f"      Addr={decoded['addr']}, RTN={decoded['rtn']} ({decoded['rtn_meaning']})")
                if decoded['data']:
                    print(f"      Data: {decoded['data']}")
        else:
            print("  RX: (no response)")

    ser.close()

    print("\n" + "="*60)
    print("If all responses show RTN=07 (Address error/No data),")
    print("you're likely connected to a SLAVE battery.")
    print("Try connecting to the MASTER battery's RS485 port instead.")

if __name__ == "__main__":
    main()
