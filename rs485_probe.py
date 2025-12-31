#!/usr/bin/env python3
"""
RS485 probe script for Pylontech-compatible batteries.

Usage:
    ./rs485_probe.py /dev/ttyUSB0
    ./rs485_probe.py /dev/ttyUSB0 --baud 115200
    ./rs485_probe.py /dev/ttyUSB0 --raw
"""

import sys
import time
import argparse
import serial
import serial.tools.list_ports


def probe_pylontech_library(port: str, baud: int = 115200):
    """Try the python-pylontech library protocol."""
    try:
        from pylontech import PylontechStack
        print(f"\n{'='*60}")
        print(f"Testing python-pylontech library...")
        print(f"Port: {port}, Baud: {baud}")
        print('='*60)

        # Try to connect and poll
        print("\nConnecting to battery stack...")
        stack = PylontechStack(device=port, baud=baud, manualBattcountLimit=5)

        print("Polling batteries (this may take 10-20 seconds)...")
        result = stack.update()

        if not result:
            print("No response from batteries")
            return False

        print(f"\nStack result keys: {list(result.keys())}")

        # Print summary info
        if 'Voltage' in result:
            print(f"\nStack Voltage: {result.get('Voltage', 'N/A')} V")
        if 'Current' in result:
            print(f"Stack Current: {result.get('Current', 'N/A')} A")
        if 'StateOfCharge' in result:
            print(f"Stack SOC: {result.get('StateOfCharge', 'N/A')} %")
        if 'battCount' in result:
            print(f"Battery count: {result.get('battCount', 'N/A')}")

        # Try to get cell voltages
        if 'battList' in result:
            for i, batt in enumerate(result['battList']):
                print(f"\n--- Battery {i} ---")
                if isinstance(batt, dict):
                    for key, val in batt.items():
                        if 'cell' in key.lower() or 'volt' in key.lower():
                            print(f"  {key}: {val}")

        return True

    except ImportError as e:
        print(f"pylontech library import error: {e}")
        print("Try: pip3 install pylontech --break-system-packages")
        return False
    except Exception as e:
        print(f"pylontech library error: {e}")
        import traceback
        traceback.print_exc()
        return False


def probe_raw_pylontech(port: str, baud: int = 115200):
    """Send raw Pylontech protocol commands."""
    print(f"\n{'='*60}")
    print(f"Raw Pylontech protocol probe at {baud} baud...")
    print('='*60)

    try:
        from pylontech import PylontechRS485, PylontechEncode, PylontechDecode

        rs485 = PylontechRS485(device=port, baud=baud)

        # Try to get analog data (command 0x42) from address 2
        # Address 2 is typically the first battery in the stack
        print("\nSending analog data request to address 2...")

        encoder = PylontechEncode()
        # Command 0x42 = Get analog values
        cmd = encoder.getAnalogValue(battNumber=2, group=0)
        print(f"  Command: {cmd.hex(' ')}")

        rs485.send(cmd)
        time.sleep(0.5)
        response = rs485.receive(timeout=2)

        if response:
            print(f"  Response ({len(response)} bytes): {bytes(response).hex(' ')}")

            # Try to decode
            decoder = PylontechDecode()
            try:
                decoded = decoder.decodeAnalogValue(response)
                print(f"  Decoded: {decoded}")
            except Exception as e:
                print(f"  Decode error: {e}")
        else:
            print("  No response")

        # Try different addresses
        for addr in [1, 2, 3, 0]:
            print(f"\nTrying address {addr}...")
            cmd = encoder.getAnalogValue(battNumber=addr, group=0)
            rs485.send(cmd)
            time.sleep(0.3)
            response = rs485.receive(timeout=1)
            if response:
                print(f"  Got response from address {addr}!")
                print(f"  Raw: {bytes(response).hex(' ')}")
                break

        rs485.close()

    except Exception as e:
        print(f"Raw probe error: {e}")
        import traceback
        traceback.print_exc()


def probe_raw_serial(port: str):
    """
    Raw serial probe - try common baud rates and send test commands.
    """
    print(f"\n{'='*60}")
    print("Raw serial probe (all baud rates)...")
    print('='*60)

    baud_rates = [115200, 9600, 19200, 4800, 1200]

    # Common commands to try
    test_commands = [
        (b'\r\n', "Empty line"),
        (b'pwr\r\n', "Pylontech console: pwr"),
        (b'info\r\n', "Pylontech console: info"),
        (b'bat\r\n', "Pylontech console: bat"),
        # Pylontech RS485 protocol - get analog values for battery 2
        (bytes.fromhex('7E3230303234363432453030323031464433370D'), "Pylontech RS485: Analog addr 2"),
    ]

    for baud in baud_rates:
        print(f"\n--- Trying {baud} baud ---")
        try:
            ser = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1.0
            )

            # Flush buffers
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.1)

            # Check if there's any unsolicited data
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                print(f"  Unsolicited data: {data.hex(' ')} | {repr(data)}")

            # Try each command
            for cmd, desc in test_commands:
                ser.reset_input_buffer()
                ser.write(cmd)
                ser.flush()
                time.sleep(0.5)

                if ser.in_waiting:
                    response = ser.read(min(ser.in_waiting, 256))
                    print(f"  [{desc}] Response ({len(response)} bytes):")
                    print(f"    HEX: {response.hex(' ')}")
                    try:
                        ascii_str = response.decode('ascii', errors='replace')
                        if any(c.isalnum() for c in ascii_str):
                            print(f"    ASCII: {repr(ascii_str)}")
                    except:
                        pass

            ser.close()

        except Exception as e:
            print(f"  Error at {baud} baud: {e}")


def main():
    parser = argparse.ArgumentParser(description='RS485 probe for Pylontech batteries')
    parser.add_argument('port', help='Serial port (e.g., /dev/ttyUSB0)')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--raw', action='store_true', help='Raw serial probe only')
    args = parser.parse_args()

    print(f"RS485 Probe - Port: {args.port}")
    print(f"Make sure RS485 dongle is connected to the MASTER battery RS485 port")
    print(f"(Slave batteries typically don't respond - only master does!)")
    print(f"RS485 wiring: A+ <-> A+, B- <-> B-, GND optional")

    # Check port exists
    try:
        ser = serial.Serial(args.port, 9600, timeout=0.1)
        ser.close()
    except Exception as e:
        print(f"\nError opening {args.port}: {e}")
        print("\nAvailable serial ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        sys.exit(1)

    if args.raw:
        probe_raw_serial(args.port)
    else:
        # Try pylontech library first
        success = probe_pylontech_library(args.port, args.baud)

        if not success:
            print("\n" + "="*60)
            print("Library method didn't work. Trying raw protocol...")
            print("="*60)
            probe_raw_pylontech(args.port, args.baud)

            print("\n" + "="*60)
            print("Trying raw serial at all baud rates...")
            print("="*60)
            probe_raw_serial(args.port)

    print("\n" + "="*60)
    print("TROUBLESHOOTING:")
    print("="*60)
    print("1. Connect to MASTER battery, not slave")
    print("2. Check A+/B- wiring (try swapping if no response)")
    print("3. Some batteries need the DIP switch set for RS485 baud rate")
    print("4. Try both 9600 and 115200: ./rs485_probe.py /dev/ttyUSB0 --baud 9600")


if __name__ == "__main__":
    main()
