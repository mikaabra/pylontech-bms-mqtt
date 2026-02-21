#!/usr/bin/env python3
"""
Test script for EPever inverter using Modbus RTU over TCP
(Raw RTU frames over TCP socket, not Modbus TCP protocol)
"""

import socket
import struct
import time

INVERTER_HOST = "10.10.0.117"
INVERTER_PORT = 9999
SLAVE_ID = 10
PRIORITY_REGISTER = 0x9608

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
    """Build Modbus RTU read holding registers command"""
    cmd = bytes([
        slave,
        0x03,  # Function: Read Holding Registers
        (register >> 8) & 0xFF,  # Register high byte
        register & 0xFF,         # Register low byte
        (count >> 8) & 0xFF,     # Count high byte
        count & 0xFF             # Count low byte
    ])
    crc = calc_crc16_modbus(cmd)
    cmd += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return cmd

def build_write_command(slave, register, value):
    """Build Modbus RTU write single register command"""
    cmd = bytes([
        slave,
        0x06,  # Function: Write Single Register
        (register >> 8) & 0xFF,  # Register high byte
        register & 0xFF,         # Register low byte
        (value >> 8) & 0xFF,     # Value high byte
        value & 0xFF             # Value low byte
    ])
    crc = calc_crc16_modbus(cmd)
    cmd += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return cmd

def build_write_multiple_command(slave, register, values):
    """Build Modbus RTU write multiple registers command"""
    count = len(values)
    byte_count = count * 2
    cmd = bytes([
        slave,
        0x10,  # Function: Write Multiple Registers
        (register >> 8) & 0xFF,  # Register high byte
        register & 0xFF,         # Register low byte
        (count >> 8) & 0xFF,     # Count high byte
        count & 0xFF,            # Count low byte
        byte_count               # Byte count
    ])
    for value in values:
        cmd += bytes([(value >> 8) & 0xFF, value & 0xFF])

    crc = calc_crc16_modbus(cmd)
    cmd += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return cmd

def send_modbus_rtu(sock, command):
    """Send Modbus RTU command and receive response"""
    sock.send(command)
    time.sleep(0.2)  # Give inverter time to respond
    response = sock.recv(1024)
    return response

def test_read_priority():
    """Test reading current priority mode"""
    print("\n" + "="*60)
    print("TEST 1: Read Register 0x9608")
    print("="*60)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((INVERTER_HOST, INVERTER_PORT))
        print(f"✓ Connected to {INVERTER_HOST}:{INVERTER_PORT}")

        cmd = build_read_command(SLAVE_ID, PRIORITY_REGISTER, 1)
        print(f"TX: {' '.join(f'{b:02X}' for b in cmd)}")

        response = send_modbus_rtu(sock, cmd)
        print(f"RX: {' '.join(f'{b:02X}' for b in response)} ({len(response)} bytes)")

        if len(response) >= 5:
            slave = response[0]
            func = response[1]

            # Check for exception
            if func & 0x80:
                exception_code = response[2]
                exception_names = {
                    1: "Illegal Function",
                    2: "Illegal Data Address",
                    3: "Illegal Data Value",
                    4: "Slave Device Failure"
                }
                print(f"❌ Modbus Exception: {exception_names.get(exception_code, 'Unknown')} (code {exception_code})")
                return None

            if len(response) >= 7 and slave == SLAVE_ID and (func == 0x03 or func == 0x04):
                byte_count = response[2]
                value = (response[3] << 8) | response[4]
                mode_str = "Inverter Priority" if value == 0 else "Utility Priority"
                if func == 0x04:
                    print(f"⚠️  Note: Response used function 0x04 (Read Input) instead of 0x03 (Read Holding)")
                print(f"✓ Value: {value} (0x{value:04X})")
                if value == 0 or value == 1:
                    print(f"✓ Mode: {mode_str}")
                else:
                    print(f"⚠️  Unexpected value for priority (expected 0 or 1)")
                return value

        print(f"❌ Unexpected response (slave={slave}, func=0x{func:02X})")
        return None

    finally:
        sock.close()

