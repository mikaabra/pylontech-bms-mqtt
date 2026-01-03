# Battery Protocol Reference

Comprehensive documentation of CAN and RS485 protocols for Pylontech-compatible BMS systems, plus EPever inverter protocols for the planned protocol translator project.

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
**Length:** 2 bytes

| Byte | Bit | Meaning |
|------|-----|---------|
| 0 | 7 | Force charge request |
| 0 | 6 | Discharge enable |
| 0 | 5 | Charge enable |
| 0 | 4 | Force charge request II |

**Example:** `C0 00` → Charge enabled, Discharge enabled

**Note:** V1.3 replaces 0x35C with 0x35F.

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

**Tested with:** Shoto SDA10-48200 via FTDI USB-RS485 adapter

### Physical Layer
- **Baud Rate:** 115200 (default, configurable via DIP switch on some models)
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

```python
def calc_chksum(frame_content: str) -> str:
    total = sum(ord(c) for c in frame_content)
    chk = (~total + 1) & 0xFFFF
    return f"{chk:04X}"
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
| 100-103 | Voltage | 4 | ÷1000 V | Pack voltage in mV |
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

#### Ext_Bit Section Layout (from BMS XML spec)

**Important:** The Ext_Bit section starts 3 bytes after the GB_Byte section (Current + PackVolt + Count).

| ByteIndex | Bit | Flag Name | Type |
|-----------|-----|-----------|------|
| 0 | 0 | Balance On | Normal |
| 0 | 1 | Static Balance | Normal |
| 0 | 2 | Static Balance Timeout | Protect |
| 0 | 3 | Over Temp Prohibit Balanced | Normal |
| 0 | 6 | Communication Protect ON | Normal |
| 1 | 0 | Cell Over Voltage Alarm | Warn |
| 1 | 1 | Cell Over Voltage Protect | Protect |
| 1 | 2 | Cell Under Voltage Alarm | Warn |
| 1 | 3 | Cell Under Voltage Protect | Protect |
| 1 | 4 | Pack Over Voltage Alarm | Warn |
| 1 | 5 | Pack Over Voltage Protect | Protect |
| 1 | 6 | Pack Under Voltage Alarm | Warn |
| 1 | 7 | Pack Under Voltage Protect | Protect |
| 2 | 0-7 | Temperature alarms/protections | Various |
| 3 | 0-7 | Environment/MOSFET temp, Fire alarm | Various |
| 4 | 0-7 | Current alarms/protections | Various |
| 5 | 0-2 | Current protect locking | Protect |
| 6 | 2-5 | SOC alarm, calibration flags | Warn |
| 7 | 0-7 | Hardware failures | Warn |
| 8 | 0 | DISCHG_MOSFET On | Normal |
| 8 | 1 | CHG_MOSFET On | Normal |
| 8 | 2 | LMCHG_MOSFET On | Normal |
| 8 | 3 | Heat_MOSFET On | Normal |
| 9 | 0-7 | Balance1-8 (individual cell flags) | Normal |
| 10 | 0-7 | Balance9-16 (individual cell flags) | Normal |

#### Operating State (Last Byte)

| Bit | State |
|-----|-------|
| 0 | Discharge |
| 1 | Charge |
| 2 | Float |
| 3 | Full |
| 4 | Standby |
| 5 | Shutdown |

### CW Flag (Cell Warning)

Discovered during debugging: bytes at offset `status_pos + 18/20` correlate with "CW=Y" shown on BMS display. This appears to indicate cells currently receiving balancing attention but differs from the Balance1-16 flags which indicate active balancing FETs.

---

## EPever Inverter Protocols

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

### UPower Series RS485 BMS Communication

The EPever UPower series (UP1000-UP5000, UP-Hi series) can communicate with lithium BMS systems:

**Direct RS485 Connection (without BMS-Link):**
- RS485-A to RS485-A
- RS485-B to RS485-B
- Must use twisted pair cable

**RJ45 Pinout Options:**
- Pins 3 & 6 (green pair)
- Pins 4 & 5 (blue pair)

**Important:** EPever inverters do NOT have CAN bus - only RS485 for BMS communication.

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

Create a translator that:
1. Reads battery data from CAN bus (Pylontech protocol)
2. Responds to RS485 queries from EPever inverter

### Hardware Options

**Option A: Dual RS485**
- One RS485 to sniff/respond to EPever
- One RS485 to query battery (if no CAN available)
- Suitable for: Raspberry Pi with 2× USB-RS485 adapters

**Option B: CAN + RS485 (ESP32)**
- CAN bus to read from battery stack (passive listener)
- RS485 to respond to EPever inverter queries
- Suitable for: ESP32-S3-RS485-CAN (Waveshare)

### Implementation Strategy

**Phase 1: Protocol Capture**
1. Connect RS485 sniffer between EPever and battery
2. Capture what commands EPever sends (likely CID2=0x42, 0x44)
3. Document exact frame format and timing expectations

**Phase 2: Translator Development**
1. Continuously read CAN messages from battery stack
2. Cache latest values (SOC, voltage, current, limits, alarms)
3. When RS485 query received, respond with cached data in Pylontech RS485 format

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

### Open Questions

1. What exact RS485 commands does EPever send? (need to capture)
2. Does EPever use standard Pylontech framing or modified?
3. What is the polling interval from EPever?
4. Does BMS-Link do any additional protocol translation beyond framing?

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
