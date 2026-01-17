#!/usr/bin/env python3
"""
Generic Modbus RTU over TCP tool for EPever inverters
Supports reading and writing holding registers

Examples:
    # Read register 0x9608
    ./modbus_rtu_tcp.py 0x9608

    # Write value 1 to register 0x9608
    ./modbus_rtu_tcp.py 0x9608 -w 1

    # Read multiple registers starting at 0x9000
    ./modbus_rtu_tcp.py 0x9000 -c 5

    # Use custom host/port/slave
    ./modbus_rtu_tcp.py 0x9608 --host 10.10.0.200 --port 502 --slave 1
"""

import socket
import struct
import time
import argparse
import sys

def calc_crc16_modbus(data):
    """Calculate Modbus CRC16"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc = crc >> 1
    return crc

def build_read_command(slave, register, count=1):
    """Build Modbus RTU read holding registers command (function 0x03)"""
    cmd = bytes([
        slave,
        0x03,  # Function: Read Holding Registers
        (register >> 8) & 0xFF,
        register & 0xFF,
        (count >> 8) & 0xFF,
        count & 0xFF
    ])
    crc = calc_crc16_modbus(cmd)
    cmd += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return cmd

def build_write_command(slave, register, value):
    """Build Modbus RTU write multiple registers command (function 0x10)"""
    count = 1
    byte_count = count * 2
    cmd = bytes([
        slave,
        0x10,  # Function: Write Multiple Registers
        (register >> 8) & 0xFF,
        register & 0xFF,
        (count >> 8) & 0xFF,
        count & 0xFF,
        byte_count
    ])
    # Add register value (16-bit big-endian)
    cmd += bytes([(value >> 8) & 0xFF, value & 0xFF])

    crc = calc_crc16_modbus(cmd)
    cmd += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return cmd

def send_modbus_rtu(sock, command):
    """Send Modbus RTU command and receive response"""
    sock.send(command)
    time.sleep(0.1)  # Give device time to respond
    response = sock.recv(1024)
    return response

def parse_response(response, slave_id, expected_func):
    """Parse Modbus RTU response"""
    if len(response) < 5:
        return None, f"Response too short: {len(response)} bytes"

    resp_slave = response[0]
    resp_func = response[1]

    # Check for exception response
    if resp_func & 0x80:
        exception_code = response[2]
        exception_names = {
            1: "Illegal Function",
            2: "Illegal Data Address",
            3: "Illegal Data Value",
            4: "Slave Device Failure"
        }
        exception_msg = exception_names.get(exception_code, f"Unknown ({exception_code})")
        return None, f"Modbus Exception: {exception_msg}"

    # Check slave ID
    if resp_slave != slave_id:
        return None, f"Wrong slave ID: got {resp_slave}, expected {slave_id}"

    # Check function code (accept both 0x03 and 0x04 for reads)
    if expected_func == 0x03 and resp_func not in [0x03, 0x04]:
        return None, f"Wrong function code: got 0x{resp_func:02X}, expected 0x03 or 0x04"
    elif expected_func != 0x03 and resp_func != expected_func:
        return None, f"Wrong function code: got 0x{resp_func:02X}, expected 0x{expected_func:02X}"

    return response, None

def read_registers(host, port, slave, register, count=1, verbose=False):
    """Read holding registers from Modbus device"""
    cmd = build_read_command(slave, register, count)

    if verbose:
        print(f"TX: {' '.join(f'{b:02X}' for b in cmd)}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((host, port))
        response = send_modbus_rtu(sock, cmd)

        if verbose:
            print(f"RX: {' '.join(f'{b:02X}' for b in response)} ({len(response)} bytes)")

        parsed, error = parse_response(response, slave, 0x03)
        if error:
            print(f"❌ Error: {error}")
            return None

        # Extract register values
        if len(response) >= 5:
            byte_count = response[2]
            values = []
            for i in range(0, byte_count, 2):
                if i + 3 < len(response):
                    value = (response[3 + i] << 8) | response[4 + i]
                    values.append(value)

            return values

        return None

    except socket.timeout:
        print("❌ Timeout waiting for response")
        return None
    except Exception as e:
        print(f"❌ Error: {e}")
        return None
    finally:
        sock.close()

def write_register(host, port, slave, register, value, verbose=False):
    """Write to holding register on Modbus device"""
    cmd = build_write_command(slave, register, value)

    if verbose:
        print(f"TX: {' '.join(f'{b:02X}' for b in cmd)}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((host, port))
        response = send_modbus_rtu(sock, cmd)

        if verbose:
            print(f"RX: {' '.join(f'{b:02X}' for b in response)} ({len(response)} bytes)")

        parsed, error = parse_response(response, slave, 0x10)
        if error:
            print(f"❌ Error: {error}")
            return False

        # Verify response
        if len(response) >= 8:
            resp_register = (response[2] << 8) | response[3]
            resp_count = (response[4] << 8) | response[5]

            if resp_register == register and resp_count == 1:
                return True

        return False

    except socket.timeout:
        print("❌ Timeout waiting for response")
        return False
    except Exception as e:
        print(f"❌ Error: {e}")
        return False
    finally:
        sock.close()

def main():
    parser = argparse.ArgumentParser(
        description='Generic Modbus RTU over TCP tool',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s 0x9608                    # Read register 0x9608
  %(prog)s 0x9608 -w 1               # Write value 1 to register 0x9608
  %(prog)s 0x9000 -c 5               # Read 5 registers starting at 0x9000
  %(prog)s 0x9608 --host 10.10.0.200 # Use custom host
        """
    )

    parser.add_argument('register', type=str,
                        help='Register address (hex: 0x9608 or decimal: 38408)')
    parser.add_argument('-w', '--write', type=int, metavar='VALUE',
                        help='Write value to register (0-65535)')
    parser.add_argument('-c', '--count', type=int, default=1,
                        help='Number of registers to read (default: 1)')
    parser.add_argument('--host', type=str, default='10.10.0.117',
                        help='Modbus TCP host (default: 10.10.0.117)')
    parser.add_argument('--port', type=int, default=9999,
                        help='Modbus TCP port (default: 9999)')
    parser.add_argument('--slave', type=int, default=10,
                        help='Modbus slave ID (default: 10)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Show raw Modbus frames')

    args = parser.parse_args()

    # Parse register address (support both hex and decimal)
    try:
        if args.register.startswith('0x') or args.register.startswith('0X'):
            register = int(args.register, 16)
        else:
            register = int(args.register)
    except ValueError:
        print(f"❌ Invalid register address: {args.register}")
        sys.exit(1)

    # Validate register address
    if not (0 <= register <= 0xFFFF):
        print(f"❌ Register address out of range: 0x{register:04X} (must be 0x0000-0xFFFF)")
        sys.exit(1)

    # Print connection info
    print(f"Modbus RTU over TCP")
    print(f"  Host: {args.host}:{args.port}")
    print(f"  Slave: {args.slave}")
    print(f"  Register: 0x{register:04X} ({register})")
    print()

    if args.write is not None:
        # Write mode
        if not (0 <= args.write <= 0xFFFF):
            print(f"❌ Value out of range: {args.write} (must be 0-65535)")
            sys.exit(1)

        print(f"Writing value {args.write} (0x{args.write:04X})...")
        success = write_register(args.host, args.port, args.slave, register, args.write, args.verbose)

        if success:
            print(f"✓ Write successful!")

            # Read back to verify
            print("\nReading back to verify...")
            values = read_registers(args.host, args.port, args.slave, register, 1, args.verbose)
            if values:
                print(f"✓ Verified: {values[0]} (0x{values[0]:04X})")
        else:
            print("❌ Write failed")
            sys.exit(1)

    else:
        # Read mode
        if args.count < 1 or args.count > 125:
            print(f"❌ Count out of range: {args.count} (must be 1-125)")
            sys.exit(1)

        print(f"Reading {args.count} register(s)...")
        values = read_registers(args.host, args.port, args.slave, register, args.count, args.verbose)

        if values:
            print(f"✓ Success!")
            print()
            for i, value in enumerate(values):
                reg_addr = register + i
                print(f"  0x{reg_addr:04X} ({reg_addr:5d}): {value:5d} (0x{value:04X})")
        else:
            sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n⚠️  Interrupted by user")
        sys.exit(130)
