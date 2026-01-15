# Battery Protocol Reference

Comprehensive documentation of CAN and RS485 protocols for Pylontech-compatible BMS systems, plus EPever UPower-HI inverter protocols for the planned protocol translator project.

**Note:** Protocol details in this document have been verified against working implementations in `pylon_can2mqtt.py` and `pylon_rs485_monitor.py`, tested with Shoto SDA10-48200 batteries.

## Table of Contents

1. [Pylontech CAN Protocol](#pylontech-can-protocol)
2. [Pylontech RS485 Protocol](#pylontech-rs485-protocol)
3. [EPever Inverter Protocols](#epever-inverter-protocols)
4. [EPever BMS-Link Converter](#epever-bms-link-converter)
5. [Protocol Translator Design Notes](#protocol-translator-design-notes)

---

## Pylontech CAN Protocol

**Tested with:** Shoto SDA10-48200 (16S 200Ah LFP) - Pylontech-compatible BMS

### Physical Layer
- **Bus Speed:** 500 kbps
- **Connector:** Typically RJ45 (CAN-H on pin 1, CAN-L on pin 2)
- **Termination:** 120Ω may be needed at end of bus

### Message Transmission
- Battery stack outputs CAN data **once per second**
- 6-7 packets transmitted in sequence per cycle

### CAN Message IDs

#### 0x351 - Charge/Discharge Voltage & Current Limits
**Length:** 8 bytes (Little-endian)

| Byte | Field | Unit | Scale | Notes |
|------|-------|------|-------|-------|
| 0-1 | Max Charge Voltage | V | ÷10 | Voltage to charge to |
| 2-3 | Max Charge Current | A | ÷10 | Current limit for charging |
| 4-5 | Max Discharge Current | A | ÷10 | Current limit for discharging |
| 6-7 | Min Discharge Voltage | V | ÷10 | Low voltage cutoff |

**Example:** `14 02 74 0E 74 0E CC 01`
- Max charge voltage: 0x0214 = 532 → 53.2V
- Charge current: 0x0E74 = 3700 → 370.0A
- Discharge current: 0x0E74 = 3700 → 370.0A
- Min voltage: 0x01CC = 460 → 46.0V

#### 0x355 - State of Charge & Health
**Length:** 4 bytes (V1.2) or 6 bytes (V1.3)

| Byte | Field | Unit | Notes |
|------|-------|------|-------|
| 0-1 | SOC | % | State of Charge (0-100) |
| 2-3 | SOH | % | State of Health (0-100) |

**Example:** `1A 00 64 00` → SOC=26%, SOH=100%

#### 0x356 - Pack Voltage, Current, Temperature
**Length:** 6 bytes

| Byte | Field | Unit | Scale | Notes |
|------|-------|------|-------|-------|
| 0-1 | Pack Voltage | V | ÷100 | Total pack voltage |
| 2-3 | Pack Current | A | ÷10 | Signed: +charge, -discharge |
| 4-5 | Temperature | °C | ÷10 | Pack temperature |

#### 0x359 - Protection & Alarm Flags (V1.2)
**Length:** 7 bytes

Bitfield indicating various protection and alarm states. This message is deprecated in V1.3 protocol.

| Byte | Bits | Meaning |
|------|------|---------|
| 0-3 | Various | Protection flags |
| 4-6 | Various | Alarm indicators |

**Note:** V1.3 replaces 0x359 with 0x35A for compatibility with SMA/Victron protocols.

#### 0x35C - Battery Charge Request Flags (V1.2)
**Length:** 1-2 bytes (only byte 0 is used)

**Byte 0 bit mapping (verified against [Setfire Labs documentation](https://www.setfirelabs.com/energy-monitoring/pylontech-battery-can-comms-v1-2-vs-v1-3)):**

| Bit | Mask | Field Name | Meaning |
|-----|------|------------|---------|
| 7 | 0x80 | RCE | **Charge Enable** - Battery accepts charging |
| 6 | 0x40 | RDE | **Discharge Enable** - Battery allows discharge |
| 5 | 0x20 | CI1 | **Force Charge Level 1** - Request immediate charge |
| 4 | 0x10 | CI2 | **Force Charge Level 2** - Urgent charge request |
| 3 | 0x08 | RFC | **Request Full Charge** - Request 100% charge |
| 2-0 | 0x07 | - | Reserved/unused |

**Common Values:**
- `0xC0` (11000000) → Charge enabled, Discharge enabled (normal operation)
- `0xE0` (11100000) → Charge enabled, Discharge enabled, Force charge level 1 (low SOC)
- `0x40` (01000000) → Discharge only (charge disabled, e.g., high SOC/overvolt protection)
- `0x80` (10000000) → Charge only (discharge disabled, e.g., low SOC/undervolt protection)

**Critical Implementation Note:** Bits 7 and 6 control charge/discharge enable. Bit 5 is NOT charge enable - it's a force charge request flag that may be set simultaneously with bit 7. Always check bit 7 for charge enable status.

**Typical Behavior:**
- At 14% SOC: `0x20` (00100000) - Force charge active, charge/discharge both disabled (protection)
- At 50% SOC: `0xC0` (11000000) - Normal operation, both enabled
- At 98% SOC: `0x40` (01000000) - Charge disabled (approaching full)

**Note:** V1.3 protocol drops 0x35C entirely. In V1.3, 0x35F contains battery information (type, version, capacity), not charge request flags.

#### 0x35E - Manufacturer Identification
**Length:** 8 bytes (ASCII)

Contains manufacturer name string, typically "PYLON   " (padded with spaces).

#### 0x370 - Cell Voltage Extremes (Shoto/Deye extension)
**Length:** 8 bytes

| Byte | Field | Unit | Scale |
|------|-------|------|-------|
| 0-1 | Min Cell Temp or Temp1 | °C | ÷10 |
| 2-3 | Max Cell Temp or Temp2 | °C | ÷10 |
| 4-5 | Min Cell Voltage | V | ÷1000 |
| 6-7 | Max Cell Voltage | V | ÷1000 |

**Note:** This message ID may be specific to Shoto/Deye batteries and not standard Pylontech.

### Protocol Versions

| Version | Changes |
|---------|---------|
| V1.2 | Original Pylontech protocol with 0x359 and 0x35C |
| V1.3 | Replaced 0x359→0x35A, 0x35C→0x35F for SMA/Victron compatibility |

---

## Pylontech RS485 Protocol

**Tested with:** Shoto SDA10-48200 via Waveshare Industrial USB-RS485 adapter

### Physical Layer
- **Baud Rate:** 9600 (some models support 115200 via DIP switch)
- **Format:** 8N1 (8 data bits, no parity, 1 stop bit)
- **Connector:** RJ45 (RS485-A on pin 7, RS485-B on pin 8)

### Frame Structure

All frames use ASCII hex encoding:

```
SOI  VER   ADR   CID1  CID2  LENGTH  INFO        CHKSUM  EOI
7E   3232  3032  3441  3432  E002    30323031    FD27    0D
~    22    02    4A    42    ...     0201        ....    CR
```

| Field | Bytes | Description |
|-------|-------|-------------|
| SOI | 1 | Start of Information: 0x7E (`~`) |
| VER | 2 | Protocol version (ASCII): "22" = 0x22 |
| ADR | 2 | Device address (ASCII): "02" = address 2 |
| CID1 | 2 | Command ID 1 (ASCII): "4A" = 0x4A (fixed) |
| CID2 | 2 | Command ID 2 (ASCII): command type |
| LENGTH | 4 | Info length with checksum nibble |
| INFO | N | Command/response data (ASCII hex) |
| CHKSUM | 4 | Frame checksum (ASCII hex) |
| EOI | 1 | End of Information: 0x0D (CR) |

### Checksum Calculation

**Frame Checksum (CHKSUM):**
```python
def calc_chksum(frame_content: str) -> str:
    total = sum(ord(c) for c in frame_content)
    chk = (~total + 1) & 0xFFFF
    return f"{chk:04X}"
```

**LENID Calculation (Critical!):**

The LENGTH field includes a checksum nibble (LCHKSUM):
```
LENID = LCHKSUM (1 hex char) + LENGTH (3 hex chars)
LCHKSUM = (~(sum of hex digits of LENGTH) + 1) & 0xF
```

**Example:** For INFO length of 2 hex chars (1 byte):
- LENGTH = "002" (3 hex chars representing value 2)
- Sum of hex digits: 0 + 0 + 2 = 2
- LCHKSUM = (~2 + 1) & 0xF = 0xE
- LENID = "E002"

**Common mistake:** Using the byte count (1) instead of summing hex digits (0+0+2=2).
This error results in response code 03 (CID2 invalid / checksum error).

```python
def calc_lenid(info_len: int) -> str:
    """Calculate LENID field for Pylontech RS485 protocol."""
    length_hex = f"{info_len:03X}"  # 3 hex chars for length
    digit_sum = sum(int(c, 16) for c in length_hex)
    lchksum = (~digit_sum + 1) & 0xF
    return f"{lchksum:X}{length_hex}"
```

### CID2 Command Codes

| CID2 | Command | Description |
|------|---------|-------------|
| 0x42 | Get Analog Values | Cell voltages, temps, current, capacity |
| 0x44 | Get Alarm Info | Cell status, balancing flags, protections |
| 0x47 | Get System Parameters | Voltage/temp/current limits |
| 0x92 | Get Charge/Discharge Management | Charge/discharge control info |

### CID2=0x42 Analog Data Response

Response INFO field structure (all values ASCII hex, 2 chars per byte):

| Offset | Field | Size | Scale | Notes |
|--------|-------|------|-------|-------|
| 0-1 | Info flag | 2 | - | Response flags |
| 2-3 | Battery number | 2 | - | Battery index |
| 4-5 | Num cells | 2 | - | Number of cells (16) |
| 6-69 | Cell voltages | 64 | ÷1000 V | 16 × 4 chars (mV) |
| 70-71 | Num temps | 2 | - | Number of temp sensors (6) |
| 72-95 | Temperatures | 24 | Kelvin×10 | 6 × 4 chars |
| 96-99 | Current | 4 | ÷100 A | Signed, 10mA units |
| 100-103 | Voltage | 4 | ÷100 V | Pack voltage in 10mV units |
| 104-107 | Remain capacity | 4 | ÷100 Ah | 10mAh units |
| 108-109 | Custom byte | 2 | - | User defined |
| 110-113 | Total capacity | 4 | ÷100 Ah | 10mAh units |
| 114-117 | Cycle count | 4 | - | Number of cycles |

**Temperature conversion:** `celsius = (raw - 2731) / 10.0`

### CID2=0x44 Alarm Data Response

Response structure (more complex, variable length):

| Section | Description |
|---------|-------------|
| Per-cell status | 0x00=normal, 0x01=undervolt, 0x02=overvolt |
| Per-temp status | 0x00=normal, 0x01=undertemp, 0x02=overtemp |
| GB_Byte section | Current status, Pack voltage status |
| Ext_Bit section | Detailed flags (see below) |

#### Ext_Bit Section Layout (Verified from BMS XML + Working Code)

**Critical:** The Ext_Bit section starts 3 bytes (6 hex chars) after the status section start:
```
status_pos = temp_start + num_temps * 2
ext_bit_start = status_pos + 6  # Skip Current(1) + PackVolt(1) + Count(1)
```

| ByteIndex | Hex Offset | Bit | Flag Name | Type |
|-----------|------------|-----|-----------|------|
| 0 | ext_bit_start+0 | 0 | Balance On | Normal |
| 0 | ext_bit_start+0 | 1 | Static Balance | Normal |
| 0 | ext_bit_start+0 | 2 | Static Balance Timeout | Protect |
| 4 | ext_bit_start+8 | 0 | Cell Over Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 1 | Cell Over Voltage Protect | Warn |
| 4 | ext_bit_start+8 | 2 | Cell Under Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 3 | Cell Under Voltage Protect | **Protect** |
| 4 | ext_bit_start+8 | 4 | Pack Over Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 5 | Pack Over Voltage Protect | Warn |
| 4 | ext_bit_start+8 | 6 | Pack Under Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 7 | Pack Under Voltage Protect | **Protect** |
| 8 | ext_bit_start+16 | 0 | DISCHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 1 | CHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 2 | LMCHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 3 | Heat_MOSFET On | Normal |
| 9 | ext_bit_start+18 | 0-7 | Balance1-8 (bit0=cell1) | Normal |
| 10 | ext_bit_start+20 | 0-7 | Balance9-16 (bit0=cell9) | Normal |

**Verified behavior:**
- Balancing cells only reported when ByteIndex 0 bit 0 (Balance On) is set
- Cell overvolt at 100% SOC is warning only, not alarm
- Only undervolt protections (bit 3, bit 7) are actual alarms

#### Operating State (Last Byte)

| Bit | State |
|-----|-------|
| 0 | Discharge |
| 1 | Charge |
| 2 | Float |
| 3 | Full |
| 4 | Standby |
| 5 | Shutdown |

### CW Flag (Cell Warning) - Empirically Discovered

**Location:** `status_pos + 18` and `status_pos + 20` (NOT ext_bit_start!)

During testing, we observed that bytes at offset `status_pos + 18/20` correlate with "CW=Y" shown on the BMS display panel. This is different from the documented Balance1-16 flags.

```python
# CW flag detection (verified in pylon_rs485_monitor.py)
if status_pos + 22 <= len(data_hex):
    cw_byte1 = int(data_hex[status_pos+18:status_pos+20], 16)  # Cells 1-8
    cw_byte2 = int(data_hex[status_pos+20:status_pos+22], 16)  # Cells 9-16
    cw_active = (cw_byte1 != 0) or (cw_byte2 != 0)
```

**Observed behavior:**
- CW=Y appears at ~3.501V (balancing threshold on Shoto BMS)
- CW=N appears at ~3.500V (just below threshold)
- This correlates with cells being "watched" for balancing, not necessarily active balancing

**Relationship to Balance flags:**
- CW flag (status_pos+18): "Cell Warning" - cell is at/above balancing threshold
- Balance flags (ext_bit_start+18): Active balancing FET state
- A cell may have CW=Y but not be actively balancing yet

---

## EPever Inverter Protocols

**Target Device:** EPever UPower-HI Series (UP-Hi3000, UP-Hi5000, etc.)

The UPower-HI series is a hybrid inverter/charger that supports lithium battery communication via RS485. It does NOT have CAN bus - only RS485.

### Native Modbus Protocol (Charge Controllers & Inverters)

**Communication Parameters:**
- Protocol: Modbus RTU
- Baud Rate: 115200 (default)
- Format: 8N1
- Default Address: 0x01

#### Key Register Addresses (Function 0x04 - Read Input)

| Register | Decimal | Parameter | Unit | Scale |
|----------|---------|-----------|------|-------|
| 0x3100 | 12544 | PV Array Voltage | V | ÷100 |
| 0x3101 | 12545 | PV Array Current | A | ÷100 |
| 0x3102-3 | 12546-7 | PV Array Power | W | ÷100 (32-bit) |
| 0x331A | 13082 | Battery Voltage | V | ÷100 |
| 0x3110 | 12560 | Battery Temperature | °C | - |
| 0x311A | 12570 | Battery SOC | % | - |

#### Battery Configuration Registers (Function 0x03/0x10 - Holding)

| Register | Parameter |
|----------|-----------|
| 0x9000 | Battery Type (0=User defined) |
| 0x9001 | Battery Capacity |
| 0x9003 | High Voltage Disconnect |
| 0x9004 | Charging Limit Voltage |
| 0x9007 | Boost Voltage |
| 0x9008 | Float Voltage |
| 0x900D | Low Voltage Disconnect |

### UPower-HI Series RS485 BMS Communication

The EPever UPower-HI series communicates with lithium BMS via RS485 using the Pylontech protocol (or other supported protocols via BMS-Link).

**Direct RS485 Connection (without BMS-Link):**
- RS485-A to RS485-A (typically pin 7 on Pylontech RJ45)
- RS485-B to RS485-B (typically pin 8 on Pylontech RJ45)
- Must use twisted pair cable

**EPever RJ45 Pinout Options:**
- Pins 3 & 6 (green pair) - RS485 A/B
- Pins 4 & 5 (blue pair) - RS485 A/B

**Critical:** EPever UPower-HI does NOT have CAN bus - only RS485 for BMS communication. This is why a protocol translator is needed when the battery only outputs CAN.

**Known Issue:** User reports indicate Pylontech protocol sometimes doesn't work reliably with UPower-HI, even with BMS-Link dongle. This may require protocol sniffing to determine exact requirements.

---

## EPever BMS-Link Converter

The BMS-Link is an external protocol converter with independent MCU that translates various BMS protocols to EPever's standard protocol.

### Supported Protocols (PRO Settings)

| PRO | Manufacturer | Protocol Name | Fixed ID |
|-----|--------------|---------------|----------|
| 1 | EPEVER | BMS_RS485_Modbus_Protocol V1.3 | - |
| 2 | Pylontech | RS485-protocol-pylon-low-voltage-V3.3 | 2 |
| 3 | MERITSUN | (proprietary) | - |
| 4 | FOXESS | (proprietary) | - |
| 5 | AOBO | (proprietary) | - |
| 6 | Dyness | (proprietary) | - |
| 7 | EverExceed | (proprietary) | - |
| ... | ... | ... | ... |

**Total:** 32+ supported protocols

### Configuration

1. Connect BMS-Link between inverter RS485 and battery RS485
2. Set PRO parameter (item 40) via remote meter or PC software
3. Configure battery Fixed ID via DIP switch (if required)

### Hardware

- Input: Non-isolated RS485 (to inverter)
- Output: Isolated RS485 (to battery)
- RJ45 connectors on both ends

---

## Protocol Translator Design Notes

### Project Goal

Create a translator for the EPever UPower-HI installation that:
1. Reads battery data from CAN bus (Pylontech protocol from Shoto/similar battery)
2. Responds to RS485 queries from UPower-HI inverter in Pylontech RS485 format

### Current Setup Context

- EPever UPower-HI inverter at remote location
- Same type of battery stack (Shoto SDA10-48200 or similar Pylontech-compatible)
- BMS-Link dongle is present but "never got it working"
- Need to understand why and potentially bypass with custom translator

### Hardware Options

**Option A: Dual RS485**
- One RS485 to sniff/respond to EPever UPower-HI
- One RS485 to query battery (if no CAN available or CAN unreliable)
- Suitable for: Raspberry Pi with 2× USB-RS485 adapters

**Option B: CAN + RS485 (ESP32) - Preferred**
- CAN bus to passively read from battery stack (same as current Deye setup)
- RS485 to respond to UPower-HI inverter queries
- Suitable for: ESP32-S3-RS485-CAN (Waveshare) - same board we have ESPHome config for

### Implementation Strategy

**Phase 1: Protocol Capture (Next Site Visit)**
1. Connect RS485 sniffer between UPower-HI and battery (or BMS-Link)
2. Capture what commands UPower-HI sends (expected: CID2=0x42, 0x44)
3. Document exact frame format and timing expectations
4. Check if BMS-Link is translating correctly or failing

**Phase 2: Determine Root Cause**
- Is UPower-HI sending correct Pylontech RS485 frames?
- Is BMS-Link responding but with wrong data?
- Is there a timing/baud rate mismatch?
- Is the battery even responding to RS485?

**Phase 3: Translator Development**
1. Continuously read CAN messages from battery stack (passive listener, no bus impact)
2. Cache latest values (SOC, voltage, current, limits, alarms)
3. When RS485 query received from UPower-HI, respond with cached data in Pylontech RS485 format
4. Use our verified frame building code from `pylon_rs485_monitor.py`

### Key Translation Mappings

| CAN Source | RS485 Response Field |
|------------|---------------------|
| 0x351 bytes 0-1 | Max charge voltage |
| 0x351 bytes 2-3 | Max charge current |
| 0x351 bytes 4-5 | Max discharge current |
| 0x351 bytes 6-7 | Min discharge voltage |
| 0x355 bytes 0-1 | SOC |
| 0x355 bytes 2-3 | SOH |
| 0x356 bytes 0-1 | Pack voltage |
| 0x356 bytes 2-3 | Pack current |
| 0x356 bytes 4-5 | Temperature |
| 0x359 | Alarm flags |

### Timing Considerations

- CAN updates: Every 1 second
- RS485 query timeout: Typically 200-500ms
- Translator must respond within timeout window

### Open Questions (To Resolve During Site Visit)

1. What exact RS485 commands does UPower-HI send? (expected: CID2=0x42, 0x44)
2. What baud rate is UPower-HI using? (likely 115200, but verify)
3. What is the polling interval from UPower-HI? (likely 1-5 seconds)
4. Is BMS-Link receiving queries? (sniff between UPower-HI and BMS-Link)
5. Is BMS-Link forwarding to battery? (sniff between BMS-Link and battery)
6. What is battery responding with? (if anything)
7. What PRO setting is configured on BMS-Link? (should be PRO=2 for Pylontech)
8. What Fixed ID is set on DIP switches? (should be ID=2 for Pylontech)

### Sniffing Setup

For protocol capture, we need:
- USB-RS485 adapter connected to laptop/Pi running capture script
- Tap into RS485 lines (A/B) - can be done with parallel connection
- Capture script that logs raw hex with timestamps

```python
# Simple RS485 sniffer (run on Pi with USB-RS485)
import serial
import time

ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.1)
while True:
    data = ser.read(256)
    if data:
        ts = time.strftime('%H:%M:%S')
        print(f"{ts} [{len(data):3d}] {data.hex()}")
```

---

## References

### Official Documentation
- BMS073-H17-BL08-16S-en-US.xml (BMS protocol XML specification)
- RS485-protocol-pylon-low-voltage-V3.3-20180821.pdf

### Online Resources
- [Pylontech CAN V1.2 vs V1.3](https://www.setfirelabs.com/energy-monitoring/pylontech-battery-can-comms-v1-2-vs-v1-3)
- [EPever B-Series Modbus Specification](https://www.developpez.net/forums/attachments/p196506d1451307310/systemes/autres-systemes/automation/probleme-com-modbus-pl7-pro/controllerprotocolv2.3.pdf/)
- [EPever Modbus Register Map](https://www.aggsoft.com/serial-data-logger/tutorials/modbus-data-logging/epever-b-series.htm)
- [EPever BMS-Link Manual](https://manuals.plus/epever/bms-link-bms-protocol-converter-manual)
- [python-pylontech Library](https://github.com/Frankkkkk/python-pylontech)
- [bms-to-inverter Project](https://github.com/ai-republic/bms-to-inverter)

### DIY Solar Forum Discussions
- [Pylontech RS485 Protocol](https://diysolarforum.com/threads/pylontech-rs485-protocol.37510/)
- [EPever UP5000-HM8042](https://diysolarforum.com/threads/epever-up5000-hm8042.80974/)
- [EPever UPower RS485 with Daly BMS](https://diysolarforum.com/threads/epever-upower-rs485-com-with-daly-bms.115109/)
