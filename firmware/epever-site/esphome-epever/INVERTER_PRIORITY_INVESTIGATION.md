# Inverter Priority Register Investigation

## Problem Statement

**Objective**: Change EPever UP5000 inverter output priority (Inverter/Battery vs Utility/Grid) via Modbus

**Current Status**:
- ✅ Can READ current mode from register 0x9608
- ❌ Cannot WRITE to register 0x9608 (returns Modbus Exception 0x01 "Illegal Function")

## Test Results

### Successful READ (2026-01-16)
```
Command:  0A 03 96 08 00 01 29 3B
Response: 0A 03 02 00 00 1D 85
Result:   Current mode = 0 (Inverter Priority)
```

### Failed WRITE (2026-01-16)
```
Command:  0A 06 96 08 00 01 E5 3B
Response: 0A 86 01 F2 62
Result:   Exception 0x01 = "Illegal Function"
```

## Possible Explanations

### 1. Register 0x9608 is Read-Only
The register shows current status but cannot be modified. There may be a different register for setting the priority.

### 2. Wrong Function Code
- Tried: **0x06** (Write Single Register)
- Maybe needs: **0x10** (Write Multiple Registers)
- Some devices only accept function 0x10 even for single values

### 3. Register Requires Unlock
Some inverters require:
- Writing a password to a specific register first
- Setting a "configuration mode" flag
- Sequence like: unlock → write → lock

### 4. Different Register for Writing
Common patterns:
- 0x9608 = read current mode (status)
- 0x96XX = write desired mode (control)
- Example: 0x9600 might be the writable version

## Investigation Steps

### Step 1: Check EPever Documentation
Look for in the UP5000 manual:
- "Output Source Priority" or "Inverter Priority" settings
- Modbus register map section
- "Parameter setting" vs "Parameter reading" register tables
- Any mention of register 0x9608 or nearby addresses

**Documentation to check**:
- EPever UP5000 User Manual
- EPever UP5000 Modbus Protocol Document
- EPever Solar Off-Grid Inverter Communication Protocol

### Step 2: Check Inverter LCD Menu
On the physical inverter:
1. Navigate to settings menu
2. Find "Output Source Priority" or similar
3. Try changing it manually
4. Note if it requires:
   - Password/unlock code
   - Specific mode (e.g., "Settings Mode")
   - Multiple steps

### Step 3: Try Alternative Function Codes

#### Try Function 0x10 (Write Multiple Registers)
Even though we're writing one register, try function 0x10:

```python
# Register 0x9608, write 1 register with value 0x0001
slave_id = 10
function = 0x10  # Write Multiple Registers
address = 0x9608
quantity = 0x0001
byte_count = 0x02
value = 0x0001  # Utility Priority

Command: 0A 10 96 08 00 01 02 00 01 [CRC]
```

#### Try Function 0x05 (Write Single Coil)
If it's a boolean flag:
```
Command: 0A 05 96 08 FF 00 [CRC]  # Turn ON
Command: 0A 05 96 08 00 00 [CRC]  # Turn OFF
```

### Step 4: Scan Nearby Registers

Try reading registers around 0x9608 to find writable ones:

**Read register range 0x9600-0x960F**:
```bash
# Example using modbus tool
for addr in {0x9600..0x960F}; do
  modbus_read --host 10.10.0.117 --port 9999 --slave 10 --register $addr
done
```

Look for:
- Registers with similar values (0/1 for priority mode)
- Registers labeled "setting" vs "status"
- Pattern like 0x96XX for settings, 0x9EXX for status

### Step 5: Try Writing with Different Values

If function 0x06 works on other registers, try:
```
# Maybe it expects different values?
Write 0x0000 for Inverter Priority
Write 0x0001 for Utility Priority
Write 0x0002 for possible third mode?
```

### Step 6: Check for Password/Unlock Register

Some EPever inverters use:
- **Register 0x9000**: Configuration password
- **Register 0x9001**: Unlock/lock flag
- **Register 0xE000**: System parameter enable

Try:
1. Write password to unlock register
2. Write to 0x9608
3. Write lock value

## Alternative Approaches

### Option A: Use Inverter's Native Priority Rules
Instead of forcing priority via Modbus, configure the inverter to:
- Use voltage-based switching (e.g., below 48V → grid, above 52V → battery)
- Use SOC-based switching (if inverter supports it natively)
- Use time-based switching (e.g., peak hours → grid, off-peak → battery)

### Option B: Relay-Based Switching
Add a relay to physically switch the inverter's DIP switches or jumpers that control priority mode.

### Option C: Contact EPever Support
Ask EPever technical support:
- "How do I change output priority (register 0x9608) via Modbus?"
- "What register do I write to change from Inverter Priority to Utility Priority?"
- "Does register 0x9608 support Modbus write operations?"

## Tools for Testing

### Modbus Testing Tools
```bash
# mbpoll (command line)
mbpoll -m tcp -a 10 -p 9999 -t 4 -r 0x9608 10.10.0.117  # Read
mbpoll -m tcp -a 10 -p 9999 -t 4 -r 0x9608 10.10.0.117 1  # Write

# modpoll
modpoll -m tcp -a 10 -p 9999 -t 4 -r 0x9608 10.10.0.117  # Read
modpoll -m tcp -a 10 -p 9999 -t 4 -r 0x9608 10.10.0.117 1  # Write

# Python pymodbus
from pymodbus.client import ModbusTcpClient
client = ModbusTcpClient('10.10.0.117', port=9999)
result = client.read_holding_registers(0x9608, 1, slave=10)
print(f"Current: {result.registers[0]}")

# Try write
result = client.write_register(0x9608, 1, slave=10)
print(f"Write result: {result}")
```

### Home Assistant Testing
Add a write service to your existing Modbus config:
```yaml
modbus:
  - name: epever_inverter
    type: rtuovertcp
    host: 10.10.0.117
    port: 9999

    sensors:
      - name: "Inverter Priority Mode"
        slave: 10
        address: 0x9608
        input_type: holding

    # Try adding write support
    numbers:
      - name: "Set Inverter Priority"
        slave: 10
        address: 0x9608
        min: 0
        max: 1
```

Then call the service:
```yaml
service: modbus.write_register
data:
  address: 0x9608
  value: 1
  slave: 10
  hub: epever_inverter
```

## Expected Outcomes

### If Register is Writable
- Write succeeds without Modbus exception
- Read shows new value immediately
- Inverter physically switches priority mode

### If Register is Read-Only
- Continue getting Exception 0x01
- Need to find alternative register or method
- May require contacting EPever support

## Documentation Links

- [EPever Official Website](https://www.epever.com/)
- [EPever UP5000 Manual](https://www.epever.com/product/up5000-hm9042.html)
- [Modbus Protocol Specification](https://www.modbus.org/specs.php)

## Current Firmware Status

The ESP32 firmware is ready to:
- ✅ Read current priority mode
- ✅ Detect when mode change is needed
- ✅ Handle Modbus exceptions gracefully
- ⏳ Write to correct register (once identified)

Once we find the correct register/method, just update the register address in the code.