def test_write_single_register(value):
    """Test writing with function 0x06"""
    print("\n" + "="*60)
    print(f"TEST 2: Write Single Register (0x06) - Value {value}")
    print("="*60)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((INVERTER_HOST, INVERTER_PORT))

        cmd = build_write_command(SLAVE_ID, PRIORITY_REGISTER, value)
        print(f"TX: {' '.join(f'{b:02X}' for b in cmd)}")

        response = send_modbus_rtu(sock, cmd)
        print(f"RX: {' '.join(f'{b:02X}' for b in response)} ({len(response)} bytes)")

        if len(response) >= 5:
            slave = response[0]
            func = response[1]

            # Check for exception
            if func & 0x80:
                exception_code = response[2]
                exception_names = {
                    1: "Illegal Function",
                    2: "Illegal Data Address",
                    3: "Illegal Data Value",
                    4: "Slave Device Failure"
                }
                print(f"❌ Modbus Exception: {exception_names.get(exception_code, 'Unknown')} (code {exception_code})")
                return False

            if len(response) >= 8 and slave == SLAVE_ID and func == 0x06:
                print(f"✓ Write successful!")
                return True

        print(f"❌ Unexpected response")
        return False

    finally:
        sock.close()

def test_write_multiple_registers(value):
    """Test writing with function 0x10"""
    print("\n" + "="*60)
    print(f"TEST 3: Write Multiple Registers (0x10) - Value {value}")
    print("="*60)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((INVERTER_HOST, INVERTER_PORT))

        cmd = build_write_multiple_command(SLAVE_ID, PRIORITY_REGISTER, [value])
        print(f"TX: {' '.join(f'{b:02X}' for b in cmd)}")

        response = send_modbus_rtu(sock, cmd)
        print(f"RX: {' '.join(f'{b:02X}' for b in response)} ({len(response)} bytes)")

        if len(response) >= 5:
            slave = response[0]
            func = response[1]

            # Check for exception
            if func & 0x80:
                exception_code = response[2]
                exception_names = {
                    1: "Illegal Function",
                    2: "Illegal Data Address",
                    3: "Illegal Data Value",
                    4: "Slave Device Failure"
                }
                print(f"❌ Modbus Exception: {exception_names.get(exception_code, 'Unknown')} (code {exception_code})")
                return False

            if len(response) >= 8 and slave == SLAVE_ID and func == 0x10:
                print(f"✓ Write successful!")
                return True

        print(f"❌ Unexpected response")
        return False

    finally:
        sock.close()

def scan_nearby_registers():
    """Scan registers around 0x9608"""
    print("\n" + "="*60)
    print("TEST 4: Scan Nearby Registers (0x9600-0x9610)")
    print("="*60)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((INVERTER_HOST, INVERTER_PORT))

        for addr in range(0x9600, 0x9611):
            cmd = build_read_command(SLAVE_ID, addr, 1)
            try:
                response = send_modbus_rtu(sock, cmd)

                if len(response) >= 7:
                    slave = response[0]
                    func = response[1]

                    if func & 0x80:
                        print(f"  0x{addr:04X}: ERROR (exception {response[2]})")
                    elif slave == SLAVE_ID and func == 0x03:
                        value = (response[3] << 8) | response[4]
                        print(f"  0x{addr:04X}: {value:5d} (0x{value:04X})")
                    else:
                        print(f"  0x{addr:04X}: UNEXPECTED RESPONSE")
            except Exception as e:
                print(f"  0x{addr:04X}: TIMEOUT/ERROR")

            time.sleep(0.2)

    finally:
        sock.close()

def main():
    print("EPever Inverter Priority Register Test (Modbus RTU over TCP)")
    print(f"Target: {INVERTER_HOST}:{INVERTER_PORT}")
    print(f"Slave: {SLAVE_ID}")
    print(f"Register: 0x{PRIORITY_REGISTER:04X}")

    # Test 1: Read current value
    current_value = test_read_priority()
    if current_value is None:
        print("\n❌ Cannot read register, aborting")
        return

    # Determine what value to write (opposite of current)
    write_value = 1 if current_value == 0 else 0
    write_mode_str = "Utility Priority" if write_value == 1 else "Inverter Priority"

    print(f"\nWill attempt to change mode to: {write_value} ({write_mode_str})")
    input("\nPress ENTER to continue with write tests (or Ctrl+C to abort)...")

    # Test 2: Write Single Register (function 0x06)
    success_single = test_write_single_register(write_value)

    if not success_single:
        # Test 3: Write Multiple Registers (function 0x10)
        test_write_multiple_registers(write_value)

    # Test 4: Scan nearby registers
    print("\nScanning nearby registers...")
    input("Press ENTER to continue...")
    scan_nearby_registers()

    # Final read
    print("\n" + "="*60)
    print("FINAL STATE")
    print("="*60)
    test_read_priority()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n⚠️  Test interrupted by user")
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()
