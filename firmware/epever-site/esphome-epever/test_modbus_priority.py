#!/usr/bin/env python3
"""
Test script to investigate EPever inverter priority register 0x9608
"""

from pymodbus.client import ModbusTcpClient
import time

INVERTER_HOST = "10.10.0.117"
INVERTER_PORT = 9999
SLAVE_ID = 10
PRIORITY_REGISTER = 0x9608

def test_read_priority(client):
    """Test reading current priority mode"""
    print("\n" + "="*60)
    print("TEST 1: Read Register 0x9608")
    print("="*60)

    result = client.read_holding_registers(PRIORITY_REGISTER, 1, slave=SLAVE_ID)

    if result.isError():
        print(f"❌ Read failed: {result}")
        return None
    else:
        value = result.registers[0]
        mode_str = "Inverter Priority" if value == 0 else "Utility Priority"
        print(f"✓ Current mode: {value} ({mode_str})")
        return value

def test_write_single_register(client, value):
    """Test writing with function 0x06 (Write Single Register)"""
    print("\n" + "="*60)
    print(f"TEST 2: Write Single Register (0x06) - Value {value}")
    print("="*60)

    result = client.write_register(PRIORITY_REGISTER, value, slave=SLAVE_ID)

    if result.isError():
        print(f"❌ Write failed: {result}")
        return False
    else:
        print(f"✓ Write successful!")
        # Verify by reading back
        time.sleep(0.2)
        verify = client.read_holding_registers(PRIORITY_REGISTER, 1, slave=SLAVE_ID)
        if not verify.isError():
            new_value = verify.registers[0]
            mode_str = "Inverter Priority" if new_value == 0 else "Utility Priority"
            print(f"✓ Verified: {new_value} ({mode_str})")
            return True
        return True

def test_write_multiple_registers(client, value):
    """Test writing with function 0x10 (Write Multiple Registers)"""
    print("\n" + "="*60)
    print(f"TEST 3: Write Multiple Registers (0x10) - Value {value}")
    print("="*60)

    result = client.write_registers(PRIORITY_REGISTER, [value], slave=SLAVE_ID)

    if result.isError():
        print(f"❌ Write failed: {result}")
        return False
    else:
        print(f"✓ Write successful!")
        # Verify by reading back
        time.sleep(0.2)
        verify = client.read_holding_registers(PRIORITY_REGISTER, 1, slave=SLAVE_ID)
        if not verify.isError():
            new_value = verify.registers[0]
            mode_str = "Inverter Priority" if new_value == 0 else "Utility Priority"
            print(f"✓ Verified: {new_value} ({mode_str})")
            return True
        return True

def scan_nearby_registers(client):
    """Scan registers around 0x9608 to find similar ones"""
    print("\n" + "="*60)
    print("TEST 4: Scan Nearby Registers (0x9600-0x9610)")
    print("="*60)

    for addr in range(0x9600, 0x9611):
        result = client.read_holding_registers(addr, 1, slave=SLAVE_ID)
        if not result.isError():
            value = result.registers[0]
            print(f"  0x{addr:04X}: {value:5d} (0x{value:04X})")
        else:
            print(f"  0x{addr:04X}: ERROR")
        time.sleep(0.1)

def test_write_coil(client, value):
    """Test if register is actually a coil (boolean)"""
    print("\n" + "="*60)
    print(f"TEST 5: Write Single Coil (0x05) - Value {value}")
    print("="*60)

    coil_value = True if value == 1 else False
    result = client.write_coil(PRIORITY_REGISTER, coil_value, slave=SLAVE_ID)

    if result.isError():
        print(f"❌ Write failed: {result}")
        return False
    else:
        print(f"✓ Write successful!")
        return True

def main():
    print("EPever Inverter Priority Register Test")
    print(f"Target: {INVERTER_HOST}:{INVERTER_PORT}")
    print(f"Slave: {SLAVE_ID}")
    print(f"Register: 0x{PRIORITY_REGISTER:04X}")

    # Connect with longer timeout
    client = ModbusTcpClient(
        INVERTER_HOST,
        port=INVERTER_PORT,
        timeout=3,
        retries=3,
        retry_on_empty=True
    )

    try:
        connected = client.connect()
        print(f"Connect result: {connected}")

        if not connected:
            print("❌ Failed to connect to inverter")
            return

        print("✓ Connected to inverter")
        print(f"  Socket: {client.socket}")

        # Small delay after connection
        time.sleep(0.5)

        # Test 1: Read current value
        current_value = test_read_priority(client)
        if current_value is None:
            print("\n❌ Cannot read register, aborting")
            return

        # Determine what value to write (opposite of current)
        write_value = 1 if current_value == 0 else 0
        write_mode_str = "Utility Priority" if write_value == 1 else "Inverter Priority"

        print(f"\nWill attempt to change mode to: {write_value} ({write_mode_str})")
        input("\nPress ENTER to continue with write tests (or Ctrl+C to abort)...")

        # Test 2: Write Single Register (function 0x06)
        success_single = test_write_single_register(client, write_value)

        if not success_single:
            # Test 3: Write Multiple Registers (function 0x10)
            test_write_multiple_registers(client, write_value)

        # Test 4: Scan nearby registers
        print("\nScanning nearby registers for comparison...")
        input("Press ENTER to continue...")
        scan_nearby_registers(client)

        # Test 5: Try as coil
        print("\nTrying as coil (boolean) instead of holding register...")
        input("Press ENTER to continue...")
        test_write_coil(client, write_value)

        # Final read to show current state
        print("\n" + "="*60)
        print("FINAL STATE")
        print("="*60)
        test_read_priority(client)

    finally:
        client.close()
        print("\n✓ Connection closed")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n⚠️  Test interrupted by user")
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()
